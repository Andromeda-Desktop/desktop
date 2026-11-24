#include "app/app_notifier.h"
#include "app/desk_app.h"
#include "app/notice_keys.h"

#include "core/log.h"
#include "dbus/session_bus.h"

#include <sdbus-c++/sdbus-c++.h>

#include <charconv>
#include <vector>

namespace {
  constexpr Logger kLog("notify");
  constexpr auto kInterface = "org.freedesktop.Notifications";
} // namespace

struct AppNotifier::Impl {
  explicit Impl(DeskApp& app);
  ~Impl();
  void notify(const AppNotice& notice, const std::string& key, ActionFn onAction, std::function<void()> onClosed);
  void close(const std::string& key);
  void sendClose(std::uint32_t id);

  struct Entry {
    std::string key;
    ActionFn onAction;
    std::function<void()> onClosed;
  };
  DeskApp& m_app;
  std::unique_ptr<sdbus::IProxy> m_proxy;
  std::map<std::uint32_t, Entry> m_entries;      // by notification id
  NoticeKeys m_keys;
  std::map<std::uint32_t, std::string> m_tokens; // ActivationToken before ActionInvoked
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

AppNotifier::AppNotifier(DeskApp& app) : m_impl(std::make_unique<Impl>(app)) {}
AppNotifier::~AppNotifier() = default;

void AppNotifier::notify(const AppNotice& notice, const std::string& key, ActionFn onAction, std::function<void()> onClosed) {
  m_impl->notify(notice, key, std::move(onAction), std::move(onClosed));
}

void AppNotifier::close(const std::string& key) { m_impl->close(key); }

AppNotifier::Impl::Impl(DeskApp& app) : m_app(app) {
  SessionBus* bus = app.bus();
  if (bus == nullptr || !bus->connected()) {
    return;
  }
  try {
    m_proxy = sdbus::createProxy(bus->connection(), sdbus::ServiceName{kInterface},
                                 sdbus::ObjectPath{"/org/freedesktop/Notifications"});
    m_proxy->uponSignal("ActivationToken").onInterface(kInterface).call([this](std::uint32_t id, const std::string& token) {
      if (m_entries.contains(id)) {
        m_tokens[id] = token;
      }
    });
    m_proxy->uponSignal("ActionInvoked").onInterface(kInterface).call([this](std::uint32_t id, const std::string& action) {
      const auto it = m_entries.find(id);
      int button = 0;
      if (it == m_entries.end()
          || (action != "default" && std::from_chars(action.data(), action.data() + action.size(), button).ec != std::errc{})) {
        return;
      }
      const auto token = m_tokens.extract(id);
      if (it->second.onAction) {
        it->second.onAction(token.empty() ? std::string{} : token.mapped(), button);
      }
    });
    m_proxy->uponSignal("NotificationClosed").onInterface(kInterface).call([this](std::uint32_t id, std::uint32_t) {
      auto entry = m_entries.extract(id);
      m_tokens.erase(id);
      if (entry.empty()) {
        return;
      }
      if (!entry.mapped().key.empty()) {
        m_keys.closedByServer(entry.mapped().key, id);
      }
      if (entry.mapped().onClosed) {
        entry.mapped().onClosed();
      }
    });
  } catch (const sdbus::Error& e) {
    kLog.warn("notifications unavailable: {}", e.what());
    m_proxy.reset();
  }
}

AppNotifier::Impl::~Impl() { m_alive.reset(); }

void AppNotifier::Impl::notify(const AppNotice& notice, const std::string& key, ActionFn onAction, std::function<void()> onClosed) {
  if (m_proxy == nullptr) {
    return;
  }
  std::vector<std::string> actions{"default", "Show"};
  for (std::size_t i = 0; i < notice.buttons.size(); ++i) {
    actions.insert(actions.end(), {std::to_string(i + 1), notice.buttons[i]});
  }
  const std::map<std::string, sdbus::Variant> hints{
      {"desktop-entry", sdbus::Variant{m_app.info().id}},
      {"urgency", sdbus::Variant{notice.urgency}}};
  const NoticeKeys::Request request = key.empty() ? NoticeKeys::Request{.gen = 0, .replaces = 0} : m_keys.notify(key);
  try {
    m_proxy->callMethodAsync("Notify")
        .onInterface(kInterface)
        .withArguments(m_app.info().name, request.replaces, m_app.info().id,
                       notice.title.empty() ? m_app.info().name : notice.title, notice.body, actions, hints,
                       std::int32_t{notice.urgency == 2 ? 0 : -1})
        .uponReplyInvoke([this, alive = std::weak_ptr<bool>(m_alive), key, gen = request.gen, onAction = std::move(onAction),
                          onClosed = std::move(onClosed), sending = std::make_shared<DeskApp::Lease>(m_app.lease())](
                             std::optional<sdbus::Error> error, std::uint32_t id) {
          if (alive.expired()) {
            return;
          }
          if (error) {
            kLog.warn("notify: {}", error->what());
            if (!key.empty()) {
              m_keys.failed(key, gen);
            }
            return;
          }
          if (!key.empty() && m_keys.replied(key, gen, id) == NoticeKeys::Reply::Close) {
            m_entries.erase(id);
            sendClose(id);
            return;
          }
          m_entries[id] = Entry{.key = key, .onAction = onAction, .onClosed = onClosed};
        });
  } catch (const sdbus::Error& e) {
    kLog.warn("notify: {}", e.what());
    if (!key.empty()) {
      m_keys.failed(key, request.gen);
    }
  }
}

void AppNotifier::Impl::close(const std::string& key) {
  if (const auto shown = m_keys.close(key)) {
    sendClose(*shown);
  }
}

void AppNotifier::Impl::sendClose(std::uint32_t id) {
  try {
    m_proxy->callMethodAsync("CloseNotification").onInterface(kInterface).withArguments(id).uponReplyInvoke(
        [](std::optional<sdbus::Error> error) {
          if (error) {
            kLog.warn("close notification: {}", error->what());
          }
        });
  } catch (const sdbus::Error& e) {
    kLog.warn("close notification: {}", e.what());
  }
}

