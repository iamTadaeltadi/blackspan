#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "blackspan/byteio.h"
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

static std::vector<uint8_t> build_payload(uint16_t channel_id,
                                          uint8_t declared_version,
                                          const std::string& name,
                                          const std::vector<FieldDef>& fields) {
  ByteWriter w;
  w.u16(channel_id);
  w.u8(declared_version);
  w.u8(static_cast<uint8_t>(fields.size()));
  w.u8(static_cast<uint8_t>(name.size()));
  w.bytes(reinterpret_cast<const uint8_t*>(name.data()), name.size());
  for (const auto& f : fields) {
    w.u8(static_cast<uint8_t>(f.second));
    w.u8(static_cast<uint8_t>(f.first.size()));
    w.bytes(reinterpret_cast<const uint8_t*>(f.first.data()), f.first.size());
  }
  return w.buf();
}

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

static void test_field_type_helpers() {
  struct Row {
    FieldType t;
    size_t size;
    const char* name;
    bool is_signed;
    bool is_float;
  };
  const Row rows[] = {
      {FieldType::U8, 1, "u8", false, false},
      {FieldType::I8, 1, "i8", true, false},
      {FieldType::U16, 2, "u16", false, false},
      {FieldType::I16, 2, "i16", true, false},
      {FieldType::U32, 4, "u32", false, false},
      {FieldType::I32, 4, "i32", true, false},
      {FieldType::U64, 8, "u64", false, false},
      {FieldType::I64, 8, "i64", true, false},
      {FieldType::F32, 4, "f32", false, true},
      {FieldType::F64, 8, "f64", false, true},
  };
  for (const Row& r : rows) {
    CHECK(field_type_size(r.t) == r.size);
    CHECK(std::strcmp(field_type_name(r.t), r.name) == 0);
    CHECK(field_type_is_signed(r.t) == r.is_signed);
    CHECK(field_type_is_float(r.t) == r.is_float);
    const auto parsed = parse_field_type(static_cast<uint8_t>(r.t));
    CHECK(parsed && *parsed == r.t);
  }
  CHECK(!parse_field_type(0));
  CHECK(!parse_field_type(11));
  CHECK(!parse_field_type(255));
}

static void test_layout_computation() {
  ChannelSchema s = make_schema(
      3, "imu",
      {{"ax", FieldType::F32}, {"ok", FieldType::U8}, {"seq", FieldType::U32},
       {"ts", FieldType::U64}, {"temp", FieldType::I16}});

  CHECK(s.fields[0].offset == 0);
  CHECK(s.fields[1].offset == 4);
  CHECK(s.fields[2].offset == 5);
  CHECK(s.fields[3].offset == 9);
  CHECK(s.fields[4].offset == 17);
  CHECK(s.stride == 19);
  CHECK(s.validate() == SchemaError::None);

  CHECK(s.field_index("seq") && *s.field_index("seq") == 2);
  CHECK(!s.field_index("missing"));
  const SchemaField* f = s.field_by_name("ts");
  CHECK(f && f->type == FieldType::U64 && f->offset == 9);
  CHECK(s.field_by_name("nope") == nullptr);

  const std::string d = s.describe();
  CHECK(d.find("channel 3") != std::string::npos);
  CHECK(d.find("imu") != std::string::npos);
  CHECK(d.find("stride 19") != std::string::npos);
}

static void test_validate_paths() {
  ChannelSchema empty;
  empty.channel_id = 1;
  CHECK(empty.validate() == SchemaError::ZeroFields);

  ChannelSchema dup = make_schema(1, "c", {{"x", FieldType::U8},
                                           {"x", FieldType::U16}});
  CHECK(dup.validate() == SchemaError::DuplicateFieldName);

  ChannelSchema en = make_schema(1, "c", {{"", FieldType::U8}});
  CHECK(en.validate() == SchemaError::EmptyName);

  ChannelSchema longname =
      make_schema(1, std::string(kMaxSchemaNameLen + 1, 'a'),
                  {{"x", FieldType::U8}});
  CHECK(longname.validate() == SchemaError::NameTooLong);

  ChannelSchema wide = make_schema(1, "c", {{"a", FieldType::U64},
                                            {"b", FieldType::U64}});
  CHECK(wide.stride == 16);
  CHECK(wide.validate(8) == SchemaError::StrideTooLarge);
  CHECK(wide.validate() == SchemaError::None);
}

static void test_decode_round_trip() {
  const std::vector<FieldDef> fields = {
      {"altitude", FieldType::F64}, {"valid", FieldType::U8},
      {"count", FieldType::U32},    {"bias", FieldType::I16}};
  const std::vector<uint8_t> payload = build_payload(42, 7, "baro", fields);

  const SchemaDecodeResult r =
      decode_channel_schema(payload.data(), payload.size());
  CHECK(r.ok());
  CHECK(r.error == SchemaError::None);
  CHECK(r.declared_version == 7);
  CHECK(r.schema.channel_id == 42);
  CHECK(r.schema.name == "baro");
  CHECK(r.schema.version == 0);
  CHECK(r.schema.fields.size() == 4);
  CHECK(r.schema.fields[0].name == "altitude");
  CHECK(r.schema.fields[0].type == FieldType::F64);
  CHECK(r.schema.fields[0].offset == 0);
  CHECK(r.schema.fields[1].offset == 8);
  CHECK(r.schema.fields[2].offset == 9);
  CHECK(r.schema.fields[3].offset == 13);
  CHECK(r.schema.stride == 15);
}

static void test_decode_errors() {
  const std::vector<FieldDef> ok_fields = {{"x", FieldType::U16},
                                           {"y", FieldType::U16}};

  {
    const std::vector<uint8_t> p = {1, 0, 0};
    const auto r = decode_channel_schema(p.data(), p.size());
    CHECK(r.error == SchemaError::Truncated);
  }

  {
    const auto r = decode_channel_schema(nullptr, 0);
    CHECK(r.error == SchemaError::Truncated);
  }

  {
    const auto p = build_payload(1, 0, "c", {});
    const auto r = decode_channel_schema(p.data(), p.size());
    CHECK(r.error == SchemaError::ZeroFields);
  }

  {
    ByteWriter w;
    w.u16(1);
    w.u8(0);
    w.u8(1);
    w.u8(static_cast<uint8_t>(kMaxSchemaNameLen + 1));
    const auto r = decode_channel_schema(w.buf().data(), w.buf().size());
    CHECK(r.error == SchemaError::NameTooLong);
  }

  {
    auto p = build_payload(1, 0, "c", {{"x", FieldType::U8}});
    p[6] = 99;
    const auto r = decode_channel_schema(p.data(), p.size());
    CHECK(r.error == SchemaError::BadFieldType);
  }

  {
    const auto p = build_payload(1, 0, "c", {{"", FieldType::U8}});
    const auto r = decode_channel_schema(p.data(), p.size());
    CHECK(r.error == SchemaError::EmptyName);
  }

  {
    ByteWriter w;
    w.u16(1);
    w.u8(0);
    w.u8(1);
    w.u8(1);
    w.u8('c');
    w.u8(static_cast<uint8_t>(FieldType::U8));
    w.u8(static_cast<uint8_t>(kMaxSchemaNameLen + 1));
    const auto r = decode_channel_schema(w.buf().data(), w.buf().size());
    CHECK(r.error == SchemaError::NameTooLong);
  }

  {
    const auto p = build_payload(1, 0, "c", {{"a", FieldType::U8},
                                             {"a", FieldType::U8}});
    const auto r = decode_channel_schema(p.data(), p.size());
    CHECK(r.error == SchemaError::DuplicateFieldName);
  }

  {
    auto p = build_payload(1, 0, "c", ok_fields);
    p.resize(p.size() - 2);
    const auto r = decode_channel_schema(p.data(), p.size());
    CHECK(r.error == SchemaError::Truncated);
  }

  {
    auto p = build_payload(1, 0, "c", ok_fields);
    p.push_back(0xFF);
    const auto r = decode_channel_schema(p.data(), p.size());
    CHECK(r.error == SchemaError::TrailingBytes);
  }
}

static void test_registry_versioning() {
  SchemaRegistry reg;

  const ChannelSchema a = make_schema(10, "gps", {{"lat", FieldType::F64},
                                                  {"lon", FieldType::F64}});
  SchemaError err = SchemaError::TrailingBytes;
  const ChannelSchema* v1 = reg.register_schema(a, &err);
  CHECK(v1 != nullptr);
  CHECK(err == SchemaError::None);
  CHECK(v1->version == 1);
  CHECK(reg.count() == 1);
  CHECK(reg.total_versions() == 1);
  CHECK(reg.contains(10));
  CHECK(!reg.contains(11));

  const ChannelSchema* again = reg.register_schema(a);
  CHECK(again == v1);
  CHECK(again->version == 1);
  CHECK(reg.total_versions() == 1);

  const ChannelSchema b = make_schema(10, "gps", {{"lat", FieldType::F64},
                                                  {"lon", FieldType::F64},
                                                  {"fix", FieldType::U8}});
  const ChannelSchema* v2 = reg.register_schema(b);
  CHECK(v2 != nullptr);
  CHECK(v2->version == 2);
  CHECK(reg.count() == 1);
  CHECK(reg.total_versions() == 2);

  const auto vers = reg.versions(10);
  CHECK(vers.size() == 2 && vers[0] == 1 && vers[1] == 2);

  CHECK(reg.latest(10) == v2);
  CHECK(reg.get(10, 1) != nullptr);
  CHECK(reg.get(10, 1)->fields.size() == 2);
  CHECK(reg.get(10, 2)->fields.size() == 3);
  CHECK(reg.get(10, 3) == nullptr);
  CHECK(reg.get(99, 1) == nullptr);
  CHECK(reg.latest(99) == nullptr);
  CHECK(reg.versions(99).empty());

  reg.register_schema(make_schema(5, "power", {{"v", FieldType::U16}}));
  CHECK(reg.count() == 2);
  const auto chans = reg.channels();
  CHECK(chans.size() == 2 && chans[0] == 5 && chans[1] == 10);

  ChannelSchema bad = make_schema(7, "x", {{"", FieldType::U8}});
  SchemaError bad_err = SchemaError::None;
  CHECK(reg.register_schema(bad, &bad_err) == nullptr);
  CHECK(bad_err == SchemaError::EmptyName);
  CHECK(!reg.contains(7));
  CHECK(reg.count() == 2);
}

static void test_ingest_frame() {
  SchemaRegistry reg;
  const std::vector<uint8_t> payload =
      build_payload(21, 1, "vibe", {{"amp", FieldType::F32},
                                    {"phase", FieldType::I32}});

  Frame f;
  f.header.type = RecordType::SchemaDef;
  f.payload = payload;
  CHECK(reg.ingest_frame(f) == SchemaError::None);
  CHECK(reg.contains(21));
  CHECK(reg.latest(21)->fields.size() == 2);

  Frame other;
  other.header.type = RecordType::SampleBlock;
  other.payload = payload;
  CHECK(reg.ingest_frame(other) == SchemaError::NotSchemaFrame);

  Frame broken;
  broken.header.type = RecordType::SchemaDef;
  broken.payload = {1, 0, 0};
  CHECK(reg.ingest_frame(broken) == SchemaError::Truncated);

  std::vector<uint8_t> wire;
  encode_frame(f, wire);
  Frame decoded;
  size_t consumed = 0;
  CHECK(decode_frame(wire.data(), wire.size(), decoded, &consumed) ==
        FrameStatus::Ok);
  SchemaRegistry reg2;
  CHECK(reg2.ingest_frame(decoded) == SchemaError::None);
  CHECK(reg2.contains(21));
}

static void test_sample_view() {
  const ChannelSchema s = make_schema(
      1, "all",
      {{"a", FieldType::U8}, {"b", FieldType::I8}, {"c", FieldType::U16},
       {"d", FieldType::I16}, {"e", FieldType::U32}, {"f", FieldType::I32},
       {"g", FieldType::U64}, {"h", FieldType::I64}, {"i", FieldType::F32},
       {"j", FieldType::F64}});
  CHECK(s.stride == 42);

  const float f32v = -3.5f;
  const double f64v = 2.5;
  uint32_t f32bits;
  uint64_t f64bits;
  std::memcpy(&f32bits, &f32v, 4);
  std::memcpy(&f64bits, &f64v, 8);

  ByteWriter w;
  w.u8(200);
  w.u8(0xFB);
  w.u16(60000);
  w.u16(static_cast<uint16_t>(-1000));
  w.u32(4000000000u);
  w.u32(static_cast<uint32_t>(-123456));
  w.u64(0x0102030405060708ull);
  w.u64(static_cast<uint64_t>(-9000000000LL));
  w.u32(f32bits);
  w.u64(f64bits);
  const std::vector<uint8_t>& buf = w.buf();
  CHECK(buf.size() == s.stride);

  SampleView sv(s, buf.data(), buf.size());
  CHECK(sv.complete());
  CHECK(sv.field_count() == 10);

  CHECK(sv.get_u8(0) && *sv.get_u8(0) == 200);
  CHECK(sv.get_i8(1) && *sv.get_i8(1) == -5);
  CHECK(sv.get_u16(2) && *sv.get_u16(2) == 60000);
  CHECK(sv.get_i16(3) && *sv.get_i16(3) == -1000);
  CHECK(sv.get_u32(4) && *sv.get_u32(4) == 4000000000u);
  CHECK(sv.get_i32(5) && *sv.get_i32(5) == -123456);
  CHECK(sv.get_u64(6) && *sv.get_u64(6) == 0x0102030405060708ull);
  CHECK(sv.get_i64(7) && *sv.get_i64(7) == -9000000000LL);
  CHECK(sv.get_f32(8) && *sv.get_f32(8) == -3.5f);
  CHECK(sv.get_f64(9) && *sv.get_f64(9) == 2.5);

  CHECK(sv.get_as_double(0) && *sv.get_as_double(0) == 200.0);
  CHECK(sv.get_as_double(1) && *sv.get_as_double(1) == -5.0);
  CHECK(sv.get_as_double(3) && *sv.get_as_double(3) == -1000.0);
  CHECK(sv.get_as_double(5) && *sv.get_as_double(5) == -123456.0);
  CHECK(sv.get_as_double(7) && *sv.get_as_double(7) == -9000000000.0);
  CHECK(sv.get_as_double(8) && *sv.get_as_double(8) == -3.5);
  CHECK(sv.get_as_double(9) && *sv.get_as_double(9) == 2.5);

  CHECK(!sv.get_u32(0));
  CHECK(!sv.get_f64(8));
  CHECK(!sv.get_i16(2));

  CHECK(!sv.get_u8(10));
  CHECK(!sv.get_as_double(10));
  CHECK(!sv.get_u8(1000));
}

static void test_sample_view_short_span() {
  const ChannelSchema s = make_schema(
      1, "s", {{"a", FieldType::U8}, {"b", FieldType::U16},
               {"c", FieldType::U32}});
  CHECK(s.stride == 7);

  const std::vector<uint8_t> buf = {0x11, 0x22, 0x33};
  SampleView sv(s, buf.data(), buf.size());
  CHECK(!sv.complete());
  CHECK(sv.get_u8(0) && *sv.get_u8(0) == 0x11);
  CHECK(sv.get_u16(1) && *sv.get_u16(1) == 0x3322);
  CHECK(!sv.get_u32(2));
  CHECK(!sv.get_as_double(2));

  SampleView empty(s, nullptr, 0);
  CHECK(!empty.complete());
  CHECK(!empty.get_u8(0));
  CHECK(!empty.get_as_double(0));
}

int main() {
  test_field_type_helpers();
  test_layout_computation();
  test_validate_paths();
  test_decode_round_trip();
  test_decode_errors();
  test_registry_versioning();
  test_ingest_frame();
  test_sample_view();
  test_sample_view_short_span();
  if (failures) {
    std::fprintf(stderr, "%d checks failed\n", failures);
    return 1;
  }
  std::printf("all schema tests passed\n");
  return 0;
}
