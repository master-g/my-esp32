SHELL := /bin/bash

.DEFAULT_GOAL := help

PORT ?= auto
BAUD ?= 460800
TARGET ?= esp32s3

IDF_ACTIVATE ?= source ~/.espressif/tools/activate_idf_v6.0.sh >/dev/null 2>&1
IDF_PY ?= python3 ~/.espressif/v6.0/esp-idf/tools/idf.py
CARGO ?= cargo
NPM ?= npm
SERIAL_PORT_RESOLVER ?= bash ./tools/resolve_serial_port.sh

.PHONY: help \
	build set-target clean fullclean menuconfig \
	port ports flash flash-slow monitor flash-monitor \
	fmt fmt-check \
	font-install font-build font-generate \
	agent-run agent-status

define run_idf
$(IDF_ACTIVATE) && $(IDF_PY) $(1)
endef

help: ## Show available targets and overridable variables
	@printf "Common variables:\n"
	@printf "  PORT=%s\n" "$(PORT)"
	@printf "  BAUD=%s\n" "$(BAUD)"
	@printf "  TARGET=%s\n" "$(TARGET)"
	@printf "  PORT_BUSY_OK=%s\n" "$${PORT_BUSY_OK:-0}"
	@printf "\nTargets:\n"
	@awk 'BEGIN {FS = ":.*## "}; /^[a-zA-Z0-9_.-]+:.*## / {printf "  %-15s %s\n", $$1, $$2}' $(MAKEFILE_LIST)

set-target: ## Set the ESP-IDF target, defaults to TARGET=esp32s3
	@$(call run_idf,set-target $(TARGET))

build: ## Build firmware with ESP-IDF
	@$(call run_idf,build)

clean: ## Remove firmware build artifacts with idf.py clean
	@$(call run_idf,clean)

fullclean: ## Remove firmware build artifacts and generated configuration outputs
	@$(call run_idf,fullclean)

menuconfig: ## Open ESP-IDF menuconfig
	@$(call run_idf,menuconfig)

ports: ## List candidate serial ports
	@$(SERIAL_PORT_RESOLVER) --list

port: ## Resolve the serial port that flash/monitor will use
	@PORT_VALUE="$$(PORT="$(PORT)" ESPPORT="$${ESPPORT:-}" PORT_BUSY_OK="$${PORT_BUSY_OK:-0}" \
		$(SERIAL_PORT_RESOLVER) --select)" && \
	printf "%s\n" "$$PORT_VALUE"

flash: ## Flash firmware to PORT using BAUD (PORT=auto will auto-detect)
	@PORT_VALUE="$$(PORT="$(PORT)" ESPPORT="$${ESPPORT:-}" PORT_BUSY_OK="$${PORT_BUSY_OK:-0}" \
		$(SERIAL_PORT_RESOLVER) --select)" && \
	printf "Using serial port %s\n" "$$PORT_VALUE" && \
	$(call run_idf,-p "$$PORT_VALUE" -b $(BAUD) flash)

flash-slow: ## Flash firmware to PORT using 115200 baud (PORT=auto will auto-detect)
	@PORT_VALUE="$$(PORT="$(PORT)" ESPPORT="$${ESPPORT:-}" PORT_BUSY_OK="$${PORT_BUSY_OK:-0}" \
		$(SERIAL_PORT_RESOLVER) --select)" && \
	printf "Using serial port %s\n" "$$PORT_VALUE" && \
	$(call run_idf,-p "$$PORT_VALUE" -b 115200 flash)

monitor: ## Open ESP-IDF serial monitor on PORT (PORT=auto will auto-detect)
	@PORT_VALUE="$$(PORT="$(PORT)" ESPPORT="$${ESPPORT:-}" PORT_BUSY_OK="$${PORT_BUSY_OK:-0}" \
		$(SERIAL_PORT_RESOLVER) --select)" && \
	printf "Using serial port %s\n" "$$PORT_VALUE" && \
	$(call run_idf,-p "$$PORT_VALUE" monitor)

flash-monitor: ## Flash firmware, then open the serial monitor
	@PORT_VALUE="$$(PORT="$(PORT)" ESPPORT="$${ESPPORT:-}" PORT_BUSY_OK="$${PORT_BUSY_OK:-0}" \
		$(SERIAL_PORT_RESOLVER) --select)" && \
	printf "Using serial port %s\n" "$$PORT_VALUE" && \
	$(call run_idf,-p "$$PORT_VALUE" -b $(BAUD) flash monitor)

fmt: ## Format the Rust host tool (firmware C code has no repo formatter configured)
	@$(CARGO) fmt --all --manifest-path tools/esp32dash/Cargo.toml

fmt-check: ## Check Rust formatting without rewriting files
	@$(CARGO) fmt --all --manifest-path tools/esp32dash/Cargo.toml -- --check

font-install: ## Install local font pipeline dependencies
	@cd tools/lv_font_pipeline && $(NPM) install

font-generate: ## Regenerate the Home icon font
	@cd tools/lv_font_pipeline && $(NPM) run generate:home-status

font-build: ## Fetch upstream icon sources and regenerate the Home icon font
	@cd tools/lv_font_pipeline && $(NPM) run build

agent-run: ## Run the esp32dash host agent
	@$(CARGO) run --manifest-path tools/esp32dash/Cargo.toml -- agent run

agent-status: ## Query the esp32dash host agent status
	@$(CARGO) run --manifest-path tools/esp32dash/Cargo.toml -- agent status
