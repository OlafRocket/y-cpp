# y-cpp — Minimal feature set for a Yjs-compatible text CRDT

This document defines the smallest feature set that lets a C++ process
synchronize plain text with real Yjs peers (browser or Node) using the
standard Yjs **v1 binary update format**. Everything in scope is required
for wire-level conformity; everything out of scope can be layered on later
without changing the core.

Reference implementations used while writing this code:

- [yjs](https://github.com/yjs/yjs), the canonical JavaScript
  implementation (cloned in `/mnt/work/crdt/yjs`). Port baseline v13.6.27,
  pinned reference v13.6.33; see section 4 and `docs/UPSTREAM_TRACKING.md`
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
Merging adjacent items is an optional optimization and is *not* required for
conformity — y-cpp implements it since 0.2 (yjs `Item.mergeWith` conditions:
same client, contiguous clocks, list-adjacent, matching origins and deleted
flag). The conditions guarantee a later split reconstructs the original
origins exactly, so merging is invisible on the wire.

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
- Y.Xml* types and subdocuments: decoded and preserved on the wire, no API.
- Rich-text formatting attributes and embeds (`ContentFormat`,
  `ContentEmbed`): decoded and preserved on the wire, no API.
- Snapshots.
- Deep (subtree) observers (`observeDeep`).
- Any network transport (the protocol modules are byte-array in/out).

Implemented since 0.2 (was out of scope in 0.1):

- Item merging and tombstone garbage collection: deleted droppable content
  (string/any/binary) is replaced with `ContentDeleted` (yjs `Item.gc` with a
  live parent) and adjacent mergeable items are compacted, mirroring yjs's
  transaction cleanup. Tombstones themselves are kept — deleted nested types
  keep their type ref and are not replaced with `GC` structs (equivalent to a
  yjs doc with `gc: false` for type nodes); GC structs from peers are still
  accepted.

Implemented since 0.8 (was out of scope in 0.1):

- UndoManager: a port of yjs UndoManager/redoItem/keepItem/followRedone —
  origin-filtered capture with timeout grouping, keep-flagged tombstones
  exempt from GC while revivable, and redone chains linking deleted items to
  their revived copies (followed by relative positions).

Implemented since 0.7 (was out of scope in 0.1):

- Relative positions (yjs RelativePosition, without followRedone), and — in
  the separate `ycpp-protocols` target, mirroring the yjs / y-protocols
  package split — the sync handshake and the awareness protocol, verified
  end-to-end against the stock y-websocket provider.

Implemented since 0.4 (was out of scope in 0.1):

- Transactions (`doc.transact`, implicit per-mutation transactions, origin
  tags) and observers: yjs `YEvent.changes` list deltas and `YEvent.keys`
  map-key changes, computed at transaction cleanup before garbage
  collection/merging (which now runs at end of transaction, as in yjs), plus
  doc-level update events (`writeUpdateMessageFromTransaction` equivalent).

Implemented since 0.3 (was out of scope in 0.1):

- Y.Array and Y.Map, on root and nested types: `ContentAny` (lib0 "Any"
  values), `ContentBinary`, `ContentType` with `parentSub` map semantics
  (key chains, last-writer-wins by YATA order), parents addressed by item ID,
  and cascade deletion of nested subtrees (`ContentType.delete`).

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

## 4. Upstream review

Pinned yjs tag: v13.6.33 (2026-09-23). The port baseline is v13.6.27; the
differences between the two tags are classified in
`docs/UPSTREAM_TRACKING.md`, which also records the yjs 14 and y-octo
findings.

Core files watched in the yjs clone (`/mnt/work/crdt/yjs`):

| Area | Files |
|---|---|
| structs | `src/structs/Item.js`, `src/structs/Content*.js`, `src/structs/GC.js`, `src/structs/Skip.js` |
| store and transactions | `src/utils/StructStore.js`, `src/utils/Transaction.js`, `src/utils/DeleteSet.js` |
| codec | `src/utils/UpdateEncoder.js`, `src/utils/UpdateDecoder.js`, `src/utils/encoding.js`, `src/utils/updates.js` |
| features | `src/utils/UndoManager.js`, `src/utils/RelativePosition.js`, `src/utils/ID.js` |
| types | `src/types/YText.js`, `src/types/YArray.js`, `src/types/YMap.js`, `src/types/AbstractType.js` |
| y-protocols (separate package, pinned 1.0.7 in `tests/interop/package.json`) | `sync.js`, `awareness.js` |

Procedure for a new upstream tag:

1. `git -C /mnt/work/crdt/yjs fetch --tags`
2. `tools/upstream-diff.sh vX.Y.Z` prints the stat and the diff of the core
   files between the pin and the tag (`tools/upstream-diff.sh` alone diffs
   against the clone's HEAD).
3. Classify every hunk as one of: convergence or encoding change (mirror it
   in y-cpp and add a test), feature not ported, irrelevant. Record the
   result in `docs/UPSTREAM_TRACKING.md` with the y-cpp file and function.
4. Run the interop suite against the tag:
   `YCPP_INTEROP_YJS=yjs@X.Y.Z tests/interop/run_interop.sh build/ycpp`.
   The ctest job `interop-yjs13-latest` (CMake option `YCPP_INTEROP_LATEST13`,
   on by default) does the same for the latest 13.x; `YCPP_INTEROP_V14_SPEC`
   adds an optional job for a yjs 14 package.
5. Update the "Pinned yjs tag" line above; the script reads the pin from it.
