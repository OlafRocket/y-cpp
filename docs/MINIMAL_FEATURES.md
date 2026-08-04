# y-cpp — Minimal feature set for a Yjs-compatible text CRDT

This document defines the smallest feature set that lets a C++ process
synchronize plain text with real Yjs peers (browser or Node) using the
standard Yjs **v1 binary update format**. Everything in scope is required
for wire-level conformity; everything out of scope can be layered on later
without changing the core.

Reference implementations used while writing this code:

- [yjs (v13.6.x)](https://github.com/yjs/yjs) — the canonical JavaScript
  implementation (cloned in `/mnt/work/crdt/yjs`)
- [y-octo](https://github.com/y-crdt/y-octo) — a Rust implementation of the
  same protocol
- [docs.yjs.dev](https://docs.yjs.dev/) — user-level documentation,
  especially "Document Updates" and "Internals"

## 1. Concepts that must be implemented

### 1.1 IDs and the struct store

Every atom of content is an **Item** (yjs calls them "structs") identified by
`ID = (clientID, clock)`:

- `clientID` — random 32-bit integer chosen per document session.
- `clock` — per-client Lamport counter. An item of length `n` occupies clock
  range `[clock, clock + n)`. **For text, length is counted in UTF-16 code
  units** (JavaScript string semantics) — a C++ implementation must do its
  clock arithmetic in UTF-16 units, not bytes or code points.

The **struct store** keeps, per client, an array of items sorted by clock with
no gaps. The **state vector** maps each known client to the next expected
clock (`last.clock + last.length`).

### 1.2 Items (the doubly linked list + YATA)

Text is a doubly linked list of items. Each item carries:

| field         | meaning                                                          |
|---------------|------------------------------------------------------------------|
| `id`          | (client, clock) of the first element                             |
| `origin`      | ID of the element that was immediately left at insertion time    |
| `rightOrigin` | ID of the element that was immediately right at insertion time   |
| `parent`      | the containing type — for us always a *root* type name (`ykey`)  |
| `parentSub`   | map key — always absent for text (we reject it)                  |
| `content`     | see 1.3                                                          |

Concurrent inserts at the same position are ordered by the **YATA**
integration algorithm (`Item.integrate` in yjs) using `origin`/`rightOrigin`
and, as a tiebreaker, the smaller `clientID` first. This algorithm must be
ported exactly — it is the heart of conflict resolution.

Items must be **splittable** at any UTF-16 offset (a remote peer may insert
into the middle of a run, or delete half of it). Splitting inside a surrogate
pair replaces both halves' boundary units with U+FFFD, exactly like yjs.
Merging adjacent items is an optional optimization and is *not* required.

### 1.3 Content types (minimal subset)

Of the 11 yjs content refs, three are needed for plain text:

| ref | name             | purpose                                            |
|-----|------------------|----------------------------------------------------|
| 0   | `GC`             | tombstone with no content (from peers that ran GC) |
| 1   | `ContentDeleted` | deleted range, content dropped, length kept        |
| 4   | `ContentString`  | a run of text (UTF-8 on the wire, UTF-16 lengths)  |
| 10  | `Skip`           | gap marker in diff updates (decode-only)           |

Everything else (`JSON`, `Binary`, `Embed`, `Format`, `Type`, `Any`, `Doc`)
is rejected with a clear "unsupported in minimal build" error.

### 1.4 Deletes and the DeleteSet

Deletes never remove items; they mark them as tombstones. A **DeleteSet** —
`client → [(clock, len), …]` — travels with every update. Applying a delete
range may require splitting items at the range boundaries. Content of locally
deleted `ContentString` items may be dropped (yjs replaces it with
`ContentDeleted` on transaction cleanup); their clock length must be kept.

### 1.5 Update decoding/encoding (v1 format, lib0)

The wire format uses **lib0** primitives: unsigned LEB128 var-ints
(`writeVarUint`) and length-prefixed UTF-8 strings (`writeVarString`).

An **update** is:

```
update      := clientStructs deleteSet
clientStructs := numClients { numStructs client startClock struct* }   // clients in descending order
struct      := info(uint8) [origin] [rightOrigin] [parentInfo parent [parentSub]] content
deleteSet   := numClients { client numRanges { clock len }* }
```

`info` packs the content ref in the low 5 bits, plus flags 0x80 (origin
present), 0x40 (rightOrigin present), 0x20 (parentSub present). Parent info
is only encoded when *both* origins are absent.

Required operations:

- `applyUpdate(doc, update)` — decode, integrate structs (handling offsets
  for already-known prefixes), apply the delete set. Structs whose causal
  dependencies (origin / rightOrigin / same-client predecessor) are missing
  are parked in a **pending set** and retried after later updates — Yjs
  explicitly allows out-of-order update delivery.
- `encodeStateVector(doc)`
- `encodeStateAsUpdate(doc, remoteStateVector?)` — everything the remote is
  missing, plus the full delete set.

This yields the standard two-step sync: exchange state vectors, exchange
diff updates. (The `y-protocols` sync/awareness message framing and
websocket transport are *not* part of the core and are out of scope.)

### 1.6 The Y.Text type (root types only)

- `doc.getText(name)` — root types addressed by name (`ykey`); y-cpp uses
  `"default"` as its conventional root key.
- `insert(index, string)` / `remove(index, length)` — index in UTF-16 units,
  positions found by walking the list and skipping tombstones.
- `toString()` — concatenation of non-deleted string content.
- `length()` — UTF-16 length.

## 2. Explicitly out of scope (for now)

- v2 update encoding (`applyUpdateV2`) — v1 is what yjs uses by default.
- Y.Map / Y.Array / Y.XmlFragment / subdocuments (`parentSub`, `ContentType`).
- Rich-text formatting attributes and embeds (`ContentFormat`, `ContentEmbed`).
- Undo manager, snapshots, relative positions.
- Observers/events, transactions with origin tracking.
- Garbage collection / item merging (we keep tombstones; we still *accept*
  GC structs from peers).
- Awareness protocol and any network transport (sync is byte-array in/out;
  the demo exchanges base64 blobs).

## 3. Conformity test strategy

1. **C++ unit tests** — lib0 var-int round-trips, UTF-8/16 conversion,
   splitting (incl. surrogate pairs), local editing, DeleteSet encoding.
2. **Convergence tests** — N docs, random concurrent edits, pairwise sync in
   random order until quiescent → all docs byte-identical (CRDT property).
3. **Interop tests against real yjs** (Node): updates generated by yjs are
   applied by y-cpp and vice versa; concurrent edits from both sides must
   converge to identical text. This is the actual conformity check.
4. **Live browser test** — `web/index.html` runs two real Yjs peers and can
   exchange base64 updates with the `ycpp` CLI.
