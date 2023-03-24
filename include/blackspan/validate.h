#ifndef BLACKSPAN_VALIDATE_H
#define BLACKSPAN_VALIDATE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "blackspan/pipeline.h"

namespace blackspan {

enum class Severity3 : uint8_t {
  Note = 0,
  Warning = 1,
  Error = 2,
};

const char* severity3_name(Severity3 s);

enum class ValidationCode : uint16_t {

  StreamTruncated = 100,
  StreamHalted = 101,
  EmptyRecording = 102,

  UnterminatedSession = 200,
  SessionSeqRegression = 201,
  SessionTsRegression = 202,
  SessionNoActivity = 203,
  EndWithoutBegin = 204,
  ZeroDurationSession = 205,

  ChannelDeclaredUnused = 300,
  ChannelSampledUndeclared = 301,
  ChannelEmptySeries = 302,
  SchemaMultipleVersions = 303,

  StateDesync = 400,
  CriticalFault = 401,
  FaultBurst = 402,
  FaultWithoutState = 403,

  FaultUnknownDevice = 500,
  OrphanConfigChange = 501,
};

const char* validation_code_name(ValidationCode c);

struct Finding {
  Severity3 severity = Severity3::Note;
  ValidationCode code = ValidationCode::EmptyRecording;
  uint32_t device_id = 0;
  uint32_t session_seq = 0;
  std::string detail;
};

std::string describe_finding(const Finding& f);

struct ValidationSummary {
  size_t errors = 0;
  size_t warnings = 0;
  size_t notes = 0;

  size_t total() const { return errors + warnings + notes; }

  Severity3 worst() const {
    if (errors) return Severity3::Error;
    if (warnings) return Severity3::Warning;
    return Severity3::Note;
  }
};

struct ValidationOptions {

  uint64_t burst_window_ns = 1000;
  size_t burst_min_count = 3;

  bool include_notes = true;
};

class ValidationReport {
 public:
  static ValidationReport run(const DecodedRecording& rec,
                              const ValidationOptions& opts = {});

  const std::vector<Finding>& findings() const { return findings_; }
  const ValidationSummary& summary() const { return summary_; }

  std::vector<const Finding*> at_severity(Severity3 s) const;

  size_t count_of(ValidationCode c) const;

 private:
  std::vector<Finding> findings_;
  ValidationSummary summary_;
};

std::string render_validation(const ValidationReport& report);

}

#endif
