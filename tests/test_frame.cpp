#include <cstdio>
#include <cstdint>
#include <vector>

#include "blackspan/crc.h"
#include "blackspan/frame.h"

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

static void test_crc_known_vectors() {
  const char* s = "123456789";
  CHECK(crc32(reinterpret_cast<const uint8_t*>(s), 9) == 0xCBF43926u);
  CHECK(crc16_ccitt(reinterpret_cast<const uint8_t*>(s), 9) == 0x29B1);
}

static void test_frame_round_trip() {
  FrameHeader h;
  h.type = RecordType::SampleBlock;
  h.device_id = 0xAABBCCDDu;
  h.session_seq = 7;
  h.monotonic_ts = 0x0102030405060708ull;
  h.flags = 0x1234;
  std::vector<uint8_t> payload = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

  std::vector<uint8_t> wire;
  const size_t n = encode_frame(h, payload.data(), payload.size(), wire);
  CHECK(n == kFrameHeaderSize + payload.size() + kFrameTrailerSize);
  CHECK(wire.size() == n);

  Frame f;
  size_t consumed = 0;
  CHECK(decode_frame(wire.data(), wire.size(), f, &consumed) == FrameStatus::Ok);
  CHECK(consumed == n);
  CHECK(f.header.type == RecordType::SampleBlock);
  CHECK(f.header.device_id == 0xAABBCCDDu);
  CHECK(f.header.session_seq == 7);
  CHECK(f.header.monotonic_ts == 0x0102030405060708ull);
  CHECK(f.header.flags == 0x1234);
  CHECK(f.payload == payload);
}

static void test_truncation() {
  FrameHeader h;
  h.type = RecordType::Marker;
  std::vector<uint8_t> wire;
  encode_frame(h, nullptr, 0, wire);
  Frame f;
  size_t consumed = 0;
  CHECK(decode_frame(wire.data(), wire.size() - 1, f, &consumed) ==
        FrameStatus::Incomplete);
}

static void test_corrupt_header_crc() {
  FrameHeader h;
  h.type = RecordType::Marker;
  std::vector<uint8_t> payload = {9, 9, 9};
  std::vector<uint8_t> wire;
  encode_frame(h, payload.data(), payload.size(), wire);
  wire[5] ^= 0xFF;
  Frame f;
  size_t consumed = 0;
  CHECK(decode_frame(wire.data(), wire.size(), f, &consumed) ==
        FrameStatus::HeaderCrcMismatch);
}

static void test_corrupt_payload_crc() {
  FrameHeader h;
  h.type = RecordType::Fault;
  std::vector<uint8_t> payload = {1, 2, 3, 4};
  std::vector<uint8_t> wire;
  encode_frame(h, payload.data(), payload.size(), wire);
  wire[kFrameHeaderSize + 1] ^= 0xFF;
  Frame f;
  size_t consumed = 0;
  CHECK(decode_frame(wire.data(), wire.size(), f, &consumed) ==
        FrameStatus::PayloadCrcMismatch);
}

static void test_stream() {
  std::vector<uint8_t> wire;
  for (int i = 0; i < 5; ++i) {
    FrameHeader h;
    h.type = RecordType::Marker;
    h.session_seq = static_cast<uint32_t>(i);
    std::vector<uint8_t> p = {static_cast<uint8_t>(i)};
    encode_frame(h, p.data(), p.size(), wire);
  }
  FrameStream fs(wire.data(), wire.size());
  Frame f;
  int count = 0;
  uint32_t seqsum = 0;
  while (fs.next(f)) {
    ++count;
    seqsum += f.header.session_seq;
  }
  CHECK(count == 5);
  CHECK(seqsum == 0u + 1u + 2u + 3u + 4u);
  CHECK(fs.status() == FrameStatus::Ok);
}

int main() {
  test_crc_known_vectors();
  test_frame_round_trip();
  test_truncation();
  test_corrupt_header_crc();
  test_corrupt_payload_crc();
  test_stream();
  if (failures) {
    std::fprintf(stderr, "%d checks failed\n", failures);
    return 1;
  }
  std::printf("all frame tests passed\n");
  return 0;
}
