#!/usr/bin/env bash
# Prints what changed upstream in the yjs files that y-cpp mirrors.
#
# Usage: tools/upstream-diff.sh [TARGET]
#   TARGET  a tag or ref of the yjs clone (default: HEAD)
# Environment:
#   YJS_CLONE          path of the yjs clone (default /mnt/work/crdt/yjs)
#   YCPP_UPSTREAM_PIN  the pinned tag (default: "Pinned yjs tag:" line in
#                      docs/MINIMAL_FEATURES.md)
#
# Output: a --stat summary of the core file list between the pin and TARGET,
# then the full diff. Files that only exist in yjs 14 (successors of the 13.x
# files) are included so a 14.x target shows them as added.
# y-protocols (sync.js, awareness.js) lives in its own package and is not
# covered; compare it against the version pinned in tests/interop/package.json.
# Needs git and bash only. Refresh the clone's tags with
#   git -C "$YJS_CLONE" fetch --tags
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CLONE="${YJS_CLONE:-/mnt/work/crdt/yjs}"
PIN="${YCPP_UPSTREAM_PIN:-$(sed -n 's/^Pinned yjs tag: \(v[0-9][0-9.]*\).*/\1/p' "$ROOT/docs/MINIMAL_FEATURES.md" | head -1)}"
PIN="${PIN:-v13.6.33}"
TARGET="${1:-HEAD}"

CORE=(
  src/structs/Item.js
  src/structs/ContentAny.js
  src/structs/ContentBinary.js
  src/structs/ContentDeleted.js
  src/structs/ContentDoc.js
  src/structs/ContentEmbed.js
  src/structs/ContentFormat.js
  src/structs/ContentJSON.js
  src/structs/ContentString.js
  src/structs/ContentType.js
  src/structs/GC.js
  src/structs/Skip.js
  src/utils/StructStore.js
  src/utils/Transaction.js
  src/utils/DeleteSet.js
  src/utils/UpdateEncoder.js
  src/utils/UpdateDecoder.js
  src/utils/encoding.js
  src/utils/updates.js
  src/utils/UndoManager.js
  src/utils/RelativePosition.js
  src/utils/ID.js
  src/types/YText.js
  src/types/YArray.js
  src/types/YMap.js
  src/types/AbstractType.js
  # yjs 14 successors of the files above
  src/ynode.js
  src/structs/AbstractStruct.js
  src/utils/ids.js
  src/utils/BlockSet.js
  src/utils/encoding-helpers.js
  src/utils/transaction-helpers.js
)

for ref in "$PIN" "$TARGET"; do
  if ! git -C "$CLONE" rev-parse --verify -q "$ref^{commit}" >/dev/null; then
    echo "error: $ref not found in $CLONE (try: git -C $CLONE fetch --tags)" >&2
    exit 2
  fi
done

echo "# yjs core files: $PIN -> $TARGET ($(git -C "$CLONE" rev-parse --short "$TARGET"), $(git -C "$CLONE" log -1 --format=%cs "$TARGET"))"
echo
if git -C "$CLONE" diff --quiet "$PIN" "$TARGET" -- "${CORE[@]}"; then
  echo "no changes in the core file list"
  exit 0
fi
git -C "$CLONE" diff --stat "$PIN" "$TARGET" -- "${CORE[@]}"
echo
git -C "$CLONE" diff "$PIN" "$TARGET" -- "${CORE[@]}"
