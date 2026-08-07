// y-cpp: minimal C++ implementation of the Yjs CRDT protocol (v1 encoding),
// supporting collaborative plain text (Y.Text), sequences (Y.Array), and
// key-value maps (Y.Map) on root and nested types.
//
// Ported from the reference implementation yjs v13 (https://github.com/yjs/yjs);
// struct/field names intentionally mirror the yjs sources so the two can be
// read side by side.
//
// This umbrella header pulls in the whole public API; the per-concern
// headers below can also be included individually.
#pragma once

#include "ycpp/lib0.hpp"      // lib0 binary primitives (varints, strings)
#include "ycpp/utf.hpp"       // UTF-8 <-> UTF-16 with JS semantics
#include "ycpp/any.hpp"       // the lib0 "Any" value model + binary codec
#include "ycpp/json.hpp"      // JSON text output for Any values
#include "ycpp/item.hpp"      // Item: the CRDT content atom
#include "ycpp/event.hpp"     // observer events + Subscription
#include "ycpp/type.hpp"      // YType: the shared type node
#include "ycpp/wrappers.hpp"  // Text / Array / Map public handles
#include "ycpp/doc.hpp"       // Doc: store, transactions, update codec
#include "ycpp/position.hpp"  // relative positions (content-anchored cursors)
#include "ycpp/undo.hpp"      // UndoManager: selective collaborative undo/redo
