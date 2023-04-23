#ifndef BLACKSPAN_CHANNEL_H
#define BLACKSPAN_CHANNEL_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "blackspan/frame.h"
#include "blackspan/schema.h"

namespace blackspan {

constexpr size_t kSampleBlockHeaderSize = 12;

constexpr uint16_t kBlockAnchor = 0x0001;

constexpr size_t kChannelRetentionCap = 4096;

struct SampleBlockHeader {
  uint16_t channel_id = 0;
  uint16_t schema_version = 0;
  uint32_t base_ts_delta = 0;
  uint16_t sample_count = 0;
  uint16_t flags = 0;
};

enum class ChannelError : uint8_t {
  None = 0,
  NotSampleBlock,
  TruncatedHeader,
  UnknownChannel,
  UnknownVersion,
  VersionMismatch,
  LengthMismatch,
  StrideOverflow,
  ZeroStride,
  ZeroCount,
};

const char* channel_error_str(ChannelError e);

ChannelError decode_sample_block_header(const uint8_t* payload, size_t len,
                                        SampleBlockHeader& out);

class ChannelSeries {
 public:
  explicit ChannelSeries(const ChannelSchema* schema) : schema_(schema) {}

  uint16_t channel_id() const { return schema_->channel_id; }
  uint16_t schema_version() const { return schema_->version; }
  const ChannelSchema& schema() const { return *schema_; }
  size_t stride() const { return schema_->stride; }

  size_t total_samples() const { return total_; }
  size_t block_count() const { return blocks_.size(); }

  uint16_t block_sample_count(size_t block_index) const;
  uint64_t block_base_ts(size_t block_index) const;
  uint16_t block_flags(size_t block_index) const;

  std::vector<size_t> keyframe_sample_indices() const;
  std::vector<uint64_t> keyframe_timestamps() const;

  const uint8_t* sample_bytes(size_t index, size_t* len) const;

  std::optional<SampleView> view(size_t index) const;

 private:
  friend class ChannelStore;

  struct Block {
    std::vector<uint8_t> bytes;
    uint16_t sample_count = 0;
    uint16_t flags = 0;
    uint64_t base_ts = 0;
  };

  void append_block(const uint8_t* rows, size_t rows_len, uint16_t sample_count,
                    uint16_t flags, uint64_t base_ts);

  void compact_to(size_t keep_last_n);

  const ChannelSchema* schema_;
  std::vector<Block> blocks_;
  size_t total_ = 0;
};

class ChannelStore {
 public:

  ChannelError ingest_frame(const Frame& f, const SchemaRegistry& registry);

  const ChannelSeries* series(uint16_t channel_id) const;

  std::vector<uint16_t> channels() const;

  size_t total_samples() const;

  ChannelError compact_channel(uint16_t channel_id, size_t keep_last_n);

  size_t retained_samples(uint16_t channel_id) const;

  std::vector<double> column_as_doubles(uint16_t channel_id,
                                        size_t field_index) const;

 private:
  std::map<uint16_t, ChannelSeries> series_;
};

}

#endif
