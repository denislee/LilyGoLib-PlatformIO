// Package weather mounts the device-facing weather endpoints. Each one is a
// near-pass-through of a public API the device used to call directly, so the
// JSON shape on the wire is unchanged — the device just swaps the host.
//
// Caching TTLs reflect upstream advice: open-meteo recommends <= 1 req/min,
// city geocoding is effectively static, and IP geo is stable as long as the
// public IP doesn't change.
package weather

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/url"
	"time"

	"github.com/lilygo/lilyhub/internal/cache"
	"github.com/lilygo/lilyhub/internal/httpx"
)

// ipAPIStatusOK rejects ip-api's 200-with-error responses. ip-api reports
// failure as {"status":"fail",...} at HTTP 200, which the generic proxy would
// otherwise cache as a success for the full 30-minute TTL — pinning a transient
// failure (rate limit, reserved range) and starving the device of a retry.
func ipAPIStatusOK(body []byte) error {
	var s struct {
		Status string `json:"status"`
	}
	if err := json.Unmarshal(body, &s); err != nil {
		return fmt.Errorf("unparseable ip-api body")
	}
	if s.Status != "success" {
		return fmt.Errorf("ip-api status %q", s.Status)
	}
	return nil
}

type Handler struct {
	cache  *cache.Cache
	client *http.Client
}

func New(c *cache.Cache) *Handler {
	return &Handler{
		cache: c,
		client: &http.Client{
			Timeout: 8 * time.Second,
		},
	}
}

func (h *Handler) Register(mux *http.ServeMux) {
	mux.HandleFunc("/api/weather/geo/ip", h.geoIP)
	mux.HandleFunc("/api/weather/geo/search", h.geoSearch)
	mux.HandleFunc("/api/weather/forecast", h.forecast)
}

// geoIP — pass-through of ip-api.com. Same response shape as the device's
// previous call so the on-device JSON parser is untouched.
func (h *Handler) geoIP(w http.ResponseWriter, r *http.Request) {
	const upstream = "http://ip-api.com/json/?fields=status,lat,lon,city,regionName,country"
	httpx.Proxy(h.cache, h.client, w, r, "geoip", upstream, 30*time.Minute, ipAPIStatusOK)
}

// geoSearch — pass-through of geocoding-api.open-meteo.com/v1/search.
// The `name` query is the user-typed city; we forward `count` if given, else
// default to 10 (matches the device's old behavior).
func (h *Handler) geoSearch(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query().Get("name")
	if q == "" {
		http.Error(w, "missing name", http.StatusBadRequest)
		return
	}
	count := r.URL.Query().Get("count")
	if count == "" {
		count = "10"
	}
	upstream := "https://geocoding-api.open-meteo.com/v1/search?name=" +
		url.QueryEscape(q) + "&count=" + url.QueryEscape(count) + "&format=json"
	httpx.Proxy(h.cache, h.client, w, r, "geosearch:"+q+":"+count, upstream, 24*time.Hour, nil)
}

// forecast — pass-through of api.open-meteo.com/v1/forecast. We forward the
// raw query string verbatim rather than re-building it: the device may add or
// drop hourly/daily fields without server changes, and the cache key naturally
// follows whatever set of fields was requested.
func (h *Handler) forecast(w http.ResponseWriter, r *http.Request) {
	qs := r.URL.RawQuery
	if qs == "" || r.URL.Query().Get("latitude") == "" || r.URL.Query().Get("longitude") == "" {
		http.Error(w, "missing latitude/longitude", http.StatusBadRequest)
		return
	}
	upstream := "https://api.open-meteo.com/v1/forecast?" + qs
	httpx.Proxy(h.cache, h.client, w, r, "forecast:"+qs, upstream, 10*time.Minute, nil)
}
