#!/usr/bin/env bash
set -euo pipefail

list_candidates() {
  shopt -s nullglob
  local raw=(
    /dev/cu.usb*
    /dev/tty.usb*
    /dev/ttyUSB*
    /dev/ttyACM*
    /dev/*usbmodem*
    /dev/*usbserial*
    /dev/*wchusbserial*
  )
  shopt -u nullglob

  local path base canonical priority

  for path in "${raw[@]}"; do
    [[ -e "$path" ]] || continue
    base="${path#/dev/}"
    canonical="${base#tty.}"
    canonical="${canonical#cu.}"
    priority=1
    if [[ "$base" == cu.* ]]; then
      priority=0
    fi
    printf '%s\t%s\t%s\n' "$canonical" "$priority" "$path"
  done | sort -t "$(printf '\t')" -k1,1 -k2,2n | awk -F '\t' '!seen[$1]++ { print $3 }'
}

print_candidates() {
  local candidates=()
  local candidate

  while IFS= read -r candidate; do
    [[ -n "$candidate" ]] || continue
    candidates+=("$candidate")
  done < <(list_candidates)

  if [[ ${#candidates[@]} -eq 0 ]]; then
    printf 'No candidate serial ports found.\n' >&2
    return 1
  fi

  printf '%s\n' "${candidates[@]}"
}

check_port_exists() {
  local port="$1"

  if [[ -e "$port" ]]; then
    return 0
  fi

  printf 'Configured serial port not found: %s\n' "$port" >&2
  printf 'Available candidates:\n' >&2
  print_candidates >&2 || true
  return 1
}

check_port_busy() {
  local port="$1"
  local holders

  if [[ "${PORT_BUSY_OK:-0}" == "1" ]]; then
    return 0
  fi

  if ! command -v lsof >/dev/null 2>&1; then
    return 0
  fi

  holders="$(lsof "$port" 2>/dev/null | awk 'NR > 1 {printf "  %s (pid %s)\n", $1, $2}' || true)"
  if [[ -z "$holders" ]]; then
    return 0
  fi

  printf 'Serial port appears busy: %s\n' "$port" >&2
  printf '%s' "$holders" >&2
  printf 'Close the serial monitor, esp32dash agent, or any other process using the device.\n' >&2
  printf 'Set PORT_BUSY_OK=1 to skip this preflight check.\n' >&2
  return 1
}

paired_serial_port() {
  local port="$1"
  local base="${port#/dev/}"

  if [[ "$base" == cu.* ]]; then
    printf '/dev/tty.%s\n' "${base#cu.}"
    return 0
  fi

  if [[ "$base" == tty.* ]]; then
    printf '/dev/cu.%s\n' "${base#tty.}"
    return 0
  fi

  return 1
}

describe_other_alias_holder() {
  local port="$1"
  local paired_port
  local holders

  if ! paired_port="$(paired_serial_port "$port")"; then
    return 0
  fi

  [[ -e "$paired_port" ]] || return 0
  command -v lsof >/dev/null 2>&1 || return 0

  holders="$(lsof "$paired_port" 2>/dev/null | awk 'NR > 1 {printf "  %s (pid %s)\n", $1, $2}' || true)"
  if [[ -z "$holders" ]]; then
    return 0
  fi

  printf 'The paired serial alias is in use: %s\n' "$paired_port" >&2
  printf '%s' "$holders" >&2
}

check_port_access() {
  local port="$1"
  local stty_flag
  local stty_output

  if [[ "${PORT_ACCESS_OK:-0}" == "1" ]]; then
    return 0
  fi

  case "$(uname -s)" in
    Darwin)
      stty_flag="-f"
      ;;
    *)
      stty_flag="-F"
      ;;
  esac

  if stty_output="$(stty "$stty_flag" "$port" 2>&1 >/dev/null)"; then
    return 0
  fi

  printf 'Serial port cannot be opened right now: %s\n' "$port" >&2
  if [[ -n "$stty_output" ]]; then
    printf '%s\n' "$stty_output" >&2
  fi
  describe_other_alias_holder "$port"
  printf 'Close any serial monitor, esp32dash agent, or other tool using this port, then retry.\n' >&2
  printf 'If the device was just replugged, wait a moment and run `make port` again.\n' >&2
  return 1
}

resolve_port() {
  local requested="${PORT:-auto}"
  local candidates=()
  local candidate

  if [[ -n "${ESPPORT:-}" && ( "$requested" == "auto" || -z "$requested" ) ]]; then
    requested="$ESPPORT"
  fi

  if [[ "$requested" != "auto" && -n "$requested" ]]; then
    check_port_exists "$requested"
    check_port_busy "$requested"
    check_port_access "$requested"
    printf '%s\n' "$requested"
    return 0
  fi

  while IFS= read -r candidate; do
    [[ -n "$candidate" ]] || continue
    candidates+=("$candidate")
  done < <(list_candidates)
  case "${#candidates[@]}" in
    0)
      printf 'No serial port candidates found under /dev.\n' >&2
      printf 'Connect the board, then run `make ports` or pass PORT=/dev/... explicitly.\n' >&2
      return 1
      ;;
    1)
      check_port_busy "${candidates[0]}"
      check_port_access "${candidates[0]}"
      printf '%s\n' "${candidates[0]}"
      ;;
    *)
      printf 'Multiple serial port candidates found:\n' >&2
      printf '  %s\n' "${candidates[@]}" >&2
      printf 'Pass PORT=/dev/... explicitly, or use `make port PORT=/dev/...`.\n' >&2
      return 1
      ;;
  esac
}

case "${1:---select}" in
  --list)
    print_candidates
    ;;
  --select)
    resolve_port
    ;;
  *)
    printf 'usage: %s [--list|--select]\n' "$0" >&2
    exit 1
    ;;
esac
