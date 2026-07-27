#include "fswatch/watcher.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <print>
#include <csignal>
#include <chrono>
#include <thread>

using namespace fswatch;

// ── Global flag for signal handler ──
static FileWatcher* g_watcher = nullptr;

extern "C" void handle_signal(int sig) {
  if (g_watcher) {
    std::println(std::cout, "\n⏹  Received signal {}, stopping...", sig);
    g_watcher->stop();
  }
}

// ── Main ──
int main(int argc, char** argv) {
  // ── Parse arguments ──
  bool help = false;
  bool version = false;
  bool json = false;
  bool all_events = false;
  bool recursive = false;
  std::vector<std::string> dirs;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg{argv[i]};
    if (arg == "--help" || arg == "-h") {
      help = true;
    } else if (arg == "--version" || arg == "-V") {
      version = true;
    } else if (arg == "--json" || arg == "-j") {
      json = true;
    } else if (arg == "--all" || arg == "-a") {
      all_events = true;
    } else if (arg == "--recursive" || arg == "-r") {
      recursive = true;
    } else if (arg.starts_with('-')) {
      std::println(std::cerr, "❌ Unknown option: {}", arg);
      return 1;
    } else {
      dirs.emplace_back(arg);
    }
  }

  // ── Help ──
  if (help) {
    std::println(std::cout, R"(
╔══════════════════════════════════════════╗
║  FS Watcher — Linux File System Monitor  ║
╚══════════════════════════════════════════╝

Monitor file system events using Linux inotify.

USAGE:
  fs-watch [OPTIONS] <directory> [<directory> ...]

OPTIONS:
  -h, --help       Show this help message
  -V, --version    Show version
  -j, --json       Output in JSON format
  -a, --all        Watch all event types (default: create, delete, modify, move, attrib)
  -r, --recursive  Watch directories recursively (experimental)

EXAMPLES:
  fs-watch /tmp           Watch /tmp for changes
  fs-watch -a /var/log    Watch /var/log for all events
  fs-watch -j src docs    Watch src/ and docs/ with JSON output
)");
    return 0;
  }

  if (version) {
    std::println(std::cout, "fs-watch v1.0.0 — C++23 Linux inotify file watcher");
    return 0;
  }

  // ── Default to current dir ──
  if (dirs.empty()) {
    dirs.push_back(".");
  }

  // ── Create watcher ──
  try {
    FileWatcher watcher;
    g_watcher = &watcher;

    // ── Signal handlers ──
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    // ── Set callback ──
    if (json) {
      watcher.set_callback([json](const Event& e) {
        auto tt = std::chrono::system_clock::to_time_t(e.timestamp);
        auto tm = *std::localtime(&tt);
        char time_buf[24];
        std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%S", &tm);

        std::println(std::cout,
          R"({{"timestamp":"{}","type":"{}","path":"{}","cookie":{}}})",
          time_buf, event_type_name(e.type), e.path, e.cookie);
      });
    } else {
      watcher.set_callback([](const Event& e) {
        std::cout << format_event(e) << std::endl;
      });
    }

    // ── Add watches ──
    uint32_t mask = all_events ? FileWatcher::all_events_mask()
                               : FileWatcher::default_mask();

    for (const auto& dir : dirs) {
      auto result = watcher.add_watch(dir, mask);
      if (!result) {
        std::println(std::cerr, "❌ {}: {}", dir, result.error().message);
      } else {
        std::println(std::cout, "✓ Watching: {} (mask=0x{:08x})", dir, mask);
      }
    }

    // Check if any watches succeeded
    if (watcher.watched_paths().empty()) {
      std::println(std::cerr, "❌ No directories could be watched");
      return 1;
    }

    // If recursive, attempt to add subdirectories
    if (recursive) {
      for (const auto& dir : dirs) {
        auto* p = popen(("find " + std::string(dir) + " -type d 2>/dev/null").c_str(), "r");
        if (p) {
          char buf[4096];
          while (fgets(buf, sizeof(buf), p)) {
            std::string subdir(buf);
            while (!subdir.empty() && (subdir.back() == '\n' || subdir.back() == '\r'))
              subdir.pop_back();
            if (!subdir.empty() && subdir != dir) {
              auto r = watcher.add_watch(subdir, mask);
              if (r) {
                std::println(std::cout, "  ↳ Watching: {}", subdir);
              }
            }
          }
          pclose(p);
        }
      }
    }

    // ── Start event loop ──
    std::println(std::cout, "▶ Listening for events... (Ctrl+C to stop)");
    auto result = watcher.run();

    if (!result) {
      std::println(std::cerr, "❌ Error: {}", result.error().message);
      return 1;
    }

    std::println(std::cout, "✓ Stopped. Processed {} events.", *result);
    return 0;

  } catch (const std::exception& e) {
    std::println(std::cerr, "❌ Fatal error: {}", e.what());
    return 1;
  }
}
