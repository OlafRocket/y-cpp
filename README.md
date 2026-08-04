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
  if node is not found)

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

## Live browser test

`web/index.html` hosts two real Yjs peers with editable text areas. Serve it
(CDN import of yjs, so any static server works):

```sh
python3 -m http.server -d web 8000
# open http://localhost:8000
```

Edit both peers, hit *Sync*, and exchange base64 updates with the C++ side
via copy/paste:

```sh
./build/ycpp new /tmp/doc.ydoc
./build/ycpp apply /tmp/doc.ydoc BASE64   # paste a peer's "Export state"
./build/ycpp insert /tmp/doc.ydoc 0 "from C++ "
./build/ycpp export /tmp/doc.ydoc         # paste into a peer's "Apply update"
```

All three parties converge to the same text.

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
