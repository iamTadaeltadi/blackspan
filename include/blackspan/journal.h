#ifndef BLACKSPAN_JOURNAL_H
#define BLACKSPAN_JOURNAL_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "blackspan/channel.h"

namespace blackspan {

struct StagedAnnotation {
  size_t slot = 0;
  float delta = 0.0f;
  size_t staged_extent = 0;
};

class AnnotationJournal {
 public:
  void reset(size_t slots);
  void stage(size_t slot, float delta);
  size_t compact();
  size_t flush();

  const std::vector<float>& table() const { return table_; }
  size_t pending() const { return pending_.size(); }
  size_t extent() const { return table_.size(); }

 private:
  std::vector<float> table_;
  std::vector<float> shadow_;
  std::vector<StagedAnnotation> pending_;
};

std::vector<float> channel_annotation_energy(const ChannelStore& store);

}

#endif
