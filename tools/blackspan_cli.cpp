#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "blackspan/aggregate.h"
#include "blackspan/anomaly.h"
#include "blackspan/assemble.h"
#include "blackspan/calibrate.h"
#include "blackspan/channel.h"
#include "blackspan/diff.h"
#include "blackspan/export.h"
#include "blackspan/frame.h"
#include "blackspan/pipeline.h"
#include "blackspan/query.h"
#include "blackspan/report.h"
#include "blackspan/rewrite.h"
#include "blackspan/schema.h"
#include "blackspan/session.h"
#include "blackspan/stats.h"
#include "blackspan/timeline.h"
#include "blackspan/validate.h"
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
    std::printf("channel %u \"%s\" v%u samples=%zu stride=%zu blocks=%zu keyframes=%zu\n",
                id, s->schema().name.c_str(), s->schema_version(),
                s->total_samples(), s->stride(), s->block_count(),
                s->keyframe_sample_indices().size());
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

bool parse_kind_mask(const char* spec, uint32_t& mask, std::string& bad) {
  mask = 0;
  std::string s = spec;
  size_t start = 0;
  while (start <= s.size()) {
    size_t comma = s.find(',', start);
    std::string tok =
        s.substr(start, comma == std::string::npos ? std::string::npos
                                                   : comma - start);
    if (!tok.empty()) {
      bool matched = false;
      for (size_t k = 0; k < kTimelineKindCount; ++k) {
        TimelineKind kind = static_cast<TimelineKind>(k);
        if (tok == timeline_kind_name(kind)) {
          mask |= timeline_kind_bit(kind);
          matched = true;
          break;
        }
      }
      if (!matched) {
        bad = tok;
        return false;
      }
    }
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  if (mask == 0) mask = kTimelineAllKinds;
  return true;
}

bool parse_record_type(const std::string& name, RecordType& out) {
  for (int v = 0x01; v <= 0x0C; ++v) {
    RecordType t = static_cast<RecordType>(v);
    if (name == record_type_name(t)) {
      out = t;
      return true;
    }
  }
  return false;
}

int write_output(const std::vector<uint8_t>& bytes, int argc, char** argv,
                 size_t frame_count) {
  const char* out_path = flag_value(argc, argv, "--out");
  if (!out_path) {
    std::fwrite(bytes.data(), 1, bytes.size(), stdout);
    return 0;
  }
  FILE* f = std::fopen(out_path, "wb");
  if (!f) {
    std::fprintf(stderr, "error: cannot write %s\n", out_path);
    return 2;
  }
  size_t written = std::fwrite(bytes.data(), 1, bytes.size(), f);
  std::fclose(f);
  if (written != bytes.size()) {
    std::fprintf(stderr, "error: short write to %s\n", out_path);
    return 2;
  }
  std::fprintf(stderr, "%s: %zu frames, %zu bytes\n", out_path, frame_count,
               bytes.size());
  return 0;
}

int cmd_calibrate(const char* path, int argc, char** argv) {
  const char* ch = flag_value(argc, argv, "--channel");
  if (!ch) {
    std::fprintf(stderr, "error: calibrate requires --channel <id>\n");
    return 2;
  }
  const char* fl = flag_value(argc, argv, "--field");
  uint16_t channel_id = static_cast<uint16_t>(std::strtoul(ch, nullptr, 0));
  size_t field = fl ? static_cast<size_t>(std::strtoul(fl, nullptr, 0)) : 0;

  CalibrationTable table;
  const char* table_path = flag_value(argc, argv, "--table");
  if (table_path) {
    bool ok = false;
    std::vector<uint8_t> tbytes = read_file(table_path, ok);
    if (!ok) {
      std::fprintf(stderr, "error: cannot read %s\n", table_path);
      return 2;
    }
    CalibrationParse parse =
        parse_calibration(std::string(tbytes.begin(), tbytes.end()));
    if (!parse.ok()) {
      for (const CalibrationDiagnostic& d : parse.errors) {
        std::fprintf(stderr, "%s:%zu: %s (%s)\n", table_path, d.line,
                     calibration_error_str(d.error), d.detail.c_str());
      }
      return 2;
    }
    table = parse.table;
  }

  std::vector<uint8_t> buf;
  DecodedRecording rec;
  if (!load_decoded(path, buf, rec)) return 2;

  std::string unit;
  std::vector<double> values =
      table.apply_column(rec.channels(), channel_id, field, &unit);
  std::printf("channel %u field %zu%s%s: %zu samples\n", channel_id, field,
              unit.empty() ? "" : " in ", unit.c_str(), values.size());
  for (size_t i = 0; i < values.size(); ++i) {
    std::printf("  %zu %.6g\n", i, values[i]);
  }
  return 0;
}

int cmd_slice(const char* path, int argc, char** argv) {
  bool ok = false;
  std::vector<uint8_t> buf = read_file(path, ok);
  if (!ok) {
    std::fprintf(stderr, "error: cannot read %s\n", path);
    return 2;
  }

  RewriteFilter filter;
  const char* dev = flag_value(argc, argv, "--device");
  if (dev) filter.device = static_cast<uint32_t>(std::strtoul(dev, nullptr, 0));
  const char* since = flag_value(argc, argv, "--since");
  if (since) filter.since = std::strtoull(since, nullptr, 0);
  const char* until = flag_value(argc, argv, "--until");
  if (until) filter.until = std::strtoull(until, nullptr, 0);
  const char* seq_lo = flag_value(argc, argv, "--seq-lo");
  if (seq_lo) filter.seq_lo = static_cast<uint32_t>(std::strtoul(seq_lo, nullptr, 0));
  const char* seq_hi = flag_value(argc, argv, "--seq-hi");
  if (seq_hi) filter.seq_hi = static_cast<uint32_t>(std::strtoul(seq_hi, nullptr, 0));

  const char* types = flag_value(argc, argv, "--types");
  if (types) {
    std::string s = types;
    size_t start = 0;
    while (start <= s.size()) {
      size_t comma = s.find(',', start);
      std::string tok = s.substr(
          start, comma == std::string::npos ? std::string::npos : comma - start);
      if (!tok.empty()) {
        RecordType t;
        if (!parse_record_type(tok, t)) {
          std::fprintf(stderr, "error: unknown record type '%s'\n", tok.c_str());
          return 2;
        }
        filter.types.push_back(t);
      }
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
  }

  RewriteResult result = rewrite_slice(buf, filter);
  if (result.input_status != FrameStatus::Ok &&
      result.input_status != FrameStatus::Incomplete) {
    std::fprintf(stderr, "warning: input decode halted: %s\n",
                 frame_status_str(result.input_status));
  }
  std::fprintf(stderr, "sliced %zu of %zu frames\n", result.frames_kept,
               result.frames_in);
  return write_output(result.bytes, argc, argv, result.frames_kept);
}

int cmd_merge(int argc, char** argv) {

  std::vector<std::vector<uint8_t>> inputs;
  for (int i = 2; i < argc; ++i) {
    if (std::strncmp(argv[i], "--", 2) == 0) {
      ++i;
      continue;
    }
    bool ok = false;
    std::vector<uint8_t> buf = read_file(argv[i], ok);
    if (!ok) {
      std::fprintf(stderr, "error: cannot read %s\n", argv[i]);
      return 2;
    }
    inputs.push_back(std::move(buf));
  }
  if (inputs.size() < 2) {
    std::fprintf(stderr, "usage: blackspan merge <in1> <in2> [...] --out <file>\n");
    return 2;
  }

  MergeResult result = rewrite_merge(inputs);
  if (result.inputs_truncated) {
    std::fprintf(stderr, "warning: %zu input(s) ended on a partial or corrupt "
                         "frame\n",
                 result.inputs_truncated);
  }
  std::fprintf(stderr, "merged %zu inputs into %zu frames\n", inputs.size(),
               result.frames_out);
  return write_output(result.bytes, argc, argv, result.frames_out);
}

int cmd_diff(const char* left_path, const char* right_path) {
  std::vector<uint8_t> lbuf, rbuf;
  DecodedRecording left, right;
  if (!load_decoded(left_path, lbuf, left)) return 2;
  if (!load_decoded(right_path, rbuf, right)) return 2;

  RecordingDiff diff = RecordingDiff::compare(left, right);
  std::string out = render_diff(diff);
  std::fwrite(out.data(), 1, out.size(), stdout);

  return diff.identical() ? 0 : 1;
}

int cmd_validate(const char* path, int argc, char** argv) {
  std::vector<uint8_t> buf;
  DecodedRecording rec;
  if (!load_decoded(path, buf, rec)) return 2;

  ValidationOptions opts;
  if (flag_value(argc, argv, "--no-notes")) opts.include_notes = false;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--no-notes") == 0) opts.include_notes = false;
  }

  ValidationReport report = ValidationReport::run(rec, opts);
  std::string out = render_validation(report);
  std::fwrite(out.data(), 1, out.size(), stdout);

  switch (report.summary().worst()) {
    case Severity3::Error: return 2;
    case Severity3::Warning: return 1;
    case Severity3::Note: return 0;
  }
  return 0;
}

int cmd_timeline(const char* path, int argc, char** argv) {
  std::vector<uint8_t> buf;
  DecodedRecording rec;
  if (!load_decoded(path, buf, rec)) return 2;

  TimelineOptions opts;
  const char* dev = flag_value(argc, argv, "--device");
  if (dev) opts.device = static_cast<uint32_t>(std::strtoul(dev, nullptr, 0));
  const char* since = flag_value(argc, argv, "--since");
  if (since) opts.since = std::strtoull(since, nullptr, 0);
  const char* until = flag_value(argc, argv, "--until");
  if (until) opts.until = std::strtoull(until, nullptr, 0);
  const char* kinds = flag_value(argc, argv, "--kinds");
  if (kinds) {
    std::string bad;
    if (!parse_kind_mask(kinds, opts.kind_mask, bad)) {
      std::fprintf(stderr, "error: unknown timeline kind '%s'\n", bad.c_str());
      return 2;
    }
  }

  Timeline tl = Timeline::build(rec, opts);
  std::string out = render_timeline(tl);
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

int cmd_aggregate(const char* path, int argc, char** argv) {
  const char* ch = flag_value(argc, argv, "--channel");
  if (!ch) {
    std::fprintf(stderr, "error: aggregate requires --channel <id>\n");
    return 2;
  }
  const char* wn = flag_value(argc, argv, "--window");
  if (!wn) {
    std::fprintf(stderr, "error: aggregate requires --window <ns>\n");
    return 2;
  }
  const char* fl = flag_value(argc, argv, "--field");
  const char* fn_name = flag_value(argc, argv, "--fn");

  AggFn fn = AggFn::Mean;
  if (fn_name && !parse_agg_fn(fn_name, fn)) {
    std::fprintf(stderr, "error: unknown aggregation function '%s'\n", fn_name);
    return 2;
  }

  std::vector<uint8_t> buf;
  DecodedRecording rec;
  if (!load_decoded(path, buf, rec)) return 2;

  uint16_t channel_id = static_cast<uint16_t>(std::strtoul(ch, nullptr, 0));
  size_t field = fl ? static_cast<size_t>(std::strtoul(fl, nullptr, 0)) : 0;
  uint64_t window_ns = std::strtoull(wn, nullptr, 0);

  std::vector<AggBucket> buckets =
      aggregate_channel(rec.channels(), channel_id, field, window_ns, fn);
  std::string rendered = render_buckets(buckets, fn);
  std::fputs(rendered.c_str(), stdout);
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

int cmd_assemble(const char* path, int argc, char** argv) {
  bool ok = false;
  std::vector<uint8_t> src = read_file(path, ok);
  if (!ok) {
    std::fprintf(stderr, "error: cannot read %s\n", path);
    return 2;
  }
  AssembleResult result =
      assemble_script(std::string(src.begin(), src.end()));
  if (!result.ok()) {
    for (const AssembleDiagnostic& d : result.errors) {
      std::fprintf(stderr, "%s: %s\n", path,
                   describe_assemble_diagnostic(d).c_str());
    }
    std::fprintf(stderr, "%zu error(s)\n", result.errors.size());
    return 1;
  }

  const char* out_path = flag_value(argc, argv, "--out");
  if (!out_path) {
    std::fwrite(result.bytes.data(), 1, result.bytes.size(), stdout);
    return 0;
  }
  FILE* f = std::fopen(out_path, "wb");
  if (!f) {
    std::fprintf(stderr, "error: cannot write %s\n", out_path);
    return 2;
  }
  size_t written = std::fwrite(result.bytes.data(), 1, result.bytes.size(), f);
  std::fclose(f);
  if (written != result.bytes.size()) {
    std::fprintf(stderr, "error: short write to %s\n", out_path);
    return 2;
  }
  std::fprintf(stderr, "%s: %zu frames, %zu bytes\n", out_path,
               result.frame_count, result.bytes.size());
  return 0;
}

int cmd_import(int argc, char** argv) {

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
      "  blackspan timeline <recording.bsp> [--device <id>] [--kinds a,b] "
      "[--since <ts>] [--until <ts>]\n"
      "  blackspan validate <recording.bsp> [--no-notes]\n"
      "  blackspan diff     <baseline.bsp> <comparand.bsp>\n"
      "  blackspan slice    <recording.bsp> [--device <id>] [--types a,b] "
      "[--since <ts>] [--until <ts>] [--seq-lo <n>] [--seq-hi <n>] "
      "[--out <file>]\n"
      "  blackspan merge    <in1.bsp> <in2.bsp> [...] [--out <file>]\n"
      "  blackspan calibrate <recording.bsp> --channel <id> [--field <n>] "
      "[--table <cal.txt>]\n"
      "  blackspan query    <recording.bsp> \"select ... \"\n"
      "  blackspan stats    <recording.bsp> --channel <id> [--field <n>]\n"
      "  blackspan aggregate <recording.bsp> --channel <id> --window <ns> "
      "[--field <n>] [--fn mean|min|max|first|last|count|sum]\n"
      "  blackspan export   <recording.bsp> --channel <id> "
      "[--format csv|tsv|ndjson]\n"
      "  blackspan import   <name> <recording.bsp> --workspace <dir>\n"
      "  blackspan inspect  --workspace <dir>\n"
      "  blackspan assemble <script.bsr> [--out <recording.bsp>]\n");
}

}

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
  if (cmd == "timeline") {
    const char* p = need_file(3);
    return p ? cmd_timeline(p, argc, argv) : 2;
  }
  if (cmd == "validate") {
    const char* p = need_file(3);
    return p ? cmd_validate(p, argc, argv) : 2;
  }
  if (cmd == "diff") {
    if (argc < 4) {
      usage();
      return 2;
    }
    return cmd_diff(argv[2], argv[3]);
  }
  if (cmd == "slice") {
    const char* p = need_file(3);
    return p ? cmd_slice(p, argc, argv) : 2;
  }
  if (cmd == "merge") return cmd_merge(argc, argv);
  if (cmd == "calibrate") {
    const char* p = need_file(3);
    return p ? cmd_calibrate(p, argc, argv) : 2;
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
  if (cmd == "aggregate") {
    const char* p = need_file(3);
    return p ? cmd_aggregate(p, argc, argv) : 2;
  }
  if (cmd == "export") {
    const char* p = need_file(3);
    return p ? cmd_export(p, argc, argv) : 2;
  }
  if (cmd == "assemble") {
    const char* p = need_file(3);
    return p ? cmd_assemble(p, argc, argv) : 2;
  }
  if (cmd == "import") return cmd_import(argc, argv);
  if (cmd == "inspect") return cmd_inspect(argc, argv);

  usage();
  return 2;
}
