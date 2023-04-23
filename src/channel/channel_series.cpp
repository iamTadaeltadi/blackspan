#include <utility>

#include "blackspan/channel.h"

namespace blackspan {

uint16_t ChannelSeries::block_sample_count(size_t block_index) const {
  if (block_index >= blocks_.size()) return 0;
  return blocks_[block_index].sample_count;
}

uint64_t ChannelSeries::block_base_ts(size_t block_index) const {
  if (block_index >= blocks_.size()) return 0;
  return blocks_[block_index].base_ts;
}

uint16_t ChannelSeries::block_flags(size_t block_index) const {
  if (block_index >= blocks_.size()) return 0;
  return blocks_[block_index].flags;
}

const uint8_t* ChannelSeries::sample_bytes(size_t index, size_t* len) const {
  if (len) *len = 0;
  const size_t st = stride();
  if (st == 0) return nullptr;
  size_t remaining = index;
  for (const Block& b : blocks_) {
    const size_t count = b.sample_count;
    if (remaining < count) {
      if (len) *len = st;
      return b.bytes.data() + remaining * st;
    }
    remaining -= count;
  }
  return nullptr;
}

std::optional<SampleView> ChannelSeries::view(size_t index) const {
  size_t len = 0;
  const uint8_t* p = sample_bytes(index, &len);
  if (p == nullptr) return std::nullopt;
  return SampleView(schema(), p, len);
}

void ChannelSeries::append_block(const uint8_t* rows, size_t rows_len,
                                 uint16_t sample_count, uint16_t flags,
                                 uint64_t base_ts) {
  Block b;
  if (rows_len > 0 && rows != nullptr) {
    b.bytes.assign(rows, rows + rows_len);
  }
  b.sample_count = sample_count;
  b.flags = flags;
  b.base_ts = base_ts;
  blocks_.push_back(std::move(b));
  total_ += sample_count;
}

void ChannelSeries::compact_to(size_t keep_last_n) {
  if (keep_last_n >= total_) return;
  if (keep_last_n == 0) {
    blocks_.clear();
    total_ = 0;
    return;
  }

  const size_t st = stride();
  size_t drop = total_ - keep_last_n;
  size_t first_keep = 0;
  for (; first_keep < blocks_.size(); ++first_keep) {
    Block& b = blocks_[first_keep];
    const size_t count = b.sample_count;
    if (count <= drop) {
      drop -= count;
      continue;
    }
    if (drop > 0) {

      b.bytes.erase(b.bytes.begin(),
                    b.bytes.begin() + static_cast<std::ptrdiff_t>(drop * st));
      b.bytes.shrink_to_fit();

      if (!(b.flags & kBlockAnchor)) {
        b.sample_count = static_cast<uint16_t>(count - drop);
      }
      drop = 0;
    }
    break;
  }
  if (first_keep > 0) {
    blocks_.erase(blocks_.begin(),
                  blocks_.begin() + static_cast<std::ptrdiff_t>(first_keep));
  }
  total_ = keep_last_n;
}

std::vector<size_t> ChannelSeries::keyframe_sample_indices() const {
  std::vector<size_t> out;
  size_t global = 0;
  for (const Block& b : blocks_) {
    if (b.flags & kBlockAnchor) out.push_back(global);
    global += b.sample_count;
  }
  return out;
}

std::vector<uint64_t> ChannelSeries::keyframe_timestamps() const {
  std::vector<uint64_t> out;
  for (const Block& b : blocks_) {
    if (b.flags & kBlockAnchor) out.push_back(b.base_ts);
  }
  return out;
}

}
