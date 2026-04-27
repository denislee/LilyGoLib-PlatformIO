// Package notessync mounts /api/notes/sync — the device-delegated GitHub push
// path. The device sends its local note list (name + base64 content) along
// with the GitHub repo/branch/PAT, and we do the listing + PUTting against
// api.github.com on the device's behalf. Net win is wall-clock: a real CPU
// with HTTP keepalive and parallel uploads finishes in seconds where the
// ESP32 takes tens of seconds to a minute, and the device only has to make
// one round-trip to the LAN hub.
//
// Semantics mirror the device-side code in src/apps/ui_notes_sync.cpp exactly:
// additive-only — we never overwrite a name that already exists on the
// remote, and we never delete. So a sync request is safe to retry: at worst
// it's a no-op because the file landed last time.
//
// Encryption: notes are pushed as opaque bytes. If the device encrypts at
// rest (notes_crypto.cpp's "Salted__"-prefixed AES-256-CBC), the hub neither
// sees nor needs the passphrase.
package notessync

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
	"sync"
	"time"
)

// SyncRequest is the body of POST /api/notes/sync. The token is a GitHub PAT
// with `contents:write` scope; we never log it or persist it. Files carry the
// same opaque bytes the device would have PUT directly — we don't decode or
// re-encode the base64 payload so encrypted notes pass through verbatim.
type SyncRequest struct {
	Repo   string      `json:"repo"`
	Branch string      `json:"branch"`
	Token  string      `json:"token"`
	Files  []SyncFile  `json:"files"`
}

type SyncFile struct {
	Name      string `json:"name"`
	ContentB64 string `json:"content_b64"`
}

// SyncResponse mirrors what the device's run_sync() prints to its log: how
// many were already on the remote, how many we uploaded, how many failed.
// The device renders this back to the user; per-file errors are listed so
// they can be retried individually.
type SyncResponse struct {
	Uploaded int           `json:"uploaded"`
	Already  int           `json:"already"`
	Errors   []SyncError   `json:"errors,omitempty"`
}

type SyncError struct {
	Name string `json:"name"`
	Err  string `json:"error"`
}

type Handler struct {
	client *http.Client
	// Bound on parallel GitHub PUTs. GitHub's secondary rate limit kicks in at
	// "more than a few" concurrent writes against the same repo; 4 is the
	// sweet spot we observed where total wall time stops improving.
	maxParallel int
}

func New() *Handler {
	return &Handler{
		client: &http.Client{
			Timeout: 30 * time.Second,
		},
		maxParallel: 4,
	}
}

func (h *Handler) Register(mux *http.ServeMux) {
	mux.HandleFunc("/api/notes/sync", h.sync)
}

func (h *Handler) sync(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST required", http.StatusMethodNotAllowed)
		return
	}
	var req SyncRequest
	dec := json.NewDecoder(io.LimitReader(r.Body, 8<<20))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&req); err != nil {
		http.Error(w, "bad json: "+err.Error(), http.StatusBadRequest)
		return
	}
	if req.Repo == "" || !strings.Contains(req.Repo, "/") {
		http.Error(w, "repo must be owner/name", http.StatusBadRequest)
		return
	}
	if req.Branch == "" {
		req.Branch = "main"
	}
	if req.Token == "" {
		http.Error(w, "token required", http.StatusBadRequest)
		return
	}

	resp, err := h.runSync(r.Context(), &req)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadGateway)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(resp)
}

func (h *Handler) runSync(ctx context.Context, req *SyncRequest) (*SyncResponse, error) {
	remote, err := h.listRemote(ctx, req)
	if err != nil {
		return nil, fmt.Errorf("list: %w", err)
	}

	// Partition files into "already on remote" (skip) and "to upload". Done
	// here so the parallel section only does work that has to hit GitHub.
	have := make(map[string]bool, len(remote))
	for _, n := range remote {
		have[n] = true
	}

	var toUpload []SyncFile
	already := 0
	for _, f := range req.Files {
		if f.Name == "" {
			continue
		}
		if have[f.Name] {
			already++
			continue
		}
		toUpload = append(toUpload, f)
	}

	out := &SyncResponse{Already: already}

	// Bounded parallel uploads. errors slice is built under a mutex so the
	// final response order is deterministic-enough for the device's log.
	sem := make(chan struct{}, h.maxParallel)
	var wg sync.WaitGroup
	var mu sync.Mutex

	for _, f := range toUpload {
		wg.Add(1)
		sem <- struct{}{}
		go func(f SyncFile) {
			defer wg.Done()
			defer func() { <-sem }()

			bs, decErr := base64.StdEncoding.DecodeString(f.ContentB64)
			if decErr != nil {
				mu.Lock()
				out.Errors = append(out.Errors, SyncError{Name: f.Name, Err: "bad base64"})
				mu.Unlock()
				return
			}
			if putErr := h.putFile(ctx, req, f.Name, bs); putErr != nil {
				mu.Lock()
				out.Errors = append(out.Errors, SyncError{Name: f.Name, Err: putErr.Error()})
				mu.Unlock()
				return
			}
			mu.Lock()
			out.Uploaded++
			mu.Unlock()
		}(f)
	}
	wg.Wait()
	return out, nil
}

// listRemote fetches the names currently in `notes/` on the given branch.
// 404 is not an error — it means the folder doesn't exist yet, so every
// local file is a candidate. We only ask for names; SHAs aren't needed for
// additive-only writes.
func (h *Handler) listRemote(ctx context.Context, req *SyncRequest) ([]string, error) {
	url := fmt.Sprintf("https://api.github.com/repos/%s/contents/notes?ref=%s",
		req.Repo, req.Branch)
	httpReq, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return nil, err
	}
	httpReq.Header.Set("Authorization", "Bearer "+req.Token)
	httpReq.Header.Set("Accept", "application/vnd.github+json")
	httpReq.Header.Set("User-Agent", "lilyhub/1.0")

	resp, err := h.client.Do(httpReq)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()

	if resp.StatusCode == http.StatusNotFound {
		return nil, nil
	}
	body, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if err != nil {
		return nil, err
	}
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("github %d: %s", resp.StatusCode, truncate(string(body), 200))
	}

	var entries []struct {
		Name string `json:"name"`
		Type string `json:"type"`
	}
	if err := json.Unmarshal(body, &entries); err != nil {
		return nil, fmt.Errorf("parse: %w", err)
	}
	out := make([]string, 0, len(entries))
	for _, e := range entries {
		if e.Type != "" && e.Type != "file" {
			continue
		}
		out = append(out, e.Name)
	}
	return out, nil
}

// putFile creates notes/<name> on the remote. Caller has already confirmed
// the name is missing on the remote, so no prev-sha is needed; a 422 here
// means someone raced us and we just surface it.
func (h *Handler) putFile(ctx context.Context, req *SyncRequest, name string, content []byte) error {
	body := struct {
		Message string `json:"message"`
		Content string `json:"content"`
		Branch  string `json:"branch"`
	}{
		Message: "sync: add " + name,
		Content: base64.StdEncoding.EncodeToString(content),
		Branch:  req.Branch,
	}
	buf, _ := json.Marshal(body)

	url := fmt.Sprintf("https://api.github.com/repos/%s/contents/notes/%s",
		req.Repo, name)
	httpReq, err := http.NewRequestWithContext(ctx, http.MethodPut, url, bytes.NewReader(buf))
	if err != nil {
		return err
	}
	httpReq.Header.Set("Authorization", "Bearer "+req.Token)
	httpReq.Header.Set("Accept", "application/vnd.github+json")
	httpReq.Header.Set("Content-Type", "application/json")
	httpReq.Header.Set("User-Agent", "lilyhub/1.0")

	resp, err := h.client.Do(httpReq)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	respBody, _ := io.ReadAll(io.LimitReader(resp.Body, 64<<10))
	if resp.StatusCode/100 != 2 {
		return fmt.Errorf("github %d: %s", resp.StatusCode, truncate(string(respBody), 200))
	}
	return nil
}

func truncate(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n] + "..."
}
