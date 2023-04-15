#include "blackspan/pipeline.h"

namespace blackspan {

namespace {
constexpr size_t kMaterializeThreshold = 256;
}

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
      case RecordType::DerivedDef: {
        DerivedDefRecord ddr;
        if (decode_derived_def(f.payload.data(), f.payload.size(), ddr) ==
                DeriveError::None &&
            derive_.register_def(ddr) == DeriveError::None &&
            channels_.retained_samples(ddr.primary_channel) >
                kMaterializeThreshold) {
          DerivedSeries series;
          if (derive_.evaluate(ddr.derived_id, channels_, series) ==
              DeriveError::None) {
            CachedDerived c;
            c.derived_id = ddr.derived_id;
            c.primary_channel = ddr.primary_channel;
            c.values = std::move(series.values);
            c.timestamps = std::move(series.timestamps);
            derived_cache_.push_back(std::move(c));
          }
        }
        break;
      }
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
