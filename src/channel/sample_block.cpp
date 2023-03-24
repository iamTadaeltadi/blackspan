#include "blackspan/byteio.h"
#include "blackspan/channel.h"

namespace blackspan {

ChannelError decode_sample_block_header(const uint8_t* payload, size_t len,
                                        SampleBlockHeader& out) {
  if (payload == nullptr || len < kSampleBlockHeaderSize) {
    return ChannelError::TruncatedHeader;
  }
  ByteReader r(payload, len);
  out.channel_id = r.u16();
  out.schema_version = r.u16();
  out.base_ts_delta = r.u32();
  out.sample_count = r.u16();
  out.flags = r.u16();
  return ChannelError::None;
}

const char* channel_error_str(ChannelError e) {
  switch (e) {
    case ChannelError::None:
      return "ok";
    case ChannelError::NotSampleBlock:
      return "not-sample-block";
    case ChannelError::TruncatedHeader:
      return "truncated-header";
    case ChannelError::UnknownChannel:
      return "unknown-channel";
    case ChannelError::UnknownVersion:
      return "unknown-version";
    case ChannelError::VersionMismatch:
      return "version-mismatch";
    case ChannelError::LengthMismatch:
      return "length-mismatch";
    case ChannelError::StrideOverflow:
      return "stride-overflow";
    case ChannelError::ZeroStride:
      return "zero-stride";
    case ChannelError::ZeroCount:
      return "zero-count";
  }
  return "unknown";
}

}
