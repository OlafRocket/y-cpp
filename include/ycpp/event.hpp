// Change notification vocabulary: Value, list deltas, map key changes,
// the Event delivered to observers, and the RAII Subscription.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "ycpp/any.hpp"

namespace ycpp {
class Doc;
struct YType;
class TypeRef;
// The result of reading one position/key: either a plain value or a nested
// shared type (inspect with the wrapper classes below).
struct Value {
  Any any;
  YType* type = nullptr;
  bool isType() const { return type != nullptr; }
};

// One operation of a list-type change description (Quill-style delta).
struct DeltaOp {
  enum class Kind { Retain, Insert, Delete };
  Kind kind = Kind::Retain;
  uint64_t length = 0;        // retain/delete count; also insert length
  std::u16string str;         // inserted text (from ContentString items)
  std::vector<Value> values;  // inserted values (from other content)
};

// One changed key of a map-type change description.
struct MapChange {
  enum class Action { Add, Update, Delete };
  Action action = Action::Add;
  std::optional<Value> oldValue;  // value before the transaction (Update/Delete)
};

// Fired on observers at the end of the transaction that changed the type.
// Computed like yjs YEvent.changes/.keys: from the transaction's before-state
// and delete set, before any content is garbage-collected.
struct Event {
  YType* target = nullptr;
  std::string origin;  // transact()/applyUpdate() origin tag
  bool local = true;   // false when produced by applyUpdate
  std::vector<DeltaOp> delta;         // list changes (Text/Array)
  std::map<std::string, MapChange> keys;  // map key changes
};

using Observer = std::function<void(const Event&)>;

// RAII observer registration: detaches automatically when destroyed (like
// boost::signals2::scoped_connection). Movable, not copyable. Tracks the
// Doc's lifetime through a weak liveness token, so a Subscription that
// outlives its Doc detaches into a safe no-op instead of touching freed
// memory.
class Subscription {
 public:
  Subscription() = default;
  Subscription(const Subscription&) = delete;
  Subscription& operator=(const Subscription&) = delete;
  Subscription(Subscription&& o) noexcept { *this = std::move(o); }
  Subscription& operator=(Subscription&& o) noexcept {
    if (this != &o) {
      reset();
      type_ = o.type_;
      doc_ = o.doc_;
      handle_ = o.handle_;
      alive_ = std::move(o.alive_);
      o.type_ = nullptr;
      o.doc_ = nullptr;
      o.handle_ = 0;
      o.alive_.reset();
    }
    return *this;
  }
  ~Subscription() { reset(); }

  void reset();  // detach now (idempotent, safe after the Doc is gone)
  // True while attached and the Doc is still alive.
  bool active() const { return handle_ != 0 && !alive_.expired(); }

 private:
  friend class TypeRef;
  friend class Doc;
  YType* type_ = nullptr;  // set for type observers
  Doc* doc_ = nullptr;     // set for doc update handlers
  uint64_t handle_ = 0;
  std::weak_ptr<void> alive_;  // expires when the Doc is destroyed
};

}  // namespace ycpp
