#include "blackspan/workspace.h"

#include <filesystem>
#include <fstream>
#include <system_error>

namespace blackspan {

bool MemoryBackend::put(const std::string& id, const uint8_t* data,
                        size_t len) {
  std::vector<uint8_t> bytes;
  if (len > 0 && data != nullptr) bytes.assign(data, data + len);
  objects_[id] = std::move(bytes);
  return true;
}

bool MemoryBackend::get(const std::string& id,
                        std::vector<uint8_t>& out) const {
  const auto it = objects_.find(id);
  if (it == objects_.end()) return false;
  out = it->second;
  return true;
}

bool MemoryBackend::has(const std::string& id) const {
  return objects_.find(id) != objects_.end();
}

std::vector<std::string> MemoryBackend::ids() const {
  std::vector<std::string> out;
  out.reserve(objects_.size());
  for (const auto& kv : objects_) out.push_back(kv.first);
  return out;
}

namespace {

namespace fs = std::filesystem;

bool is_storable_id(const std::string& id) {
  if (id.size() < 3) return false;
  for (const char c : id) {
    if (c == '/' || c == '\\' || c == '\0') return false;
  }
  return true;
}

}

FileBackend::FileBackend(std::string root) : root_(std::move(root)) {
  std::error_code ec;
  fs::create_directories(fs::path(root_) / "objects", ec);
  fs::create_directories(fs::path(root_) / "tmp", ec);
}

bool FileBackend::put(const std::string& id, const uint8_t* data, size_t len) {
  if (!is_storable_id(id)) return false;

  const fs::path object =
      fs::path(root_) / "objects" / id.substr(0, 2) / id.substr(2);

  std::error_code ec;
  fs::create_directories(object.parent_path(), ec);
  if (ec) return false;

  static uint64_t counter = 0;
  const fs::path tmp =
      fs::path(root_) / "tmp" / (id + "." + std::to_string(counter++) + ".tmp");

  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (len > 0 && data != nullptr) {
      f.write(reinterpret_cast<const char*>(data),
              static_cast<std::streamsize>(len));
    }
    f.flush();
    if (!f) {
      f.close();
      fs::remove(tmp, ec);
      return false;
    }
  }

  fs::rename(tmp, object, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return false;
  }
  return true;
}

bool FileBackend::get(const std::string& id, std::vector<uint8_t>& out) const {
  if (!is_storable_id(id)) return false;

  const fs::path object =
      fs::path(root_) / "objects" / id.substr(0, 2) / id.substr(2);

  std::error_code ec;
  if (!fs::is_regular_file(object, ec)) return false;

  std::ifstream f(object, std::ios::binary | std::ios::ate);
  if (!f) return false;
  const std::streampos end = f.tellg();
  if (end < 0) return false;
  f.seekg(0, std::ios::beg);

  out.assign(static_cast<size_t>(end), 0);
  if (end > 0) {
    f.read(reinterpret_cast<char*>(out.data()), end);
    if (!f) {
      out.clear();
      return false;
    }
  }
  return true;
}

bool FileBackend::has(const std::string& id) const {
  if (!is_storable_id(id)) return false;
  const fs::path object =
      fs::path(root_) / "objects" / id.substr(0, 2) / id.substr(2);
  std::error_code ec;
  return fs::is_regular_file(object, ec);
}

std::vector<std::string> FileBackend::ids() const {
  std::vector<std::string> out;
  std::error_code ec;

  const fs::path objdir = fs::path(root_) / "objects";
  fs::directory_iterator bucket(objdir, ec), bucket_end;
  if (ec) return out;

  for (; bucket != bucket_end; bucket.increment(ec)) {
    if (ec) break;
    std::error_code bec;
    if (!bucket->is_directory(bec) || bec) continue;
    const std::string prefix = bucket->path().filename().string();

    std::error_code fec;
    fs::directory_iterator file(bucket->path(), fec), file_end;
    if (fec) continue;
    for (; file != file_end; file.increment(fec)) {
      if (fec) break;
      std::error_code rec;
      if (!file->is_regular_file(rec) || rec) continue;
      out.push_back(prefix + file->path().filename().string());
    }
  }
  return out;
}

}
