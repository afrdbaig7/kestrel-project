#!/usr/bin/env bash
# ============================================================================
# Kestrel CONNECTED-mode demo (ADR-0007 closed): record -> project -> serve
# the React/TS/Vite console through the Rust WebSocket gateway.
#
#   scripts/demo_connected.sh [--port N]
#
# Prereqs (one-time):
#   cmake --preset release && cmake --build --preset release   (C++ tools)
#   (cd services/gateway && cargo build --release)             (Rust gateway)
#   (cd web/console && npm install && npm run build)           (React console)
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."

PORT="${PORT:-8902}"
if [[ "${1:-}" == "--port" ]]; then PORT="${2:?}"; fi

echo "==> [1/3] recording a real engine session (journal)"
mkdir -p web/demo-data
./build/release/tools/kestrel_record_demo web/demo-data/demo.journal

echo "==> [2/3] projecting journal -> JSONL snapshots (cold path)"
./build/release/tools/kestrel_journal_export \
  --in web/demo-data/demo.journal \
  --out web/demo-data/demo.jsonl --every 40

echo "==> [3/3] gateway serving console + WS on http://localhost:${PORT}/"
exec services/gateway/target/release/kestrel-gateway \
  --jsonl web/demo-data/demo.jsonl --static web/console-dist --port "$PORT"
