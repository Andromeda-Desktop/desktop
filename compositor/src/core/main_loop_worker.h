#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

struct wl_event_loop;
struct wl_event_source;

namespace andromeda {

  // One thread for slow work (/proc scans, state file saves) that must stay off the compositor main thread. `done`
  // runs on the event loop. Jobs run in order; stop() and the destructor still run every queued job, and the
  // destructor drops the results not yet delivered.
  class MainLoopWorker {
  public:
    // `name` is the thread name (15 bytes at most), static storage.
    explicit MainLoopWorker(wl_event_loop* loop, const char* name = "andro-worker");
    ~MainLoopWorker();

    // Runs the queued jobs and ends the thread; a job posted after it never runs. For an owner that
    // writes its final state in place and must not race a job on the same file.
    void stop();

    // False when the worker could not attach to the event loop: a post is dropped and its `done` never runs.
    [[nodiscard]] bool ready() const { return m_source != nullptr; }

    MainLoopWorker(const MainLoopWorker&) = delete;
    MainLoopWorker& operator=(const MainLoopWorker&) = delete;

    // `work` runs on the worker thread and must touch no main-thread state; `done` gets its result on the loop.
    template <typename Result> void post(std::function<Result()> work, std::function<void(Result)> done) {
      postJob([work = std::move(work), done = std::move(done)]() mutable -> std::function<void()> {
        auto result = std::make_shared<Result>(work());
        return [done = std::move(done), result] { done(std::move(*result)); };
      });
    }

    // Runs `fn` on the loop; callable from any thread. Dropped when the worker is not ready.
    void postToLoop(std::function<void()> fn);

  private:
    using Job = std::function<std::function<void()>()>;
    void postJob(Job job);
    void run(const std::stop_token& stop);
    static int onReady(int fd, std::uint32_t mask, void* data);

    int m_eventFd = -1;
    wl_event_source* m_source = nullptr;
    std::mutex m_mutex;
    std::condition_variable_any m_wake;
    std::deque<Job> m_jobs;
    std::deque<std::function<void()>> m_results;
    const char* m_name;
    // Last member: joined before the queues it reads go away.
    std::jthread m_thread;
  };

} // namespace andromeda
