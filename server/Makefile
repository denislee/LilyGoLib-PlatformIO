# bash for `read -s` (silent input) in the install prompt — POSIX sh has no
# silent-read primitive without stty gymnastics.
SHELL := /bin/bash

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
ENVDIR  := /etc/lilyhub
ENVFILE := $(ENVDIR)/env

.PHONY: build run test fmt vet \
        linux-arm64 linux-armv7 linux-amd64 \
        install upgrade uninstall \
        set-groq-key \
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
ifneq ($(strip $(GROQ_API_KEY)),)
	@$(MAKE) --no-print-directory set-groq-key
else
	@if sudo test -s $(ENVFILE) && sudo grep -q '^GROQ_API_KEY=' $(ENVFILE); then \
		echo ">> $(ENVFILE) already has GROQ_API_KEY — leaving as-is."; \
		echo ">> (rotate with: GROQ_API_KEY=gsk_new make set-groq-key)"; \
	elif [ -t 0 ] && [ -r /dev/tty ]; then \
		echo ""; \
		echo ">> No GROQ_API_KEY set. Enter one to enable /api/chat,"; \
		echo ">> or just press Enter to skip and configure later."; \
		echo ">> Get a free key at https://console.groq.com"; \
		read -rsp ">> GROQ_API_KEY (input hidden): " key </dev/tty; echo ""; \
		if [ -n "$$key" ]; then \
			GROQ_API_KEY="$$key" $(MAKE) --no-print-directory set-groq-key; \
			unset key; \
		else \
			echo ">> skipped. /api/chat will return 503 until you run:"; \
			echo ">>   GROQ_API_KEY=gsk_xxx make set-groq-key"; \
		fi; \
	else \
		echo ">> no $(ENVFILE) and GROQ_API_KEY not provided (non-interactive)."; \
		echo ">> /api/chat will return 503 until you run:"; \
		echo ">>   GROQ_API_KEY=gsk_xxx make set-groq-key"; \
	fi
endif
	@echo ">> done. tail logs with: make logs"
	@$(MAKE) --no-print-directory status

# Write GROQ_API_KEY to /etc/lilyhub/env (0600 root:root) and restart the
# service if it's running. Pass the key on the make command line —
#   make set-groq-key GROQ_API_KEY=gsk_xxx
# — or via the environment to keep it out of `ps` / shell history:
#   GROQ_API_KEY=gsk_xxx make set-groq-key
# Idempotent: re-running with a new key rotates it.
set-groq-key:
ifeq ($(strip $(GROQ_API_KEY)),)
	$(error GROQ_API_KEY is not set. Usage: make set-groq-key GROQ_API_KEY=gsk_xxx)
endif
	@echo ">> writing $(ENVFILE) (0600 root:root)"
	@sudo install -d -m 0755 -o root -g root $(ENVDIR)
	@printf 'GROQ_API_KEY=%s\n' '$(GROQ_API_KEY)' | sudo tee $(ENVFILE) >/dev/null
	@sudo chmod 0600 $(ENVFILE)
	@sudo chown root:root $(ENVFILE)
	@if systemctl is-active --quiet $(UNIT); then \
		echo ">> restarting $(UNIT)"; \
		sudo systemctl restart $(UNIT); \
	else \
		echo ">> $(UNIT) not running; key staged at $(ENVFILE)"; \
	fi

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
	@if [ -f $(ENVFILE) ]; then \
		echo ">> $(ENVFILE) kept (remove with: sudo rm -rf $(ENVDIR))"; \
	fi

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
