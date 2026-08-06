// Port of y-protocols/awareness.js (see header for the model).
#include "ycpp/protocols/awareness.hpp"

#include <algorithm>

#include "ycpp/json.hpp"
#include "ycpp/lib0.hpp"

namespace ycpp::protocols {

namespace {

std::string stateToJson(const Any& state) {
  std::string out;
  json::dump(out, state);
  return out;
}

}  // namespace

Awareness::Awareness(uint64_t clientId) : clientId_(clientId) {
  // yjs starts every client with an empty visible state at clock 0, so the
  // first real broadcast carries clock >= 1 and passes receivers' LWW gate.
  setLocalState(Any::object({}));
}

std::optional<Any> Awareness::getLocalState() const {
  auto it = states_.find(clientId_);
  if (it == states_.end()) return std::nullopt;
  return it->second;
}

void Awareness::setLocalState(const std::optional<Any>& state) {
  auto mit = meta_.find(clientId_);
  uint64_t clock = mit == meta_.end() ? 0 : mit->second.clock + 1;
  auto prev = states_.find(clientId_);
  bool hadPrev = prev != states_.end();
  Any prevState = hadPrev ? prev->second : Any();
  if (!state) {
    states_.erase(clientId_);
  } else {
    states_[clientId_] = *state;
  }
  meta_[clientId_] = Meta{clock, std::chrono::steady_clock::now()};

  Change update, change;
  update.origin = change.origin = "local";
  if (!state) {
    update.removed.push_back(clientId_);
    change.removed.push_back(clientId_);
  } else if (!hadPrev) {
    update.added.push_back(clientId_);
    change.added.push_back(clientId_);
  } else {
    update.updated.push_back(clientId_);
    if (!(prevState == *state)) change.updated.push_back(clientId_);
  }
  emit(update, change);
}

void Awareness::setLocalStateField(const std::string& key, const Any& value) {
  Any::Dict dict;
  if (auto local = getLocalState(); local && local->kind() == Any::Kind::Dict) {
    dict = local->dict();
  }
  dict[key] = value;
  setLocalState(Any::object(std::move(dict)));
}

std::map<uint64_t, Any> Awareness::getStates() const { return states_; }

std::vector<uint8_t> Awareness::encodeUpdate(const std::vector<uint64_t>& clients) const {
  lib0::Encoder enc;
  enc.writeVarUint(clients.size());
  for (uint64_t client : clients) {
    auto mit = meta_.find(client);
    uint64_t clock = mit == meta_.end() ? 0 : mit->second.clock;
    auto sit = states_.find(client);
    enc.writeVarUint(client);
    enc.writeVarUint(clock);
    enc.writeVarString(sit == states_.end() ? "null" : stateToJson(sit->second));
  }
  return enc.take();
}

std::vector<uint8_t> Awareness::encodeAllStates() const {
  std::vector<uint64_t> clients;
  for (const auto& [client, state] : states_) clients.push_back(client);
  return encodeUpdate(clients);
}

void Awareness::applyUpdate(std::span<const uint8_t> update, const std::string& origin) {
  lib0::Decoder dec(update);
  auto now = std::chrono::steady_clock::now();
  Change upd, chg;
  upd.origin = chg.origin = origin;
  uint64_t len = dec.readVarUint();
  for (uint64_t i = 0; i < len; i++) {
    uint64_t client = dec.readVarUint();
    uint64_t clock = dec.readVarUint();
    Any state = json::parse(dec.readVarString());
    bool isNull = state.kind() == Any::Kind::Null || state.kind() == Any::Kind::Undefined;
    auto mit = meta_.find(client);
    bool hadMeta = mit != meta_.end();
    uint64_t currClock = hadMeta ? mit->second.clock : 0;
    auto sit = states_.find(client);
    bool hadState = sit != states_.end();
    Any prevState = hadState ? sit->second : Any();
    if (currClock < clock || (currClock == clock && isNull && hadState)) {
      if (isNull) {
        // Never let a remote client remove this local state: bump the clock
        // so the follow-up broadcast announces we are still here.
        if (client == clientId_ && getLocalState().has_value()) {
          clock++;
        } else {
          states_.erase(client);
        }
      } else {
        states_[client] = state;
      }
      meta_[client] = Meta{clock, now};
      if (!hadMeta && !isNull) {
        upd.added.push_back(client);
        chg.added.push_back(client);
      } else if (hadMeta && isNull) {
        upd.removed.push_back(client);
        chg.removed.push_back(client);
      } else if (!isNull) {
        upd.updated.push_back(client);
        if (!(state == prevState)) chg.updated.push_back(client);
      }
    }
  }
  emit(upd, chg);
}

void Awareness::removeStates(const std::vector<uint64_t>& clients, const std::string& origin) {
  Change upd, chg;
  upd.origin = chg.origin = origin;
  for (uint64_t client : clients) {
    if (states_.erase(client) > 0) {
      if (client == clientId_) {
        auto& m = meta_[client];
        m.clock++;
        m.lastUpdated = std::chrono::steady_clock::now();
      }
      upd.removed.push_back(client);
      chg.removed.push_back(client);
    }
  }
  emit(upd, chg);
}

std::vector<uint64_t> Awareness::removeOutdated(uint64_t timeoutMs) {
  auto now = std::chrono::steady_clock::now();
  std::vector<uint64_t> outdated;
  for (const auto& [client, meta] : meta_) {
    if (client == clientId_) continue;
    auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - meta.lastUpdated);
    if (states_.count(client) > 0 && age.count() >= static_cast<int64_t>(timeoutMs)) {
      outdated.push_back(client);
    }
  }
  if (!outdated.empty()) removeStates(outdated, "timeout");
  return outdated;
}

uint64_t Awareness::onUpdate(Handler fn) {
  uint64_t handle = nextHandle_++;
  updateHandlers_.emplace_back(handle, std::move(fn));
  return handle;
}

uint64_t Awareness::onChange(Handler fn) {
  uint64_t handle = nextHandle_++;
  changeHandlers_.emplace_back(handle, std::move(fn));
  return handle;
}

void Awareness::off(uint64_t handle) {
  auto drop = [&](auto& v) {
    v.erase(std::remove_if(v.begin(), v.end(),
                           [&](const auto& p) { return p.first == handle; }),
            v.end());
  };
  drop(updateHandlers_);
  drop(changeHandlers_);
}

void Awareness::emit(const Change& update, const Change& change) {
  if (!change.added.empty() || !change.updated.empty() || !change.removed.empty()) {
    auto handlers = changeHandlers_;
    for (const auto& [h, fn] : handlers) fn(change);
  }
  if (!update.added.empty() || !update.updated.empty() || !update.removed.empty()) {
    auto handlers = updateHandlers_;
    for (const auto& [h, fn] : handlers) fn(update);
  }
}

}  // namespace ycpp::protocols
