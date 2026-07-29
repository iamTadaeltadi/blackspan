#ifndef BLACKSPAN_REPORT_GRAPH_H
#define BLACKSPAN_REPORT_GRAPH_H

#include <cstdint>
#include <string>
#include <vector>

#include "blackspan/anomaly.h"
#include "blackspan/pipeline.h"
#include "blackspan/session.h"

namespace blackspan {

size_t state_index_extent(const AnomalyEngine& anomalies);

std::string render_state_machine_dot(const AnomalyEngine& anomalies);

std::string render_state_machine_dot(const AnomalyEngine& anomalies,
                                     uint32_t device_id);

std::string render_session_timeline_dot(const std::vector<Session>& sessions);

std::string render_html_report(const DecodedRecording& rec);

std::string dot_escape(const std::string& s);

std::string html_escape(const std::string& s);

}

#endif
