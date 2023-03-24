#ifndef BLACKSPAN_FRAME_H
#define BLACKSPAN_FRAME_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace blackspan {

constexpr uint32_t kFrameMagic = 0x31505342u;
constexpr uint8_t kFrameVersion = 1;
constexpr size_t kFrameHeaderSize = 28;
constexpr size_t kFrameTrailerSize = 4;
constexpr size_t kMaxPayload = 65535;

enum class RecordType : uint8_t {
  SessionBegin = 0x01,
  SessionEnd = 0x02,
  SchemaDef = 0x03,
  SampleBlock = 0x04,
  StateTransition = 0x05,
  ConfigChange = 0x06,
  Fault = 0x07,
  Marker = 0x08,
  Checkpoint = 0x09,
  Reconnect = 0x0A,
  DerivedDef = 0x0B,
  ExportRequest = 0x0C,
};

struct FrameHeader {
  uint8_t version = kFrameVersion;
  RecordType type = RecordType::Marker;
  uint16_t flags = 0;
  uint32_t device_id = 0;
  uint32_t session_seq = 0;
  uint64_t monotonic_ts = 0;
  uint16_t payload_len = 0;
};

struct Frame {
  FrameHeader header;
  std::vector<uint8_t> payload;
};

enum class FrameStatus {
  Ok,
  Incomplete,
  BadMagic,
  BadVersion,
  HeaderCrcMismatch,
  PayloadCrcMismatch,
  PayloadTooLong,
};

size_t encode_frame(const FrameHeader& h, const uint8_t* payload,
                    size_t payload_len, std::vector<uint8_t>& out);
size_t encode_frame(const Frame& f, std::vector<uint8_t>& out);

FrameStatus decode_frame(const uint8_t* data, size_t len, Frame& out,
                         size_t* consumed);

const char* frame_status_str(FrameStatus s);
const char* record_type_name(RecordType t);

class FrameStream {
 public:
  FrameStream(const uint8_t* data, size_t len)
      : data_(data), len_(len), off_(0), status_(FrameStatus::Ok) {}

  bool next(Frame& out);
  FrameStatus status() const { return status_; }
  size_t offset() const { return off_; }

 private:
  const uint8_t* data_;
  size_t len_;
  size_t off_;
  FrameStatus status_;
};

}

#endif
