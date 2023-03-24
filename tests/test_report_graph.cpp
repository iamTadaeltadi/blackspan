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

static bool before(const std::string& s, const std::string& a,
                   const std::string& b) {
  const size_t pa = s.find(a);
  const size_t pb = s.find(b);
  return pa != std::string::npos && pb != std::string::npos && pa < pb;
}

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

  CHECK(dot_escape("a\"b") == "a\\\"b");
  CHECK(dot_escape("x\\y") == "x\\\\y");
  CHECK(dot_escape("a\nb") == "a\\nb");

  CHECK(dot_escape("<a>&b") == "<a>&b");

  CHECK(dot_escape("a\tb") == "a b");
  CHECK(dot_escape("a\rb") == "a b");

  CHECK(dot_escape("plain") == "plain");
  CHECK(dot_escape("") == "");
}

static void test_html_escape() {
  CHECK(html_escape("<script>") == "&lt;script&gt;");
  CHECK(html_escape("a & b") == "a &amp; b");
  CHECK(html_escape("\"q\"") == "&quot;q&quot;");
  CHECK(html_escape("it's") == "it&#39;s");
  CHECK(html_escape("x<y>z&\"'") == "x&lt;y&gt;z&amp;&quot;&#39;");

  CHECK(html_escape("a\nb") == "a\nb");
  CHECK(html_escape("c\\d") == "c\\d");
  CHECK(html_escape("plain") == "plain");
}

static void test_state_machine_all() {
  std::vector<uint8_t> bytes = fixture_bytes();
  DecodedRecording rec;
  rec.decode(bytes.data(), bytes.size());

  const std::string out = render_state_machine_dot(rec.anomalies());

  CHECK(out.rfind("digraph state_machine {", 0) == 0);
  CHECK(has(out, "subgraph cluster_dev_00000001"));
  CHECK(has(out, "subgraph cluster_dev_00000002"));
  CHECK(out.back() == '\n' && out[out.size() - 2] == '}');

  CHECK(has(out, "dev_00000001_s0 [label=\"s0"));
  CHECK(has(out, "dev_00000001_s1 [label=\"s1"));
  CHECK(has(out, "dev_00000001_s2 [label=\"s2"));
  CHECK(has(out, "dev_00000001_s5 [label=\"s5"));
  CHECK(has(out, "dev_00000002_s7 [label=\"s7"));
  CHECK(has(out, "dev_00000002_s8 [label=\"s8"));

  CHECK(has(out, "dev_00000001_s0 -> dev_00000001_s1 [label=\"cause 0 x2\""));
  CHECK(has(out, "dev_00000001_s1 -> dev_00000001_s0 [label=\"cause 0 x1\""));

  CHECK(has(out, "dev_00000001_s5 -> dev_00000001_s2 [label=\"cause 1 x1 "
                 "desync\", style=dashed"));

  CHECK(has(out, "dev_00000001_s1 [label=\"s1\\n1 fault\", style=filled"));
  CHECK(has(out, "dev_00000001_s2 [label=\"s2\\n1 fault\", style=filled"));

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

  CHECK(!has(d1, "subgraph"));
  CHECK(has(d1, "s0 [label=\"s0"));
  CHECK(has(d1, "s0 -> s1 [label=\"cause 0 x2\"]"));
  CHECK(has(d1, "s5 -> s2 [label=\"cause 1 x1 desync\""));

  CHECK(!has(d1, "s7"));
  CHECK(!has(d1, "s8"));

  const std::string d2 = render_state_machine_dot(rec.anomalies(), 2);
  CHECK(has(d2, "s7 -> s8 [label=\"cause 2 x1\"]"));

  CHECK(!has(d2, "desync"));
  CHECK(!has(d2, "fault"));
  CHECK(!has(d2, " s5 "));

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

  CHECK(has(out, "dev_00000001_s0 [label=\"dev 00000001 #0"));
  CHECK(has(out, "dev_00000001_s1 [label=\"dev 00000001 #1"));
  CHECK(before(out, "dev_00000001_s0 [label", "dev_00000001_s1 [label"));

  CHECK(has(out, "dev_00000001_s0 -> dev_00000001_s1 [label=\"gap 1000ns\"]"));

  CHECK(has(out, "dev_00000002_s0 [label=\"dev 00000002 #0"));
  CHECK(!has(out, "dev_00000002_s0 -> "));

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

  CHECK(out.rfind("<!DOCTYPE html>", 0) == 0);
  CHECK(has(out, "<title>blackspan recording report</title>"));
  CHECK(has(out, "</body>"));
  CHECK(has(out, "</html>"));

  CHECK(has(out, "<th>Sessions</th><td>3</td>"));
  CHECK(has(out, "<th>Faults</th><td>2</td>"));
  CHECK(has(out, "<th>Transitions</th><td>5</td>"));
  CHECK(has(out, "<th>State desyncs</th><td>1</td>"));
  CHECK(has(out, "<th>Frames decoded</th>"));

  CHECK(has(out, "<h2>Sessions</h2>"));
  CHECK(has(out, "<td>00000001</td>"));
  CHECK(has(out, "ended (reason 0)"));

  CHECK(has(out, "<h2>Faults</h2>"));
  CHECK(has(out, "background:#ffe9e6"));
  CHECK(has(out, "background:#f6c9c9"));
  CHECK(has(out, "<td>error</td>"));
  CHECK(has(out, "<td>critical</td>"));
  CHECK(has(out, "<td>0x0010</td>"));
  CHECK(has(out, ">oops<"));
  CHECK(has(out, ">boom<"));

  CHECK(has(out, "&lt;script&gt;alert(1)&lt;/script&gt;"));
  CHECK(!has(out, "<script>alert(1)"));

  CHECK(has(out, "<pre class=\"dot\">digraph state_machine {"));
  CHECK(has(out, "cause 0 x2"));

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

  CHECK(has(out, "<h2>Sessions</h2>"));
  CHECK(has(out, "<h2>Faults</h2>"));

  CHECK(has(out, "<pre class=\"dot\">digraph state_machine {"));

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
