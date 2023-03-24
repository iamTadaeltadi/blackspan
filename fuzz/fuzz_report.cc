#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "blackspan/pipeline.h"
#include "blackspan/report.h"
#include "blackspan/session.h"

using namespace blackspan;

namespace {
volatile size_t g_sink = 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  DecodedRecording rec;
  rec.decode(data, size);

  const std::vector<Session>& sessions = rec.sessions().sessions();
  ReportModel model;
  model.sessions = &sessions;
  model.schemas = &rec.schemas();
  model.channels = &rec.channels();
  model.anomalies = &rec.anomalies();

  uint16_t mask = size ? static_cast<uint16_t>(data[size - 1]) : kSectionAll;

  g_sink += render_text(model, mask).size();
  g_sink += render_json(model, mask).size();
  g_sink += render_dot(model, mask).size();
  g_sink += render(model, ReportFormat::Text, kSectionAll).size();

  if (size >= kExportRequestSize) {
    ExportRequest req;
    ExportError err =
        decode_export_request(data + size - kExportRequestSize,
                              kExportRequestSize, req);
    if (err == ExportError::None) {
      g_sink += render(model, req).size();
    }
  }
  return 0;
}
