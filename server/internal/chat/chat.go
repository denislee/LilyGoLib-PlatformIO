// Package chat mounts /api/chat — a single-shot chat endpoint that proxies
// to Groq's free OpenAI-compatible API. The device sends either a typed
// prompt or a base64-encoded WAV; we transcribe with Whisper if needed,
// run the chat completion, and return the reply as plain text.
//
// Why Groq: free tier, OpenAI-compatible (so the same client code can swap
// to Ollama later by changing one URL), and fast enough that the small
// pager screen doesn't need streaming for v1.
//
// History lives in process memory keyed by device_id. A pager is
// effectively single-user, so a sync.Mutex-guarded map is plenty — we
// don't pull in SQLite for ~20 messages per session.
package chat

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"mime/multipart"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"
)

const (
	groqBase     = "https://api.groq.com/openai/v1"
	sttModel     = "whisper-large-v3-turbo"
	chatModel    = "llama-3.3-70b-versatile"
	systemPrompt = "You are a concise assistant on a small pager screen. " +
		"Answer in plain text, no markdown, ≤120 words unless asked."

	maxHistory       = 20             // messages, dropping oldest pairs
	maxContent       = 4 * 1024       // per-message cap before we trim
	maxRequestBytes  = 8 << 20        // 8 MiB JSON envelope
	maxRespBytes     = 1 << 20        // 1 MiB upstream response cap
	sessionIdleLimit = time.Hour      // sessions idle this long are reaped
	clientTimeout    = 60 * time.Second
)

type Request struct {
	DeviceID string `json:"device_id"`
	Text     string `json:"text,omitempty"`
	AudioB64 string `json:"audio_b64,omitempty"`
	Reset    bool   `json:"reset,omitempty"`
}

type Response struct {
	Transcript string `json:"transcript,omitempty"`
	Reply      string `json:"reply"`
}

type message struct {
	Role    string `json:"role"`
	Content string `json:"content"`
}

type session struct {
	msgs    []message
	updated time.Time
}

type Handler struct {
	client   *http.Client
	apiKey   string
	mu       sync.Mutex
	sessions map[string]*session
}

func New() *Handler {
	return &Handler{
		client:   &http.Client{Timeout: clientTimeout},
		apiKey:   os.Getenv("GROQ_API_KEY"),
		sessions: make(map[string]*session),
	}
}

func (h *Handler) Register(mux *http.ServeMux) {
	mux.HandleFunc("/api/chat", h.chat)
}

func (h *Handler) chat(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST required", http.StatusMethodNotAllowed)
		return
	}
	var req Request
	dec := json.NewDecoder(io.LimitReader(r.Body, maxRequestBytes))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&req); err != nil {
		http.Error(w, "bad json: "+err.Error(), http.StatusBadRequest)
		return
	}
	if req.DeviceID == "" {
		http.Error(w, "device_id required", http.StatusBadRequest)
		return
	}

	if req.Reset {
		h.mu.Lock()
		delete(h.sessions, req.DeviceID)
		h.mu.Unlock()
		writeJSON(w, &Response{Reply: ""})
		return
	}

	if h.apiKey == "" {
		http.Error(w, "GROQ_API_KEY not set on hub", http.StatusServiceUnavailable)
		return
	}

	resp := &Response{}
	prompt := strings.TrimSpace(req.Text)

	// Audio takes precedence: if both are set, voice is what we treat as
	// the user's actual turn, and the device is free to send a hint in
	// `text`. We still return the transcript so the device can show
	// what we heard.
	if req.AudioB64 != "" {
		audio, err := base64.StdEncoding.DecodeString(req.AudioB64)
		if err != nil {
			http.Error(w, "bad audio_b64", http.StatusBadRequest)
			return
		}
		t, err := h.transcribe(r.Context(), audio)
		if err != nil {
			http.Error(w, "stt: "+err.Error(), http.StatusBadGateway)
			return
		}
		resp.Transcript = t
		prompt = strings.TrimSpace(t)
	}

	if prompt == "" {
		http.Error(w, "text or audio_b64 required", http.StatusBadRequest)
		return
	}

	reply, err := h.complete(r.Context(), req.DeviceID, prompt)
	if err != nil {
		http.Error(w, "llm: "+err.Error(), http.StatusBadGateway)
		return
	}
	resp.Reply = reply
	writeJSON(w, resp)
}

// transcribe sends the WAV bytes to Groq's Whisper endpoint as multipart
// form data. We name the file with a .wav extension so Groq's content-
// sniffer doesn't reject it on extension grounds — the actual audio
// format is detected from the bytes.
func (h *Handler) transcribe(ctx context.Context, audio []byte) (string, error) {
	var buf bytes.Buffer
	mw := multipart.NewWriter(&buf)
	if err := mw.WriteField("model", sttModel); err != nil {
		return "", err
	}
	if err := mw.WriteField("response_format", "json"); err != nil {
		return "", err
	}
	fw, err := mw.CreateFormFile("file", "audio.wav")
	if err != nil {
		return "", err
	}
	if _, err := fw.Write(audio); err != nil {
		return "", err
	}
	if err := mw.Close(); err != nil {
		return "", err
	}

	httpReq, err := http.NewRequestWithContext(ctx, http.MethodPost,
		groqBase+"/audio/transcriptions", &buf)
	if err != nil {
		return "", err
	}
	httpReq.Header.Set("Authorization", "Bearer "+h.apiKey)
	httpReq.Header.Set("Content-Type", mw.FormDataContentType())

	body, err := h.do(httpReq)
	if err != nil {
		return "", err
	}
	var out struct {
		Text  string `json:"text"`
		Error *struct {
			Message string `json:"message"`
		} `json:"error,omitempty"`
	}
	if err := json.Unmarshal(body, &out); err != nil {
		return "", fmt.Errorf("parse: %w", err)
	}
	if out.Error != nil {
		return "", fmt.Errorf("groq: %s", out.Error.Message)
	}
	return strings.TrimSpace(out.Text), nil
}

// complete appends the prompt to the device's session, calls Groq's
// chat-completions endpoint, and stores the reply. The session is
// trimmed to maxHistory before the call so the upstream payload stays
// bounded across long conversations.
func (h *Handler) complete(ctx context.Context, deviceID, prompt string) (string, error) {
	h.mu.Lock()
	h.reapLocked()
	s, ok := h.sessions[deviceID]
	if !ok {
		s = &session{}
		h.sessions[deviceID] = s
	}
	s.msgs = append(s.msgs, message{Role: "user", Content: clip(prompt, maxContent)})
	if len(s.msgs) > maxHistory {
		s.msgs = s.msgs[len(s.msgs)-maxHistory:]
	}
	// Snapshot under the lock so we can release before the network call.
	snapshot := make([]message, 0, len(s.msgs)+1)
	snapshot = append(snapshot, message{Role: "system", Content: systemPrompt})
	snapshot = append(snapshot, s.msgs...)
	h.mu.Unlock()

	body, err := json.Marshal(map[string]any{
		"model":    chatModel,
		"messages": snapshot,
	})
	if err != nil {
		return "", err
	}
	httpReq, err := http.NewRequestWithContext(ctx, http.MethodPost,
		groqBase+"/chat/completions", bytes.NewReader(body))
	if err != nil {
		return "", err
	}
	httpReq.Header.Set("Authorization", "Bearer "+h.apiKey)
	httpReq.Header.Set("Content-Type", "application/json")

	respBody, err := h.do(httpReq)
	if err != nil {
		return "", err
	}
	var out struct {
		Choices []struct {
			Message struct {
				Content string `json:"content"`
			} `json:"message"`
		} `json:"choices"`
		Error *struct {
			Message string `json:"message"`
		} `json:"error,omitempty"`
	}
	if err := json.Unmarshal(respBody, &out); err != nil {
		return "", fmt.Errorf("parse: %w", err)
	}
	if out.Error != nil {
		return "", fmt.Errorf("groq: %s", out.Error.Message)
	}
	if len(out.Choices) == 0 {
		return "", fmt.Errorf("no choices in response")
	}
	reply := strings.TrimSpace(out.Choices[0].Message.Content)

	h.mu.Lock()
	if s, ok := h.sessions[deviceID]; ok {
		s.msgs = append(s.msgs, message{Role: "assistant", Content: clip(reply, maxContent)})
		s.updated = time.Now()
	}
	h.mu.Unlock()

	return reply, nil
}

// do executes the request and returns the body. Non-2xx responses are
// surfaced as errors so the caller doesn't have to peek at status codes
// — the error string includes a short prefix of the upstream body to
// make Groq's auth / rate-limit messages legible in the device log.
func (h *Handler) do(req *http.Request) ([]byte, error) {
	resp, err := h.client.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(io.LimitReader(resp.Body, maxRespBytes))
	if err != nil {
		return nil, err
	}
	if resp.StatusCode/100 != 2 {
		return body, fmt.Errorf("upstream %d: %s", resp.StatusCode, truncate(string(body), 200))
	}
	return body, nil
}

// reapLocked drops sessions that have been idle for sessionIdleLimit.
// Called inline on each request so we don't need a goroutine.
func (h *Handler) reapLocked() {
	cutoff := time.Now().Add(-sessionIdleLimit)
	for k, s := range h.sessions {
		if s.updated.Before(cutoff) {
			delete(h.sessions, k)
		}
	}
}

func writeJSON(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(v)
}

func clip(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n]
}

func truncate(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n] + "..."
}
