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

// The derive subsystem computes derived channels: virtual channels whose
// samples are given by a text formula over the fields of recorded channels. A
// derived-def frame names a derived id, a primary channel, a display name, and
// an ASCII expression; this subsystem decodes that record, parses the
// expression once into an AST (via the expr language), and evaluates it against
// a ChannelStore to produce a per-sample value series aligned to the primary
// channel's timeline.
//
// Evaluation walks the primary channel's samples in order. A bare identifier in
// the formula binds to a field of the same-indexed primary sample; a
// ch<id>.<field> reference binds to the same-indexed sample of another channel.
// Division by zero yields a NaN value and is not fatal; a reference that cannot
// be resolved (an unknown field, an absent channel, or a channel with no sample
// at the needed index) is an error for that definition. Nothing here throws:
// every failure is reported as a DeriveError value.

namespace blackspan {

// Error codes for derived-def decoding, registration, and evaluation. Errors
// are values; nothing in this subsystem throws.
enum class DeriveError : uint8_t {
  None = 0,
  NotDerivedDef,     // ingest_frame was handed a non-derived-def frame
  Truncated,         // a derived-def payload ended before its grammar
  ParseFailed,       // the expression did not parse
  DuplicateId,       // a derived_id was already registered
  UnknownId,         // evaluate/get named an unregistered derived_id
  NoPrimaryChannel,  // the primary channel has no series in the store
  UnknownFieldRef,   // a referenced field or channel is absent at eval time
  ChannelTooShort,   // a cross-channel ref had no sample at the primary index
};

// Human-readable form of a DeriveError. Never null.
const char* derive_error_str(DeriveError e);

// A decoded derived-def (0x0B) record: the derived channel's id, the primary
// channel whose samples drive evaluation, a display name, and the formula.
struct DerivedDefRecord {
  uint16_t derived_id = 0;
  uint16_t primary_channel = 0;
  std::string name;
  std::string expr;
};

// Decodes a derived-def frame payload. Grammar (all little-endian):
//   [derived_id u16][primary_channel u16][name_len u8][name bytes]
//   [expr_len u16][expr bytes]
// Robust to truncation: a payload that ends before any declared field or its
// bytes yields DeriveError::Truncated. Bytes trailing a complete record are
// ignored so the format can grow. On success returns DeriveError::None.
DeriveError decode_derived_def(const uint8_t* payload, size_t len,
                               DerivedDefRecord& out);

// The result of evaluating a derived channel: one value per primary sample,
// with the timestamp each value inherits from its primary sample's block. The
// two vectors always have equal length.
struct DerivedSeries {
  uint16_t derived_id = 0;
  std::vector<double> values;
  std::vector<uint64_t> timestamps;
};

// Holds the registered derived-channel definitions and evaluates them against a
// ChannelStore. Each definition's expression is parsed once at registration and
// the AST is retained, so evaluation over many samples never re-parses.
class DeriveEngine {
 public:
  // Registers a definition, parsing its expression to an AST. Returns
  // DuplicateId when the derived_id is already registered, ParseFailed when the
  // expression is malformed (and, if parse_err / parse_pos are non-null, the
  // specific ExprError and its source position), or None on success.
  DeriveError register_def(const DerivedDefRecord& def,
                           ExprError* parse_err = nullptr,
                           size_t* parse_pos = nullptr);

  // Decodes a derived-def frame and registers it. Returns NotDerivedDef for a
  // frame of any other type, the decode error for a malformed payload, or the
  // registration result otherwise.
  DeriveError ingest_frame(const Frame& f, ExprError* parse_err = nullptr,
                           size_t* parse_pos = nullptr);

  // Evaluates a registered definition over `store`, filling `out` with a value
  // and timestamp per primary sample. Returns UnknownId for an unregistered id,
  // NoPrimaryChannel when the primary channel has no series, ChannelTooShort or
  // UnknownFieldRef when a reference cannot be resolved, or None on success. On
  // any error `out` is left empty.
  DeriveError evaluate(uint16_t derived_id, const ChannelStore& store,
                       DerivedSeries& out) const;

  // The registered derived ids, ascending.
  std::vector<uint16_t> defs() const;

  // The registered record for a derived id, or nullptr if none.
  const DerivedDefRecord* get(uint16_t derived_id) const;

  // Number of registered definitions.
  size_t count() const { return defs_.size(); }

 private:
  struct Entry {
    DerivedDefRecord def;
    std::unique_ptr<ExprNode> ast;
  };

  std::map<uint16_t, Entry> defs_;
};

}  // namespace blackspan

#endif  // BLACKSPAN_DERIVE_H
