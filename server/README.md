# LilyHub

Local-first HTTP server for LilyGo pager / watch apps. Sits on a Linux box on
the same LAN as the device (e.g. a Raspberry Pi or any always-on ARM box).
On-device apps point at it first; on any failure they fall back to the public
internet so the device keeps working when the hub is offline.

The first app served is **Weather** — it proxies and caches `ip-api.com` and
`open-meteo.com`. Adding more apps is a matter of dropping a new package next
to `internal/weather/` and wiring its `Register(mux)` in `cmd/lilyhub/main.go`.

## Layout

```
server/
├── cmd/lilyhub/        entry point — wires the cache, mux and apps
├── internal/cache/     in-memory TTL cache shared across apps
├── internal/httpx/     proxy helper + request-logger middleware
├── internal/weather/   /api/weather/* endpoints (first app)
├── deploy/             systemd unit
└── Makefile            build + cross-compile targets
```

## Endpoints

| Method | Path                              | Cache TTL | Upstream |
|-------:|-----------------------------------|-----------|----------|
| GET    | `/healthz`                        | —         | (local)  |
| GET    | `/api/weather/geo/ip`             | 30 min    | `http://ip-api.com/json/?fields=...` |
| GET    | `/api/weather/geo/search?name=…&count=…` | 24 h | `https://geocoding-api.open-meteo.com/v1/search` |
| GET    | `/api/weather/forecast?latitude=…&longitude=…&…` | 10 min | `https://api.open-meteo.com/v1/forecast` |

Each weather response is byte-identical to the upstream (we don't reshape
JSON), so the device parsers stay unchanged. The forecast endpoint forwards
the full query string, so adding/removing `hourly=`/`daily=` fields on the
device needs no server change.

`X-Cache: HIT|MISS` is set on responses for debugging.

## Build

```sh
make build           # native
make linux-arm64     # Raspberry Pi 3/4/5, BPi, Pine64, etc.
make linux-armv7     # 32-bit ARM (Raspberry Pi 2, older boards)
make linux-amd64     # x86_64 Linux
```

The binary is statically linked (`CGO_ENABLED=0`), so it has no glibc/musl
dependency on the target host.

## Deploy on Arch Linux ARM

```sh
# one-time setup
sudo useradd -r -s /usr/sbin/nologin lilyhub
sudo install -d -o lilyhub -g lilyhub /opt/lilyhub

# build + ship
make linux-arm64
scp bin/lilyhub-linux-arm64 deploy/lilyhub.service pi@<host>:/tmp/

# on the host:
sudo install -m 0755 /tmp/lilyhub-linux-arm64 /opt/lilyhub/lilyhub
sudo install -m 0644 /tmp/lilyhub.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now lilyhub
sudo systemctl status lilyhub
```

Verify:

```sh
curl -s http://<host>:8080/healthz
curl -s 'http://<host>:8080/api/weather/forecast?latitude=-23.55&longitude=-46.63&hourly=temperature_2m&forecast_days=1&timezone=auto'
```

## Configure the device

Open **Settings → Weather → Local hub URL** on the device and enter the hub's
address, e.g. `http://192.168.1.10:8080` (no trailing slash). Empty disables
hub usage. The device tries the hub first; on any failure (timeout, 5xx, DNS,
connection refused) it transparently falls back to the public internet.

## Adding a new app

1. Create `internal/<app>/<app>.go` with a `Handler` and a `Register(mux *http.ServeMux)` method.
2. In `cmd/lilyhub/main.go`, instantiate it after `weather.New(c)` and call `Register(mux)`.
3. Reuse `httpx.Proxy` for cached pass-throughs; talk to the cache directly for anything bespoke.
