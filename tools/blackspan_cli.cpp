#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "blackspan/anomaly.h"
#include "blackspan/channel.h"
#include "blackspan/export.h"
#include "blackspan/frame.h"
#include "blackspan/pipeline.h"
#include "blackspan/query.h"
#include "blackspan/report.h"
#include "blackspan/schema.h"
#include "blackspan/session.h"
#include "blackspan/stats.h"
#include "blackspan/workspace.h"

namespace {

using namespace blackspan;

std::vector<uint8_t> read_file(const char* path, bool& ok) {
  ok = false;
  FILE* f = std::fopen(path, "rb");
  if (!f) return {};
  std::fseek(f, 0, SEEK_END);
  long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> buf;
  if (n > 0) {
    buf.resize(static_cast<size_t>(n));
    if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) {
      std::fclose(f);
      return {};
    }
  }
  std::fclose(f);
  ok = true;
  return buf;
}

// Returns the value following a --flag argument, or nullptr if absent.
const char* flag_value(int argc, char** argv, const char* name) {
  for (int i = 0; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
  }
  return nullptr;
}

bool load_decoded(const char* path, std::vector<uint8_t>& buf,
                  DecodedRecording& rec) {
  bool ok = false;
  buf = read_file(path, ok);
  if (!ok) {
    std::fprintf(stderr, "error: cannot read %s\n", path);
    return false;
  }
  rec.decode(buf.data(), buf.size());
  return true;
}

int cmd_frames(const char* path) {
  bool ok = false;
  std::vector<uint8_t> buf = read_file(path, ok);
  if (!ok) {
    std::fprintf(stderr, "error: cannot read %s\n", path);
    return 2;
  }
  FrameStream fs(buf.data(), buf.size());
  Frame f;
  size_t idx = 0;
  while (fs.next(f)) {
    std::printf("#%zu %-16s dev=%08x seq=%u ts=%llu len=%u\n", idx++,
                record_type_name(f.header.type), f.header.device_id,
                f.header.session_seq,
                static_cast<unsigned long long>(f.header.monotonic_ts),
                f.header.payload_len);
  }
  if (fs.status() != FrameStatus::Ok &&
      fs.status() != FrameStatus::Incomplete) {
    std::fprintf(stderr, "decode stopped at offset %zu: %s\n", fs.offset(),
                 frame_status_str(fs.status()));
    return 1;
  }
  std::printf("%zu frames (%s)\n", idx, frame_status_str(fs.status()));
  return 0;
}

int cmd_sessions(const char* path) {
  std::vector<uint8_t> buf;
  DecodedRecording rec;
  if (!load_decoded(path, buf, rec)) return 2;
  for (const Session& s : rec.sessions().sessions()) {
    std::printf("%s\n", describe_session(s).c_str());
  }
  const auto& diags = rec.sessions().diagnostics();
  std::printf("%zu sessions, %zu diagnostics\n",
              rec.sessions().sessions().size(), diags.size());
  for (const auto& d : diags) {
    std::printf("  diag %s dev=%08x seq=%u %s\n", session_diag_str(d.code),
                d.device_id, d.session_seq, d.detail.c_str());
  }
  return 0;
}

int cmd_schemas(const char* path) {
  std::vector<uint8_t> buf;
  DecodedRecording rec;
  if (!load_decoded(path, buf, rec)) return 2;
  for (uint16_t id : rec.schemas().channels()) {
    const ChannelSchema* cs = rec.schemas().latest(id);
    if (cs) std::printf("%s\n", cs->describe().c_str());
  }
  std::printf("%zu channels defined\n", rec.schemas().count());
  return 0;
}

int cmd_channels(const char* path) {
  std::vector<uint8_t> buf;
  DecodedRecording rec;
  if (!load_decoded(path, buf, rec)) return 2;
  for (uint16_t id : rec.channels().channels()) {
    const ChannelSeries* s = rec.channels().series(id);
    if (!s) continue;
    std::printf("channel %u \"%s\" v%u samples=%zu stride=%zu blocks=%zu\n", id,
                s->schema().name.c_str(), s->schema_version(),
                s->total_samples(), s->stride(), s->block_count());
  }
  std::printf("%zu channels, %zu total samples\n",
              rec.channels().channels().size(),
              rec.channels().total_samples());
  return 0;
}

int cmd_faults(const char* path) {
  std::vector<uint8_t> buf;
  DecodedRecording rec;
  if (!load_decoded(path, buf, rec)) return 2;
  for (const FaultEvent& f : rec.anomalies().faults()) {
    std::printf("fault dev=%08x seq=%u ts=%llu code=%u sev=%s subsystem=%u\n",
                f.device_id, f.session_seq,
                static_cast<unsigned long long>(f.monotonic_ts), f.fault_code,
                severity_name(f.severity), f.subsystem_id);
  }
  std::array<size_t, 4> by_sev = rec.anomalies().fault_count_by_severity();
  std::printf(
      "%zu faults (info=%zu warning=%zu error=%zu critical=%zu), %zu "
      "transitions\n",
      rec.anomalies().faults().size(), by_sev[0], by_sev[1], by_sev[2],
      by_sev[3], rec.anomalies().transitions().size());
  return 0;
}

int cmd_report(const char* path, int argc, char** argv) {
  std::vector<uint8_t> buf;
  DecodedRecording rec;
  if (!load_decoded(path, buf, rec)) return 2;

  ReportFormat fmt = ReportFormat::Text;
  const char* fs = flag_value(argc, argv, "--format");
  if (fs) {
    if (std::strcmp(fs, "text") == 0)
      fmt = ReportFormat::Text;
    else if (std::strcmp(fs, "json") == 0)
      fmt = ReportFormat::Json;
    else if (std::strcmp(fs, "dot") == 0)
      fmt = ReportFormat::Dot;
    else {
      std::fprintf(stderr, "error: unknown format '%s'\n", fs);
      return 2;
    }
  }

  const auto& sessions = rec.sessions().sessions();
  ReportModel model;
  model.sessions = &sessions;
  model.schemas = &rec.schemas();
  model.channels = &rec.channels();
  model.anomalies = &rec.anomalies();
  std::string out = render(model, fmt);
  std::fwrite(out.data(), 1, out.size(), stdout);
  return 0;
}

int cmd_query(const char* path, const char* query) {
  std::vector<uint8_t> buf;
  DecodedRecording rec;
  if (!load_decoded(path, buf, rec)) return 2;
  const auto& sessions = rec.sessions().sessions();
  QueryEngine engine(&sessions, &rec.channels(), &rec.anomalies(),
                     &rec.schemas());
  QueryResult result;
  QueryError err = engine.run(query, result);
  if (err != QueryError::None) {
    std::fprintf(stderr, "query error: %s\n", query_error_str(err));
    return 1;
  }
  std::printf("%s: %zu rows\n", result.entity.c_str(), result.rows.size());
  for (const QueryResultRow& row : result.rows) {
    for (size_t i = 0; i < row.columns.size(); ++i) {
      std::printf("%s%s=%s", i ? " " : "  ", row.columns[i].first.c_str(),
                  row.columns[i].second.c_str());
    }
    std::printf("\n");
  }
  return 0;
}

int cmd_stats(const char* path, int argc, char** argv) {
  const char* ch = flag_value(argc, argv, "--channel");
  if (!ch) {
    std::fprintf(stderr, "error: stats requires --channel <id>\n");
    return 2;
  }
  const char* fl = flag_value(argc, argv, "--field");
  std::vector<uint8_t> buf;
  DecodedRecording rec;
  if (!load_decoded(path, buf, rec)) return 2;

  uint16_t channel_id = static_cast<uint16_t>(std::strtoul(ch, nullptr, 0));
  size_t field = fl ? static_cast<size_t>(std::strtoul(fl, nullptr, 0)) : 0;
  ColumnStats out;
  StatsError err = compute_column_stats(rec.channels(), channel_id, field, out);
  if (err != StatsError::None) {
    std::fprintf(stderr, "stats error: %s\n", stats_error_str(err));
    return 1;
  }
  std::printf(
      "channel %u field %zu: count=%zu min=%.6g max=%.6g mean=%.6g "
      "stddev=%.6g sum=%.6g\n",
      channel_id, field, out.count, out.min, out.max, out.mean, out.stddev,
      out.sum);
  return 0;
}

int cmd_export(const char* path, int argc, char** argv) {
  const char* ch = flag_value(argc, argv, "--channel");
  if (!ch) {
    std::fprintf(stderr, "error: export requires --channel <id>\n");
    return 2;
  }
  const char* fmt = flag_value(argc, argv, "--format");
  std::string format = fmt ? fmt : "csv";

  std::vector<uint8_t> buf;
  DecodedRecording rec;
  if (!load_decoded(path, buf, rec)) return 2;
  uint16_t channel_id = static_cast<uint16_t>(std::strtoul(ch, nullptr, 0));

  std::string out;
  TableExportError err = TableExportError::None;
  if (format == "csv") {
    err = export_channel_delimited(rec.channels(), rec.schemas(), channel_id,
                                   DelimFormat::Csv, out);
  } else if (format == "tsv") {
    err = export_channel_delimited(rec.channels(), rec.schemas(), channel_id,
                                   DelimFormat::Tsv, out);
  } else if (format == "ndjson") {
    err = export_channel_ndjson(rec.channels(), rec.schemas(), channel_id, out);
  } else {
    std::fprintf(stderr, "error: unknown export format '%s'\n", format.c_str());
    return 2;
  }
  if (err != TableExportError::None) {
    std::fprintf(stderr, "export error: %s\n", table_export_error_str(err));
    return 1;
  }
  std::fwrite(out.data(), 1, out.size(), stdout);
  return 0;
}

int cmd_import(int argc, char** argv) {
  // blackspan import <name> <file> --workspace <dir>
  if (argc < 4) {
    std::fprintf(stderr,
                 "usage: blackspan import <name> <file> --workspace <dir>\n");
    return 2;
  }
  const char* name = argv[2];
  const char* file = argv[3];
  const char* dir = flag_value(argc, argv, "--workspace");
  if (!dir) {
    std::fprintf(stderr, "error: import requires --workspace <dir>\n");
    return 2;
  }
  bool ok = false;
  std::vector<uint8_t> buf = read_file(file, ok);
  if (!ok) {
    std::fprintf(stderr, "error: cannot read %s\n", file);
    return 2;
  }
  auto backend = std::make_shared<FileBackend>(dir);
  Workspace ws(backend, std::string(dir) + "/catalog.tsv");
  WorkspaceError err = ws.import(name, buf.data(), buf.size());
  if (err != WorkspaceError::Ok) {
    std::fprintf(stderr, "import error: %s\n", workspace_error_str(err));
    return 1;
  }
  std::printf("imported %s (%zu bytes)\n", name, buf.size());
  return 0;
}

int cmd_inspect(int argc, char** argv) {
  const char* dir = flag_value(argc, argv, "--workspace");
  if (!dir) {
    std::fprintf(stderr, "error: inspect requires --workspace <dir>\n");
    return 2;
  }
  auto backend = std::make_shared<FileBackend>(dir);
  Workspace ws(backend, std::string(dir) + "/catalog.tsv");
  for (const RecordingInfo& info : ws.list()) {
    std::printf("%s  %s  %llu bytes  %llu frames (%s)\n", info.name.c_str(),
                info.content_id.c_str(),
                static_cast<unsigned long long>(info.byte_size),
                static_cast<unsigned long long>(info.summary.total_frames),
                frame_status_str(info.summary.terminal_status));
  }
  std::printf("%zu recordings\n", ws.size());
  return 0;
}

void usage() {
  std::fprintf(
      stderr,
      "blackspan - offline diagnostic recorder decoder\n"
      "usage:\n"
      "  blackspan frames   <recording.bsp>\n"
      "  blackspan sessions <recording.bsp>\n"
      "  blackspan schemas  <recording.bsp>\n"
      "  blackspan channels <recording.bsp>\n"
      "  blackspan faults   <recording.bsp>\n"
      "  blackspan report   <recording.bsp> [--format text|json|dot]\n"
      "  blackspan query    <recording.bsp> \"select ... \"\n"
      "  blackspan stats    <recording.bsp> --channel <id> [--field <n>]\n"
      "  blackspan export   <recording.bsp> --channel <id> "
      "[--format csv|tsv|ndjson]\n"
      "  blackspan import   <name> <recording.bsp> --workspace <dir>\n"
      "  blackspan inspect  --workspace <dir>\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  const std::string cmd = argv[1];
  auto need_file = [&](int min_args) -> const char* {
    if (argc < min_args) {
      usage();
      return nullptr;
    }
    return argv[2];
  };

  if (cmd == "frames") {
    const char* p = need_file(3);
    return p ? cmd_frames(p) : 2;
  }
  if (cmd == "sessions") {
    const char* p = need_file(3);
    return p ? cmd_sessions(p) : 2;
  }
  if (cmd == "schemas") {
    const char* p = need_file(3);
    return p ? cmd_schemas(p) : 2;
  }
  if (cmd == "channels") {
    const char* p = need_file(3);
    return p ? cmd_channels(p) : 2;
  }
  if (cmd == "faults") {
    const char* p = need_file(3);
    return p ? cmd_faults(p) : 2;
  }
  if (cmd == "report") {
    const char* p = need_file(3);
    return p ? cmd_report(p, argc, argv) : 2;
  }
  if (cmd == "query") {
    if (argc < 4) {
      usage();
      return 2;
    }
    return cmd_query(argv[2], argv[3]);
  }
  if (cmd == "stats") {
    const char* p = need_file(3);
    return p ? cmd_stats(p, argc, argv) : 2;
  }
  if (cmd == "export") {
    const char* p = need_file(3);
    return p ? cmd_export(p, argc, argv) : 2;
  }
  if (cmd == "import") return cmd_import(argc, argv);
  if (cmd == "inspect") return cmd_inspect(argc, argv);

  usage();
  return 2;
}
