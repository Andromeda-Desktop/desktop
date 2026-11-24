#include "core/deferred_call.h"
#include "core/files/directory_watcher.h"
#include "core/files/file_watcher.h"

#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <algorithm>
#include <format>
#include <fstream>
#include <map>
#include <poll.h>
#include <print>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

// Model test: random file operations against three watches; after each operation every watch whose trigger the
// operation matches was called, no other was, and no watch was called twice in one loop turn.
namespace {
  int g_failures = 0;

  void expect(bool ok, const std::string& what) {
    if (!ok) {
      std::println(stderr, "file_watcher_test: {}", what);
      ++g_failures;
    }
  }

  void write(const fs::path& path) { std::ofstream(path) << "x"; }

  enum class W : std::uint8_t { AModified, AComplete, BModified, Count };

  struct Harness {
    DirectoryWatcher dirs;
    FileWatcher files{dirs};
    fs::path dir;
    fs::path outside;
    int turn = 0;
    std::map<W, std::vector<int>> calls;
    bool sentinel = false;

    FileWatcher::WatchId watch(const char* name, FileWatcher::WatchTrigger trigger, W w) {
      return files.watch(dir / name, [this, w] { calls[w].push_back(turn); }, trigger);
    }

    // Runs loop turns until the sentinel file's watch fires: inotify keeps order, so every earlier event and its
    // queued call has run by then.
    bool settle() {
      sentinel = false;
      write(dir / "zz-sentinel");
      const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      while (!sentinel) {
        if (std::chrono::steady_clock::now() > until) {
          return false;
        }
        ++turn;
        std::vector<pollfd> fds{{.fd = DeferredCall::wakeFd(), .events = POLLIN, .revents = 0}};
        const std::size_t start = dirs.addPollFds(fds);
        ::poll(fds.data(), fds.size(), std::max(dirs.pollTimeoutMs(), 10));
        DeferredCall::drainWakeFd();
        for (auto& fn : DeferredCall::takePending()) {
          fn();
        }
        dirs.dispatch(fds, start);
      }
      return true;
    }
  };
} // namespace

int main() {
  const auto root = fs::temp_directory_path() / std::format("file_watcher_test.{}", ::getpid());
  Harness h{.dir = root / "watched", .outside = root / "outside"};
  fs::create_directories(h.dir);
  fs::create_directories(h.outside);
  h.files.watch(h.dir / "zz-sentinel", [&h] { h.sentinel = true; }, FileWatcher::WatchTrigger::WriteCompleted);
  h.watch("a", FileWatcher::WatchTrigger::Modified, W::AModified);
  h.watch("a", FileWatcher::WatchTrigger::WriteCompleted, W::AComplete);
  auto bId = h.watch("b", FileWatcher::WatchTrigger::Modified, W::BModified);
  expect(bId >= 0, "watch failed");

  std::mt19937 rng(std::random_device{}());
  int held = -1;
  for (int step = 0; step < 300; ++step) {
    h.calls.clear();
    std::vector<W> want;
    std::string op;
    const int pick = held >= 0 ? -1 : static_cast<int>(rng() % 8);
    if (held >= 0) {
      op = "close held a";
      ::close(held);
      held = -1;
      want = {W::AModified, W::AComplete};
    } else if (pick == 0) {
      op = "write a";
      write(h.dir / "a");
      want = {W::AModified, W::AComplete};
    } else if (pick == 1) {
      op = "open a and write, still open";
      held = ::open((h.dir / "a").c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
      expect(::write(held, "y", 1) == 1, "write to held a");
      want = {W::AModified};
    } else if (pick == 2) {
      op = "rename tmp onto a";
      write(h.dir / "a.tmp");
      fs::rename(h.dir / "a.tmp", h.dir / "a");
      want = {W::AModified, W::AComplete};
    } else if (pick == 3) {
      op = "move a in from outside";
      write(h.outside / "a");
      fs::rename(h.outside / "a", h.dir / "a");
      want = {W::AModified, W::AComplete};
    } else if (pick == 4) {
      op = "delete a";
      fs::remove(h.dir / "a");
    } else if (pick == 5) {
      op = "write b";
      write(h.dir / "b");
      want = {W::BModified};
    } else if (pick == 6) {
      op = "write c";
      write(h.dir / "c");
    } else {
      op = "write b, unwatch b before its call runs";
      write(h.dir / "b");
      h.dirs.processEvents();
      h.dirs.unwatch(bId);
      bId = -1;
    }
    expect(h.settle(), std::format("step {} ({}): sentinel never fired", step, op));
    for (int i = 0; i < static_cast<int>(W::Count); ++i) {
      const auto w = static_cast<W>(i);
      const auto& got = h.calls[w];
      const bool wanted = std::ranges::contains(want, w);
      expect(wanted == !got.empty(), std::format("step {} ({}): watch {} called {} times, want {}", step, op, i,
                                                 got.size(), wanted ? ">0" : "0"));
      for (std::size_t k = 1; k < got.size(); ++k) {
        expect(got[k] != got[k - 1], std::format("step {} ({}): watch {} called twice in turn {}", step, op, i, got[k]));
      }
    }
    if (bId < 0) {
      bId = h.watch("b", FileWatcher::WatchTrigger::Modified, W::BModified);
    }
    if (g_failures > 10) {
      break;
    }
  }
  if (held >= 0) {
    ::close(held);
  }
  fs::remove_all(root);
  if (g_failures == 0) {
    std::println("file_watcher_test: ok");
  }
  return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
