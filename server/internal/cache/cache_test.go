package cache

import (
	"strconv"
	"testing"
	"time"
)

// The entry cap is the OOM backstop against a client minting unique keys.
func TestCacheBoundsEntries(t *testing.T) {
	c := New()
	for i := 0; i < maxEntries+50; i++ {
		c.Set(strconv.Itoa(i), []byte("x"), time.Hour)
	}
	c.mu.RLock()
	n := len(c.items)
	c.mu.RUnlock()
	if n > maxEntries {
		t.Fatalf("cache holds %d entries, want <= %d", n, maxEntries)
	}
}

// Overwriting an existing key must not count against the cap or evict anything.
func TestCacheOverwriteDoesNotGrow(t *testing.T) {
	c := New()
	for i := 0; i < 100; i++ {
		c.Set("same", []byte(strconv.Itoa(i)), time.Hour)
	}
	c.mu.RLock()
	n := len(c.items)
	c.mu.RUnlock()
	if n != 1 {
		t.Fatalf("cache holds %d entries, want 1", n)
	}
	if v, ok := c.Get("same"); !ok || string(v) != "99" {
		t.Fatalf("Get = %q,%v; want \"99\",true", v, ok)
	}
}

// Expired entries are the first eviction candidates when the cache is full.
func TestCacheEvictsExpiredFirst(t *testing.T) {
	c := New()
	// Fill to capacity with already-expired entries.
	for i := 0; i < maxEntries; i++ {
		c.Set("old"+strconv.Itoa(i), []byte("x"), -time.Second)
	}
	// One more insert should reclaim the expired ones, not the fresh key.
	c.Set("fresh", []byte("y"), time.Hour)
	if _, ok := c.Get("fresh"); !ok {
		t.Fatal("fresh entry missing after insert-over-capacity")
	}
}
