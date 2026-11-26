#include "async_file_writer.h"
#include "crash_capture.h"
#include "log.h"
#include "xdg_dirs.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fcntl.h>
#include <format>
#include <random>
#include <sys/file.h>
#include <sys/stat.h>
#include <mutex>
#include <pthread.h>
#include <set>
#include <stop_token>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace andromeda {

  namespace {

    constexpr Logger kLog("file-writer");

    // A whole-file write (path set) or an ordered operation (op set, path empty).
    struct Job {
      std::filesystem::path path;
      std::string text;
      WriteFn write;
      std::function<bool()> op;
      std::vector<WriteDone> dones;
    };

    constexpr std::string_view kTempTag = ".andro-";
    constexpr std::string_view kTempSuffix = ".tmp";
    // An unlocked temporary this old has no writer: the writer locks it right after it creates it.
    constexpr auto kDeadTemporaryAge = std::chrono::seconds(10);

    // Removes `<name>.andro-*.tmp` files no writer holds (a crash between write and rename). The flock test also
    // holds for writers in another PID namespace or on another host sharing the home directory.
    void sweepDeadTemporaries(const std::filesystem::path& dir) {
      std::error_code error;
      for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
        const std::string name = entry.path().filename().string();
        if (!name.ends_with(kTempSuffix) || name.find(kTempTag) == std::string::npos) {
          continue;
        }
        std::error_code ignored;
        const auto written = std::filesystem::last_write_time(entry.path(), ignored);
        if (ignored || std::filesystem::file_time_type::clock::now() - written < kDeadTemporaryAge) {
          continue;
        }
        const int fd = ::open(entry.path().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) {
          continue;
        }
        if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
          std::filesystem::remove(entry.path(), ignored);
        }
        ::close(fd);
      }
    }

    // Unique per process across PID namespaces: two sandboxes may both run as pid 2.
    const std::string& temporaryTag() {
      static const auto* tag = new std::string([] {
        std::random_device random;
        return std::format("{}{}-{:08x}{:08x}", kTempTag, ::getpid(), random(), random());
      }());
      return *tag;
    }

    bool makeDirectories(const std::filesystem::path& dir, unsigned mode) {
      std::error_code error;
      if (dir.empty() || std::filesystem::is_directory(dir, error)) {
        return true;
      }
      if (!makeDirectories(dir.parent_path(), mode)) {
        return false;
      }
      return ::mkdir(dir.c_str(), mode) == 0 || errno == EEXIST;
    }

    bool writeAll(int fd, std::string_view text) {
      std::size_t done = 0;
      while (done < text.size()) {
        const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR) {
          continue;
        }
        if (n <= 0) {
          return false;
        }
        done += static_cast<std::size_t>(n);
      }
      return true;
    }

    class Writer {
    public:
      void post(Job job) {
        std::scoped_lock lock(m_mutex);
        if (!job.path.empty()) {
          for (auto& pending : m_queue) {
            if (pending.path == job.path) {
              job.dones.insert(
                  job.dones.begin(), std::make_move_iterator(pending.dones.begin()),
                  std::make_move_iterator(pending.dones.end())
              );
              pending = std::move(job);
              return;
            }
          }
        }
        m_queue.push_back(std::move(job));
        if (!m_thread.joinable()) {
          m_thread = std::jthread([this](const std::stop_token& stop) { run(stop); });
        }
        m_wake.notify_one();
      }

      std::optional<std::string> pending(const std::filesystem::path& path) {
        std::scoped_lock lock(m_mutex);
        for (const auto& queued : m_queue) {
          if (!path.empty() && queued.path == path) {
            return queued.text;
          }
        }
        if (!path.empty() && m_running && m_running->path == path) {
          return m_running->text;
        }
        return std::nullopt;
      }

      void setPoster(MainLoopPoster poster) {
        std::scoped_lock lock(m_mutex);
        m_poster = std::move(poster);
      }

      [[nodiscard]] bool hasPoster() {
        std::scoped_lock lock(m_mutex);
        return static_cast<bool>(m_poster);
      }

      // Destroyed at exit: the stop request lets the queue drain, then the thread ends.
      ~Writer() {
        if (m_thread.joinable()) {
          m_thread.request_stop();
          m_wake.notify_one();
        }
      }

    private:
      void run(const std::stop_token& stop) {
        pthread_setname_np(pthread_self(), "andro-filewrite");
        andromeda::crash::installThreadStack();
        std::unique_lock lock(m_mutex);
        while (true) {
          m_wake.wait(lock, stop, [this] { return !m_queue.empty(); });
          if (m_queue.empty()) {
            return;
          }
          m_running = std::move(m_queue.front());
          m_queue.pop_front();
          lock.unlock();
          bool ok = false;
          if (m_running->op) {
            ok = m_running->op();
          } else {
            ok = m_running->write ? m_running->write(m_running->path, m_running->text)
                                  : writeFileReplacing(m_running->path, m_running->text);
            if (!ok) {
              kLog.warn("cannot write {}", m_running->path.string());
            }
          }
          lock.lock();
          std::vector<WriteDone> dones = std::move(m_running->dones);
          m_running.reset();
          // The poster is cleared at teardown: its loop no longer runs a done.
          for (auto& done : dones) {
            if (m_poster) {
              m_poster([done = std::move(done), ok] { done(ok); });
            }
          }
        }
      }

      std::mutex m_mutex;
      std::condition_variable_any m_wake;
      std::deque<Job> m_queue;
      // The write in progress, until it lands; only the writer thread sets it, under m_mutex.
      std::optional<Job> m_running;
      MainLoopPoster m_poster;
      std::jthread m_thread; // last: joined before the queue goes away
    };

    Writer& writer() {
      static Writer instance;
      return instance;
    }

  } // namespace

  bool writeFileReplacing(const std::filesystem::path& path, std::string_view text, unsigned fileMode, unsigned dirMode) {
    return writeFileReplacing(path, {text}, fileMode, dirMode);
  }

  bool writeFileReplacing(
      const std::filesystem::path& path, std::initializer_list<std::string_view> parts, unsigned fileMode,
      unsigned dirMode
  ) {
    noteBlockingWrite(path);
    // Each directory is swept once per process; leaked, so a write that drains at exit never sees it destroyed.
    static auto* swept = new std::set<std::filesystem::path>;
    static auto* sweptMutex = new std::mutex;
    const std::filesystem::path dir = path.parent_path();
    if (!makeDirectories(dir, dirMode)) {
      return false;
    }
    if (const std::scoped_lock lock(*sweptMutex); swept->insert(dir).second) {
      sweepDeadTemporaries(dir);
    }
    const std::filesystem::path temporary = path.string() + temporaryTag() + std::string(kTempSuffix);
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, fileMode);
    if (fd < 0) {
      return false;
    }
    // Held until close: the sweep in another process leaves a locked temporary alone.
    ::flock(fd, LOCK_EX);
    // rename is atomic: a crash mid-write keeps the previous file.
    const bool ok = std::ranges::all_of(parts, [fd](std::string_view part) { return writeAll(fd, part); }) &&
                    ::rename(temporary.c_str(), path.c_str()) == 0;
    if (!ok) {
      ::unlink(temporary.c_str());
    }
    ::close(fd);
    return ok;
  }

  bool writeKernelAttribute(const std::filesystem::path& path, std::string_view value) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
      return false;
    }
    const bool ok = writeAll(fd, value);
    ::close(fd);
    return ok;
  }

  namespace {
    thread_local bool t_loopRunning = false;
    std::atomic<std::uint64_t> g_blockingLoopWrites{0};
  } // namespace

  LoopThreadScope::LoopThreadScope() noexcept { t_loopRunning = true; }
  LoopThreadScope::~LoopThreadScope() { t_loopRunning = false; }

  void noteBlockingWrite(const std::filesystem::path& path) {
    if (!t_loopRunning) {
      return;
    }
    const std::string runtime = xdg::runtimeDir();
    const std::string& text = path.native();
    if (!runtime.empty() && text.starts_with(runtime) && (text.size() == runtime.size() || text[runtime.size()] == '/')) {
      return;
    }
    g_blockingLoopWrites.fetch_add(1, std::memory_order_relaxed);
    static auto* logged = new std::set<std::filesystem::path>;
    static auto* loggedMutex = new std::mutex;
    if (const std::scoped_lock lock(*loggedMutex); logged->insert(path).second) {
      kLog.warn("blocking write on the main loop: {}", text);
    }
  }

  std::uint64_t blockingLoopWrites() noexcept { return g_blockingLoopWrites.load(std::memory_order_relaxed); }

  void setFileWriteDonePoster(MainLoopPoster poster) { writer().setPoster(std::move(poster)); }

  namespace {
    std::vector<WriteDone> doneList(WriteDone done) {
      std::vector<WriteDone> dones;
      if (done) {
        if (!writer().hasPoster()) {
          kLog.error("a file write has a completion but this process set no main-loop poster; dropping it");
          return dones;
        }
        dones.push_back(std::move(done));
      }
      return dones;
    }
  } // namespace

  void writeFileAsync(std::filesystem::path path, std::string text, WriteFn write, WriteDone done) {
    writer().post({std::move(path), std::move(text), std::move(write), {}, doneList(std::move(done))});
  }

  void runFileOpAsync(std::function<bool()> op, WriteDone done) {
    writer().post({{}, {}, {}, std::move(op), doneList(std::move(done))});
  }

  std::optional<std::string> pendingFileText(const std::filesystem::path& path) { return writer().pending(path); }

} // namespace andromeda
