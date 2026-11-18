#!/usr/bin/env bash
# Cross-compile the automation server for the Raspberry Pi (aarch64 Linux) in Docker.
# Much faster than building on the Pi (905 MB RAM). Output: build/server-aarch64/automation-server
#
#   scripts/build-server-pi.sh
#
# Built on Debian bookworm (glibc 2.36), so it runs on the Pi's newer Debian (glibc is backward compatible).
# Deploy: see server/README.md ("Deploy with a prebuilt binary").
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/build/server-aarch64"
mkdir -p "$OUT"

# Docker on Windows (Git Bash) needs a Windows-style path and no path mangling.
SRC="$ROOT/server"
if command -v cygpath >/dev/null 2>&1; then
  SRC="$(cygpath -w "$SRC")"; OUT_HOST="$(cygpath -w "$OUT")"; export MSYS_NO_PATHCONV=1
else
  OUT_HOST="$OUT"
fi

docker run --rm \
  -v "$SRC:/src:ro" -v "$OUT_HOST:/out" \
  -v automation-cargo:/usr/local/cargo/registry -v automation-target-aarch64:/target \
  -e CARGO_TARGET_DIR=/target \
  -e CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER=aarch64-linux-gnu-gcc \
  -e CC_aarch64_unknown_linux_gnu=aarch64-linux-gnu-gcc \
  -e AR_aarch64_unknown_linux_gnu=aarch64-linux-gnu-ar \
  -w /src rust:1-bookworm bash -c '
    set -e
    command -v aarch64-linux-gnu-gcc >/dev/null || { apt-get update -qq && apt-get install -y -qq gcc-aarch64-linux-gnu libc6-dev-arm64-cross >/dev/null; }
    rustup target add aarch64-unknown-linux-gnu >/dev/null 2>&1
    cargo build --release --locked --target aarch64-unknown-linux-gnu
    cp /target/aarch64-unknown-linux-gnu/release/automation-server /out/
  '
file "$OUT/automation-server" 2>/dev/null || true
sha256sum "$OUT/automation-server"
