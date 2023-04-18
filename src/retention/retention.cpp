#include "blackspan/retention.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace blackspan {

RetentionPriority priority_of(RecordType t) {
  switch (t) {
    case RecordType::Fault:
      return RetentionPriority::Faults;
    case RecordType::StateTransition:
      return RetentionPriority::StateTransitions;
    case RecordType::SessionBegin:
    case RecordType::SessionEnd:
    case RecordType::Reconnect:
      return RetentionPriority::Sessions;
    case RecordType::SampleBlock:
      return RetentionPriority::Samples;
    case RecordType::Marker:
      return RetentionPriority::Markers;
    case RecordType::ConfigChange:
      return RetentionPriority::ConfigChanges;
    case RecordType::SchemaDef:
    case RecordType::Checkpoint:
    case RecordType::DerivedDef:
    case RecordType::ExportRequest:
      return RetentionPriority::Other;
  }
  return RetentionPriority::Other;
}

const char* retention_priority_name(RetentionPriority p) {
  switch (p) {
    case RetentionPriority::Faults:
      return "faults";
    case RetentionPriority::StateTransitions:
      return "state-transitions";
    case RetentionPriority::Sessions:
      return "sessions";
    case RetentionPriority::Samples:
      return "samples";
    case RetentionPriority::Markers:
      return "markers";
    case RetentionPriority::ConfigChanges:
      return "config-changes";
    case RetentionPriority::Other:
      return "other";
  }
  return "other";
}

std::vector<RetentionPriority> default_priority_order() {
  return {
      RetentionPriority::Faults,      RetentionPriority::StateTransitions,
      RetentionPriority::Sessions,    RetentionPriority::ConfigChanges,
      RetentionPriority::Markers,     RetentionPriority::Samples,
      RetentionPriority::Other,
  };
}

bool RetentionPolicy::keeps(const FrameHeader& h) const {
  if (h.monotonic_ts < keep_since_ts) return false;
  if (h.monotonic_ts > keep_until_ts) return false;
  if (drop_samples && h.type == RecordType::SampleBlock) return false;
  if (drop_markers && h.type == RecordType::Marker) return false;
  return true;
}

namespace {

struct Candidate {
  size_t index = 0;
  uint64_t ts = 0;
  RecordType type = RecordType::Marker;
  RetentionPriority priority = RetentionPriority::Other;
  size_t bytes = 0;
};

bool is_session_framing(RecordType t) {
  return t == RecordType::SessionBegin || t == RecordType::SessionEnd;
}

size_t encoded_size(const Frame& f) {
  return kFrameHeaderSize + f.payload.size() + kFrameTrailerSize;
}

std::vector<RetentionPriority> normalize_order(
    const std::vector<RetentionPriority>& order) {
  std::vector<RetentionPriority> out;
  std::array<bool, kRetentionPriorityCount> seen{};
  for (RetentionPriority p : order) {
    size_t idx = static_cast<size_t>(p);
    if (idx >= kRetentionPriorityCount) continue;
    if (seen[idx]) continue;
    seen[idx] = true;
    out.push_back(p);
  }
  return out;
}

size_t priority_rank(RetentionPriority p,
                     const std::vector<RetentionPriority>& order) {
  for (size_t i = 0; i < order.size(); ++i) {
    if (order[i] == p) return i;
  }
  return order.size() + static_cast<size_t>(p);
}

std::vector<char> select_within_budget(const std::vector<Candidate>& candidates,
                                        const RetentionPolicy& policy) {
  std::vector<char> chosen(candidates.size(), 0);

  const bool frame_limited = policy.max_frames != 0;
  const bool byte_limited = policy.max_bytes != 0;
  if (!frame_limited && !byte_limited) {
    std::fill(chosen.begin(), chosen.end(), static_cast<char>(1));
    return chosen;
  }
  if (frame_limited && !byte_limited &&
      candidates.size() <= policy.max_frames) {
    std::fill(chosen.begin(), chosen.end(), static_cast<char>(1));
    return chosen;
  }

  const std::vector<RetentionPriority> order =
      normalize_order(policy.priority_order);

  std::vector<size_t> ranking(candidates.size());
  for (size_t i = 0; i < ranking.size(); ++i) ranking[i] = i;

  std::sort(ranking.begin(), ranking.end(), [&](size_t a, size_t b) {
    const Candidate& ca = candidates[a];
    const Candidate& cb = candidates[b];
    const size_t ra = priority_rank(ca.priority, order);
    const size_t rb = priority_rank(cb.priority, order);
    if (ra != rb) return ra < rb;
    if (ca.ts != cb.ts) return ca.ts > cb.ts;
    return ca.index < cb.index;
  });

  size_t frames_used = 0;
  size_t bytes_used = 0;
  auto pin = [&](size_t i) {
    if (chosen[i]) return;
    chosen[i] = 1;
    ++frames_used;
    bytes_used += candidates[i].bytes;
  };

  if (policy.keep_session_framing) {
    for (size_t i = 0; i < candidates.size(); ++i) {
      if (is_session_framing(candidates[i].type)) pin(i);
    }
  }
  if (policy.keep_tail != 0 && !candidates.empty()) {
    const size_t start = candidates.size() > policy.keep_tail
                             ? candidates.size() - policy.keep_tail
                             : 0;
    for (size_t i = start; i < candidates.size(); ++i) pin(i);
  }

  for (size_t r = 0; r < ranking.size(); ++r) {
    const size_t idx = ranking[r];
    if (chosen[idx]) continue;
    if (frame_limited && frames_used >= policy.max_frames) break;
    const Candidate& c = candidates[idx];
    if (byte_limited && bytes_used + c.bytes > policy.max_bytes) continue;
    chosen[idx] = 1;
    ++frames_used;
    bytes_used += c.bytes;
  }
  return chosen;
}

}

RetentionPlan plan_retention(const uint8_t* data, size_t len,
                             const RetentionPolicy& policy) {
  RetentionPlan plan;

  std::vector<Candidate> candidates;
  FrameStream fs(data, len);
  Frame f;
  size_t index = 0;
  while (fs.next(f)) {
    const size_t sz = encoded_size(f);
    ++plan.frames_in;
    plan.bytes_in += sz;
    if (policy.keeps(f.header)) {
      Candidate c;
      c.index = index;
      c.ts = f.header.monotonic_ts;
      c.type = f.header.type;
      c.priority = priority_of(f.header.type);
      c.bytes = sz;
      candidates.push_back(c);
    }
    ++index;
  }
  plan.input_status = fs.status();

  const std::vector<char> chosen = select_within_budget(candidates, policy);
  for (size_t i = 0; i < candidates.size(); ++i) {
    if (!chosen[i]) continue;
    const Candidate& c = candidates[i];
    plan.kept_indices.push_back(c.index);
    plan.bytes_kept += c.bytes;
    plan.kept_by_priority[static_cast<size_t>(c.priority)] += 1;
    ++plan.frames_kept;
  }

  return plan;
}

RetentionPlan plan_retention(const std::vector<uint8_t>& data,
                             const RetentionPolicy& policy) {
  return plan_retention(data.data(), data.size(), policy);
}

std::vector<uint8_t> apply_retention(const uint8_t* data, size_t len,
                                     const RetentionPolicy& policy) {
  const RetentionPlan plan = plan_retention(data, len, policy);

  std::vector<uint8_t> out;
  if (plan.kept_indices.empty()) return out;

  FrameStream fs(data, len);
  Frame f;
  size_t index = 0;
  size_t next = 0;
  while (fs.next(f)) {
    if (next < plan.kept_indices.size() && plan.kept_indices[next] == index) {
      encode_frame(f.header, f.payload.data(), f.payload.size(), out);
      ++next;
    }
    ++index;
  }
  return out;
}

std::vector<uint8_t> apply_retention(const std::vector<uint8_t>& data,
                                     const RetentionPolicy& policy) {
  return apply_retention(data.data(), data.size(), policy);
}

double retention_ratio(const RetentionPlan& plan) {
  if (plan.frames_in == 0) return 0.0;
  return static_cast<double>(plan.frames_kept) /
         static_cast<double>(plan.frames_in);
}

size_t frames_dropped(const RetentionPlan& plan) {
  if (plan.frames_kept >= plan.frames_in) return 0;
  return plan.frames_in - plan.frames_kept;
}

size_t bytes_dropped(const RetentionPlan& plan) {
  if (plan.bytes_kept >= plan.bytes_in) return 0;
  return plan.bytes_in - plan.bytes_kept;
}

std::vector<size_t> dropped_indices(const RetentionPlan& plan) {
  std::vector<size_t> out;
  size_t next = 0;
  for (size_t i = 0; i < plan.frames_in; ++i) {
    if (next < plan.kept_indices.size() && plan.kept_indices[next] == i) {
      ++next;
      continue;
    }
    out.push_back(i);
  }
  return out;
}

std::string describe_plan(const RetentionPlan& plan) {
  std::ostringstream os;
  os << "retention: " << plan.frames_kept << "/" << plan.frames_in
     << " frames kept, " << plan.bytes_kept << "/" << plan.bytes_in
     << " bytes, dropped " << frames_dropped(plan) << " frames ("
     << bytes_dropped(plan) << " bytes), ratio ";
  os << std::fixed << std::setprecision(3) << retention_ratio(plan);
  os << ", kept[";
  for (size_t i = 0; i < kRetentionPriorityCount; ++i) {
    if (i != 0) os << ' ';
    os << retention_priority_name(static_cast<RetentionPriority>(i)) << '='
       << plan.kept_by_priority[i];
  }
  os << ']';
  if (plan.input_status != FrameStatus::Ok &&
      plan.input_status != FrameStatus::Incomplete) {
    os << ", input " << frame_status_str(plan.input_status);
  } else if (plan.input_status == FrameStatus::Incomplete) {
    os << ", input truncated";
  }
  return os.str();
}

std::string describe_policy(const RetentionPolicy& policy) {
  std::ostringstream os;
  os << "policy: window[" << policy.keep_since_ts << ',';
  if (policy.keep_until_ts == UINT64_MAX) {
    os << "max";
  } else {
    os << policy.keep_until_ts;
  }
  os << "], max_frames=" << policy.max_frames << ", max_bytes="
     << policy.max_bytes << ", keep_tail=" << policy.keep_tail
     << ", drop_samples="
     << (policy.drop_samples ? "true" : "false") << ", drop_markers="
     << (policy.drop_markers ? "true" : "false") << ", keep_session_framing="
     << (policy.keep_session_framing ? "true" : "false") << ", priority[";
  for (size_t i = 0; i < policy.priority_order.size(); ++i) {
    if (i != 0) os << ' ';
    os << retention_priority_name(policy.priority_order[i]);
  }
  os << ']';
  return os.str();
}

}
