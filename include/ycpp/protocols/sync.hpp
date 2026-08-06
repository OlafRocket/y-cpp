// Port of y-protocols/sync: the standard Yjs sync handshake, transport-
// agnostic (bytes in, bytes out). Mirrors the npm package's API:
//
//   client                         server/peer
//   ------ SyncStep1(stateVector) ------>
//   <----- SyncStep2(missing diff) ------
//   <----- SyncStep1(stateVector) ------   (peers both initiate)
//   ------ SyncStep2(missing diff) ----->
//   ~~~~~~ Update(incremental)  ~~~~~~~~   (both directions, ongoing)
//
// A message is [messageType : varUint, varUint8Array payload]. Room naming /
// envelope framing is the upper layer's concern (see y-websocket framing in
// tools/ycpp_sync.cpp).
#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>

#include "ycpp/doc.hpp"
#include "ycpp/lib0.hpp"

namespace ycpp::protocols::sync {

constexpr uint32_t messageYjsSyncStep1 = 0;
constexpr uint32_t messageYjsSyncStep2 = 1;
constexpr uint32_t messageYjsUpdate = 2;

// "Here is my state vector" — the opening message of a sync.
inline void writeSyncStep1(lib0::Encoder& enc, const Doc& doc) {
  enc.writeVarUint(messageYjsSyncStep1);
  enc.writeVarUint8Array(doc.encodeStateVector());
}

// "Here is everything you are missing" — reply to SyncStep1.
inline void writeSyncStep2(lib0::Encoder& enc, const Doc& doc,
                           std::span<const uint8_t> remoteStateVector) {
  enc.writeVarUint(messageYjsSyncStep2);
  enc.writeVarUint8Array(doc.encodeStateAsUpdate(remoteStateVector));
}

inline void writeSyncStep2(lib0::Encoder& enc, const Doc& doc) {
  enc.writeVarUint(messageYjsSyncStep2);
  enc.writeVarUint8Array(doc.encodeStateAsUpdate());
}

// An incremental update (typically from Doc::onUpdate).
inline void writeUpdate(lib0::Encoder& enc, std::span<const uint8_t> update) {
  enc.writeVarUint(messageYjsUpdate);
  enc.writeVarUint8Array(update);
}

// Dispatches one incoming sync message. A SyncStep1 writes the SyncStep2
// reply into `reply` (send it if non-empty); SyncStep2/Update are applied to
// the doc with the given transaction origin. Returns the message type.
inline uint32_t readSyncMessage(lib0::Decoder& dec, lib0::Encoder& reply, Doc& doc,
                                const std::string& origin) {
  uint64_t messageType = dec.readVarUint();
  switch (messageType) {
    case messageYjsSyncStep1: {
      auto sv = dec.readVarUint8Array();
      writeSyncStep2(reply, doc, sv);
      break;
    }
    case messageYjsSyncStep2:
    case messageYjsUpdate: {
      auto update = dec.readVarUint8Array();
      doc.applyUpdate(update, origin);
      break;
    }
    default:
      throw std::runtime_error("y-sync: unknown message type " + std::to_string(messageType));
  }
  return static_cast<uint32_t>(messageType);
}

}  // namespace ycpp::protocols::sync
