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
  auto& t = d.getText();
  t.insert(0, "hello world");
  t.insert(5, ",");
  checkEq(t.toString(), std::string("hello, world"), "insert middle");
  t.remove(5, 1);
  checkEq(t.toString(), std::string("hello world"), "delete middle");
  t.insert(11, "!");
  checkEq(t.toString(), std::string("hello world!"), "append");
  t.remove(0, 6);
  checkEq(t.toString(), std::string("world!"), "delete prefix");
  checkEq(t.length, uint64_t(6), "length tracking");
}

TEST(text_unicode_lengths) {
  ycpp::Doc d(1);
  auto& t = d.getText();
  t.insert(0, "a\xF0\x9F\x98\x80z");  // a😀z — 4 UTF-16 units
  checkEq(t.length, uint64_t(4), "UTF-16 length");
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
  checkEq(a.getText().length, uint64_t(3), "3 units");
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
      auto& t = d.getText();
      uint32_t action = rng() % 10;
      if (action < 6) {
        uint64_t idx = t.length ? rng() % (t.length + 1) : 0;
        t.insert(idx, words[rng() % 8]);
      } else if (action < 8 && t.length > 0) {
        uint64_t idx = rng() % t.length;
        uint64_t len = 1 + rng() % std::min<uint64_t>(t.length - idx, 4);
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
    auto& t = d.getText();
    if (rng() % 3 && t.length > 0) {
      t.remove(rng() % t.length, 1);
    } else {
      t.insert(t.length ? rng() % (t.length + 1) : 0, "ab");
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
