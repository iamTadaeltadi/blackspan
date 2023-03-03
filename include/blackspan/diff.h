#ifndef BLACKSPAN_DIFF_H
#define BLACKSPAN_DIFF_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "blackspan/pipeline.h"

// The diff subsystem compares two reconstructed recordings and reports how they
// differ. The comparison a service engineer wants is structural, not a byte
// diff: "this returned unit ran the same schema as the golden bench recording
// but logged three extra faults and one fewer session". This subsystem answers
// that by walking the two reconstructed models in parallel and reporting the
// sessions, channels, schema versions, and anomaly counts that appear on only
// one side or that changed between them.
//
// The two inputs are named "left" (the baseline) and "right" (the comparand).
// The diff reads only the reconstructed models; it mutates nothing and never
// touches raw bytes. Every comparison is a pure function of its inputs. Nothing
// here throws.

namespace blackspan {

// Which side of a comparison a difference lies on.
enum class DiffSide : uint8_t {
  LeftOnly = 0,   // present in the baseline, absent in the comparand
  RightOnly = 1,  // present in the comparand, absent in the baseline
  Changed = 2,    // present in both but differing
};

// Lowercase name of a side ("left-only", "right-only", "changed"). Never null.
const char* diff_side_name(DiffSide s);

// The category of thing that differs, so a caller can filter without string
// matching.
enum class DiffCategory : uint8_t {
  Session = 0,        // a reconstructed session
  Channel = 1,        // a channel's presence or sample count
  SchemaVersion = 2,  // a channel's schema versions or layout
  FaultCount = 3,     // the fault count for a severity
  EventCount = 4,     // a transition or config-change count
  Diagnostic = 5,     // a session/anomaly diagnostic count
};

// Lowercase name of a category. Never null.
const char* diff_category_name(DiffCategory c);

// One reported difference. `key` names the entity (a device id, a channel id, a
// severity name); `detail` describes the difference in words, including the
// left and right values for a Changed difference.
struct Difference {
  DiffCategory category = DiffCategory::Session;
  DiffSide side = DiffSide::Changed;
  std::string key;
  std::string detail;
};

// A single-line rendering, e.g.
// `changed channel 1: samples 6 -> 9`.
std::string describe_difference(const Difference& d);

// Aggregate counts for a diff.
struct DiffSummary {
  size_t left_only = 0;
  size_t right_only = 0;
  size_t changed = 0;

  size_t total() const { return left_only + right_only + changed; }
  bool identical() const { return total() == 0; }
};

// The structural difference between two recordings.
class RecordingDiff {
 public:
  static RecordingDiff compare(const DecodedRecording& left,
                              const DecodedRecording& right);

  const std::vector<Difference>& differences() const { return diffs_; }
  const DiffSummary& summary() const { return summary_; }

  // Differences of a given category, as pointers into this diff's storage.
  std::vector<const Difference*> in_category(DiffCategory c) const;

  // True when the two recordings are structurally identical.
  bool identical() const { return summary_.identical(); }

 private:
  std::vector<Difference> diffs_;
  DiffSummary summary_;
};

// Renders a diff to a deterministic multi-line block: one line per difference,
// followed by a summary line. Two identical recordings render a single
// "identical: no structural differences" line.
std::string render_diff(const RecordingDiff& diff);

}  // namespace blackspan

#endif  // BLACKSPAN_DIFF_H
