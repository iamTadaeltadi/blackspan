#ifndef BLACKSPAN_WORKSPACE_H
#define BLACKSPAN_WORKSPACE_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "blackspan/frame.h"

namespace blackspan {

class StorageBackend {
 public:
  virtual ~StorageBackend() = default;

  virtual bool put(const std::string& id, const uint8_t* data, size_t len) = 0;
  virtual bool get(const std::string& id, std::vector<uint8_t>& out) const = 0;
  virtual bool has(const std::string& id) const = 0;
  virtual std::vector<std::string> ids() const = 0;
};

class MemoryBackend : public StorageBackend {
 public:
  bool put(const std::string& id, const uint8_t* data, size_t len) override;
  bool get(const std::string& id, std::vector<uint8_t>& out) const override;
  bool has(const std::string& id) const override;
  std::vector<std::string> ids() const override;

 private:
  std::map<std::string, std::vector<uint8_t>> objects_;
};

class FileBackend : public StorageBackend {
 public:
  explicit FileBackend(std::string root);

  bool put(const std::string& id, const uint8_t* data, size_t len) override;
  bool get(const std::string& id, std::vector<uint8_t>& out) const override;
  bool has(const std::string& id) const override;
  std::vector<std::string> ids() const override;

  const std::string& root() const { return root_; }

 private:
  std::string root_;
};

class ContentStore {
 public:
  explicit ContentStore(std::shared_ptr<StorageBackend> backend);

  std::string put(const uint8_t* data, size_t len);

  bool get(const std::string& id, std::vector<uint8_t>& out) const;
  bool has(const std::string& id) const;

  size_t count() const;

  StorageBackend* backend() { return backend_.get(); }
  const StorageBackend* backend() const { return backend_.get(); }

 private:
  std::shared_ptr<StorageBackend> backend_;
};

enum class WorkspaceError {
  Ok,
  NotFound,
  Exists,
  StorageFailure,
  IoError,
  BadName,
};

const char* workspace_error_str(WorkspaceError e);

struct RecordingSummary {
  uint64_t total_frames = 0;
  std::map<RecordType, uint64_t> type_counts;
  FrameStatus terminal_status = FrameStatus::Ok;
};

struct RecordingInfo {
  std::string name;
  std::string content_id;
  uint64_t byte_size = 0;
  RecordingSummary summary;
};

class Workspace {
 public:

  explicit Workspace(std::shared_ptr<StorageBackend> backend,
                     std::string catalog_path = std::string());

  Workspace(const Workspace&) = delete;
  Workspace& operator=(const Workspace&) = delete;
  Workspace(Workspace&&) = default;
  Workspace& operator=(Workspace&&) = default;

  WorkspaceError import(const std::string& name, const uint8_t* data,
                        size_t len);
  WorkspaceError open(const std::string& name,
                      std::vector<uint8_t>& out) const;
  WorkspaceError stat(const std::string& name, RecordingInfo& out) const;
  WorkspaceError remove(const std::string& name);

  std::vector<RecordingInfo> list() const;

  bool has(const std::string& name) const;
  size_t size() const { return catalog_.size(); }

  size_t object_count() const { return store_.count(); }

 private:
  bool save() const;
  void load();

  ContentStore store_;
  std::string catalog_path_;
  std::map<std::string, RecordingInfo> catalog_;
};

}

#endif
