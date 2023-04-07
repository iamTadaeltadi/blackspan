#ifndef BLACKSPAN_INTEGRITY_H
#define BLACKSPAN_INTEGRITY_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "blackspan/pipeline.h"

namespace blackspan {

enum class IntegrityLevel : uint8_t {
  Info,
  Warning,
  Error,
};

const char* integrity_level_name(IntegrityLevel l);

enum class IntegrityCode : uint8_t {
  Ok,
  FrameCrcSuspect,
  TruncatedStream,
  NonMonotonicSeq,
  NonMonotonicTs,
  UnknownRecordType,
  SessionUnterminated,
  SessionEndWithoutBegin,
  SchemaRedefined,
  SampleWithoutSchema,
  EmptyRecording,
  DeviceCountAnomaly,
};

const char* integrity_code_name(IntegrityCode c);

struct IntegrityFinding {
  IntegrityCode code = IntegrityCode::Ok;
  IntegrityLevel level = IntegrityLevel::Info;
  uint32_t device_id = 0;
  uint32_t session_seq = 0;
  uint64_t ts = 0;
  std::string detail;
};

struct IntegrityReport {
  std::vector<IntegrityFinding> findings;
  size_t frames_total = 0;
  size_t frames_ok = 0;
  size_t error_count = 0;
  size_t warning_count = 0;

  bool clean() const;
};

IntegrityReport audit_frames(const uint8_t* data, size_t len);

IntegrityReport audit_recording(const DecodedRecording& rec,
                                const uint8_t* data, size_t len);

std::string render_integrity(const IntegrityReport& r);

std::string describe_integrity_finding(const IntegrityFinding& f);

}

#endif
