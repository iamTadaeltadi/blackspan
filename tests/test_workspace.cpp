#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "blackspan/frame.h"
#include "blackspan/sha256.h"
#include "blackspan/workspace.h"

using namespace blackspan;

static int failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond,         \
                   __FILE__, __LINE__);                                 \
      ++failures;                                                       \
    }                                                                   \
  } while (0)

static std::string hex_of(const std::string& s) {
  return sha256(reinterpret_cast<const uint8_t*>(s.data()), s.size()).hex();
}

static std::filesystem::path make_temp_dir() {
  const auto base = std::filesystem::temp_directory_path();
  for (int attempt = 0; attempt < 4096; ++attempt) {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto p = base / ("bs_ws_test_" + std::to_string(stamp) + "_" +
                           std::to_string(attempt));
    std::error_code ec;
    if (std::filesystem::create_directory(p, ec) && !ec) return p;
  }
  return base / "bs_ws_test_fallback";
}

// Builds a small but structurally valid recording with known record-type
// counts: SessionBegin 1, SampleBlock 2, Marker 1, Fault 1, SessionEnd 1.
static std::vector<uint8_t> build_recording() {
  std::vector<uint8_t> wire;
  const auto add = [&](RecordType t, std::vector<uint8_t> payload) {
    FrameHeader h;
    h.type = t;
    encode_frame(h, payload.data(), payload.size(), wire);
  };
  add(RecordType::SessionBegin, {});
  add(RecordType::SampleBlock, {1, 2, 3});
  add(RecordType::SampleBlock, {4, 5, 6, 7});
  add(RecordType::Marker, {0xAA});
  add(RecordType::Fault, {0x07});
  add(RecordType::SessionEnd, {});
  return wire;
}

static void test_sha256_vectors() {
  CHECK(hex_of("") ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(hex_of("abc") ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  // FIPS 180-4 two-block example (448-bit message).
  CHECK(hex_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

  // Streaming in odd-sized chunks must match the one-shot digest.
  const std::string msg(1000, 'a');
  Sha256 h;
  size_t off = 0;
  const size_t chunks[] = {1, 7, 64, 100, 63, 200};
  for (size_t step : chunks) {
    const size_t n = std::min(step, msg.size() - off);
    h.update(reinterpret_cast<const uint8_t*>(msg.data()) + off, n);
    off += n;
  }
  h.update(reinterpret_cast<const uint8_t*>(msg.data()) + off, msg.size() - off);
  CHECK(h.finish().hex() == hex_of(msg));
}

static void run_content_store_suite(ContentStore& store) {
  const std::string a = "the quick brown fox";
  const std::string b = "the quick brown fox";  // identical bytes
  const std::string c = "a different recording";

  const std::string ida =
      store.put(reinterpret_cast<const uint8_t*>(a.data()), a.size());
  const std::string idb =
      store.put(reinterpret_cast<const uint8_t*>(b.data()), b.size());
  const std::string idc =
      store.put(reinterpret_cast<const uint8_t*>(c.data()), c.size());

  CHECK(!ida.empty());
  CHECK(ida == hex_of(a));      // id is the content hash
  CHECK(ida == idb);            // dedup: identical bytes share an id
  CHECK(idc != ida);
  CHECK(store.count() == 2);    // three puts, two distinct objects

  CHECK(store.has(ida));
  CHECK(store.has(idc));
  CHECK(!store.has(
      "0000000000000000000000000000000000000000000000000000000000000000"));

  std::vector<uint8_t> out;
  CHECK(store.get(ida, out));
  CHECK(out.size() == a.size());
  CHECK(std::string(out.begin(), out.end()) == a);

  out.clear();
  CHECK(!store.get("nope", out));

  // Empty content round-trips and hashes to the well-known empty digest.
  const std::string ide = store.put(nullptr, 0);
  CHECK(ide == hex_of(""));
  CHECK(store.has(ide));
  std::vector<uint8_t> empty_out{1, 2, 3};
  CHECK(store.get(ide, empty_out));
  CHECK(empty_out.empty());
}

static void test_content_store_memory() {
  ContentStore store(std::make_shared<MemoryBackend>());
  run_content_store_suite(store);
}

static void test_content_store_file(const std::filesystem::path& root) {
  auto backend = std::make_shared<FileBackend>(root.string());
  ContentStore store(backend);
  run_content_store_suite(store);

  // A fresh backend over the same root must see the persisted objects.
  auto reopened = std::make_shared<FileBackend>(root.string());
  const std::string id = hex_of("the quick brown fox");
  CHECK(reopened->has(id));
  std::vector<uint8_t> out;
  CHECK(reopened->get(id, out));
  CHECK(std::string(out.begin(), out.end()) == "the quick brown fox");
  CHECK(reopened->ids().size() == 3);  // two texts + empty object
}

static void check_recording_summary(const RecordingSummary& s) {
  CHECK(s.total_frames == 6);
  CHECK(s.terminal_status == FrameStatus::Ok);
  CHECK(s.type_counts.at(RecordType::SessionBegin) == 1);
  CHECK(s.type_counts.at(RecordType::SampleBlock) == 2);
  CHECK(s.type_counts.at(RecordType::Marker) == 1);
  CHECK(s.type_counts.at(RecordType::Fault) == 1);
  CHECK(s.type_counts.at(RecordType::SessionEnd) == 1);
  CHECK(s.type_counts.find(RecordType::Reconnect) == s.type_counts.end());
}

static void test_workspace_memory() {
  Workspace ws(std::make_shared<MemoryBackend>());
  const std::vector<uint8_t> rec = build_recording();

  CHECK(ws.import("flight-01", rec.data(), rec.size()) == WorkspaceError::Ok);
  CHECK(ws.has("flight-01"));
  CHECK(ws.size() == 1);

  // Bad and duplicate names are rejected as status values, never thrown.
  CHECK(ws.import("", rec.data(), rec.size()) == WorkspaceError::BadName);
  CHECK(ws.import("bad\tname", rec.data(), rec.size()) ==
        WorkspaceError::BadName);
  CHECK(ws.import("flight-01", rec.data(), rec.size()) ==
        WorkspaceError::Exists);

  RecordingInfo info;
  CHECK(ws.stat("flight-01", info) == WorkspaceError::Ok);
  CHECK(info.name == "flight-01");
  CHECK(info.byte_size == rec.size());
  CHECK(info.content_id == sha256(rec.data(), rec.size()).hex());
  check_recording_summary(info.summary);

  CHECK(ws.stat("missing", info) == WorkspaceError::NotFound);

  std::vector<uint8_t> out;
  CHECK(ws.open("flight-01", out) == WorkspaceError::Ok);
  CHECK(out == rec);
  CHECK(ws.open("missing", out) == WorkspaceError::NotFound);

  // Dedup across two names: one object, two catalog entries.
  CHECK(ws.import("flight-01-copy", rec.data(), rec.size()) ==
        WorkspaceError::Ok);
  CHECK(ws.size() == 2);
  CHECK(ws.object_count() == 1);
  RecordingInfo copy_info;
  CHECK(ws.stat("flight-01-copy", copy_info) == WorkspaceError::Ok);
  CHECK(copy_info.content_id == info.content_id);

  CHECK(ws.list().size() == 2);

  CHECK(ws.remove("flight-01") == WorkspaceError::Ok);
  CHECK(!ws.has("flight-01"));
  CHECK(ws.remove("flight-01") == WorkspaceError::NotFound);
  CHECK(ws.open("flight-01", out) == WorkspaceError::NotFound);
  // The shared object is still reachable through the surviving name.
  CHECK(ws.open("flight-01-copy", out) == WorkspaceError::Ok);
  CHECK(out == rec);
}

static void test_workspace_persistence(const std::filesystem::path& root) {
  const std::string catalog = (root / "catalog.tsv").string();
  const std::vector<uint8_t> rec = build_recording();
  const std::vector<uint8_t> other = {0xDE, 0xAD, 0xBE, 0xEF};

  {
    Workspace ws(std::make_shared<FileBackend>(root.string()), catalog);
    CHECK(ws.import("alpha", rec.data(), rec.size()) == WorkspaceError::Ok);
    CHECK(ws.import("alpha-dup", rec.data(), rec.size()) == WorkspaceError::Ok);
    CHECK(ws.import("beta", other.data(), other.size()) == WorkspaceError::Ok);
    CHECK(ws.object_count() == 2);  // alpha == alpha-dup share one object
  }

  // A brand-new Workspace over the same root and catalog file must recover the
  // catalog, its metadata, and the underlying objects.
  {
    Workspace ws(std::make_shared<FileBackend>(root.string()), catalog);
    CHECK(ws.size() == 3);
    CHECK(ws.has("alpha"));
    CHECK(ws.has("alpha-dup"));
    CHECK(ws.has("beta"));

    RecordingInfo info;
    CHECK(ws.stat("alpha", info) == WorkspaceError::Ok);
    CHECK(info.byte_size == rec.size());
    CHECK(info.content_id == sha256(rec.data(), rec.size()).hex());
    check_recording_summary(info.summary);

    std::vector<uint8_t> out;
    CHECK(ws.open("alpha", out) == WorkspaceError::Ok);
    CHECK(out == rec);
    CHECK(ws.open("beta", out) == WorkspaceError::Ok);
    CHECK(out == other);

    // Persisted removal survives another reopen.
    CHECK(ws.remove("beta") == WorkspaceError::Ok);
  }
  {
    Workspace ws(std::make_shared<FileBackend>(root.string()), catalog);
    CHECK(ws.size() == 2);
    CHECK(!ws.has("beta"));
    CHECK(ws.has("alpha"));
  }
}

int main() {
  test_sha256_vectors();
  test_content_store_memory();
  test_workspace_memory();

  const std::filesystem::path tmp = make_temp_dir();
  test_content_store_file(tmp / "cas");
  test_workspace_persistence(tmp / "ws");

  std::error_code ec;
  std::filesystem::remove_all(tmp, ec);

  if (failures) {
    std::fprintf(stderr, "%d checks failed\n", failures);
    return 1;
  }
  std::printf("all workspace tests passed\n");
  return 0;
}
