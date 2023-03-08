// Standalone driver for the fuzz targets, used for local replay only.
//
// ClusterFuzzLite links libFuzzer, which supplies its own main; this file is
// NOT part of that build. It exists so a target can be built with just an
// AddressSanitizer toolchain (no libFuzzer runtime) and replayed over the
// checked-in corpus: each command-line path is read whole and handed to the
// target once, and with no paths the driver reads a single input from stdin.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

namespace {

std::vector<uint8_t> read_all(std::FILE* f) {
  std::vector<uint8_t> buf;
  uint8_t chunk[4096];
  size_t n;
  while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
    buf.insert(buf.end(), chunk, chunk + n);
  }
  return buf;
}

int run_file(const char* path) {
  std::FILE* f = std::fopen(path, "rb");
  if (!f) {
    std::fprintf(stderr, "cannot open %s\n", path);
    return 1;
  }
  std::vector<uint8_t> buf = read_all(f);
  std::fclose(f);
  LLVMFuzzerTestOneInput(buf.data(), buf.size());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::vector<uint8_t> buf = read_all(stdin);
    LLVMFuzzerTestOneInput(buf.data(), buf.size());
    return 0;
  }
  int rc = 0;
  for (int i = 1; i < argc; ++i) {
    rc |= run_file(argv[i]);
  }
  return rc;
}
