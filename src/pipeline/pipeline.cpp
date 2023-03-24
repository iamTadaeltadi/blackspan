#include "blackspan/pipeline.h"

namespace blackspan {

void DecodedRecording::decode(const uint8_t* data, size_t len) {
  FrameStream fs(data, len);
  Frame f;
  while (fs.next(f)) {
    ++frame_count_;
    switch (f.header.type) {
      case RecordType::SchemaDef:
        schemas_.ingest_frame(f);
        break;
      case RecordType::SampleBlock:
        channels_.ingest_frame(f, schemas_);
        break;
      default:
        break;
    }

    sessions_.accept(f);
    anomalies_.accept(f);
  }
  sessions_.finish();
  status_ = fs.status();
}

}
