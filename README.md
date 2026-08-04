# y-cpp

A minimal C++ implementation of the [Yjs](https://github.com/yjs/yjs) CRDT
protocol (v1 binary encoding), interoperable at the wire level with real Yjs
peers. The current scope is **collaborative plain text**: multiple parties —
browsers running yjs, Node processes, and native C++ programs — edit the same
text concurrently and converge to the identical result.

The minimal feature set and its rationale are described in
[docs/MINIMAL_FEATURES.md](docs/MINIMAL_FEATURES.md). The code is a direct
port of the relevant parts of yjs v13 (struct store, YATA integration,
lib0 v1 update codec); the Rust implementation
[y-octo](https://github.com/y-crdt/y-octo) was used as a second reference.

## What works

- `Y.Text` on root types: insert / delete, UTF-16 indexing (JS semantics),
  full unicode incl. surrogate pairs
- Yjs **v1 update** encode/decode: `applyUpdate`, `encodeStateAsUpdate`,
  `encodeStateVector`, state-vector–based diff sync
- YATA conflict resolution — byte-for-byte the same ordering decisions as yjs
- Tombstone deletes with DeleteSets, item splitting, GC structs from peers
- Out-of-order update delivery (pending structs/deletes, retried and
  preserved across re-encodes)

Out of scope for now: Y.Map/Y.Array/XML types, rich-text attributes, v2
encoding, undo, awareness, transports. See the feature doc.

## Build & test

```sh
cmake -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Tests:

- `tests/test_main.cpp` — unit tests + multi-doc random-op convergence fuzzing
- `tests/interop/` — **conformity suite**: exchanges real binary updates with
  the reference yjs implementation via Node (auto-`npm install`s yjs; skipped
  if node is not found), plus an end-to-end test of the `ycpp-sync` live
  server with concurrent yjs peers over WebSocket

## CLI

`build/ycpp` manipulates doc files that are raw Yjs v1 updates — any Yjs peer
can read them directly:

```sh
./build/ycpp new    doc.ydoc
./build/ycpp insert doc.ydoc 0 "hello"     # insert at UTF-16 index
./build/ycpp delete doc.ydoc 0 2
./build/ycpp show   doc.ydoc
./build/ycpp export doc.ydoc               # base64 update for other peers
./build/ycpp apply  doc.ydoc BASE64        # apply an update from another peer
./build/ycpp sv     doc.ydoc               # base64 state vector
./build/ycpp diff   doc.ydoc BASE64_SV     # diff update vs a remote state vector
```

The text lives in the Yjs root type `"default"`.

## Live sync (WebSocket)

`ycpp-sync` (built on [IXWebSocket](https://github.com/machinezone/IXWebSocket),
fetched automatically by CMake; disable with `-DYCPP_BUILD_SYNC=OFF`) hosts a
shared document behind an HTTP + WebSocket server — the CRDT logic stays
entirely inside the ycpp library, the tool only moves update bytes:

```sh
./build/ycpp-sync serve                     # http://localhost:8765
```

- **Browsers**: open `http://localhost:8765/` (serves `web/live.html`, a real
  Yjs peer) in any number of windows — every keystroke syncs live.
- **Native peers**: `./build/ycpp-sync connect ws://localhost:8765` opens a
  minimal terminal text box (POSIX terminals) editing the same text with its
  own ycpp document. `serve` itself also opens the text box when run on a TTY;
  use `--headless` on servers/Windows.
- `--doc FILE` persists the document as a Yjs v1 update file (readable by
  `ycpp` and by yjs itself).

Protocol: on connect a peer receives the full state as one Yjs v1 update and
offers its own; afterwards every binary WebSocket message is an incremental
update relayed to all other peers. Updates are idempotent and commutative, so
this trivial relay is a correct sync provider.

## Manual browser test (no server)

`web/index.html` hosts two Yjs peers with manual sync buttons and base64
import/export boxes for copy/paste exchange with the `ycpp` CLI — useful for
inspecting individual updates. Serve with any static server, e.g.
`python3 -m http.server -d web 8000`.

## Library usage

```cpp
#include <ycpp/ycpp.hpp>

ycpp::Doc a, b;
a.getText().insert(0, "hello");
b.applyUpdate(a.encodeStateAsUpdate(b.encodeStateVector()));
// b.getText().toString() == "hello"
```

## License

MIT (see LICENSE). Portions are ported from [yjs](https://github.com/yjs/yjs)
(MIT, © Kevin Jahns).
