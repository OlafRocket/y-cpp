#!/usr/bin/env bash
# Runs the yjs interop conformity tests. Usage: run_interop.sh <path-to-ycpp-cli>
set -euo pipefail
cd "$(dirname "$0")"
YCPP="${1:?usage: run_interop.sh <path-to-ycpp-cli>}"
NODE="${NODE:-node}"

if [ ! -d node_modules/yjs ]; then
  echo "installing yjs for interop tests..."
  npm install --no-audit --no-fund --loglevel=error
fi

exec "$NODE" interop.test.js "$YCPP"
