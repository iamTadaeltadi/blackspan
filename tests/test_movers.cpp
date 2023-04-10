#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "blackspan/assemble.h"
#include "blackspan/movers.h"
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

static void test_basic_movers() {
  const char* script =
      "device 1\n"
      "session 1\n"
      "ts 1000\n"
      "session-begin label=a\n"
      "schema id=1 name=m fields=v:u16\n"
      "samples channel=1 delta=0 ts=1000\n"
      "  row 10\n"
      "  row 20\n"
      "  row 15\n"
      "  row 40\n"
      "  row 5\n"
      "end\n"
      "session-end reason=0\n";
  AssembleResult a = assemble_script(script);
  CHECK(a.ok());
  DecodedRecording rec;
  rec.decode(a.bytes.data(), a.bytes.size());

  ChannelMovers m = field_movers(rec.channels(), 1, 0, 256);
  CHECK(m.channel_id == 1);
  CHECK(m.field_name == "v");
  CHECK(m.samples == 5);
  CHECK(m.movers.size() == 4);
  for (size_t i = 1; i < m.movers.size(); ++i) {
    CHECK(m.movers[i - 1].position < m.movers[i].position);
  }
  CHECK(!describe_movers(m).empty());
}

static void test_all_channels() {
  const char* script =
      "device 1\n"
      "session 1\n"
      "ts 1000\n"
      "session-begin label=a\n"
      "schema id=1 name=m fields=v:u16\n"
      "samples channel=1 delta=0 ts=1000\n"
      "  row 1\n"
      "  row 100\n"
      "  row 2\n"
      "end\n"
      "session-end reason=0\n";
  AssembleResult a = assemble_script(script);
  CHECK(a.ok());
  DecodedRecording rec;
  rec.decode(a.bytes.data(), a.bytes.size());

  std::vector<ChannelMovers> all = channel_movers(rec.channels());
  CHECK(all.size() == 1);
  if (!all.empty()) {
    CHECK(all[0].samples == 3);
    CHECK(all[0].movers.size() == 2);
  }
}

static void test_short_channel() {
  const char* script =
      "device 1\n"
      "session 1\n"
      "ts 1000\n"
      "session-begin label=a\n"
      "schema id=1 name=m fields=v:u16\n"
      "samples channel=1 delta=0 ts=1000\n"
      "  row 7\n"
      "end\n"
      "session-end reason=0\n";
  AssembleResult a = assemble_script(script);
  CHECK(a.ok());
  DecodedRecording rec;
  rec.decode(a.bytes.data(), a.bytes.size());
  ChannelMovers m = field_movers(rec.channels(), 1, 0, 256);
  CHECK(m.samples == 1);
  CHECK(m.movers.empty());
}

int main() {
  test_basic_movers();
  test_all_channels();
  test_short_channel();
  return failures ? 1 : 0;
}
