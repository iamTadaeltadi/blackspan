#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "blackspan/frame.h"

namespace {

using namespace blackspan;

std::vector<uint8_t> read_file(const char* path, bool& ok) {
  ok = false;
  FILE* f = std::fopen(path, "rb");
  if (!f) return {};
  std::fseek(f, 0, SEEK_END);
  long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> buf;
  if (n > 0) {
    buf.resize(static_cast<size_t>(n));
    if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) {
      std::fclose(f);
      return {};
    }
  }
  std::fclose(f);
  ok = true;
  return buf;
}

int cmd_frames(const char* path) {
  bool ok = false;
  std::vector<uint8_t> buf = read_file(path, ok);
  if (!ok) {
    std::fprintf(stderr, "error: cannot read %s\n", path);
    return 2;
  }
  FrameStream fs(buf.data(), buf.size());
  Frame f;
  size_t idx = 0;
  while (fs.next(f)) {
    std::printf("#%zu %-16s dev=%08x seq=%u ts=%llu len=%u\n", idx++,
                record_type_name(f.header.type), f.header.device_id,
                f.header.session_seq,
                static_cast<unsigned long long>(f.header.monotonic_ts),
                f.header.payload_len);
  }
  if (fs.status() != FrameStatus::Ok &&
      fs.status() != FrameStatus::Incomplete) {
    std::fprintf(stderr, "decode stopped at offset %zu: %s\n", fs.offset(),
                 frame_status_str(fs.status()));
    return 1;
  }
  std::printf("%zu frames\n", idx);
  return 0;
}

void usage() {
  std::fprintf(stderr,
               "blackspan - offline diagnostic recorder decoder\n"
               "usage:\n"
               "  blackspan frames <recording.bsp>\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  const std::string cmd = argv[1];
  if (cmd == "frames") {
    if (argc < 3) {
      usage();
      return 2;
    }
    return cmd_frames(argv[2]);
  }
  usage();
  return 2;
}
