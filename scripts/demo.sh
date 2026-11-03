#!/usr/bin/env bash
# ============================================================================
# Kestrel one-command demo (V1, software engine).
#
#   scripts/demo.sh [--quick] [--port N]
#
# Pipeline: build -> RECORD a real engine session (journal) -> project it to
# JSONL snapshots (cold-path exporter) -> serve the dark-theme console.
# The console then plays the RECORDING of real engine behavior — no scripted
# data. Requires: cmake, ninja, g++ (GCC 14 tested), python3, curl.
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."

QUICK=""
PORT="${PORT:-8901}"
if [[ "${1:-}" == "--quick" ]]; then QUICK="--quick"; shift; fi
if [[ "${1:-}" == "--port" ]]; then PORT="${2:?}"; shift 2; fi

echo "==> [1/4] building release preset"
cmake --preset release > /dev/null
cmake --build --preset release

echo "==> [2/4] recording a real engine session (journal)"
mkdir -p web/demo-data
./build/release/tools/kestrel_record_demo web/demo-data/demo.journal ${QUICK:+3000}

echo "==> [3/4] projecting journal -> JSONL snapshots (cold path)"
./build/release/tools/kestrel_journal_export \
  --in web/demo-data/demo.journal \
  --out web/demo-data/demo.jsonl --every 40

echo "==> [4/4] serving console on http://localhost:${PORT}/demo.html"
(cd web && exec python3 -m http.server "$PORT") &
SRV=$!
trap 'kill "$SRV" 2>/dev/null || true' EXIT
sleep 1
echo
echo "Kestrel demo ready:  http://localhost:${PORT}/demo.html"
echo "Press Ctrl-C to stop the server."
wait "$SRV"
