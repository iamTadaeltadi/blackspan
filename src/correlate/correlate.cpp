#include "blackspan/correlate.h"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <map>
#include <set>

#include "blackspan/anomaly.h"
#include "blackspan/channel.h"
#include "blackspan/session.h"

namespace blackspan {
namespace {

constexpr size_t kMergeReserve = 256;

bool event_order(const CorrelatedEvent& a, const CorrelatedEvent& b) {
  if (a.ts != b.ts) return a.ts < b.ts;
  if (a.cls != b.cls) {
    return static_cast<uint8_t>(a.cls) < static_cast<uint8_t>(b.cls);
  }
  return a.code < b.code;
}

class DeviceAttribution {
 public:
  explicit DeviceAttribution(const std::vector<Session>& sessions) {
    for (const Session& s : sessions) {
      Span sp;
      sp.device_id = s.device_id;
      sp.lo = s.begin_ts;
      sp.hi = s.closed ? s.end_ts : std::numeric_limits<uint64_t>::max();
      if (sp.hi < sp.lo) sp.hi = sp.lo;
      spans_.push_back(sp);
    }
  }

  uint32_t device_for(uint64_t ts) const {
    bool have = false;
    uint32_t best_dev = 0;
    uint64_t best_lo = 0;
    for (const Span& sp : spans_) {
      if (ts < sp.lo || ts > sp.hi) continue;
      const bool better =
          !have || sp.lo > best_lo ||
          (sp.lo == best_lo && sp.device_id < best_dev);
      if (better) {
        have = true;
        best_lo = sp.lo;
        best_dev = sp.device_id;
      }
    }
    if (have) return best_dev;

    bool near = false;
    uint32_t near_dev = 0;
    uint64_t near_gap = 0;
    for (const Span& sp : spans_) {
      const uint64_t gap = (ts >= sp.lo) ? (ts - sp.lo) : (sp.lo - ts);
      const bool better =
          !near || gap < near_gap ||
          (gap == near_gap && sp.device_id < near_dev);
      if (better) {
        near = true;
        near_gap = gap;
        near_dev = sp.device_id;
      }
    }
    return near ? near_dev : 0u;
  }

 private:
  struct Span {
    uint32_t device_id = 0;
    uint64_t lo = 0;
    uint64_t hi = 0;
  };
  std::vector<Span> spans_;
};

void tally_classes(const std::vector<CorrelatedEvent>& evs, size_t begin,
                   size_t end, size_t& det, size_t& flt, size_t& tr) {
  det = 0;
  flt = 0;
  tr = 0;
  for (size_t k = begin; k <= end; ++k) {
    switch (evs[k].cls) {
      case EventClass::Detection: ++det; break;
      case EventClass::Fault: ++flt; break;
      case EventClass::Transition: ++tr; break;
    }
  }
}

}

const char* event_class_name(EventClass c) {
  switch (c) {
    case EventClass::Detection: return "detection";
    case EventClass::Fault: return "fault";
    case EventClass::Transition: return "transition";
  }
  return "unknown";
}

std::vector<Correlation> correlate_events(
    const std::vector<CorrelatedEvent>& events, const CorrelationConfig& cfg) {
  std::map<uint32_t, std::vector<CorrelatedEvent>> by_device;
  for (const CorrelatedEvent& e : events) {
    by_device[e.device_id].push_back(e);
  }

  std::vector<Correlation> out;
  for (auto& kv : by_device) {
    std::vector<CorrelatedEvent>& evs = kv.second;
    std::stable_sort(evs.begin(), evs.end(), event_order);

    const size_t n = evs.size();
    size_t i = 0;
    while (i < n) {
      const uint64_t anchor = evs[i].ts;
      size_t j = i;
      while (j + 1 < n && evs[j + 1].ts - anchor <= cfg.window_ns) ++j;

      const size_t count = j - i + 1;
      size_t det = 0;
      size_t flt = 0;
      size_t tr = 0;
      tally_classes(evs, i, j, det, flt, tr);
      const size_t distinct =
          (det > 0 ? 1u : 0u) + (flt > 0 ? 1u : 0u) + (tr > 0 ? 1u : 0u);

      if (count >= cfg.min_members && distinct >= 2) {
        Correlation c;
        c.device_id = kv.first;
        c.anchor_ts = anchor;
        c.detections = det;
        c.faults = flt;
        c.transitions = tr;
        c.members.assign(evs.begin() + static_cast<std::ptrdiff_t>(i),
                         evs.begin() + static_cast<std::ptrdiff_t>(j + 1));
        out.push_back(std::move(c));
        i = j + 1;
      } else {
        ++i;
      }
    }
  }
  return out;
}

std::vector<CorrelatedEvent> gather_events(
    const DecodedRecording& rec, const std::vector<DetectRule>& rules) {
  std::vector<CorrelatedEvent> out;

  const std::vector<Session>& sessions = rec.sessions().sessions();
  const std::vector<FaultEvent>& faults = rec.anomalies().faults();
  const std::vector<TransitionEvent>& transitions = rec.anomalies().transitions();

  std::set<uint32_t> devices;
  for (const Session& s : sessions) devices.insert(s.device_id);
  for (const FaultEvent& f : faults) devices.insert(f.device_id);
  for (const TransitionEvent& t : transitions) devices.insert(t.device_id);

  const DeviceAttribution attribution(sessions);
  const bool single_device = devices.size() == 1;
  const uint32_t only_device = single_device ? *devices.begin() : 0u;

  const std::vector<Detection> hits = run_detectors(rec.channels(), rules);
  for (const Detection& d : hits) {
    CorrelatedEvent e;
    e.cls = EventClass::Detection;
    e.device_id = single_device ? only_device : attribution.device_for(d.ts);
    e.ts = d.ts;
    e.code = d.rule_id;
    out.push_back(e);
  }

  for (const FaultEvent& f : faults) {
    CorrelatedEvent e;
    e.cls = EventClass::Fault;
    e.device_id = f.device_id;
    e.ts = f.monotonic_ts;
    e.code = f.fault_code;
    out.push_back(e);
  }

  for (const TransitionEvent& t : transitions) {
    CorrelatedEvent e;
    e.cls = EventClass::Transition;
    e.device_id = t.device_id;
    e.ts = t.monotonic_ts;
    e.code = (static_cast<uint32_t>(t.from_state) << 16) |
             static_cast<uint32_t>(t.to_state);
    out.push_back(e);
  }

  return out;
}

std::vector<Correlation> correlate_recording(
    const DecodedRecording& rec, const std::vector<DetectRule>& rules,
    const CorrelationConfig& cfg) {
  return correlate_events(gather_events(rec, rules), cfg);
}

uint64_t correlation_span(const Correlation& c) {
  if (c.members.empty()) return 0;
  uint64_t lo = c.members.front().ts;
  uint64_t hi = lo;
  for (const CorrelatedEvent& e : c.members) {
    if (e.ts < lo) lo = e.ts;
    if (e.ts > hi) hi = e.ts;
  }
  return hi - lo;
}

bool correlation_contains(const Correlation& c, EventClass cls) {
  switch (cls) {
    case EventClass::Detection: return c.detections > 0;
    case EventClass::Fault: return c.faults > 0;
    case EventClass::Transition: return c.transitions > 0;
  }
  return false;
}

uint64_t correlation_strength(const Correlation& c) {
  const uint64_t members = c.detections + c.faults + c.transitions;
  const uint64_t distinct = (c.detections > 0 ? 1u : 0u) +
                            (c.faults > 0 ? 1u : 0u) +
                            (c.transitions > 0 ? 1u : 0u);
  return members * 1000u + static_cast<uint64_t>(c.faults) * 100u +
         static_cast<uint64_t>(c.transitions) * 10u + distinct;
}

std::vector<size_t> rank_correlations(const std::vector<Correlation>& cs) {
  std::vector<size_t> idx(cs.size());
  for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
  std::stable_sort(idx.begin(), idx.end(), [&cs](size_t a, size_t b) {
    const uint64_t sa = correlation_strength(cs[a]);
    const uint64_t sb = correlation_strength(cs[b]);
    if (sa != sb) return sa > sb;
    if (cs[a].device_id != cs[b].device_id) {
      return cs[a].device_id < cs[b].device_id;
    }
    return cs[a].anchor_ts < cs[b].anchor_ts;
  });
  return idx;
}

std::vector<Correlation> correlations_for_device(
    const std::vector<Correlation>& cs, uint32_t device) {
  std::vector<Correlation> out;
  for (const Correlation& c : cs) {
    if (c.device_id == device) out.push_back(c);
  }
  return out;
}

std::vector<Correlation> correlations_with_class(
    const std::vector<Correlation>& cs, EventClass cls) {
  std::vector<Correlation> out;
  for (const Correlation& c : cs) {
    if (correlation_contains(c, cls)) out.push_back(c);
  }
  return out;
}

std::string describe_correlation(const Correlation& c) {
  const uint64_t lo = c.members.empty() ? c.anchor_ts : c.members.front().ts;
  const uint64_t hi = lo + correlation_span(c);

  char head[32];
  std::snprintf(head, sizeof(head), "%08x", static_cast<unsigned>(c.device_id));

  std::string out = "device ";
  out += head;
  out += " anchor ts " + std::to_string(c.anchor_ts);
  out += " span [" + std::to_string(lo) + "," + std::to_string(hi) + "]";
  out += " members " + std::to_string(c.members.size());
  out += " (det " + std::to_string(c.detections);
  out += " fault " + std::to_string(c.faults);
  out += " trans " + std::to_string(c.transitions) + ")";
  return out;
}

std::vector<Correlation> merge_correlations(const std::vector<Correlation>& cs,
                                            uint64_t span_ns) {
  std::vector<Correlation> out;
  out.reserve(cs.size());
  std::vector<const CorrelatedEvent*> anchors;
  std::vector<const CorrelatedEvent*> tails;
  anchors.reserve(cs.size());
  tails.reserve(cs.size());

  for (const Correlation& c : cs) {
    const bool extend =
        !out.empty() && out.back().device_id == c.device_id &&
        c.anchor_ts >= out.back().anchor_ts &&
        c.anchor_ts - out.back().anchor_ts <= span_ns;
    if (extend) {
      Correlation& dst = out.back();
      dst.members.insert(dst.members.end(), c.members.begin(), c.members.end());
      dst.detections += c.detections;
      dst.faults += c.faults;
      dst.transitions += c.transitions;
    } else {
      Correlation copy = c;
      copy.members.reserve(copy.members.size() + kMergeReserve);
      out.push_back(std::move(copy));
      const bool empty = out.back().members.empty();
      anchors.push_back(empty ? nullptr : &out.back().members.front());
      tails.push_back(empty ? nullptr : &out.back().members.back());
    }
  }

  for (size_t i = 0; i < out.size(); ++i) {
    if (anchors[i] != nullptr) out[i].anchor_ts = anchors[i]->ts;
  }
  for (size_t i = 0; i < out.size(); ++i) {
    if (tails[i] != nullptr) out[i].last_member_ts = tails[i]->ts;
  }
  return out;
}

std::string render_correlations(const std::vector<Correlation>& cs) {
  char head[48];
  std::snprintf(head, sizeof(head), "%zu correlation(s)\n", cs.size());
  std::string out = head;
  for (const Correlation& c : cs) {
    out += "  ";
    out += describe_correlation(c);
    out += "\n";
  }
  return out;
}

CorrelationSummary summarize_correlations(const std::vector<Correlation>& cs) {
  CorrelationSummary s;
  s.total = cs.size();
  std::set<uint32_t> devices;
  for (const Correlation& c : cs) {
    s.detections += c.detections;
    s.faults += c.faults;
    s.transitions += c.transitions;
    devices.insert(c.device_id);
    const size_t members = c.detections + c.faults + c.transitions;
    if (members > s.largest_members) s.largest_members = members;
    const uint64_t span = correlation_span(c);
    if (span > s.widest_span) s.widest_span = span;
  }
  s.device_count = devices.size();
  return s;
}

std::string describe_correlation_summary(const CorrelationSummary& s) {
  std::string out = "correlations " + std::to_string(s.total);
  out += " across " + std::to_string(s.device_count) + " device(s)";
  out += " det " + std::to_string(s.detections);
  out += " fault " + std::to_string(s.faults);
  out += " trans " + std::to_string(s.transitions);
  out += " largest " + std::to_string(s.largest_members);
  out += " widest-span " + std::to_string(s.widest_span);
  return out;
}

std::vector<Coincidence> detection_fault_coincidences(
    const std::vector<CorrelatedEvent>& events, uint64_t window_ns) {
  std::map<uint32_t, std::vector<uint64_t>> fault_ts;
  std::map<uint32_t, std::vector<uint32_t>> fault_code;
  for (const CorrelatedEvent& e : events) {
    if (e.cls != EventClass::Fault) continue;
    fault_ts[e.device_id].push_back(e.ts);
    fault_code[e.device_id].push_back(e.code);
  }
  for (auto& kv : fault_ts) {
    std::vector<uint32_t>& codes = fault_code[kv.first];
    std::vector<size_t> order(kv.second.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&kv](size_t a, size_t b) {
                       return kv.second[a] < kv.second[b];
                     });
    std::vector<uint64_t> sorted_ts(order.size());
    std::vector<uint32_t> sorted_codes(order.size());
    for (size_t i = 0; i < order.size(); ++i) {
      sorted_ts[i] = kv.second[order[i]];
      sorted_codes[i] = codes[order[i]];
    }
    kv.second.swap(sorted_ts);
    codes.swap(sorted_codes);
  }

  std::vector<Coincidence> out;
  for (const CorrelatedEvent& e : events) {
    if (e.cls != EventClass::Detection) continue;
    auto it = fault_ts.find(e.device_id);
    if (it == fault_ts.end()) continue;
    const std::vector<uint64_t>& tss = it->second;
    const std::vector<uint32_t>& codes = fault_code[e.device_id];

    const auto lb = std::lower_bound(tss.begin(), tss.end(), e.ts);
    size_t best = tss.size();
    uint64_t best_gap = 0;
    if (lb != tss.end()) {
      const size_t idx = static_cast<size_t>(lb - tss.begin());
      best = idx;
      best_gap = tss[idx] - e.ts;
    }
    if (lb != tss.begin()) {
      const size_t idx = static_cast<size_t>((lb - tss.begin()) - 1);
      const uint64_t gap = e.ts - tss[idx];
      if (best == tss.size() || gap <= best_gap) {
        best = idx;
        best_gap = gap;
      }
    }
    if (best == tss.size() || best_gap > window_ns) continue;

    Coincidence c;
    c.device_id = e.device_id;
    c.detection_ts = e.ts;
    c.detection_code = e.code;
    c.fault_ts = tss[best];
    c.fault_code = codes[best];
    c.gap = best_gap;
    out.push_back(c);
  }

  std::stable_sort(out.begin(), out.end(),
                   [](const Coincidence& a, const Coincidence& b) {
                     if (a.device_id != b.device_id) {
                       return a.device_id < b.device_id;
                     }
                     if (a.detection_ts != b.detection_ts) {
                       return a.detection_ts < b.detection_ts;
                     }
                     return a.detection_code < b.detection_code;
                   });
  return out;
}

std::string describe_coincidence(const Coincidence& c) {
  char head[32];
  std::snprintf(head, sizeof(head), "%08x", static_cast<unsigned>(c.device_id));
  std::string out = "device ";
  out += head;
  out += " detection code " + std::to_string(c.detection_code);
  out += " at ts " + std::to_string(c.detection_ts);
  out += " near fault code " + std::to_string(c.fault_code);
  out += " at ts " + std::to_string(c.fault_ts);
  out += " gap " + std::to_string(c.gap);
  return out;
}

}
