#!/usr/bin/env bash
# Runs the yjs interop conformity tests. Usage: run_interop.sh <path-to-ycpp-cli>
#
# Default: the yjs version pinned in package.json (installed into node_modules
# on first run). YCPP_INTEROP_YJS=<npm spec> runs the same suite against another
# package, e.g. "yjs@13" (latest 13.x) or "@y/y@14.0.0-rc.29". Each spec is
# installed once into .yjs-versions/<spec>/node_modules; delete that directory
# to resolve the spec again (a range such as yjs@13 stays at the version first
# installed).
set -euo pipefail
cd "$(dirname "$0")"
YCPP="${1:?usage: run_interop.sh <path-to-ycpp-cli>}"
NODE="${NODE:-node}"

if [ ! -d node_modules/yjs ]; then
  echo "installing yjs for interop tests..."
  npm install --no-audit --no-fund --loglevel=error
fi

SPEC="${YCPP_INTEROP_YJS:-}"
if [ -n "$SPEC" ]; then
  # "@y/y@14.0.0-rc.29" -> name "@y/y", version "14.0.0-rc.29"; "yjs@13" -> "yjs", "13"
  rest="${SPEC#@}"
  name="${rest%%@*}"
  [ "$rest" != "$SPEC" ] && name="@$name"
  version="latest"
  [ "${rest#*@}" != "$rest" ] && version="${rest#*@}"
  dir="$PWD/.yjs-versions/$(printf '%s' "$SPEC" | tr -c 'A-Za-z0-9._-' '_')"
  if [ ! -d "$dir/node_modules/$name" ]; then
    echo "installing $SPEC into $dir ..."
    mkdir -p "$dir"
    printf '{"name":"ycpp-interop-yjs-version","private":true,"dependencies":{"%s":"%s"}}\n' \
      "$name" "$version" > "$dir/package.json"
    (cd "$dir" && npm install --no-audit --no-fund --loglevel=error)
  fi
  export YCPP_INTEROP_YJS_DIR="$dir"
  export YCPP_INTEROP_YJS_NAME="$name"
fi

exec "$NODE" interop.test.js "$YCPP"
