// The file writer reports every result on the main loop: a replaced write's caller gets the newer write's result, an
// operation runs after the writes queued before it, a failed write reports false, and a directory's first write
// removes only the temporary files no writer holds, and a private write makes private files.

#include "core/deferred_call.h"
#include "protocols/async_file_writer.h"
#include "tests/test_check.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

  template <typename Pred> bool pumpUntil(Pred done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done()) {
      if (std::chrono::steady_clock::now() > deadline) {
        return false;
      }
      DeferredCall::drainWakeFd();
      for (auto& fn : DeferredCall::takePending()) {
        fn();
      }
      std::this_thread::yield();
    }
    return true;
  }

  std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }

  void touch(const std::filesystem::path& path) { std::ofstream(path) << "x"; }

  // Holds the writer thread until release(), so later posts stay queued.
  struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool open = false;
    void release() {
      {
        std::scoped_lock lock(mutex);
        open = true;
      }
      cv.notify_all();
    }
    bool wait() {
      std::unique_lock lock(mutex);
      return cv.wait_for(lock, std::chrono::seconds(5), [this] { return open; });
    }
  };

  void age(const std::filesystem::path& path) {
    std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() - std::chrono::minutes(1));
  }

  // A writer in another process (or PID namespace): it holds the temporary's flock until `release` closes.
  struct OtherWriter {
    pid_t pid = -1;
    int release = -1;
    explicit OtherWriter(const std::filesystem::path& path) {
      int locked[2];
      int hold[2];
      TEST_CHECK(::pipe(locked) == 0 && ::pipe(hold) == 0);
      pid = ::fork();
      if (pid == 0) {
        ::close(hold[1]);
        const int fd = ::open(path.c_str(), O_RDONLY);
        ::flock(fd, LOCK_EX);
        char c = 0;
        [[maybe_unused]] auto w = ::write(locked[1], &c, 1);
        [[maybe_unused]] auto r = ::read(hold[0], &c, 1);
        ::_exit(0);
      }
      ::close(hold[0]);
      ::close(locked[1]);
      char c = 0;
      TEST_CHECK(::read(locked[0], &c, 1) == 1);
      ::close(locked[0]);
      release = hold[1];
    }
    ~OtherWriter() {
      ::close(release);
      ::waitpid(pid, nullptr, 0);
    }
  };

  unsigned mode(const std::filesystem::path& path) {
    struct stat st{};
    ::stat(path.c_str(), &st);
    return st.st_mode & 0777U;
  }

} // namespace

int main() {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / ("async-file-writer-test-" + std::to_string(::getpid()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  // The sweep runs on a directory's first write in this process, so its fixture goes in before any write there.
  const std::filesystem::path sweepDir = dir / "sweep";
  std::filesystem::create_directories(sweepDir);
  // Pid 2 in two names: the sweep must not judge a writer by its pid, which another namespace may reuse.
  const auto dead = sweepDir / "a.json.andro-2-0000000000000001.tmp";
  const auto live = sweepDir / "a.json.andro-2-0000000000000002.tmp";
  const auto fresh = sweepDir / "a.json.andro-3-0000000000000003.tmp";
  const auto foreign = sweepDir / "notes.2.tmp";
  for (const auto& path : {dead, live, fresh, foreign}) {
    touch(path);
  }
  for (const auto& path : {dead, live, foreign}) {
    age(path);
  }
  const OtherWriter liveWriter(live);

  // Replace: two writes to one path queued behind a held operation; both callers get the landed (newer) write.
  Gate gate;
  bool opRan = false;
  andromeda::runFileOpAsync([&gate] { return gate.wait(); }, [&opRan](bool ok) { opRan = ok; });
  const auto target = dir / "state.json";
  std::vector<std::string> results;
  andromeda::writeFileAsync(target, "first", {}, [&](bool ok) { results.push_back(std::string("first:") + (ok ? "ok" : "fail")); });
  andromeda::writeFileAsync(target, "second", {}, [&](bool ok) { results.push_back(std::string("second:") + (ok ? "ok" : "fail")); });
  TEST_CHECK(andromeda::pendingFileText(target) == std::optional<std::string>("second"));
  // An operation queued after the writes sees the file they left.
  std::optional<std::string> seenByOp;
  andromeda::runFileOpAsync([&] {
    seenByOp = readFile(target);
    return true;
  });
  gate.release();
  TEST_CHECK(pumpUntil([&] { return opRan && results.size() == 2; }));
  TEST_CHECK(results == (std::vector<std::string>{"first:ok", "second:ok"}));
  TEST_CHECK(readFile(target) == "second");

  // A write the disk refuses reports false: the parent is a file, so no directory can be made.
  std::optional<bool> failed;
  andromeda::writeFileAsync(target / "child.json", "x", {}, [&failed](bool ok) { failed = ok; });
  TEST_CHECK(pumpUntil([&] { return failed.has_value(); }));
  TEST_CHECK(failed == false);
  TEST_CHECK(seenByOp == std::optional<std::string>("second"));

  // Sweep: the old unlocked temporary goes; a locked one, one just made (its writer locks it next) and a name
  // without our tag stay.
  std::optional<bool> swept;
  andromeda::writeFileAsync(sweepDir / "a.json", "y", {}, [&swept](bool ok) { swept = ok; });
  TEST_CHECK(pumpUntil([&] { return swept.has_value(); }));
  TEST_CHECK(swept == true);
  TEST_CHECK(!std::filesystem::exists(dead));
  TEST_CHECK(std::filesystem::exists(live));
  TEST_CHECK(std::filesystem::exists(fresh));
  TEST_CHECK(std::filesystem::exists(foreign));
  TEST_CHECK(readFile(sweepDir / "a.json") == "y");

  // A private write makes its missing directories 0700 and its file 0600.
  const auto privateFile = dir / "cache" / "programs" / "blob";
  std::optional<bool> wrotePrivate;
  andromeda::writeFileAsync(
      privateFile, "z",
      [](const std::filesystem::path& file, const std::string& text) {
        return andromeda::writeFileReplacing(file, text, 0600, 0700);
      },
      [&wrotePrivate](bool ok) { wrotePrivate = ok; }
  );
  TEST_CHECK(pumpUntil([&] { return wrotePrivate.has_value(); }));
  TEST_CHECK(wrotePrivate == true);
  TEST_CHECK(mode(dir / "cache") == 0700U && mode(dir / "cache" / "programs") == 0700U);
  TEST_CHECK(mode(privateFile) == 0600U);

  std::filesystem::remove_all(dir);
  return 0;
}
