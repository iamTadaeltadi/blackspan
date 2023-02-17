#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "blackspan/json_writer.h"
#include "blackspan/report.h"
#include "report_internal.h"

namespace blackspan {

std::string render_json(const ReportModel& model, uint16_t section_mask) {
  using report_detail::ColumnStats;
  using report_detail::column_stats;
  using report_detail::want;

  JsonWriter w;
  w.begin_object();

  if (want(section_mask, kSectionSessions, model.sessions)) {
    w.key("sessions");
    w.begin_array();
    for (const Session& s : *model.sessions) {
      w.begin_object();
      w.key("device_id");
      w.uint_value(s.device_id);
      w.key("session_index");
      w.uint_value(s.session_index);
      w.key("fw");
      w.begin_object();
      w.key("major");
      w.uint_value(s.fw_major);
      w.key("minor");
      w.uint_value(s.fw_minor);
      w.key("patch");
      w.uint_value(s.fw_patch);
      w.end_object();
      w.key("device_class");
      w.uint_value(s.device_class);
      w.key("schema_epoch");
      w.uint_value(s.schema_epoch);
      w.key("label");
      w.string_value(s.label);
      w.key("begin_seq");
      w.uint_value(s.begin_seq);
      w.key("begin_ts");
      w.uint_value(s.begin_ts);
      w.key("end_seq");
      w.uint_value(s.end_seq);
      w.key("end_ts");
      w.uint_value(s.end_ts);
      w.key("closed");
      w.bool_value(s.closed);
      w.key("end_reason");
      w.uint_value(s.end_reason);
      w.key("duration_ns");
      w.uint_value(s.duration_ns());
      w.key("checkpoints");
      w.uint_value(s.checkpoints.size());
      w.key("reconnects");
      w.uint_value(s.reconnects.size());
      w.key("markers");
      w.uint_value(s.markers.size());
      w.key("seq_regressions");
      w.uint_value(s.seq_regressions);
      w.key("ts_regressions");
      w.uint_value(s.ts_regressions);
      w.end_object();
    }
    w.end_array();
  }

  if (want(section_mask, kSectionSchemas, model.schemas)) {
    w.key("schemas");
    w.begin_array();
    for (uint16_t id : model.schemas->channels()) {
      const ChannelSchema* sc = model.schemas->latest(id);
      if (!sc) continue;
      w.begin_object();
      w.key("channel_id");
      w.uint_value(sc->channel_id);
      w.key("version");
      w.uint_value(sc->version);
      w.key("name");
      w.string_value(sc->name);
      w.key("stride");
      w.uint_value(sc->stride);
      w.key("fields");
      w.begin_array();
      for (const SchemaField& fld : sc->fields) {
        w.begin_object();
        w.key("name");
        w.string_value(fld.name);
        w.key("type");
        w.string_value(field_type_name(fld.type));
        w.key("offset");
        w.uint_value(fld.offset);
        w.end_object();
      }
      w.end_array();
      w.end_object();
    }
    w.end_array();
  }

  if (want(section_mask, kSectionChannels, model.channels)) {
    w.key("channels");
    w.begin_array();
    for (uint16_t id : model.channels->channels()) {
      const ChannelSeries* series = model.channels->series(id);
      if (!series) continue;
      const ChannelSchema& sc = series->schema();
      w.begin_object();
      w.key("channel_id");
      w.uint_value(id);
      w.key("version");
      w.uint_value(sc.version);
      w.key("name");
      w.string_value(sc.name);
      w.key("field_count");
      w.uint_value(sc.fields.size());
      w.key("stride");
      w.uint_value(sc.stride);
      w.key("samples");
      w.uint_value(series->total_samples());
      if (!sc.fields.empty()) {
        const std::vector<double> col =
            model.channels->column_as_doubles(id, 0);
        const ColumnStats st = column_stats(col);
        w.key("stat");
        w.begin_object();
        w.key("field");
        w.string_value(sc.fields[0].name);
        w.key("type");
        w.string_value(field_type_name(sc.fields[0].type));
        w.key("count");
        w.uint_value(st.count);
        if (st.present) {
          w.key("min");
          w.double_value(st.min);
          w.key("max");
          w.double_value(st.max);
          w.key("mean");
          w.double_value(st.mean);
        } else {
          w.key("min");
          w.null_value();
          w.key("max");
          w.null_value();
          w.key("mean");
          w.null_value();
        }
        w.end_object();
      }
      w.end_object();
    }
    w.end_array();
  }

  if (want(section_mask, kSectionAnomalies, model.anomalies)) {
    const AnomalyEngine& a = *model.anomalies;
    w.key("anomalies");
    w.begin_object();

    const std::array<size_t, 4> sev = a.fault_count_by_severity();
    w.key("fault_counts");
    w.begin_object();
    w.key("info");
    w.uint_value(sev[0]);
    w.key("warning");
    w.uint_value(sev[1]);
    w.key("error");
    w.uint_value(sev[2]);
    w.key("critical");
    w.uint_value(sev[3]);
    w.end_object();

    w.key("faults");
    w.begin_array();
    for (const FaultEvent& f : a.faults()) {
      w.begin_object();
      w.key("device_id");
      w.uint_value(f.device_id);
      w.key("session_seq");
      w.uint_value(f.session_seq);
      w.key("monotonic_ts");
      w.uint_value(f.monotonic_ts);
      w.key("fault_code");
      w.uint_value(f.fault_code);
      w.key("severity");
      w.string_value(severity_name(f.severity));
      w.key("subsystem_id");
      w.uint_value(f.subsystem_id);
      w.key("detail_len");
      w.uint_value(f.detail.size());
      w.key("state_at_fault");
      if (f.state_at_fault.has_value()) {
        w.uint_value(f.state_at_fault.value());
      } else {
        w.null_value();
      }
      w.end_object();
    }
    w.end_array();

    w.key("transitions");
    w.begin_array();
    for (const TransitionEvent& t : a.transitions()) {
      w.begin_object();
      w.key("device_id");
      w.uint_value(t.device_id);
      w.key("session_seq");
      w.uint_value(t.session_seq);
      w.key("monotonic_ts");
      w.uint_value(t.monotonic_ts);
      w.key("from_state");
      w.uint_value(t.from_state);
      w.key("to_state");
      w.uint_value(t.to_state);
      w.key("cause");
      w.uint_value(t.cause);
      w.key("continues_chain");
      w.bool_value(t.continues_chain);
      w.end_object();
    }
    w.end_array();

    w.key("config_changes");
    w.begin_array();
    for (const ConfigEvent& c : a.config_changes()) {
      w.begin_object();
      w.key("device_id");
      w.uint_value(c.device_id);
      w.key("session_seq");
      w.uint_value(c.session_seq);
      w.key("monotonic_ts");
      w.uint_value(c.monotonic_ts);
      w.key("param_id");
      w.uint_value(c.param_id);
      w.key("old_value");
      w.uint_value(c.old_value);
      w.key("new_value");
      w.uint_value(c.new_value);
      w.end_object();
    }
    w.end_array();

    w.key("diagnostics");
    w.begin_array();
    for (const AnomalyDiagnostic& d : a.diagnostics()) {
      w.begin_object();
      w.key("code");
      w.string_value(anomaly_diag_str(d.code));
      w.key("device_id");
      w.uint_value(d.device_id);
      w.key("session_seq");
      w.uint_value(d.session_seq);
      w.key("detail");
      w.string_value(d.detail);
      w.end_object();
    }
    w.end_array();

    w.end_object();  // anomalies
  }

  w.end_object();  // root
  return w.str();
}

}  // namespace blackspan
