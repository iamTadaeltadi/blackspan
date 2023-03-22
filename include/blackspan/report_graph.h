#ifndef BLACKSPAN_REPORT_GRAPH_H
#define BLACKSPAN_REPORT_GRAPH_H

#include <cstdint>
#include <string>
#include <vector>

#include "blackspan/anomaly.h"
#include "blackspan/pipeline.h"
#include "blackspan/session.h"

// The report-graph subsystem draws the reconstructed model as graphs and
// structured documents that go beyond the plain text/JSON/dot views the report
// subsystem already emits: the state-machine diagrams and the self-contained
// HTML page a triage engineer opens to read one recording at a glance. Like the
// report subsystem it is purely a consumer at the end of the decode pipeline. It
// decodes no wire bytes and mutates none of the model it is handed; every
// renderer is a pure function of its input, so rendering the same model twice
// yields byte-identical output, and nothing here throws. Text drawn from the
// model is always escaped for its target format, so recorder-supplied labels can
// never break out of a graph label or inject markup into the HTML.

namespace blackspan {

// Renders the per-device state machines reconstructed from the anomaly engine's
// transition chain as a single Graphviz DOT digraph. Every device that recorded
// at least one transition becomes a cluster subgraph; within it, each distinct
// state the device visited is a node and each observed (from, to, cause) triple
// is one edge whose label carries the cause and the number of times the
// transition was seen. Edges that broke the known chain (a desync) are drawn
// dashed, and a state that a fault was recorded in is filled and annotated with
// the fault count. An engine with no transitions yields a valid empty digraph.
std::string render_state_machine_dot(const AnomalyEngine& anomalies);

// The same rendering restricted to a single device_id: no cluster wrapper, and
// only the states and transitions of that one device. A device_id the engine
// never saw yields a valid empty digraph.
std::string render_state_machine_dot(const AnomalyEngine& anomalies,
                                     uint32_t device_id);

// Renders the reconstructed sessions as a Graphviz DOT timeline: one cluster
// subgraph per device, the device's sessions as boxes in session order, and an
// edge between consecutive sessions labelled with the inter-session gap (the
// device-monotonic time from one session's close to the next session's open).
std::string render_session_timeline_dot(const std::vector<Session>& sessions);

// Renders a whole decoded recording as one self-contained HTML document: a
// summary table of the model's aggregate counts, a per-device session table, a
// fault table whose rows are tinted by severity, and the state-machine DOT
// embedded verbatim in a <pre> block. Every value taken from the model is
// HTML-escaped. An empty recording still yields a complete, well-formed page.
std::string render_html_report(const DecodedRecording& rec);

// ---- escaping helpers (unit-tested directly) ----------------------------

// Escapes a string for a Graphviz double-quoted label: a double quote and a
// backslash are backslash-escaped, a newline becomes the two-character sequence
// backslash-n (which Graphviz renders as a line break), and every other control
// character below 0x20 becomes a space, so a label can never introduce a stray
// quote, backslash, or hard line break into the graph. Bytes 0x20 and above --
// including '<', '>' and '&', which carry no special meaning inside a DOT label
// -- pass through unchanged.
std::string dot_escape(const std::string& s);

// Escapes a string for HTML text or a double-quoted attribute value: the five
// characters that are structural in that context ('&', '<', '>', '"', and '\'')
// become their named or numeric character references. All other bytes, newlines
// included, pass through unchanged so already-valid text round-trips.
std::string html_escape(const std::string& s);

}  // namespace blackspan

#endif  // BLACKSPAN_REPORT_GRAPH_H
