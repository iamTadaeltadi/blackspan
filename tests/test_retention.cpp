#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "blackspan/assemble.h"
#include "blackspan/frame.h"
#include "blackspan/retention.h"

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

static std::vector<Frame> decode_all(const std::vector<uint8_t>& bytes,
                                     FrameStatus* status) {
  FrameStream fs(bytes.data(), bytes.size());
  std::vector<Frame> out;
  Frame f;
  while (fs.next(f)) out.push_back(f);
  if (status) *status = fs.status();
  return out;
}

static size_t count_type(const std::vector<uint8_t>& bytes, RecordType want) {
  FrameStatus st = FrameStatus::Ok;
  std::vector<Frame> frames = decode_all(bytes, &st);
  size_t n = 0;
  for (const Frame& f : frames) {
    if (f.header.type == want) ++n;
  }
  return n;
}

static size_t count_frames(const std::vector<uint8_t>& bytes) {
  FrameStatus st = FrameStatus::Ok;
  return decode_all(bytes, &st).size();
}

static const char* kMix =
    "device 1\n"
    "session 1\n"
    "tick 10\n"
    "ts 100\n"
    "session-begin label=a\n"
    "schema id=1 name=m fields=v:u16\n"
    "samples channel=1\n"
    "  row 10\n"
    "end\n"
    "state from=0 to=1 cause=1\n"
    "config param=1 old=0 new=1\n"
    "fault code=1 severity=warning\n"
    "marker text=note\n"
    "session-end reason=0\n";

static const char* kBudget =
    "device 1\n"
    "session 1\n"
    "tick 0\n"
    "schema id=1 name=m fields=v:u16\n"
    "samples channel=1 ts=100\n"
    "  row 1\n"
    "end\n"
    "samples channel=1 ts=200\n"
    "  row 2\n"
    "end\n"
    "samples channel=1 ts=300\n"
    "  row 3\n"
    "end\n"
    "fault code=1 severity=error ts=400\n"
    "fault code=2 severity=error ts=500\n";

static const char* kMarkers =
    "device 1\n"
    "session 1\n"
    "tick 0\n"
    "marker text=m1 ts=100\n"
    "marker text=m2 ts=200\n"
    "marker text=m3 ts=300\n"
    "marker text=m4 ts=400\n";

static void test_priority_mapping() {
  CHECK(priority_of(RecordType::Fault) == RetentionPriority::Faults);
  CHECK(priority_of(RecordType::StateTransition) ==
        RetentionPriority::StateTransitions);
  CHECK(priority_of(RecordType::SessionBegin) == RetentionPriority::Sessions);
  CHECK(priority_of(RecordType::SessionEnd) == RetentionPriority::Sessions);
  CHECK(priority_of(RecordType::Reconnect) == RetentionPriority::Sessions);
  CHECK(priority_of(RecordType::SampleBlock) == RetentionPriority::Samples);
  CHECK(priority_of(RecordType::Marker) == RetentionPriority::Markers);
  CHECK(priority_of(RecordType::ConfigChange) ==
        RetentionPriority::ConfigChanges);
  CHECK(priority_of(RecordType::SchemaDef) == RetentionPriority::Other);
  CHECK(default_priority_order().size() == kRetentionPriorityCount);
}

static void test_no_limits_keeps_everything() {
  std::vector<uint8_t> bytes = assemble(kMix);
  CHECK(count_frames(bytes) == 8);

  RetentionPolicy policy;
  RetentionPlan plan = plan_retention(bytes, policy);
  CHECK(plan.input_status == FrameStatus::Ok);
  CHECK(plan.frames_in == 8);
  CHECK(plan.frames_kept == plan.frames_in);
  CHECK(plan.bytes_kept == plan.bytes_in);
  CHECK(plan.kept_indices.size() == 8);
  CHECK(retention_ratio(plan) == 1.0);
  CHECK(frames_dropped(plan) == 0);
  CHECK(bytes_dropped(plan) == 0);

  std::vector<uint8_t> out = apply_retention(bytes, policy);
  CHECK(out == bytes);

  FrameStatus st = FrameStatus::Ok;
  std::vector<Frame> reframes = decode_all(out, &st);
  CHECK(st == FrameStatus::Ok);
  CHECK(reframes.size() == 8);
}

static void test_drop_samples_only() {
  std::vector<uint8_t> bytes = assemble(kMix);
  RetentionPolicy policy;
  policy.drop_samples = true;

  RetentionPlan plan = plan_retention(bytes, policy);
  CHECK(plan.frames_kept == 7);
  CHECK(plan.kept_by_priority[static_cast<size_t>(RetentionPriority::Samples)] ==
        0);

  std::vector<uint8_t> out = apply_retention(bytes, policy);
  CHECK(count_type(out, RecordType::SampleBlock) == 0);
  CHECK(count_type(out, RecordType::Marker) == 1);
  CHECK(count_type(out, RecordType::Fault) == 1);
  CHECK(count_frames(out) == 7);
}

static void test_drop_markers_only() {
  std::vector<uint8_t> bytes = assemble(kMix);
  RetentionPolicy policy;
  policy.drop_markers = true;

  RetentionPlan plan = plan_retention(bytes, policy);
  CHECK(plan.frames_kept == 7);
  CHECK(plan.kept_by_priority[static_cast<size_t>(RetentionPriority::Markers)] ==
        0);

  std::vector<uint8_t> out = apply_retention(bytes, policy);
  CHECK(count_type(out, RecordType::Marker) == 0);
  CHECK(count_type(out, RecordType::SampleBlock) == 1);
  CHECK(count_frames(out) == 7);
}

static void test_time_window() {
  std::vector<uint8_t> bytes = assemble(kMix);
  RetentionPolicy policy;
  policy.keep_since_ts = 120;
  policy.keep_until_ts = 150;

  RetentionPlan plan = plan_retention(bytes, policy);
  CHECK(plan.frames_kept == 4);

  std::vector<uint8_t> out = apply_retention(bytes, policy);
  FrameStatus st = FrameStatus::Ok;
  std::vector<Frame> frames = decode_all(out, &st);
  CHECK(st == FrameStatus::Ok);
  CHECK(frames.size() == 4);
  std::vector<uint64_t> stamps;
  for (const Frame& f : frames) {
    CHECK(f.header.monotonic_ts >= 120);
    CHECK(f.header.monotonic_ts <= 150);
    stamps.push_back(f.header.monotonic_ts);
  }
  CHECK(stamps.size() == 4);
  if (stamps.size() == 4) {
    CHECK(stamps[0] == 120 && stamps[1] == 130 && stamps[2] == 140 &&
          stamps[3] == 150);
  }
}

static void test_budget_priority_favors_faults() {
  std::vector<uint8_t> bytes = assemble(kBudget);
  CHECK(count_frames(bytes) == 6);

  RetentionPolicy policy;
  policy.max_frames = 2;
  policy.priority_order = {RetentionPriority::Faults};

  RetentionPlan plan = plan_retention(bytes, policy);
  CHECK(plan.frames_kept == 2);
  CHECK(plan.kept_indices.size() == 2);
  CHECK(plan.kept_by_priority[static_cast<size_t>(RetentionPriority::Faults)] ==
        2);
  CHECK(plan.kept_by_priority[static_cast<size_t>(RetentionPriority::Samples)] ==
        0);

  std::vector<uint8_t> out = apply_retention(bytes, policy);
  FrameStatus st = FrameStatus::Ok;
  std::vector<Frame> frames = decode_all(out, &st);
  CHECK(st == FrameStatus::Ok);
  CHECK(frames.size() == 2);
  for (const Frame& f : frames) {
    CHECK(f.header.type == RecordType::Fault);
  }

  RetentionPlan again = plan_retention(bytes, policy);
  CHECK(again.kept_indices == plan.kept_indices);
}

static void test_budget_recency_tiebreak() {
  std::vector<uint8_t> bytes = assemble(kMarkers);
  CHECK(count_frames(bytes) == 4);

  RetentionPolicy policy;
  policy.max_frames = 2;

  RetentionPlan plan = plan_retention(bytes, policy);
  CHECK(plan.frames_kept == 2);
  std::vector<size_t> expected = {2, 3};
  CHECK(plan.kept_indices == expected);

  std::vector<uint8_t> out = apply_retention(bytes, policy);
  FrameStatus st = FrameStatus::Ok;
  std::vector<Frame> frames = decode_all(out, &st);
  CHECK(st == FrameStatus::Ok);
  CHECK(frames.size() == 2);
  if (frames.size() == 2) {
    CHECK(frames[0].header.monotonic_ts == 300);
    CHECK(frames[1].header.monotonic_ts == 400);
  }
}

static void test_budget_zero_is_unlimited() {
  std::vector<uint8_t> bytes = assemble(kBudget);
  RetentionPolicy policy;
  policy.max_frames = 0;
  RetentionPlan plan = plan_retention(bytes, policy);
  CHECK(plan.frames_kept == plan.frames_in);
}

static void test_apply_matches_planned_kept() {
  std::vector<uint8_t> bytes = assemble(kBudget);
  RetentionPolicy policy;
  policy.max_frames = 3;
  policy.priority_order = {RetentionPriority::Faults,
                           RetentionPriority::Samples};

  RetentionPlan plan = plan_retention(bytes, policy);
  std::vector<uint8_t> out = apply_retention(bytes, policy);

  FrameStatus st = FrameStatus::Ok;
  std::vector<Frame> input = decode_all(bytes, &st);
  CHECK(st == FrameStatus::Ok);
  std::vector<Frame> kept = decode_all(out, &st);
  CHECK(st == FrameStatus::Ok);

  CHECK(kept.size() == plan.frames_kept);
  CHECK(kept.size() == plan.kept_indices.size());
  for (size_t i = 0; i < plan.kept_indices.size(); ++i) {
    size_t src = plan.kept_indices[i];
    CHECK(src < input.size());
    if (src < input.size()) {
      CHECK(kept[i].header.type == input[src].header.type);
      CHECK(kept[i].header.monotonic_ts == input[src].header.monotonic_ts);
      CHECK(kept[i].payload == input[src].payload);
    }
  }
}

static void test_empty_input() {
  std::vector<uint8_t> empty;
  RetentionPolicy policy;
  RetentionPlan plan = plan_retention(empty, policy);
  CHECK(plan.frames_in == 0);
  CHECK(plan.frames_kept == 0);
  CHECK(plan.bytes_in == 0);
  CHECK(retention_ratio(plan) == 0.0);

  std::vector<uint8_t> out = apply_retention(empty, policy);
  CHECK(out.empty());

  RetentionPlan null_plan = plan_retention(nullptr, 0, policy);
  CHECK(null_plan.frames_in == 0);
}

static void test_keep_nothing() {
  std::vector<uint8_t> bytes = assemble(kMix);
  RetentionPolicy policy;
  policy.keep_since_ts = 100000;

  RetentionPlan plan = plan_retention(bytes, policy);
  CHECK(plan.frames_in == 8);
  CHECK(plan.frames_kept == 0);
  CHECK(plan.kept_indices.empty());

  std::vector<uint8_t> out = apply_retention(bytes, policy);
  CHECK(out.empty());
  CHECK(retention_ratio(plan) == 0.0);
  CHECK(frames_dropped(plan) == 8);
}

static void test_corrupt_trailing_bytes() {
  std::string script =
      "device 1\n"
      "session 1\n"
      "tick 0\n"
      "marker text=ok ts=100\n"
      "marker text=cut ts=200\n"
      "damage mode=truncate bytes=6\n";
  std::vector<uint8_t> bytes = assemble(script);

  RetentionPolicy policy;
  RetentionPlan plan = plan_retention(bytes, policy);
  CHECK(plan.input_status == FrameStatus::Incomplete);
  CHECK(plan.frames_in == 1);
  CHECK(plan.frames_kept == 1);

  std::vector<uint8_t> out = apply_retention(bytes, policy);
  FrameStatus st = FrameStatus::Ok;
  std::vector<Frame> frames = decode_all(out, &st);
  CHECK(st == FrameStatus::Ok);
  CHECK(frames.size() == 1);
  if (frames.size() == 1) {
    CHECK(frames[0].header.monotonic_ts == 100);
  }
}

static void test_byte_budget() {
  std::vector<uint8_t> bytes = assemble(kMarkers);

  RetentionPolicy probe;
  probe.max_frames = 1;
  RetentionPlan one = plan_retention(bytes, probe);
  CHECK(one.frames_kept == 1);
  size_t per_frame = one.bytes_kept;
  CHECK(per_frame > 1);

  RetentionPolicy policy;
  policy.max_bytes = per_frame * 2 + 1;
  RetentionPlan plan = plan_retention(bytes, policy);
  CHECK(plan.frames_kept == 2);
  CHECK(plan.bytes_kept <= policy.max_bytes);
  std::vector<size_t> expected = {2, 3};
  CHECK(plan.kept_indices == expected);

  std::vector<uint8_t> out = apply_retention(bytes, policy);
  CHECK(count_frames(out) == 2);
}

static void test_dropped_indices() {
  std::vector<uint8_t> bytes = assemble(kMix);
  RetentionPolicy policy;
  policy.keep_since_ts = 120;
  policy.keep_until_ts = 150;
  RetentionPlan plan = plan_retention(bytes, policy);

  std::vector<size_t> dropped = dropped_indices(plan);
  std::vector<size_t> expected = {0, 1, 6, 7};
  CHECK(dropped == expected);
  CHECK(dropped.size() == frames_dropped(plan));
}

static const char* kFramed =
    "device 1\n"
    "session 1\n"
    "tick 0\n"
    "schema id=1 name=m fields=v:u16 ts=5\n"
    "session-begin label=a ts=10\n"
    "samples channel=1 ts=100\n"
    "  row 1\n"
    "end\n"
    "samples channel=1 ts=200\n"
    "  row 2\n"
    "end\n"
    "samples channel=1 ts=300\n"
    "  row 3\n"
    "end\n"
    "session-end reason=0 ts=900\n";

static void test_keep_session_framing() {
  std::vector<uint8_t> bytes = assemble(kFramed);
  CHECK(count_frames(bytes) == 6);

  RetentionPolicy plain;
  plain.max_frames = 1;
  plain.priority_order = {RetentionPriority::Samples};
  RetentionPlan without = plan_retention(bytes, plain);
  CHECK(without.frames_kept == 1);
  CHECK(without.kept_by_priority[static_cast<size_t>(
            RetentionPriority::Samples)] == 1);

  RetentionPolicy framed = plain;
  framed.keep_session_framing = true;
  RetentionPlan with = plan_retention(bytes, framed);
  CHECK(with.frames_kept == 2);
  CHECK(with.kept_by_priority[static_cast<size_t>(
            RetentionPriority::Sessions)] == 2);
  CHECK(with.kept_by_priority[static_cast<size_t>(
            RetentionPriority::Samples)] == 0);

  std::vector<uint8_t> out = apply_retention(bytes, framed);
  FrameStatus st = FrameStatus::Ok;
  std::vector<Frame> frames = decode_all(out, &st);
  CHECK(st == FrameStatus::Ok);
  CHECK(frames.size() == 2);
  if (frames.size() == 2) {
    CHECK(frames[0].header.type == RecordType::SessionBegin);
    CHECK(frames[1].header.type == RecordType::SessionEnd);
  }
}

static void test_keep_tail() {
  std::vector<uint8_t> bytes = assemble(kBudget);
  CHECK(count_frames(bytes) == 6);

  RetentionPolicy policy;
  policy.max_frames = 1;
  policy.priority_order = {RetentionPriority::Faults};
  policy.keep_tail = 3;

  RetentionPlan plan = plan_retention(bytes, policy);
  CHECK(plan.frames_kept == 3);
  std::vector<size_t> expected = {3, 4, 5};
  CHECK(plan.kept_indices == expected);

  std::vector<uint8_t> out = apply_retention(bytes, policy);
  CHECK(count_frames(out) == 3);
}

static void test_describe_plan_nonempty() {
  std::vector<uint8_t> bytes = assemble(kMix);
  RetentionPolicy policy;
  policy.drop_samples = true;
  policy.priority_order = {RetentionPriority::Faults};
  RetentionPlan plan = plan_retention(bytes, policy);
  std::string text = describe_plan(plan);
  CHECK(!text.empty());
  CHECK(text.find("frames") != std::string::npos);

  std::string ptext = describe_policy(policy);
  CHECK(!ptext.empty());
  CHECK(ptext.find("policy") != std::string::npos);
}

int main() {
  test_priority_mapping();
  test_no_limits_keeps_everything();
  test_drop_samples_only();
  test_drop_markers_only();
  test_time_window();
  test_budget_priority_favors_faults();
  test_budget_recency_tiebreak();
  test_budget_zero_is_unlimited();
  test_apply_matches_planned_kept();
  test_empty_input();
  test_keep_nothing();
  test_corrupt_trailing_bytes();
  test_byte_budget();
  test_dropped_indices();
  test_keep_session_framing();
  test_keep_tail();
  test_describe_plan_nonempty();

  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("test_retention: all checks passed\n");
  return 0;
}
