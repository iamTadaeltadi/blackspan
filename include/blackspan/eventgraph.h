#ifndef BLACKSPAN_EVENTGRAPH_H
#define BLACKSPAN_EVENTGRAPH_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "blackspan/anomaly.h"
#include "blackspan/pipeline.h"

namespace blackspan {

struct GraphNode {
  uint16_t state = 0;
  size_t in_degree = 0;
  size_t out_degree = 0;
  size_t visits = 0;
};

struct GraphEdge {
  uint16_t from = 0;
  uint16_t to = 0;
  size_t count = 0;
};

struct StateGraph {
  std::vector<GraphNode> nodes;
  std::vector<GraphEdge> edges;

  size_t node_index(uint16_t state) const;
};

StateGraph build_state_graph(const std::vector<TransitionEvent>& transitions);

StateGraph build_state_graph(const std::vector<TransitionEvent>& transitions,
                             uint32_t device_id);

std::vector<uint16_t> reachable_from(const StateGraph& g, uint16_t start);

bool has_path(const StateGraph& g, uint16_t from, uint16_t to);

std::vector<std::vector<uint16_t>> strongly_connected_components(
    const StateGraph& g);

std::vector<uint16_t> topological_order(const StateGraph& g, bool& acyclic);

std::vector<uint16_t> terminal_states(const StateGraph& g);

std::vector<uint16_t> source_states(const StateGraph& g);

GraphEdge busiest_edge(const StateGraph& g, bool& found);

size_t edge_count(const StateGraph& g, uint16_t from, uint16_t to);

struct PathResult {
  bool reachable = false;
  std::vector<uint16_t> states;
};

PathResult shortest_path(const StateGraph& g, uint16_t from, uint16_t to);

std::vector<uint32_t> bfs_depths(const StateGraph& g, uint16_t start);

uint32_t graph_eccentricity(const StateGraph& g, uint16_t start);

std::vector<GraphNode> hotspots(const StateGraph& g, size_t limit);

std::vector<uint16_t> unreachable_states(const StateGraph& g, uint16_t start);

std::vector<uint16_t> nodes_in_cycles(const StateGraph& g);

bool contains_cycle(const StateGraph& g);

struct GraphSummary {
  size_t nodes = 0;
  size_t edges = 0;
  size_t components = 0;
  bool acyclic = true;
  size_t self_loops = 0;
  uint16_t busiest_from = 0;
  uint16_t busiest_to = 0;
  size_t busiest_count = 0;
};

GraphSummary summarize_graph(const StateGraph& g);

StateGraph recording_state_graph(const DecodedRecording& rec);

std::string describe_graph_summary(const GraphSummary& s);

}

#endif
