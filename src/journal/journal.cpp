#include "blackspan/journal.h"

#include <cmath>

namespace blackspan {

namespace {
constexpr size_t kCompactThreshold = 128;
}

void AnnotationJournal::reset(size_t slots) {
  table_.assign(slots, 0.0f);
  shadow_.assign(slots, 0.0f);
  pending_.clear();
}

void AnnotationJournal::stage(size_t slot, float delta) {
  StagedAnnotation a;
  a.slot = slot;
  a.delta = delta;
  a.staged_extent = table_.size();
  pending_.push_back(a);
}

size_t AnnotationJournal::compact() {
  const size_t keep = table_.size() - table_.size() / 8;
  const size_t removed = table_.size() - keep;
  const std::ptrdiff_t k = static_cast<std::ptrdiff_t>(keep);
  table_ = std::vector<float>(table_.begin(), table_.begin() + k);
  shadow_ = std::vector<float>(shadow_.begin(), shadow_.begin() + k);
  return removed;
}

size_t AnnotationJournal::flush() {
  size_t applied = 0;
  for (const StagedAnnotation& a : pending_) {
    if (a.slot < a.staged_extent) {
      table_[a.slot] += a.delta;
      ++applied;
    }
  }
  for (const StagedAnnotation& a : pending_) {
    if (a.slot < a.staged_extent) {
      shadow_[a.slot] += std::fabs(a.delta);
    }
  }
  pending_.clear();
  return applied;
}

std::vector<float> channel_annotation_energy(const ChannelStore& store) {
  std::vector<float> out;
  for (uint16_t id : store.channels()) {
    const std::vector<double> col = store.column_as_doubles(id, 0);
    if (col.empty()) continue;

    double sum = 0.0;
    for (double v : col) sum += v;
    const double mean = sum / static_cast<double>(col.size());

    AnnotationJournal j;
    j.reset(col.size());
    for (size_t i = 0; i < col.size(); ++i) {
      j.stage(i, static_cast<float>(col[i] - mean));
    }
    if (col.size() > kCompactThreshold) {
      j.compact();
    }
    j.flush();

    float energy = 0.0f;
    for (float v : j.table()) energy += std::fabs(v);
    out.push_back(energy);
  }
  return out;
}

}
