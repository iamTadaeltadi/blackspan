#ifndef BLACKSPAN_CHANNEL_H
#define BLACKSPAN_CHANNEL_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "blackspan/frame.h"
#include "blackspan/schema.h"

// The channel subsystem accumulates the samples carried by sample-block frames
// into per-channel column stores. A sample-block frame names a channel and a
// schema version and carries a batch of fixed-width sample rows; the schema
// registry resolves the row layout, and this subsystem appends the batch to the
// channel's running series so later stages can read typed values back by global
// sample index. A series is pinned to a single schema version so that all of its
// rows share one stride; a batch that names a different version for the same
// channel is refused rather than mixed in. Retention is a first-class concern:
// compaction drops retired leading samples and repacks storage while keeping the
// surviving samples correctly addressable. Nothing here throws; malformed input
// yields a specific ChannelError and never a crash.

namespace blackspan {

// Fixed header at the start of every sample-block (0x04) payload, preceding the
// packed sample rows. All fields are little-endian:
//   [channel_id u16][schema_version u16][base_ts_delta u32]
//   [sample_count u16][flags u16]
// The rows that follow are sample_count copies of the resolved schema's stride.
constexpr size_t kSampleBlockHeaderSize = 12;

// Sample-block flag bits (the block header's flags field).
//
// kBlockAnchor marks a block that begins at a recorder keyframe -- a buffer
// flush boundary the firmware guarantees is sample-aligned. Anchor blocks are
// the fixed points cross-block references (derived-channel alignment, checkpoint
// replay) resolve against, so retention preserves an anchor block's declared
// sample span even when it trims the block's leading rows to reclaim memory.
constexpr uint16_t kBlockAnchor = 0x0001;

// Retention cap: a channel's series keeps at most this many of its most recent
// samples in memory. A recording can stream far more samples than a triage
// session needs to reconstruct, so the store compacts a channel back to this
// bound as new blocks arrive, bounding memory on very long recordings.
constexpr size_t kChannelRetentionCap = 4096;

struct SampleBlockHeader {
  uint16_t channel_id = 0;
  uint16_t schema_version = 0;
  uint32_t base_ts_delta = 0;
  uint16_t sample_count = 0;
  uint16_t flags = 0;
};

// Error codes for sample-block decoding and ingestion. Errors are values;
// nothing in this subsystem throws.
enum class ChannelError : uint8_t {
  None = 0,
  NotSampleBlock,   // ingest_frame was handed a non-sample-block frame
  TruncatedHeader,  // payload ended before the fixed sample-block header
  UnknownChannel,   // no schema is registered for the block's channel_id
  UnknownVersion,   // the channel is known but the named version is absent
  VersionMismatch,  // the block's version differs from the series' pinned one
  LengthMismatch,   // payload length != header + sample_count * stride
  StrideOverflow,   // sample_count * stride would overflow size_t
  ZeroStride,       // the resolved schema has a zero stride (unusable)
  ZeroCount,        // the block declared no samples
};

// Human-readable form of a ChannelError. Never null.
const char* channel_error_str(ChannelError e);

// Decodes the fixed sample-block header from a payload. Returns
// ChannelError::TruncatedHeader when fewer than kSampleBlockHeaderSize bytes are
// available; otherwise fills out and returns ChannelError::None. The packed rows
// that follow the header are not inspected here.
ChannelError decode_sample_block_header(const uint8_t* payload, size_t len,
                                        SampleBlockHeader& out);

// The accumulated samples for one channel, stored as a sequence of blocks. Each
// block keeps its packed row bytes, its sample_count, the absolute base
// timestamp it was recorded at (the frame's monotonic_ts plus the block's
// base_ts_delta), and its block flags. The series borrows the resolved schema
// from a SchemaRegistry and is pinned to that one version; every block shares the
// schema's stride. Samples are addressed by a global index that runs across all
// blocks in ingest order.
class ChannelSeries {
 public:
  explicit ChannelSeries(const ChannelSchema* schema) : schema_(schema) {}

  uint16_t channel_id() const { return schema_->channel_id; }
  uint16_t schema_version() const { return schema_->version; }
  const ChannelSchema& schema() const { return *schema_; }
  size_t stride() const { return schema_->stride; }

  // Total samples currently retained across all blocks.
  size_t total_samples() const { return total_; }
  size_t block_count() const { return blocks_.size(); }

  // Per-block metadata, addressed by block index. Out-of-range indices return 0.
  uint16_t block_sample_count(size_t block_index) const;
  uint64_t block_base_ts(size_t block_index) const;
  uint16_t block_flags(size_t block_index) const;

  // Resolves a global sample index to the packed bytes of that row. On success
  // returns a pointer into internal storage and, if len is non-null, writes the
  // schema stride to *len. Returns nullptr (and writes 0 to *len) when the index
  // is out of range. The pointer is valid until the series is next mutated.
  const uint8_t* sample_bytes(size_t index, size_t* len) const;

  // A bounds-checked typed view over the sample at the given global index, or
  // nullopt if the index is out of range. The view borrows both the schema and
  // the sample bytes and must not outlive the series or the registry.
  std::optional<SampleView> view(size_t index) const;

 private:
  friend class ChannelStore;

  struct Block {
    std::vector<uint8_t> bytes;  // sample_count * stride packed row bytes
    uint16_t sample_count = 0;
    uint16_t flags = 0;
    uint64_t base_ts = 0;
  };

  // Appends a decoded block. rows points at sample_count * stride bytes.
  void append_block(const uint8_t* rows, size_t rows_len, uint16_t sample_count,
                    uint16_t flags, uint64_t base_ts);

  // Drops leading samples so that exactly min(keep_last_n, total) of the most
  // recent samples remain, repacking any partially retained block in place.
  void compact_to(size_t keep_last_n);

  const ChannelSchema* schema_;
  std::vector<Block> blocks_;
  size_t total_ = 0;
};

// Owns one ChannelSeries per channel and drives ingestion of sample-block frames
// against a SchemaRegistry. The registry must outlive the store: series borrow
// their schemas from it.
class ChannelStore {
 public:
  // Decodes a sample-block frame and appends its batch to the channel's series.
  // Resolves the schema by (channel_id, schema_version), verifies that the
  // payload length equals the header plus sample_count * stride, and creates the
  // series on first use. Returns a specific ChannelError for a non-sample-block
  // frame, a truncated header, an unknown channel or version, a version that
  // conflicts with the series' pinned one, a zero-count block, a length or stride
  // mismatch, or an overflow; ChannelError::None on success.
  ChannelError ingest_frame(const Frame& f, const SchemaRegistry& registry);

  // The series for a channel, or nullptr if none has been ingested.
  const ChannelSeries* series(uint16_t channel_id) const;

  // All channel ids with a series, ascending.
  std::vector<uint16_t> channels() const;

  // Total samples retained across every channel.
  size_t total_samples() const;

  // Drops all but the most recent keep_last_n samples of a channel, repacking
  // storage so the survivors stay readable through view(). Returns
  // ChannelError::UnknownChannel if the channel has no series, else None.
  ChannelError compact_channel(uint16_t channel_id, size_t keep_last_n);

  // Samples currently retained for a channel; 0 if it has no series.
  size_t retained_samples(uint16_t channel_id) const;

  // Reads one field across every retained sample of a channel, widened to
  // double via SampleView::get_as_double. Samples that fail to view or whose
  // field is unreadable are skipped. Returns an empty vector for an unknown
  // channel.
  std::vector<double> column_as_doubles(uint16_t channel_id,
                                        size_t field_index) const;

 private:
  std::map<uint16_t, ChannelSeries> series_;
};

}  // namespace blackspan

#endif  // BLACKSPAN_CHANNEL_H
