# y-cpp

A minimal C++ implementation of the [Yjs](https://github.com/yjs/yjs) CRDT
protocol (v1 binary encoding), interoperable at the wire level with real Yjs
peers. It covers **collaborative plain text (Y.Text), sequences (Y.Array),
and key-value maps (Y.Map)** — including nested combinations — so multiple
parties (browsers running yjs, Node processes, native C++ programs) edit the
same document concurrently and converge to the identical result.

The minimal feature set and its rationale are described in
[docs/MINIMAL_FEATURES.md](docs/MINIMAL_FEATURES.md). The code is a direct
port of the relevant parts of yjs v13 (struct store, YATA integration,
lib0 v1 update codec); the Rust implementation
[y-octo](https://github.com/y-crdt/y-octo) was used as a second reference.

## What works

- `Y.Text`: insert / delete, UTF-16 indexing (JS semantics), full unicode
  incl. surrogate pairs
- `Y.Array` and `Y.Map` (since 0.3): JSON-ish values (lib0 "Any" encoding),
  binary values, arbitrary nesting of arrays/maps/texts, cascade deletion of
  nested types, `parentSub` map semantics with yjs-identical conflict winners
- Legacy/unsupported content (`ContentJSON`, `ContentEmbed`, `ContentFormat`,
  `ContentDoc`) is decoded, preserved, and re-encoded opaquely instead of
  rejected
- Transactions and observers (since 0.4): `doc.transact()` batching with
  origin tags, `observe()` on any type (Quill-style deltas for Text/Array,
  per-key changes with old values for Map, for local and remote changes
  alike), and `doc.onUpdate()` delivering each transaction as an incremental
  v1 update — the hook a sync provider subscribes to
- Yjs **v1 update** encode/decode: `applyUpdate`, `encodeStateAsUpdate`,
  `encodeStateVector`, state-vector–based diff sync
- YATA conflict resolution — byte-for-byte the same ordering decisions as yjs
- Tombstone deletes with DeleteSets, item splitting, GC structs from peers
- Item merging + tombstone garbage collection (since 0.2): adjacent items
  merge back together and deleted content is dropped, so memory and update
  size are bounded by the document's current content, not its edit history
- Out-of-order update delivery (pending structs/deletes, retried and
  preserved across re-encodes)

Out of scope for now: XML types (decoded/preserved, no API), rich-text
attributes, subdocuments, deep (subtree) observers, v2 encoding, undo,
awareness, transports. See the feature doc.

## Project layout

Headers are split by concern; `#include <ycpp/ycpp.hpp>` is the umbrella that
pulls in everything (individual headers can be included directly):

```
include/ycpp/
  lib0.hpp      lib0 binary primitives (varints, strings, floats)
  utf.hpp       UTF-8 <-> UTF-16 with JavaScript semantics
  any.hpp       the lib0 "Any" value model + binary codec
  json.hpp      JSON text output for Any values
  item.hpp      Item: the CRDT content atom (+ ID, content kinds)
  event.hpp     observer events, deltas, map changes, Subscription
  type.hpp      YType: the shared type node (list start + key map)
  wrappers.hpp  Text / Array / Map public handles
  doc.hpp       Doc: store, transactions, update codec entry points
  ycpp.hpp      umbrella header
src/
  doc.cpp           construction, arenas, root types, validate
  store.cpp         item splitting, deletion, merging, freeing
  integrate.cpp     YATA integration, pending structs, delete ranges
  update_codec.cpp  Yjs v1 update decode/encode
  transaction.cpp   transactions, events, observers, subscriptions
  types.cpp         list/map operations, wrappers, JSON dump
  internal.hpp      shared implementation helpers (not installed)
```

## Build & test

Requires a C++20 compiler (GCC 10+, Clang 13+, MSVC 19.29+) and CMake 3.16+.

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

auto arr = a.getArray("list");
arr.push(ycpp::Any::of(42));
arr.push(ycpp::Any::of("hi"));

auto meta = a.getMap("meta");
meta.set("title", ycpp::Any::of("demo"));
auto nested = meta.setMap("nested");     // nested shared types
nested.set("deep", ycpp::Any::of(true));

b.applyUpdate(a.encodeStateAsUpdate(b.encodeStateVector()));
// b.getText().toString() == "hello"
// b.toJson() == a.toJson()

// Observers + update events (fire for local and remote changes alike).
// subscribe() returns a RAII Subscription that detaches on destruction;
// observe() returns a manual handle for unobserve().
ycpp::Subscription sub =
    a.getText().subscribe([](const ycpp::Event& e) { /* e.delta, e.origin */ });
a.onUpdate([](const std::vector<uint8_t>& update, const std::string& origin) {
  /* forward `update` to peers */
});
a.transact([&] { /* several edits -> one event, one update */ }, "my-origin");
```

## License

MIT (see LICENSE). Portions are ported from [yjs](https://github.com/yjs/yjs)
(MIT, © Kevin Jahns). `third_party/` contains the vendored
[macaron Base64](https://github.com/tomykaira/Base64.h) codec (MIT,
© 2016 tomykaira) used by the CLI.
