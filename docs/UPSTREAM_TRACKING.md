# Upstream tracking: yjs changes relevant to y-cpp

Scope: convergence, v1 encoding and the ported features (integration and
pending structs, delete sets and garbage collection, Y.Map conflict rules,
Y.Text, Y.Array, UndoManager, relative positions, y-protocols sync and
awareness). Typings, bundling, API sugar, XML types and deep observers are
out of scope.

Review date: 2026-10-28. Pinned reference: yjs v13.6.27 (port baseline).
Recommended pin after this review: v13.6.33.

## 1. Sources and clone state

| Source | State used for this report | Clone state |
|---|---|---|
| yjs 13.x tags v13.6.28 to v13.6.33 | GitHub release tarballs | `/mnt/work/crdt/yjs` is checked out at v13.6.27 (987c9eb, 2025-05-13); tags fetched 2026-10-28, so `tools/upstream-diff.sh` can diff against them |
| yjs v14 | v14.0.0-rc.29 tarball (2026-10-15, npm package `@y/y`) | `/mnt/work/crdt/yjs-v14` is at rc.24 (ceae18f, 2026-08-04); rc.25 to rc.29 change the core files by under 30 lines each except BlockSet.js (+55/-71); `ytype.js` is renamed `ynode.js`; INTERNALS.md and attributing-content.md are identical to rc.29 |
| y-protocols | v1.0.7 tag and master (1.0.6-rc.1, the v14 line) tarballs | no clone; `tests/interop/node_modules/y-protocols` is 1.0.7 and byte-identical to the tag |
| y-octo | GitHub commits, PRs and issues since 2025-05-01 | `/mnt/work/crdt/y-octo` is a single-commit checkout of PR #59 (3936a47, 2026-07-30), so it carries no history; `origin/main` fetched 2026-10-28 has ten later commits, one substantive (#70) |

Interop suite: `tests/interop/package.json` asks for `yjs ^13.6.27`;
`package-lock.json` and the installed `node_modules/yjs` resolve to
13.6.32. The suite therefore already runs against 13.6.32, not 13.6.27.

## 2. 13.x line: v13.6.28 to v13.6.33

Tags after v13.6.27:

| Tag | Date | Core files touched | Other files |
|---|---|---|---|
| v13.6.28 | 2025-12-17 | none | YEvent.js (remove `newValue`), YXmlElement.js (clone non-string attributes), README, testHelper |
| v13.6.29 | 2026-01-03 | Transaction.js | none |
| v13.6.30 | 2026-03-14 | DeleteSet.js | snapshot.tests.js |
| v13.6.31 | 2026-05-28 | Item.js | undo-redo.tests.js |
| v13.6.32 | 2026-08-04 | UndoManager.js | none |
| v13.6.33 | 2026-09-23 | Transaction.js | y-map.tests.js |

Hunks in the core file list:

| Tag | File, function | Change | Class | y-cpp counterpart |
|---|---|---|---|---|
| v13.6.29 | Transaction.js `cleanupTransactions` | Deep-event dispatch, the `afterTransaction` emit and YText formatting cleanup are queued through `callAll` so a throwing deep handler no longer skips the rest (commit cff7de11) | Irrelevant: deep observers and formatting are not ported; event ordering, not state | `Doc::cleanupTransaction` (src/transaction.cpp), no change |
| v13.6.30 | DeleteSet.js `sortAndMergeDeleteSet` | Merging allocates a new `DeleteItem` instead of mutating the shared one; `mergeDeleteSets` no longer corrupts its inputs, which made `snapshotContainsUpdate` return true wrongly (#767) | Irrelevant: no wire change; y-cpp delete ranges are value types and every merge builds a new vector; snapshots are not ported | `Doc::cleanupTransaction` step 1 (src/transaction.cpp), `Doc::writeDeleteSet` (src/update_codec.cpp), `UndoManager::dsSortAndMerge` (src/undo.cpp), no change |
| v13.6.31 | Item.js `redoItem`, map (`parentSub`) branch | After `left` is chosen for a map item, it is dropped when `left.parent._item` is not the (possibly redone) target parent, and re-read from `parentType._map`. Without this the redone item carried an `origin` inside the deleted old parent, the encoder omitted parent info (origin present), and remote peers attached the item to the deleted type: the restored key was missing after sync while the local doc was correct (#757) | Encoding and convergence fix: must be mirrored | `UndoManager::redoItem`, map branch (src/undo.cpp:311-328). Two corrections: (a) the else branch reads `item->parent->map`, upstream reads `parentType._map` (the redone parent) since before v13.6.27; (b) add the cross-parent check from v13.6.31. Trigger in y-cpp terms: nested Y.Map in a Y.Array, set a key and delete the map in one capture group, undo, sync. Before the fix this scenario aborted in `Doc::validate` (map value not the rightmost item); unit test `undo_map_key_then_parent_delete_syncs` |
| v13.6.32 | UndoManager.js constructor, `destroy` | `destroy` is bound once and removed from the doc `destroy` event on `destroy()` (#797, handler leak) | Irrelevant: lifecycle, no state | `UndoManager::~UndoManager` unregisters its `afterTransaction` handler (src/undo.cpp); `Doc` has no destroy event; no change |
| v13.6.33 | Transaction.js `cleanupTransactions` | `currentTarget` and `_path` of deep events are set immediately before calling each type's handlers because one event instance is shared by all ancestors (#768, #801) | Irrelevant: deep observers not ported | none |

Changes in v13.6.28 outside the core list (`YEvent.newValue`, XML element
cloning) are JavaScript API changes.

y-protocols: v1.0.7 (2025-12-16) adds an optional error handler argument to
`readSyncMessage`; message types 0, 1, 2 and the awareness encoding are
unchanged. master (1.0.6-rc.1) differs from v1.0.7 only in imports
(`@y/y`, `ObservableV2`). The sync and awareness ports need no change.

Net result for the port from the 13.x line: one change to mirror (#757 in
`UndoManager::redoItem`), plus the pre-existing map lookup divergence found
next to it.

## 3. v14 release candidate (v14.0.0-rc.29)

### 3.1 Wire format and integration algorithm

| Area | v14 state | Evidence |
|---|---|---|
| v1 update layout | Unchanged | `Item.write` differs from v13 only by an `offsetEnd` parameter (used to write partial structs for `intersectUpdateWithContentIds`); info bits, origin, rightOrigin, parent info and parentSub are the same |
| Content refs 0 to 10 | Unchanged | `contentRefs` in ynode.js lists the same eleven entries; `readContentDeleted`, `JSON`, `Binary`, `String`, `Embed`, `Format`, `Any` are byte-identical to v13; `ContentType` still writes the type ref and the XmlElement or XmlHook name (`_legacyTypeRef`); `ContentDoc` still writes guid string and opts Any |
| Type ref of nested types | Layout unchanged, value changed | A `YNode` created locally by v14 carries `_legacyTypeRef` 4 (unnamed, the former YXmlFragment ref) or 3 (named), whatever it is used for; decoded nodes keep the ref they were received with. See 3.4 |
| Delete set section | Unchanged | `writeIdSet` writes numClients, client, numRanges, clock, len with the V1 varuint encoders; `DSEncoderV1` is renamed `IdSetEncoderV1` |
| State vector | Unchanged | `writeStateVector` and `readStateVector` unchanged |
| Default codec | v1 | `applyUpdate`, `encodeStateAsUpdate`, `encodeStateVector` are the V1 entry points; `encodeStateVectorV2` and friends are separate |
| YATA integration | Unchanged | `Item.integrate` differs from v13.6.33 only by type-name casts and `store.add`; Map conflict rule (`_map.set` when `right === null`, delete `left`) identical |
| Compatibility tests | Present | `tests/compatibility.tests.js` decodes v1 documents generated with yjs 13.2.0 |

### 3.2 Behaviour changes below the wire (library level)

| Change | Effect | y-cpp impact |
|---|---|---|
| Struct store with gaps. `integrateStructs` no longer parks a struct whose same-client predecessor is missing: it inserts a `Skip` into `store.skips`, integrates the struct, and later splices the missing structs into the gap (`StructStore.add`). `getMissing` treats an origin, rightOrigin or parent that falls inside a skip as missing, so causal dependencies are still honoured | `getStateVector` reports the clock of the first gap (the same value a 13.x peer reports for pending structs). `encodeStateAsUpdate` emits `Skip` structs (ref 10) between runs of a client; in 13.x these appeared only in `mergeUpdates` and `diffUpdate` output | None required. `Doc::applyUpdate` (src/update_codec.cpp:31) decodes `Skip` by advancing the clock; `Doc::integrateLoop` (src/integrate.cpp) parks structs after a gap until the gap is filled. Recommended: an interop case that applies a v14 update containing a mid-stream `Skip` |
| `generateNewClientId` is `random.uint53` instead of `random.uint32` | Client IDs up to 2^53 in every v1 message, state vector, delete set and awareness update | None. y-cpp carries client IDs as `uint64_t` in `ID`, the store, state vectors, delete sets and `Awareness`; the descending-client write order uses numeric comparison. Ports with 32-bit client IDs break; y-octo uses `u64` |
| Transaction bookkeeping as `IdSet`s (`insertSet`, `deleteSet`, `cleanUps`); `beforeState` and `afterState` derived lazily from `insertSet` | GC, merge and `ContentType` change tracking moved to transaction-helpers.js with the same rules (`tryGcDeleteSet`, `tryToMergeWithLefts`) | None |
| `readAndApplyDeleteSet` keeps a delete range that hits a `Skip` in the store as unapplied (`pendingDs`) | Delete sets for content in a gap are retried later | None; y-cpp has no gaps in the store, so the range stays in `pendingDs_` until the structs arrive |
| All shared types are one class `YNode` (ynode.js); the type ref survives only as `_legacyTypeRef` for encoding | API change | None |
| `Doc` option `isSuggestionDoc` disables formatting cleanup | API change | None |
| `UndoManager`: `StackItem` holds `inserts` and `deletes` IdSets; option renamed `ignoreRemoteAttributeChanges`; new `undoContentIds` | API change; `redoItem` carries the #757 fix | None beyond section 2 |

### 3.3 New internals

| Internal | File | What it is | Level |
|---|---|---|---|
| IdSet | ids.js | `DeleteSet` renamed and generalised: per-client sorted ranges with diff, merge, intersect; encoders `IdSetEncoderV1`/`V2`. The v1 delete set section of an update is an IdSet | Protocol level only where it replaces the delete set (unchanged bytes); the set algebra is library level |
| IdMap | ids.js | Ranges of IDs mapped to attribute lists (`ContentAttribute`), de-duplicated attributes, run-length encoded. Encoded with `IdSetEncoderV2` only | Library level. Not transported by y-protocols; appears only in the byte output of `encodeIdMap`, `encodeContentMap` |
| BlockSet | BlockSet.js | Decoded update container (`readBlockSet` is the former `readClientsStructRefs`); `exclude()` drops already known ranges before integration; `mergeUpdates` is reimplemented on it | Library level |
| Snapshot | Snapshot.js | State vector plus delete IdSet, as in 13.x; `encodeSnapshot` writes the IdSet then the state vector with V1 encoders, same layout as 13.x `encodeSnapshot` | Library level; wire-compatible with 13.x snapshots |
| Attribution | ids.js, Renderer.js, attributing-content.md | IdMaps of attributed inserts and deletes applied at render time (`toDelta({ renderer })`, `TwosetRenderer`), relative positions take a renderer | Library level; nothing on the wire |
| meta.js | meta.js | `ContentIds` (insert and delete IdSets) and `ContentMap` (insert and delete IdMaps) with diff, merge, intersect, filter and V2 encoders; `createContentIdsFromDocDiff`, `undoContentIds` | Library level; separate byte APIs |
| schemas.js | schemas.js | lib0 runtime schema for `Doc` | Library level |

### 3.4 What a v14 peer sends that a 13.x-compatible peer cannot read

Nothing on the sync path. Updates, state vectors and sync messages are v1
and byte-identical in layout. Two things a 13.x peer sees more often:

| Item | 13.x handling | y-cpp handling |
|---|---|---|
| `Skip` structs in an ordinary state update | Decoded, following structs parked as pending | Same (`Doc::applyUpdate`, `Doc::integrateLoop`) |
| 53-bit client IDs | Decoded (JavaScript number) | `uint64_t` throughout |

Readable but interpreted differently: a nested type created by a v14 peer
(a `YNode` placed in a map or array) is encoded with type ref 4 or 3. A
13.x peer decodes it as `Y.XmlFragment` or `Y.XmlElement`, not as the
`Y.Map`, `Y.Array` or `Y.Text` the application expects; y-cpp decodes it
as an XML kind (preserved on the wire, no API). Root types are not
affected (their kind is chosen by the reader). Mixed 13.x and 14 peers
therefore interoperate on root types and on nested types created by 13.x
peers, not on nested types created by v14 peers. The optional v14 interop
job skips the nested-type case for this reason.

Not readable by a 13.x peer, and not sent through y-protocols: encoded
IdMaps, ContentMaps and ContentIds (V2 only), used by the versioning,
attribution and `undoContentIds` APIs.

### 3.5 Cost of snapshots and attribution for the port

| Feature | Needed pieces | Present in y-cpp | Estimate |
|---|---|---|---|
| Snapshot (take, encode, decode, `createDocFromSnapshot`) | state vector codec, delete set codec, a rebuild that keeps items with `clock < sv[client]` and marks deleted by the delete set; requires retained tombstone content (`gc: false`) | state vector and delete set codecs exist; deleted string content is dropped at cleanup (`ContentDeleted`), so a rebuilt snapshot would lack the text of deleted ranges unless a no-GC mode is added | small codec work, moderate for the no-GC mode |
| Attribution | IdSet algebra (diff, merge, intersect), IdMap with attribute ranges, V2 IdSet encoder (run-length), renderer hooks in `toString`, `toDelta`, relative positions; `gc: false` as above | none of the IdSet algebra; no V2 codec at all | moderate to large, and intrusive in the type walkers |

Neither is needed for the file-based playlist and annotation sync.

## 4. y-octo since 2025-05

No issues were opened or closed in y-crdt/y-octo since 2025-05-01. Merged
pull requests touching the algorithm or the codec:

| PR | Date | Fix | Algorithm level | y-cpp |
|---|---|---|---|---|
| #53 | 2026-06-30 | `ListType::find_pos` skipped tombstones in a way that left `origin` and `rightOrigin` non-adjacent (a deleted item between them); the resulting item makes yjs and yrs diverge too | Yes: local insert must use the actual list neighbours, tombstones included | Not affected: `Doc::findListPosition` (src/types.cpp) advances `left` over every item, deleted or not, so origins are adjacent. Recommended: add the three-peer scenario from the PR as a convergence test in tests/test_main.cpp and interop.test.js |
| #55 | 2026-07-26 | Items whose parent type was released or whose parent item is deleted are turned into GC structs to a fixpoint; encoding a struct with a released parent is an error instead of writing nothing | Partly: the encoder must never emit a struct without parent information | Not applicable to GC: y-cpp keeps type tombstones and never emits GC structs. Encoder invariant already holds: `Doc::writeStruct` throws on an item without parent |
| #54 | 2026-07-20 | Fuzzing regressions: checked clock arithmetic, capacity caps on untrusted lengths, canonical delete set on encode (sorted, merged, empty clients dropped), zero-length structs dropped on encode, cyclic type nesting rejected | No: decoder hardening | Not verified. `Doc::applyUpdate` rolls back allocated items on a decode error; clock overflow checks are absent. Candidate for a later fuzz pass |
| #59 | 2026-07-29 | var-int decoder: negative overflow in `read_var_i32` | No: codec robustness | `lib0::Decoder::readVarInt` is 64-bit; overflow behaviour not verified |
| #48 | 2026-01-10 | `Array::remove` returned wrong results when the position landed on a tombstone; `find_pos` now skips leading tombstones for removal; batch API | No: local API bug | `Doc::listRemove` skips deleted items; not affected |
| #50 | 2026-01-11 | `get_node_index` binary search returned wrong indexes or panicked for clocks beyond the end; `repair` resolves an item's parent when the parent type item arrives in the same update | No: local bugs | Parent in the same update is covered by `Doc::depsMissing` and `Doc::integrateLoop` ordering |
| #57 | 2026-07-27 | Cached item length | No | none |
| #70 | 2026-09-22 | YText identity feature | No | none |

## 5. Actions

| Action | Where | Priority |
|---|---|---|
| Mirror #757 and fix the map lookup in the redo path | `UndoManager::redoItem`, src/undo.cpp; unit test `undo_map_key_then_parent_delete_syncs` | done 2026-10-28 |
| Move the pin to v13.6.33 | docs/MINIMAL_FEATURES.md section 4 | done 2026-10-28 |
| Pin the default interop job to the version the lock resolved (13.6.32) | tests/interop/package.json | done 2026-10-28 |
| Add the y-octo #53 scenario as a convergence test | `sync_insert_before_leading_tombstone` (tests/test_main.cpp), `insert before a leading tombstone converges` (interop.test.js, both role assignments) | done 2026-10-28 |
| Add a mid-stream `Skip` interop case (v14 store with gaps) | `update with a mid-stream Skip struct` (interop.test.js) | done 2026-10-28 |
| Interop version matrix | ctest jobs `interop-yjs13-latest` (default on) and `interop-yjs14` (`YCPP_INTEROP_V14_SPEC`); loader tests/interop/yjs-compat.js | done 2026-10-28 |
| Decoder hardening against malformed updates (clock overflow, length caps, cyclic parents), after y-octo #54 | src/update_codec.cpp, src/lib0.hpp | later, with a fuzz pass |
