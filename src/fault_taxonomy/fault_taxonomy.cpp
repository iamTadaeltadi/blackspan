#include "blackspan/fault_taxonomy.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <utility>

namespace blackspan {
namespace {

size_t severity_index(Severity s) {
  const size_t v = static_cast<size_t>(s);
  return v < 4 ? v : 3;
}

Severity worse_severity(Severity a, Severity b) {
  return static_cast<uint8_t>(b) > static_cast<uint8_t>(a) ? b : a;
}

std::string hex16(uint16_t value) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%04x", static_cast<unsigned>(value));
  return std::string(buf);
}

std::string hex32(uint32_t value) {
  char buf[12];
  std::snprintf(buf, sizeof(buf), "%08x", static_cast<unsigned>(value));
  return std::string(buf);
}

struct ClassAccumulator {
  size_t count = 0;
  Severity worst = Severity::Info;
  uint64_t first_ts = 0;
  uint64_t last_ts = 0;
  bool seen = false;
  std::set<uint32_t> devices;
};

struct EscalationEntry {
  uint64_t ts = 0;
  uint32_t seq = 0;
  Severity severity = Severity::Info;
};

}

std::vector<FaultClass> classify_faults(const std::vector<FaultEvent>& faults) {
  std::map<std::pair<uint16_t, uint8_t>, ClassAccumulator> groups;
  for (const FaultEvent& f : faults) {
    ClassAccumulator& acc = groups[{f.fault_code, f.subsystem_id}];
    if (!acc.seen) {
      acc.first_ts = f.monotonic_ts;
      acc.last_ts = f.monotonic_ts;
      acc.worst = f.severity;
      acc.seen = true;
    } else {
      if (f.monotonic_ts < acc.first_ts) acc.first_ts = f.monotonic_ts;
      if (f.monotonic_ts > acc.last_ts) acc.last_ts = f.monotonic_ts;
      acc.worst = worse_severity(acc.worst, f.severity);
    }
    acc.count += 1;
    acc.devices.insert(f.device_id);
  }

  std::vector<FaultClass> out;
  out.reserve(groups.size());
  for (const auto& kv : groups) {
    FaultClass c;
    c.fault_code = kv.first.first;
    c.subsystem_id = kv.first.second;
    c.count = kv.second.count;
    c.worst_severity = kv.second.worst;
    c.first_ts = kv.second.first_ts;
    c.last_ts = kv.second.last_ts;
    c.device_count = kv.second.devices.size();
    out.push_back(c);
  }

  std::stable_sort(out.begin(), out.end(),
                   [](const FaultClass& a, const FaultClass& b) {
                     if (a.count != b.count) return a.count > b.count;
                     if (a.fault_code != b.fault_code) {
                       return a.fault_code < b.fault_code;
                     }
                     return a.subsystem_id < b.subsystem_id;
                   });
  return out;
}

std::vector<SubsystemLoad> subsystem_load(
    const std::vector<FaultEvent>& faults) {
  std::map<uint8_t, SubsystemLoad> loads;
  for (const FaultEvent& f : faults) {
    auto it = loads.find(f.subsystem_id);
    if (it == loads.end()) {
      SubsystemLoad fresh;
      fresh.subsystem_id = f.subsystem_id;
      it = loads.emplace(f.subsystem_id, fresh).first;
    }
    SubsystemLoad& load = it->second;
    load.total += 1;
    load.by_severity[severity_index(f.severity)] += 1;
  }

  std::vector<SubsystemLoad> out;
  out.reserve(loads.size());
  for (const auto& kv : loads) out.push_back(kv.second);
  return out;
}

std::vector<Recurrence> recurring_faults(const std::vector<FaultEvent>& faults,
                                         size_t min_occurrences) {
  std::map<std::pair<uint16_t, uint32_t>, std::vector<uint64_t>> groups;
  for (const FaultEvent& f : faults) {
    groups[{f.fault_code, f.device_id}].push_back(f.monotonic_ts);
  }

  std::vector<Recurrence> out;
  for (auto& kv : groups) {
    std::vector<uint64_t>& stamps = kv.second;
    if (stamps.size() < min_occurrences) continue;
    std::sort(stamps.begin(), stamps.end());

    Recurrence r;
    r.fault_code = kv.first.first;
    r.device_id = kv.first.second;
    r.occurrences = stamps.size();
    if (stamps.size() >= 2) {
      uint64_t sum = 0;
      uint64_t smallest = UINT64_MAX;
      for (size_t i = 1; i < stamps.size(); ++i) {
        const uint64_t gap = stamps[i] - stamps[i - 1];
        sum += gap;
        if (gap < smallest) smallest = gap;
      }
      r.mean_interval_ns = sum / (stamps.size() - 1);
      r.min_interval_ns = smallest;
    }
    out.push_back(r);
  }

  std::stable_sort(out.begin(), out.end(),
                   [](const Recurrence& a, const Recurrence& b) {
                     if (a.occurrences != b.occurrences) {
                       return a.occurrences > b.occurrences;
                     }
                     if (a.fault_code != b.fault_code) {
                       return a.fault_code < b.fault_code;
                     }
                     return a.device_id < b.device_id;
                   });
  return out;
}

std::vector<EscalationRun> severity_escalations(
    const std::vector<FaultEvent>& faults, size_t min_length) {
  std::map<uint32_t, std::vector<EscalationEntry>> by_device;
  for (const FaultEvent& f : faults) {
    EscalationEntry e;
    e.ts = f.monotonic_ts;
    e.seq = f.session_seq;
    e.severity = f.severity;
    by_device[f.device_id].push_back(e);
  }

  std::vector<EscalationRun> out;
  for (auto& kv : by_device) {
    std::vector<EscalationEntry>& entries = kv.second;
    std::stable_sort(entries.begin(), entries.end(),
                     [](const EscalationEntry& a, const EscalationEntry& b) {
                       if (a.ts != b.ts) return a.ts < b.ts;
                       return a.seq < b.seq;
                     });

    const size_t n = entries.size();
    size_t start = 0;
    for (size_t i = 1; i <= n; ++i) {
      const bool boundary =
          i == n || static_cast<uint8_t>(entries[i].severity) <
                        static_cast<uint8_t>(entries[i - 1].severity);
      if (!boundary) continue;

      const size_t length = i - start;
      const Severity first = entries[start].severity;
      const Severity peak = entries[i - 1].severity;
      if (length >= min_length &&
          static_cast<uint8_t>(peak) > static_cast<uint8_t>(first)) {
        EscalationRun run;
        run.device_id = kv.first;
        run.start_ts = entries[start].ts;
        run.end_ts = entries[i - 1].ts;
        run.length = length;
        run.peak = peak;
        out.push_back(run);
      }
      start = i;
    }
  }

  std::stable_sort(out.begin(), out.end(),
                   [](const EscalationRun& a, const EscalationRun& b) {
                     if (a.device_id != b.device_id) {
                       return a.device_id < b.device_id;
                     }
                     return a.start_ts < b.start_ts;
                   });
  return out;
}

TaxonomySummary summarize_taxonomy(const std::vector<FaultEvent>& faults) {
  TaxonomySummary s;
  s.total_faults = faults.size();
  if (faults.empty()) return s;

  std::set<uint16_t> codes;
  std::set<uint8_t> subsystems;
  std::map<uint16_t, size_t> code_counts;
  Severity worst = Severity::Info;
  for (const FaultEvent& f : faults) {
    codes.insert(f.fault_code);
    subsystems.insert(f.subsystem_id);
    code_counts[f.fault_code] += 1;
    worst = worse_severity(worst, f.severity);
  }

  s.distinct_codes = codes.size();
  s.distinct_subsystems = subsystems.size();
  s.worst = worst;

  bool have = false;
  uint16_t top = 0;
  size_t top_count = 0;
  for (const auto& kv : code_counts) {
    if (!have || kv.second > top_count) {
      have = true;
      top = kv.first;
      top_count = kv.second;
    }
  }
  s.top_code = top;
  s.top_code_count = top_count;
  return s;
}

std::string describe_fault_class(const FaultClass& c) {
  std::string out = "fault " + hex16(c.fault_code);
  out += " subsystem " + std::to_string(static_cast<unsigned>(c.subsystem_id));
  out += " count " + std::to_string(c.count);
  out += " worst " + std::string(severity_name(c.worst_severity));
  out += " span [" + std::to_string(c.first_ts) + "," +
         std::to_string(c.last_ts) + "]";
  out += " devices " + std::to_string(c.device_count);
  return out;
}

std::string describe_subsystem_load(const SubsystemLoad& l) {
  std::string out =
      "subsystem " + std::to_string(static_cast<unsigned>(l.subsystem_id));
  out += " total " + std::to_string(l.total);
  out += " (info " + std::to_string(l.by_severity[0]);
  out += " warning " + std::to_string(l.by_severity[1]);
  out += " error " + std::to_string(l.by_severity[2]);
  out += " critical " + std::to_string(l.by_severity[3]) + ")";
  return out;
}

std::string describe_recurrence(const Recurrence& r) {
  std::string out = "fault " + hex16(r.fault_code);
  out += " device " + hex32(r.device_id);
  out += " x" + std::to_string(r.occurrences);
  out += " mean " + std::to_string(r.mean_interval_ns) + "ns";
  out += " min " + std::to_string(r.min_interval_ns) + "ns";
  return out;
}

std::string describe_escalation(const EscalationRun& e) {
  std::string out = "device " + hex32(e.device_id);
  out += " escalation length " + std::to_string(e.length);
  out += " span [" + std::to_string(e.start_ts) + "," +
         std::to_string(e.end_ts) + "]";
  out += " peak " + std::string(severity_name(e.peak));
  return out;
}

std::string describe_taxonomy(const TaxonomySummary& s) {
  std::string out = "taxonomy total " + std::to_string(s.total_faults);
  out += " codes " + std::to_string(s.distinct_codes);
  out += " subsystems " + std::to_string(s.distinct_subsystems);
  out += " worst " + std::string(severity_name(s.worst));
  out += " top " + hex16(s.top_code);
  out += " x" + std::to_string(s.top_code_count);
  return out;
}

std::string render_taxonomy(const std::vector<FaultClass>& classes) {
  char head[48];
  std::snprintf(head, sizeof(head), "%zu fault class(es)\n", classes.size());
  std::string out = head;
  for (const FaultClass& c : classes) {
    out += "  ";
    out += describe_fault_class(c);
    out += "\n";
  }
  return out;
}

std::vector<FaultClass> taxonomy_of(const DecodedRecording& rec) {
  return classify_faults(rec.anomalies().faults());
}

}
