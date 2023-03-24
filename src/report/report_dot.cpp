#include <cstddef>
#include <set>
#include <string>
#include <vector>

#include "blackspan/report.h"
#include "report_internal.h"

namespace blackspan {

std::string render_dot(const ReportModel& model, uint16_t section_mask) {
  using report_detail::device_hex;
  using report_detail::want;

  std::string out;
  out += "digraph blackspan {\n";
  out += "  rankdir=LR;\n";
  out += "  node [fontname=\"monospace\"];\n";

  if (want(section_mask, kSectionSessions, model.sessions)) {
    const std::vector<Session>& sessions = *model.sessions;
    for (size_t i = 0; i < sessions.size(); ++i) {
      const Session& s = sessions[i];
      std::string label = "dev " + device_hex(s.device_id) + " #" +
                          std::to_string(s.session_index);
      if (!s.label.empty()) label += " " + s.label;
      out += "  session_" + std::to_string(i) + " [shape=box, label=\"" +
             escape_dot(label) + "\"];\n";
    }
  }

  if (want(section_mask, kSectionChannels, model.channels)) {
    for (uint16_t id : model.channels->channels()) {
      const ChannelSeries* series = model.channels->series(id);
      if (!series) continue;
      const ChannelSchema& sc = series->schema();
      std::string label = sc.name + " v" + std::to_string(sc.version) + " (" +
                          std::to_string(series->total_samples()) +
                          " samples)";
      out += "  channel_" + std::to_string(id) + " [shape=ellipse, label=\"" +
             escape_dot(label) + "\"];\n";
    }
  }

  if (want(section_mask, kSectionAnomalies, model.anomalies)) {
    const AnomalyEngine& a = *model.anomalies;
    std::set<uint16_t> states;
    for (const TransitionEvent& t : a.transitions()) {
      states.insert(t.from_state);
      states.insert(t.to_state);
    }
    for (uint16_t st : states) {
      out += "  state_" + std::to_string(st) + " [shape=circle, label=\"" +
             escape_dot("s" + std::to_string(st)) + "\"];\n";
    }
    for (const TransitionEvent& t : a.transitions()) {
      out += "  state_" + std::to_string(t.from_state) + " -> state_" +
             std::to_string(t.to_state) + " [label=\"" +
             escape_dot("cause " + std::to_string(t.cause)) + "\"";
      if (!t.continues_chain) out += ", style=dashed, color=red";
      out += "];\n";
    }
  }

  out += "}\n";
  return out;
}

}
