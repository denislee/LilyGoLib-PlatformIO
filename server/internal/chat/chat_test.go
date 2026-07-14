package chat

import (
	"bytes"
	"context"
	"encoding/base64"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func testHandler(url string) *Handler {
	return &Handler{
		client:   &http.Client{Timeout: 5 * time.Second},
		apiKey:   "test-key",
		baseURL:  url,
		sessions: make(map[string]*session),
	}
}

// fastBackoff shrinks the retry delay so retry tests don't sleep for real.
func fastBackoff(t *testing.T) {
	t.Helper()
	old := baseBackoff
	baseBackoff = time.Millisecond
	t.Cleanup(func() { baseBackoff = old })
}

func waitFor(t *testing.T, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if cond() {
			return
		}
		time.Sleep(2 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for %s", what)
}

// A reap firing while a session's first LLM call is in flight must not evict
// it. Pre-fix (updated stamped only after success) this test fails: the session
// is dropped and its reply + history are lost.
func TestSessionSurvivesReapDuringInflightCall(t *testing.T) {
	release := make(chan struct{})
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		<-release // hold the call "in flight" until the test releases it
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte(`{"choices":[{"message":{"content":"hi there"}}]}`))
	}))
	defer srv.Close()
	h := testHandler(srv.URL)

	done := make(chan error, 1)
	go func() {
		_, err := h.complete(context.Background(), "dev", "hello")
		done <- err
	}()

	waitFor(t, "session to be created", func() bool {
		h.mu.Lock()
		defer h.mu.Unlock()
		return len(h.sessions) == 1
	})

	// Reap with "now" == real now: a session stamped at creation stays (updated
	// ~= now, after the now-1h cutoff); an unstamped one (updated == zero) dies.
	h.mu.Lock()
	h.reapLocked(time.Now())
	h.mu.Unlock()

	close(release)
	if err := <-done; err != nil {
		t.Fatalf("complete: %v", err)
	}

	h.mu.Lock()
	s, ok := h.sessions["dev"]
	h.mu.Unlock()
	if !ok {
		t.Fatal("session evicted mid-flight — reply and history lost")
	}
	if len(s.msgs) != 2 {
		t.Fatalf("want user+assistant (2 msgs), got %d", len(s.msgs))
	}
}

func TestCompleteRetriesOn429ThenSucceeds(t *testing.T) {
	fastBackoff(t)
	var attempts int32
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if atomic.AddInt32(&attempts, 1) < 3 {
			w.WriteHeader(http.StatusTooManyRequests)
			_, _ = w.Write([]byte(`{"error":{"message":"slow down"}}`))
			return
		}
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte(`{"choices":[{"message":{"content":"ok"}}]}`))
	}))
	defer srv.Close()
	h := testHandler(srv.URL)

	reply, err := h.complete(context.Background(), "dev", "hi")
	if err != nil {
		t.Fatalf("want success after retries, got %v", err)
	}
	if reply != "ok" {
		t.Fatalf("reply=%q, want %q", reply, "ok")
	}
	if n := atomic.LoadInt32(&attempts); n != 3 {
		t.Fatalf("attempts=%d, want 3 (two 429s then success)", n)
	}
}

func TestCompleteGivesUpAfterRetryStorm(t *testing.T) {
	fastBackoff(t)
	var attempts int32
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		atomic.AddInt32(&attempts, 1)
		w.WriteHeader(http.StatusServiceUnavailable)
		_, _ = w.Write([]byte(`{"error":{"message":"down"}}`))
	}))
	defer srv.Close()
	h := testHandler(srv.URL)

	if _, err := h.complete(context.Background(), "dev", "hi"); err == nil {
		t.Fatal("want error after exhausting retries")
	}
	if n := atomic.LoadInt32(&attempts); n != 3 {
		t.Fatalf("attempts=%d, want 3 (maxAttempts)", n)
	}
}

// transcribe must decode audioB64 straight into the multipart body without
// ever materializing the full decoded []byte itself — this checks the
// upstream actually receives the right bytes, not just that no error occurs.
func TestTranscribeStreamsDecodedAudio(t *testing.T) {
	want := bytes.Repeat([]byte("RIFFfakewavdata"), 100)
	var gotAudio []byte
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		mr, err := r.MultipartReader()
		if err != nil {
			t.Errorf("MultipartReader: %v", err)
			http.Error(w, "bad multipart", http.StatusBadRequest)
			return
		}
		for {
			part, err := mr.NextPart()
			if err == io.EOF {
				break
			}
			if err != nil {
				t.Errorf("NextPart: %v", err)
				return
			}
			if part.FormName() == "file" {
				b, err := io.ReadAll(part)
				if err != nil {
					t.Errorf("read file part: %v", err)
					return
				}
				gotAudio = b
			}
		}
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte(`{"text":"hello world"}`))
	}))
	defer srv.Close()
	h := testHandler(srv.URL)

	text, err := h.transcribe(context.Background(), base64.StdEncoding.EncodeToString(want))
	if err != nil {
		t.Fatalf("transcribe: %v", err)
	}
	if text != "hello world" {
		t.Fatalf("text=%q, want %q", text, "hello world")
	}
	if !bytes.Equal(gotAudio, want) {
		t.Fatalf("upstream received %d bytes, want %d bytes matching the original audio", len(gotAudio), len(want))
	}
}

// Invalid base64 must fail before any upstream request is made, and the
// error must be identifiable as caller-input (not upstream) so chat() can
// return 400 rather than 502.
func TestTranscribeBadBase64ReturnsErrBadAudio(t *testing.T) {
	var called int32
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		atomic.AddInt32(&called, 1)
		w.WriteHeader(http.StatusOK)
	}))
	defer srv.Close()
	h := testHandler(srv.URL)

	_, err := h.transcribe(context.Background(), "not-valid-base64!!")
	if err == nil {
		t.Fatal("want error for invalid base64")
	}
	var badAudio *errBadAudio
	if !errors.As(err, &badAudio) {
		t.Fatalf("err=%v (%T), want *errBadAudio", err, err)
	}
	if n := atomic.LoadInt32(&called); n != 0 {
		t.Fatalf("upstream called %d times, want 0 (should fail decoding before the HTTP request)", n)
	}
}

// End-to-end through the handler: a device that sends garbage audio_b64
// should see 400, not the 502 an upstream STT failure would produce.
func TestChatBadAudioBase64Returns400(t *testing.T) {
	h := testHandler("http://unused.invalid")
	h.apiKey = "test-key"
	body := `{"device_id":"dev","audio_b64":"not-valid-base64!!"}`
	req := httptest.NewRequest(http.MethodPost, "/api/chat", strings.NewReader(body))
	w := httptest.NewRecorder()
	h.chat(w, req)
	if w.Code != http.StatusBadRequest {
		t.Fatalf("status=%d, want %d; body=%s", w.Code, http.StatusBadRequest, w.Body.String())
	}
}

func TestCompleteDoesNotRetry4xx(t *testing.T) {
	fastBackoff(t)
	var attempts int32
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		atomic.AddInt32(&attempts, 1)
		w.WriteHeader(http.StatusBadRequest)
		_, _ = w.Write([]byte(`{"error":{"message":"bad model"}}`))
	}))
	defer srv.Close()
	h := testHandler(srv.URL)

	if _, err := h.complete(context.Background(), "dev", "hi"); err == nil {
		t.Fatal("want error on 400")
	}
	if n := atomic.LoadInt32(&attempts); n != 1 {
		t.Fatalf("attempts=%d, want 1 (400 is not retryable)", n)
	}
}
