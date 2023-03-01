#ifndef BLACKSPAN_VALIDATE_H
#define BLACKSPAN_VALIDATE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "blackspan/pipeline.h"

// The validate subsystem is a structural lint over a reconstructed recording.
// Decoding a recording tells you what it contains; validation tells you whether
// what it contains is coherent. It reads the reconstructed model produced by the
// decode pipeline and reports findings at three severities: an error is a
// structural contradiction (a truncated frame stream, a fault attributed to a
// device that never opened a session), a warning is a plausible-but-suspect
// shape (an unterminated session, a channel that was declared but never
// sampled, a timestamp that ran backwards), and a note is an observation worth
// surfacing during triage (a critical fault, a fault burst, a state desync).
//
// The subsystem reads only the reconstructed model; it never touches raw bytes
// beyond what the pipeline already decoded, and it mutates nothing. Every check
// is a pure function of the model. Nothing here throws.

namespace blackspan {

// The severity of a finding, ascending. The numeric order lets a caller take the
// maximum severity across a report to choose an exit status.
enum class Severity3 : uint8_t {
  Note = 0,
  Warning = 1,
  Error = 2,
};

// Lowercase name of a severity ("note", "warning", "error"). Never null.
const char* severity3_name(Severity3 s);

// A stable identifier for each kind of finding, so callers can filter or count
// by cause without string matching. Grouped by the subsystem the check draws on.
enum class ValidationCode : uint16_t {
  // Frame-stream level.
  StreamTruncated = 100,       // decode stopped before the buffer was consumed
  StreamHalted = 101,          // decode stopped on a hard frame error
  EmptyRecording = 102,        // the recording held no complete frames

  // Session level.
  UnterminatedSession = 200,   // a session was left open at end of stream
  SessionSeqRegression = 201,  // session_seq moved backwards within a session
  SessionTsRegression = 202,   // monotonic_ts moved backwards within a session
  SessionNoActivity = 203,     // a session carried no samples, faults, or state
  EndWithoutBegin = 204,       // a session-end had no active session
  ZeroDurationSession = 205,   // a closed session began and ended at one instant

  // Schema and channel level.
  ChannelDeclaredUnused = 300, // a schema was declared but never sampled
  ChannelSampledUndeclared = 301,  // samples exist with no resolvable schema
  ChannelEmptySeries = 302,    // a channel has a series but zero samples
  SchemaMultipleVersions = 303,  // a channel changed layout mid-recording

  // Anomaly level.
  StateDesync = 400,           // a transition did not continue the known chain
  CriticalFault = 401,         // a fault of Critical severity was recorded
  FaultBurst = 402,            // several faults fell within a short window
  FaultWithoutState = 403,     // a fault fired before any state was known

  // Cross-subsystem.
  FaultUnknownDevice = 500,    // a fault named a device with no session
  OrphanConfigChange = 501,    // a config change fell outside any session span
};

// Human-readable name of a validation code. Never null.
const char* validation_code_name(ValidationCode c);

// One validation finding.
struct Finding {
  Severity3 severity = Severity3::Note;
  ValidationCode code = ValidationCode::EmptyRecording;
  uint32_t device_id = 0;
  uint32_t session_seq = 0;
  std::string detail;
};

// A single-line rendering, e.g.
// `error [stream-truncated] dev=00000000 seq=0: 12 of 40 bytes decoded`.
std::string describe_finding(const Finding& f);

// Aggregate counts for a report.
struct ValidationSummary {
  size_t errors = 0;
  size_t warnings = 0;
  size_t notes = 0;

  size_t total() const { return errors + warnings + notes; }
  // The highest severity present, or Note when the report is empty.
  Severity3 worst() const {
    if (errors) return Severity3::Error;
    if (warnings) return Severity3::Warning;
    return Severity3::Note;
  }
};

// Tunable thresholds for the checks that need one.
struct ValidationOptions {
  // A fault burst is at least this many faults within burst_window_ns of the
  // run's first fault. Matches AnomalyEngine::burst_windows semantics.
  uint64_t burst_window_ns = 1000;
  size_t burst_min_count = 3;
  // When false, the note-level checks (critical faults, bursts, desyncs) are
  // skipped, leaving only warnings and errors.
  bool include_notes = true;
};

// The result of validating a recording: every finding, in a deterministic
// order (frame-stream, then session, then schema/channel, then anomaly, then
// cross-subsystem; within a group, by device and sequence), plus the summary.
class ValidationReport {
 public:
  static ValidationReport run(const DecodedRecording& rec,
                              const ValidationOptions& opts = {});

  const std::vector<Finding>& findings() const { return findings_; }
  const ValidationSummary& summary() const { return summary_; }

  // Findings of a given severity, as pointers into this report's storage.
  std::vector<const Finding*> at_severity(Severity3 s) const;

  // The number of findings with a given code.
  size_t count_of(ValidationCode c) const;

 private:
  std::vector<Finding> findings_;
  ValidationSummary summary_;
};

// Renders a report to a deterministic multi-line block: one line per finding,
// followed by a summary line. An empty report renders a single "ok: no findings"
// line.
std::string render_validation(const ValidationReport& report);

}  // namespace blackspan

#endif  // BLACKSPAN_VALIDATE_H
