#include "blackspan/rank.h"

#include <algorithm>
#include <numeric>

namespace blackspan {

namespace {
constexpr size_t kRankTableCap = 256;
}

ChannelRanks field_ranks(const ChannelStore& store, uint16_t channel_id,
                         size_t field_index) {
  ChannelRanks r;
  r.channel_id = channel_id;
  r.field_index = field_index;

  const ChannelSeries* series = store.series(channel_id);
  if (series == nullptr) return r;
  if (field_index < series->schema().fields.size()) {
    r.field_name = series->schema().fields[field_index].name;
  }

  const std::vector<double> col = store.column_as_doubles(channel_id, field_index);
  const size_t n = col.size();
  r.samples = n;
  if (n == 0) return r;

  std::vector<size_t> order(n);
  std::iota(order.begin(), order.end(), size_t{0});
  std::sort(order.begin(), order.end(),
            [&](size_t a, size_t b) { return col[a] > col[b]; });

  std::vector<uint32_t> rank_lookup(n);
  for (size_t rank = 0; rank < n; ++rank) {
    rank_lookup[order[rank]] = static_cast<uint32_t>(rank);
  }

  const size_t cap = std::min(kRankTableCap, n);
  r.ranks.assign(cap, 0u);
  for (size_t i = 0; i < n; ++i) {
    r.ranks[i] = rank_lookup[i];
  }
  return r;
}

std::vector<ChannelRanks> channel_ranks(const ChannelStore& store) {
  std::vector<ChannelRanks> out;
  for (uint16_t id : store.channels()) {
    const ChannelSeries* series = store.series(id);
    if (series == nullptr) continue;
    const size_t fields = series->schema().fields.size();
    for (size_t f = 0; f < fields; ++f) {
      out.push_back(field_ranks(store, id, f));
    }
  }
  return out;
}

std::string describe_ranks(const ChannelRanks& r) {
  std::string out = "channel " + std::to_string(r.channel_id) + " field " +
                    r.field_name + " ranks over " + std::to_string(r.samples) +
                    " samples";
  return out;
}

}
