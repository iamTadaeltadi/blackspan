#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "blackspan/assemble.h"
#include "blackspan/frame.h"
#include "blackspan/pipeline.h"
#include "blackspan/rewrite.h"

using namespace blackspan;

static int failures = 0;
#define CHECK(cond)                                             \
  do {                                                          \
    if (!(cond)) {                                              \
      std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, \
                   __FILE__, __LINE__);                         \
      ++failures;                                               \
    }                                                           \
  } while (0)

static std::vector<uint8_t> assemble(const std::string& script) {
  AssembleResult r = assemble_script(script);
  for (const AssembleDiagnostic& d : r.errors) {
    std::fprintf(stderr, "assemble: %s\n",
                 describe_assemble_diagnostic(d).c_str());
    ++failures;
  }
  return r.bytes;
}

// Counts the frames of a given record type in a recording.
static size_t count_type(const std::vector<uint8_t>& bytes, RecordType want) {
  FrameStream fs(bytes.data(), bytes.size());
  Frame f;
  size_t n = 0;
  while (fs.next(f)) {
    if (f.header.type == want) ++n;
  }
  return n;
}

static size_t count_frames(const std::vector<uint8_t>& bytes) {
  FrameStream fs(bytes.data(), bytes.size());
  Frame f;
  size_t n = 0;
  while (fs.next(f)) ++n;
  return n;
}

static const char* kMulti =
    "tick 10\n"
    "device 1\n"
    "session 1\n"
    "ts 100\n"
    "session-begin label=a\n"
    "schema id=1 name=m fields=v:u16\n"
    "samples channel=1 delta=0\n"
    "  row 10\n"
    "end\n"
    "fault code=1 severity=warning\n"
    "session-end reason=0\n"
    "device 2\n"
    "session 1\n"
    "ts 200\n"
    "session-begin label=b\n"
    "fault code=2 severity=error\n"
    "session-end reason=0\n";

static void test_slice_by_device() {
  std::vector<uint8_t> bytes = assemble(kMulti);
  RewriteFilter filter;
  filter.device = 1;
  RewriteResult r = rewrite_slice(bytes, filter);
  CHECK(r.input_status == FrameStatus::Ok);
  CHECK(r.frames_in == count_frames(bytes));
  CHECK(r.frames_kept > 0);

  // Every frame in the output belongs to device 1, and the output re-decodes.
  DecodedRecording rec;
  rec.decode(r.bytes.data(), r.bytes.size());
  CHECK(rec.final_status() == FrameStatus::Ok);
  CHECK(rec.frame_count() == r.frames_kept);
  FrameStream fs(r.bytes.data(), r.bytes.size());
  Frame f;
  while (fs.next(f)) CHECK(f.header.device_id == 1);
  CHECK(rec.sessions().sessions().size() == 1);
  CHECK(rec.sessions().sessions()[0].device_id == 1);
}

static void test_slice_by_type() {
  std::vector<uint8_t> bytes = assemble(kMulti);
  RewriteFilter filter;
  filter.types = {RecordType::Fault};
  RewriteResult r = rewrite_slice(bytes, filter);
  CHECK(r.frames_kept == 2);  // one fault per device
  CHECK(count_frames(r.bytes) == 2);
  CHECK(count_type(r.bytes, RecordType::Fault) == 2);
  CHECK(count_type(r.bytes, RecordType::SampleBlock) == 0);
}

static void test_slice_by_time() {
  std::vector<uint8_t> bytes = assemble(kMulti);
  RewriteFilter filter;
  filter.since = 200;  // only device 2's frames start at ts 200
  RewriteResult r = rewrite_slice(bytes, filter);
  FrameStream fs(r.bytes.data(), r.bytes.size());
  Frame f;
  while (fs.next(f)) CHECK(f.header.monotonic_ts >= 200);
  CHECK(r.frames_kept > 0);
}

static void test_slice_empty_filter_is_identity_in_content() {
  std::vector<uint8_t> bytes = assemble(kMulti);
  RewriteFilter filter;  // no constraints
  RewriteResult r = rewrite_slice(bytes, filter);
  CHECK(r.frames_kept == r.frames_in);
  // Re-encoding a valid recording reproduces it byte-for-byte.
  CHECK(r.bytes == bytes);
}

static void test_slice_preserves_crc_on_corrupt_tail() {
  // A recording whose final frame is truncated: the slice keeps the good frames
  // and reports Incomplete.
  std::string script =
      "device 1\n"
      "session 1\n"
      "marker text=ok\n"
      "marker text=cut\n"
      "damage mode=truncate bytes=6\n";
  std::vector<uint8_t> bytes = assemble(script);
  RewriteFilter filter;
  RewriteResult r = rewrite_slice(bytes, filter);
  CHECK(r.input_status == FrameStatus::Incomplete);
  CHECK(r.frames_kept == 1);  // only the first marker decoded
  DecodedRecording rec;
  rec.decode(r.bytes.data(), r.bytes.size());
  CHECK(rec.final_status() == FrameStatus::Ok);  // the output is clean
  CHECK(rec.frame_count() == 1);
}

static void test_merge_orders_by_timestamp() {
  // Two single-device recordings whose timestamps interleave.
  std::vector<uint8_t> a = assemble(
      "device 1\n"
      "session 1\n"
      "marker text=a1 ts=100\n"
      "marker text=a2 ts=300\n");
  std::vector<uint8_t> b = assemble(
      "device 2\n"
      "session 1\n"
      "marker text=b1 ts=200\n"
      "marker text=b2 ts=400\n");

  MergeResult m = rewrite_merge({a, b});
  CHECK(m.frames_out == 4);
  CHECK(m.inputs_truncated == 0);

  FrameStream fs(m.bytes.data(), m.bytes.size());
  Frame f;
  std::vector<uint64_t> stamps;
  while (fs.next(f)) stamps.push_back(f.header.monotonic_ts);
  CHECK(stamps.size() == 4);
  // Merged output is sorted by timestamp: 100, 200, 300, 400.
  for (size_t i = 1; i < stamps.size(); ++i) {
    CHECK(stamps[i - 1] <= stamps[i]);
  }
  if (stamps.size() == 4) {
    CHECK(stamps[0] == 100 && stamps[1] == 200 && stamps[2] == 300 &&
          stamps[3] == 400);
  }
}

static void test_merge_stable_on_equal_ts() {
  // Frames sharing a timestamp keep input order (input 0 before input 1).
  std::vector<uint8_t> a = assemble(
      "device 1\n"
      "session 1\n"
      "marker text=from_a ts=500\n");
  std::vector<uint8_t> b = assemble(
      "device 2\n"
      "session 1\n"
      "marker text=from_b ts=500\n");
  MergeResult m = rewrite_merge({a, b});
  FrameStream fs(m.bytes.data(), m.bytes.size());
  Frame f;
  std::vector<uint32_t> devs;
  while (fs.next(f)) devs.push_back(f.header.device_id);
  CHECK(devs.size() == 2);
  if (devs.size() == 2) {
    CHECK(devs[0] == 1 && devs[1] == 2);  // input 0's frame first
  }
}

static void test_merge_reconstructs_both_devices() {
  std::vector<uint8_t> a = assemble(
      "device 1\n"
      "session 1\n"
      "ts 100\n"
      "session-begin label=a\n"
      "session-end reason=0\n");
  std::vector<uint8_t> b = assemble(
      "device 2\n"
      "session 1\n"
      "ts 150\n"
      "session-begin label=b\n"
      "session-end reason=0\n");
  MergeResult m = rewrite_merge({a, b});
  DecodedRecording rec;
  rec.decode(m.bytes.data(), m.bytes.size());
  CHECK(rec.final_status() == FrameStatus::Ok);
  CHECK(rec.sessions().sessions().size() == 2);
  CHECK(rec.sessions().summary().device_count == 2);
}

int main() {
  test_slice_by_device();
  test_slice_by_type();
  test_slice_by_time();
  test_slice_empty_filter_is_identity_in_content();
  test_slice_preserves_crc_on_corrupt_tail();
  test_merge_orders_by_timestamp();
  test_merge_stable_on_equal_ts();
  test_merge_reconstructs_both_devices();

  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("test_rewrite: all checks passed\n");
  return 0;
}
