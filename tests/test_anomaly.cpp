#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "blackspan/anomaly.h"
#include "blackspan/byteio.h"
#include "blackspan/frame.h"

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

static void put_frame(std::vector<uint8_t>& wire, RecordType type, uint32_t dev,
                      uint32_t seq, uint64_t ts,
                      const std::vector<uint8_t>& payload) {
  FrameHeader h;
  h.type = type;
  h.device_id = dev;
  h.session_seq = seq;
  h.monotonic_ts = ts;
  encode_frame(h, payload.data(), payload.size(), wire);
}

static std::vector<uint8_t> transition_payload(uint16_t from, uint16_t to,
                                               uint8_t cause,
                                               uint8_t reserved = 0) {
  ByteWriter w;
  w.u16(from);
  w.u16(to);
  w.u8(cause);
  w.u8(reserved);
  return w.buf();
}

static std::vector<uint8_t> config_payload(uint16_t param, uint32_t oldv,
                                           uint32_t newv) {
  ByteWriter w;
  w.u16(param);
  w.u32(oldv);
  w.u32(newv);
  return w.buf();
}

static std::vector<uint8_t> fault_payload(uint16_t code, uint8_t sev,
                                          uint8_t subsys,
                                          const std::vector<uint8_t>& detail) {
  ByteWriter w;
  w.u16(code);
  w.u8(sev);
  w.u8(subsys);
  w.u8(static_cast<uint8_t>(detail.size()));
  w.bytes(detail);
  return w.buf();
}

static void put_transition(std::vector<uint8_t>& wire, uint32_t dev,
                           uint32_t seq, uint64_t ts, uint16_t from, uint16_t to,
                           uint8_t cause) {
  put_frame(wire, RecordType::StateTransition, dev, seq, ts,
            transition_payload(from, to, cause));
}

static void put_config(std::vector<uint8_t>& wire, uint32_t dev, uint32_t seq,
                       uint64_t ts, uint16_t param, uint32_t oldv,
                       uint32_t newv) {
  put_frame(wire, RecordType::ConfigChange, dev, seq, ts,
            config_payload(param, oldv, newv));
}

static void put_fault(std::vector<uint8_t>& wire, uint32_t dev, uint32_t seq,
                      uint64_t ts, uint16_t code, uint8_t sev, uint8_t subsys,
                      const std::vector<uint8_t>& detail) {
  put_frame(wire, RecordType::Fault, dev, seq, ts,
            fault_payload(code, sev, subsys, detail));
}

static size_t count_diag(const AnomalyEngine& e, AnomalyDiagCode c) {
  size_t n = 0;
  for (const auto& d : e.diagnostics())
    if (d.code == c) ++n;
  return n;
}

// ---- tests ----------------------------------------------------------------

static void test_decoders_round_trip() {
  {
    const auto p = transition_payload(1, 2, 3, 4);
    StateTransition st;
    CHECK(decode_state_transition(p.data(), p.size(), st) == DecodeStatus::Ok);
    CHECK(st.from_state == 1 && st.to_state == 2 && st.cause == 3 &&
          st.reserved == 4);
  }
  {
    const auto p = config_payload(0x1122, 0xDEADBEEF, 0x0BADF00D);
    ConfigChange cc;
    CHECK(decode_config_change(p.data(), p.size(), cc) == DecodeStatus::Ok);
    CHECK(cc.param_id == 0x1122 && cc.old_value == 0xDEADBEEF &&
          cc.new_value == 0x0BADF00D);
  }
  {
    const std::vector<uint8_t> detail = {9, 8, 7, 6};
    const auto p = fault_payload(0x0505, 2, 11, detail);
    Fault fx;
    CHECK(decode_fault(p.data(), p.size(), fx) == DecodeStatus::Ok);
    CHECK(fx.fault_code == 0x0505 && fx.severity == Severity::Error &&
          fx.subsystem_id == 11 && fx.detail == detail);
  }
}

static void test_decoders_truncation() {
  // Empty buffers report truncation for every record type.
  StateTransition st;
  CHECK(decode_state_transition(nullptr, 0, st) == DecodeStatus::Truncated);
  ConfigChange cc;
  CHECK(decode_config_change(nullptr, 0, cc) == DecodeStatus::Truncated);
  Fault fx;
  CHECK(decode_fault(nullptr, 0, fx) == DecodeStatus::Truncated);

  // State-transition with only 3 of 6 bytes: the first field survives.
  {
    ByteWriter w;
    w.u16(0x1234);
    w.u8(0x56);
    StateTransition partial;
    CHECK(decode_state_transition(w.buf().data(), w.buf().size(), partial) ==
          DecodeStatus::Truncated);
    CHECK(partial.from_state == 0x1234);
  }
  // Config-change with only 5 of 10 bytes: param_id survives.
  {
    ByteWriter w;
    w.u16(77);
    w.u16(0);
    w.u8(0);
    ConfigChange partial;
    CHECK(decode_config_change(w.buf().data(), w.buf().size(), partial) ==
          DecodeStatus::Truncated);
    CHECK(partial.param_id == 77);
  }
  // Fault whose detail_len claims more bytes than are present: whatever detail
  // bytes exist are still captured.
  {
    ByteWriter w;
    w.u16(0xABCD);
    w.u8(3);   // severity Critical
    w.u8(5);   // subsystem
    w.u8(10);  // claims 10 detail bytes
    const std::vector<uint8_t> present = {1, 2, 3};
    w.bytes(present);
    Fault partial;
    CHECK(decode_fault(w.buf().data(), w.buf().size(), partial) ==
          DecodeStatus::Truncated);
    CHECK(partial.fault_code == 0xABCD && partial.severity == Severity::Critical);
    CHECK(partial.subsystem_id == 5 && partial.detail == present);
  }
  // Fault truncated before the detail-length byte.
  {
    ByteWriter w;
    w.u16(0x1);
    w.u8(1);
    Fault partial;
    CHECK(decode_fault(w.buf().data(), w.buf().size(), partial) ==
          DecodeStatus::Truncated);
    CHECK(partial.fault_code == 0x1);
  }
}

static void test_transition_chain_no_desync() {
  std::vector<uint8_t> wire;
  put_transition(wire, 1, 0, 100, 0, 1, 0);
  put_transition(wire, 1, 1, 110, 1, 2, 0);
  put_transition(wire, 1, 2, 120, 2, 3, 0);

  AnomalyEngine e;
  e.ingest(wire.data(), wire.size());

  CHECK(e.transitions().size() == 3);
  CHECK(count_diag(e, AnomalyDiagCode::StateDesync) == 0);
  CHECK(e.current_state(1).has_value() && e.current_state(1).value() == 3);
  for (const auto& t : e.transitions()) CHECK(t.continues_chain);
  // An unseen device has no known state.
  CHECK(!e.current_state(999).has_value());
}

static void test_transition_chain_desync() {
  std::vector<uint8_t> wire;
  put_transition(wire, 1, 0, 100, 0, 1, 0);
  put_transition(wire, 1, 1, 110, 5, 2, 0);  // from_state 5 != last to_state 1

  AnomalyEngine e;
  e.ingest(wire.data(), wire.size());

  CHECK(e.transitions().size() == 2);
  CHECK(count_diag(e, AnomalyDiagCode::StateDesync) == 1);
  CHECK(e.transitions()[0].continues_chain == true);   // first continues
  CHECK(e.transitions()[1].continues_chain == false);  // second broke chain
  // The broken transition still advances the state to its to_state.
  CHECK(e.current_state(1).has_value() && e.current_state(1).value() == 2);
}

static void test_fault_state_tagging() {
  std::vector<uint8_t> wire;
  // Fault before any transition: state unknown.
  put_fault(wire, 1, 0, 100, 0x10, 1, 0, {});
  put_transition(wire, 1, 1, 110, 0, 10, 0);
  put_fault(wire, 1, 2, 120, 0x11, 2, 0, {});  // state 10
  put_transition(wire, 1, 3, 130, 10, 20, 0);
  put_fault(wire, 1, 4, 140, 0x12, 3, 0, {});  // state 20

  AnomalyEngine e;
  e.ingest(wire.data(), wire.size());

  CHECK(e.faults().size() == 3);
  CHECK(!e.faults()[0].state_at_fault.has_value());
  CHECK(e.faults()[1].state_at_fault.has_value() &&
        e.faults()[1].state_at_fault.value() == 10);
  CHECK(e.faults()[2].state_at_fault.has_value() &&
        e.faults()[2].state_at_fault.value() == 20);
}

static void test_faults_in_state() {
  std::vector<uint8_t> wire;
  put_transition(wire, 1, 0, 100, 0, 7, 0);
  put_fault(wire, 1, 1, 110, 0xA, 1, 0, {});  // state 7
  put_fault(wire, 1, 2, 120, 0xB, 1, 0, {});  // state 7
  put_transition(wire, 1, 3, 130, 7, 9, 0);
  put_fault(wire, 1, 4, 140, 0xC, 1, 0, {});  // state 9

  AnomalyEngine e;
  e.ingest(wire.data(), wire.size());

  const auto in7 = e.faults_in_state(7);
  const auto in9 = e.faults_in_state(9);
  const auto in3 = e.faults_in_state(3);
  CHECK(in7.size() == 2);
  CHECK(in7[0]->fault_code == 0xA && in7[1]->fault_code == 0xB);
  CHECK(in9.size() == 1 && in9[0]->fault_code == 0xC);
  CHECK(in3.empty());
}

static void test_severity_counts_and_clamping() {
  std::vector<uint8_t> wire;
  put_fault(wire, 1, 0, 100, 1, 0, 0, {});    // Info
  put_fault(wire, 1, 1, 101, 2, 1, 0, {});    // Warning
  put_fault(wire, 1, 2, 102, 3, 2, 0, {});    // Error
  put_fault(wire, 1, 3, 103, 4, 3, 0, {});    // Critical
  put_fault(wire, 1, 4, 104, 5, 200, 0, {});  // out-of-range -> Critical

  AnomalyEngine e;
  e.ingest(wire.data(), wire.size());

  const std::array<size_t, 4> counts = e.fault_count_by_severity();
  CHECK(counts[0] == 1);  // Info
  CHECK(counts[1] == 1);  // Warning
  CHECK(counts[2] == 1);  // Error
  CHECK(counts[3] == 2);  // Critical + clamped
  // The clamped fault carries Critical.
  CHECK(e.faults()[4].severity == Severity::Critical);
  // Direct clamp helper.
  CHECK(severity_from_byte(0) == Severity::Info);
  CHECK(severity_from_byte(3) == Severity::Critical);
  CHECK(severity_from_byte(255) == Severity::Critical);
}

static void test_fault_burst() {
  std::vector<uint8_t> wire;
  // Device 1: four faults clustered within 300ns, then a lone fault far away.
  put_fault(wire, 1, 0, 1000, 1, 2, 0, {});
  put_fault(wire, 1, 1, 1100, 1, 2, 0, {});
  put_fault(wire, 1, 2, 1200, 1, 2, 0, {});
  put_fault(wire, 1, 3, 1300, 1, 2, 0, {});
  put_fault(wire, 1, 4, 5000, 1, 2, 0, {});
  // Device 2: four faults spread 1000ns apart -> never a burst.
  put_fault(wire, 2, 0, 1000, 1, 2, 0, {});
  put_fault(wire, 2, 1, 2000, 1, 2, 0, {});
  put_fault(wire, 2, 2, 3000, 1, 2, 0, {});
  put_fault(wire, 2, 3, 4000, 1, 2, 0, {});

  AnomalyEngine e;
  e.ingest(wire.data(), wire.size());

  const auto bursts = e.burst_windows(500, 3);
  CHECK(bursts.size() == 1);
  if (bursts.size() == 1) {
    const BurstWindow& b = bursts[0];
    CHECK(b.device_id == 1);
    CHECK(b.count == 4);
    CHECK(b.first_ts == 1000 && b.last_ts == 1300);
    CHECK(b.first_seq == 0 && b.last_seq == 3);
    CHECK(b.member_indices.size() == 4);
  }

  // A wider min_count than the cluster size finds nothing.
  CHECK(e.burst_windows(500, 5).empty());
  // A window wide enough to swallow device 1's lone late fault: its window now
  // covers all five faults. (Device 2's spread also fits such a wide window.)
  const auto wide = e.burst_windows(5000, 3);
  bool found_dev1 = false;
  for (const auto& b : wide) {
    if (b.device_id == 1) {
      found_dev1 = true;
      CHECK(b.count == 5);
    }
  }
  CHECK(found_dev1);
}

static void test_multi_device_independence() {
  std::vector<uint8_t> wire;
  // Interleave two devices; each keeps its own state chain.
  put_transition(wire, 100, 0, 10, 0, 1, 0);
  put_transition(wire, 200, 0, 11, 0, 5, 0);
  put_transition(wire, 100, 1, 12, 1, 2, 0);   // continues dev 100
  put_transition(wire, 200, 1, 13, 9, 6, 0);   // desync on dev 200 only
  put_fault(wire, 100, 2, 14, 0xA, 1, 0, {});  // dev 100 in state 2
  put_fault(wire, 200, 2, 15, 0xB, 1, 0, {});  // dev 200 in state 6

  AnomalyEngine e;
  e.ingest(wire.data(), wire.size());

  CHECK(e.current_state(100).value() == 2);
  CHECK(e.current_state(200).value() == 6);
  CHECK(count_diag(e, AnomalyDiagCode::StateDesync) == 1);
  CHECK(e.diagnostics().size() == 1 && e.diagnostics()[0].device_id == 200);

  CHECK(e.faults_in_state(2).size() == 1);
  CHECK(e.faults_in_state(2)[0]->device_id == 100);
  CHECK(e.faults_in_state(6).size() == 1);
  CHECK(e.faults_in_state(6)[0]->device_id == 200);
}

static void test_config_values() {
  std::vector<uint8_t> wire;
  put_config(wire, 1, 0, 100, 42, 7, 99);
  put_config(wire, 1, 1, 110, 43, 99, 1);

  AnomalyEngine e;
  e.ingest(wire.data(), wire.size());

  CHECK(e.config_changes().size() == 2);
  const ConfigEvent& c0 = e.config_changes()[0];
  CHECK(c0.param_id == 42 && c0.old_value == 7 && c0.new_value == 99);
  CHECK(c0.device_id == 1 && c0.session_seq == 0 && c0.monotonic_ts == 100);
  const ConfigEvent& c1 = e.config_changes()[1];
  CHECK(c1.param_id == 43 && c1.old_value == 99 && c1.new_value == 1);
}

static void test_truncated_payload_through_engine() {
  std::vector<uint8_t> wire;
  ByteWriter w;
  w.u16(0x11);
  w.u8(2);
  w.u8(3);
  w.u8(10);  // claims 10 detail bytes
  const std::vector<uint8_t> present = {1, 2};
  w.bytes(present);  // only 2 present
  put_frame(wire, RecordType::Fault, 1, 0, 100, w.buf());

  AnomalyEngine e;
  e.ingest(wire.data(), wire.size());

  CHECK(e.faults().size() == 1);
  CHECK(e.faults()[0].detail == present);
  CHECK(count_diag(e, AnomalyDiagCode::TruncatedPayload) == 1);
}

static void test_unknown_and_foreign_records() {
  std::vector<uint8_t> wire;
  // A record type byte the format does not define -> UnknownRecord.
  put_frame(wire, static_cast<RecordType>(0x7F), 1, 0, 100, {9, 9});
  // A valid record owned by another subsystem -> ignored, no diagnostic.
  put_frame(wire, RecordType::SessionBegin, 1, 1, 110, {1, 2, 3});
  put_fault(wire, 1, 2, 120, 0xA, 1, 0, {});

  AnomalyEngine e;
  e.ingest(wire.data(), wire.size());

  CHECK(count_diag(e, AnomalyDiagCode::UnknownRecord) == 1);
  CHECK(e.diagnostics().size() == 1);  // session-begin produced nothing
  CHECK(e.faults().size() == 1);
  CHECK(e.transitions().empty() && e.config_changes().empty());
}

static void test_summary() {
  std::vector<uint8_t> wire;
  put_transition(wire, 1, 0, 100, 0, 1, 0);
  put_transition(wire, 1, 1, 110, 9, 2, 0);  // desync
  put_config(wire, 1, 2, 120, 5, 0, 1);
  put_fault(wire, 1, 3, 130, 0xA, 0, 0, {});    // Info
  put_fault(wire, 1, 4, 140, 0xB, 3, 0, {});    // Critical
  put_fault(wire, 2, 0, 150, 0xC, 200, 0, {});  // clamped Critical, dev 2

  AnomalyEngine e;
  e.ingest(wire.data(), wire.size());

  const AnomalySummary s = e.summary();
  CHECK(s.device_count == 2);
  CHECK(s.total_transitions == 2);
  CHECK(s.total_config_changes == 1);
  CHECK(s.total_faults == 3);
  CHECK(s.state_desyncs == 1);
  CHECK(s.faults_by_severity[0] == 1);  // Info
  CHECK(s.faults_by_severity[1] == 0);
  CHECK(s.faults_by_severity[2] == 0);
  CHECK(s.faults_by_severity[3] == 2);  // Critical + clamped
  CHECK(s.diagnostic_count == 1);       // the desync
}

static void test_describe_and_strings() {
  std::vector<uint8_t> wire;
  put_transition(wire, 0x1234, 0, 100, 3, 4, 1);
  put_config(wire, 0x1234, 1, 110, 42, 7, 99);
  put_fault(wire, 0x1234, 2, 120, 0xAB, 2, 5, {1, 2, 3});

  AnomalyEngine e;
  e.ingest(wire.data(), wire.size());

  const std::string ft = describe_fault(e.faults()[0]);
  CHECK(ft.find("00001234") != std::string::npos);
  CHECK(ft.find("severity error") != std::string::npos);
  CHECK(ft.find("in state 4") != std::string::npos);

  const std::string tr = describe_transition(e.transitions()[0]);
  CHECK(tr.find("3 -> 4") != std::string::npos);

  const std::string cf = describe_config(e.config_changes()[0]);
  CHECK(cf.find("7 -> 99") != std::string::npos);

  const std::string sm = describe_anomaly_summary(e.summary());
  CHECK(!sm.empty());
  CHECK(sm.find("1 faults") != std::string::npos);

  CHECK(std::string(decode_status_str(DecodeStatus::Ok)) == "ok");
  CHECK(std::string(decode_status_str(DecodeStatus::Truncated)) == "truncated");
  CHECK(std::string(severity_name(Severity::Warning)) == "warning");
  CHECK(std::string(anomaly_diag_str(AnomalyDiagCode::StateDesync)) ==
        "state-desync");
  CHECK(std::string(anomaly_diag_str(AnomalyDiagCode::UnknownRecord)) ==
        "unknown-record");
}

int main() {
  test_decoders_round_trip();
  test_decoders_truncation();
  test_transition_chain_no_desync();
  test_transition_chain_desync();
  test_fault_state_tagging();
  test_faults_in_state();
  test_severity_counts_and_clamping();
  test_fault_burst();
  test_multi_device_independence();
  test_config_values();
  test_truncated_payload_through_engine();
  test_unknown_and_foreign_records();
  test_summary();
  test_describe_and_strings();
  if (failures) {
    std::fprintf(stderr, "%d checks failed\n", failures);
    return 1;
  }
  std::printf("all anomaly tests passed\n");
  return 0;
}
