package httpx

import (
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/lilygo/lilyhub/internal/cache"
)

// upstream spins up a fake upstream that returns the given status/body and
// counts how many times it was actually hit (to prove cache HITs skip it).
func upstream(t *testing.T, status int, body string, hits *int) *httptest.Server {
	t.Helper()
	return httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if hits != nil {
			*hits++
		}
		w.WriteHeader(status)
		_, _ = w.Write([]byte(body))
	}))
}

func call(c *cache.Cache, client *http.Client, key, url string, validate func([]byte) error) *httptest.ResponseRecorder {
	return callT(c, client, key, url, validate, nil)
}

func callT(c *cache.Cache, client *http.Client, key, url string, validate func([]byte) error, transform func([]byte) ([]byte, error)) *httptest.ResponseRecorder {
	rec := httptest.NewRecorder()
	Proxy(c, client, rec, httptest.NewRequest(http.MethodGet, "/", nil), key, url, time.Minute, validate, transform)
	return rec
}

func TestProxyCachesSuccessThenServesHIT(t *testing.T) {
	var hits int
	srv := upstream(t, http.StatusOK, `{"ok":true}`, &hits)
	defer srv.Close()
	c := cache.New()

	rec1 := call(c, srv.Client(), "k", srv.URL, nil)
	if rec1.Code != http.StatusOK || rec1.Header().Get("X-Cache") != "MISS" {
		t.Fatalf("first call: code=%d xcache=%q", rec1.Code, rec1.Header().Get("X-Cache"))
	}
	rec2 := call(c, srv.Client(), "k", srv.URL, nil)
	if rec2.Code != http.StatusOK || rec2.Header().Get("X-Cache") != "HIT" {
		t.Fatalf("second call: code=%d xcache=%q", rec2.Code, rec2.Header().Get("X-Cache"))
	}
	if hits != 1 {
		t.Fatalf("upstream hit %d times, want 1 (second should be a cache HIT)", hits)
	}
}

// The core truncation-cache-poisoning fix: an oversized body is a 502, not a
// silently truncated 200, and it is never cached.
func TestProxyRejectsOversizedBodyAndDoesNotCache(t *testing.T) {
	srv := upstream(t, http.StatusOK, strings.Repeat("a", maxBodyBytes+100), nil)
	defer srv.Close()
	c := cache.New()

	rec := call(c, srv.Client(), "k", srv.URL, nil)
	if rec.Code != http.StatusBadGateway {
		t.Fatalf("code=%d, want 502 for oversized body", rec.Code)
	}
	if _, ok := c.Get("k"); ok {
		t.Fatal("oversized (truncated) body must not be cached")
	}
}

// A body exactly at the limit is still valid and cached.
func TestProxyBodyExactlyAtLimitIsCached(t *testing.T) {
	srv := upstream(t, http.StatusOK, strings.Repeat("a", maxBodyBytes), nil)
	defer srv.Close()
	c := cache.New()

	rec := call(c, srv.Client(), "k", srv.URL, nil)
	if rec.Code != http.StatusOK {
		t.Fatalf("code=%d, want 200 for at-limit body", rec.Code)
	}
	if _, ok := c.Get("k"); !ok {
		t.Fatal("at-limit body should be cached")
	}
}

// The ip-api fix path: a validator error means the 200 body is treated as an
// upstream failure and is never cached.
func TestProxyValidateRejectsAndDoesNotCache(t *testing.T) {
	srv := upstream(t, http.StatusOK, `{"status":"fail"}`, nil)
	defer srv.Close()
	c := cache.New()

	validate := func(b []byte) error {
		if strings.Contains(string(b), `"fail"`) {
			return errors.New("upstream reported failure")
		}
		return nil
	}
	rec := call(c, srv.Client(), "k", srv.URL, validate)
	if rec.Code != http.StatusBadGateway {
		t.Fatalf("code=%d, want 502 when validate rejects", rec.Code)
	}
	if _, ok := c.Get("k"); ok {
		t.Fatal("validate-rejected body must not be cached")
	}
}

func TestProxyValidatePassesIsCached(t *testing.T) {
	srv := upstream(t, http.StatusOK, `{"status":"success"}`, nil)
	defer srv.Close()
	c := cache.New()

	validate := func(b []byte) error {
		if !strings.Contains(string(b), `"success"`) {
			return errors.New("not success")
		}
		return nil
	}
	rec := call(c, srv.Client(), "k", srv.URL, validate)
	if rec.Code != http.StatusOK {
		t.Fatalf("code=%d, want 200 when validate passes", rec.Code)
	}
	if _, ok := c.Get("k"); !ok {
		t.Fatal("validated body should be cached")
	}
}

// A transform's output — not the raw upstream body — is what's cached and
// written back.
func TestProxyTransformRewritesBodyBeforeCaching(t *testing.T) {
	srv := upstream(t, http.StatusOK, `{"a":1,"b":2}`, nil)
	defer srv.Close()
	c := cache.New()

	transform := func(b []byte) ([]byte, error) {
		return []byte(`{"a":1}`), nil
	}
	rec := callT(c, srv.Client(), "k", srv.URL, nil, transform)
	if rec.Code != http.StatusOK || rec.Body.String() != `{"a":1}` {
		t.Fatalf("code=%d body=%q, want 200 with transformed body", rec.Code, rec.Body.String())
	}
	cached, ok := c.Get("k")
	if !ok || string(cached) != `{"a":1}` {
		t.Fatalf("cached=%q ok=%v, want the transformed body cached", cached, ok)
	}
}

// A transform error is treated like a validate error: 502, nothing cached.
func TestProxyTransformErrorNotCached(t *testing.T) {
	srv := upstream(t, http.StatusOK, `not json`, nil)
	defer srv.Close()
	c := cache.New()

	transform := func(b []byte) ([]byte, error) {
		return nil, errors.New("bad shape")
	}
	rec := callT(c, srv.Client(), "k", srv.URL, nil, transform)
	if rec.Code != http.StatusBadGateway {
		t.Fatalf("code=%d, want 502 when transform errors", rec.Code)
	}
	if _, ok := c.Get("k"); ok {
		t.Fatal("transform-rejected body must not be cached")
	}
}

func TestProxyNon200NotCached(t *testing.T) {
	srv := upstream(t, http.StatusInternalServerError, `oops`, nil)
	defer srv.Close()
	c := cache.New()

	rec := call(c, srv.Client(), "k", srv.URL, nil)
	if rec.Code != http.StatusBadGateway {
		t.Fatalf("code=%d, want 502 for upstream 500", rec.Code)
	}
	if _, ok := c.Get("k"); ok {
		t.Fatal("non-200 upstream must not be cached")
	}
}
