// Port of y-protocols/awareness: ephemeral per-client presence state
// (cursors, names, a driven timebar, ...) with last-write-wins semantics.
//
// Each client owns one JSON-ish state (an Any, usually a Dict) guarded by a
// per-client clock; updates replace the whole state, nothing is merged or
// persisted, and states expire when not renewed. The wire format is
// [numClients, { clientID, clock, JSON-string state }*] — byte-compatible
// with the y-protocols npm package.
//
// Unlike the JS package the library runs no timers: the host calls
// removeOutdated() periodically (y-protocols uses a 30s timeout) and renews
// the local state (setLocalState(getLocalState())) about every timeout/2 to
// stay alive. tools/ycpp_sync.cpp shows the pattern.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ycpp/any.hpp"

namespace ycpp::protocols {

class Awareness {
 public:
  struct Change {
    std::vector<uint64_t> added;
    std::vector<uint64_t> updated;  // for onUpdate: every processed entry;
                                    // for onChange: only actual state changes
    std::vector<uint64_t> removed;
    std::string origin;             // "local" for local mutations
  };
  using Handler = std::function<void(const Change&)>;

  explicit Awareness(uint64_t clientId);

  [[nodiscard]] uint64_t clientId() const { return clientId_; }

  // Local state. nullopt removes it (an explicit "I'm leaving").
  void setLocalState(const std::optional<Any>& state);
  void setLocalStateField(const std::string& key, const Any& value);
  [[nodiscard]] std::optional<Any> getLocalState() const;

  // All live states (clients whose state is currently set), including local.
  [[nodiscard]] std::map<uint64_t, Any> getStates() const;

  // Wire codec (y-protocols byte-compatible).
  [[nodiscard]] std::vector<uint8_t> encodeUpdate(const std::vector<uint64_t>& clients) const;
  [[nodiscard]] std::vector<uint8_t> encodeAllStates() const;  // every known client
  void applyUpdate(std::span<const uint8_t> update, const std::string& origin);

  // Explicit removal (e.g. a server dropping a disconnected peer's clients).
  void removeStates(const std::vector<uint64_t>& clients, const std::string& origin);

  // Drops states not renewed within timeoutMs (default: y-protocols' 30s);
  // never drops the local state. Returns the removed client ids. Call
  // periodically from the host's loop.
  std::vector<uint64_t> removeOutdated(uint64_t timeoutMs = 30000);

  // "update" fires for every processed entry (use it to re-broadcast, even
  // when the state content did not change — it keeps clients alive);
  // "change" fires only when state content actually changed (drive UI).
  uint64_t onUpdate(Handler fn);
  uint64_t onChange(Handler fn);
  void off(uint64_t handle);

 private:
  struct Meta {
    uint64_t clock = 0;
    std::chrono::steady_clock::time_point lastUpdated;
  };
  void emit(const Change& update, const Change& change);

  uint64_t clientId_;
  std::map<uint64_t, Any> states_;
  std::map<uint64_t, Meta> meta_;
  std::vector<std::pair<uint64_t, Handler>> updateHandlers_;
  std::vector<std::pair<uint64_t, Handler>> changeHandlers_;
  uint64_t nextHandle_ = 1;
};

}  // namespace ycpp::protocols
