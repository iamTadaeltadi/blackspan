#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "blackspan/assemble.h"
#include "blackspan/pipeline.h"
#include "blackspan/rank.h"
#include "blackspan/stats.h"

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

static void test_basic_ranks() {
  const char* script =
      "device 1\n"
      "session 1\n"
      "ts 1000\n"
      "session-begin label=a\n"
      "schema id=1 name=m fields=v:u16\n"
      "samples channel=1 delta=0 ts=1000\n"
      "  row 30\n"
      "  row 10\n"
      "  row 20\n"
      "end\n"
      "session-end reason=0\n";
  AssembleResult a = assemble_script(script);
  CHECK(a.ok());
  DecodedRecording rec;
  rec.decode(a.bytes.data(), a.bytes.size());

  ChannelRanks r = field_ranks(rec.channels(), 1, 0);
  CHECK(r.channel_id == 1);
  CHECK(r.field_name == "v");
  CHECK(r.samples == 3);
  CHECK(r.ranks.size() == 3);
  CHECK(r.ranks[0] == 0);
  CHECK(r.ranks[1] == 2);
  CHECK(r.ranks[2] == 1);
  CHECK(!describe_ranks(r).empty());
}

static void test_all_channels() {
  const char* script =
      "device 1\n"
      "session 1\n"
      "ts 1000\n"
      "session-begin label=a\n"
      "schema id=1 name=m fields=v:u16\n"
      "samples channel=1 delta=0 ts=1000\n"
      "  row 5\n"
      "  row 7\n"
      "end\n"
      "session-end reason=0\n";
  AssembleResult a = assemble_script(script);
  CHECK(a.ok());
  DecodedRecording rec;
  rec.decode(a.bytes.data(), a.bytes.size());

  std::vector<ChannelRanks> all = channel_ranks(rec.channels());
  CHECK(all.size() == 1);
  if (!all.empty()) {
    CHECK(all[0].samples == 2);
    CHECK(all[0].ranks.size() == 2);
    CHECK(all[0].ranks[0] == 1);
    CHECK(all[0].ranks[1] == 0);
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
  ChannelRanks r = field_ranks(rec.channels(), 42, 0);
  CHECK(r.samples == 0);
  CHECK(r.ranks.empty());
}

static void test_rank_series_consumer() {
  const char* script =
      "device 1\n"
      "session 1\n"
      "ts 1000\n"
      "session-begin label=a\n"
      "schema id=1 name=m fields=v:u16\n"
      "samples channel=1 delta=0 ts=1000\n"
      "  row 30\n"
      "  row 10\n"
      "  row 20\n"
      "end\n"
      "session-end reason=0\n";
  AssembleResult a = assemble_script(script);
  CHECK(a.ok());
  DecodedRecording rec;
  rec.decode(a.bytes.data(), a.bytes.size());

  ChannelRanks r = field_ranks(rec.channels(), 1, 0);
  std::vector<uint32_t> series = rank_series(r);
  CHECK(series.size() == r.samples + 2);
  if (series.size() >= 3) {
    CHECK(series[0] == r.ranks[0]);
    CHECK(series[1] == r.ranks[1]);
    CHECK(series[2] == r.ranks[2]);
  }
}

int main() {
  test_basic_ranks();
  test_all_channels();
  test_unknown_channel();
  test_rank_series_consumer();
  return failures ? 1 : 0;
}
