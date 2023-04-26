#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "blackspan/anomaly.h"
#include "blackspan/assemble.h"
#include "blackspan/eventgraph.h"
#include "blackspan/pipeline.h"

using namespace blackspan;

static int failures = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__, \
                   __LINE__);                                             \
      ++failures;                                                         \
    }                                                                     \
  } while (0)

namespace {

TransitionEvent edge(uint16_t from, uint16_t to, uint32_t device = 1) {
  TransitionEvent t;
  t.from_state = from;
  t.to_state = to;
  t.device_id = device;
  return t;
}

const GraphNode* node_of(const StateGraph& g, uint16_t state) {
  const size_t index = g.node_index(state);
  if (index >= g.nodes.size()) return nullptr;
  return &g.nodes[index];
}

bool same(const std::vector<uint16_t>& got,
          const std::vector<uint16_t>& expected) {
  return got == expected;
}

std::vector<uint8_t> assemble(const std::string& script) {
  AssembleResult result = assemble_script(script);
  CHECK(result.ok());
  if (!result.ok()) {
    for (const auto& e : result.errors) {
      std::fprintf(stderr, "  assemble error: %s\n",
                   describe_assemble_diagnostic(e).c_str());
    }
  }
  return result.bytes;
}

void test_build_nodes_edges_degrees() {
  const std::vector<TransitionEvent> transitions = {
      edge(0, 1), edge(0, 1), edge(1, 2), edge(1, 2), edge(1, 2), edge(2, 3)};
  const StateGraph g = build_state_graph(transitions);

  CHECK(g.nodes.size() == 4);
  CHECK(g.edges.size() == 3);

  CHECK(g.nodes[0].state == 0 && g.nodes[1].state == 1 &&
        g.nodes[2].state == 2 && g.nodes[3].state == 3);

  const GraphNode* n0 = node_of(g, 0);
  const GraphNode* n1 = node_of(g, 1);
  const GraphNode* n2 = node_of(g, 2);
  const GraphNode* n3 = node_of(g, 3);
  CHECK(n0 && n0->in_degree == 0 && n0->out_degree == 1 && n0->visits == 2);
  CHECK(n1 && n1->in_degree == 1 && n1->out_degree == 1 && n1->visits == 5);
  CHECK(n2 && n2->in_degree == 1 && n2->out_degree == 1 && n2->visits == 4);
  CHECK(n3 && n3->in_degree == 1 && n3->out_degree == 0 && n3->visits == 1);

  CHECK(g.edges[0].from == 0 && g.edges[0].to == 1 && g.edges[0].count == 2);
  CHECK(g.edges[1].from == 1 && g.edges[1].to == 2 && g.edges[1].count == 3);
  CHECK(g.edges[2].from == 2 && g.edges[2].to == 3 && g.edges[2].count == 1);

  bool found = false;
  const GraphEdge busiest = busiest_edge(g, found);
  CHECK(found);
  CHECK(busiest.from == 1 && busiest.to == 2 && busiest.count == 3);
}

void test_reachable_chain_and_branch() {
  const std::vector<TransitionEvent> chain = {edge(0, 1), edge(1, 2),
                                              edge(2, 3)};
  const StateGraph gc = build_state_graph(chain);
  CHECK(same(reachable_from(gc, 0), {0, 1, 2, 3}));
  CHECK(same(reachable_from(gc, 2), {2, 3}));
  CHECK(has_path(gc, 0, 3));
  CHECK(has_path(gc, 1, 2));
  CHECK(has_path(gc, 0, 0));
  CHECK(!has_path(gc, 3, 0));
  CHECK(!has_path(gc, 2, 1));

  const std::vector<TransitionEvent> branch = {edge(0, 1), edge(0, 2),
                                               edge(2, 3)};
  const StateGraph gb = build_state_graph(branch);
  CHECK(same(reachable_from(gb, 0), {0, 1, 2, 3}));
  CHECK(same(reachable_from(gb, 1), {1}));
  CHECK(has_path(gb, 0, 1));
  CHECK(has_path(gb, 0, 3));
  CHECK(!has_path(gb, 1, 3));
  CHECK(!has_path(gb, 1, 2));
}

void test_scc_cycle_and_chain() {
  const std::vector<TransitionEvent> cycle = {edge(0, 1), edge(1, 2),
                                              edge(2, 0)};
  const StateGraph gcyc = build_state_graph(cycle);
  const std::vector<std::vector<uint16_t>> comps =
      strongly_connected_components(gcyc);
  CHECK(comps.size() == 1);
  if (comps.size() == 1) {
    CHECK(same(comps[0], {0, 1, 2}));
  }

  const std::vector<TransitionEvent> chain = {edge(0, 1), edge(1, 2),
                                              edge(2, 3)};
  const StateGraph gch = build_state_graph(chain);
  const std::vector<std::vector<uint16_t>> singles =
      strongly_connected_components(gch);
  CHECK(singles.size() == 4);
  for (const auto& c : singles) CHECK(c.size() == 1);
  CHECK(singles[0].front() == 0 && singles[3].front() == 3);

  const std::vector<TransitionEvent> mixed = {
      edge(0, 1), edge(1, 2), edge(2, 1), edge(2, 3)};
  const StateGraph gm = build_state_graph(mixed);
  const std::vector<std::vector<uint16_t>> mcomps =
      strongly_connected_components(gm);
  CHECK(mcomps.size() == 3);
  bool has_pair = false;
  for (const auto& c : mcomps) {
    if (c.size() == 2) {
      has_pair = true;
      CHECK(same(c, {1, 2}));
    }
  }
  CHECK(has_pair);
}

void test_topological_order() {
  const std::vector<TransitionEvent> chain = {edge(0, 1), edge(1, 2),
                                              edge(2, 3)};
  const StateGraph gch = build_state_graph(chain);
  bool acyclic = false;
  const std::vector<uint16_t> order = topological_order(gch, acyclic);
  CHECK(acyclic);
  CHECK(order.size() == gch.nodes.size());
  CHECK(same(order, {0, 1, 2, 3}));

  for (const GraphEdge& e : gch.edges) {
    const auto pu = std::find(order.begin(), order.end(), e.from);
    const auto pv = std::find(order.begin(), order.end(), e.to);
    CHECK(pu != order.end() && pv != order.end());
    CHECK(pu < pv);
  }

  const std::vector<TransitionEvent> cycle = {edge(0, 1), edge(1, 2),
                                              edge(2, 0)};
  const StateGraph gcyc = build_state_graph(cycle);
  bool cyclic_flag = true;
  const std::vector<uint16_t> partial = topological_order(gcyc, cyclic_flag);
  CHECK(!cyclic_flag);
  CHECK(partial.size() < gcyc.nodes.size());
}

void test_sources_terminals_selfloop() {
  const std::vector<TransitionEvent> branch = {edge(0, 1), edge(0, 2),
                                               edge(2, 3)};
  const StateGraph gb = build_state_graph(branch);
  CHECK(same(source_states(gb), {0}));
  CHECK(same(terminal_states(gb), {1, 3}));

  const std::vector<TransitionEvent> loop = {edge(7, 7)};
  const StateGraph gl = build_state_graph(loop);
  CHECK(gl.nodes.size() == 1);
  CHECK(gl.edges.size() == 1);
  const GraphNode* n7 = node_of(gl, 7);
  CHECK(n7 && n7->in_degree == 1 && n7->out_degree == 1 && n7->visits == 2);
  CHECK(source_states(gl).empty());
  CHECK(terminal_states(gl).empty());
  CHECK(same(reachable_from(gl, 7), {7}));
  CHECK(has_path(gl, 7, 7));

  const std::vector<std::vector<uint16_t>> comps =
      strongly_connected_components(gl);
  CHECK(comps.size() == 1 && comps[0].size() == 1 && comps[0].front() == 7);

  bool acyclic = true;
  topological_order(gl, acyclic);
  CHECK(!acyclic);

  const GraphSummary s = summarize_graph(gl);
  CHECK(s.self_loops == 1);
  CHECK(!s.acyclic);
  CHECK(s.busiest_from == 7 && s.busiest_to == 7 && s.busiest_count == 1);
}

void test_empty_and_unknown() {
  const StateGraph empty = build_state_graph({});
  CHECK(empty.nodes.empty());
  CHECK(empty.edges.empty());
  CHECK(empty.node_index(5) == 0);
  CHECK(reachable_from(empty, 5).empty());
  CHECK(!has_path(empty, 1, 2));
  CHECK(strongly_connected_components(empty).empty());
  CHECK(terminal_states(empty).empty());
  CHECK(source_states(empty).empty());
  bool found = true;
  busiest_edge(empty, found);
  CHECK(!found);
  bool acyclic = false;
  CHECK(topological_order(empty, acyclic).empty());
  CHECK(acyclic);

  const GraphSummary es = summarize_graph(empty);
  CHECK(es.nodes == 0 && es.edges == 0 && es.components == 0);
  CHECK(es.acyclic && es.self_loops == 0 && es.busiest_count == 0);

  const std::vector<TransitionEvent> transitions = {edge(0, 1), edge(1, 2)};
  const StateGraph g = build_state_graph(transitions);
  CHECK(g.node_index(999) == g.nodes.size());
  CHECK(node_of(g, 999) == nullptr);
  CHECK(reachable_from(g, 999).empty());
  CHECK(!has_path(g, 999, 0));
  CHECK(!has_path(g, 0, 999));
}

void test_device_filter() {
  const std::vector<TransitionEvent> transitions = {
      edge(0, 1, 1), edge(1, 2, 1), edge(5, 6, 2), edge(6, 7, 2)};

  const StateGraph all = build_state_graph(transitions);
  CHECK(all.nodes.size() == 6);

  const StateGraph dev1 = build_state_graph(transitions, 1);
  CHECK(dev1.nodes.size() == 3);
  CHECK(same(reachable_from(dev1, 0), {0, 1, 2}));
  CHECK(node_of(dev1, 0) != nullptr && node_of(dev1, 5) == nullptr);

  const StateGraph dev2 = build_state_graph(transitions, 2);
  CHECK(dev2.nodes.size() == 3);
  CHECK(node_of(dev2, 5) != nullptr && node_of(dev2, 0) == nullptr);
}

void test_shortest_path_and_edge_count() {
  const std::vector<TransitionEvent> transitions = {
      edge(0, 1), edge(1, 3), edge(0, 2), edge(2, 3), edge(3, 4)};
  const StateGraph g = build_state_graph(transitions);

  const PathResult direct = shortest_path(g, 0, 3);
  CHECK(direct.reachable);
  CHECK(direct.states.size() == 3);
  CHECK(direct.states.front() == 0 && direct.states.back() == 3);
  for (size_t i = 1; i < direct.states.size(); ++i) {
    CHECK(edge_count(g, direct.states[i - 1], direct.states[i]) >= 1);
  }

  const PathResult trivial = shortest_path(g, 2, 2);
  CHECK(trivial.reachable);
  CHECK(trivial.states.size() == 1 && trivial.states.front() == 2);

  const PathResult none = shortest_path(g, 4, 0);
  CHECK(!none.reachable && none.states.empty());

  const PathResult unknown = shortest_path(g, 0, 99);
  CHECK(!unknown.reachable);

  CHECK(edge_count(g, 3, 4) == 1);
  CHECK(edge_count(g, 4, 3) == 0);
  CHECK(edge_count(g, 99, 0) == 0);
}

void test_hotspots_and_cycles() {
  const std::vector<TransitionEvent> transitions = {
      edge(0, 1), edge(1, 2), edge(1, 2), edge(2, 1), edge(2, 3)};
  const StateGraph g = build_state_graph(transitions);

  const std::vector<GraphNode> ranked = hotspots(g, 2);
  CHECK(ranked.size() == 2);
  CHECK(ranked[0].visits >= ranked[1].visits);
  CHECK(ranked[0].visits == 4);
  CHECK(ranked[0].state == 1 && ranked[1].state == 2);

  const std::vector<GraphNode> all = hotspots(g, 100);
  CHECK(all.size() == g.nodes.size());

  CHECK(contains_cycle(g));
  CHECK(same(nodes_in_cycles(g), {1, 2}));

  const std::vector<TransitionEvent> chain = {edge(0, 1), edge(1, 2)};
  const StateGraph gc = build_state_graph(chain);
  CHECK(!contains_cycle(gc));
  CHECK(nodes_in_cycles(gc).empty());
}

void test_unreachable_states() {
  const std::vector<TransitionEvent> transitions = {
      edge(0, 1), edge(1, 2), edge(5, 6)};
  const StateGraph g = build_state_graph(transitions);

  CHECK(same(unreachable_states(g, 0), {5, 6}));
  CHECK(same(unreachable_states(g, 5), {0, 1, 2}));
  CHECK(same(unreachable_states(g, 2), {0, 1, 5, 6}));
  CHECK(same(unreachable_states(g, 999), {0, 1, 2, 5, 6}));
}

void test_recording_end_to_end() {
  const std::vector<uint8_t> wire = assemble(
      "state from=0 to=1\n"
      "state from=1 to=2\n"
      "state from=2 to=0\n"
      "state from=0 to=3\n");

  DecodedRecording rec;
  rec.decode(wire.data(), wire.size());
  CHECK(rec.anomalies().transitions().size() == 4);

  const StateGraph g = recording_state_graph(rec);
  CHECK(g.nodes.size() == 4);
  CHECK(g.edges.size() == 4);

  CHECK(same(terminal_states(g), {3}));
  CHECK(source_states(g).empty());
  CHECK(has_path(g, 0, 3));
  CHECK(has_path(g, 1, 0));
  CHECK(has_path(g, 2, 3));

  const std::vector<std::vector<uint16_t>> comps =
      strongly_connected_components(g);
  CHECK(comps.size() == 2);

  const GraphSummary s = summarize_graph(g);
  CHECK(s.nodes == 4 && s.edges == 4);
  CHECK(s.components == 2);
  CHECK(!s.acyclic);
  CHECK(s.self_loops == 0);

  const std::string text = describe_graph_summary(s);
  CHECK(text.find("states=4") != std::string::npos);
  CHECK(text.find("transitions=4") != std::string::npos);
  CHECK(text.find("cyclic") != std::string::npos);
}

void test_bfs_depths_and_eccentricity() {
  const std::vector<TransitionEvent> chain = {edge(0, 1), edge(1, 2),
                                              edge(2, 3)};
  const StateGraph gc = build_state_graph(chain);
  const std::vector<uint32_t> d = bfs_depths(gc, 0);
  CHECK(d.size() == 4);
  CHECK(d[gc.node_index(0)] == 0);
  CHECK(d[gc.node_index(1)] == 1);
  CHECK(d[gc.node_index(2)] == 2);
  CHECK(d[gc.node_index(3)] == 3);
  CHECK(graph_eccentricity(gc, 0) == 3);
  CHECK(graph_eccentricity(gc, 2) == 1);

  const std::vector<TransitionEvent> branch = {edge(0, 1), edge(0, 2),
                                               edge(2, 3)};
  const StateGraph gb = build_state_graph(branch);
  CHECK(graph_eccentricity(gb, 0) == 2);
  CHECK(bfs_depths(gb, 9).empty() == gb.nodes.empty());
}

}

int main() {
  test_build_nodes_edges_degrees();
  test_reachable_chain_and_branch();
  test_bfs_depths_and_eccentricity();
  test_scc_cycle_and_chain();
  test_topological_order();
  test_sources_terminals_selfloop();
  test_empty_and_unknown();
  test_device_filter();
  test_shortest_path_and_edge_count();
  test_hotspots_and_cycles();
  test_unreachable_states();
  test_recording_end_to_end();
  if (failures) {
    std::fprintf(stderr, "%d checks failed\n", failures);
    return 1;
  }
  std::printf("all eventgraph tests passed\n");
  return 0;
}
