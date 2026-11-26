#include "core/async_file_writer.h"
#include "protocols/crash_capture.h"

#include "core/log.h"

#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <pthread.h>
#include <stop_token>
#include <thread>
#include <utility>

namespace andromeda {

  namespace {

    constexpr Logger kLog("file-writer");

    class Writer {
    public:
      void post(std::filesystem::path path, std::string text) {
        std::scoped_lock lock(m_mutex);
        for (auto& pending : m_queue) {
          if (pending.first == path) {
            pending.second = std::move(text);
            return;
          }
        }
        m_queue.emplace_back(std::move(path), std::move(text));
        if (!m_thread.joinable()) {
          m_thread = std::jthread([this](const std::stop_token& stop) { run(stop); });
        }
        m_wake.notify_one();
      }

      std::optional<std::string> pending(const std::filesystem::path& path) {
        std::scoped_lock lock(m_mutex);
        for (const auto& queued : m_queue) {
          if (queued.first == path) {
            return queued.second;
          }
        }
        if (m_running && m_running->first == path) {
          return m_running->second;
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
          write(m_running->first, m_running->second);
          lock.lock();
          m_running.reset();
        }
      }

      static void write(const std::filesystem::path& path, const std::string& text) {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        const std::filesystem::path temporary = path.string() + ".tmp";
        {
          std::ofstream out(temporary, std::ios::trunc);
          out << text;
          if (!out) {
            kLog.warn("cannot write {}", temporary.string());
            return;
          }
        }
        // rename is atomic: a crash mid-write keeps the previous file.
        std::filesystem::rename(temporary, path, error);
      }

      std::mutex m_mutex;
      std::condition_variable_any m_wake;
      std::deque<std::pair<std::filesystem::path, std::string>> m_queue;
      // The write in progress, until its rename lands; only the writer thread sets it, under m_mutex.
      std::optional<std::pair<std::filesystem::path, std::string>> m_running;
      std::jthread m_thread; // last: joined before the queue goes away
    };

    Writer& writer() {
      static Writer instance;
      return instance;
    }

  } // namespace

  void writeFileAsync(std::filesystem::path path, std::string text) { writer().post(std::move(path), std::move(text)); }

  std::optional<std::string> pendingFileText(const std::filesystem::path& path) { return writer().pending(path); }

} // namespace andromeda
