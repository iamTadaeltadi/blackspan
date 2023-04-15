#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "blackspan/assemble.h"
#include "blackspan/driftmodel.h"
#include "blackspan/pipeline.h"

using namespace blackspan;

static int failures = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__, \
                   __LINE__);                                             \
      ++failures;                                                         \
    }                                                                     \
  } while (0)

static std::string large_recording(size_t count, bool derived_after) {
  std::string s;
  s += "device 1\nsession 1\nts 1000\nsession-begin label=a\n";
  s += "schema id=1 name=m fields=v:u16\n";
  s += "samples channel=1 delta=0 ts=1000\n";
  for (size_t i = 0; i < count; ++i) {
    s += "  row " + std::to_string(i % 500) + "\n";
  }
  s += "end\n";
  if (derived_after) {
    s += "derived id=9 primary=1 name=d expr=\"v\"\n";
  }
  s += "session-end reason=0\n";
  return s;
}

static void test_large_derived_after_samples() {
  const std::string script = large_recording(300, true);
  AssembleResult a = assemble_script(script);
  CHECK(a.ok());
  DecodedRecording rec;
  rec.decode(a.bytes.data(), a.bytes.size());

  CHECK(rec.derived().size() == 1);
  if (!rec.derived().empty()) {
    CHECK(rec.derived()[0].derived_id == 9);
    CHECK(rec.derived()[0].values.size() == 300);
  }

  std::vector<ChannelDrift> drift = channel_drift(rec);
  CHECK(drift.size() == 1);
  if (!drift.empty()) {
    CHECK(drift[0].samples == 300);
    CHECK(drift[0].residual_rms == 0.0);
    CHECK(!describe_drift(drift[0]).empty());
  }
}

static void test_small_channel_not_materialized() {
  const std::string script = large_recording(8, true);
  AssembleResult a = assemble_script(script);
  CHECK(a.ok());
  DecodedRecording rec;
  rec.decode(a.bytes.data(), a.bytes.size());
  CHECK(rec.derived().empty());
  CHECK(channel_drift(rec).empty());
}

static void test_no_derived() {
  const std::string script = large_recording(300, false);
  AssembleResult a = assemble_script(script);
  CHECK(a.ok());
  DecodedRecording rec;
  rec.decode(a.bytes.data(), a.bytes.size());
  CHECK(rec.derived().empty());
  CHECK(channel_drift(rec).empty());
}

int main() {
  test_large_derived_after_samples();
  test_small_channel_not_materialized();
  test_no_derived();
  return failures ? 1 : 0;
}
