PIO ?= $(HOME)/.platformio/penv/bin/pio
ENV ?= tlora_pager
PORT ?=

UPLOAD_FLAGS :=
MONITOR_FLAGS :=
ifneq ($(PORT),)
UPLOAD_FLAGS += --upload-port $(PORT)
MONITOR_FLAGS += --port $(PORT)
endif

.PHONY: all build upload monitor flash clean emulator help envs

all: build

build:
	$(PIO) run -e $(ENV)

upload:
	$(PIO) run -e $(ENV) --target upload $(UPLOAD_FLAGS)

monitor:
	$(PIO) device monitor -b 115200 $(MONITOR_FLAGS)

flash: upload monitor

clean:
	$(PIO) run -e $(ENV) --target clean

emulator:
	$(PIO) run -e emulator_lora_pager

envs:
	$(PIO) project config | grep '^\[env:'

help:
	@echo "Targets:"
	@echo "  make build          - compile (ENV=$(ENV))"
	@echo "  make upload         - compile and upload"
	@echo "  make monitor        - open serial monitor @ 115200"
	@echo "  make flash          - upload then monitor"
	@echo "  make clean          - clean build artifacts"
	@echo "  make emulator       - build SDL2 emulator"
	@echo "  make envs           - list available PlatformIO envs"
	@echo ""
	@echo "Variables:"
	@echo "  ENV=<env>           - PlatformIO env (default: tlora_pager)"
	@echo "  PORT=/dev/ttyACM0   - override serial port"
	@echo "  PIO=<path>          - override pio binary (default: ~/.platformio/penv/bin/pio)"
