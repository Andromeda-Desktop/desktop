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

// Model test: random file operations against four watches, one of them on a symlink that is made, retargeted,
// removed and unwatched; after each operation every watch whose trigger the
// operation matches was called, no other was, and no watch was called twice in one loop turn. The watched directory
// and its parent are also deleted and made again: a watch made while the directory is missing still fires.
namespace {
  int g_failures = 0;

  void expect(bool ok, const std::string& what) {
    if (!ok) {
      std::println(stderr, "file_watcher_test: {}", what);
      ++g_failures;
    }
  }

  void write(const fs::path& path) { std::ofstream(path) << "x"; }

  enum class W : std::uint8_t { AModified, AComplete, BModified, LinkModified, Count };

  struct Harness {
    DirectoryWatcher dirs;
    FileWatcher files{dirs};
    fs::path dir;
    fs::path outside;
    fs::path sync;
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
      write(sync / "zz-sentinel");
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
  Harness h{.dir = root / "parent" / "watched", .outside = root / "outside", .sync = root / "sync"};
  fs::create_directories(h.dir);
  fs::create_directories(h.outside);
  fs::create_directories(h.sync);
  h.files.watch(h.sync / "zz-sentinel", [&h] { h.sentinel = true; }, FileWatcher::WatchTrigger::WriteCompleted);
  h.watch("a", FileWatcher::WatchTrigger::Modified, W::AModified);
  h.watch("a", FileWatcher::WatchTrigger::WriteCompleted, W::AComplete);
  auto bId = h.watch("b", FileWatcher::WatchTrigger::Modified, W::BModified);
  expect(bId >= 0, "watch failed");
  // A link in its own directory to a target in another: writes to the target reach the link's watch.
  fs::create_directories(root / "links");
  fs::create_directories(root / "targets");
  write(root / "targets" / "t");
  write(root / "targets" / "u");
  const fs::path link = root / "links" / "l";
  std::string linkTo; // "", "t" or "u"
  const auto watchLink = [&] { return h.files.watch(link, [&h] { h.calls[W::LinkModified].push_back(h.turn); }); };
  auto linkId = watchLink();
  expect(linkId >= 0, "watch link failed");

  std::mt19937 rng(std::random_device{}());
  int held = -1;
  bool dirGone = false;
  const std::vector<W> all{W::AModified, W::AComplete, W::BModified};
  for (int step = 0; step < 400; ++step) {
    h.calls.clear();
    std::vector<W> want;
    std::string op;
    const int pick = held >= 0 || dirGone ? -1 : static_cast<int>(rng() % 15);
    if (dirGone) {
      const auto how = rng() % 4;
      if (how == 0) {
        op = "make the directory and its parent";
        fs::create_directories(h.dir);
      } else if (how == 1) {
        op = "move a directory holding a in";
        fs::create_directories(h.dir.parent_path());
        fs::create_directories(h.outside / "w");
        write(h.outside / "w" / "a");
        fs::rename(h.outside / "w", h.dir);
      } else if (how == 3) {
        op = "make the directory unreadable, then readable one turn later";
        fs::create_directories(h.dir.parent_path());
        fs::create_directory(h.dir);
        fs::permissions(h.dir, fs::perms::none);
        expect(h.settle(), std::format("step {} ({}): sentinel never fired", step, op));
        h.calls.clear();
        fs::permissions(h.dir, fs::perms::owner_all);
      } else {
        op = "make the parent, then the directory one turn later";
        fs::create_directories(h.dir.parent_path());
        expect(h.settle(), std::format("step {} ({}): sentinel never fired", step, op));
        expect(h.calls.empty(), std::format("step {} ({}): a watch fired for the parent alone", step, op));
        fs::create_directory(h.dir);
      }
      dirGone = false;
      want = all;
    } else if (held >= 0) {
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
      if (fs::remove(h.dir / "a")) {
        want = {W::AModified};
      }
    } else if (pick == 5) {
      op = "write b";
      write(h.dir / "b");
      want = {W::BModified};
    } else if (pick == 6) {
      op = "write c";
      write(h.dir / "c");
    } else if (pick == 8) {
      op = "delete the directory";
      fs::remove_all(h.dir);
      dirGone = true;
      want = all;
    } else if (pick == 11 || pick == 12) {
      const std::string target = pick == 11 ? "t" : "u";
      op = std::format("write target {} (link to '{}', watched {})", target, linkTo, linkId >= 0);
      write(root / "targets" / target);
      if (linkId >= 0 && linkTo == target) {
        want = {W::LinkModified};
      }
    } else if (pick == 13) {
      linkTo = linkTo == "t" ? "u" : "t";
      op = std::format("point the link at {}", linkTo);
      fs::create_symlink(root / "targets" / linkTo, root / "links" / "l.tmp");
      fs::rename(root / "links" / "l.tmp", link);
      if (linkId >= 0) {
        want = {W::LinkModified};
      }
    } else if (pick == 14 && !linkTo.empty()) {
      op = "remove the link";
      fs::remove(link);
      linkTo.clear();
      if (linkId >= 0) {
        want = {W::LinkModified};
      }
    } else if (pick == 14) {
      op = linkId >= 0 ? "unwatch the link" : "watch the link again";
      if (linkId >= 0) {
        h.files.unwatch(linkId);
        linkId = -1;
      } else {
        linkId = watchLink();
        expect(linkId >= 0, "watch link failed");
      }
    } else if (pick == 10) {
      op = "move the directory away";
      fs::rename(h.dir, h.outside / std::format("away-{}", step));
      dirGone = true;
      want = all;
    } else if (pick == 9) {
      op = "delete the parent, unwatch b: b is watched again while it is missing";
      fs::remove_all(h.dir.parent_path());
      h.files.unwatch(bId);
      bId = -1;
      dirGone = true;
      want = {W::AModified, W::AComplete};
    } else {
      op = "write b, unwatch b before its call runs";
      write(h.dir / "b");
      h.dirs.processEvents();
      h.files.unwatch(bId);
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
      expect(bId >= 0, std::format("step {}: watch b failed (directory {})", step, dirGone ? "missing" : "present"));
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
