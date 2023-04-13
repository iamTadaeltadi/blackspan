#include "blackspan/compress.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include "blackspan/byteio.h"

namespace blackspan {

static_assert(sizeof(double) == 8, "compress assumes 64-bit double");
static_assert(sizeof(uint64_t) == 8, "compress assumes 64-bit words");

namespace {

uint64_t zigzag_encode(int64_t v) {
  const uint64_t uv = static_cast<uint64_t>(v);
  return (uv << 1) ^ (uint64_t{0} - (uv >> 63));
}

int64_t zigzag_decode(uint64_t v) {
  return static_cast<int64_t>((v >> 1) ^ (uint64_t{0} - (v & 1)));
}

void put_uvarint(std::vector<uint8_t>& out, uint64_t v) {
  while (v >= 0x80) {
    out.push_back(static_cast<uint8_t>(v) | 0x80);
    v >>= 7;
  }
  out.push_back(static_cast<uint8_t>(v));
}

bool get_uvarint(ByteReader& r, uint64_t& out) {
  uint64_t result = 0;
  int shift = 0;
  for (int i = 0; i < 10; ++i) {
    const uint8_t b = r.u8();
    if (!r.ok()) return false;
    if (shift == 63 && (b & 0x7F) > 1) return false;
    result |= static_cast<uint64_t>(b & 0x7F) << shift;
    if ((b & 0x80) == 0) {
      out = result;
      return true;
    }
    shift += 7;
  }
  return false;
}

void put_svarint(std::vector<uint8_t>& out, int64_t v) {
  put_uvarint(out, zigzag_encode(v));
}

bool get_svarint(ByteReader& r, int64_t& out) {
  uint64_t u = 0;
  if (!get_uvarint(r, u)) return false;
  out = zigzag_decode(u);
  return true;
}

void put_u64_le(std::vector<uint8_t>& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
  }
}

uint64_t round_field_double_to_u64(double d) {
  if (std::isnan(d)) return 0;
  const double r = std::nearbyint(d);
  const double two63 = 9223372036854775808.0;
  if (r >= two63) {
    return static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
  }
  if (r < -two63) {
    return static_cast<uint64_t>(std::numeric_limits<int64_t>::min());
  }
  return static_cast<uint64_t>(static_cast<int64_t>(r));
}

void encode_raw_u64(const std::vector<uint64_t>& v, std::vector<uint8_t>& out) {
  out.reserve(v.size() * 8);
  for (const uint64_t x : v) put_u64_le(out, x);
}

bool decode_raw_u64(ByteReader& r, size_t count, std::vector<uint64_t>& out) {
  for (size_t i = 0; i < count; ++i) {
    const uint64_t x = r.u64();
    if (!r.ok()) return false;
    out.push_back(x);
  }
  return r.remaining() == 0;
}

void encode_delta_u64(const std::vector<uint64_t>& v,
                      std::vector<uint8_t>& out) {
  if (v.empty()) return;
  put_uvarint(out, v[0]);
  for (size_t i = 1; i < v.size(); ++i) {
    const uint64_t d = v[i] - v[i - 1];
    put_svarint(out, static_cast<int64_t>(d));
  }
}

bool decode_delta_u64(ByteReader& r, size_t count, std::vector<uint64_t>& out) {
  if (count == 0) return r.remaining() == 0;
  uint64_t prev = 0;
  if (!get_uvarint(r, prev)) return false;
  out.push_back(prev);
  for (size_t i = 1; i < count; ++i) {
    int64_t sd = 0;
    if (!get_svarint(r, sd)) return false;
    prev += static_cast<uint64_t>(sd);
    out.push_back(prev);
  }
  return r.remaining() == 0;
}

void encode_rle_u64(const std::vector<uint64_t>& v, std::vector<uint8_t>& out) {
  size_t i = 0;
  while (i < v.size()) {
    size_t j = i + 1;
    while (j < v.size() && v[j] == v[i]) ++j;
    put_uvarint(out, v[i]);
    put_uvarint(out, static_cast<uint64_t>(j - i));
    i = j;
  }
}

bool decode_rle_u64(ByteReader& r, size_t count, std::vector<uint64_t>& out) {
  size_t total = 0;
  while (total < count) {
    uint64_t val = 0;
    uint64_t run = 0;
    if (!get_uvarint(r, val)) return false;
    if (!get_uvarint(r, run)) return false;
    if (run == 0) return false;
    if (run > static_cast<uint64_t>(count - total)) return false;
    for (uint64_t k = 0; k < run; ++k) out.push_back(val);
    total += static_cast<size_t>(run);
  }
  return total == count && r.remaining() == 0;
}

void encode_raw_i64(const std::vector<int64_t>& v, std::vector<uint8_t>& out) {
  out.reserve(v.size() * 8);
  for (const int64_t x : v) put_u64_le(out, static_cast<uint64_t>(x));
}

bool decode_raw_i64(ByteReader& r, size_t count, std::vector<int64_t>& out) {
  for (size_t i = 0; i < count; ++i) {
    const uint64_t x = r.u64();
    if (!r.ok()) return false;
    out.push_back(static_cast<int64_t>(x));
  }
  return r.remaining() == 0;
}

void encode_delta_i64(const std::vector<int64_t>& v,
                      std::vector<uint8_t>& out) {
  if (v.empty()) return;
  put_svarint(out, v[0]);
  for (size_t i = 1; i < v.size(); ++i) {
    const uint64_t d =
        static_cast<uint64_t>(v[i]) - static_cast<uint64_t>(v[i - 1]);
    put_svarint(out, static_cast<int64_t>(d));
  }
}

bool decode_delta_i64(ByteReader& r, size_t count, std::vector<int64_t>& out) {
  if (count == 0) return r.remaining() == 0;
  int64_t first = 0;
  if (!get_svarint(r, first)) return false;
  uint64_t prev = static_cast<uint64_t>(first);
  out.push_back(first);
  for (size_t i = 1; i < count; ++i) {
    int64_t sd = 0;
    if (!get_svarint(r, sd)) return false;
    prev += static_cast<uint64_t>(sd);
    out.push_back(static_cast<int64_t>(prev));
  }
  return r.remaining() == 0;
}

void encode_rle_i64(const std::vector<int64_t>& v, std::vector<uint8_t>& out) {
  size_t i = 0;
  while (i < v.size()) {
    size_t j = i + 1;
    while (j < v.size() && v[j] == v[i]) ++j;
    put_svarint(out, v[i]);
    put_uvarint(out, static_cast<uint64_t>(j - i));
    i = j;
  }
}

bool decode_rle_i64(ByteReader& r, size_t count, std::vector<int64_t>& out) {
  size_t total = 0;
  while (total < count) {
    int64_t val = 0;
    uint64_t run = 0;
    if (!get_svarint(r, val)) return false;
    if (!get_uvarint(r, run)) return false;
    if (run == 0) return false;
    if (run > static_cast<uint64_t>(count - total)) return false;
    for (uint64_t k = 0; k < run; ++k) out.push_back(val);
    total += static_cast<size_t>(run);
  }
  return total == count && r.remaining() == 0;
}

ColumnCodec normalize_integer_codec(ColumnCodec c) {
  return c == ColumnCodec::DoubleSplit ? ColumnCodec::DeltaZigzagVarint : c;
}

size_t decode_reserve_hint(size_t count, size_t byte_len) {
  return std::min(count, byte_len + 1);
}

}

const char* column_codec_name(ColumnCodec c) {
  switch (c) {
    case ColumnCodec::Raw:
      return "Raw";
    case ColumnCodec::DeltaZigzagVarint:
      return "DeltaZigzagVarint";
    case ColumnCodec::Rle:
      return "Rle";
    case ColumnCodec::DoubleSplit:
      return "DoubleSplit";
  }
  return "Unknown";
}

EncodedColumn encode_u64_column(const std::vector<uint64_t>& values,
                                ColumnCodec codec) {
  EncodedColumn enc;
  enc.count = values.size();
  enc.codec = normalize_integer_codec(codec);
  switch (enc.codec) {
    case ColumnCodec::Raw:
      encode_raw_u64(values, enc.bytes);
      break;
    case ColumnCodec::DeltaZigzagVarint:
      encode_delta_u64(values, enc.bytes);
      break;
    case ColumnCodec::Rle:
      encode_rle_u64(values, enc.bytes);
      break;
    case ColumnCodec::DoubleSplit:
      encode_delta_u64(values, enc.bytes);
      break;
  }
  return enc;
}

bool decode_u64_column(const EncodedColumn& enc, std::vector<uint64_t>& out) {
  out.clear();
  if (enc.count > kMaxColumnCount) return false;
  out.reserve(decode_reserve_hint(enc.count, enc.bytes.size()));
  ByteReader r(enc.bytes.data(), enc.bytes.size());
  switch (enc.codec) {
    case ColumnCodec::Raw:
      return decode_raw_u64(r, enc.count, out);
    case ColumnCodec::DeltaZigzagVarint:
      return decode_delta_u64(r, enc.count, out);
    case ColumnCodec::Rle:
      return decode_rle_u64(r, enc.count, out);
    case ColumnCodec::DoubleSplit:
      return decode_delta_u64(r, enc.count, out);
  }
  return false;
}

EncodedColumn encode_i64_column(const std::vector<int64_t>& values,
                                ColumnCodec codec) {
  EncodedColumn enc;
  enc.count = values.size();
  enc.codec = normalize_integer_codec(codec);
  switch (enc.codec) {
    case ColumnCodec::Raw:
      encode_raw_i64(values, enc.bytes);
      break;
    case ColumnCodec::DeltaZigzagVarint:
      encode_delta_i64(values, enc.bytes);
      break;
    case ColumnCodec::Rle:
      encode_rle_i64(values, enc.bytes);
      break;
    case ColumnCodec::DoubleSplit:
      encode_delta_i64(values, enc.bytes);
      break;
  }
  return enc;
}

bool decode_i64_column(const EncodedColumn& enc, std::vector<int64_t>& out) {
  out.clear();
  if (enc.count > kMaxColumnCount) return false;
  out.reserve(decode_reserve_hint(enc.count, enc.bytes.size()));
  ByteReader r(enc.bytes.data(), enc.bytes.size());
  switch (enc.codec) {
    case ColumnCodec::Raw:
      return decode_raw_i64(r, enc.count, out);
    case ColumnCodec::DeltaZigzagVarint:
      return decode_delta_i64(r, enc.count, out);
    case ColumnCodec::Rle:
      return decode_rle_i64(r, enc.count, out);
    case ColumnCodec::DoubleSplit:
      return decode_delta_i64(r, enc.count, out);
  }
  return false;
}

EncodedColumn encode_f64_column(const std::vector<double>& values) {
  EncodedColumn enc;
  enc.count = values.size();
  enc.codec = ColumnCodec::DoubleSplit;
  std::vector<uint64_t> bits;
  bits.reserve(values.size());
  for (const double d : values) {
    uint64_t u = 0;
    std::memcpy(&u, &d, sizeof(u));
    bits.push_back(u);
  }
  encode_delta_u64(bits, enc.bytes);
  return enc;
}

bool decode_f64_column(const EncodedColumn& enc, std::vector<double>& out) {
  out.clear();
  if (enc.codec != ColumnCodec::DoubleSplit) return false;
  if (enc.count > kMaxColumnCount) return false;
  std::vector<uint64_t> bits;
  bits.reserve(decode_reserve_hint(enc.count, enc.bytes.size()));
  ByteReader r(enc.bytes.data(), enc.bytes.size());
  if (!decode_delta_u64(r, enc.count, bits)) return false;
  out.reserve(bits.size());
  for (const uint64_t u : bits) {
    double d = 0.0;
    std::memcpy(&d, &u, sizeof(d));
    out.push_back(d);
  }
  return true;
}

ColumnCodec best_codec_for(const std::vector<uint64_t>& values) {
  const ColumnCodec candidates[] = {ColumnCodec::Raw,
                                    ColumnCodec::DeltaZigzagVarint,
                                    ColumnCodec::Rle};
  ColumnCodec best = ColumnCodec::Raw;
  size_t best_size = 0;
  bool have = false;
  for (const ColumnCodec c : candidates) {
    const EncodedColumn e = encode_u64_column(values, c);
    if (!have || e.bytes.size() < best_size) {
      best = c;
      best_size = e.bytes.size();
      have = true;
    }
  }
  return best;
}

double compression_ratio(const EncodedColumn& enc) {
  const size_t raw = enc.count * 8;
  if (raw == 0) return 0.0;
  return static_cast<double>(enc.bytes.size()) / static_cast<double>(raw);
}

std::vector<uint8_t> serialize_column(const EncodedColumn& enc) {
  std::vector<uint8_t> out;
  out.push_back('B');
  out.push_back('S');
  out.push_back('C');
  out.push_back('1');
  out.push_back(1);
  out.push_back(static_cast<uint8_t>(enc.codec));
  put_uvarint(out, static_cast<uint64_t>(enc.count));
  put_uvarint(out, static_cast<uint64_t>(enc.bytes.size()));
  out.insert(out.end(), enc.bytes.begin(), enc.bytes.end());
  return out;
}

bool deserialize_column(const uint8_t* data, size_t len, EncodedColumn& out) {
  ByteReader r(data, len);
  const uint8_t m0 = r.u8();
  const uint8_t m1 = r.u8();
  const uint8_t m2 = r.u8();
  const uint8_t m3 = r.u8();
  if (!r.ok()) return false;
  if (m0 != 'B' || m1 != 'S' || m2 != 'C' || m3 != '1') return false;
  const uint8_t ver = r.u8();
  if (!r.ok() || ver != 1) return false;
  const uint8_t codec_raw = r.u8();
  if (!r.ok()) return false;
  if (codec_raw > static_cast<uint8_t>(ColumnCodec::DoubleSplit)) return false;

  uint64_t count = 0;
  uint64_t blen = 0;
  if (!get_uvarint(r, count)) return false;
  if (!get_uvarint(r, blen)) return false;
  if (count > kMaxColumnCount) return false;
  if (blen > static_cast<uint64_t>(r.remaining())) return false;

  std::vector<uint8_t> payload(static_cast<size_t>(blen));
  if (blen > 0 && !r.read_bytes(payload.data(), static_cast<size_t>(blen))) {
    return false;
  }
  if (r.remaining() != 0) return false;

  out.codec = static_cast<ColumnCodec>(codec_raw);
  out.count = static_cast<size_t>(count);
  out.bytes = std::move(payload);
  return true;
}

ColumnStats compress_channel_field(const ChannelStore& store,
                                   uint16_t channel_id, size_t field_index) {
  ColumnStats s;
  const std::vector<double> col =
      store.column_as_doubles(channel_id, field_index);
  std::vector<uint64_t> values;
  values.reserve(col.size());
  for (const double d : col) values.push_back(round_field_double_to_u64(d));

  const ColumnCodec codec = best_codec_for(values);
  const EncodedColumn enc = encode_u64_column(values, codec);
  s.codec = enc.codec;
  s.raw_bytes = values.size() * 8;
  s.encoded_bytes = enc.bytes.size();
  s.ratio = compression_ratio(enc);
  return s;
}

std::string describe_column_stats(const ColumnStats& s) {
  std::string out = "codec=";
  out += column_codec_name(s.codec);
  out += " raw=";
  out += std::to_string(s.raw_bytes);
  out += " enc=";
  out += std::to_string(s.encoded_bytes);
  out += " ratio=";
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.4f", s.ratio);
  out += buf;
  return out;
}

}
