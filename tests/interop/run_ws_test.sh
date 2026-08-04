#!/usr/bin/env bash
# Runs the ycpp-sync websocket end-to-end test. Usage: run_ws_test.sh <path-to-ycpp-sync>
set -euo pipefail
cd "$(dirname "$0")"
BIN="${1:?usage: run_ws_test.sh <path-to-ycpp-sync>}"
NODE="${NODE:-node}"

if [ ! -d node_modules/yjs ] || [ ! -d node_modules/ws ]; then
  echo "installing yjs + ws for websocket tests..."
  npm install --no-audit --no-fund --loglevel=error
fi

exec "$NODE" websocket.test.js "$BIN" "$(cd ../../web && pwd)"
