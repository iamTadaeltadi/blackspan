#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "blackspan/byteio.h"
#include "blackspan/channel.h"
#include "blackspan/frame.h"
#include "blackspan/schema.h"

using namespace blackspan;

static int failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond,         \
                   __FILE__, __LINE__);                                 \
      ++failures;                                                       \
    }                                                                   \
  } while (0)

using FieldDef = std::pair<std::string, FieldType>;

static ChannelSchema make_schema(uint16_t channel_id, const std::string& name,
                                 const std::vector<FieldDef>& fields) {
  ChannelSchema s;
  s.channel_id = channel_id;
  s.name = name;
  for (const auto& f : fields) {
    SchemaField sf;
    sf.name = f.first;
    sf.type = f.second;
    s.fields.push_back(sf);
  }
  s.recompute_layout();
  return s;
}

static ChannelSchema imu_schema(uint16_t channel_id = 3) {
  return make_schema(channel_id, "imu",
                     {{"ax", FieldType::F32}, {"seq", FieldType::U32},
                      {"ok", FieldType::U8}, {"temp", FieldType::I16},
                      {"alt", FieldType::F64}});
}

struct ImuRow {
  float ax;
  uint32_t seq;
  uint8_t ok;
  int16_t temp;
  double alt;
};

static ImuRow imu_row(int i) {
  ImuRow r;
  r.ax = static_cast<float>(i) * 1.5f;
  r.seq = static_cast<uint32_t>(100 + i);
  r.ok = static_cast<uint8_t>(i % 2);
  r.temp = static_cast<int16_t>(-i * 10);
  r.alt = static_cast<double>(i) * 0.25;
  return r;
}

static void write_imu_row(ByteWriter& w, const ImuRow& r) {
  uint32_t axbits;
  std::memcpy(&axbits, &r.ax, 4);
  w.u32(axbits);
  w.u32(r.seq);
  w.u8(r.ok);
  w.u16(static_cast<uint16_t>(r.temp));
  uint64_t altbits;
  std::memcpy(&altbits, &r.alt, 8);
  w.u64(altbits);
}

static std::vector<uint8_t> imu_rows(int lo, int hi) {
  ByteWriter w;
  for (int i = lo; i < hi; ++i) write_imu_row(w, imu_row(i));
  return w.buf();
}

static std::vector<uint8_t> build_block_payload(uint16_t channel_id,
                                                uint16_t version,
                                                uint32_t base_ts_delta,
                                                uint16_t sample_count,
                                                uint16_t flags,
                                                const std::vector<uint8_t>& rows) {
  ByteWriter w;
  w.u16(channel_id);
  w.u16(version);
  w.u32(base_ts_delta);
  w.u16(sample_count);
  w.u16(flags);
  w.bytes(rows);
  return w.buf();
}

static Frame make_block_frame(uint64_t monotonic_ts,
                              const std::vector<uint8_t>& payload) {
  Frame f;
  f.header.type = RecordType::SampleBlock;
  f.header.device_id = 0x1234u;
  f.header.monotonic_ts = monotonic_ts;
  f.payload = payload;
  return f;
}

static bool view_matches(const SampleView& v, const ImuRow& r) {
  const auto ax = v.get_f32(0);
  const auto seq = v.get_u32(1);
  const auto ok = v.get_u8(2);
  const auto temp = v.get_i16(3);
  const auto alt = v.get_f64(4);
  return ax && *ax == r.ax && seq && *seq == r.seq && ok && *ok == r.ok &&
         temp && *temp == r.temp && alt && *alt == r.alt;
}

static void test_header_decode() {
  const std::vector<uint8_t> p =
      build_block_payload(7, 2, 0xAABBCCDDu, 5, 0x0FF0, {});
  SampleBlockHeader h;
  CHECK(decode_sample_block_header(p.data(), p.size(), h) == ChannelError::None);
  CHECK(h.channel_id == 7);
  CHECK(h.schema_version == 2);
  CHECK(h.base_ts_delta == 0xAABBCCDDu);
  CHECK(h.sample_count == 5);
  CHECK(h.flags == 0x0FF0);

  SampleBlockHeader h2;
  CHECK(decode_sample_block_header(p.data(), kSampleBlockHeaderSize - 1, h2) ==
        ChannelError::TruncatedHeader);
  CHECK(decode_sample_block_header(nullptr, 0, h2) ==
        ChannelError::TruncatedHeader);

  CHECK(std::strcmp(channel_error_str(ChannelError::None), "ok") == 0);
  CHECK(std::strcmp(channel_error_str(ChannelError::LengthMismatch),
                    "length-mismatch") == 0);
}

static void test_basic_ingest_via_wire() {
  SchemaRegistry reg;
  const ChannelSchema* v1 = reg.register_schema(imu_schema());
  CHECK(v1 != nullptr && v1->version == 1 && v1->stride == 19);

  const std::vector<uint8_t> payload =
      build_block_payload(3, 1, 100, 2, 0x00AB, imu_rows(0, 2));
  const Frame f = make_block_frame(1000, payload);

  std::vector<uint8_t> wire;
  encode_frame(f, wire);
  Frame decoded;
  size_t consumed = 0;
  CHECK(decode_frame(wire.data(), wire.size(), decoded, &consumed) ==
        FrameStatus::Ok);

  ChannelStore store;
  CHECK(store.ingest_frame(decoded, reg) == ChannelError::None);
  CHECK(store.total_samples() == 2);

  const ChannelSeries* s = store.series(3);
  CHECK(s != nullptr);
  CHECK(s->channel_id() == 3);
  CHECK(s->schema_version() == 1);
  CHECK(s->total_samples() == 2);
  CHECK(s->block_count() == 1);
  CHECK(s->block_sample_count(0) == 2);
  CHECK(s->block_flags(0) == 0x00AB);
  CHECK(s->block_base_ts(0) == 1000 + 100);

  const auto v0 = s->view(0);
  const auto v1v = s->view(1);
  CHECK(v0 && view_matches(*v0, imu_row(0)));
  CHECK(v1v && view_matches(*v1v, imu_row(1)));
  CHECK(!s->view(2));

  size_t len = 0;
  CHECK(s->sample_bytes(1, &len) != nullptr && len == 19);
  CHECK(s->sample_bytes(2, &len) == nullptr && len == 0);

  const std::vector<double> ax = store.column_as_doubles(3, 0);
  CHECK(ax.size() == 2);
  CHECK(ax[0] == static_cast<double>(imu_row(0).ax));
  CHECK(ax[1] == static_cast<double>(imu_row(1).ax));
}

static void test_multi_block_indexing() {
  SchemaRegistry reg;
  reg.register_schema(imu_schema());
  ChannelStore store;

  CHECK(store.ingest_frame(
            make_block_frame(1000, build_block_payload(3, 1, 0, 2, 0,
                                                       imu_rows(0, 2))),
            reg) == ChannelError::None);
  CHECK(store.ingest_frame(
            make_block_frame(2000, build_block_payload(3, 1, 0, 3, 0,
                                                       imu_rows(2, 5))),
            reg) == ChannelError::None);

  const ChannelSeries* s = store.series(3);
  CHECK(s != nullptr);
  CHECK(s->block_count() == 2);
  CHECK(s->total_samples() == 5);
  CHECK(store.total_samples() == 5);
  CHECK(s->block_base_ts(1) == 2000);

  for (int i = 0; i < 5; ++i) {
    const auto v = s->view(static_cast<size_t>(i));
    CHECK(v && view_matches(*v, imu_row(i)));
  }
  CHECK(!s->view(5));

  const std::vector<double> seq = store.column_as_doubles(3, 1);
  CHECK(seq.size() == 5);
  for (int i = 0; i < 5; ++i) {
    CHECK(seq[static_cast<size_t>(i)] == static_cast<double>(100 + i));
  }
}

static void test_multiple_channels_independent() {
  SchemaRegistry reg;
  reg.register_schema(imu_schema(3));
  const ChannelSchema* pw = reg.register_schema(
      make_schema(5, "power", {{"v", FieldType::U16}, {"i", FieldType::I16}}));
  CHECK(pw != nullptr && pw->stride == 4);

  ChannelStore store;
  CHECK(store.ingest_frame(
            make_block_frame(500, build_block_payload(3, 1, 0, 2, 0,
                                                      imu_rows(0, 2))),
            reg) == ChannelError::None);

  ByteWriter pwrows;
  pwrows.u16(10);
  pwrows.u16(static_cast<uint16_t>(-1));
  pwrows.u16(20);
  pwrows.u16(static_cast<uint16_t>(-2));
  CHECK(store.ingest_frame(
            make_block_frame(700, build_block_payload(5, 1, 0, 2, 0,
                                                      pwrows.buf())),
            reg) == ChannelError::None);

  const std::vector<uint16_t> chans = store.channels();
  CHECK(chans.size() == 2 && chans[0] == 3 && chans[1] == 5);
  CHECK(store.retained_samples(3) == 2);
  CHECK(store.retained_samples(5) == 2);
  CHECK(store.total_samples() == 4);

  const ChannelSeries* ps = store.series(5);
  CHECK(ps != nullptr);
  const auto pv0 = ps->view(0);
  const auto pv1 = ps->view(1);
  CHECK(pv0 && pv0->get_u16(0) && *pv0->get_u16(0) == 10);
  CHECK(pv0 && pv0->get_i16(1) && *pv0->get_i16(1) == -1);
  CHECK(pv1 && pv1->get_u16(0) && *pv1->get_u16(0) == 20);
  CHECK(pv1 && pv1->get_i16(1) && *pv1->get_i16(1) == -2);

  CHECK(store.compact_channel(3, 1) == ChannelError::None);
  CHECK(store.retained_samples(3) == 1);
  CHECK(store.retained_samples(5) == 2);
}

static void seed_five(SchemaRegistry& reg, ChannelStore& store) {
  reg.register_schema(imu_schema());
  store.ingest_frame(
      make_block_frame(1000, build_block_payload(3, 1, 0, 2, 0, imu_rows(0, 2))),
      reg);
  store.ingest_frame(
      make_block_frame(2000, build_block_payload(3, 1, 0, 3, 0, imu_rows(2, 5))),
      reg);
}

static void test_compaction_keep_last_within_block() {
  SchemaRegistry reg;
  ChannelStore store;
  seed_five(reg, store);

  CHECK(store.compact_channel(3, 2) == ChannelError::None);
  CHECK(store.retained_samples(3) == 2);
  const ChannelSeries* s = store.series(3);
  CHECK(s->total_samples() == 2);
  CHECK(s->block_count() == 1);
  const auto v0 = s->view(0);
  const auto v1 = s->view(1);
  CHECK(v0 && view_matches(*v0, imu_row(3)));
  CHECK(v1 && view_matches(*v1, imu_row(4)));
  CHECK(!s->view(2));

  const std::vector<double> alt = store.column_as_doubles(3, 4);
  CHECK(alt.size() == 2);
  CHECK(alt[0] == imu_row(3).alt);
  CHECK(alt[1] == imu_row(4).alt);
}

static void test_compaction_partial_block() {
  SchemaRegistry reg;
  ChannelStore store;
  seed_five(reg, store);

  CHECK(store.compact_channel(3, 4) == ChannelError::None);
  const ChannelSeries* s = store.series(3);
  CHECK(s->total_samples() == 4);
  CHECK(s->block_count() == 2);
  CHECK(s->block_sample_count(0) == 1);
  CHECK(s->block_sample_count(1) == 3);
  for (int i = 0; i < 4; ++i) {
    const auto v = s->view(static_cast<size_t>(i));
    CHECK(v && view_matches(*v, imu_row(i + 1)));
  }
  CHECK(!s->view(4));
}

static void test_compaction_edges() {
  SchemaRegistry reg;
  ChannelStore store;
  seed_five(reg, store);

  CHECK(store.compact_channel(3, 10) == ChannelError::None);
  CHECK(store.retained_samples(3) == 5);

  CHECK(store.compact_channel(3, 0) == ChannelError::None);
  CHECK(store.retained_samples(3) == 0);
  const ChannelSeries* s = store.series(3);
  CHECK(s != nullptr);
  CHECK(s->block_count() == 0);
  CHECK(!s->view(0));
  CHECK(store.column_as_doubles(3, 0).empty());

  CHECK(store.compact_channel(99, 1) == ChannelError::UnknownChannel);
}

static void test_error_cases() {
  SchemaRegistry reg;
  reg.register_schema(imu_schema());
  ChannelStore store;

  {
    Frame f = make_block_frame(0, build_block_payload(3, 1, 0, 1, 0,
                                                      imu_rows(0, 1)));
    f.header.type = RecordType::Marker;
    CHECK(store.ingest_frame(f, reg) == ChannelError::NotSampleBlock);
  }

  {
    Frame f = make_block_frame(0, {1, 2, 3});
    CHECK(store.ingest_frame(f, reg) == ChannelError::TruncatedHeader);
  }

  {
    Frame f = make_block_frame(0, build_block_payload(99, 1, 0, 1, 0,
                                                      imu_rows(0, 1)));
    CHECK(store.ingest_frame(f, reg) == ChannelError::UnknownChannel);
  }

  {
    Frame f = make_block_frame(0, build_block_payload(3, 2, 0, 1, 0,
                                                      imu_rows(0, 1)));
    CHECK(store.ingest_frame(f, reg) == ChannelError::UnknownVersion);
  }

  {
    Frame f = make_block_frame(0, build_block_payload(3, 1, 0, 0, 0, {}));
    CHECK(store.ingest_frame(f, reg) == ChannelError::ZeroCount);
  }

  {
    Frame f = make_block_frame(0, build_block_payload(3, 1, 0, 2, 0,
                                                      imu_rows(0, 1)));
    CHECK(store.ingest_frame(f, reg) == ChannelError::LengthMismatch);
  }

  {
    std::vector<uint8_t> p = build_block_payload(3, 1, 0, 1, 0, imu_rows(0, 1));
    p.push_back(0xFF);
    Frame f = make_block_frame(0, p);
    CHECK(store.ingest_frame(f, reg) == ChannelError::LengthMismatch);
  }

  CHECK(store.series(3) == nullptr);
  CHECK(store.total_samples() == 0);
}

static void test_version_mismatch_pinning() {
  SchemaRegistry reg;
  const ChannelSchema* v1 = reg.register_schema(imu_schema());
  CHECK(v1 && v1->version == 1);

  const ChannelSchema* v2 = reg.register_schema(make_schema(
      3, "imu",
      {{"ax", FieldType::F32}, {"seq", FieldType::U32}, {"ok", FieldType::U8},
       {"temp", FieldType::I16}, {"alt", FieldType::F64},
       {"extra", FieldType::U32}}));
  CHECK(v2 && v2->version == 2 && v2->stride == 23);

  ChannelStore store;

  CHECK(store.ingest_frame(
            make_block_frame(0, build_block_payload(3, 1, 0, 1, 0,
                                                    imu_rows(0, 1))),
            reg) == ChannelError::None);
  CHECK(store.series(3)->schema_version() == 1);

  std::vector<uint8_t> v2rows(23, 0);
  Frame f = make_block_frame(0, build_block_payload(3, 2, 0, 1, 0, v2rows));
  CHECK(store.ingest_frame(f, reg) == ChannelError::VersionMismatch);
  CHECK(store.series(3)->total_samples() == 1);
}

int main() {
  test_header_decode();
  test_basic_ingest_via_wire();
  test_multi_block_indexing();
  test_multiple_channels_independent();
  test_compaction_keep_last_within_block();
  test_compaction_partial_block();
  test_compaction_edges();
  test_error_cases();
  test_version_mismatch_pinning();
  if (failures) {
    std::fprintf(stderr, "%d checks failed\n", failures);
    return 1;
  }
  std::printf("all channel tests passed\n");
  return 0;
}
