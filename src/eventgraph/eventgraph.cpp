#include "blackspan/eventgraph.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace blackspan {

namespace {

struct Adjacency {
  std::vector<std::vector<size_t>> successors;
  std::vector<std::vector<size_t>> predecessors;
};

Adjacency build_adjacency(const StateGraph& g) {
  Adjacency adj;
  adj.successors.resize(g.nodes.size());
  adj.predecessors.resize(g.nodes.size());
  for (const GraphEdge& e : g.edges) {
    const size_t from = g.node_index(e.from);
    const size_t to = g.node_index(e.to);
    if (from >= g.nodes.size() || to >= g.nodes.size()) continue;
    adj.successors[from].push_back(to);
    adj.predecessors[to].push_back(from);
  }
  return adj;
}

StateGraph build_impl(const std::vector<TransitionEvent>& transitions,
                      bool filter_device, uint32_t device_id) {
  std::set<uint16_t> node_set;
  std::map<std::pair<uint16_t, uint16_t>, size_t> edge_counts;
  std::map<uint16_t, size_t> visit_counts;

  for (const TransitionEvent& t : transitions) {
    if (filter_device && t.device_id != device_id) continue;
    node_set.insert(t.from_state);
    node_set.insert(t.to_state);
    ++edge_counts[std::make_pair(t.from_state, t.to_state)];
    ++visit_counts[t.from_state];
    ++visit_counts[t.to_state];
  }

  StateGraph g;
  g.nodes.reserve(node_set.size());
  for (const uint16_t state : node_set) {
    GraphNode node;
    node.state = state;
    const auto it = visit_counts.find(state);
    node.visits = it == visit_counts.end() ? 0 : it->second;
    g.nodes.push_back(node);
  }

  g.edges.reserve(edge_counts.size());
  for (const auto& kv : edge_counts) {
    GraphEdge edge;
    edge.from = kv.first.first;
    edge.to = kv.first.second;
    edge.count = kv.second;
    g.edges.push_back(edge);
  }

  for (const GraphEdge& edge : g.edges) {
    const size_t from = g.node_index(edge.from);
    const size_t to = g.node_index(edge.to);
    if (from < g.nodes.size()) ++g.nodes[from].out_degree;
    if (to < g.nodes.size()) ++g.nodes[to].in_degree;
  }

  return g;
}

std::vector<size_t> reachable_indices(const StateGraph& g, size_t start) {
  std::vector<size_t> out;
  if (start >= g.nodes.size()) return out;
  const Adjacency adj = build_adjacency(g);
  std::vector<char> seen(g.nodes.size(), 0);
  std::vector<size_t> stack;
  stack.push_back(start);
  seen[start] = 1;
  while (!stack.empty()) {
    const size_t node = stack.back();
    stack.pop_back();
    for (const size_t next : adj.successors[node]) {
      if (!seen[next]) {
        seen[next] = 1;
        stack.push_back(next);
      }
    }
  }
  for (size_t i = 0; i < g.nodes.size(); ++i) {
    if (seen[i]) out.push_back(i);
  }
  return out;
}

}

size_t StateGraph::node_index(uint16_t state) const {
  size_t lo = 0;
  size_t hi = nodes.size();
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (nodes[mid].state < state) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < nodes.size() && nodes[lo].state == state) return lo;
  return nodes.size();
}

StateGraph build_state_graph(const std::vector<TransitionEvent>& transitions) {
  return build_impl(transitions, false, 0);
}

StateGraph build_state_graph(const std::vector<TransitionEvent>& transitions,
                             uint32_t device_id) {
  return build_impl(transitions, true, device_id);
}

std::vector<uint16_t> reachable_from(const StateGraph& g, uint16_t start) {
  std::vector<uint16_t> out;
  const size_t start_index = g.node_index(start);
  const std::vector<size_t> indices = reachable_indices(g, start_index);
  out.reserve(indices.size());
  for (const size_t i : indices) out.push_back(g.nodes[i].state);
  return out;
}

bool has_path(const StateGraph& g, uint16_t from, uint16_t to) {
  const std::vector<uint16_t> reached = reachable_from(g, from);
  return std::binary_search(reached.begin(), reached.end(), to);
}

std::vector<std::vector<uint16_t>> strongly_connected_components(
    const StateGraph& g) {
  std::vector<std::vector<uint16_t>> result;
  const size_t n = g.nodes.size();
  if (n == 0) return result;

  const Adjacency adj = build_adjacency(g);

  std::vector<char> visited(n, 0);
  std::vector<size_t> finish_order;
  finish_order.reserve(n);
  std::vector<std::pair<size_t, size_t>> stack;

  for (size_t start = 0; start < n; ++start) {
    if (visited[start]) continue;
    stack.emplace_back(start, 0);
    visited[start] = 1;
    while (!stack.empty()) {
      const size_t node = stack.back().first;
      const size_t cursor = stack.back().second;
      if (cursor < adj.successors[node].size()) {
        stack.back().second = cursor + 1;
        const size_t next = adj.successors[node][cursor];
        if (!visited[next]) {
          visited[next] = 1;
          stack.emplace_back(next, 0);
        }
      } else {
        finish_order.push_back(node);
        stack.pop_back();
      }
    }
  }

  std::vector<char> assigned(n, 0);
  for (size_t k = finish_order.size(); k-- > 0;) {
    const size_t root = finish_order[k];
    if (assigned[root]) continue;
    std::vector<uint16_t> component;
    std::vector<size_t> work;
    work.push_back(root);
    assigned[root] = 1;
    while (!work.empty()) {
      const size_t node = work.back();
      work.pop_back();
      component.push_back(g.nodes[node].state);
      for (const size_t prev : adj.predecessors[node]) {
        if (!assigned[prev]) {
          assigned[prev] = 1;
          work.push_back(prev);
        }
      }
    }
    std::sort(component.begin(), component.end());
    result.push_back(std::move(component));
  }

  std::sort(result.begin(), result.end(),
            [](const std::vector<uint16_t>& a, const std::vector<uint16_t>& b) {
              return a.front() < b.front();
            });
  return result;
}

std::vector<uint16_t> topological_order(const StateGraph& g, bool& acyclic) {
  acyclic = true;
  std::vector<uint16_t> out;
  const size_t n = g.nodes.size();
  if (n == 0) return out;

  const Adjacency adj = build_adjacency(g);
  std::vector<size_t> in_degree(n, 0);
  for (size_t node = 0; node < n; ++node) {
    for (const size_t next : adj.successors[node]) ++in_degree[next];
  }

  std::set<size_t> ready;
  for (size_t node = 0; node < n; ++node) {
    if (in_degree[node] == 0) ready.insert(node);
  }

  out.reserve(n);
  while (!ready.empty()) {
    const size_t node = *ready.begin();
    ready.erase(ready.begin());
    out.push_back(g.nodes[node].state);
    for (const size_t next : adj.successors[node]) {
      if (in_degree[next] > 0 && --in_degree[next] == 0) ready.insert(next);
    }
  }

  if (out.size() != n) acyclic = false;
  return out;
}

std::vector<uint16_t> terminal_states(const StateGraph& g) {
  std::vector<uint16_t> out;
  for (const GraphNode& node : g.nodes) {
    if (node.out_degree == 0) out.push_back(node.state);
  }
  return out;
}

std::vector<uint16_t> source_states(const StateGraph& g) {
  std::vector<uint16_t> out;
  for (const GraphNode& node : g.nodes) {
    if (node.in_degree == 0) out.push_back(node.state);
  }
  return out;
}

GraphEdge busiest_edge(const StateGraph& g, bool& found) {
  GraphEdge best;
  found = false;
  for (const GraphEdge& edge : g.edges) {
    if (!found || edge.count > best.count) {
      best = edge;
      found = true;
    }
  }
  return best;
}

size_t edge_count(const StateGraph& g, uint16_t from, uint16_t to) {
  const std::pair<uint16_t, uint16_t> key(from, to);
  size_t lo = 0;
  size_t hi = g.edges.size();
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const std::pair<uint16_t, uint16_t> here(g.edges[mid].from, g.edges[mid].to);
    if (here < key) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < g.edges.size() && g.edges[lo].from == from && g.edges[lo].to == to) {
    return g.edges[lo].count;
  }
  return 0;
}

PathResult shortest_path(const StateGraph& g, uint16_t from, uint16_t to) {
  PathResult result;
  const size_t source = g.node_index(from);
  const size_t target = g.node_index(to);
  if (source >= g.nodes.size() || target >= g.nodes.size()) return result;

  const Adjacency adj = build_adjacency(g);
  const size_t none = g.nodes.size();
  std::vector<size_t> previous(g.nodes.size(), none);
  std::vector<char> seen(g.nodes.size(), 0);
  std::vector<size_t> queue;
  size_t head = 0;
  queue.push_back(source);
  seen[source] = 1;
  while (head < queue.size()) {
    const size_t node = queue[head++];
    if (node == target) break;
    for (const size_t next : adj.successors[node]) {
      if (!seen[next]) {
        seen[next] = 1;
        previous[next] = node;
        queue.push_back(next);
      }
    }
  }

  if (!seen[target]) return result;

  std::vector<size_t> chain;
  for (size_t at = target; at != none; at = previous[at]) {
    chain.push_back(at);
    if (at == source) break;
  }
  std::reverse(chain.begin(), chain.end());
  result.reachable = true;
  result.states.reserve(chain.size());
  for (const size_t index : chain) result.states.push_back(g.nodes[index].state);
  return result;
}

std::vector<GraphNode> hotspots(const StateGraph& g, size_t limit) {
  std::vector<GraphNode> ranked = g.nodes;
  std::sort(ranked.begin(), ranked.end(),
            [](const GraphNode& a, const GraphNode& b) {
              if (a.visits != b.visits) return a.visits > b.visits;
              return a.state < b.state;
            });
  if (limit < ranked.size()) ranked.resize(limit);
  return ranked;
}

std::vector<uint16_t> unreachable_states(const StateGraph& g, uint16_t start) {
  std::vector<uint16_t> out;
  const std::vector<uint16_t> reached = reachable_from(g, start);
  for (const GraphNode& node : g.nodes) {
    if (!std::binary_search(reached.begin(), reached.end(), node.state)) {
      out.push_back(node.state);
    }
  }
  return out;
}

std::vector<uint16_t> nodes_in_cycles(const StateGraph& g) {
  std::set<uint16_t> members;
  for (const GraphEdge& edge : g.edges) {
    if (edge.from == edge.to) members.insert(edge.from);
  }
  const std::vector<std::vector<uint16_t>> components =
      strongly_connected_components(g);
  for (const std::vector<uint16_t>& component : components) {
    if (component.size() > 1) {
      for (const uint16_t state : component) members.insert(state);
    }
  }
  return std::vector<uint16_t>(members.begin(), members.end());
}

bool contains_cycle(const StateGraph& g) {
  bool acyclic = true;
  topological_order(g, acyclic);
  return !acyclic;
}

GraphSummary summarize_graph(const StateGraph& g) {
  GraphSummary summary;
  summary.nodes = g.nodes.size();
  summary.edges = g.edges.size();
  summary.components = strongly_connected_components(g).size();

  bool acyclic = true;
  topological_order(g, acyclic);
  summary.acyclic = acyclic;

  size_t self_loops = 0;
  for (const GraphEdge& edge : g.edges) {
    if (edge.from == edge.to) ++self_loops;
  }
  summary.self_loops = self_loops;

  bool found = false;
  const GraphEdge busiest = busiest_edge(g, found);
  if (found) {
    summary.busiest_from = busiest.from;
    summary.busiest_to = busiest.to;
    summary.busiest_count = busiest.count;
  }
  return summary;
}

StateGraph recording_state_graph(const DecodedRecording& rec) {
  return build_state_graph(rec.anomalies().transitions());
}

std::string describe_graph_summary(const GraphSummary& s) {
  char buf[256];
  std::snprintf(
      buf, sizeof(buf),
      "states=%zu transitions=%zu components=%zu self-loops=%zu %s "
      "busiest %u->%u x%zu",
      s.nodes, s.edges, s.components, s.self_loops,
      s.acyclic ? "acyclic" : "cyclic", static_cast<unsigned>(s.busiest_from),
      static_cast<unsigned>(s.busiest_to), s.busiest_count);
  return std::string(buf);
}

}
