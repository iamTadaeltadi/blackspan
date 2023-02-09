#include "blackspan/workspace.h"

#include <charconv>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

#include "blackspan/sha256.h"

namespace blackspan {

// ---------------------------------------------------------------------------
// ContentStore
// ---------------------------------------------------------------------------

ContentStore::ContentStore(std::shared_ptr<StorageBackend> backend)
    : backend_(std::move(backend)) {}

std::string ContentStore::put(const uint8_t* data, size_t len) {
  const std::string id = sha256(data, len).hex();
  if (backend_->has(id)) return id;
  if (!backend_->put(id, data, len)) return std::string();
  return id;
}

bool ContentStore::get(const std::string& id, std::vector<uint8_t>& out) const {
  return backend_->get(id, out);
}

bool ContentStore::has(const std::string& id) const {
  return backend_->has(id);
}

size_t ContentStore::count() const { return backend_->ids().size(); }

// ---------------------------------------------------------------------------
// Errors and summary
// ---------------------------------------------------------------------------

const char* workspace_error_str(WorkspaceError e) {
  switch (e) {
    case WorkspaceError::Ok:
      return "ok";
    case WorkspaceError::NotFound:
      return "not-found";
    case WorkspaceError::Exists:
      return "exists";
    case WorkspaceError::StorageFailure:
      return "storage-failure";
    case WorkspaceError::IoError:
      return "io-error";
    case WorkspaceError::BadName:
      return "bad-name";
  }
  return "unknown";
}

namespace {

namespace fs = std::filesystem;

RecordingSummary summarize(const uint8_t* data, size_t len) {
  RecordingSummary s;
  FrameStream stream(data, len);
  Frame f;
  while (stream.next(f)) {
    ++s.total_frames;
    ++s.type_counts[f.header.type];
  }
  s.terminal_status = stream.status();
  return s;
}

// A recording name must be non-empty and free of characters that would break
// the tab-separated catalog line or the filesystem layout.
bool valid_name(const std::string& name) {
  if (name.empty()) return false;
  for (const unsigned char c : name) {
    if (c < 0x20) return false;  // control characters, tab, newline, CR
    if (c == '/' || c == '\\') return false;
  }
  return true;
}

bool parse_u64(const std::string& s, uint64_t& out) {
  if (s.empty()) return false;
  const char* begin = s.data();
  const char* end = begin + s.size();
  const auto r = std::from_chars(begin, end, out);
  return r.ec == std::errc() && r.ptr == end;
}

int hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string encode_counts(const std::map<RecordType, uint64_t>& counts) {
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  bool first = true;
  for (const auto& kv : counts) {
    if (!first) out += ',';
    first = false;
    const uint8_t t = static_cast<uint8_t>(kv.first);
    out += kHex[(t >> 4) & 0x0F];
    out += kHex[t & 0x0F];
    out += ':';
    out += std::to_string(kv.second);
  }
  return out;
}

std::map<RecordType, uint64_t> decode_counts(const std::string& field) {
  std::map<RecordType, uint64_t> counts;
  size_t pos = 0;
  while (pos < field.size()) {
    const size_t comma = field.find(',', pos);
    const std::string token =
        field.substr(pos, comma == std::string::npos ? std::string::npos
                                                      : comma - pos);
    pos = (comma == std::string::npos) ? field.size() : comma + 1;

    const size_t colon = token.find(':');
    if (colon != 2) continue;  // type is exactly two hex digits
    const int hi = hex_nibble(token[0]);
    const int lo = hex_nibble(token[1]);
    if (hi < 0 || lo < 0) continue;
    uint64_t n = 0;
    if (!parse_u64(token.substr(colon + 1), n)) continue;
    counts[static_cast<RecordType>((hi << 4) | lo)] = n;
  }
  return counts;
}

std::vector<std::string> split_tabs(const std::string& line) {
  std::vector<std::string> fields;
  size_t pos = 0;
  for (;;) {
    const size_t tab = line.find('\t', pos);
    if (tab == std::string::npos) {
      fields.push_back(line.substr(pos));
      break;
    }
    fields.push_back(line.substr(pos, tab - pos));
    pos = tab + 1;
  }
  return fields;
}

std::string encode_entry(const RecordingInfo& info) {
  std::string line;
  line += info.name;
  line += '\t';
  line += info.content_id;
  line += '\t';
  line += std::to_string(info.byte_size);
  line += '\t';
  line += std::to_string(info.summary.total_frames);
  line += '\t';
  line += std::to_string(static_cast<uint64_t>(
      static_cast<uint8_t>(info.summary.terminal_status)));
  line += '\t';
  line += encode_counts(info.summary.type_counts);
  return line;
}

bool decode_entry(const std::string& line, RecordingInfo& info) {
  const std::vector<std::string> f = split_tabs(line);
  if (f.size() < 6) return false;
  if (f[0].empty()) return false;

  uint64_t byte_size = 0;
  uint64_t total_frames = 0;
  uint64_t status_raw = 0;
  if (!parse_u64(f[2], byte_size)) return false;
  if (!parse_u64(f[3], total_frames)) return false;
  if (!parse_u64(f[4], status_raw)) return false;

  info.name = f[0];
  info.content_id = f[1];
  info.byte_size = byte_size;
  info.summary.total_frames = total_frames;
  info.summary.terminal_status =
      status_raw <= static_cast<uint64_t>(FrameStatus::PayloadTooLong)
          ? static_cast<FrameStatus>(status_raw)
          : FrameStatus::Ok;
  info.summary.type_counts = decode_counts(f[5]);
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Workspace
// ---------------------------------------------------------------------------

Workspace::Workspace(std::shared_ptr<StorageBackend> backend,
                     std::string catalog_path)
    : store_(std::move(backend)), catalog_path_(std::move(catalog_path)) {
  load();
}

WorkspaceError Workspace::import(const std::string& name, const uint8_t* data,
                                 size_t len) {
  if (!valid_name(name)) return WorkspaceError::BadName;
  if (catalog_.find(name) != catalog_.end()) return WorkspaceError::Exists;

  const std::string id = store_.put(data, len);
  if (id.empty()) return WorkspaceError::StorageFailure;

  RecordingInfo info;
  info.name = name;
  info.content_id = id;
  info.byte_size = len;
  info.summary = summarize(data, len);

  catalog_[name] = info;
  if (!save()) {
    catalog_.erase(name);  // keep the in-memory catalog matching disk
    return WorkspaceError::IoError;
  }
  return WorkspaceError::Ok;
}

WorkspaceError Workspace::open(const std::string& name,
                               std::vector<uint8_t>& out) const {
  const auto it = catalog_.find(name);
  if (it == catalog_.end()) return WorkspaceError::NotFound;
  if (!store_.get(it->second.content_id, out)) {
    return WorkspaceError::StorageFailure;
  }
  return WorkspaceError::Ok;
}

WorkspaceError Workspace::stat(const std::string& name,
                               RecordingInfo& out) const {
  const auto it = catalog_.find(name);
  if (it == catalog_.end()) return WorkspaceError::NotFound;
  out = it->second;
  return WorkspaceError::Ok;
}

WorkspaceError Workspace::remove(const std::string& name) {
  const auto it = catalog_.find(name);
  if (it == catalog_.end()) return WorkspaceError::NotFound;

  RecordingInfo saved = std::move(it->second);
  catalog_.erase(it);
  if (!save()) {
    catalog_[name] = std::move(saved);  // roll back to match disk
    return WorkspaceError::IoError;
  }
  return WorkspaceError::Ok;
}

std::vector<RecordingInfo> Workspace::list() const {
  std::vector<RecordingInfo> out;
  out.reserve(catalog_.size());
  for (const auto& kv : catalog_) out.push_back(kv.second);
  return out;
}

bool Workspace::has(const std::string& name) const {
  return catalog_.find(name) != catalog_.end();
}

bool Workspace::save() const {
  if (catalog_path_.empty()) return true;  // in-memory catalog, nothing to do

  std::string content;
  for (const auto& kv : catalog_) {
    content += encode_entry(kv.second);
    content += '\n';
  }

  std::error_code ec;
  const fs::path target(catalog_path_);
  const fs::path parent = target.parent_path();
  if (!parent.empty()) {
    fs::create_directories(parent, ec);
    if (ec) return false;
  }

  const fs::path tmp = target.string() + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (!content.empty()) {
      f.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    f.flush();
    if (!f) {
      f.close();
      fs::remove(tmp, ec);
      return false;
    }
  }

  fs::rename(tmp, target, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return false;
  }
  return true;
}

void Workspace::load() {
  if (catalog_path_.empty()) return;

  std::ifstream f(catalog_path_, std::ios::binary);
  if (!f) return;  // no catalog persisted yet

  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    RecordingInfo info;
    if (decode_entry(line, info)) catalog_[info.name] = std::move(info);
  }
}

}  // namespace blackspan
