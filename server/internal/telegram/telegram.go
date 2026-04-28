package telegram

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"
)

type ProxyRequest struct {
	URL    string `json:"url"`
	Method string `json:"method"`
	Token  string `json:"token"`
	Body   string `json:"body,omitempty"`
}

type Handler struct {
	client *http.Client
}

func New() *Handler {
	return &Handler{
		client: &http.Client{
			Timeout: 15 * time.Second,
		},
	}
}

func (h *Handler) Register(mux *http.ServeMux) {
	mux.HandleFunc("/api/telegram/proxy", h.proxy)
}

func (h *Handler) proxy(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST required", http.StatusMethodNotAllowed)
		return
	}
	var req ProxyRequest
	dec := json.NewDecoder(io.LimitReader(r.Body, 1<<20))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&req); err != nil {
		http.Error(w, "bad json: "+err.Error(), http.StatusBadRequest)
		return
	}
	if req.URL == "" || !strings.HasPrefix(req.URL, "http") {
		http.Error(w, "valid url required", http.StatusBadRequest)
		return
	}
	if req.Method != "GET" && req.Method != "POST" {
		http.Error(w, "invalid method", http.StatusBadRequest)
		return
	}

	var bodyReader io.Reader
	if req.Body != "" {
		bodyReader = strings.NewReader(req.Body)
	}

	upstreamReq, err := http.NewRequestWithContext(r.Context(), req.Method, req.URL, bodyReader)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	if req.Token != "" {
		upstreamReq.Header.Set("Authorization", "Bearer "+req.Token)
	}
	if req.Body != "" {
		upstreamReq.Header.Set("Content-Type", "application/json")
	}
	upstreamReq.Header.Set("User-Agent", "lilyhub/1.0")

	resp, err := h.client.Do(upstreamReq)
	if err != nil {
		http.Error(w, "upstream: "+err.Error(), http.StatusBadGateway)
		return
	}
	defer resp.Body.Close()

	respBody, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if err != nil {
		http.Error(w, "upstream read: "+err.Error(), http.StatusBadGateway)
		return
	}
	if resp.StatusCode != http.StatusOK {
		http.Error(w, fmt.Sprintf("upstream %d: %s", resp.StatusCode, truncate(string(respBody), 200)), http.StatusBadGateway)
		return
	}

	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(respBody)
}

func truncate(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n] + "..."
}