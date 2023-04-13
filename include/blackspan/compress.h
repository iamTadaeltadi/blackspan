#ifndef BLACKSPAN_COMPRESS_H
#define BLACKSPAN_COMPRESS_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "blackspan/channel.h"

namespace blackspan {

enum class ColumnCodec : uint8_t {
  Raw = 0,
  DeltaZigzagVarint = 1,
  Rle = 2,
  DoubleSplit = 3,
};

const char* column_codec_name(ColumnCodec c);

constexpr size_t kMaxColumnCount = static_cast<size_t>(1) << 28;

struct EncodedColumn {
  ColumnCodec codec = ColumnCodec::Raw;
  size_t count = 0;
  std::vector<uint8_t> bytes;
};

EncodedColumn encode_u64_column(const std::vector<uint64_t>& values,
                                ColumnCodec codec);
bool decode_u64_column(const EncodedColumn& enc, std::vector<uint64_t>& out);

EncodedColumn encode_i64_column(const std::vector<int64_t>& values,
                                ColumnCodec codec);
bool decode_i64_column(const EncodedColumn& enc, std::vector<int64_t>& out);

EncodedColumn encode_f64_column(const std::vector<double>& values);
bool decode_f64_column(const EncodedColumn& enc, std::vector<double>& out);

ColumnCodec best_codec_for(const std::vector<uint64_t>& values);

double compression_ratio(const EncodedColumn& enc);

std::vector<uint8_t> serialize_column(const EncodedColumn& enc);
bool deserialize_column(const uint8_t* data, size_t len, EncodedColumn& out);

struct ColumnStats {
  size_t raw_bytes = 0;
  size_t encoded_bytes = 0;
  ColumnCodec codec = ColumnCodec::Raw;
  double ratio = 0.0;
};

ColumnStats compress_channel_field(const ChannelStore& store,
                                   uint16_t channel_id, size_t field_index);

std::string describe_column_stats(const ColumnStats& s);

}

#endif
