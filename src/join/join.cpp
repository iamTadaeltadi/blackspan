#include "blackspan/join.h"

#include <algorithm>

namespace blackspan {
namespace {

using Sample = std::pair<uint64_t, double>;

bool by_ts(const Sample& a, const Sample& b) { return a.first < b.first; }

std::vector<Sample> channel_column(const ChannelStore& store, uint16_t channel_id,
                                   size_t field_index) {
  std::vector<Sample> out;
  const ChannelSeries* series = store.series(channel_id);
  if (series == nullptr) return out;
  if (field_index >= series->schema().fields.size()) return out;

  size_t global = 0;
  for (size_t b = 0; b < series->block_count(); ++b) {
    const uint64_t base_ts = series->block_base_ts(b);
    const uint16_t n = series->block_sample_count(b);
    for (uint16_t i = 0; i < n; ++i, ++global) {
      const std::optional<SampleView> view = series->view(global);
      if (!view.has_value()) continue;
      const std::optional<double> value = view->get_as_double(field_index);
      if (!value.has_value()) continue;
      out.emplace_back(base_ts, value.value());
    }
  }
  return out;
}

std::vector<Sample> dedup_last(const std::vector<Sample>& sorted) {
  std::vector<Sample> out;
  out.reserve(sorted.size());
  for (const Sample& s : sorted) {
    if (!out.empty() && out.back().first == s.first) {
      out.back().second = s.second;
    } else {
      out.push_back(s);
    }
  }
  return out;
}

JoinResult inner_join(const std::vector<Sample>& left,
                      const std::vector<Sample>& right) {
  const std::vector<Sample> l = dedup_last(left);
  const std::vector<Sample> r = dedup_last(right);

  JoinResult res;
  size_t i = 0;
  size_t j = 0;
  while (i < l.size() && j < r.size()) {
    if (l[i].first == r[j].first) {
      JoinedRow row;
      row.ts = l[i].first;
      row.left = l[i].second;
      row.right = r[j].second;
      row.right_ts = r[j].first;
      res.rows.push_back(row);
      ++i;
      ++j;
    } else if (l[i].first < r[j].first) {
      ++res.left_unmatched;
      ++i;
    } else {
      ++res.right_unmatched;
      ++j;
    }
  }
  res.left_unmatched += l.size() - i;
  res.right_unmatched += r.size() - j;
  return res;
}

template <typename Pick>
JoinResult asof_join(const std::vector<Sample>& left,
                     const std::vector<Sample>& right, uint64_t tolerance_ns,
                     Pick pick) {
  JoinResult res;
  std::vector<char> used(right.size(), 0);

  for (const Sample& ls : left) {
    size_t idx = right.size();
    uint64_t gap = 0;
    if (!pick(right, ls.first, idx, gap) || (tolerance_ns != 0 && gap > tolerance_ns)) {
      ++res.left_unmatched;
      continue;
    }
    JoinedRow row;
    row.ts = ls.first;
    row.left = ls.second;
    row.right = right[idx].second;
    row.right_ts = right[idx].first;
    res.rows.push_back(row);
    used[idx] = 1;
  }

  for (char u : used) {
    if (!u) ++res.right_unmatched;
  }
  return res;
}

bool pick_backward(const std::vector<Sample>& right, uint64_t target, size_t& idx,
                   uint64_t& gap) {
  const auto ub = std::upper_bound(
      right.begin(), right.end(), target,
      [](uint64_t v, const Sample& e) { return v < e.first; });
  if (ub == right.begin()) return false;
  idx = static_cast<size_t>((ub - right.begin()) - 1);
  gap = target - right[idx].first;
  return true;
}

bool pick_forward(const std::vector<Sample>& right, uint64_t target, size_t& idx,
                  uint64_t& gap) {
  const auto lb = std::lower_bound(
      right.begin(), right.end(), target,
      [](const Sample& e, uint64_t v) { return e.first < v; });
  if (lb == right.end()) return false;
  idx = static_cast<size_t>(lb - right.begin());
  gap = right[idx].first - target;
  return true;
}

const std::vector<Sample>& sorted_view(const std::vector<Sample>& in,
                                       std::vector<Sample>& scratch) {
  if (std::is_sorted(in.begin(), in.end(), by_ts)) return in;
  scratch = in;
  std::stable_sort(scratch.begin(), scratch.end(), by_ts);
  return scratch;
}

}

const char* join_kind_name(JoinKind k) {
  switch (k) {
    case JoinKind::Inner: return "inner";
    case JoinKind::AsOfBackward: return "asof-backward";
    case JoinKind::AsOfForward: return "asof-forward";
  }
  return "unknown";
}

bool parse_join_kind(const std::string& s, JoinKind& out) {
  if (s == "inner") { out = JoinKind::Inner; return true; }
  if (s == "asof-backward") { out = JoinKind::AsOfBackward; return true; }
  if (s == "asof-forward") { out = JoinKind::AsOfForward; return true; }
  return false;
}

JoinResult join_series(const std::vector<std::pair<uint64_t, double>>& left,
                       const std::vector<std::pair<uint64_t, double>>& right,
                       JoinKind kind, uint64_t tolerance_ns) {
  std::vector<Sample> left_scratch;
  std::vector<Sample> right_scratch;
  const std::vector<Sample>& l = sorted_view(left, left_scratch);
  const std::vector<Sample>& r = sorted_view(right, right_scratch);

  switch (kind) {
    case JoinKind::Inner:
      return inner_join(l, r);
    case JoinKind::AsOfBackward:
      return asof_join(l, r, tolerance_ns, pick_backward);
    case JoinKind::AsOfForward:
      return asof_join(l, r, tolerance_ns, pick_forward);
  }
  return JoinResult{};
}

JoinResult join_channels(const ChannelStore& store, uint16_t left_channel,
                         size_t left_field, uint16_t right_channel,
                         size_t right_field, JoinKind kind,
                         uint64_t tolerance_ns) {
  return join_series(channel_column(store, left_channel, left_field),
                     channel_column(store, right_channel, right_field), kind,
                     tolerance_ns);
}

}
