#ifndef BLACKSPAN_DERIVE_H
#define BLACKSPAN_DERIVE_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "blackspan/channel.h"
#include "blackspan/expr.h"
#include "blackspan/frame.h"

namespace blackspan {

enum class DeriveError : uint8_t {
  None = 0,
  NotDerivedDef,
  Truncated,
  ParseFailed,
  DuplicateId,
  UnknownId,
  NoPrimaryChannel,
  UnknownFieldRef,
  ChannelTooShort,
};

const char* derive_error_str(DeriveError e);

struct DerivedDefRecord {
  uint16_t derived_id = 0;
  uint16_t primary_channel = 0;
  std::string name;
  std::string expr;
};

DeriveError decode_derived_def(const uint8_t* payload, size_t len,
                               DerivedDefRecord& out);

struct DerivedSeries {
  uint16_t derived_id = 0;
  std::vector<double> values;
  std::vector<uint64_t> timestamps;
};

class DeriveEngine {
 public:

  DeriveError register_def(const DerivedDefRecord& def,
                           ExprError* parse_err = nullptr,
                           size_t* parse_pos = nullptr);

  DeriveError ingest_frame(const Frame& f, ExprError* parse_err = nullptr,
                           size_t* parse_pos = nullptr);

  DeriveError evaluate(uint16_t derived_id, const ChannelStore& store,
                       DerivedSeries& out) const;

  std::vector<uint16_t> defs() const;

  const DerivedDefRecord* get(uint16_t derived_id) const;

  size_t count() const { return defs_.size(); }

 private:
  struct Entry {
    DerivedDefRecord def;
    std::unique_ptr<ExprNode> ast;
  };

  std::map<uint16_t, Entry> defs_;
};

}

#endif
