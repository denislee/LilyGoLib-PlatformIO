BIN := lilyhub
PKG := ./cmd/lilyhub
LDFLAGS := -ldflags="-s -w"

.PHONY: build run test fmt vet linux-arm64 linux-armv7 linux-amd64 clean

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

clean:
	rm -rf bin
