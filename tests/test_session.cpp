#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "blackspan/byteio.h"
#include "blackspan/frame.h"
#include "blackspan/session.h"

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

// ---- recorder-stream builders -------------------------------------------

static void put_frame(std::vector<uint8_t>& wire, RecordType type,
                      uint32_t dev, uint32_t seq, uint64_t ts,
                      const std::vector<uint8_t>& payload) {
  FrameHeader h;
  h.type = type;
  h.device_id = dev;
  h.session_seq = seq;
  h.monotonic_ts = ts;
  encode_frame(h, payload.data(), payload.size(), wire);
}

static void put_begin(std::vector<uint8_t>& wire, uint32_t dev, uint32_t seq,
                      uint64_t ts, uint16_t dclass, uint8_t maj, uint8_t min,
                      uint16_t patch, uint32_t epoch, const std::string& label) {
  ByteWriter w;
  w.u16(dclass);
  w.u8(maj);
  w.u8(min);
  w.u16(patch);
  w.u32(epoch);
  w.u8(static_cast<uint8_t>(label.size()));
  w.bytes(reinterpret_cast<const uint8_t*>(label.data()), label.size());
  put_frame(wire, RecordType::SessionBegin, dev, seq, ts, w.buf());
}

static void put_end(std::vector<uint8_t>& wire, uint32_t dev, uint32_t seq,
                    uint64_t ts, uint16_t reason) {
  ByteWriter w;
  w.u16(reason);
  put_frame(wire, RecordType::SessionEnd, dev, seq, ts, w.buf());
}

static void put_checkpoint(std::vector<uint8_t>& wire, uint32_t dev,
                           uint32_t seq, uint64_t ts, uint32_t id,
                           uint32_t bytes_since) {
  ByteWriter w;
  w.u32(id);
  w.u32(bytes_since);
  put_frame(wire, RecordType::Checkpoint, dev, seq, ts, w.buf());
}

static void put_reconnect(std::vector<uint8_t>& wire, uint32_t dev,
                          uint32_t seq, uint64_t ts, uint32_t gap_ms,
                          uint32_t prev_seq) {
  ByteWriter w;
  w.u32(gap_ms);
  w.u32(prev_seq);
  put_frame(wire, RecordType::Reconnect, dev, seq, ts, w.buf());
}

static void put_marker(std::vector<uint8_t>& wire, uint32_t dev, uint32_t seq,
                       uint64_t ts, uint8_t kind, const std::string& text) {
  ByteWriter w;
  w.u8(kind);
  w.u8(static_cast<uint8_t>(text.size()));
  w.bytes(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  put_frame(wire, RecordType::Marker, dev, seq, ts, w.buf());
}

// ---- diagnostic helpers ---------------------------------------------------

static size_t count_diag(const SessionReconstructor& r, SessionDiagCode c) {
  size_t n = 0;
  for (const auto& d : r.diagnostics())
    if (d.code == c) ++n;
  return n;
}

// ---- tests ----------------------------------------------------------------

static void test_single_normal_session() {
  std::vector<uint8_t> wire;
  put_begin(wire, 0xABCD, 0, 1000, 42, 3, 7, 100, 0xDEADBEEF, "flight-01");
  put_marker(wire, 0xABCD, 1, 1100, 2, "takeoff");
  // A record owned by another subsystem still flows through in order.
  put_frame(wire, RecordType::SampleBlock, 0xABCD, 2, 1200, {1, 2, 3});
  put_checkpoint(wire, 0xABCD, 3, 1300, 9, 4096);
  put_end(wire, 0xABCD, 4, 2000, 5);

  SessionReconstructor r;
  r.reconstruct(wire.data(), wire.size());

  CHECK(r.sessions().size() == 1);
  CHECK(r.diagnostics().empty());
  const Session& s = r.sessions()[0];
  CHECK(s.device_id == 0xABCD);
  CHECK(s.session_index == 0);
  CHECK(s.device_class == 42);
  CHECK(s.fw_major == 3 && s.fw_minor == 7 && s.fw_patch == 100);
  CHECK(s.schema_epoch == 0xDEADBEEF);
  CHECK(s.label == "flight-01");
  CHECK(s.begin_seq == 0 && s.begin_ts == 1000);
  CHECK(s.end_seq == 4 && s.end_ts == 2000);
  CHECK(s.end_reason == 5);
  CHECK(s.closed);
  CHECK(s.duration_ns() == 1000);
  CHECK(s.markers.size() == 1 && s.markers[0].marker_kind == 2 &&
        s.markers[0].text == "takeoff");
  CHECK(s.checkpoints.size() == 1 && s.checkpoints[0].checkpoint_id == 9 &&
        s.checkpoints[0].bytes_since_last == 4096);
  CHECK(s.reconnects.empty());
  CHECK(s.reconnect_count == 0);
  CHECK(s.seq_regressions == 0 && s.ts_regressions == 0);
}

static void test_multiple_sessions_per_device() {
  std::vector<uint8_t> wire;
  put_begin(wire, 7, 0, 10, 1, 0, 0, 0, 1, "a");
  put_end(wire, 7, 1, 20, 0);
  put_begin(wire, 7, 2, 30, 1, 0, 0, 0, 2, "b");
  put_end(wire, 7, 3, 55, 0);

  SessionReconstructor r;
  r.reconstruct(wire.data(), wire.size());

  CHECK(r.sessions().size() == 2);
  CHECK(r.diagnostics().empty());
  CHECK(r.sessions()[0].session_index == 0);
  CHECK(r.sessions()[0].label == "a");
  CHECK(r.sessions()[0].duration_ns() == 10);
  CHECK(r.sessions()[1].session_index == 1);
  CHECK(r.sessions()[1].label == "b");
  CHECK(r.sessions()[1].duration_ns() == 25);
  CHECK(r.sessions()[0].closed && r.sessions()[1].closed);
}

static void test_multiple_devices_interleaved() {
  std::vector<uint8_t> wire;
  put_begin(wire, 100, 0, 10, 1, 0, 0, 0, 0, "devA-s0");
  put_begin(wire, 200, 0, 11, 2, 0, 0, 0, 0, "devB-s0");
  put_checkpoint(wire, 100, 1, 12, 1, 10);
  put_marker(wire, 200, 1, 13, 0, "b-mark");
  put_end(wire, 100, 2, 14, 0);
  put_begin(wire, 100, 3, 15, 1, 0, 0, 0, 0, "devA-s1");
  put_end(wire, 200, 2, 16, 0);
  put_end(wire, 100, 4, 17, 0);

  SessionReconstructor r;
  r.reconstruct(wire.data(), wire.size());

  CHECK(r.sessions().size() == 3);
  CHECK(r.diagnostics().empty());

  // Sessions are created in stream order: devA#0, devB#0, devA#1.
  const Session& a0 = r.sessions()[0];
  const Session& b0 = r.sessions()[1];
  const Session& a1 = r.sessions()[2];
  CHECK(a0.device_id == 100 && a0.session_index == 0 && a0.label == "devA-s0");
  CHECK(a0.checkpoints.size() == 1);
  CHECK(b0.device_id == 200 && b0.session_index == 0 && b0.label == "devB-s0");
  CHECK(b0.markers.size() == 1 && b0.markers[0].text == "b-mark");
  CHECK(a1.device_id == 100 && a1.session_index == 1 && a1.label == "devA-s1");
  CHECK(a0.closed && b0.closed && a1.closed);

  SessionSummary sum = r.summary();
  CHECK(sum.device_count == 2);
  CHECK(sum.total_sessions == 3 && sum.closed_sessions == 3);
  CHECK(sum.total_checkpoints == 1 && sum.total_markers == 1);
}

static void test_checkpoints_and_reconnects_attached() {
  std::vector<uint8_t> wire;
  put_begin(wire, 1, 0, 100, 1, 0, 0, 0, 0, "sess");
  put_checkpoint(wire, 1, 1, 110, 11, 1000);
  put_reconnect(wire, 1, 2, 130, 20, 99);
  put_checkpoint(wire, 1, 3, 140, 12, 2000);
  put_reconnect(wire, 1, 4, 175, 5, 3);
  put_end(wire, 1, 5, 200, 0);

  SessionReconstructor r;
  r.reconstruct(wire.data(), wire.size());

  CHECK(r.sessions().size() == 1);
  const Session& s = r.sessions()[0];
  CHECK(s.checkpoints.size() == 2);
  CHECK(s.checkpoints[0].checkpoint_id == 11 &&
        s.checkpoints[0].bytes_since_last == 1000);
  CHECK(s.checkpoints[1].checkpoint_id == 12 &&
        s.checkpoints[1].bytes_since_last == 2000);
  CHECK(s.reconnects.size() == 2);
  CHECK(s.reconnect_count == 2);
  CHECK(s.reconnects[0].gap_ms == 20 && s.reconnects[0].prev_session_seq == 99);
  CHECK(s.reconnects[0].at_seq == 2 && s.reconnects[0].at_ts == 130);
  CHECK(s.reconnects[1].gap_ms == 5 && s.reconnects[1].prev_session_seq == 3);
  CHECK(s.reconnects[1].at_seq == 4 && s.reconnects[1].at_ts == 175);
  CHECK(r.diagnostics().empty());
}

static void test_end_without_begin() {
  std::vector<uint8_t> wire;
  put_end(wire, 5, 0, 100, 0);

  SessionReconstructor r;
  r.reconstruct(wire.data(), wire.size());

  CHECK(r.sessions().empty());
  CHECK(count_diag(r, SessionDiagCode::EndWithoutBegin) == 1);
}

static void test_begin_while_active() {
  std::vector<uint8_t> wire;
  put_begin(wire, 9, 0, 100, 1, 0, 0, 0, 0, "first");
  put_marker(wire, 9, 1, 150, 0, "m");
  put_begin(wire, 9, 2, 200, 1, 0, 0, 0, 0, "second");
  put_end(wire, 9, 3, 300, 0);

  SessionReconstructor r;
  r.reconstruct(wire.data(), wire.size());

  CHECK(r.sessions().size() == 2);
  CHECK(count_diag(r, SessionDiagCode::BeginWhileActive) == 1);

  const Session& first = r.sessions()[0];
  const Session& second = r.sessions()[1];
  CHECK(first.label == "first");
  CHECK(!first.closed);              // auto-closed as unterminated
  CHECK(first.duration_ns() == 0);   // duration only for properly closed
  CHECK(first.end_seq == 1 && first.end_ts == 150);  // last observed record
  CHECK(first.markers.size() == 1);
  CHECK(second.label == "second");
  CHECK(second.session_index == 1);
  CHECK(second.closed);
  CHECK(second.duration_ns() == 100);
}

static void test_idle_records() {
  std::vector<uint8_t> wire;
  put_reconnect(wire, 3, 0, 100, 10, 0);
  put_checkpoint(wire, 3, 1, 110, 1, 2);
  put_marker(wire, 3, 2, 120, 0, "orphan");

  SessionReconstructor r;
  r.reconstruct(wire.data(), wire.size());

  CHECK(r.sessions().empty());
  CHECK(count_diag(r, SessionDiagCode::ReconnectWhileIdle) == 1);
  CHECK(count_diag(r, SessionDiagCode::CheckpointWhileIdle) == 1);
  CHECK(count_diag(r, SessionDiagCode::MarkerWhileIdle) == 1);
}

static void test_seq_regression() {
  std::vector<uint8_t> wire;
  put_begin(wire, 1, 100, 1000, 1, 0, 0, 0, 0, "s");
  put_marker(wire, 1, 50, 1100, 0, "back");  // seq 50 < 100
  put_marker(wire, 1, 60, 1200, 0, "fwd");   // seq 60 >= 50, no regression
  put_end(wire, 1, 200, 1300, 0);

  SessionReconstructor r;
  r.reconstruct(wire.data(), wire.size());

  CHECK(r.sessions().size() == 1);
  const Session& s = r.sessions()[0];
  CHECK(s.seq_regressions == 1);
  CHECK(s.ts_regressions == 0);
  CHECK(count_diag(r, SessionDiagCode::SeqRegression) == 1);
}

static void test_ts_regression() {
  std::vector<uint8_t> wire;
  put_begin(wire, 1, 0, 1000, 1, 0, 0, 0, 0, "s");
  put_checkpoint(wire, 1, 1, 500, 7, 0);  // ts 500 < 1000
  put_end(wire, 1, 2, 2000, 0);

  SessionReconstructor r;
  r.reconstruct(wire.data(), wire.size());

  CHECK(r.sessions().size() == 1);
  const Session& s = r.sessions()[0];
  CHECK(s.ts_regressions == 1);
  CHECK(s.seq_regressions == 0);
  CHECK(count_diag(r, SessionDiagCode::TsRegression) == 1);
}

static void test_unterminated_trailing() {
  std::vector<uint8_t> wire;
  put_begin(wire, 1, 0, 10, 1, 0, 0, 0, 0, "done");
  put_end(wire, 1, 1, 20, 0);
  put_begin(wire, 1, 2, 30, 1, 0, 0, 0, 0, "open");
  put_checkpoint(wire, 1, 3, 40, 1, 8);

  SessionReconstructor r;
  r.reconstruct(wire.data(), wire.size());

  CHECK(r.sessions().size() == 2);
  CHECK(r.sessions()[0].closed);
  const Session& open = r.sessions()[1];
  CHECK(!open.closed);
  CHECK(open.duration_ns() == 0);
  CHECK(open.end_seq == 3 && open.end_ts == 40);  // last observed record
  CHECK(count_diag(r, SessionDiagCode::UnterminatedSession) == 1);

  SessionSummary sum = r.summary();
  CHECK(sum.closed_sessions == 1 && sum.unterminated_sessions == 1);
}

static void test_unknown_record() {
  std::vector<uint8_t> wire;
  put_begin(wire, 1, 0, 10, 1, 0, 0, 0, 0, "s");
  put_frame(wire, static_cast<RecordType>(0x7F), 1, 1, 20, {9, 9});
  put_end(wire, 1, 2, 30, 0);

  SessionReconstructor r;
  r.reconstruct(wire.data(), wire.size());

  CHECK(r.sessions().size() == 1);
  CHECK(r.sessions()[0].closed);  // unknown record does not disturb the session
  CHECK(count_diag(r, SessionDiagCode::UnknownRecordInSession) == 1);
}

static void test_decoders_round_trip() {
  {
    ByteWriter w;
    w.u16(42);
    w.u8(1);
    w.u8(2);
    w.u16(3);
    w.u32(0xCAFEBABE);
    w.u8(3);
    const char* lbl = "abc";
    w.bytes(reinterpret_cast<const uint8_t*>(lbl), 3);
    SessionBegin b;
    CHECK(decode_session_begin(w.buf().data(), w.buf().size(), b) ==
          PayloadStatus::Ok);
    CHECK(b.device_class == 42 && b.fw_major == 1 && b.fw_minor == 2 &&
          b.fw_patch == 3 && b.schema_epoch == 0xCAFEBABE && b.label == "abc");
  }
  {
    ByteWriter w;
    w.u16(7);
    SessionEnd e;
    CHECK(decode_session_end(w.buf().data(), w.buf().size(), e) ==
          PayloadStatus::Ok);
    CHECK(e.reason_code == 7);
  }
  {
    ByteWriter w;
    w.u32(11);
    w.u32(22);
    Checkpoint c;
    CHECK(decode_checkpoint(w.buf().data(), w.buf().size(), c) ==
          PayloadStatus::Ok);
    CHECK(c.checkpoint_id == 11 && c.bytes_since_last == 22);
  }
  {
    ByteWriter w;
    w.u32(33);
    w.u32(44);
    Reconnect rc;
    CHECK(decode_reconnect(w.buf().data(), w.buf().size(), rc) ==
          PayloadStatus::Ok);
    CHECK(rc.gap_ms == 33 && rc.prev_session_seq == 44);
  }
  {
    ByteWriter w;
    w.u8(9);
    w.u8(5);
    const char* t = "hello";
    w.bytes(reinterpret_cast<const uint8_t*>(t), 5);
    Marker m;
    CHECK(decode_marker(w.buf().data(), w.buf().size(), m) == PayloadStatus::Ok);
    CHECK(m.marker_kind == 9 && m.text == "hello");
  }
}

static void test_truncated_decoders() {
  // Empty buffers.
  SessionBegin b;
  CHECK(decode_session_begin(nullptr, 0, b) == PayloadStatus::Truncated);
  SessionEnd e;
  CHECK(decode_session_end(nullptr, 0, e) == PayloadStatus::Truncated);
  Checkpoint c;
  CHECK(decode_checkpoint(nullptr, 0, c) == PayloadStatus::Truncated);
  Reconnect rc;
  CHECK(decode_reconnect(nullptr, 0, rc) == PayloadStatus::Truncated);
  Marker m;
  CHECK(decode_marker(nullptr, 0, m) == PayloadStatus::Truncated);

  // Fixed fields present but declared label longer than available bytes:
  // whatever is present is still captured.
  {
    ByteWriter w;
    w.u16(1);
    w.u8(0);
    w.u8(0);
    w.u16(0);
    w.u32(0);
    w.u8(10);  // claims 10 label bytes
    const char* lbl = "xy";
    w.bytes(reinterpret_cast<const uint8_t*>(lbl), 2);  // only 2 present
    SessionBegin partial;
    CHECK(decode_session_begin(w.buf().data(), w.buf().size(), partial) ==
          PayloadStatus::Truncated);
    CHECK(partial.label == "xy");
  }

  // Marker with a text_len longer than the bytes present.
  {
    ByteWriter w;
    w.u8(1);
    w.u8(4);  // claims 4 text bytes
    const char* t = "z";
    w.bytes(reinterpret_cast<const uint8_t*>(t), 1);
    Marker partial;
    CHECK(decode_marker(w.buf().data(), w.buf().size(), partial) ==
          PayloadStatus::Truncated);
    CHECK(partial.text == "z");
  }

  // Checkpoint with only 5 of 8 bytes.
  {
    ByteWriter w;
    w.u32(1);
    w.u8(0);
    Checkpoint partial;
    CHECK(decode_checkpoint(w.buf().data(), w.buf().size(), partial) ==
          PayloadStatus::Truncated);
    CHECK(partial.checkpoint_id == 1);
  }
}

static void test_truncated_payload_through_reconstructor() {
  // A session-begin whose label is truncated must still open a session and be
  // flagged, never dropped.
  std::vector<uint8_t> wire;
  {
    ByteWriter w;
    w.u16(5);
    w.u8(1);
    w.u8(0);
    w.u16(0);
    w.u32(0);
    w.u8(8);  // claims 8 label bytes
    const char* lbl = "hi";
    w.bytes(reinterpret_cast<const uint8_t*>(lbl), 2);
    put_frame(wire, RecordType::SessionBegin, 1, 0, 100, w.buf());
  }
  put_end(wire, 1, 1, 200, 0);

  SessionReconstructor r;
  r.reconstruct(wire.data(), wire.size());

  CHECK(r.sessions().size() == 1);
  CHECK(r.sessions()[0].device_class == 5);
  CHECK(r.sessions()[0].label == "hi");
  CHECK(r.sessions()[0].closed);
  CHECK(count_diag(r, SessionDiagCode::TruncatedPayload) == 1);
}

static void test_describe_and_summary() {
  std::vector<uint8_t> wire;
  put_begin(wire, 0x1234, 0, 1000, 3, 1, 2, 3, 77, "route-9");
  put_checkpoint(wire, 0x1234, 1, 1100, 1, 512);
  put_reconnect(wire, 0x1234, 2, 1200, 30, 0);
  put_end(wire, 0x1234, 3, 3000, 0);

  SessionReconstructor r;
  r.reconstruct(wire.data(), wire.size());

  CHECK(r.sessions().size() == 1);
  const std::string desc = describe_session(r.sessions()[0]);
  CHECK(desc.find("route-9") != std::string::npos);
  CHECK(desc.find("session #0") != std::string::npos);
  CHECK(desc.find("2000ns") != std::string::npos);
  CHECK(desc.find("ended") != std::string::npos);

  const std::string ssum = describe_summary(r.summary());
  CHECK(!ssum.empty());
  CHECK(ssum.find("1 sessions") != std::string::npos);
}

static void test_status_strings() {
  CHECK(std::string(payload_status_str(PayloadStatus::Ok)) == "ok");
  CHECK(std::string(payload_status_str(PayloadStatus::Truncated)) ==
        "truncated");
  CHECK(std::string(session_diag_str(SessionDiagCode::EndWithoutBegin)) ==
        "end-without-begin");
  CHECK(std::string(session_diag_str(SessionDiagCode::UnterminatedSession)) ==
        "unterminated-session");
}

int main() {
  test_single_normal_session();
  test_multiple_sessions_per_device();
  test_multiple_devices_interleaved();
  test_checkpoints_and_reconnects_attached();
  test_end_without_begin();
  test_begin_while_active();
  test_idle_records();
  test_seq_regression();
  test_ts_regression();
  test_unterminated_trailing();
  test_unknown_record();
  test_decoders_round_trip();
  test_truncated_decoders();
  test_truncated_payload_through_reconstructor();
  test_describe_and_summary();
  test_status_strings();
  if (failures) {
    std::fprintf(stderr, "%d checks failed\n", failures);
    return 1;
  }
  std::printf("all session tests passed\n");
  return 0;
}
