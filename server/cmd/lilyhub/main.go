// lilyhub — local-first server for LilyGo pager / watch apps. Runs on a Linux
// box on the same LAN as the device. The device tries it first; on failure it
// falls back to the public internet directly.
//
// Add new apps as new packages exposing a `Register(mux)` method, then wire
// them in main(). The shared TTL cache and HTTP client live here; handlers
// only need to know their own URLs and TTLs.
package main

import (
	"context"
	"errors"
	"flag"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/lilygo/lilyhub/internal/cache"
	"github.com/lilygo/lilyhub/internal/chat"
	"github.com/lilygo/lilyhub/internal/httpx"
	"github.com/lilygo/lilyhub/internal/notessync"
	"github.com/lilygo/lilyhub/internal/weather"
)

func main() {
	addr := flag.String("addr", envOr("LILYHUB_ADDR", ":8080"), "listen address")
	flag.Parse()

	c := cache.New()
	go sweepLoop(c)

	mux := http.NewServeMux()

	// The device pings /healthz (or any 200-returning path) to decide whether
	// to use the hub for a given request. Keep it cheap and side-effect free.
	mux.HandleFunc("/healthz", func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte("ok\n"))
	})

	weather.New(c).Register(mux)
	notessync.New().Register(mux)
	chat.New().Register(mux)

	srv := &http.Server{
		Addr:              *addr,
		Handler:           httpx.Logger(mux),
		ReadHeaderTimeout: 5 * time.Second,
	}

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	go func() {
		log.Printf("lilyhub listening on %s", *addr)
		if err := srv.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			log.Fatal(err)
		}
	}()

	<-ctx.Done()
	log.Print("shutting down")
	sd, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_ = srv.Shutdown(sd)
}

func sweepLoop(c *cache.Cache) {
	t := time.NewTicker(5 * time.Minute)
	defer t.Stop()
	for range t.C {
		c.Sweep()
	}
}

func envOr(k, def string) string {
	if v, ok := os.LookupEnv(k); ok {
		return v
	}
	return def
}
