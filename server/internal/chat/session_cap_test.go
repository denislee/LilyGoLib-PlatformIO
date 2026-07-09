package chat

import (
	"strconv"
	"testing"
	"time"
)

// The session cap bounds memory against a burst of unique device_ids inside
// the idle window, evicting the least-recently-used (oldest `updated`) first.
func TestEvictSessionsLocked(t *testing.T) {
	h := &Handler{sessions: make(map[string]*session)}
	base := time.Now()
	// i=0 is oldest, i=299 is newest.
	for i := 0; i < 300; i++ {
		h.sessions[strconv.Itoa(i)] = &session{updated: base.Add(time.Duration(i) * time.Second)}
	}

	h.evictSessionsLocked(maxSessions)

	if len(h.sessions) != maxSessions {
		t.Fatalf("got %d sessions, want %d", len(h.sessions), maxSessions)
	}
	if _, ok := h.sessions["0"]; ok {
		t.Error("oldest session (0) survived eviction")
	}
	if _, ok := h.sessions["299"]; !ok {
		t.Error("newest session (299) was evicted")
	}
}

func TestEvictSessionsLockedNoOpUnderCap(t *testing.T) {
	h := &Handler{sessions: make(map[string]*session)}
	h.sessions["a"] = &session{updated: time.Now()}
	h.evictSessionsLocked(maxSessions)
	if len(h.sessions) != 1 {
		t.Fatalf("got %d sessions, want 1 (no eviction under cap)", len(h.sessions))
	}
}
