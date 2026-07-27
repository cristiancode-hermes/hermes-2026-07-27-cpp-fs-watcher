#include "fswatch/watcher.hpp"

#include <unistd.h>
#include <sys/inotify.h>
#include <poll.h>
#include <cstring>
#include <cerrno>
#include <iostream>
#include <map>
#include <format>
#include <print>

namespace fswatch {

// ── Inotify constants ──
constexpr size_t EVENT_BUF_SIZE = 4096 * (sizeof(inotify_event) + 16);

// ── PIMPL implementation ──
struct FileWatcher::Impl {
  int inotify_fd = -1;
  std::map<int, std::string> wd_to_path;  // watch descriptor → path
  std::map<std::string, int> path_to_wd;  // path → watch descriptor
};

// ── Mask helpers ──
uint32_t FileWatcher::default_mask() {
  return IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB;
}

uint32_t FileWatcher::all_events_mask() {
  return IN_ALL_EVENTS;
}

// ── Constructor / Destructor ──
FileWatcher::FileWatcher()
  : impl_(std::make_unique<Impl>())
{
  impl_->inotify_fd = inotify_init1(IN_NONBLOCK);
  if (impl_->inotify_fd == -1) {
    throw std::system_error(errno, std::generic_category(), "inotify_init1");
  }
}

FileWatcher::~FileWatcher() {
  if (impl_ && impl_->inotify_fd >= 0) {
    // Inotify watches are automatically cleaned up on fd close
    ::close(impl_->inotify_fd);
  }
}

FileWatcher::FileWatcher(FileWatcher&&) noexcept = default;
FileWatcher& FileWatcher::operator=(FileWatcher&&) noexcept = default;

// ── add_watch ──
Result<bool> FileWatcher::add_watch(const std::string& path, uint32_t mask) {
  if (impl_->path_to_wd.contains(path)) {
    return std::unexpected(Error{0, "Path already being watched: " + path});
  }

  int wd = inotify_add_watch(impl_->inotify_fd, path.c_str(), mask);
  if (wd == -1) {
    return std::unexpected(Error{errno, "Failed to watch " + path + ": " +
                                 std::strerror(errno)});
  }

  impl_->wd_to_path[wd] = path;
  impl_->path_to_wd[path] = wd;
  return true;
}

// ── remove_watch ──
void FileWatcher::remove_watch(const std::string& path) {
  auto it = impl_->path_to_wd.find(path);
  if (it == impl_->path_to_wd.end()) return;

  int wd = it->second;
  inotify_rm_watch(impl_->inotify_fd, wd);
  impl_->wd_to_path.erase(wd);
  impl_->path_to_wd.erase(it);
}

// ── watched_paths ──
std::vector<std::string> FileWatcher::watched_paths() const {
  std::vector<std::string> paths;
  for (const auto& [_, p] : impl_->wd_to_path) {
    paths.push_back(p);
  }
  return paths;
}

// ── poll (single non-blocking read) ──
Result<int> FileWatcher::poll() {
  if (!impl_ || impl_->inotify_fd < 0) {
    return std::unexpected(Error{1, "Watcher not initialized"});
  }

  char buf[EVENT_BUF_SIZE];
  ssize_t len = ::read(impl_->inotify_fd, buf, sizeof(buf));
  if (len == -1) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;  // No events
    return std::unexpected(Error{errno, "read: " + std::string(std::strerror(errno))});
  }

  int count = 0;
  size_t pos = 0;
  while (pos < static_cast<size_t>(len)) {
    auto* event = reinterpret_cast<struct inotify_event*>(&buf[pos]);

    Event ev;
    ev.timestamp = std::chrono::system_clock::now();

    // Determine event type
    if (event->mask & IN_ACCESS)      ev.type = EventType::Access;
    else if (event->mask & IN_MODIFY) ev.type = EventType::Modify;
    else if (event->mask & IN_ATTRIB) ev.type = EventType::Attrib;
    else if (event->mask & IN_CLOSE_WRITE)  ev.type = EventType::CloseWrite;
    else if (event->mask & IN_CLOSE_NOWRITE) ev.type = EventType::CloseNoWrite;
    else if (event->mask & IN_OPEN)   ev.type = EventType::Open;
    else if (event->mask & IN_MOVED_FROM)   ev.type = EventType::MovedFrom;
    else if (event->mask & IN_MOVED_TO)     ev.type = EventType::MovedTo;
    else if (event->mask & IN_CREATE) ev.type = EventType::Create;
    else if (event->mask & IN_DELETE) ev.type = EventType::Delete;
    else if (event->mask & IN_DELETE_SELF)  ev.type = EventType::DeleteSelf;
    else if (event->mask & IN_MOVE_SELF)    ev.type = EventType::MoveSelf;
    else if (event->mask & IN_UNMOUNT)      ev.type = EventType::Unmount;
    else if (event->mask & IN_Q_OVERFLOW)   ev.type = EventType::QOverflow;
    else if (event->mask & IN_IGNORED)      ev.type = EventType::Ignored;
    else ev.type = EventType::Modify;  // fallback

    // Build path
    auto dir_it = impl_->wd_to_path.find(event->wd);
    std::string full_path;
    if (dir_it != impl_->wd_to_path.end()) {
      full_path = dir_it->second;
      if (event->len > 0 && event->name[0] != '\0') {
        full_path += '/';
        full_path += event->name;
      }
    } else {
      full_path = "unknown";
    }

    ev.path = std::move(full_path);
    ev.cookie = event->cookie;

    // Callback
    if (callback_) {
      callback_(ev);
    }

    pos += sizeof(inotify_event) + event->len;
    ++count;
  }

  return count;
}

// ── run (blocking event loop) ──
Result<int> FileWatcher::run() {
  if (!impl_ || impl_->inotify_fd < 0) {
    return std::unexpected(Error{1, "Watcher not initialized"});
  }

  running_ = true;

  struct pollfd pfd;
  pfd.fd = impl_->inotify_fd;
  pfd.events = POLLIN;

  int total = 0;

  while (running_) {
    int ret = ::poll(&pfd, 1, 500);  // 500ms timeout for responsiveness
    if (ret == -1) {
      if (errno == EINTR) continue;
      return std::unexpected(Error{errno, "poll: " + std::string(std::strerror(errno))});
    }

    if (ret > 0 && (pfd.revents & POLLIN)) {
      auto result = poll();
      if (!result) return result;
      total += *result;
    }
  }

  return total;
}

// ── Event name ──
std::string_view event_type_name(EventType t) {
  using namespace std::string_view_literals;
  switch (t) {
    case EventType::Access:      return "ACCESS"sv;
    case EventType::Modify:      return "MODIFY"sv;
    case EventType::Attrib:      return "ATTRIB"sv;
    case EventType::CloseWrite:  return "CLOSE_WRITE"sv;
    case EventType::CloseNoWrite: return "CLOSE_NOWRITE"sv;
    case EventType::Open:        return "OPEN"sv;
    case EventType::MovedFrom:   return "MOVED_FROM"sv;
    case EventType::MovedTo:     return "MOVED_TO"sv;
    case EventType::Create:      return "CREATE"sv;
    case EventType::Delete:      return "DELETE"sv;
    case EventType::DeleteSelf:  return "DELETE_SELF"sv;
    case EventType::MoveSelf:    return "MOVE_SELF"sv;
    case EventType::Unmount:     return "UNMOUNT"sv;
    case EventType::QOverflow:   return "Q_OVERFLOW"sv;
    case EventType::Ignored:     return "IGNORED"sv;
    case EventType::IsDir:       return "ISDIR"sv;
  }
  return "UNKNOWN"sv;
}

// ── Format event ──
std::string format_event(const Event& e) {
  auto tt = std::chrono::system_clock::to_time_t(e.timestamp);
  auto tm = *std::localtime(&tt);
  char time_buf[20];
  std::strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &tm);

  return std::format("[{}] {:12} {}", time_buf, event_type_name(e.type), e.path);
}

}  // namespace fswatch
