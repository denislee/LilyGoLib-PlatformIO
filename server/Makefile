BIN     := lilyhub
PKG     := ./cmd/lilyhub
LDFLAGS := -ldflags="-s -w"

# Install layout — override on the make command line if you want a different
# prefix, e.g. `make install PREFIX=/usr/local`.
PREFIX  := /opt/lilyhub
USER    := lilyhub
GROUP   := lilyhub
UNIT    := lilyhub.service
SYSTEMD := /etc/systemd/system

.PHONY: build run test fmt vet \
        linux-arm64 linux-armv7 linux-amd64 \
        install upgrade uninstall \
        status logs restart start stop \
        clean

build:
	go build -o bin/$(BIN) $(PKG)

run: build
	./bin/$(BIN)

test:
	go test ./...

fmt:
	go fmt ./...

vet:
	go vet ./...

linux-arm64:
	GOOS=linux GOARCH=arm64 CGO_ENABLED=0 go build $(LDFLAGS) -o bin/$(BIN)-linux-arm64 $(PKG)

linux-armv7:
	GOOS=linux GOARCH=arm GOARM=7 CGO_ENABLED=0 go build $(LDFLAGS) -o bin/$(BIN)-linux-armv7 $(PKG)

linux-amd64:
	GOOS=linux GOARCH=amd64 CGO_ENABLED=0 go build $(LDFLAGS) -o bin/$(BIN)-linux-amd64 $(PKG)

# --- systemd install -------------------------------------------------------
# `install` does the whole one-shot setup: build, create the service user if
# missing, drop the binary + unit in place, enable, start. Idempotent — safe
# to re-run; that's also what `upgrade` is for after a rebuild.

install: build
	@echo ">> ensuring user '$(USER)' exists"
	@id -u $(USER) >/dev/null 2>&1 || sudo useradd -r -s /usr/sbin/nologin $(USER)
	@echo ">> installing binary to $(PREFIX)/$(BIN)"
	sudo install -d -o $(USER) -g $(GROUP) $(PREFIX)
	sudo install -m 0755 bin/$(BIN) $(PREFIX)/$(BIN)
	@echo ">> installing systemd unit to $(SYSTEMD)/$(UNIT)"
	sudo install -m 0644 deploy/$(UNIT) $(SYSTEMD)/$(UNIT)
	sudo systemctl daemon-reload
	sudo systemctl enable --now $(UNIT)
	@echo ">> done. tail logs with: make logs"
	@$(MAKE) --no-print-directory status

upgrade: build
	sudo install -m 0755 bin/$(BIN) $(PREFIX)/$(BIN)
	sudo systemctl restart $(UNIT)
	@$(MAKE) --no-print-directory status

uninstall:
	-sudo systemctl disable --now $(UNIT)
	sudo rm -f $(SYSTEMD)/$(UNIT)
	sudo systemctl daemon-reload
	sudo rm -rf $(PREFIX)
	@echo ">> service removed. user '$(USER)' kept (remove with: sudo userdel $(USER))"

status:
	-systemctl status $(UNIT) --no-pager

logs:
	journalctl -u $(UNIT) -f

restart:
	sudo systemctl restart $(UNIT)

start:
	sudo systemctl start $(UNIT)

stop:
	sudo systemctl stop $(UNIT)

clean:
	rm -rf bin
