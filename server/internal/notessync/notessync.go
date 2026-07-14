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
	"log"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

// maxRespBytes caps how much of a GitHub API response body we ever read.
const maxRespBytes = 1 << 20

// baseBackoff, retryableStatus, backoff, and doWithRetry mirror
// internal/chat's retry pattern for GitHub's 429/transient 5xx: sync is
// additive/idempotent, so retrying is safe, and one flaky call shouldn't
// abort the whole (serial) device sync.
var baseBackoff = 200 * time.Millisecond

func retryableStatus(code int) bool {
	return code == http.StatusTooManyRequests || code >= 500
}

func backoff(attempt int) time.Duration {
	shift := max(attempt-2, 0)
	return min(baseBackoff<<shift, 2*time.Second)
}

// doWithRetry runs req, retrying on 429/5xx up to maxAttempts times with
// chat.go's 200ms-doubling backoff. Unlike chat.go's do(), the final HTTP
// status is returned rather than turned into an error — callers here have
// status-specific handling (listRemote treats 404 as "no notes yet", not a
// failure).
func (h *Handler) doWithRetry(req *http.Request, maxAttempts int) ([]byte, int, error) {
	var lastErr error
	for attempt := 1; attempt <= maxAttempts; attempt++ {
		if attempt > 1 {
			d := backoff(attempt)
			log.Printf("notessync: %s %s retry %d/%d after %s (%v)",
				req.Method, req.URL.Path, attempt, maxAttempts, d, lastErr)
			timer := time.NewTimer(d)
			select {
			case <-timer.C:
			case <-req.Context().Done():
				timer.Stop()
				return nil, 0, req.Context().Err()
			}
			if req.GetBody != nil {
				b, err := req.GetBody()
				if err != nil {
					return nil, 0, err
				}
				req.Body = b
			}
		}
		resp, err := h.client.Do(req)
		if err != nil {
			lastErr = err
			continue
		}
		body, err := io.ReadAll(io.LimitReader(resp.Body, maxRespBytes))
		resp.Body.Close()
		if err != nil {
			lastErr = err
			continue
		}
		if retryableStatus(resp.StatusCode) && attempt < maxAttempts {
			lastErr = fmt.Errorf("github %d: %s", resp.StatusCode, truncate(string(body), 200))
			continue
		}
		return body, resp.StatusCode, nil
	}
	return nil, 0, lastErr
}

// SyncRequest is the body of POST /api/notes/sync. The token is a GitHub PAT
// with `contents:write` scope; we never log it or persist it. Files carry the
// same opaque bytes the device would have PUT directly — we don't decode or
// re-encode the base64 payload so encrypted notes pass through verbatim.
type SyncRequest struct {
	Repo   string     `json:"repo"`
	Branch string     `json:"branch"`
	Token  string     `json:"token"`
	Files  []SyncFile `json:"files"`
}

type SyncFile struct {
	Name       string `json:"name"`
	ContentB64 string `json:"content_b64"`
}

// SyncResponse mirrors what the device's run_sync() prints to its log: how
// many were already on the remote, how many we uploaded, how many failed.
// The device renders this back to the user; per-file errors are listed so
// they can be retried individually.
type SyncResponse struct {
	Uploaded int         `json:"uploaded"`
	Already  int         `json:"already"`
	Errors   []SyncError `json:"errors,omitempty"`
}

type SyncError struct {
	Name string `json:"name"`
	Err  string `json:"error"`
}

type Handler struct {
	client *http.Client
	// Bound on parallel GitHub PUTs. Must stay at 1: the Contents API computes
	// the parent ref at request time, so two PUTs to the same branch race and
	// the loser comes back as 409 "is at X but expected Y". Serial is fast
	// enough at the note counts this app deals with.
	maxParallel int
	// Local on-disk note store. Lets the device offload bulk note storage to
	// the hub when its internal flash fills up. Files are saved as opaque
	// bytes (whatever the device sent — encrypted Salted__ blobs pass through
	// untouched). Path defaults to $LILYHUB_NOTES_DIR or, failing that,
	// $HOME/.lilyhub/notes.
	notesDir string
	// Serializes upload writes — the device may fan out multiple uploads in
	// parallel, but the same name landing twice would race fsync of the temp
	// file vs. rename. Cheap given typical traffic.
	storeMu sync.Mutex
}

func New() *Handler {
	dir := os.Getenv("LILYHUB_NOTES_DIR")
	if dir == "" {
		if home, err := os.UserHomeDir(); err == nil && home != "" {
			dir = filepath.Join(home, ".lilyhub", "notes")
		} else {
			dir = filepath.Join(os.TempDir(), "lilyhub-notes")
		}
	}
	return &Handler{
		client: &http.Client{
			Timeout: 30 * time.Second,
		},
		maxParallel: 1,
		notesDir:    dir,
	}
}

func (h *Handler) Register(mux *http.ServeMux) {
	mux.HandleFunc("/api/notes/sync", h.sync)
	mux.HandleFunc("/api/notes/upload", h.upload)
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
	if !validRepo(req.Repo) {
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

	out := &SyncResponse{}
	var toUpload []SyncFile
	already := 0
	for _, f := range req.Files {
		if f.Name == "" {
			continue
		}
		// Reject traversal/odd names before they reach the GitHub URL path.
		// safeName mirrors the /upload guard (no slash, NUL, or leading dot).
		// Without it a name like "../secrets.txt" writes outside notes/ and
		// bypasses the additive-only guard, which only enumerates notes/.
		if _, ok := safeName(f.Name); !ok {
			out.Errors = append(out.Errors, SyncError{Name: f.Name, Err: "invalid name"})
			continue
		}
		if have[f.Name] {
			already++
			continue
		}
		toUpload = append(toUpload, f)
	}
	out.Already = already

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

			// Forward the device's base64 verbatim per the package contract —
			// no decode/re-encode — so encrypted notes reach GitHub byte-for-
			// byte and we skip two full-size allocations per file. GitHub
			// validates the base64 itself, so a malformed payload surfaces as
			// the PUT error below rather than a local decode failure.
			if putErr := h.putFile(ctx, req, f.Name, f.ContentB64); putErr != nil {
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
	// req.Repo is validated (validRepo) to owner/name with URL-safe chars and
	// no "."/".." segments, so it interpolates directly. req.Branch is caller-
	// controlled and unvalidated, so escape it before it lands in the query.
	endpoint := fmt.Sprintf("https://api.github.com/repos/%s/contents/notes?ref=%s",
		req.Repo, url.QueryEscape(req.Branch))
	httpReq, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint, nil)
	if err != nil {
		return nil, err
	}
	httpReq.Header.Set("Authorization", "Bearer "+req.Token)
	httpReq.Header.Set("Accept", "application/vnd.github+json")
	httpReq.Header.Set("User-Agent", "lilyhub/1.0")

	body, status, err := h.doWithRetry(httpReq, 2) // one retry
	if err != nil {
		return nil, err
	}
	if status == http.StatusNotFound {
		return nil, nil
	}
	if status != http.StatusOK {
		return nil, fmt.Errorf("github %d: %s", status, truncate(string(body), 200))
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
func (h *Handler) putFile(ctx context.Context, req *SyncRequest, name string, contentB64 string) error {
	body := struct {
		Message string `json:"message"`
		Content string `json:"content"`
		Branch  string `json:"branch"`
	}{
		Message: "sync: add " + name,
		Content: contentB64,
		Branch:  req.Branch,
	}
	buf, _ := json.Marshal(body)

	// name is safeName-validated in runSync (no slash/NUL/leading dot), but
	// PathEscape it anyway: safeName still permits spaces, '?', '#', '%' etc.,
	// which would otherwise inject into the URL path/query.
	endpoint := fmt.Sprintf("https://api.github.com/repos/%s/contents/notes/%s",
		req.Repo, url.PathEscape(name))
	httpReq, err := http.NewRequestWithContext(ctx, http.MethodPut, endpoint, bytes.NewReader(buf))
	if err != nil {
		return err
	}
	httpReq.Header.Set("Authorization", "Bearer "+req.Token)
	httpReq.Header.Set("Accept", "application/vnd.github+json")
	httpReq.Header.Set("Content-Type", "application/json")
	httpReq.Header.Set("User-Agent", "lilyhub/1.0")

	respBody, status, err := h.doWithRetry(httpReq, 3) // chat.go's full retry pattern
	if err != nil {
		return err
	}
	if status/100 != 2 {
		return fmt.Errorf("github %d: %s", status, truncate(string(respBody), 200))
	}
	return nil
}

func truncate(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n] + "..."
}

// UploadRequest is the body of POST /api/notes/upload. The hub stores the
// raw decoded bytes under notesDir/<name>. Idempotent: a second upload with
// the same name and same bytes is a no-op; different bytes overwrite. The
// device uses this to mirror its internal-flash note store to the hub when
// flash gets tight, and as the source of truth for the new sync flow that
// no longer reads from the SD card.
type UploadRequest struct {
	Name       string `json:"name"`
	ContentB64 string `json:"content_b64"`
}

type UploadResponse struct {
	Stored bool   `json:"stored"`
	Bytes  int    `json:"bytes"`
	Path   string `json:"path,omitempty"`
}

// safeName guards against directory traversal and oddball names landing in
// the notes dir. Notes filenames are app-generated (YYYYMMDD_HHMMSS.txt or
// similar), so the rules can be strict — anything containing a slash, NUL,
// or starting with a dot is rejected.
// validRepo enforces exactly "owner/name" with GitHub-legal characters and
// rejects any "." or ".." segment, so req.Repo is safe to interpolate into the
// api.github.com URL path. Without it, a repo like "../../x" — or a segment
// "." / ".." — would traverse the API path, the sync-side equivalent of the
// filename traversal safeName guards against on /upload.
func validRepo(repo string) bool {
	owner, name, ok := strings.Cut(repo, "/")
	if !ok {
		return false
	}
	return validRepoSegment(owner) && validRepoSegment(name)
}

func validRepoSegment(s string) bool {
	if s == "" || s == "." || s == ".." {
		return false
	}
	for _, c := range s {
		switch {
		case c >= 'A' && c <= 'Z':
		case c >= 'a' && c <= 'z':
		case c >= '0' && c <= '9':
		case c == '.' || c == '-' || c == '_':
		default:
			return false
		}
	}
	return true
}

func safeName(name string) (string, bool) {
	if name == "" {
		return "", false
	}
	if strings.ContainsAny(name, "/\\\x00") {
		return "", false
	}
	if name == "." || name == ".." || strings.HasPrefix(name, ".") {
		return "", false
	}
	if len(name) > 200 {
		return "", false
	}
	return name, true
}

func (h *Handler) ensureNotesDir() error {
	return os.MkdirAll(h.notesDir, 0o755)
}

func (h *Handler) upload(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST required", http.StatusMethodNotAllowed)
		return
	}
	var req UploadRequest
	dec := json.NewDecoder(io.LimitReader(r.Body, 8<<20))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&req); err != nil {
		http.Error(w, "bad json: "+err.Error(), http.StatusBadRequest)
		return
	}
	name, ok := safeName(req.Name)
	if !ok {
		http.Error(w, "invalid name", http.StatusBadRequest)
		return
	}
	bs, err := base64.StdEncoding.DecodeString(req.ContentB64)
	if err != nil {
		http.Error(w, "bad base64: "+err.Error(), http.StatusBadRequest)
		return
	}
	req.ContentB64 = "" // let GC drop the ~1.33x base64 string before the file write below

	h.storeMu.Lock()
	defer h.storeMu.Unlock()

	if err := h.ensureNotesDir(); err != nil {
		http.Error(w, "mkdir: "+err.Error(), http.StatusInternalServerError)
		return
	}
	dst := filepath.Join(h.notesDir, name)
	tmp := dst + ".tmp"
	if err := os.WriteFile(tmp, bs, 0o644); err != nil {
		http.Error(w, "write: "+err.Error(), http.StatusInternalServerError)
		return
	}
	if err := os.Rename(tmp, dst); err != nil {
		_ = os.Remove(tmp)
		http.Error(w, "rename: "+err.Error(), http.StatusInternalServerError)
		return
	}

	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(UploadResponse{Stored: true, Bytes: len(bs), Path: name})
}
