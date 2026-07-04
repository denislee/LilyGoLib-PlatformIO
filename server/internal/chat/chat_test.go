package chat

import (
	"context"
	"net/http"
	"net/http/httptest"
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
