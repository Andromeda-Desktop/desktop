#include "agent_host.h"
#include "core/sys/sys.h"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core/deferred_call.h"

#ifndef ANDROMEDA_AGENTD_PATH
#define ANDROMEDA_AGENTD_PATH "/usr/lib/andromeda/andromeda-agentd"
#endif

extern char** environ;

namespace term::agent {

  struct Host::Shared {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::string> lines;
    bool closing = false;
    int stdinFd = -1;
  };

  namespace {

    constexpr std::size_t kStderrTail = 4096;
    constexpr std::int64_t kSessionIdSpan = std::int64_t{1} << 32;

    bool writeAll(int fd, const std::string& data) {
      std::size_t done = 0;
      while (done < data.size()) {
        const auto n = ::write(fd, data.data() + done, data.size() - done);
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

  } // namespace

  Host::Host(Kind kind, Changed changed) : m_kind(kind), m_changed(std::move(changed)) {}

  Host::~Host() {
    m_alive.reset();
    if (m_shared) {
      {
        const std::lock_guard lock(m_shared->mutex);
        m_shared->closing = true;
      }
      m_shared->wake.notify_all();
    }
    if (m_pid > 0) {
      ::kill(m_pid, SIGTERM);
    }
    for (auto* thread : {&m_writer, &m_reader, &m_errors}) {
      if (thread->joinable()) {
        thread->join();
      }
    }
  }

  std::string Host::program() {
    if (const char* path = std::getenv("ANDROMEDA_AGENTD"); path != nullptr && *path != '\0') {
      return path;
    }
    return ANDROMEDA_AGENTD_PATH;
  }

  Session& Host::open(std::uint64_t key, const std::string& cwd) {
    auto& slot = m_slots[key];
    slot.model = std::make_unique<Session>(cwd, static_cast<std::int64_t>(key + 1) * kSessionIdSpan);
    flush(key);
    return *slot.model;
  }

  void Host::close(std::uint64_t key) {
    if (auto* model = session(key)) {
      model->cancel();
      flush(key);
    }
    m_slots.erase(key);
  }

  Session* Host::session(std::uint64_t key) {
    const auto it = m_slots.find(key);
    return it == m_slots.end() ? nullptr : it->second.model.get();
  }

  void Host::flush(std::uint64_t key) {
    auto it = m_slots.find(key);
    if (it == m_slots.end()) {
      return;
    }
    auto& slot = it->second;
    if (slot.model->phase() == Phase::Starting && slot.newId == 0) {
      if (!ensureStarted()) {
        return;
      }
      if (m_initialized) {
        slot.newId = m_nextId++;
        if (slot.sessionId.empty()) {
          request("session/new", {{"cwd", slot.model->cwd()}, {"mcpServers", Json::array()}}, slot.newId);
        } else {
          request("session/load", {{"sessionId", slot.sessionId}, {"cwd", slot.model->cwd()}, {"mcpServers", Json::array()}}, slot.newId);
        }
      }
    }
    for (const auto& message : slot.model->takeOutbox()) {
      write(message);
    }
  }

  bool Host::ensureStarted() {
    if (m_pid > 0) {
      return true;
    }
    for (auto* thread : {&m_writer, &m_reader, &m_errors}) {
      if (thread->joinable()) {
        thread->join();
      }
    }
    std::array<int, 2> in{-1, -1};
    std::array<int, 2> out{-1, -1};
    std::array<int, 2> err{-1, -1};
    if (!sys::pipeCloexec(in.data()) || !sys::pipeCloexec(out.data()) || !sys::pipeCloexec(err.data())) {
      for (const int fd : {in[0], in[1], out[0], out[1], err[0], err[1]}) {
        if (fd >= 0) {
          ::close(fd);
        }
      }
      onExit(127, "Could not create pipes for the agent.");
      return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, err[1], STDERR_FILENO);
    std::vector<std::string> args = command(m_kind);
    std::vector<char*> argv;
    for (std::string& arg : args) {
      argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    // The agent and the tools it runs search the user's PATH, which a macOS app started from Finder lacks.
    const std::string program = resolve(m_kind);
    std::vector<std::string> env{"PATH=" + sys::userPath()};
    for (char** e = environ; *e != nullptr; ++e) {
      if (std::string_view(*e).starts_with("PATH=")) continue;
      env.emplace_back(*e);
    }
    std::vector<char*> envp;
    for (std::string& entry : env) {
      envp.push_back(entry.data());
    }
    envp.push_back(nullptr);
    pid_t pid = -1;
    const int rc = program.empty() ? ENOENT : posix_spawn(&pid, program.c_str(), &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    ::close(in[0]);
    ::close(out[1]);
    ::close(err[1]);
    if (rc != 0) {
      ::close(in[1]);
      ::close(out[0]);
      ::close(err[0]);
      onExit(127, "Could not start " + args.front() + ".");
      return false;
    }
    m_pid = pid;
    m_initialized = false;
    m_shared = std::make_shared<Shared>();
    m_shared->stdinFd = in[1];

    m_writer = std::thread([shared = m_shared] {
      for (;;) {
        std::unique_lock lock(shared->mutex);
        shared->wake.wait(lock, [&] { return shared->closing || !shared->lines.empty(); });
        if (shared->lines.empty()) {
          break;
        }
        auto line = std::move(shared->lines.front());
        shared->lines.pop_front();
        lock.unlock();
        if (!writeAll(shared->stdinFd, line)) {
          break;
        }
      }
      ::close(shared->stdinFd);
    });

    auto tail = std::make_shared<std::string>();
    auto tailMutex = std::make_shared<std::mutex>();
    m_errors = std::thread([fd = err[0], tail, tailMutex] {
      std::array<char, 1024> buffer{};
      for (;;) {
        const auto n = ::read(fd, buffer.data(), buffer.size());
        if (n < 0 && errno == EINTR) {
          continue;
        }
        if (n <= 0) {
          break;
        }
        const std::lock_guard lock(*tailMutex);
        tail->append(buffer.data(), static_cast<std::size_t>(n));
        if (tail->size() > kStderrTail) {
          tail->erase(0, tail->size() - kStderrTail);
        }
      }
      ::close(fd);
    });

    m_reader = std::thread([this, fd = out[0], pid, alive = std::weak_ptr<int>(m_alive), tail, tailMutex] {
      std::string pending;
      std::array<char, 16384> buffer{};
      for (;;) {
        const auto n = ::read(fd, buffer.data(), buffer.size());
        if (n < 0 && errno == EINTR) {
          continue;
        }
        if (n <= 0) {
          break;
        }
        pending.append(buffer.data(), static_cast<std::size_t>(n));
        std::size_t start = 0;
        for (auto end = pending.find('\n'); end != std::string::npos; end = pending.find('\n', start)) {
          auto message = Json::parse(pending.begin() + static_cast<std::ptrdiff_t>(start),
                                     pending.begin() + static_cast<std::ptrdiff_t>(end), nullptr, false);
          start = end + 1;
          if (message.is_discarded() || !message.is_object()) {
            continue;
          }
          DeferredCall::callLater([this, alive, message = std::move(message)] {
            if (!alive.expired()) {
              onMessage(message);
            }
          });
        }
        pending.erase(0, start);
      }
      ::close(fd);
      int status = 0;
      while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      const int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
      std::string text;
      {
        const std::lock_guard lock(*tailMutex);
        text = *tail;
      }
      DeferredCall::callLater([this, alive, code, text = std::move(text)] {
        if (!alive.expired()) {
          onExit(code, text);
        }
      });
    });

    m_initializeId = m_nextId++;
    request("initialize",
            {{"protocolVersion", 1},
             {"clientCapabilities", {{"fs", {{"readTextFile", false}, {"writeTextFile", false}}}, {"terminal", false}}}},
            m_initializeId);
    return true;
  }

  void Host::request(const std::string& method, Json params, std::int64_t id) {
    write({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}});
  }

  void Host::write(const Json& message) {
    if (!m_shared) {
      return;
    }
    {
      const std::lock_guard lock(m_shared->mutex);
      m_shared->lines.push_back(message.dump(-1, ' ', false, Json::error_handler_t::replace) + "\n");
    }
    m_shared->wake.notify_one();
  }

  Host::Slot* Host::slotBySessionId(const std::string& id, std::uint64_t* key) {
    for (auto& [k, slot] : m_slots) {
      if (slot.sessionId == id) {
        *key = k;
        return &slot;
      }
    }
    return nullptr;
  }

  void Host::onMessage(const Json& message) {
    const auto method = message.value("method", std::string{});
    const auto params = message.value("params", Json::object());
    const auto id = message.find("id");
    if (method.empty() && id != message.end()) {
      if (id->is_number_integer() && id->get<std::int64_t>() == m_initializeId) {
        m_initialized = !message.contains("error");
        if (!m_initialized) {
          ::kill(m_pid, SIGTERM);
          return;
        }
        for (auto& [key, slot] : m_slots) {
          if (slot.model->phase() == Phase::Starting) {
            slot.newId = 0;
            flush(key);
          }
        }
        return;
      }
      for (auto& [key, slot] : m_slots) {
        if (id->is_number_integer() && slot.newId != 0 && id->get<std::int64_t>() == slot.newId) {
          slot.newId = 0;
          const auto result = message.value("result", Json::object());
          if (const auto sid = result.value("sessionId", slot.sessionId); !sid.empty() && !message.contains("error")) {
            slot.sessionId = sid;
            slot.model->configure(result.value("configOptions", Json::array()));
            slot.model->started(sid);
          } else {
            slot.model->exited(1, message.value("error", Json::object()).value("message", std::string{"The agent did not open the session."}));
          }
          flush(key);
          m_changed(key);
          return;
        }
        if (slot.model->response(message)) {
          flush(key);
          m_changed(key);
          return;
        }
      }
      return;
    }
    std::uint64_t key = 0;
    Slot* slot = slotBySessionId(params.value("sessionId", std::string{}), &key);
    if (method == "session/update" && slot != nullptr) {
      slot->model->update(params);
    } else if (method == "_andromeda/compacting" && slot != nullptr) {
      slot->model->setCompacting(params.value("compacting", false));
    } else if (method == "session/request_permission") {
      if (slot == nullptr) {
        // No session can show it: cancelled, never approved.
        write({{"jsonrpc", "2.0"}, {"id", *id}, {"result", {{"outcome", {{"outcome", "cancelled"}}}}}});
        return;
      }
      slot->model->permission(*id, params);
    } else if (id != message.end()) {
      write({{"jsonrpc", "2.0"}, {"id", *id}, {"error", {{"code", -32601}, {"message", "Method not found"}}}});
      return;
    } else {
      return;
    }
    if (slot != nullptr) {
      flush(key);
      m_changed(key);
    }
  }

  void Host::onExit(int status, std::string stderrTail) {
    m_pid = -1;
    m_initialized = false;
    if (m_shared) {
      {
        const std::lock_guard lock(m_shared->mutex);
        m_shared->closing = true;
      }
      m_shared->wake.notify_all();
      m_shared.reset();
    }
    for (auto& [key, slot] : m_slots) {
      slot.newId = 0;
      slot.model->exited(status, stderrTail);
      m_changed(key);
    }
  }

} // namespace term::agent
