#include <array>
#include <cstdio>
#include <map>
#include <set>

#include "blackspan/diff.h"

namespace blackspan {
namespace {

std::string hex8(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof(b), "%08x", v);
  return b;
}

std::map<uint64_t, const Session*> index_sessions(
    const std::vector<Session>& sessions) {
  std::map<uint64_t, const Session*> out;
  for (const Session& s : sessions) {
    uint64_t key = (static_cast<uint64_t>(s.device_id) << 32) | s.session_index;
    out[key] = &s;
  }
  return out;
}

std::string session_fingerprint(const Session& s) {
  char b[192];
  std::snprintf(b, sizeof(b),
                "fw=%u.%u.%u class=%u epoch=%u closed=%d reason=%u ckpt=%zu "
                "recon=%zu mark=%zu seqreg=%u tsreg=%u",
                s.fw_major, s.fw_minor, s.fw_patch, s.device_class,
                s.schema_epoch, s.closed ? 1 : 0, s.end_reason,
                s.checkpoints.size(), s.reconnects.size(), s.markers.size(),
                s.seq_regressions, s.ts_regressions);
  return b;
}

void diff_sessions(const DecodedRecording& left, const DecodedRecording& right,
                   std::vector<Difference>& out) {
  auto l = index_sessions(left.sessions().sessions());
  auto r = index_sessions(right.sessions().sessions());

  std::set<uint64_t> keys;
  for (const auto& kv : l) keys.insert(kv.first);
  for (const auto& kv : r) keys.insert(kv.first);

  for (uint64_t key : keys) {
    auto li = l.find(key);
    auto ri = r.find(key);
    uint32_t dev = static_cast<uint32_t>(key >> 32);
    uint32_t idx = static_cast<uint32_t>(key & 0xffffffffu);
    std::string k = "device " + hex8(dev) + " session #" + std::to_string(idx);

    if (li != l.end() && ri == r.end()) {
      Difference d;
      d.category = DiffCategory::Session;
      d.side = DiffSide::LeftOnly;
      d.key = k;
      d.detail = "session only in baseline";
      out.push_back(std::move(d));
    } else if (li == l.end() && ri != r.end()) {
      Difference d;
      d.category = DiffCategory::Session;
      d.side = DiffSide::RightOnly;
      d.key = k;
      d.detail = "session only in comparand";
      out.push_back(std::move(d));
    } else {
      std::string lf = session_fingerprint(*li->second);
      std::string rf = session_fingerprint(*ri->second);
      if (lf != rf) {
        Difference d;
        d.category = DiffCategory::Session;
        d.side = DiffSide::Changed;
        d.key = k;
        d.detail = "[" + lf + "] -> [" + rf + "]";
        out.push_back(std::move(d));
      }
    }
  }
}

void diff_channels(const DecodedRecording& left, const DecodedRecording& right,
                   std::vector<Difference>& out) {
  const ChannelStore& lc = left.channels();
  const ChannelStore& rc = right.channels();
  const SchemaRegistry& ls = left.schemas();
  const SchemaRegistry& rs = right.schemas();

  std::set<uint16_t> ids;
  for (uint16_t id : ls.channels()) ids.insert(id);
  for (uint16_t id : rs.channels()) ids.insert(id);

  for (uint16_t id : ids) {
    std::string k = "channel " + std::to_string(id);
    bool in_left = ls.contains(id);
    bool in_right = rs.contains(id);

    if (in_left && !in_right) {
      Difference d;
      d.category = DiffCategory::Channel;
      d.side = DiffSide::LeftOnly;
      d.key = k;
      d.detail = "channel only in baseline";
      out.push_back(std::move(d));
      continue;
    }
    if (!in_left && in_right) {
      Difference d;
      d.category = DiffCategory::Channel;
      d.side = DiffSide::RightOnly;
      d.key = k;
      d.detail = "channel only in comparand";
      out.push_back(std::move(d));
      continue;
    }

    size_t lv = ls.versions(id).size();
    size_t rv = rs.versions(id).size();
    if (lv != rv) {
      Difference d;
      d.category = DiffCategory::SchemaVersion;
      d.side = DiffSide::Changed;
      d.key = k;
      d.detail = "schema versions " + std::to_string(lv) + " -> " +
                 std::to_string(rv);
      out.push_back(std::move(d));
    } else {
      const ChannelSchema* lsc = ls.latest(id);
      const ChannelSchema* rsc = rs.latest(id);
      if (lsc && rsc && !lsc->same_layout(*rsc)) {
        Difference d;
        d.category = DiffCategory::SchemaVersion;
        d.side = DiffSide::Changed;
        d.key = k;
        d.detail = "latest layout differs (" + lsc->describe() + " -> " +
                   rsc->describe() + ")";
        out.push_back(std::move(d));
      }
    }

    const ChannelSeries* lseries = lc.series(id);
    const ChannelSeries* rseries = rc.series(id);
    size_t lsamp = lseries ? lseries->total_samples() : 0;
    size_t rsamp = rseries ? rseries->total_samples() : 0;
    if (lsamp != rsamp) {
      Difference d;
      d.category = DiffCategory::Channel;
      d.side = DiffSide::Changed;
      d.key = k;
      d.detail = "samples " + std::to_string(lsamp) + " -> " +
                 std::to_string(rsamp);
      out.push_back(std::move(d));
    }
  }
}

void diff_anomalies(const DecodedRecording& left, const DecodedRecording& right,
                    std::vector<Difference>& out) {
  const AnomalyEngine& la = left.anomalies();
  const AnomalyEngine& ra = right.anomalies();

  std::array<size_t, 4> lf = la.fault_count_by_severity();
  std::array<size_t, 4> rf = ra.fault_count_by_severity();
  const char* sev_names[4] = {"info", "warning", "error", "critical"};
  for (int i = 0; i < 4; ++i) {
    if (lf[i] != rf[i]) {
      Difference d;
      d.category = DiffCategory::FaultCount;
      d.side = DiffSide::Changed;
      d.key = std::string("fault ") + sev_names[i];
      d.detail = std::to_string(lf[i]) + " -> " + std::to_string(rf[i]);
      out.push_back(std::move(d));
    }
  }

  if (la.transitions().size() != ra.transitions().size()) {
    Difference d;
    d.category = DiffCategory::EventCount;
    d.side = DiffSide::Changed;
    d.key = "transitions";
    d.detail = std::to_string(la.transitions().size()) + " -> " +
               std::to_string(ra.transitions().size());
    out.push_back(std::move(d));
  }
  if (la.config_changes().size() != ra.config_changes().size()) {
    Difference d;
    d.category = DiffCategory::EventCount;
    d.side = DiffSide::Changed;
    d.key = "config-changes";
    d.detail = std::to_string(la.config_changes().size()) + " -> " +
               std::to_string(ra.config_changes().size());
    out.push_back(std::move(d));
  }

  size_t ld = la.diagnostics().size();
  size_t rd = ra.diagnostics().size();
  if (ld != rd) {
    Difference d;
    d.category = DiffCategory::Diagnostic;
    d.side = DiffSide::Changed;
    d.key = "anomaly-diagnostics";
    d.detail = std::to_string(ld) + " -> " + std::to_string(rd);
    out.push_back(std::move(d));
  }
}

}

const char* diff_side_name(DiffSide s) {
  switch (s) {
    case DiffSide::LeftOnly: return "left-only";
    case DiffSide::RightOnly: return "right-only";
    case DiffSide::Changed: return "changed";
  }
  return "unknown";
}

const char* diff_category_name(DiffCategory c) {
  switch (c) {
    case DiffCategory::Session: return "session";
    case DiffCategory::Channel: return "channel";
    case DiffCategory::SchemaVersion: return "schema-version";
    case DiffCategory::FaultCount: return "fault-count";
    case DiffCategory::EventCount: return "event-count";
    case DiffCategory::Diagnostic: return "diagnostic";
  }
  return "unknown";
}

std::string describe_difference(const Difference& d) {
  std::string s = diff_side_name(d.side);
  s += " ";
  s += d.key;
  s += ": ";
  s += d.detail;
  return s;
}

RecordingDiff RecordingDiff::compare(const DecodedRecording& left,
                                     const DecodedRecording& right) {
  RecordingDiff diff;

  diff_sessions(left, right, diff.diffs_);
  diff_channels(left, right, diff.diffs_);
  diff_anomalies(left, right, diff.diffs_);

  for (const Difference& d : diff.diffs_) {
    switch (d.side) {
      case DiffSide::LeftOnly: ++diff.summary_.left_only; break;
      case DiffSide::RightOnly: ++diff.summary_.right_only; break;
      case DiffSide::Changed: ++diff.summary_.changed; break;
    }
  }
  return diff;
}

std::vector<const Difference*> RecordingDiff::in_category(
    DiffCategory c) const {
  std::vector<const Difference*> out;
  for (const Difference& d : diffs_) {
    if (d.category == c) out.push_back(&d);
  }
  return out;
}

std::string render_diff(const RecordingDiff& diff) {
  if (diff.identical()) return "identical: no structural differences\n";

  std::string out;
  for (const Difference& d : diff.differences()) {
    out += describe_difference(d);
    out += "\n";
  }
  const DiffSummary& s = diff.summary();
  out += std::to_string(s.left_only) + " only in baseline, " +
         std::to_string(s.right_only) + " only in comparand, " +
         std::to_string(s.changed) + " changed\n";
  return out;
}

}
