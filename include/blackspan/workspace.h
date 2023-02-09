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

// Abstract object store keyed by an opaque string id. An implementation
// persists a byte blob under an id and reads it back later. Every operation
// reports failure as a boolean; a backend must never throw and must never
// crash on a missing id or an underlying IO error.
class StorageBackend {
 public:
  virtual ~StorageBackend() = default;

  virtual bool put(const std::string& id, const uint8_t* data, size_t len) = 0;
  virtual bool get(const std::string& id, std::vector<uint8_t>& out) const = 0;
  virtual bool has(const std::string& id) const = 0;
  virtual std::vector<std::string> ids() const = 0;
};

// In-memory object store. Objects live only for the lifetime of the backend.
class MemoryBackend : public StorageBackend {
 public:
  bool put(const std::string& id, const uint8_t* data, size_t len) override;
  bool get(const std::string& id, std::vector<uint8_t>& out) const override;
  bool has(const std::string& id) const override;
  std::vector<std::string> ids() const override;

 private:
  std::map<std::string, std::vector<uint8_t>> objects_;
};

// Filesystem-backed object store. Each object is written under
// <root>/objects/<first two id chars>/<remaining id chars>, so a large store
// fans out across directories rather than piling into one. Writes are atomic:
// bytes go to a temporary file under <root>/tmp and are renamed into place, so
// a reader never observes a half-written object. Ids used for the layout must
// be at least three characters and contain no path separators; other ids are
// rejected (put returns false, get/has report absence).
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

// Content-addressed store layered over a StorageBackend. An object's id is the
// lowercase-hex SHA-256 of its bytes, so identical content is stored exactly
// once (put is idempotent) and any id both names and verifies its content.
class ContentStore {
 public:
  explicit ContentStore(std::shared_ptr<StorageBackend> backend);

  // Stores data under its content id and returns that id. If the id is already
  // present, the existing object is kept and no write occurs. Returns an empty
  // string only when the underlying backend write fails.
  std::string put(const uint8_t* data, size_t len);

  bool get(const std::string& id, std::vector<uint8_t>& out) const;
  bool has(const std::string& id) const;

  // Number of distinct objects currently stored.
  size_t count() const;

  StorageBackend* backend() { return backend_.get(); }
  const StorageBackend* backend() const { return backend_.get(); }

 private:
  std::shared_ptr<StorageBackend> backend_;
};

enum class WorkspaceError {
  Ok,
  NotFound,        // no catalog entry with that name
  Exists,          // import into a name that is already in use
  StorageFailure,  // the object backend failed to store or fetch bytes
  IoError,         // the catalog could not be persisted
  BadName,         // empty name or name with control / separator characters
};

const char* workspace_error_str(WorkspaceError e);

// A light frame-level summary of a recording, produced by decoding its frame
// stream. terminal_status is the stream status after the last decodable frame:
// Ok for a cleanly framed recording, otherwise the first error encountered.
struct RecordingSummary {
  uint64_t total_frames = 0;
  std::map<RecordType, uint64_t> type_counts;
  FrameStatus terminal_status = FrameStatus::Ok;
};

// A catalog entry: a human recording name plus the content object it names and
// the metadata computed at import time.
struct RecordingInfo {
  std::string name;
  std::string content_id;  // SHA-256 hex of the recording bytes
  uint64_t byte_size = 0;
  RecordingSummary summary;
};

// A named catalog of recordings over a content-addressed backend. Names map to
// content objects, so two names holding identical bytes share a single stored
// object while appearing as two independent catalog entries. When a catalog
// path is supplied the catalog is loaded on construction and rewritten after
// each mutation, so a later Workspace over the same path and backend sees the
// prior imports.
class Workspace {
 public:
  // Wraps a backend. catalog_path, when non-empty, is the file the catalog is
  // loaded from and persisted to; an empty path keeps the catalog in memory.
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

  // Number of distinct stored objects behind the catalog. Two names sharing
  // identical content contribute one object, so this can be less than size().
  size_t object_count() const { return store_.count(); }

 private:
  bool save() const;
  void load();

  ContentStore store_;
  std::string catalog_path_;
  std::map<std::string, RecordingInfo> catalog_;
};

}  // namespace blackspan

#endif  // BLACKSPAN_WORKSPACE_H
