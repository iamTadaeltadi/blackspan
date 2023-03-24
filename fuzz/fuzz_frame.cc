#include <cstddef>
#include <cstdint>
#include <vector>

#include "blackspan/derive.h"
#include "blackspan/frame.h"
#include "blackspan/report.h"

using namespace blackspan;

namespace {
volatile size_t g_sink = 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {

  {
    Frame one;
    size_t consumed = 0;
    FrameStatus st = decode_frame(data, size, one, &consumed);
    g_sink += static_cast<size_t>(st);
    g_sink += consumed;
  }

  FrameStream stream(data, size);
  Frame frame;
  while (stream.next(frame)) {
    g_sink += static_cast<size_t>(frame.header.type);
    g_sink += frame.payload.size();

    std::vector<uint8_t> reencoded;
    size_t n = encode_frame(frame, reencoded);
    g_sink += n;

    switch (frame.header.type) {
      case RecordType::ExportRequest: {
        ExportRequest req;
        ExportError err = decode_export_request(frame.payload.data(),
                                                frame.payload.size(), req);
        g_sink += static_cast<size_t>(err);
        break;
      }
      case RecordType::DerivedDef: {
        DerivedDefRecord rec;
        DeriveError err = decode_derived_def(frame.payload.data(),
                                             frame.payload.size(), rec);
        g_sink += static_cast<size_t>(err);
        break;
      }
      default:
        break;
    }
  }
  g_sink += static_cast<size_t>(stream.status());
  g_sink += stream.offset();
  return 0;
}
