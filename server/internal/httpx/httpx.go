// Package httpx provides shared helpers for app handlers: a cached upstream
// proxy and a request logger middleware. New "apps" mounted on the hub should
// reuse Proxy() so we keep one place that talks to the network and one place
// that owns caching semantics.
package httpx

import (
	"fmt"
	"io"
	"log"
	"net/http"
	"time"

	"github.com/lilygo/lilyhub/internal/cache"
)

// Proxy fetches `upstream` (GET) and writes the body back to w. Successful
// responses are cached under `key` for `ttl`. On any error a 502 is returned;
// the device is expected to fall back to the public internet on its own.
func Proxy(c *cache.Cache, client *http.Client, w http.ResponseWriter, r *http.Request,
	key, upstream string, ttl time.Duration,
) {
	if body, ok := c.Get(key); ok {
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("X-Cache", "HIT")
		_, _ = w.Write(body)
		return
	}

	req, err := http.NewRequestWithContext(r.Context(), http.MethodGet, upstream, nil)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadGateway)
		return
	}
	req.Header.Set("User-Agent", "lilyhub/1.0")

	resp, err := client.Do(req)
	if err != nil {
		http.Error(w, "upstream: "+err.Error(), http.StatusBadGateway)
		return
	}
	defer resp.Body.Close()

	body, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if err != nil {
		http.Error(w, "upstream read: "+err.Error(), http.StatusBadGateway)
		return
	}
	if resp.StatusCode != http.StatusOK {
		http.Error(w, fmt.Sprintf("upstream %d", resp.StatusCode), http.StatusBadGateway)
		return
	}

	c.Set(key, body, ttl)
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("X-Cache", "MISS")
	_, _ = w.Write(body)
}

// Logger logs method, path, remote, status, duration. statusRecorder lets us
// capture the status code without buffering the body.
type statusRecorder struct {
	http.ResponseWriter
	status int
}

func (s *statusRecorder) WriteHeader(code int) {
	s.status = code
	s.ResponseWriter.WriteHeader(code)
}

func Logger(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		rec := &statusRecorder{ResponseWriter: w, status: http.StatusOK}
		next.ServeHTTP(rec, r)
		log.Printf("%s %s %s %d %s",
			r.Method, r.URL.RequestURI(), r.RemoteAddr, rec.status, time.Since(start))
	})
}
