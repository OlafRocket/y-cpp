// y-cpp test suite: unit tests + CRDT convergence tests.
#include <cstdio>
#include <functional>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "../tools/base64_io.hpp"
#include "ycpp/lib0.hpp"
#include "ycpp/utf.hpp"
#include "ycpp/ycpp.hpp"

namespace {

int failures = 0;
int checks = 0;
std::string currentTest;

void check(bool cond, const std::string& msg) {
  checks++;
  if (!cond) {
    failures++;
    std::cerr << "FAIL [" << currentTest << "] " << msg << "\n";
  }
}

template <typename A, typename B>
void checkEq(const A& a, const B& b, const std::string& msg) {
  checks++;
  if (!(a == b)) {
    failures++;
    std::cerr << "FAIL [" << currentTest << "] " << msg << "\n";
  }
}

struct Test {
  std::string name;
  std::function<void()> fn;
};
std::vector<Test>& tests() {
  static std::vector<Test> t;
  return t;
}
struct Reg {
  Reg(const std::string& name, std::function<void()> fn) { tests().push_back({name, fn}); }
};
#define TEST(name)                                    \
  void test_##name();                                 \
  Reg reg_##name(#name, test_##name);                 \
  void test_##name()

// Sync two docs both ways using state-vector diffs.
void sync(ycpp::Doc& a, ycpp::Doc& b) {
  b.applyUpdate(a.encodeStateAsUpdate(b.encodeStateVector()));
  a.applyUpdate(b.encodeStateAsUpdate(a.encodeStateVector()));
}

// ----------------------------------------------------------------- lib0

TEST(lib0_varuint_roundtrip) {
  std::vector<uint64_t> values = {0, 1, 42, 127, 128, 255, 300, 16383, 16384,
                                  1u << 20, 0xffffffffULL, 1ULL << 53};
  for (uint64_t v : values) {
    ycpp::lib0::Encoder enc;
    enc.writeVarUint(v);
    ycpp::lib0::Decoder dec(enc.bytes());
    checkEq(dec.readVarUint(), v, "varUint roundtrip " + std::to_string(v));
  }
}

TEST(lib0_varuint_wire_format) {
  // 128 encodes as [0x80, 0x01] in lib0.
  ycpp::lib0::Encoder enc;
  enc.writeVarUint(128);
  checkEq(enc.bytes().size(), size_t(2), "128 is 2 bytes");
  checkEq(enc.bytes()[0], uint8_t(0x80), "first byte");
  checkEq(enc.bytes()[1], uint8_t(0x01), "second byte");
}

TEST(lib0_varstring) {
  ycpp::lib0::Encoder enc;
  enc.writeVarString("h\xC3\xA9llo");  // "héllo"
  ycpp::lib0::Decoder dec(enc.bytes());
  checkEq(dec.readVarString(), std::string("h\xC3\xA9llo"), "varString roundtrip");
}

// ----------------------------------------------------------------- utf

TEST(utf_roundtrip) {
  std::string s = "hello \xC3\xA9 \xE4\xB8\xAD\xE6\x96\x87 \xF0\x9F\x98\x80";  // é 中文 😀
  checkEq(ycpp::utf::utf16To8(ycpp::utf::utf8To16(s)), s, "utf8->16->8 roundtrip");
  std::u16string u = ycpp::utf::utf8To16(s);
  checkEq(u.size(), size_t(13), "emoji counts as 2 UTF-16 units");
}

TEST(base64_roundtrip) {
  std::vector<uint8_t> data = {0, 1, 2, 250, 255, 100, 42};
  for (size_t n = 0; n <= data.size(); n++) {
    std::vector<uint8_t> in(data.begin(), data.begin() + n);
    checkEq(ycpp_tools::base64Decode(ycpp_tools::base64Encode(in)), in,
            "base64 roundtrip len " + std::to_string(n));
  }
  // Whitespace from stdin/copy-paste is tolerated.
  std::vector<uint8_t> hey = {'h', 'e', 'y'};
  checkEq(ycpp_tools::base64Decode("aGV5\n"), hey, "trailing newline tolerated");
  checkEq(ycpp_tools::base64Decode(" aG\r\nV5 "), hey, "embedded whitespace tolerated");
  // Invalid input reports an error instead of decoding garbage.
  bool threw = false;
  try {
    ycpp_tools::base64Decode("not*base64!");
  } catch (const std::exception&) {
    threw = true;
  }
  check(threw, "invalid character throws");
}

// ----------------------------------------------------------------- local text

TEST(text_insert_delete_local) {
  ycpp::Doc d(1);
  auto t = d.getText();
  t.insert(0, "hello world");
  t.insert(5, ",");
  checkEq(t.toString(), std::string("hello, world"), "insert middle");
  t.remove(5, 1);
  checkEq(t.toString(), std::string("hello world"), "delete middle");
  t.insert(11, "!");
  checkEq(t.toString(), std::string("hello world!"), "append");
  t.remove(0, 6);
  checkEq(t.toString(), std::string("world!"), "delete prefix");
  checkEq(t.length(), uint64_t(6), "length tracking");
}

TEST(text_unicode_lengths) {
  ycpp::Doc d(1);
  auto t = d.getText();
  t.insert(0, "a\xF0\x9F\x98\x80z");  // a😀z — 4 UTF-16 units
  checkEq(t.length(), uint64_t(4), "UTF-16 length");
  t.remove(1, 2);  // delete the emoji (2 units)
  checkEq(t.toString(), std::string("az"), "delete emoji");
}

TEST(text_update_roundtrip) {
  ycpp::Doc a(1);
  a.getText().insert(0, "persistent text");
  a.getText().remove(0, 3);
  ycpp::Doc b(2);
  b.applyUpdate(a.encodeStateAsUpdate());
  checkEq(b.getText().toString(), a.getText().toString(), "state update roundtrip");
  // Re-encode from the copy and load again (checks re-encoding of tombstones).
  ycpp::Doc c(3);
  c.applyUpdate(b.encodeStateAsUpdate());
  checkEq(c.getText().toString(), a.getText().toString(), "second-generation roundtrip");
}

// ----------------------------------------------------------------- sync

TEST(sync_concurrent_inserts_converge) {
  ycpp::Doc a(1), b(2);
  a.getText().insert(0, "base");
  sync(a, b);
  a.getText().insert(4, " from-a");
  b.getText().insert(4, " from-b");
  sync(a, b);
  checkEq(a.getText().toString(), b.getText().toString(), "converged");
  // Lower client id wins the tiebreak at the same position.
  checkEq(a.getText().toString(), std::string("base from-a from-b"), "deterministic order");
}

TEST(sync_concurrent_same_position_interleave) {
  ycpp::Doc a(10), b(20), c(30);
  a.getText().insert(0, "||");
  sync(a, b);
  sync(a, c);
  a.getText().insert(1, "aaa");
  b.getText().insert(1, "bbb");
  c.getText().insert(1, "ccc");
  sync(a, b);
  sync(b, c);
  sync(a, b);
  sync(a, c);
  checkEq(a.getText().toString(), b.getText().toString(), "a==b");
  checkEq(b.getText().toString(), c.getText().toString(), "b==c");
  checkEq(a.getText().toString(), std::string("|aaabbbccc|"), "YATA order by client id");
}

TEST(sync_delete_remote) {
  ycpp::Doc a(1), b(2);
  a.getText().insert(0, "abcdef");
  sync(a, b);
  b.getText().remove(2, 2);  // delete "cd"
  a.getText().insert(6, "!");
  sync(a, b);
  checkEq(a.getText().toString(), std::string("abef!"), "delete synced to a");
  checkEq(b.getText().toString(), std::string("abef!"), "insert synced to b");
}

TEST(sync_insert_into_remote_item) {
  // Splitting a remote item on integration.
  ycpp::Doc a(1), b(2);
  a.getText().insert(0, "0123456789");
  sync(a, b);
  b.getText().insert(5, "X");
  sync(a, b);
  checkEq(a.getText().toString(), std::string("01234X56789"), "split remote item");
}

TEST(sync_concurrent_delete_same_range) {
  ycpp::Doc a(1), b(2);
  a.getText().insert(0, "abcdef");
  sync(a, b);
  a.getText().remove(1, 3);
  b.getText().remove(2, 3);  // overlapping delete
  sync(a, b);
  checkEq(a.getText().toString(), b.getText().toString(), "converged");
  checkEq(a.getText().toString(), std::string("af"), "union of deletes");
}

TEST(sync_out_of_order_updates) {
  // Updates may arrive out of order: later structs must wait as pending.
  ycpp::Doc a(1), b(2);
  a.getText().insert(0, "one");
  auto sv1 = a.encodeStateVector();
  auto u1 = a.encodeStateAsUpdate();
  a.getText().insert(3, " two");
  auto u2 = a.encodeStateAsUpdate(sv1);  // diff containing only " two"
  b.applyUpdate(u2);                     // arrives first: must go pending
  check(b.hasPending(), "diff without base is pending");
  checkEq(b.getText().toString(), std::string(""), "nothing visible yet");
  b.applyUpdate(u1);
  check(!b.hasPending(), "pending resolved");
  checkEq(b.getText().toString(), std::string("one two"), "both applied after base arrives");
}

TEST(sync_pending_delete) {
  ycpp::Doc a(1), b(2);
  a.getText().insert(0, "hello");
  auto sv1 = a.encodeStateVector();
  auto u1 = a.encodeStateAsUpdate();
  a.getText().remove(0, 2);
  auto u2 = a.encodeStateAsUpdate(sv1);  // delete of structs b doesn't have yet
  b.applyUpdate(u2);
  b.applyUpdate(u1);
  checkEq(b.getText().toString(), std::string("llo"), "pending delete applied");
}

TEST(sync_pending_survives_reencode) {
  // Structs and deletes that are still pending must not be lost when a doc's
  // state is re-encoded (yjs merges pendingStructs/pendingDs the same way).
  ycpp::Doc a(1);
  a.getText().insert(0, "one");
  auto sv1 = a.encodeStateVector();
  auto u1 = a.encodeStateAsUpdate();
  a.getText().insert(3, " two");
  a.getText().remove(0, 1);
  auto u2 = a.encodeStateAsUpdate(sv1);
  ycpp::Doc b(2);
  b.applyUpdate(u2);  // arrives before its base: everything pending
  check(b.hasPending(), "u2 alone is pending");
  ycpp::Doc c(3);
  c.applyUpdate(b.encodeStateAsUpdate());  // must carry the pending data along
  c.applyUpdate(u1);
  checkEq(c.getText().toString(), a.getText().toString(), "pending survived re-encode");
}

TEST(sync_empty_docs) {
  ycpp::Doc a(1), b(2);
  sync(a, b);
  checkEq(a.getText().toString(), std::string(""), "empty stays empty");
  auto u = a.encodeStateAsUpdate();
  b.applyUpdate(u);
}

TEST(sync_surrogate_split) {
  // Concurrent insert into the middle of a surrogate pair: both sides must
  // converge (to U+FFFD replacements, like yjs).
  ycpp::Doc a(1), b(2);
  a.getText().insert(0, "\xF0\x9F\x98\x80");  // 😀 = 2 UTF-16 units
  sync(a, b);
  b.getText().insertUtf16(1, u"X");  // split the pair
  sync(a, b);
  checkEq(a.getText().toString(), b.getText().toString(), "converged after surrogate split");
  checkEq(a.getText().length(), uint64_t(3), "3 units");
}

// ----------------------------------------------------------------- any codec

TEST(any_codec_roundtrip) {
  using ycpp::Any;
  std::vector<Any> values = {
      Any(), Any::null(), Any::of(true), Any::of(false),
      Any::of(int64_t{0}), Any::of(int64_t{-1}), Any::of(int64_t{123456}),
      Any::of(int64_t{0x7fffffff}), Any::of(int64_t{1} << 40),  // > 31 bits -> float64
      Any::of(3.5), Any::of(-0.25), Any::of("héllo"),
      Any::binary({1, 2, 255}),
      Any::array({Any::of(int64_t{1}), Any::of("x")}),
      Any::object({{"a", Any::of(int64_t{1})}, {"b", Any::array({Any::null()})}}),
  };
  for (const Any& v : values) {
    ycpp::lib0::Encoder enc;
    ycpp::anycodec::write(enc, v);
    ycpp::lib0::Decoder dec(enc.bytes());
    ycpp::Any back = ycpp::anycodec::read(dec);
    if (v.kind == Any::Kind::Int && (v.integer > 0x7fffffff || v.integer < -0x7fffffff)) {
      checkEq(back.number, static_cast<double>(v.integer), "large int as float64");
    } else {
      check(back == v, "any roundtrip");
    }
  }
}

TEST(lib0_varint_signed) {
  for (int64_t v : {int64_t{0}, int64_t{1}, int64_t{-1}, int64_t{63}, int64_t{64},
                    int64_t{-64}, int64_t{-65}, int64_t{1} << 30, -(int64_t{1} << 30)}) {
    ycpp::lib0::Encoder enc;
    enc.writeVarInt(v);
    ycpp::lib0::Decoder dec(enc.bytes());
    checkEq(dec.readVarInt(), v, "varInt roundtrip " + std::to_string(v));
  }
}

// ----------------------------------------------------------------- array

TEST(array_local_ops) {
  ycpp::Doc d(1);
  auto arr = d.getArray("a");
  arr.insert(0, {ycpp::Any::of(int64_t{1}), ycpp::Any::of("two"), ycpp::Any::of(3.5)});
  arr.push(ycpp::Any::of(true));
  checkEq(arr.length(), uint64_t(4), "length");
  checkEq(arr.get(1).any.str, std::string("two"), "get middle");
  check(arr.get(3).any.boolean, "get pushed");
  arr.remove(1, 2);
  checkEq(arr.length(), uint64_t(2), "length after remove");
  checkEq(arr.toJson(), std::string("[1,true]"), "json");
  arr.insert(1, {ycpp::Any::of("mid")});
  checkEq(arr.toJson(), std::string("[1,\"mid\",true]"), "insert middle");
}

TEST(array_sync_and_concurrent) {
  ycpp::Doc a(1), b(2);
  a.getArray("a").push(ycpp::Any::of(int64_t{1}));
  sync(a, b);
  a.getArray("a").push(ycpp::Any::of("from-a"));
  b.getArray("a").push(ycpp::Any::of("from-b"));
  sync(a, b);
  checkEq(a.getArray("a").toJson(), b.getArray("a").toJson(), "converged");
  checkEq(a.getArray("a").toJson(), std::string("[1,\"from-a\",\"from-b\"]"),
          "lower client id first");
}

// ----------------------------------------------------------------- map

TEST(map_local_ops) {
  ycpp::Doc d(1);
  auto m = d.getMap("m");
  m.set("a", ycpp::Any::of(int64_t{1}));
  m.set("b", ycpp::Any::of("x"));
  checkEq(m.size(), size_t(2), "size");
  check(m.has("a") && m.has("b") && !m.has("c"), "has");
  checkEq(m.get("b")->any.str, std::string("x"), "get");
  m.set("a", ycpp::Any::of(int64_t{2}));  // overwrite
  checkEq(m.get("a")->any.integer, int64_t{2}, "overwrite");
  m.remove("b");
  check(!m.has("b"), "removed");
  checkEq(m.toJson(), std::string("{\"a\":2}"), "json");
}

TEST(map_concurrent_set_same_key) {
  // Concurrent sets of the same key: higher client id wins, both converge.
  ycpp::Doc a(1), b(2);
  a.getMap("m").set("k", ycpp::Any::of("init"));
  sync(a, b);
  a.getMap("m").set("k", ycpp::Any::of("from-a"));
  b.getMap("m").set("k", ycpp::Any::of("from-b"));
  sync(a, b);
  checkEq(a.getMap("m").toJson(), b.getMap("m").toJson(), "converged");
  checkEq(a.getMap("m").get("k")->any.str, std::string("from-b"), "higher client wins");
}

TEST(map_update_roundtrip) {
  ycpp::Doc a(1);
  auto m = a.getMap("m");
  m.set("x", ycpp::Any::of(int64_t{7}));
  m.set("gone", ycpp::Any::of("bye"));
  m.remove("gone");
  ycpp::Doc b(2);
  b.applyUpdate(a.encodeStateAsUpdate());
  checkEq(b.getMap("m").toJson(), a.getMap("m").toJson(), "map roundtrip");
  check(!b.getMap("m").has("gone"), "tombstoned key stays gone");
}

// ----------------------------------------------------------------- nesting

TEST(nested_types_sync) {
  ycpp::Doc a(1), b(2);
  auto m = a.getMap("root");
  auto sub = m.setMap("sub");
  sub.set("deep", ycpp::Any::of(int64_t{-5}));
  auto items = sub.setArray("items");
  items.push(ycpp::Any::of("a"));
  auto note = m.setText("note");
  note.insert(0, "nested text");
  sync(a, b);
  checkEq(b.getMap("root").toJson(), a.getMap("root").toJson(), "nested roundtrip");
  // Edit the nested types on b, sync back.
  auto bsub = b.getMap("root").get("sub")->type;
  ycpp::Map(bsub).set("added", ycpp::Any::of(true));
  auto bnote = b.getMap("root").get("note")->type;
  ycpp::Text(bnote).insert(0, "more ");
  sync(a, b);
  checkEq(a.getMap("root").toJson(), b.getMap("root").toJson(), "nested edits converge");
  check(a.getMap("root").toJson().find("more nested text") != std::string::npos,
        "nested text edit visible");
}

TEST(nested_delete_cascade) {
  ycpp::Doc a(1), b(2);
  auto m = a.getMap("root");
  auto sub = m.setMap("sub");
  sub.set("x", ycpp::Any::of(int64_t{1}));
  auto arr = sub.setArray("arr");
  arr.push(ycpp::Any::of("payload"));
  sync(a, b);
  a.getMap("root").remove("sub");  // kills the whole subtree
  sync(a, b);
  check(!a.getMap("root").has("sub"), "subtree gone on a");
  check(!b.getMap("root").has("sub"), "subtree gone on b");
  checkEq(a.getMap("root").toJson(), std::string("{}"), "empty root");
  // Concurrent edit into a deleted subtree must not resurrect it.
  ycpp::Doc c(3);
  c.applyUpdate(a.encodeStateAsUpdate());
  checkEq(c.getMap("root").toJson(), std::string("{}"), "fresh load empty");
}

TEST(map_fuzz_converges) {
  std::mt19937 rng(777);
  const char* keys[] = {"a", "b", "c", "d"};
  for (int round = 0; round < 10; round++) {
    ycpp::Doc d0(100), d1(200), d2(300);
    ycpp::Doc* docs[3] = {&d0, &d1, &d2};
    for (int step = 0; step < 40; step++) {
      ycpp::Doc& d = *docs[rng() % 3];
      auto m = d.getMap("m");
      uint32_t action = rng() % 10;
      if (action < 6) {
        m.set(keys[rng() % 4], ycpp::Any::of(static_cast<int64_t>(rng() % 1000)));
      } else if (action < 8) {
        m.remove(keys[rng() % 4]);
      } else {
        sync(*docs[rng() % 3], *docs[rng() % 3]);
      }
    }
    for (int i = 0; i < 3; i++) {
      sync(d0, d1);
      sync(d1, d2);
      sync(d0, d2);
    }
    std::string j = d0.getMap("m").toJson();
    check(j == d1.getMap("m").toJson() && j == d2.getMap("m").toJson(),
          "map fuzz round " + std::to_string(round) + " converged");
  }
}

// ------------------------------------------------------ transactions/events

TEST(observer_text_delta) {
  ycpp::Doc d(1);
  auto t = d.getText();
  std::vector<ycpp::Event> events;
  t.observe([&](const ycpp::Event& e) { events.push_back(e); });
  t.insert(0, "hello");
  checkEq(events.size(), size_t(1), "one event per transaction");
  checkEq(events[0].delta.size(), size_t(1), "single insert op");
  check(events[0].delta[0].kind == ycpp::DeltaOp::Kind::Insert, "insert kind");
  checkEq(ycpp::utf::utf16To8(events[0].delta[0].str), std::string("hello"), "inserted text");
  check(events[0].local, "local event");

  events.clear();
  t.insert(2, "XX");
  checkEq(events[0].delta.size(), size_t(2), "retain + insert");
  check(events[0].delta[0].kind == ycpp::DeltaOp::Kind::Retain &&
            events[0].delta[0].length == 2,
        "retain 2");
  checkEq(ycpp::utf::utf16To8(events[0].delta[1].str), std::string("XX"), "insert XX");

  events.clear();
  t.remove(1, 3);
  checkEq(events[0].delta.size(), size_t(2), "retain + delete");
  check(events[0].delta[1].kind == ycpp::DeltaOp::Kind::Delete &&
            events[0].delta[1].length == 3,
        "delete 3");
}

TEST(observer_transact_batches) {
  ycpp::Doc d(1);
  auto t = d.getText();
  int fired = 0;
  std::vector<ycpp::DeltaOp> lastDelta;
  t.observe([&](const ycpp::Event& e) {
    fired++;
    lastDelta = e.delta;
  });
  d.transact([&] {
    t.insert(0, "abc");
    t.insert(3, "def");
    t.remove(0, 1);
  }, "batch-origin");
  checkEq(fired, 1, "one event for the whole transaction");
  // Net effect: "bcdef" inserted at 0.
  checkEq(lastDelta.size(), size_t(1), "coalesced ops");
  checkEq(ycpp::utf::utf16To8(lastDelta[0].str), std::string("bcdef"), "net insert");
}

TEST(observer_insert_and_delete_same_tx_invisible) {
  ycpp::Doc d(1);
  auto t = d.getText();
  t.insert(0, "base");
  int fired = 0;
  ycpp::Event last;
  t.observe([&](const ycpp::Event& e) { fired++; last = e; });
  d.transact([&] {
    t.insert(4, "temp");
    t.remove(4, 4);
  });
  checkEq(fired, 1, "event fires");
  check(last.delta.empty(), "add+delete within tx is invisible in the delta");
}

TEST(observer_array_delta) {
  ycpp::Doc d(1);
  auto arr = d.getArray("a");
  std::vector<ycpp::Event> events;
  arr.observe([&](const ycpp::Event& e) { events.push_back(e); });
  arr.insert(0, {ycpp::Any::of(int64_t{1}), ycpp::Any::of("two")});
  checkEq(events.size(), size_t(1), "insert event");
  checkEq(events[0].delta[0].values.size(), size_t(2), "two inserted values");
  checkEq(events[0].delta[0].values[1].any.str, std::string("two"), "value content");
  events.clear();
  arr.remove(0, 1);
  check(events[0].delta[0].kind == ycpp::DeltaOp::Kind::Delete, "delete op");
}

TEST(observer_map_keys) {
  ycpp::Doc d(1);
  auto m = d.getMap("m");
  std::vector<ycpp::Event> events;
  m.observe([&](const ycpp::Event& e) { events.push_back(e); });

  m.set("k", ycpp::Any::of(int64_t{1}));
  check(events.back().keys.at("k").action == ycpp::MapChange::Action::Add, "add action");
  check(!events.back().keys.at("k").oldValue.has_value(), "no old value on add");

  m.set("k", ycpp::Any::of(int64_t{2}));
  check(events.back().keys.at("k").action == ycpp::MapChange::Action::Update, "update action");
  checkEq(events.back().keys.at("k").oldValue->any.integer, int64_t{1}, "old value on update");

  m.remove("k");
  check(events.back().keys.at("k").action == ycpp::MapChange::Action::Delete, "delete action");
  checkEq(events.back().keys.at("k").oldValue->any.integer, int64_t{2}, "old value on delete");
  checkEq(events.size(), size_t(3), "three events");
}

TEST(observer_remote_and_origin) {
  ycpp::Doc a(1), b(2);
  auto tb = b.getText();
  std::vector<ycpp::Event> events;
  tb.observe([&](const ycpp::Event& e) { events.push_back(e); });
  a.getText().insert(0, "from a");
  b.applyUpdate(a.encodeStateAsUpdate(b.encodeStateVector()), "network");
  checkEq(events.size(), size_t(1), "remote change fires observer");
  check(!events[0].local, "remote event is not local");
  checkEq(events[0].origin, std::string("network"), "origin passed through");
  checkEq(ycpp::utf::utf16To8(events[0].delta[0].str), std::string("from a"), "remote delta");
}

TEST(observer_unobserve) {
  ycpp::Doc d(1);
  auto t = d.getText();
  int fired = 0;
  uint64_t h = t.observe([&](const ycpp::Event&) { fired++; });
  t.insert(0, "x");
  t.unobserve(h);
  t.insert(0, "y");
  checkEq(fired, 1, "no events after unobserve");
}

TEST(subscription_raii) {
  ycpp::Doc d(1);
  auto t = d.getText();
  int fired = 0;
  {
    ycpp::Subscription sub = t.subscribe([&](const ycpp::Event&) { fired++; });
    check(sub.active(), "subscription active");
    t.insert(0, "a");
    checkEq(fired, 1, "fires while in scope");
  }
  t.insert(0, "b");
  checkEq(fired, 1, "auto-detached at scope exit");

  // Move semantics: only the final owner detaches.
  ycpp::Subscription outer;
  {
    ycpp::Subscription inner = t.subscribe([&](const ycpp::Event&) { fired++; });
    outer = std::move(inner);
    check(!inner.active() && outer.active(), "moved-from is inactive");
  }
  t.insert(0, "c");
  checkEq(fired, 2, "moved subscription still fires");
  outer.reset();
  check(!outer.active(), "reset deactivates");
  t.insert(0, "d");
  checkEq(fired, 2, "no events after reset");

  // Doc update-handler flavor.
  int updates = 0;
  {
    auto usub = d.subscribeUpdates(
        [&](const std::vector<uint8_t>&, const std::string&) { updates++; });
    t.insert(0, "e");
    checkEq(updates, 1, "update subscription fires");
  }
  t.insert(0, "f");
  checkEq(updates, 1, "update subscription auto-detached");
}

TEST(on_update_event) {
  ycpp::Doc a(1);
  std::vector<std::vector<uint8_t>> updates;
  std::vector<std::string> origins;
  a.onUpdate([&](const std::vector<uint8_t>& u, const std::string& o) {
    updates.push_back(u);
    origins.push_back(o);
  });
  a.transact([&] { a.getText().insert(0, "hello"); }, "edit-1");
  a.getText().insert(5, " world");
  a.getText().remove(0, 1);
  checkEq(updates.size(), size_t(3), "one update per transaction");
  checkEq(origins[0], std::string("edit-1"), "origin on update handler");
  // Replaying the incremental updates reproduces the document.
  ycpp::Doc b(2);
  for (const auto& u : updates) b.applyUpdate(u);
  checkEq(b.getText().toString(), a.getText().toString(), "updates replay to same doc");
  check(!b.hasPending(), "replay complete");
}

TEST(on_update_covers_remote_applies) {
  // A relay can re-broadcast what applyUpdate produced.
  ycpp::Doc a(1), relay(2), c(3);
  std::vector<std::vector<uint8_t>> rebroadcast;
  relay.onUpdate([&](const std::vector<uint8_t>& u, const std::string&) {
    rebroadcast.push_back(u);
  });
  a.getText().insert(0, "via relay");
  relay.applyUpdate(a.encodeStateAsUpdate(), "remote");
  checkEq(rebroadcast.size(), size_t(1), "relay update fired");
  for (const auto& u : rebroadcast) c.applyUpdate(u);
  checkEq(c.getText().toString(), std::string("via relay"), "relayed content");
}

// ----------------------------------------------------------- compaction

TEST(compaction_sequential_typing) {
  ycpp::Doc d(1);
  auto t = d.getText();
  for (int i = 0; i < 50; i++) t.insert(t.length(), "x");
  checkEq(t.length(), uint64_t(50), "all chars present");
  checkEq(d.structCount(), size_t(1), "typed run merged into one struct");
  ycpp::Doc b(2);
  b.applyUpdate(d.encodeStateAsUpdate());
  checkEq(b.getText().toString(), t.toString(), "receiver text matches");
  checkEq(b.structCount(), size_t(1), "receiver also compacts");
}

TEST(compaction_tombstones_and_gc) {
  ycpp::Doc d(1);
  auto t = d.getText();
  t.insert(0, std::string(500, 'a'));
  t.remove(0, 500);
  checkEq(t.toString(), std::string(""), "text empty");
  checkEq(d.structCount(), size_t(1), "tombstones merged into one struct");
  auto u = d.encodeStateAsUpdate();
  check(u.size() < 40, "gc'd update is tiny (" + std::to_string(u.size()) + " bytes)");
  ycpp::Doc b(2);
  b.applyUpdate(u);
  checkEq(b.getText().toString(), std::string(""), "gc'd update applies cleanly");
}

TEST(compaction_delete_char_by_char) {
  ycpp::Doc d(1);
  auto t = d.getText();
  t.insert(0, "abcdefghij");
  for (int i = 0; i < 10; i++) t.remove(0, 1);
  checkEq(t.toString(), std::string(""), "all deleted");
  checkEq(d.structCount(), size_t(1), "char-by-char deletes compact");
}

TEST(compaction_preserves_convergence) {
  // Splitting a previously merged item must reconstruct the original
  // origins exactly (merge conditions guarantee it) — sync both ways.
  ycpp::Doc a(1), b(2);
  for (int i = 0; i < 5; i++) {
    a.getText().insert(a.getText().length(), std::string(1, static_cast<char>('a' + i)));
  }
  checkEq(a.structCount(), size_t(1), "run merged on a");
  sync(a, b);
  b.getText().insert(2, "XY");  // forces a split of the merged item
  a.getText().remove(1, 2);
  sync(a, b);
  sync(a, b);
  checkEq(a.getText().toString(), b.getText().toString(), "converged after split of merged item");
}

// ----------------------------------------------------------------- fuzz

TEST(fuzz_random_ops_converge) {
  std::mt19937 rng(12345);
  const char* words[] = {"a", "bb", "ccc", "Xy", "0", "\xC3\xA9", "\xF0\x9F\x98\x80", "hello "};
  for (int round = 0; round < 20; round++) {
    ycpp::Doc d0(100), d1(200), d2(300);
    ycpp::Doc* docs[3] = {&d0, &d1, &d2};
    for (int step = 0; step < 60; step++) {
      ycpp::Doc& d = *docs[rng() % 3];
      auto t = d.getText();
      uint32_t action = rng() % 10;
      if (action < 6) {
        uint64_t idx = t.length() ? rng() % (t.length() + 1) : 0;
        t.insert(idx, words[rng() % 8]);
      } else if (action < 8 && t.length() > 0) {
        uint64_t idx = rng() % t.length();
        uint64_t len = 1 + rng() % std::min<uint64_t>(t.length() - idx, 4);
        t.remove(idx, len);
      } else {
        sync(*docs[rng() % 3], *docs[rng() % 3]);
      }
    }
    // Full pairwise sync until quiescent.
    for (int i = 0; i < 3; i++) {
      sync(d0, d1);
      sync(d1, d2);
      sync(d0, d2);
    }
    std::string s0 = d0.getText().toString();
    check(s0 == d1.getText().toString() && s0 == d2.getText().toString(),
          "round " + std::to_string(round) + " converged");
    check(!d0.hasPending() && !d1.hasPending() && !d2.hasPending(),
          "round " + std::to_string(round) + " no pending");
  }
}

TEST(fuzz_update_reload_stability) {
  // Doc serialized and reloaded through a fresh Doc must encode the same text.
  std::mt19937 rng(999);
  ycpp::Doc a(1), b(2);
  for (int step = 0; step < 100; step++) {
    ycpp::Doc& d = step % 2 ? a : b;
    auto t = d.getText();
    if (rng() % 3 && t.length() > 0) {
      t.remove(rng() % t.length(), 1);
    } else {
      t.insert(t.length() ? rng() % (t.length() + 1) : 0, "ab");
    }
    if (step % 7 == 0) sync(a, b);
  }
  sync(a, b);
  ycpp::Doc fresh(3);
  fresh.applyUpdate(a.encodeStateAsUpdate());
  checkEq(fresh.getText().toString(), a.getText().toString(), "reload matches");
}

}  // namespace

int main(int argc, char** argv) {
  std::string filter = argc > 1 ? argv[1] : "";
  int run = 0;
  for (auto& t : tests()) {
    if (!filter.empty() && t.name.find(filter) == std::string::npos) continue;
    currentTest = t.name;
    int before = failures;
    try {
      t.fn();
    } catch (const std::exception& e) {
      failures++;
      std::cerr << "FAIL [" << t.name << "] exception: " << e.what() << "\n";
    }
    std::cout << (failures == before ? "ok   " : "FAIL ") << t.name << "\n";
    run++;
  }
  std::cout << run << " tests, " << checks << " checks, " << failures << " failures\n";
  return failures == 0 ? 0 : 1;
}
