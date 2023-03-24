#include <algorithm>

#include "blackspan/rewrite.h"

namespace blackspan {

bool RewriteFilter::keep(const FrameHeader& h) const {
  if (device.has_value() && h.device_id != device.value()) return false;
  if (since.has_value() && h.monotonic_ts < since.value()) return false;
  if (until.has_value() && h.monotonic_ts > until.value()) return false;
  if (seq_lo.has_value() && h.session_seq < seq_lo.value()) return false;
  if (seq_hi.has_value() && h.session_seq > seq_hi.value()) return false;
  if (!types.empty()) {
    bool matched = false;
    for (RecordType t : types) {
      if (t == h.type) {
        matched = true;
        break;
      }
    }
    if (!matched) return false;
  }
  return true;
}

RewriteResult rewrite_slice(const uint8_t* data, size_t len,
                            const RewriteFilter& filter) {
  RewriteResult out;
  FrameStream fs(data, len);
  Frame f;
  while (fs.next(f)) {
    ++out.frames_in;
    if (filter.keep(f.header)) {

      encode_frame(f.header, f.payload.data(), f.payload.size(), out.bytes);
      ++out.frames_kept;
    }
  }
  out.input_status = fs.status();
  return out;
}

RewriteResult rewrite_slice(const std::vector<uint8_t>& data,
                            const RewriteFilter& filter) {
  return rewrite_slice(data.data(), data.size(), filter);
}

namespace {

struct TaggedFrame {
  uint64_t monotonic_ts = 0;
  size_t input_index = 0;
  size_t within_input = 0;
  Frame frame;
};

}

MergeResult rewrite_merge(const std::vector<std::vector<uint8_t>>& inputs) {
  MergeResult out;
  std::vector<TaggedFrame> all;

  for (size_t i = 0; i < inputs.size(); ++i) {
    FrameStream fs(inputs[i].data(), inputs[i].size());
    Frame f;
    size_t within = 0;
    while (fs.next(f)) {
      TaggedFrame tf;
      tf.monotonic_ts = f.header.monotonic_ts;
      tf.input_index = i;
      tf.within_input = within++;
      tf.frame = f;
      all.push_back(std::move(tf));
    }
    if (fs.status() != FrameStatus::Ok &&
        fs.status() != FrameStatus::Incomplete) {
      ++out.inputs_truncated;
    } else if (fs.status() == FrameStatus::Incomplete) {
      ++out.inputs_truncated;
    }
  }

  std::stable_sort(all.begin(), all.end(),
                   [](const TaggedFrame& a, const TaggedFrame& b) {
                     if (a.monotonic_ts != b.monotonic_ts) {
                       return a.monotonic_ts < b.monotonic_ts;
                     }
                     if (a.input_index != b.input_index) {
                       return a.input_index < b.input_index;
                     }
                     return a.within_input < b.within_input;
                   });

  for (const TaggedFrame& tf : all) {
    encode_frame(tf.frame.header, tf.frame.payload.data(),
                 tf.frame.payload.size(), out.bytes);
    ++out.frames_out;
  }
  return out;
}

}
