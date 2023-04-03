#ifndef BLACKSPAN_FAULT_TAXONOMY_H
#define BLACKSPAN_FAULT_TAXONOMY_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "blackspan/anomaly.h"
#include "blackspan/pipeline.h"

namespace blackspan {

struct FaultClass {
  uint16_t fault_code = 0;
  uint8_t subsystem_id = 0;
  size_t count = 0;
  Severity worst_severity = Severity::Info;
  uint64_t first_ts = 0;
  uint64_t last_ts = 0;
  size_t device_count = 0;
};

std::vector<FaultClass> classify_faults(const std::vector<FaultEvent>& faults);

struct SubsystemLoad {
  uint8_t subsystem_id = 0;
  size_t total = 0;
  std::array<size_t, 4> by_severity{};
};

std::vector<SubsystemLoad> subsystem_load(const std::vector<FaultEvent>& faults);

struct Recurrence {
  uint16_t fault_code = 0;
  uint32_t device_id = 0;
  size_t occurrences = 0;
  uint64_t mean_interval_ns = 0;
  uint64_t min_interval_ns = 0;
};

std::vector<Recurrence> recurring_faults(const std::vector<FaultEvent>& faults,
                                         size_t min_occurrences);

struct EscalationRun {
  uint32_t device_id = 0;
  uint64_t start_ts = 0;
  uint64_t end_ts = 0;
  size_t length = 0;
  Severity peak = Severity::Info;
};

std::vector<EscalationRun> severity_escalations(
    const std::vector<FaultEvent>& faults, size_t min_length);

struct TaxonomySummary {
  size_t total_faults = 0;
  size_t distinct_codes = 0;
  size_t distinct_subsystems = 0;
  Severity worst = Severity::Info;
  uint16_t top_code = 0;
  size_t top_code_count = 0;
};

TaxonomySummary summarize_taxonomy(const std::vector<FaultEvent>& faults);

std::string describe_fault_class(const FaultClass& c);
std::string describe_subsystem_load(const SubsystemLoad& l);
std::string describe_recurrence(const Recurrence& r);
std::string describe_escalation(const EscalationRun& e);
std::string describe_taxonomy(const TaxonomySummary& s);
std::string render_taxonomy(const std::vector<FaultClass>& classes);

std::vector<FaultClass> taxonomy_of(const DecodedRecording& rec);

}

#endif
