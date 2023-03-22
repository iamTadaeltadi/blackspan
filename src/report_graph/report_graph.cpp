#include "blackspan/report_graph.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "blackspan/channel.h"
#include "blackspan/frame.h"
#include "blackspan/schema.h"

namespace blackspan {

namespace {

// 8-hex-digit device id, matching the "%08x" form the domain describe_* helpers
// use so a device reads identically across every view.
std::string dev_hex(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof(b), "%08x", v);
  return std::string(b);
}

// A 16-bit value as 0x-prefixed four-digit hex, used for the wire codes shown in
// the fault table.
std::string u16_hex(uint16_t v) {
  char b[16];
  std::snprintf(b, sizeof(b), "0x%04x", static_cast<unsigned>(v));
  return std::string(b);
}

// ---- state-machine model -------------------------------------------------

// Aggregated weight of one (from, to, cause) edge: how many transitions carried
// it, and how many of those broke the device's known chain.
struct EdgeAgg {
  size_t count = 0;
  size_t desync = 0;
};

// The graph distilled from one device's transition chain: the set of distinct
// states, the aggregated edges keyed by (from, to, cause) so repeats collapse
// into a single labelled edge, and how many faults were recorded in each state.
struct StateGraph {
  std::set<uint16_t> nodes;
  std::map<std::tuple<uint16_t, uint16_t, uint8_t>, EdgeAgg> edges;
  std::map<uint16_t, size_t> fault_counts;
};

// Builds the state graph for a single device from the engine's already-ordered
// transition and fault lists. Only events for `device` contribute, so the graph
// never conflates the identically-numbered states of two different devices.
StateGraph build_state_graph(const AnomalyEngine& a, uint32_t device) {
  StateGraph g;
  for (const TransitionEvent& t : a.transitions()) {
    if (t.device_id != device) continue;
    g.nodes.insert(t.from_state);
    g.nodes.insert(t.to_state);
    EdgeAgg& e = g.edges[std::make_tuple(t.from_state, t.to_state, t.cause)];
    ++e.count;
    if (!t.continues_chain) ++e.desync;
  }
  for (const FaultEvent& f : a.faults()) {
    if (f.device_id != device) continue;
    if (!f.state_at_fault.has_value()) continue;
    const uint16_t st = f.state_at_fault.value();
    // A fault's active state was set by a prior transition, so it is already a
    // node; guard anyway so a fault can never invent a stray node.
    if (g.nodes.count(st)) ++g.fault_counts[st];
  }
  return g;
}

// The distinct device ids that recorded at least one transition, ascending.
std::set<uint32_t> transition_devices(const AnomalyEngine& a) {
  std::set<uint32_t> devs;
  for (const TransitionEvent& t : a.transitions()) devs.insert(t.device_id);
  return devs;
}

// Emits the nodes and edges of one graph into `out`. `prefix` namespaces the DOT
// node ids (empty for a single-device graph, "dev_<hex>_" inside a cluster) so
// several devices can share one digraph without id collisions; `indent` is the
// leading whitespace for each emitted line.
void emit_graph_body(std::string& out, const StateGraph& g,
                     const std::string& prefix, const std::string& indent) {
  for (uint16_t st : g.nodes) {
    const std::string sid = prefix + "s" + std::to_string(st);
    std::string label = "s" + std::to_string(st);
    const auto fit = g.fault_counts.find(st);
    const size_t faults = fit == g.fault_counts.end() ? 0 : fit->second;
    if (faults > 0) {
      label += "\n" + std::to_string(faults) +
               (faults == 1 ? " fault" : " faults");
    }
    out += indent + sid + " [label=\"" + dot_escape(label) + "\"";
    if (faults > 0) out += ", style=filled, fillcolor=\"#f6c9c9\"";
    out += "];\n";
  }
  for (const auto& kv : g.edges) {
    const uint16_t from = std::get<0>(kv.first);
    const uint16_t to = std::get<1>(kv.first);
    const uint8_t cause = std::get<2>(kv.first);
    const EdgeAgg& e = kv.second;
    std::string label = "cause " + std::to_string(cause) + " x" +
                        std::to_string(e.count);
    if (e.desync > 0) label += " desync";
    out += indent + prefix + "s" + std::to_string(from) + " -> " + prefix +
           "s" + std::to_string(to) + " [label=\"" + dot_escape(label) + "\"";
    if (e.desync > 0) out += ", style=dashed, color=\"#c0392b\"";
    out += "];\n";
  }
}

// ---- HTML helpers --------------------------------------------------------

// Row background colour for a fault of the given severity. Info/Warning/Error/
// Critical run cool-to-warm so a triage engineer can scan for the worst rows.
const char* severity_row_color(Severity s) {
  switch (s) {
    case Severity::Info:
      return "#eef5ff";
    case Severity::Warning:
      return "#fff6e5";
    case Severity::Error:
      return "#ffe9e6";
    case Severity::Critical:
      return "#f6c9c9";
  }
  return "#ffffff";
}

// Appends one "<td>escaped</td>" data cell.
void td(std::string& out, const std::string& text) {
  out += "<td>" + html_escape(text) + "</td>";
}

// Appends one "<th>escaped</th>" header cell.
void th(std::string& out, const std::string& text) {
  out += "<th>" + html_escape(text) + "</th>";
}

// The firmware triple as "maj.min.patch".
std::string firmware_str(const Session& s) {
  return std::to_string(s.fw_major) + "." + std::to_string(s.fw_minor) + "." +
         std::to_string(s.fw_patch);
}

}  // namespace

// ---- escaping helpers ----------------------------------------------------

std::string dot_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    if (c == '"') {
      out += "\\\"";
    } else if (c == '\\') {
      out += "\\\\";
    } else if (c == '\n') {
      out += "\\n";
    } else if (c < 0x20) {
      // Any other control character (carriage return, tab, ...) collapses to a
      // space so it can never break the quoted label or the line structure.
      out += ' ';
    } else {
      out += static_cast<char>(c);
    }
  }
  return out;
}

std::string html_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      case '\'':
        out += "&#39;";
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

// ---- state-machine DOT ---------------------------------------------------

std::string render_state_machine_dot(const AnomalyEngine& anomalies) {
  std::string out;
  out += "digraph state_machine {\n";
  out += "  rankdir=LR;\n";
  out += "  node [shape=circle, fontname=\"monospace\"];\n";

  for (uint32_t dev : transition_devices(anomalies)) {
    const StateGraph g = build_state_graph(anomalies, dev);
    const std::string hex = dev_hex(dev);
    out += "  subgraph cluster_dev_" + hex + " {\n";
    out += "    label=\"device " + hex + "\";\n";
    emit_graph_body(out, g, "dev_" + hex + "_", "    ");
    out += "  }\n";
  }

  out += "}\n";
  return out;
}

std::string render_state_machine_dot(const AnomalyEngine& anomalies,
                                     uint32_t device_id) {
  const StateGraph g = build_state_graph(anomalies, device_id);
  const std::string hex = dev_hex(device_id);

  std::string out;
  out += "digraph state_machine_" + hex + " {\n";
  out += "  rankdir=LR;\n";
  out += "  node [shape=circle, fontname=\"monospace\"];\n";
  out += "  label=\"device " + hex + " state machine\";\n";
  emit_graph_body(out, g, "", "  ");
  out += "}\n";
  return out;
}

// ---- session timeline DOT ------------------------------------------------

std::string render_session_timeline_dot(const std::vector<Session>& sessions) {
  std::string out;
  out += "digraph session_timeline {\n";
  out += "  rankdir=LR;\n";
  out += "  node [shape=box, fontname=\"monospace\"];\n";

  // Group by device, keeping the map's ascending device order for determinism.
  std::map<uint32_t, std::vector<const Session*>> by_dev;
  for (const Session& s : sessions) by_dev[s.device_id].push_back(&s);

  for (auto& kv : by_dev) {
    std::vector<const Session*>& v = kv.second;
    // Order a device's sessions by their ordinal, falling back to the opening
    // frame position so the timeline reads left-to-right in recorded order.
    std::sort(v.begin(), v.end(), [](const Session* a, const Session* b) {
      if (a->session_index != b->session_index)
        return a->session_index < b->session_index;
      return a->begin_seq < b->begin_seq;
    });

    const std::string hex = dev_hex(kv.first);
    out += "  subgraph cluster_dev_" + hex + " {\n";
    out += "    label=\"device " + hex + "\";\n";

    for (size_t i = 0; i < v.size(); ++i) {
      const Session* s = v[i];
      const std::string nid = "dev_" + hex + "_s" + std::to_string(i);
      std::string label = "dev " + hex + " #" + std::to_string(s->session_index);
      if (!s->label.empty()) label += " " + s->label;
      label += "\nseq " + std::to_string(s->begin_seq) + ".." +
               std::to_string(s->end_seq);
      label += s->closed
                   ? "\nended reason " + std::to_string(s->end_reason)
                   : "\nunterminated";
      label += "\ndur " + std::to_string(s->duration_ns()) + "ns";
      out += "    " + nid + " [label=\"" + dot_escape(label) + "\"];\n";
    }

    // Edges span consecutive sessions and carry the inter-session gap. The gap
    // is defined only when the earlier session closed cleanly and the later
    // session opened no earlier than that close; otherwise it is reported as
    // unavailable rather than guessed.
    for (size_t i = 1; i < v.size(); ++i) {
      const Session* prev = v[i - 1];
      const Session* cur = v[i];
      std::string glabel;
      if (!prev->closed) {
        glabel = "gap n/a";
      } else if (cur->begin_ts >= prev->end_ts) {
        glabel = "gap " + std::to_string(cur->begin_ts - prev->end_ts) + "ns";
      } else {
        glabel = "gap n/a";
      }
      out += "    dev_" + hex + "_s" + std::to_string(i - 1) + " -> dev_" + hex +
             "_s" + std::to_string(i) + " [label=\"" + dot_escape(glabel) +
             "\"];\n";
    }

    out += "  }\n";
  }

  out += "}\n";
  return out;
}

// ---- HTML report ---------------------------------------------------------

std::string render_html_report(const DecodedRecording& rec) {
  const std::vector<Session>& sessions = rec.sessions().sessions();
  const SchemaRegistry& schemas = rec.schemas();
  const ChannelStore& channels = rec.channels();
  const AnomalyEngine& anomalies = rec.anomalies();
  const AnomalySummary asum = anomalies.summary();
  const std::array<size_t, 4> sev = anomalies.fault_count_by_severity();

  std::string out;
  out += "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n";
  out += "<meta charset=\"utf-8\">\n";
  out += "<title>blackspan recording report</title>\n";
  out += "<style>\n";
  out += "body{font-family:sans-serif;margin:1.5rem;color:#1a1a1a;}\n";
  out += "h1{font-size:1.4rem;}h2{font-size:1.1rem;margin-top:1.5rem;}\n";
  out += "table{border-collapse:collapse;margin-top:0.5rem;}\n";
  out += "th,td{border:1px solid #bbb;padding:0.2rem 0.5rem;text-align:left;"
         "font-size:0.9rem;}\n";
  out += "th{background:#f0f0f0;}\n";
  out += "pre.dot{background:#f7f7f7;border:1px solid #ccc;padding:0.6rem;"
         "overflow-x:auto;font-size:0.85rem;}\n";
  out += "</style>\n</head>\n<body>\n";
  out += "<h1>blackspan recording report</h1>\n";

  // ---- summary table -----------------------------------------------------
  out += "<h2>Summary</h2>\n<table class=\"summary\">\n";
  auto srow = [&out](const std::string& k, const std::string& v) {
    out += "<tr>";
    th(out, k);
    td(out, v);
    out += "</tr>\n";
  };
  srow("Frames decoded", std::to_string(rec.frame_count()));
  srow("Final status", frame_status_str(rec.final_status()));
  srow("Sessions", std::to_string(sessions.size()));
  srow("Schemas", std::to_string(schemas.count()));
  srow("Channels", std::to_string(channels.channels().size()));
  srow("Samples", std::to_string(channels.total_samples()));
  srow("Devices with faults/transitions", std::to_string(asum.device_count));
  srow("Faults", std::to_string(asum.total_faults));
  srow("Transitions", std::to_string(asum.total_transitions));
  srow("Config changes", std::to_string(asum.total_config_changes));
  srow("State desyncs", std::to_string(asum.state_desyncs));
  srow("Faults by severity (info/warning/error/critical)",
       std::to_string(sev[0]) + "/" + std::to_string(sev[1]) + "/" +
           std::to_string(sev[2]) + "/" + std::to_string(sev[3]));
  out += "</table>\n";

  // ---- per-device session table ------------------------------------------
  out += "<h2>Sessions</h2>\n<table class=\"sessions\">\n<tr>";
  th(out, "Device");
  th(out, "#");
  th(out, "Firmware");
  th(out, "Class");
  th(out, "Epoch");
  th(out, "Label");
  th(out, "Seq range");
  th(out, "Duration (ns)");
  th(out, "State");
  th(out, "Checkpoints");
  th(out, "Reconnects");
  th(out, "Markers");
  out += "</tr>\n";

  std::map<uint32_t, std::vector<const Session*>> by_dev;
  for (const Session& s : sessions) by_dev[s.device_id].push_back(&s);
  for (auto& kv : by_dev) {
    std::vector<const Session*>& v = kv.second;
    std::sort(v.begin(), v.end(), [](const Session* a, const Session* b) {
      if (a->session_index != b->session_index)
        return a->session_index < b->session_index;
      return a->begin_seq < b->begin_seq;
    });
    for (const Session* s : v) {
      out += "<tr>";
      td(out, dev_hex(s->device_id));
      td(out, std::to_string(s->session_index));
      td(out, firmware_str(*s));
      td(out, std::to_string(s->device_class));
      td(out, std::to_string(s->schema_epoch));
      td(out, s->label);
      td(out, std::to_string(s->begin_seq) + ".." + std::to_string(s->end_seq));
      td(out, std::to_string(s->duration_ns()));
      td(out, s->closed ? "ended (reason " + std::to_string(s->end_reason) + ")"
                        : "unterminated");
      td(out, std::to_string(s->checkpoints.size()));
      td(out, std::to_string(s->reconnects.size()));
      td(out, std::to_string(s->markers.size()));
      out += "</tr>\n";
    }
  }
  out += "</table>\n";

  // ---- fault table (severity-tinted rows) --------------------------------
  out += "<h2>Faults</h2>\n<table class=\"faults\">\n<tr>";
  th(out, "Device");
  th(out, "Code");
  th(out, "Severity");
  th(out, "Subsystem");
  th(out, "Seq");
  th(out, "Timestamp");
  th(out, "State at fault");
  th(out, "Detail");
  out += "</tr>\n";
  for (const FaultEvent& f : anomalies.faults()) {
    out += "<tr style=\"background:";
    out += severity_row_color(f.severity);
    out += "\">";
    td(out, dev_hex(f.device_id));
    td(out, u16_hex(f.fault_code));
    td(out, severity_name(f.severity));
    td(out, std::to_string(f.subsystem_id));
    td(out, std::to_string(f.session_seq));
    td(out, std::to_string(f.monotonic_ts));
    td(out, f.state_at_fault.has_value()
                ? std::to_string(f.state_at_fault.value())
                : std::string("unknown"));
    // Fault detail is free-form recorder bytes; render it as text so an
    // operator note is legible, escaped so it can never inject markup.
    td(out, std::string(f.detail.begin(), f.detail.end()));
    out += "</tr>\n";
  }
  out += "</table>\n";

  // ---- embedded state-machine DOT ----------------------------------------
  out += "<h2>State machine (Graphviz DOT)</h2>\n";
  out += "<pre class=\"dot\">";
  out += html_escape(render_state_machine_dot(anomalies));
  out += "</pre>\n";

  out += "</body>\n</html>\n";
  return out;
}

}  // namespace blackspan
