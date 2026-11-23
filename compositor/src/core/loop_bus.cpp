#include "core/loop_bus.h"

#include "core/log.h"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <map>
#include <poll.h>
#include <sys/auxv.h>
#include <systemd/sd-bus.h>
#include <unordered_set>
#include <utility>
#include <vector>
#include <wayland-server-core.h>

namespace andromeda::bus {

  namespace {

    constexpr Logger kLog("bus");
    constexpr int kRetryFirstMs = 2000;
    constexpr int kRetryMaxMs = 60'000;

    std::uint64_t monotonicUs() {
      timespec now{};
      clock_gettime(CLOCK_MONOTONIC, &now);
      return static_cast<std::uint64_t>(now.tv_sec) * 1'000'000 + static_cast<std::uint64_t>(now.tv_nsec) / 1000;
    }

    // sd_bus_open_user reads its environment with secure_getenv, which is empty in a secure-exec process (the
    // packaged cap_sys_nice binary); main() restored DBUS_SESSION_BUS_ADDRESS into environ for getenv.
    int openUserBus(sd_bus** bus) {
      const char* address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
      if (getauxval(AT_SECURE) == 0 || address == nullptr) {
        return sd_bus_open_user(bus);
      }
      sd_bus* opened = nullptr;
      int r = sd_bus_new(&opened);
      if (r >= 0) {
        r = sd_bus_set_address(opened, address);
      }
      if (r >= 0) {
        r = sd_bus_set_bus_client(opened, 1);
      }
      if (r >= 0) {
        r = sd_bus_start(opened);
      }
      if (r < 0) {
        sd_bus_unref(opened);
        return r;
      }
      *bus = opened;
      return 0;
    }

    const char* kindName(Kind kind) { return kind == Kind::System ? "system" : "session"; }

  } // namespace

  class Connection : public std::enable_shared_from_this<Connection> {
  public:
    static std::shared_ptr<Connection> get(wl_event_loop* loop, Kind kind);
    Connection(wl_event_loop* loop, Kind kind);
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    [[nodiscard]] sd_bus* bus() const { return m_bus; }
    void add(Client* client) { m_clients.push_back(client); }
    void remove(Client* client) { std::erase(m_clients, client); }
    bool call(sd_bus_message* message, Client::Reply reply, std::uint64_t timeoutUs, std::weak_ptr<bool> alive);
    void update();

  private:
    struct Pending {
      std::weak_ptr<bool> alive;
      Client::Reply reply;
      Connection* owner = nullptr;
      bool answered = false;
    };

    bool open();
    void lose();
    void dispatch();
    template <typename Fn> void eachClient(Fn&& fn);
    static int onEvent(int fd, std::uint32_t mask, void* data);
    static int onTimer(void* data);
    static int onRetry(void* data);
    static int onReply(sd_bus_message* reply, void* data, sd_bus_error* error);
    static void onDestroy(void* data);

    static std::map<std::pair<wl_event_loop*, Kind>, std::weak_ptr<Connection>>& registry();

    wl_event_loop* m_loop;
    Kind m_kind;
    sd_bus* m_bus = nullptr;
    wl_event_source* m_source = nullptr;
    wl_event_source* m_timer = nullptr;
    wl_event_source* m_retry = nullptr;
    int m_retryMs = kRetryFirstMs;
    std::vector<Client*> m_clients;
    std::unordered_set<Pending*> m_pending;
  };

  std::map<std::pair<wl_event_loop*, Kind>, std::weak_ptr<Connection>>& Connection::registry() {
    static std::map<std::pair<wl_event_loop*, Kind>, std::weak_ptr<Connection>> connections;
    return connections;
  }

  std::shared_ptr<Connection> Connection::get(wl_event_loop* loop, Kind kind) {
    auto& slot = registry()[{loop, kind}];
    if (auto existing = slot.lock()) {
      return existing;
    }
    auto created = std::make_shared<Connection>(loop, kind);
    slot = created;
    return created;
  }

  Connection::Connection(wl_event_loop* loop, Kind kind)
      : m_loop(loop), m_kind(kind), m_retry(wl_event_loop_add_timer(loop, onRetry, this)) {
    if (!open()) {
      kLog.info("no {} bus: opening it again in {} ms", kindName(m_kind), m_retryMs);
      wl_event_source_timer_update(m_retry, m_retryMs);
    }
  }

  Connection::~Connection() {
    registry().erase({m_loop, m_kind});
    for (Pending* pending : m_pending) {
      pending->owner = nullptr;
    }
    for (wl_event_source* source : {m_source, m_timer, m_retry}) {
      if (source != nullptr) {
        wl_event_source_remove(source);
      }
    }
    if (m_bus != nullptr) {
      sd_bus_flush_close_unref(m_bus);
    }
  }

  bool Connection::open() {
    const int r = m_kind == Kind::System ? sd_bus_open_system(&m_bus) : openUserBus(&m_bus);
    if (r < 0) {
      m_bus = nullptr;
      return false;
    }
    m_source = wl_event_loop_add_fd(m_loop, sd_bus_get_fd(m_bus), WL_EVENT_READABLE, onEvent, this);
    m_timer = wl_event_loop_add_timer(m_loop, onTimer, this);
    if (m_source == nullptr || m_timer == nullptr) {
      for (wl_event_source** source : {&m_source, &m_timer}) {
        if (*source != nullptr) {
          wl_event_source_remove(*source);
          *source = nullptr;
        }
      }
      m_bus = sd_bus_close_unref(m_bus);
      return false;
    }
    m_retryMs = kRetryFirstMs;
    update();
    return true;
  }

  template <typename Fn> void Connection::eachClient(Fn&& fn) {
    // A callback may destroy other clients; only clients still registered run.
    const std::vector<Client*> snapshot = m_clients;
    for (Client* client : snapshot) {
      if (std::ranges::find(m_clients, client) != m_clients.end()) {
        fn(*client);
      }
    }
  }

  // The event loop polls its own copy of the bus fd: after a loss it reports a hangup on every turn, so the sources go.
  void Connection::lose() {
    for (wl_event_source** source : {&m_source, &m_timer}) {
      if (*source != nullptr) {
        wl_event_source_remove(*source);
        *source = nullptr;
      }
    }
    sd_bus* old = std::exchange(m_bus, nullptr);
    const auto pending = std::exchange(m_pending, {});
    for (Pending* call : pending) {
      call->owner = nullptr;
    }
    eachClient([](Client& client) { client.lost(); });
    for (Pending* call : pending) {
      if (!std::exchange(call->answered, true)) {
        if (const auto alive = call->alive.lock()) {
          call->reply(nullptr);
        }
      }
    }
    if (old != nullptr) {
      sd_bus_close_unref(old);
    }
    wl_event_source_timer_update(m_retry, m_retryMs);
  }

  int Connection::onRetry(void* data) {
    auto* self = static_cast<Connection*>(data);
    const auto keep = self->shared_from_this();
    if (self->open()) {
      kLog.info("{} bus open again", kindName(self->m_kind));
      self->eachClient([self](Client& client) {
        if (client.m_changed && self->m_bus != nullptr) {
          client.m_changed(self->m_bus);
        }
      });
      self->update();
    } else {
      self->m_retryMs = std::min(self->m_retryMs * 2, kRetryMaxMs);
      wl_event_source_timer_update(self->m_retry, self->m_retryMs);
    }
    return 0;
  }

  int Connection::onEvent(int /*fd*/, std::uint32_t mask, void* data) {
    auto* self = static_cast<Connection*>(data);
    const auto keep = self->shared_from_this();
    // dispatch first: replies already buffered still reach their callers.
    self->dispatch();
    if (self->m_bus != nullptr && (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) != 0) {
      kLog.warn("{} bus closed: opening it again in {} ms", kindName(self->m_kind), self->m_retryMs);
      self->lose();
    }
    return 0;
  }

  int Connection::onTimer(void* data) {
    auto* self = static_cast<Connection*>(data);
    const auto keep = self->shared_from_this();
    self->dispatch();
    return 0;
  }

  void Connection::dispatch() {
    if (m_bus == nullptr) {
      return;
    }
    int result = 0;
    while ((result = sd_bus_process(m_bus, nullptr)) > 0) {
    }
    if (result < 0) {
      kLog.warn("{} bus failed: opening it again in {} ms", kindName(m_kind), m_retryMs);
      lose();
      return;
    }
    update();
  }

  void Connection::update() {
    if (m_bus == nullptr) {
      return;
    }
    const int events = sd_bus_get_events(m_bus);
    wl_event_source_fd_update(
        m_source,
        WL_EVENT_READABLE | ((events > 0 && (events & POLLOUT) != 0) ? static_cast<uint32_t>(WL_EVENT_WRITABLE) : 0U)
    );
    // sd_bus_get_timeout: absolute CLOCK_MONOTONIC µs of the next timeout, UINT64_MAX for none.
    std::uint64_t deadline = UINT64_MAX;
    int delayMs = 0;
    if (sd_bus_get_timeout(m_bus, &deadline) >= 0 && deadline != UINT64_MAX) {
      const std::uint64_t now = monotonicUs();
      delayMs = deadline <= now ? 1 : static_cast<int>((deadline - now + 999) / 1000);
    }
    wl_event_source_timer_update(m_timer, delayMs);
  }

  bool
  Connection::call(sd_bus_message* message, Client::Reply reply, std::uint64_t timeoutUs, std::weak_ptr<bool> alive) {
    if (m_bus == nullptr || message == nullptr) {
      return false;
    }
    auto* pending = new Pending{std::move(alive), std::move(reply), this};
    sd_bus_slot* slot = nullptr;
    if (sd_bus_call_async(m_bus, &slot, message, onReply, pending, timeoutUs) < 0) {
      delete pending;
      return false;
    }
    m_pending.insert(pending);
    // The bus owns the slot from here: it frees the call after the reply, or when the bus goes.
    sd_bus_slot_set_destroy_callback(slot, onDestroy);
    sd_bus_slot_set_floating(slot, 1);
    sd_bus_slot_unref(slot);
    update();
    return true;
  }

  int Connection::onReply(sd_bus_message* reply, void* data, sd_bus_error* /*error*/) {
    auto* pending = static_cast<Pending*>(data);
    if (std::exchange(pending->answered, true)) {
      return 0;
    }
    if (const auto alive = pending->alive.lock()) {
      pending->reply(reply);
    }
    return 0;
  }

  void Connection::onDestroy(void* data) {
    auto* pending = static_cast<Pending*>(data);
    if (pending->owner != nullptr) {
      pending->owner->m_pending.erase(pending);
    }
    delete pending;
  }

  Client::Client(wl_event_loop* loop, Kind kind) {
    if (loop != nullptr) {
      m_connection = Connection::get(loop, kind);
      m_connection->add(this);
    }
  }

  Client::~Client() {
    m_alive.reset();
    for (sd_bus_slot* slot : m_slots) {
      sd_bus_slot_unref(slot);
    }
    if (m_connection) {
      m_connection->remove(this);
    }
  }

  sd_bus* Client::get() const { return m_connection ? m_connection->bus() : nullptr; }

  void Client::onChange(std::function<void(sd_bus*)> changed) { m_changed = std::move(changed); }

  sd_bus_slot** Client::keep() { return &m_slots.emplace_back(nullptr); }

  bool Client::call(sd_bus_message* message, Reply reply, std::uint64_t timeoutUs) {
    return m_connection && m_connection->call(message, std::move(reply), timeoutUs, m_alive);
  }

  void Client::sent() {
    if (m_connection) {
      m_connection->update();
    }
  }

  void Client::lost() {
    for (sd_bus_slot* slot : m_slots) {
      sd_bus_slot_unref(slot);
    }
    m_slots.clear();
    if (m_changed) {
      m_changed(nullptr);
    }
  }

} // namespace andromeda::bus
