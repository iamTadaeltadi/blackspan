#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "blackspan/report.h"
#include "report_internal.h"

namespace blackspan {

std::string render_text(const ReportModel& model, uint16_t section_mask) {
  using report_detail::ColumnStats;
  using report_detail::column_stats;
  using report_detail::device_hex;
  using report_detail::want;

  std::string out;
  out += "blackspan report\n";
  out += "================\n";

  if (want(section_mask, kSectionSessions, model.sessions)) {
    const std::vector<Session>& sessions = *model.sessions;
    out += "\nSessions (" + std::to_string(sessions.size()) + ")\n";
    for (const Session& s : sessions) {
      out += "  " + describe_session(s) + "\n";
    }
  }

  if (want(section_mask, kSectionSchemas, model.schemas)) {
    const std::vector<uint16_t> ids = model.schemas->channels();
    out += "\nSchemas (" + std::to_string(model.schemas->count()) + ")\n";
    for (uint16_t id : ids) {
      const ChannelSchema* sc = model.schemas->latest(id);
      if (sc) out += "  " + sc->describe() + "\n";
    }
  }

  if (want(section_mask, kSectionChannels, model.channels)) {
    const std::vector<uint16_t> ids = model.channels->channels();
    out += "\nChannels (" + std::to_string(ids.size()) + ")\n";
    for (uint16_t id : ids) {
      const ChannelSeries* series = model.channels->series(id);
      if (!series) continue;
      const ChannelSchema& sc = series->schema();
      out += "  channel " + std::to_string(id) + " v" +
             std::to_string(sc.version) + " \"" + sc.name + "\"";
      out += " fields " + std::to_string(sc.fields.size());
      out += " stride " + std::to_string(sc.stride);
      out += " samples " + std::to_string(series->total_samples());
      if (!sc.fields.empty()) {
        const std::vector<double> col =
            model.channels->column_as_doubles(id, 0);
        const ColumnStats st = column_stats(col);
        out += " field[0] \"" + sc.fields[0].name + "\" (" +
               field_type_name(sc.fields[0].type) + ")";
        if (st.present) {
          out += " min " + format_number(st.min) + " max " +
                 format_number(st.max) + " mean " + format_number(st.mean);
        } else {
          out += " min - max - mean -";
        }
      }
      out += "\n";
    }
  }

  if (want(section_mask, kSectionAnomalies, model.anomalies)) {
    const AnomalyEngine& a = *model.anomalies;
    out += "\nAnomalies\n";

    const std::array<size_t, 4> sev = a.fault_count_by_severity();
    out += "  faults by severity: info " + std::to_string(sev[0]) +
           ", warning " + std::to_string(sev[1]) + ", error " +
           std::to_string(sev[2]) + ", critical " + std::to_string(sev[3]) +
           "\n";

    out += "  fault timeline (" + std::to_string(a.faults().size()) + "):\n";
    for (const FaultEvent& f : a.faults()) {
      out += "    " + describe_fault(f) + "\n";
    }

    out += "  state transitions (" + std::to_string(a.transitions().size()) +
           "):\n";
    for (const TransitionEvent& t : a.transitions()) {
      out += "    " + describe_transition(t) + "\n";
    }

    out += "  config changes (" + std::to_string(a.config_changes().size()) +
           "):\n";
    for (const ConfigEvent& c : a.config_changes()) {
      out += "    " + describe_config(c) + "\n";
    }

    out += "  diagnostics (" + std::to_string(a.diagnostics().size()) + "):\n";
    for (const AnomalyDiagnostic& d : a.diagnostics()) {
      out += "    " + std::string(anomaly_diag_str(d.code)) + " device " +
             device_hex(d.device_id) + " seq " +
             std::to_string(d.session_seq);
      if (!d.detail.empty()) out += " " + d.detail;
      out += "\n";
    }

    out += "  summary: " + describe_anomaly_summary(a.summary()) + "\n";
  }

  return out;
}

}  // namespace blackspan
