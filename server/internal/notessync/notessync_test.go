package notessync

import (
	"bytes"
	"context"
	"io"
	"net/http"
	"net/http/httptest"
	"sync/atomic"
	"testing"
	"time"
)

// validRepo is the guard that keeps req.Repo from traversing the api.github.com
// URL path (e.g. "../../other"), so cover the traversal and malformed cases.
func TestValidRepo(t *testing.T) {
	valid := []string{
		"octocat/Hello-World",
		"my.org/my.repo",
		"a_b/c-d",
		"user123/repo.git",
	}
	for _, r := range valid {
		if !validRepo(r) {
			t.Errorf("validRepo(%q) = false, want true", r)
		}
	}

	invalid := []string{
		"",
		"noslash",
		"owner/",
		"/repo",
		"owner/re/po",    // extra segment
		"../../x",        // traversal
		"a/..",           // dot-dot segment
		"../b",           // dot-dot segment
		".",              // no slash + dot
		"owner/repo\x00", // NUL
		"own er/repo",    // space
		"owner/re?po",    // query char
	}
	for _, r := range invalid {
		if validRepo(r) {
			t.Errorf("validRepo(%q) = true, want false", r)
		}
	}
}

// safeName is applied to every file name in the sync path so a name like
// "../secrets.txt" can't write outside notes/.
func TestSafeNameRejectsTraversal(t *testing.T) {
	ok := []string{"20250101_120000.txt", "note.md", "a-b_c.txt"}
	for _, n := range ok {
		if _, valid := safeName(n); !valid {
			t.Errorf("safeName(%q) rejected, want accepted", n)
		}
	}

	bad := []string{
		"",
		"../secrets.txt",
		"a/b",
		"..",
		".",
		".hidden",
		"x\x00y",
	}
	for _, n := range bad {
		if _, valid := safeName(n); valid {
			t.Errorf("safeName(%q) accepted, want rejected", n)
		}
	}
}

func newTestHandler() *Handler {
	return &Handler{client: &http.Client{Timeout: 5 * time.Second}}
}

// fastBackoff shrinks the retry delay so retry tests don't sleep for real.
func fastBackoff(t *testing.T) {
	t.Helper()
	old := baseBackoff
	baseBackoff = time.Millisecond
	t.Cleanup(func() { baseBackoff = old })
}

func getReq(t *testing.T, url string) *http.Request {
	t.Helper()
	req, err := http.NewRequestWithContext(context.Background(), http.MethodGet, url, nil)
	if err != nil {
		t.Fatalf("NewRequestWithContext: %v", err)
	}
	return req
}

func TestDoWithRetryRetriesOn429ThenSucceeds(t *testing.T) {
	fastBackoff(t)
	var attempts int32
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if atomic.AddInt32(&attempts, 1) < 3 {
			w.WriteHeader(http.StatusTooManyRequests)
			return
		}
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte("ok"))
	}))
	defer srv.Close()
	h := newTestHandler()

	body, status, err := h.doWithRetry(getReq(t, srv.URL), 3)
	if err != nil {
		t.Fatalf("doWithRetry: %v", err)
	}
	if status != http.StatusOK {
		t.Fatalf("status=%d, want 200", status)
	}
	if string(body) != "ok" {
		t.Fatalf("body=%q, want %q", body, "ok")
	}
	if n := atomic.LoadInt32(&attempts); n != 3 {
		t.Fatalf("attempts=%d, want 3 (two 429s then success)", n)
	}
}

// doWithRetry itself never turns a persistently-bad HTTP status into an
// error — it hands the final status back so callers can apply their own
// rules (listRemote treats 404 as success; putFile treats any non-2xx as
// failure). Retries stop once maxAttempts is spent either way.
func TestDoWithRetryExhaustsRetriesOnPersistent5xx(t *testing.T) {
	fastBackoff(t)
	var attempts int32
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		atomic.AddInt32(&attempts, 1)
		w.WriteHeader(http.StatusServiceUnavailable)
	}))
	defer srv.Close()
	h := newTestHandler()

	_, status, err := h.doWithRetry(getReq(t, srv.URL), 3)
	if err != nil {
		t.Fatalf("doWithRetry: %v, want nil error (status handling is the caller's job)", err)
	}
	if status != http.StatusServiceUnavailable {
		t.Fatalf("status=%d, want 503", status)
	}
	if n := atomic.LoadInt32(&attempts); n != 3 {
		t.Fatalf("attempts=%d, want 3 (maxAttempts)", n)
	}
}

func TestDoWithRetryDoesNotRetryNonRetryableStatus(t *testing.T) {
	fastBackoff(t)
	var attempts int32
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		atomic.AddInt32(&attempts, 1)
		w.WriteHeader(http.StatusUnprocessableEntity) // 422, not retryable
	}))
	defer srv.Close()
	h := newTestHandler()

	_, status, err := h.doWithRetry(getReq(t, srv.URL), 3)
	if err != nil {
		t.Fatalf("doWithRetry: %v, want nil error", err)
	}
	if status != http.StatusUnprocessableEntity {
		t.Fatalf("status=%d, want 422", status)
	}
	if n := atomic.LoadInt32(&attempts); n != 1 {
		t.Fatalf("attempts=%d, want 1 (422 is not retryable)", n)
	}
}

// listRemote calls doWithRetry with maxAttempts=2 — "one retry" per D7.
func TestDoWithRetryRespectsOneRetryLimit(t *testing.T) {
	fastBackoff(t)
	var attempts int32
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		atomic.AddInt32(&attempts, 1)
		w.WriteHeader(http.StatusServiceUnavailable)
	}))
	defer srv.Close()
	h := newTestHandler()

	if _, _, err := h.doWithRetry(getReq(t, srv.URL), 2); err != nil {
		t.Fatalf("doWithRetry: %v, want nil error", err)
	}
	if n := atomic.LoadInt32(&attempts); n != 2 {
		t.Fatalf("attempts=%d, want 2 (maxAttempts=2, one retry)", n)
	}
}

// putFile's request body must be replayable across retries — GetBody is what
// makes that possible for the bytes.Reader http.NewRequestWithContext builds.
func TestDoWithRetryReplaysBodyAcrossRetries(t *testing.T) {
	fastBackoff(t)
	want := []byte(`{"message":"sync: add note.txt"}`)
	var attempts int32
	var lastBody []byte
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		b, _ := io.ReadAll(r.Body)
		lastBody = b
		if atomic.AddInt32(&attempts, 1) < 2 {
			w.WriteHeader(http.StatusServiceUnavailable)
			return
		}
		w.WriteHeader(http.StatusOK)
	}))
	defer srv.Close()
	h := newTestHandler()

	req, err := http.NewRequestWithContext(context.Background(), http.MethodPut, srv.URL, bytes.NewReader(want))
	if err != nil {
		t.Fatalf("NewRequestWithContext: %v", err)
	}
	_, status, err := h.doWithRetry(req, 3)
	if err != nil {
		t.Fatalf("doWithRetry: %v", err)
	}
	if status != http.StatusOK {
		t.Fatalf("status=%d, want 200", status)
	}
	if n := atomic.LoadInt32(&attempts); n != 2 {
		t.Fatalf("attempts=%d, want 2", n)
	}
	if !bytes.Equal(lastBody, want) {
		t.Fatalf("last request body=%q, want %q (GetBody must replay it on retry)", lastBody, want)
	}
}

// A GitHub blip can also be the server being entirely unreachable, not just
// a bad status — that must still surface as an error after retries.
func TestDoWithRetryReturnsErrorOnPersistentTransportFailure(t *testing.T) {
	fastBackoff(t)
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {}))
	url := srv.URL
	srv.Close() // now connections to this URL fail
	h := newTestHandler()

	if _, _, err := h.doWithRetry(getReq(t, url), 2); err == nil {
		t.Fatal("want error when the server is unreachable on every attempt")
	}
}
