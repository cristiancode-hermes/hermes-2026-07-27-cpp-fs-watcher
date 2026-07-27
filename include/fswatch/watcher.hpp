#pragma once

#include <chrono>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fswatch {

// ── Event types ──
enum class EventType {
  Access,
  Modify,
  Attrib,
  CloseWrite,
  CloseNoWrite,
  Open,
  MovedFrom,
  MovedTo,
  Create,
  Delete,
  DeleteSelf,
  MoveSelf,
  Unmount,
  QOverflow,
  Ignored,
  IsDir,
};

// ── File system event ──
struct Event {
  EventType type;
  std::string path;
  std::chrono::system_clock::time_point timestamp;
  uint32_t cookie;  // For rename pairing
};

// ── Error ──
struct Error {
  int code;
  std::string message;
};

template <typename T>
using Result = std::expected<T, Error>;

// ── File watcher ──
class FileWatcher {
public:
  using Callback = std::function<void(const Event&)>;

  explicit FileWatcher();
  ~FileWatcher();

  FileWatcher(const FileWatcher&) = delete;
  FileWatcher& operator=(const FileWatcher&) = delete;
  FileWatcher(FileWatcher&&) noexcept;
  FileWatcher& operator=(FileWatcher&&) noexcept;

  /// Add a directory to watch. Returns true on success.
  [[nodiscard]] Result<bool> add_watch(const std::string& path,
                                        uint32_t mask = default_mask());

  /// Remove a directory watch.
  void remove_watch(const std::string& path);

  /// Set the event callback.
  void set_callback(Callback cb) { callback_ = std::move(cb); }

  /// Run a single poll (non-blocking). Returns number of events processed.
  Result<int> poll();

  /// Run the event loop (blocking). Returns when stop() is called or on error.
  Result<int> run();

  /// Stop the event loop (safe to call from signal handler).
  void stop() { running_ = false; }

  /// Get list of watched paths.
  std::vector<std::string> watched_paths() const;

  /// Default watch mask (create, delete, modify, move, attrib)
  static uint32_t default_mask();
  static uint32_t all_events_mask();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  bool running_ = false;
  Callback callback_;
};

// ── Utility functions ──
std::string_view event_type_name(EventType t);
std::string format_event(const Event& e);
Result<std::vector<Event>> read_events(int fd, int wd);

}  // namespace fswatch
