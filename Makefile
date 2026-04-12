SHELL := /bin/bash

.DEFAULT_GOAL := help

PORT ?= auto
BAUD ?= 460800
TARGET ?= esp32s3

IDF_ACTIVATE ?= source ~/.espressif/tools/activate_idf_v6.0.sh >/dev/null 2>&1
IDF_PYTHON_CACHE ?= build/CMakeCache.txt
IDF_PYTHON_FALLBACK ?= command -v python3 || command -v python
IDF_PY_SCRIPT ?= $(HOME)/.espressif/v6.0/esp-idf/tools/idf.py
CARGO ?= cargo
NPM ?= npm
SERIAL_PORT_RESOLVER ?= bash ./tools/resolve_serial_port.sh
ESP32DASH_LAUNCHD_LABEL ?= com.local.esp32dash
ESP32DASH_LAUNCHD_PLIST ?= $(HOME)/Library/LaunchAgents/$(ESP32DASH_LAUNCHD_LABEL).plist

.PHONY: help \
	build set-target clean fullclean menuconfig \
	port ports flash flash-slow monitor flash-monitor \
	fmt fmt-check \
	font-install font-build font-generate \
	agent-run agent-status

define run_idf
$(IDF_ACTIVATE) && \
IDF_PYTHON="$$(awk -F= '/^PYTHON:/{print $$2; exit}' "$(IDF_PYTHON_CACHE)" 2>/dev/null)"; \
if [ -z "$$IDF_PYTHON" ]; then \
	IDF_PYTHON="$$($(IDF_PYTHON_FALLBACK))"; \
fi; \
"$$IDF_PYTHON" "$(IDF_PY_SCRIPT)" $(1)
endef

define run_idf_with_port_guard
PORT_VALUE="$$(PORT="$(PORT)" ESPPORT="$${ESPPORT:-}" PORT_BUSY_OK=1 PORT_ACCESS_OK=1 \
	$(SERIAL_PORT_RESOLVER) --select)"; \
STATUS=$$?; \
if [ $$STATUS -ne 0 ]; then \
	exit $$STATUS; \
fi; \
PAIRED_PORT=""; \
case "$$PORT_VALUE" in \
	/dev/cu.*) PAIRED_PORT="/dev/tty.$${PORT_VALUE#/dev/cu.}" ;; \
	/dev/tty.*) PAIRED_PORT="/dev/cu.$${PORT_VALUE#/dev/tty.}" ;; \
esac; \
ESP32DASH_BLOCKING=0; \
if command -v lsof >/dev/null 2>&1; then \
	if lsof "$$PORT_VALUE" 2>/dev/null | awk 'NR > 1 && $$1 == "esp32dash" {found=1} END {exit found?0:1}'; then \
		ESP32DASH_BLOCKING=1; \
	elif [ -n "$$PAIRED_PORT" ] && [ -e "$$PAIRED_PORT" ] && \
		lsof "$$PAIRED_PORT" 2>/dev/null | awk 'NR > 1 && $$1 == "esp32dash" {found=1} END {exit found?0:1}'; then \
		ESP32DASH_BLOCKING=1; \
	fi; \
fi; \
ESP32DASH_PAUSED=0; \
cleanup() { \
	STATUS=$$?; \
	if [ $$ESP32DASH_PAUSED -eq 1 ] && [ -f "$(ESP32DASH_LAUNCHD_PLIST)" ]; then \
		launchctl bootstrap "gui/$$(id -u)" "$(ESP32DASH_LAUNCHD_PLIST)" >/dev/null 2>&1 || true; \
		launchctl kickstart -k "gui/$$(id -u)/$(ESP32DASH_LAUNCHD_LABEL)" >/dev/null 2>&1 || true; \
	fi; \
	exit $$STATUS; \
}; \
trap cleanup EXIT; \
if [ $$ESP32DASH_BLOCKING -eq 1 ] && launchctl list 2>/dev/null | grep -q "$(ESP32DASH_LAUNCHD_LABEL)"; then \
	printf "Pausing %s to free %s\n" "$(ESP32DASH_LAUNCHD_LABEL)" "$$PORT_VALUE"; \
	launchctl bootout "gui/$$(id -u)/$(ESP32DASH_LAUNCHD_LABEL)" >/dev/null 2>&1 || true; \
	ESP32DASH_PAUSED=1; \
	sleep 1; \
fi; \
PORT_VALUE="$$(PORT="$(PORT)" ESPPORT="$${ESPPORT:-}" PORT_BUSY_OK="$${PORT_BUSY_OK:-0}" \
	$(SERIAL_PORT_RESOLVER) --select)"; \
STATUS=$$?; \
if [ $$STATUS -ne 0 ]; then \
	exit $$STATUS; \
fi; \
	printf "Using serial port %s\n" "$$PORT_VALUE"; \
	$(call run_idf,-p "$$PORT_VALUE" $(1))
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
		$(SERIAL_PORT_RESOLVER) --select)"; \
	STATUS=$$?; \
	if [ $$STATUS -ne 0 ]; then \
		exit $$STATUS; \
	fi; \
	printf "%s\n" "$$PORT_VALUE"

flash: ## Flash firmware to PORT using BAUD (PORT=auto will auto-detect)
	@$(call run_idf_with_port_guard,-b $(BAUD) flash)

flash-slow: ## Flash firmware to PORT using 115200 baud (PORT=auto will auto-detect)
	@$(call run_idf_with_port_guard,-b 115200 flash)

monitor: ## Open ESP-IDF serial monitor on PORT (PORT=auto will auto-detect)
	@PORT_VALUE="$$(PORT="$(PORT)" ESPPORT="$${ESPPORT:-}" PORT_BUSY_OK="$${PORT_BUSY_OK:-0}" \
		$(SERIAL_PORT_RESOLVER) --select)"; \
	STATUS=$$?; \
	if [ $$STATUS -ne 0 ]; then \
		exit $$STATUS; \
	fi; \
	printf "Using serial port %s\n" "$$PORT_VALUE"; \
	$(call run_idf,-p "$$PORT_VALUE" monitor)

flash-monitor: ## Flash firmware, then open the serial monitor
	@$(call run_idf_with_port_guard,-b $(BAUD) flash monitor)

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
