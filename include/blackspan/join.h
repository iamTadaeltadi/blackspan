#ifndef BLACKSPAN_JOIN_H
#define BLACKSPAN_JOIN_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "blackspan/channel.h"

namespace blackspan {

enum class JoinKind : uint8_t {
  Inner,
  AsOfBackward,

  AsOfForward,

};

const char* join_kind_name(JoinKind k);

bool parse_join_kind(const std::string& s, JoinKind& out);

struct JoinedRow {
  uint64_t ts = 0;
  double left = 0.0;
  double right = 0.0;
  uint64_t right_ts = 0;
};

struct JoinResult {
  std::vector<JoinedRow> rows;
  size_t left_unmatched = 0;
  size_t right_unmatched = 0;
};

JoinResult join_series(const std::vector<std::pair<uint64_t, double>>& left,
                       const std::vector<std::pair<uint64_t, double>>& right,
                       JoinKind kind, uint64_t tolerance_ns);

JoinResult join_channels(const ChannelStore& store, uint16_t left_channel,
                         size_t left_field, uint16_t right_channel,
                         size_t right_field, JoinKind kind,
                         uint64_t tolerance_ns);

}

#endif
