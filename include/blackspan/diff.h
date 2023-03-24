#ifndef BLACKSPAN_DIFF_H
#define BLACKSPAN_DIFF_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "blackspan/pipeline.h"

namespace blackspan {

enum class DiffSide : uint8_t {
  LeftOnly = 0,
  RightOnly = 1,
  Changed = 2,
};

const char* diff_side_name(DiffSide s);

enum class DiffCategory : uint8_t {
  Session = 0,
  Channel = 1,
  SchemaVersion = 2,
  FaultCount = 3,
  EventCount = 4,
  Diagnostic = 5,
};

const char* diff_category_name(DiffCategory c);

struct Difference {
  DiffCategory category = DiffCategory::Session;
  DiffSide side = DiffSide::Changed;
  std::string key;
  std::string detail;
};

std::string describe_difference(const Difference& d);

struct DiffSummary {
  size_t left_only = 0;
  size_t right_only = 0;
  size_t changed = 0;

  size_t total() const { return left_only + right_only + changed; }
  bool identical() const { return total() == 0; }
};

class RecordingDiff {
 public:
  static RecordingDiff compare(const DecodedRecording& left,
                              const DecodedRecording& right);

  const std::vector<Difference>& differences() const { return diffs_; }
  const DiffSummary& summary() const { return summary_; }

  std::vector<const Difference*> in_category(DiffCategory c) const;

  bool identical() const { return summary_.identical(); }

 private:
  std::vector<Difference> diffs_;
  DiffSummary summary_;
};

std::string render_diff(const RecordingDiff& diff);

}

#endif
