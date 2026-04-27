// Package cache is a small in-memory TTL cache shared by upstream-proxying
// handlers. It is goroutine-safe and intentionally minimal — we only need
// Get/Set/Sweep, not eviction policies.
package cache

import (
	"sync"
	"time"
)

type entry struct {
	value     []byte
	expiresAt time.Time
}

type Cache struct {
	mu    sync.RWMutex
	items map[string]entry
}

func New() *Cache {
	return &Cache{items: make(map[string]entry)}
}

func (c *Cache) Get(key string) ([]byte, bool) {
	c.mu.RLock()
	e, ok := c.items[key]
	c.mu.RUnlock()
	if !ok || time.Now().After(e.expiresAt) {
		return nil, false
	}
	return e.value, true
}

func (c *Cache) Set(key string, value []byte, ttl time.Duration) {
	c.mu.Lock()
	c.items[key] = entry{value: value, expiresAt: time.Now().Add(ttl)}
	c.mu.Unlock()
}

func (c *Cache) Sweep() {
	now := time.Now()
	c.mu.Lock()
	for k, e := range c.items {
		if now.After(e.expiresAt) {
			delete(c.items, k)
		}
	}
	c.mu.Unlock()
}
