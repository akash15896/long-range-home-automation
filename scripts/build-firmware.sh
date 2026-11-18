#!/usr/bin/env bash
# Compile Arduino sketches (no upload). Each sketch's board comes from its sketch.yaml.
#
#   scripts/build-firmware.sh                 # every sketch under firmware/ (incl. experiments)
#   scripts/build-firmware.sh LoRa_pico_curtain LoRa_esp32_master   # only these
#
# Finds arduino-cli on PATH, or the copy bundled with Arduino IDE 2 on Windows.
# Override with ARDUINO_CLI=/path/to/arduino-cli. Build output goes to build/<sketch>/ (gitignored).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ -n "${ARDUINO_CLI:-}" ]]; then
  CLI="$ARDUINO_CLI"
elif command -v arduino-cli >/dev/null 2>&1; then
  CLI="arduino-cli"
elif [[ -x "/c/Program Files/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe" ]]; then
  CLI="/c/Program Files/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe"
else
  echo "arduino-cli not found. Install it or set ARDUINO_CLI." >&2
  exit 2
fi

mapfile -t ALL < <(find "$ROOT/firmware" -name sketch.yaml -printf '%h\n' | sort)

if [[ $# -gt 0 ]]; then
  SKETCHES=()
  for name in "$@"; do
    match=$(printf '%s\n' "${ALL[@]}" | grep -E "/${name}\$" || true)
    [[ -z "$match" ]] && { echo "No sketch named $name" >&2; exit 2; }
    SKETCHES+=("$match")
  done
else
  SKETCHES=("${ALL[@]}")
fi

fail=0
for dir in "${SKETCHES[@]}"; do
  name=$(basename "$dir")
  if [[ -f "$dir/secrets.example.h" && ! -f "$dir/secrets.h" ]]; then
    echo "SKIP  $name (no secrets.h; copy secrets.example.h to secrets.h first)"
    continue
  fi
  if out=$("$CLI" compile --build-path "$ROOT/build/$name" "$dir" 2>&1); then
    echo "OK    $name  $(grep -m1 -o 'Sketch uses [0-9]* bytes ([0-9]*%)' <<<"$out")"
  else
    echo "FAIL  $name"; echo "$out" | tail -25; fail=1
  fi
done
exit $fail
