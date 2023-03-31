#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "blackspan/assemble.h"
#include "blackspan/digest.h"
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

static void test_basic_histogram() {
  const char* script =
      "device 1\n"
      "session 1\n"
      "ts 1000\n"
      "session-begin label=a\n"
      "schema id=1 name=m fields=v:u16\n"
      "samples channel=1 delta=0 ts=1000\n"
      "  row 0\n"
      "  row 1\n"
      "  row 2\n"
      "  row 2\n"
      "  row 3\n"
      "end\n"
      "session-end reason=0\n";
  AssembleResult a = assemble_script(script);
  CHECK(a.ok());
  DecodedRecording rec;
  rec.decode(a.bytes.data(), a.bytes.size());

  ChannelDigest d = field_digest(rec.channels(), 1, 0);
  CHECK(d.channel_id == 1);
  CHECK(d.field_index == 0);
  CHECK(d.field_name == "v");
  CHECK(d.samples == 5);
  CHECK(d.min_value == 0.0);
  CHECK(d.max_value == 3.0);
  CHECK(d.bucket_count == 4);
  CHECK(d.buckets.size() == 4);

  uint32_t total = 0;
  for (uint32_t c : d.buckets) total += c;
  CHECK(total == 5);
  CHECK(d.buckets[2] == 2);
  CHECK(!describe_digest(d).empty());
}

static void test_all_channels() {
  const char* script =
      "device 1\n"
      "session 1\n"
      "ts 1000\n"
      "session-begin label=a\n"
      "schema id=1 name=m fields=v:u16\n"
      "samples channel=1 delta=0 ts=1000\n"
      "  row 10\n"
      "  row 20\n"
      "  row 30\n"
      "end\n"
      "session-end reason=0\n";
  AssembleResult a = assemble_script(script);
  CHECK(a.ok());
  DecodedRecording rec;
  rec.decode(a.bytes.data(), a.bytes.size());

  std::vector<ChannelDigest> all = channel_digests(rec.channels());
  CHECK(all.size() == 1);
  if (!all.empty()) {
    CHECK(all[0].samples == 3);
    CHECK(all[0].min_value == 10.0);
    CHECK(all[0].max_value == 30.0);
    uint32_t total = 0;
    for (uint32_t c : all[0].buckets) total += c;
    CHECK(total == 3);
  }
}

static void test_unknown_channel() {
  const char* script =
      "device 1\n"
      "session 1\n"
      "ts 1000\n"
      "session-begin label=a\n"
      "session-end reason=0\n";
  AssembleResult a = assemble_script(script);
  CHECK(a.ok());
  DecodedRecording rec;
  rec.decode(a.bytes.data(), a.bytes.size());
  ChannelDigest d = field_digest(rec.channels(), 99, 0);
  CHECK(d.samples == 0);
  CHECK(d.buckets.empty());
}

int main() {
  test_basic_histogram();
  test_all_channels();
  test_unknown_channel();
  return failures ? 1 : 0;
}
