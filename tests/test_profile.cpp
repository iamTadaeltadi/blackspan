#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "blackspan/assemble.h"
#include "blackspan/pipeline.h"
#include "blackspan/profile.h"
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

static void test_activity_and_consumer() {
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

  ActivityProfile p = field_activity(rec.channels(), 1, 0);
  CHECK(p.channel_id == 1);
  CHECK(p.field_name == "v");
  CHECK(p.samples == 3);
  CHECK(std::fabs(p.mean - 20.0) < 1e-9);
  CHECK(p.level.size() == 3);
  CHECK(!describe_activity(p).empty());

  double dev = activity_deviation(p);
  CHECK(std::fabs(dev - 20.0) < 1e-6);
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
  ActivityProfile p = field_activity(rec.channels(), 7, 0);
  CHECK(p.samples == 0);
  CHECK(p.level.empty());
  CHECK(activity_deviation(p) == 0.0);
}

int main() {
  test_activity_and_consumer();
  test_unknown_channel();
  return failures ? 1 : 0;
}
