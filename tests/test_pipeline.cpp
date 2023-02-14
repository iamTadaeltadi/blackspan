#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "blackspan/byteio.h"
#include "blackspan/channel.h"
#include "blackspan/frame.h"
#include "blackspan/pipeline.h"
#include "blackspan/schema.h"

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

namespace {

// Appends a frame with the given record type and payload to wire.
void put_frame(std::vector<uint8_t>& wire, RecordType type, uint32_t device_id,
               uint32_t seq, uint64_t ts, const std::vector<uint8_t>& payload) {
  FrameHeader h;
  h.type = type;
  h.device_id = device_id;
  h.session_seq = seq;
  h.monotonic_ts = ts;
  encode_frame(h, payload.data(), payload.size(), wire);
}

// Builds a small but complete recording for one device: begin, a one-field
// schema, a sample block of three u16 samples, a state transition, a fault, end.
std::vector<uint8_t> build_recording() {
  const uint32_t dev = 0x100;
  std::vector<uint8_t> wire;

  {  // SessionBegin
    ByteWriter p;
    p.u16(5);           // device_class
    p.u8(1);            // fw_major
    p.u8(2);            // fw_minor
    p.u16(3);           // fw_patch
    p.u32(1);           // schema_epoch
    const std::string label = "unitA";
    p.u8(static_cast<uint8_t>(label.size()));
    p.bytes(reinterpret_cast<const uint8_t*>(label.data()), label.size());
    put_frame(wire, RecordType::SessionBegin, dev, 0, 1000, p.buf());
  }
  {  // SchemaDef: channel 1, one U16 field "raw"
    ByteWriter p;
    p.u16(1);   // channel_id
    p.u8(1);    // declared_version
    p.u8(1);    // field_count
    const std::string cname = "temp";
    p.u8(static_cast<uint8_t>(cname.size()));
    p.bytes(reinterpret_cast<const uint8_t*>(cname.data()), cname.size());
    p.u8(3);    // FieldType::U16
    const std::string fname = "raw";
    p.u8(static_cast<uint8_t>(fname.size()));
    p.bytes(reinterpret_cast<const uint8_t*>(fname.data()), fname.size());
    put_frame(wire, RecordType::SchemaDef, dev, 1, 1001, p.buf());
  }
  {  // SampleBlock: 3 samples of one u16 each
    ByteWriter p;
    p.u16(1);   // channel_id
    p.u16(1);   // schema_version
    p.u32(0);   // base_ts_delta
    p.u16(3);   // sample_count
    p.u16(0);   // flags
    p.u16(100);
    p.u16(200);
    p.u16(300);
    put_frame(wire, RecordType::SampleBlock, dev, 2, 1002, p.buf());
  }
  {  // StateTransition 0 -> 1
    ByteWriter p;
    p.u16(0);
    p.u16(1);
    p.u8(0);
    p.u8(0);
    put_frame(wire, RecordType::StateTransition, dev, 3, 1003, p.buf());
  }
  {  // Fault code 42, severity 2
    ByteWriter p;
    p.u16(42);
    p.u8(2);
    p.u8(1);
    const std::string detail = "x";
    p.u8(static_cast<uint8_t>(detail.size()));
    p.bytes(reinterpret_cast<const uint8_t*>(detail.data()), detail.size());
    put_frame(wire, RecordType::Fault, dev, 4, 1004, p.buf());
  }
  {  // SessionEnd
    ByteWriter p;
    p.u16(0);
    put_frame(wire, RecordType::SessionEnd, dev, 5, 1005, p.buf());
  }
  return wire;
}

}  // namespace

static void test_full_decode() {
  const std::vector<uint8_t> wire = build_recording();
  DecodedRecording rec;
  rec.decode(wire.data(), wire.size());

  CHECK(rec.frame_count() == 6);
  CHECK(rec.final_status() == FrameStatus::Ok);

  // Sessions
  const auto& sessions = rec.sessions().sessions();
  CHECK(sessions.size() == 1);
  if (!sessions.empty()) {
    CHECK(sessions[0].device_id == 0x100);
    CHECK(sessions[0].closed);
    CHECK(sessions[0].duration_ns() == 5);  // 1005 - 1000
  }

  // Schemas
  CHECK(rec.schemas().count() == 1);
  const ChannelSchema* cs = rec.schemas().latest(1);
  CHECK(cs != nullptr);
  if (cs) CHECK(cs->stride == 2);

  // Channels
  CHECK(rec.channels().total_samples() == 3);
  const ChannelSeries* series = rec.channels().series(1);
  CHECK(series != nullptr);
  if (series) {
    CHECK(series->total_samples() == 3);
    auto v0 = series->view(0);
    auto v2 = series->view(2);
    CHECK(v0.has_value() && v0->get_u16(0).value_or(0) == 100);
    CHECK(v2.has_value() && v2->get_u16(0).value_or(0) == 300);
  }

  // Anomalies
  CHECK(rec.anomalies().faults().size() == 1);
  if (!rec.anomalies().faults().empty()) {
    CHECK(rec.anomalies().faults()[0].fault_code == 42);
  }
  CHECK(rec.anomalies().transitions().size() == 1);
}

static void test_truncated_stream() {
  std::vector<uint8_t> wire = build_recording();
  DecodedRecording rec;
  rec.decode(wire.data(), wire.size() - 3);  // chop the final frame
  CHECK(rec.frame_count() == 5);
  CHECK(rec.final_status() == FrameStatus::Incomplete);
  // The last session never saw its end, so it is left open (not closed).
  const auto& sessions = rec.sessions().sessions();
  CHECK(sessions.size() == 1);
  if (!sessions.empty()) CHECK(!sessions[0].closed);
}

static void test_empty() {
  DecodedRecording rec;
  rec.decode(nullptr, 0);
  CHECK(rec.frame_count() == 0);
  CHECK(rec.final_status() == FrameStatus::Ok);
  CHECK(rec.sessions().sessions().empty());
  CHECK(rec.channels().total_samples() == 0);
}

int main() {
  test_full_decode();
  test_truncated_stream();
  test_empty();
  if (failures) {
    std::fprintf(stderr, "%d checks failed\n", failures);
    return 1;
  }
  std::printf("all pipeline tests passed\n");
  return 0;
}
