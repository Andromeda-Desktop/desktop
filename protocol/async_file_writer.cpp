#include "async_file_writer.h"
#include "crash_capture.h"
#include "log.h"

#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <pthread.h>
#include <stop_token>
#include <thread>
#include <unistd.h>
#include <utility>

namespace andromeda {

  namespace {

    constexpr Logger kLog("file-writer");

    struct Job {
      std::filesystem::path path;
      std::string text;
      WriteFn write;
    };

    bool writeReplacing(const std::filesystem::path& path, const std::string& text) {
      std::error_code error;
      std::filesystem::create_directories(path.parent_path(), error);
      // Processes that share a cache write the same path: each uses its own temporary file.
      const std::filesystem::path temporary = path.string() + "." + std::to_string(::getpid()) + ".tmp";
      {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) {
          out.close();
          std::filesystem::remove(temporary, error);
          return false;
        }
      }
      // rename is atomic: a crash mid-write keeps the previous file.
      std::filesystem::rename(temporary, path, error);
      if (error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
      }
      return true;
    }

    class Writer {
    public:
      void post(Job job) {
        std::scoped_lock lock(m_mutex);
        for (auto& pending : m_queue) {
          if (pending.path == job.path) {
            pending = std::move(job);
            return;
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
          if (queued.path == path) {
            return queued.text;
          }
        }
        if (m_running && m_running->path == path) {
          return m_running->text;
        }
        return std::nullopt;
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
          const bool ok = m_running->write ? m_running->write(m_running->path, m_running->text)
                                           : writeReplacing(m_running->path, m_running->text);
          if (!ok) {
            kLog.warn("cannot write {}", m_running->path.string());
          }
          lock.lock();
          m_running.reset();
        }
      }

      std::mutex m_mutex;
      std::condition_variable_any m_wake;
      std::deque<Job> m_queue;
      // The write in progress, until it lands; only the writer thread sets it, under m_mutex.
      std::optional<Job> m_running;
      std::jthread m_thread; // last: joined before the queue goes away
    };

    Writer& writer() {
      static Writer instance;
      return instance;
    }

  } // namespace

  void writeFileAsync(std::filesystem::path path, std::string text, WriteFn write) {
    writer().post({std::move(path), std::move(text), std::move(write)});
  }

  std::optional<std::string> pendingFileText(const std::filesystem::path& path) { return writer().pending(path); }

} // namespace andromeda
