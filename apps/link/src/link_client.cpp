#include "link_client.h"

#include "core/log.h"
#include "dbus/session_bus.h"

#include <chrono>
#include <map>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>

namespace linkapp {

  namespace {

    constexpr Logger kLog("link");
    const sdbus::ServiceName kBusName{"org.andromeda.Link1"};
    const sdbus::ObjectPath kObjectPath{"/org/andromeda/Link1"};
    constexpr auto kInterface = "org.andromeda.Link1";

    std::int64_t nowMs() {
      return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    template <typename T>
    T value(const std::map<std::string, sdbus::Variant>& map, const std::string& key, T fallback) {
      const auto it = map.find(key);
      if (it == map.end() || !it->second.containsValueOfType<T>()) return fallback;
      return it->second.get<T>();
    }

    template <typename T>
    T property(const std::map<std::string, sdbus::Variant>& props, const std::string& key) {
      const auto it = props.find(key);
      if (it == props.end() || !it->second.containsValueOfType<T>()) return T{};
      return it->second.get<T>();
    }

  } // namespace

  LinkClient::LinkClient(SessionBus& bus) : m_bus(bus) {
    m_proxy = sdbus::createProxy(m_bus.connection(), kBusName, kObjectPath);
    m_proxy->uponSignal("PropertiesChanged")
        .onInterface("org.freedesktop.DBus.Properties")
        .call([this](const std::string& interfaceName, const std::map<std::string, sdbus::Variant>&,
                     const std::vector<std::string>&) {
          if (interfaceName == kInterface) refresh();
        });
    m_proxy->uponSignal("NotificationPosted")
        .onInterface(kInterface)
        .call([this](const std::string& device, const std::string& id, const std::string& app, const std::string& title,
                     const std::string& text, const std::vector<std::uint8_t>&,
                     const std::vector<sdbus::Struct<std::string, std::string, bool>>& actions, bool) {
          FeedItem item{.kind = FeedItem::Kind::Notification, .key = "n:" + id, .deviceId = device,
                        .who = app.empty() ? title : app + " · " + title, .body = text, .atMs = nowMs()};
          for (const auto& a : actions) item.actions.push_back({a.get<0>(), a.get<1>(), a.get<2>()});
          m_feed.upsert(std::move(item));
          changed();
        });
    m_proxy->uponSignal("NotificationRemoved")
        .onInterface(kInterface)
        .call([this](const std::string&, const std::string& id) {
          if (m_feed.remove("n:" + id)) changed();
        });
    m_proxy->uponSignal("TextReceived")
        .onInterface(kInterface)
        .call([this](const std::string& device, std::uint64_t id, const std::string& address, const std::string& body) {
          m_feed.upsert({.kind = FeedItem::Kind::Text, .key = "t:" + std::to_string(id), .deviceId = device,
                         .who = address, .body = body, .atMs = nowMs()});
          changed();
        });
    m_proxy->uponSignal("TransferOffered")
        .onInterface(kInterface)
        .call([this](const std::string& transfer, const std::string& device,
                     const std::vector<sdbus::Struct<std::string, std::uint64_t>>& files) {
          FeedItem item{.kind = FeedItem::Kind::Offer, .key = "o:" + transfer, .deviceId = device, .atMs = nowMs()};
          for (const auto& f : files) {
            item.files.emplace_back(f.get<0>(), f.get<1>());
            item.total += f.get<1>();
          }
          m_feed.upsert(std::move(item));
          changed();
        });
    m_proxy->uponSignal("TransferProgress")
        .onInterface(kInterface)
        .call([this](const std::string& transfer, std::uint64_t bytes, std::uint64_t total) {
          if (m_feed.moveTransfer("o:" + transfer, Transfer::Running, bytes, total)) changed();
        });
    m_proxy->uponSignal("TransferFinished")
        .onInterface(kInterface)
        .call([this](const std::string& transfer, const std::string& status, const std::vector<std::string>&) {
          const std::string key = "o:" + transfer;
          const bool moved = status == "done"       ? m_feed.moveTransfer(key, Transfer::Done)
                             : status == "declined" ? m_feed.remove(key)
                                                    : m_feed.moveTransfer(key, Transfer::Failed);
          if (moved) changed();
        });
    refresh();
  }

  LinkClient::~LinkClient() = default;

  void LinkClient::changed() {
    if (m_changed) m_changed();
  }

  void LinkClient::refresh() {
    const std::uint64_t serial = ++m_sent;
    m_proxy->callMethodAsync("GetAll")
        .onInterface("org.freedesktop.DBus.Properties")
        .withArguments(std::string{kInterface})
        .uponReplyInvoke([this, serial, alive = std::weak_ptr<bool>(m_alive)](
                             std::optional<sdbus::Error> error, std::map<std::string, sdbus::Variant> props) {
          if (alive.expired() || serial <= m_applied) return;
          m_applied = serial;
          m_daemon = !error.has_value();
          m_phones.clear();
          if (error.has_value()) {
            kLog.debug("link unavailable: {}", error->getMessage());
            changed();
            return;
          }
          m_pairing = property<bool>(props, "Pairing");
          const auto info = property<std::map<std::string, std::map<std::string, sdbus::Variant>>>(props, "DeviceInfo");
          const auto status =
              property<std::map<std::string, sdbus::Struct<std::uint32_t, bool, std::string, std::int32_t>>>(props, "DeviceStatus");
          const auto controls =
              property<std::map<std::string, sdbus::Struct<std::int32_t, std::string, std::int32_t, std::int32_t>>>(props, "DeviceControls");
          const auto paused = property<std::vector<std::string>>(props, "Paused");
          for (const auto& d : property<std::vector<sdbus::Struct<std::string, std::string, bool>>>(props, "Devices")) {
            Phone phone{.id = d.get<0>(), .name = d.get<1>(), .connected = d.get<2>()};
            if (const auto it = info.find(phone.id); it != info.end()) {
              phone.transport = value<std::string>(it->second, "transport", "");
              phone.lastSeen = value<std::int64_t>(it->second, "last_seen", 0);
              phone.battery = value<std::int32_t>(it->second, "battery", -1);
            }
            if (const auto it = status.find(phone.id); it != status.end()) {
              phone.battery = static_cast<int>(it->second.get<0>());
              phone.charging = it->second.get<1>();
            }
            if (const auto it = controls.find(phone.id); it != controls.end()) {
              phone.dnd = it->second.get<2>();
              phone.locked = it->second.get<3>();
            }
            phone.paused = std::ranges::find(paused, phone.id) != paused.end();
            m_phones.push_back(std::move(phone));
          }
          // Missed calls of the last day; the Calls page holds the rest.
          const std::int64_t since = nowMs() - 24LL * 3600 * 1000;
          using Call = sdbus::Struct<std::string, std::string, std::uint64_t, std::uint32_t, std::string>;
          for (const auto& [device, calls] : property<std::map<std::string, std::vector<Call>>>(props, "Calls")) {
            for (const Call& call : calls) {
              const auto at = static_cast<std::int64_t>(call.get<2>());
              if (call.get<4>() != "missed" || at < since) continue;
              const std::string key = "c:" + device + ":" + std::to_string(at);
              if (m_feed.find(key) || m_dismissed.contains(key)) continue;
              m_feed.upsert({.kind = FeedItem::Kind::MissedCall, .key = key, .deviceId = device,
                             .who = call.get<1>().empty() ? call.get<0>() : call.get<1>(), .body = "Missed call",
                             .atMs = at, .actions = {{call.get<0>(), "Call back", false}}});
            }
          }
          changed();
        });
  }

  void LinkClient::reply(const FeedItem& item, const std::string& text) {
    if (item.kind == FeedItem::Kind::Text) {
      m_proxy->callMethodAsync("SendText").onInterface(kInterface).withArguments(item.deviceId, item.who, text)
          .uponReplyInvoke([](std::optional<sdbus::Error> error) {
            if (error) kLog.warn("SendText: {}", error->getMessage());
          });
    } else {
      const auto reply = std::ranges::find_if(item.actions, &FeedAction::reply);
      if (reply == item.actions.end()) return;
      m_proxy->callMethodAsync("NotificationAction").onInterface(kInterface)
          .withArguments(item.deviceId, item.key.substr(2), reply->id, text)
          .uponReplyInvoke([](std::optional<sdbus::Error> error) {
            if (error) kLog.warn("NotificationAction: {}", error->getMessage());
          });
    }
    if (m_feed.remove(item.key)) changed();
  }

  void LinkClient::action(const FeedItem& item, const std::string& actionId) {
    if (item.kind == FeedItem::Kind::MissedCall) {
      m_proxy->callMethodAsync("Dial").onInterface(kInterface).withArguments(item.deviceId, actionId)
          .uponReplyInvoke([](std::optional<sdbus::Error> error) {
            if (error) kLog.warn("Dial: {}", error->getMessage());
          });
      return;
    }
    m_proxy->callMethodAsync("NotificationAction").onInterface(kInterface)
        .withArguments(item.deviceId, item.key.substr(2), actionId, std::string{})
        .uponReplyInvoke([](std::optional<sdbus::Error> error) {
          if (error) kLog.warn("NotificationAction: {}", error->getMessage());
        });
  }

  void LinkClient::dismiss(const FeedItem& item) {
    if (item.kind == FeedItem::Kind::Notification) {
      m_proxy->callMethodAsync("NotificationDismiss").onInterface(kInterface).withArguments(item.deviceId, item.key.substr(2))
          .uponReplyInvoke([](std::optional<sdbus::Error> error) {
            if (error) kLog.warn("NotificationDismiss: {}", error->getMessage());
          });
    }
    if (item.kind == FeedItem::Kind::MissedCall) m_dismissed.insert(item.key);
    if (m_feed.remove(item.key)) changed();
  }

  void LinkClient::accept(const FeedItem& offer) {
    m_proxy->callMethodAsync("AcceptTransfer").onInterface(kInterface).withArguments(offer.key.substr(2))
        .uponReplyInvoke([](std::optional<sdbus::Error> error) {
          if (error) kLog.warn("AcceptTransfer: {}", error->getMessage());
        });
  }

  void LinkClient::decline(const FeedItem& offer) {
    m_proxy->callMethodAsync("DeclineTransfer").onInterface(kInterface).withArguments(offer.key.substr(2))
        .uponReplyInvoke([](std::optional<sdbus::Error> error) {
          if (error) kLog.warn("DeclineTransfer: {}", error->getMessage());
        });
    if (m_feed.remove(offer.key)) changed();
  }

  void LinkClient::connect(const std::string& deviceId) {
    m_proxy->callMethodAsync("Connect").onInterface(kInterface).withArguments(deviceId)
        .uponReplyInvoke([](std::optional<sdbus::Error> error) {
          if (error) kLog.warn("Connect: {}", error->getMessage());
        });
  }

  void LinkClient::ring(const std::string& deviceId) {
    m_proxy->callMethodAsync("Ring").onInterface(kInterface).withArguments(deviceId, true)
        .uponReplyInvoke([](std::optional<sdbus::Error> error) {
          if (error) kLog.warn("Ring: {}", error->getMessage());
        });
  }

  void LinkClient::setDnd(const std::string& deviceId, bool on) {
    m_proxy->callMethodAsync("SendSystemCommand").onInterface(kInterface)
        .withArguments(deviceId, std::string("set-dnd"), static_cast<std::uint32_t>(on ? 1 : 0))
        .uponReplyInvoke([](std::optional<sdbus::Error> error) {
          if (error) kLog.warn("SendSystemCommand set-dnd: {}", error->getMessage());
        });
  }

  void LinkClient::clearAll() {
    std::vector<FeedItem> items = m_feed.items();
    for (const FeedItem& item : items) {
      if (item.kind == FeedItem::Kind::Offer && item.transfer == Transfer::Running) continue;
      dismiss(item);
    }
  }

} // namespace linkapp
