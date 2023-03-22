#include <cstdio>
#include <string>
#include <vector>

#include "blackspan/assemble.h"
#include "blackspan/pipeline.h"
#include "blackspan/report_graph.h"

using namespace blackspan;

static int failures = 0;
#define CHECK(cond)                                             \
  do {                                                          \
    if (!(cond)) {                                              \
      std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, \
                   __FILE__, __LINE__);                         \
      ++failures;                                               \
    }                                                           \
  } while (0)

static bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

// True when `a` occurs strictly before `b` in `s` (and both occur).
static bool before(const std::string& s, const std::string& a,
                   const std::string& b) {
  const size_t pa = s.find(a);
  const size_t pb = s.find(b);
  return pa != std::string::npos && pb != std::string::npos && pa < pb;
}

// A recording covering everything the graph renderers draw:
//  * device 1: two sessions; a transition chain that repeats one edge (0->1
//    cause 0, twice) so counts must aggregate, then a desync (from 5 while in
//    state 1); two faults (an error in state 1, a critical in state 2); and a
//    session label carrying a <script> tag to exercise HTML escaping.
//  * device 2: one session and a single transition (7->8), a separate cluster.
static const char* kScript =
    "device 1\n"
    "session-begin seq=0 ts=1000 label=\"<script>alert(1)</script>\"\n"
    "state seq=1 ts=1100 from=0 to=1 cause=0\n"
    "state seq=2 ts=1200 from=1 to=0 cause=0\n"
    "state seq=3 ts=1300 from=0 to=1 cause=0\n"
    "fault seq=4 ts=1400 code=0x10 severity=error subsystem=5 detail=\"oops\"\n"
    "state seq=5 ts=1500 from=5 to=2 cause=1\n"
    "fault seq=6 ts=1600 code=0x11 severity=critical subsystem=6 detail=\"boom\"\n"
    "session-end seq=7 ts=2000 reason=0\n"
    "session-begin seq=8 ts=3000 label=\"second\"\n"
    "session-end seq=9 ts=4000 reason=0\n"
    "device 2\n"
    "session-begin seq=0 ts=1000 label=\"dev2\"\n"
    "state seq=1 ts=1100 from=7 to=8 cause=2\n"
    "session-end seq=2 ts=1900 reason=0\n";

// Assembles kScript and decodes it, aborting the whole run if the fixture fails
// to assemble (a broken fixture would make every assertion meaningless).
static std::vector<uint8_t> fixture_bytes() {
  AssembleResult a = assemble_script(kScript);
  for (const AssembleDiagnostic& d : a.errors) {
    std::fprintf(stderr, "assemble: %s\n",
                 describe_assemble_diagnostic(d).c_str());
  }
  CHECK(a.ok());
  return a.bytes;
}

static void test_dot_escape() {
  // Quote and backslash are backslash-escaped; a newline becomes \n.
  CHECK(dot_escape("a\"b") == "a\\\"b");
  CHECK(dot_escape("x\\y") == "x\\\\y");
  CHECK(dot_escape("a\nb") == "a\\nb");
  // Angle brackets and ampersands carry no meaning in a DOT label: unchanged.
  CHECK(dot_escape("<a>&b") == "<a>&b");
  // Other control characters collapse to a space.
  CHECK(dot_escape("a\tb") == "a b");
  CHECK(dot_escape("a\rb") == "a b");
  // Plain text passes through untouched.
  CHECK(dot_escape("plain") == "plain");
  CHECK(dot_escape("") == "");
}

static void test_html_escape() {
  CHECK(html_escape("<script>") == "&lt;script&gt;");
  CHECK(html_escape("a & b") == "a &amp; b");
  CHECK(html_escape("\"q\"") == "&quot;q&quot;");
  CHECK(html_escape("it's") == "it&#39;s");
  CHECK(html_escape("x<y>z&\"'") == "x&lt;y&gt;z&amp;&quot;&#39;");
  // Newlines and backslashes are not HTML-special and pass through unchanged.
  CHECK(html_escape("a\nb") == "a\nb");
  CHECK(html_escape("c\\d") == "c\\d");
  CHECK(html_escape("plain") == "plain");
}

static void test_state_machine_all() {
  std::vector<uint8_t> bytes = fixture_bytes();
  DecodedRecording rec;
  rec.decode(bytes.data(), bytes.size());

  const std::string out = render_state_machine_dot(rec.anomalies());

  // Valid DOT digraph with a cluster per device.
  CHECK(out.rfind("digraph state_machine {", 0) == 0);
  CHECK(has(out, "subgraph cluster_dev_00000001"));
  CHECK(has(out, "subgraph cluster_dev_00000002"));
  CHECK(out.back() == '\n' && out[out.size() - 2] == '}');

  // Device 1's four distinct states appear as nodes, device 2's two as well.
  CHECK(has(out, "dev_00000001_s0 [label=\"s0"));
  CHECK(has(out, "dev_00000001_s1 [label=\"s1"));
  CHECK(has(out, "dev_00000001_s2 [label=\"s2"));
  CHECK(has(out, "dev_00000001_s5 [label=\"s5"));
  CHECK(has(out, "dev_00000002_s7 [label=\"s7"));
  CHECK(has(out, "dev_00000002_s8 [label=\"s8"));

  // The repeated 0->1 (cause 0) edge aggregates to a count of two; the single
  // 1->0 edge stays at one.
  CHECK(has(out, "dev_00000001_s0 -> dev_00000001_s1 [label=\"cause 0 x2\""));
  CHECK(has(out, "dev_00000001_s1 -> dev_00000001_s0 [label=\"cause 0 x1\""));

  // The desync transition (5->2) is drawn dashed and labelled as such.
  CHECK(has(out, "dev_00000001_s5 -> dev_00000001_s2 [label=\"cause 1 x1 "
                 "desync\", style=dashed"));

  // States that carried a fault are filled and annotated with the count.
  CHECK(has(out, "dev_00000001_s1 [label=\"s1\\n1 fault\", style=filled"));
  CHECK(has(out, "dev_00000001_s2 [label=\"s2\\n1 fault\", style=filled"));

  // Deterministic across calls and across a freshly decoded copy.
  CHECK(render_state_machine_dot(rec.anomalies()) == out);
  DecodedRecording rec2;
  rec2.decode(bytes.data(), bytes.size());
  CHECK(render_state_machine_dot(rec2.anomalies()) == out);
}

static void test_state_machine_per_device() {
  std::vector<uint8_t> bytes = fixture_bytes();
  DecodedRecording rec;
  rec.decode(bytes.data(), bytes.size());

  const std::string d1 = render_state_machine_dot(rec.anomalies(), 1);
  CHECK(d1.rfind("digraph state_machine_00000001 {", 0) == 0);
  CHECK(has(d1, "label=\"device 00000001 state machine\""));
  // No cluster wrapper and plain (unprefixed) node ids for the single device.
  CHECK(!has(d1, "subgraph"));
  CHECK(has(d1, "s0 [label=\"s0"));
  CHECK(has(d1, "s0 -> s1 [label=\"cause 0 x2\"]"));
  CHECK(has(d1, "s5 -> s2 [label=\"cause 1 x1 desync\""));
  // Device 2's states must not leak into device 1's graph.
  CHECK(!has(d1, "s7"));
  CHECK(!has(d1, "s8"));

  const std::string d2 = render_state_machine_dot(rec.anomalies(), 2);
  CHECK(has(d2, "s7 -> s8 [label=\"cause 2 x1\"]"));
  // Device 1's content (its desync, its faults) must not leak into device 2.
  CHECK(!has(d2, "desync"));
  CHECK(!has(d2, "fault"));
  CHECK(!has(d2, " s5 "));

  // A device the engine never saw yields a valid, empty digraph.
  const std::string none = render_state_machine_dot(rec.anomalies(), 999);
  CHECK(none.rfind("digraph state_machine_000003e7 {", 0) == 0);
  CHECK(!has(none, "->"));
  CHECK(has(none, "}\n"));
}

static void test_session_timeline() {
  std::vector<uint8_t> bytes = fixture_bytes();
  DecodedRecording rec;
  rec.decode(bytes.data(), bytes.size());
  const std::vector<Session>& sessions = rec.sessions().sessions();

  const std::string out = render_session_timeline_dot(sessions);
  CHECK(out.rfind("digraph session_timeline {", 0) == 0);
  CHECK(has(out, "subgraph cluster_dev_00000001"));
  CHECK(has(out, "subgraph cluster_dev_00000002"));

  // Device 1 has two ordered session nodes, #0 emitted before #1.
  CHECK(has(out, "dev_00000001_s0 [label=\"dev 00000001 #0"));
  CHECK(has(out, "dev_00000001_s1 [label=\"dev 00000001 #1"));
  CHECK(before(out, "dev_00000001_s0 [label", "dev_00000001_s1 [label"));

  // The inter-session gap edge: 3000 (second begin) - 2000 (first end) = 1000ns.
  CHECK(has(out, "dev_00000001_s0 -> dev_00000001_s1 [label=\"gap 1000ns\"]"));

  // Device 2 has a single session and therefore no gap edge.
  CHECK(has(out, "dev_00000002_s0 [label=\"dev 00000002 #0"));
  CHECK(!has(out, "dev_00000002_s0 -> "));

  // Determinism, and an empty input still yields a well-formed digraph.
  CHECK(render_session_timeline_dot(sessions) == out);
  const std::string empty = render_session_timeline_dot({});
  CHECK(empty.rfind("digraph session_timeline {", 0) == 0);
  CHECK(has(empty, "}\n"));
  CHECK(!has(empty, "subgraph"));
}

static void test_html_report() {
  std::vector<uint8_t> bytes = fixture_bytes();
  DecodedRecording rec;
  rec.decode(bytes.data(), bytes.size());

  const std::string out = render_html_report(rec);

  // Well-formed shell.
  CHECK(out.rfind("<!DOCTYPE html>", 0) == 0);
  CHECK(has(out, "<title>blackspan recording report</title>"));
  CHECK(has(out, "</body>"));
  CHECK(has(out, "</html>"));

  // Summary counts (3 sessions across the two devices, 2 faults, 5 transitions).
  CHECK(has(out, "<th>Sessions</th><td>3</td>"));
  CHECK(has(out, "<th>Faults</th><td>2</td>"));
  CHECK(has(out, "<th>Transitions</th><td>5</td>"));
  CHECK(has(out, "<th>State desyncs</th><td>1</td>"));
  CHECK(has(out, "<th>Frames decoded</th>"));

  // Session table carries a per-device row with firmware/state columns.
  CHECK(has(out, "<h2>Sessions</h2>"));
  CHECK(has(out, "<td>00000001</td>"));
  CHECK(has(out, "ended (reason 0)"));

  // Fault table: severity-tinted rows for the error and the critical fault, with
  // the recorder-supplied detail text shown.
  CHECK(has(out, "<h2>Faults</h2>"));
  CHECK(has(out, "background:#ffe9e6"));  // error row tint
  CHECK(has(out, "background:#f6c9c9"));  // critical row tint
  CHECK(has(out, "<td>error</td>"));
  CHECK(has(out, "<td>critical</td>"));
  CHECK(has(out, "<td>0x0010</td>"));
  CHECK(has(out, ">oops<"));
  CHECK(has(out, ">boom<"));

  // The injected <script> label is escaped, never emitted as live markup.
  CHECK(has(out, "&lt;script&gt;alert(1)&lt;/script&gt;"));
  CHECK(!has(out, "<script>alert(1)"));

  // The state-machine DOT is embedded in a <pre> block (escaped, so its label
  // quotes survive as &quot;).
  CHECK(has(out, "<pre class=\"dot\">digraph state_machine {"));
  CHECK(has(out, "cause 0 x2"));

  // Deterministic.
  CHECK(render_html_report(rec) == out);
  DecodedRecording rec2;
  rec2.decode(bytes.data(), bytes.size());
  CHECK(render_html_report(rec2) == out);
}

static void test_html_report_empty() {
  DecodedRecording rec;
  rec.decode(nullptr, 0);

  const std::string out = render_html_report(rec);
  CHECK(out.rfind("<!DOCTYPE html>", 0) == 0);
  CHECK(has(out, "</html>"));
  CHECK(has(out, "<th>Sessions</th><td>0</td>"));
  CHECK(has(out, "<th>Faults</th><td>0</td>"));
  // Tables still render their headers even with no rows.
  CHECK(has(out, "<h2>Sessions</h2>"));
  CHECK(has(out, "<h2>Faults</h2>"));
  // The embedded state machine is a valid, empty digraph.
  CHECK(has(out, "<pre class=\"dot\">digraph state_machine {"));

  // The bare-engine state machine renderer is likewise empty but valid.
  const std::string sm = render_state_machine_dot(rec.anomalies());
  CHECK(sm.rfind("digraph state_machine {", 0) == 0);
  CHECK(!has(sm, "subgraph"));
  CHECK(!has(sm, "->"));
}

int main() {
  test_dot_escape();
  test_html_escape();
  test_state_machine_all();
  test_state_machine_per_device();
  test_session_timeline();
  test_html_report();
  test_html_report_empty();

  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("test_report_graph: all checks passed\n");
  return 0;
}
