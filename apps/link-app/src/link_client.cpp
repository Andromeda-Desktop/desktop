#include "link_client.h"

#include "core/log.h"
#include "dbus/session_bus.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <array>
#include <fcntl.h>
#include <filesystem>
#include <glib.h>
#include <chrono>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>

namespace linkapp {

  namespace {

    constexpr Logger kLog("link");
    const sdbus::ServiceName kBusName{"org.andromeda.Link1"};
    const sdbus::ObjectPath kObjectPath{"/org/andromeda/Link1"};
    constexpr auto kInterface = "org.andromeda.Link1";
    constexpr auto kQuickShare = "org.andromeda.Link1.QuickShare";

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

  namespace {
    std::filesystem::path selectedFile() {
      return std::filesystem::path(g_get_user_state_dir()) / "andromeda" / "link-app" / "phone";
    }

    std::filesystem::path photosSeenFile(const std::string& deviceId) {
      return selectedFile().parent_path() / ("photos-seen-" + deviceId);
    }

    std::filesystem::path snapshotFile() {
      return std::filesystem::path(g_get_user_cache_dir()) / "andromeda" / "link-app" / "waiting.json";
    }

    // Items a phone may have removed while this app was closed: older ones stay out of a cold start.
    constexpr std::int64_t kSnapshotMaxAgeMs = 24LL * 3600 * 1000;
  } // namespace

  LinkClient::LinkClient(SessionBus& bus) : m_bus(bus) {
    std::getline(std::ifstream(selectedFile()), m_selected);
    std::ifstream cached(snapshotFile(), std::ios::binary);
    if (auto snapshot = decodeSnapshot(std::string(std::istreambuf_iterator<char>(cached), {}))) {
      m_daemon = true;
      m_phones = std::move(snapshot->phones);
      m_dismissed = std::move(snapshot->dismissed);
      const std::int64_t oldest = nowMs() - kSnapshotMaxAgeMs;
      for (FeedItem& item : snapshot->items) {
        if (item.atMs >= oldest) m_feed.upsert(std::move(item));
      }
    }
    m_proxy = sdbus::createProxy(m_bus.connection(), kBusName, kObjectPath);
    // Link1 starting or stopping after this app: ask again, so presence leaves NoDaemon or enters it.
    m_busWatch = sdbus::createProxy(m_bus.connection(), sdbus::ServiceName{"org.freedesktop.DBus"},
                                    sdbus::ObjectPath{"/org/freedesktop/DBus"});
    m_busWatch->uponSignal("NameOwnerChanged").onInterface("org.freedesktop.DBus")
        .call([this](const std::string& name, const std::string&, const std::string&) {
          if (name == kBusName) {
            refresh();
            refreshNearby();
          }
        });
    m_proxy->uponSignal("PropertiesChanged")
        .onInterface("org.freedesktop.DBus.Properties")
        .call([this](const std::string& interfaceName, const std::map<std::string, sdbus::Variant>&,
                     const std::vector<std::string>&) {
          if (interfaceName == kInterface) refresh();
          if (interfaceName == kQuickShare) refreshNearby();
        });
    m_proxy->uponSignal("PairingFinished").onInterface(kInterface).call([this](const std::string&, const std::string&) {
      if (pairStep(m_pair, {.kind = PairEvent::Kind::Finished})) changed();
    });
    m_proxy->uponSignal("PairingFailed").onInterface(kInterface).call([this](const std::string& reason) {
      if (pairStep(m_pair, {.kind = PairEvent::Kind::Failed, .error = reason})) changed();
    });
    m_proxy->uponSignal("NotificationPosted")
        .onInterface(kInterface)
        .call([this](const std::string& device, const std::string& id, const std::string& app, const std::string& title,
                     const std::string& text, const std::vector<std::uint8_t>&,
                     const std::vector<sdbus::Struct<std::string, std::string, bool>>& actions, bool,
                     const std::vector<std::string>& replies) {
          FeedItem item{.kind = FeedItem::Kind::Notification, .key = "n:" + id, .deviceId = device,
                        .who = app.empty() ? title : app + " · " + title, .body = text, .atMs = nowMs(),
                        .replies = replies};
          for (const auto& a : actions) item.actions.push_back({a.get<0>(), a.get<1>(), a.get<2>()});
          m_feed.upsert(std::move(item));
          changed();
        });
    m_proxy->uponSignal("HotspotRequested")
        .onInterface(kInterface)
        .call([this](const std::string& device, const std::string& ssid) {
          if (!ssid.empty()) {
            m_hotspotAsk = HotspotAsk{.deviceId = device, .ssid = ssid};
          } else if (m_hotspotAsk && m_hotspotAsk->deviceId == device) {
            m_hotspotAsk.reset();
          } else {
            return;
          }
          changed();
        });
    m_proxy->uponSignal("ScreenShape")
        .onInterface(kInterface)
        .call([this](const std::string& device, const std::vector<std::uint16_t>& corners,
                     const std::vector<sdbus::Struct<std::uint16_t, std::uint16_t, std::uint16_t, std::uint16_t>>&) {
          if (device != m_mirrorDevice) return;
          const std::uint16_t radius = corners.empty() ? 0 : std::ranges::max(corners);
          if (radius != std::exchange(m_mirrorCorner, radius)) changed();
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
          m_messages.received(id);
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
          m_activity.offered(transfer, device, item.files, item.atMs);
          m_feed.upsert(std::move(item));
          changed();
        });
    m_proxy->uponSignal("TransferProgress")
        .onInterface(kInterface)
        .call([this](const std::string& transfer, std::uint64_t bytes, std::uint64_t total) {
          const bool feed = m_feed.moveTransfer("o:" + transfer, Transfer::Running, bytes, total);
          if (m_activity.progress(transfer, bytes, total, nowMs()) || feed) changed();
        });
    m_proxy->uponSignal("TransferFinished")
        .onInterface(kInterface)
        .call([this](const std::string& transfer, const std::string& status, const std::vector<std::string>& paths) {
          const bool listed = m_activity.finished(transfer, status, paths, nowMs());
          const std::string key = "o:" + transfer;
          const bool moved = status == "done"       ? m_feed.moveTransfer(key, Transfer::Done)
                             : status == "declined" ? m_feed.remove(key)
                                                    : m_feed.moveTransfer(key, Transfer::Failed);
          if (moved || listed) changed();
        });
    refresh();
    refreshNearby();
  }

  LinkClient::~LinkClient() {
    std::error_code error;
    std::filesystem::create_directories(snapshotFile().parent_path(), error);
    std::ofstream(snapshotFile(), std::ios::binary | std::ios::trunc) << encodeSnapshot(m_phones, m_feed, m_dismissed);
  }

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
          // The daemon closes a window on expiry or CancelPairing by clearing Pairing, with no signal.
          const bool pairing = property<bool>(props, "Pairing");
          if (m_pairing && !pairing) (void)pairStep(m_pair, {.kind = PairEvent::Kind::Closed});
          m_pairing = pairing;
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
          // Texts and Contacts of the phone presence() shows; Link1 keeps its newest 256 texts.
          const std::string phone = presence().phone.id;
          using Part = sdbus::Struct<std::uint64_t, std::string>;
          using Sms = sdbus::Struct<std::uint64_t, std::uint64_t, std::string, std::string, std::uint64_t, std::uint8_t,
                                    std::vector<Part>>;
          const auto texts = property<std::map<std::string, std::vector<Sms>>>(props, "Texts");
          std::vector<Text> list;
          if (const auto it = texts.find(phone); it != texts.end()) {
            list.reserve(it->second.size());
            for (const Sms& t : it->second) {
              const auto status = t.get<5>() <= static_cast<std::uint8_t>(Text::Status::Failed)
                                      ? static_cast<Text::Status>(t.get<5>())
                                      : Text::Status::Sent;
              Text text{.id = t.get<0>(), .thread = t.get<1>(), .address = t.get<2>(), .body = t.get<3>(),
                        .atMs = static_cast<std::int64_t>(t.get<4>()), .status = status};
              for (const Part& part : t.get<6>()) {
                if (part.get<1>().starts_with("image/")) text.images.push_back(part.get<0>());
              }
              list.push_back(std::move(text));
            }
          }
          using Person = sdbus::Struct<std::string, std::vector<std::string>>;
          const auto contacts = property<std::map<std::string, std::vector<Person>>>(props, "Contacts");
          std::vector<Contact> people;
          if (const auto it = contacts.find(phone); it != contacts.end()) {
            people.reserve(it->second.size());
            for (const Person& c : it->second) people.push_back({.name = c.get<0>(), .numbers = c.get<1>()});
          }
          m_calls.clear();
          const auto log = property<std::map<std::string, std::vector<Call>>>(props, "Calls");
          if (const auto it = log.find(phone); it != log.end()) {
            m_calls.reserve(it->second.size());
            for (const Call& c : it->second) {
              const std::string& kind = c.get<4>();
              m_calls.push_back({.number = c.get<0>(), .name = c.get<1>(), .atMs = static_cast<std::int64_t>(c.get<2>()),
                                 .seconds = c.get<3>(),
                                 .kind = kind == "missed"     ? CallEntry::Kind::Missed
                                         : kind == "outgoing" ? CallEntry::Kind::Outgoing
                                                              : CallEntry::Kind::Incoming});
            }
          }
          const auto refused = property<std::map<std::string, std::string>>(props, "CallsRefused");
          const auto why = refused.find(phone);
          m_callLog = callLogFrom(why == refused.end() ? std::string() : why->second);
          const auto grants = property<std::map<std::string, std::vector<std::string>>>(props, "Grants");
          const auto granted = [&](const char* feature) {
            const auto it = grants.find(phone);
            return it != grants.end() && std::ranges::contains(it->second, std::string(feature));
          };
          m_phoneSettings = {.notifications = granted("notifications"), .clipboard = granted("clipboard"),
                             .screen = granted("screen"),
                             .nearby = granted("nearby"), .commands = granted("commands"),
                             .files = granted("files"), .media = granted("media"), .input = granted("input"),
                             .system = granted("system"), .ring = granted("ring"), .calls = granted("calls"),
                             .autoAccept = std::ranges::contains(property<std::vector<std::string>>(props, "AutoAccept"), phone),
                             .notificationsOff = std::ranges::contains(property<std::vector<std::string>>(props, "NotificationsOff"), phone),
                             .inputBlocked = granted("input") && !std::ranges::contains(
                                 property<std::vector<std::string>>(props, "SessionCapabilities"), std::string("input"))};
          m_desktopSettings.notificationsWhileLocked = property<bool>(props, "NotificationsWhileLocked");
          m_desktopSettings.localSendVisible = property<bool>(props, "LocalSendVisible");
          m_desktopSettings.commands.clear();
          for (const auto& [name, line] : property<std::vector<sdbus::Struct<std::string, std::string>>>(props, "Commands"))
            m_desktopSettings.commands.push_back({.name = name, .line = line});
          m_messages.setContacts(people);
          m_messages.setTexts(std::move(list));
          const Presence shown = presence();
          if (shown.kind == Presence::Kind::Connected &&
              (m_photos.load() == Photos::Load::Idle || m_photosDevice != shown.phone.id)) {
            loadPhotos();
          }
          changed();
        });
  }

  void LinkClient::refreshNearby() {
    const std::uint64_t serial = ++m_nearbySent;
    m_proxy->callMethodAsync("GetAll")
        .onInterface("org.freedesktop.DBus.Properties")
        .withArguments(std::string{kQuickShare})
        .uponReplyInvoke([this, serial, alive = std::weak_ptr<bool>(m_alive)](
                             std::optional<sdbus::Error> error, std::map<std::string, sdbus::Variant> props) {
          if (alive.expired() || serial <= m_nearbyApplied || error) return;
          m_nearbyApplied = serial;
          m_desktopSettings.nearbyVisible = property<bool>(props, "Visible");
          m_desktopSettings.nearbyName = property<std::string>(props, "Name");
          changed();
        });
  }

  void LinkClient::answerHotspot(bool allow, bool always) {
    if (!m_hotspotAsk) return;
    const std::string device = std::exchange(m_hotspotAsk, std::nullopt)->deviceId;
    if (always) setGrant(device, "nearby", true);
    m_proxy->callMethodAsync("AnswerHotspot").onInterface(kInterface).withArguments(device, allow)
        .uponReplyInvoke([](std::optional<sdbus::Error> error) {
          if (error) kLog.warn("AnswerHotspot: {}", error->getMessage());
        });
    changed();
  }

  void LinkClient::setCommands(const std::vector<Command>& commands, std::function<void(std::optional<std::string>)> done) {
    std::vector<sdbus::Struct<std::string, std::string>> list;
    for (const Command& c : commands) list.emplace_back(c.name, c.line);
    m_proxy->callMethodAsync("SetCommands").onInterface(kInterface).withArguments(list)
        .uponReplyInvoke([done = std::move(done)](std::optional<sdbus::Error> error) {
          if (error) kLog.warn("SetCommands: {}", error->getMessage());
          done(error ? std::optional<std::string>(error->getMessage()) : std::nullopt);
        });
  }

  void LinkClient::setGrant(const std::string& deviceId, const std::string& feature, bool granted) {
    m_proxy->callMethodAsync("SetGrant").onInterface(kInterface).withArguments(deviceId, feature, granted)
        .uponReplyInvoke([](std::optional<sdbus::Error> error) {
          if (error) kLog.warn("SetGrant: {}", error->getMessage());
        });
  }

  void LinkClient::setAutoAccept(const std::string& deviceId, bool on) {
    m_proxy->callMethodAsync("SetAutoAccept").onInterface(kInterface).withArguments(deviceId, on)
        .uponReplyInvoke([](std::optional<sdbus::Error> error) {
          if (error) kLog.warn("SetAutoAccept: {}", error->getMessage());
        });
  }

  void LinkClient::setNotificationsWhileLocked(bool on) {
    m_proxy->callMethodAsync("SetNotificationsWhileLocked").onInterface(kInterface).withArguments(on)
        .uponReplyInvoke([](std::optional<sdbus::Error> error) {
          if (error) kLog.warn("SetNotificationsWhileLocked: {}", error->getMessage());
        });
  }

  void LinkClient::setLocalSendVisible(bool on) {
    m_proxy->callMethodAsync("SetLocalSendVisible").onInterface(kInterface).withArguments(on)
        .uponReplyInvoke([](std::optional<sdbus::Error> error) {
          if (error) kLog.warn("SetLocalSendVisible: {}", error->getMessage());
        });
  }

  void LinkClient::setNearbyVisible(bool on) {
    m_proxy->callMethodAsync("Set").onInterface("org.freedesktop.DBus.Properties")
        .withArguments(std::string{kQuickShare}, std::string("Visible"), sdbus::Variant(on))
        .uponReplyInvoke([](std::optional<sdbus::Error> error) {
          if (error) kLog.warn("QuickShare Visible: {}", error->getMessage());
        });
  }

  void LinkClient::unpair(const std::string& deviceId) {
    m_proxy->callMethodAsync("Unpair").onInterface(kInterface).withArguments(deviceId)
        .uponReplyInvoke([](std::optional<sdbus::Error> error) {
          if (error) kLog.warn("Unpair: {}", error->getMessage());
        });
  }

  void LinkClient::viewThread(std::optional<std::uint64_t> thread) { m_messages.view(thread); }

  void LinkClient::sendText(std::uint64_t thread, const std::string& body) {
    const std::uint64_t serial = m_messages.send(thread, body, nowMs());
    if (serial == 0) return;
    send(serial);
    changed();
  }

  void LinkClient::retryText(std::uint64_t serial) {
    if (!m_messages.retry(serial)) return;
    send(serial);
    changed();
  }

  void LinkClient::send(std::uint64_t serial) {
    const Outgoing* out = m_messages.outgoing(serial);
    const Thread* thread = out ? m_messages.thread(out->thread) : nullptr;
    if (thread == nullptr) return;
    m_proxy->callMethodAsync("SendText").onInterface(kInterface).withArguments(presence().phone.id, thread->address, out->body)
        .uponReplyInvoke([this, serial, alive = std::weak_ptr<bool>(m_alive)](std::optional<sdbus::Error> error) {
          if (alive.expired() || !error) return;
          kLog.warn("SendText: {}", error->getMessage());
          if (m_messages.sendFailed(serial)) changed();
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
      dial(item.deviceId, actionId);
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
    if (item.kind == FeedItem::Kind::Photos) markPhotosSeen();
    if (m_feed.remove(item.key) || item.kind == FeedItem::Kind::Photos) changed();
  }

  void LinkClient::accept(const FeedItem& offer) { acceptTransfer(offer.key.substr(2)); }

  void LinkClient::decline(const FeedItem& offer) { declineTransfer(offer.key.substr(2)); }

  void LinkClient::acceptTransfer(const std::string& id) {
    m_proxy->callMethodAsync("AcceptTransfer").onInterface(kInterface).withArguments(id)
        .uponReplyInvoke([](std::optional<sdbus::Error> error) {
          if (error) kLog.warn("AcceptTransfer: {}", error->getMessage());
        });
  }

  // TransferFinished "declined" removes the Activity entry; the Waiting item leaves now.
  void LinkClient::declineTransfer(const std::string& id) {
    m_proxy->callMethodAsync("DeclineTransfer").onInterface(kInterface).withArguments(id)
        .uponReplyInvoke([](std::optional<sdbus::Error> error) {
          if (error) kLog.warn("DeclineTransfer: {}", error->getMessage());
        });
    if (m_feed.remove("o:" + id)) changed();
  }

  void LinkClient::cancelTransfer(const std::string& id) {
    m_proxy->callMethodAsync("CancelTransfer").onInterface(kInterface).withArguments(id)
        .uponReplyInvoke([](std::optional<sdbus::Error> error) {
          if (error) kLog.warn("CancelTransfer: {}", error->getMessage());
        });
  }

  void LinkClient::clearFinished() {
    m_activity.clearFinished();
    changed();
  }

  void LinkClient::showInFolder(const std::vector<std::string>& paths) {
    std::vector<std::string> uris;
    for (const std::string& path : paths) {
      std::string uri = "file://";
      for (const unsigned char c : path) {
        if (std::isalnum(c) != 0 || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
          uri += static_cast<char>(c);
        } else {
          uri += std::format("%{:02X}", c);
        }
      }
      uris.push_back(std::move(uri));
    }
    if (!m_fileManager) {
      m_fileManager = sdbus::createProxy(m_bus.connection(), sdbus::ServiceName{"org.freedesktop.FileManager1"},
                                         sdbus::ObjectPath{"/org/freedesktop/FileManager1"});
    }
    m_fileManager->callMethodAsync("ShowItems").onInterface("org.freedesktop.FileManager1").withArguments(uris, std::string())
        .uponReplyInvoke([](std::optional<sdbus::Error> error) {
          if (error) kLog.warn("ShowItems: {}", error->getMessage());
        });
  }

  void LinkClient::loadPhotos() {
    const std::string device = presence().phone.id;
    if (device.empty()) return;
    if (device != m_photosDevice) {
      clearThumbs();
      (void)m_feed.remove("p:new");
      m_photosSeenS = 0;
      std::ifstream(photosSeenFile(device)) >> m_photosSeenS;
    }
    m_photosDevice = device;
    if (const auto ask = m_photos.startLoad()) askPhotos(*ask);
  }

  void LinkClient::loadMorePhotos() {
    if (m_photosDevice.empty() || m_photosDevice != presence().phone.id) return;
    if (const auto ask = m_photos.startMore()) askPhotos(*ask);
  }

  void LinkClient::setPhotoMedia(Photos::Media media) {
    if (m_photos.setMedia(media)) loadPhotos();
    changed();
  }

  void LinkClient::askPhotos(const Photos::Ask& ask) {
    static constexpr std::array<const char*, 3> kMedia{"all", "photos", "videos"};
    using Row = sdbus::Struct<std::string, std::string, std::uint64_t, bool, std::uint32_t>;
    m_proxy->callMethodAsync("PhotosPage")
        .onInterface(kInterface)
        .withArguments(m_photosDevice, ask.offset, Photos::kCount, std::string(kMedia[static_cast<std::size_t>(ask.media)]))
        .uponReplyInvoke([this, ask, alive = std::weak_ptr<bool>(m_alive)](std::optional<sdbus::Error> error,
                                                                            std::uint32_t total, std::vector<Row> list) {
          if (alive.expired()) return;
          if (error) {
            kLog.warn("PhotosPage: {}", error->getMessage());
            const bool refused = error->getName().ends_with(".Refused");
            if (m_photos.failed(ask.serial, !refused                             ? Photos::Load::Failed
                                            : error->getMessage() == "denied" ? Photos::Load::Denied
                                                                              : Photos::Load::Unsupported)) {
              changed();
            }
            return;
          }
          std::vector<Photo> page;
          page.reserve(list.size());
          for (const auto& p : list) {
            page.push_back({.path = p.get<0>(),
                            .name = p.get<1>(),
                            .atS = static_cast<std::int64_t>(p.get<2>()),
                            .videoS = p.get<3>() ? std::optional(p.get<4>()) : std::nullopt});
          }
          if (!m_photos.loaded(ask.serial, total, std::move(page))) return;
          // The Waiting strip counts the newest items of a first page that holds photos.
          if (ask.offset == 0 && ask.media != Photos::Media::Videos) {
            if (auto item = newPhotosItem(m_photosDevice, m_photos.items(), m_photosSeenS, nowMs())) {
              m_feed.upsert(std::move(*item));
            } else {
              (void)m_feed.remove("p:new");
            }
          }
          // A page whose names were all dropped draws no tile that could ask for the next one.
          if (m_photos.items().size() < Photos::kCount) loadMorePhotos();
          changed();
        });
  }

  void LinkClient::clearThumbs() {
    m_thumbs.clear();
    m_thumbUse.clear();
    m_thumbBytes = 0;
  }

  void LinkClient::markPhotosSeen() {
    const std::vector<Photo>& photos = m_photos.items();
    if (!photos.empty() && photos.front().atS > m_photosSeenS) {
      m_photosSeenS = photos.front().atS;
      std::error_code error;
      std::filesystem::create_directories(photosSeenFile(m_photosDevice).parent_path(), error);
      std::ofstream(photosSeenFile(m_photosDevice)) << m_photosSeenS << '\n';
    }
    (void)m_feed.remove("p:new");
  }

  void LinkClient::togglePhoto(std::size_t index) {
    m_photos.toggle(index);
    changed();
  }

  void LinkClient::clearPhotoSelection() {
    m_photos.clearSelection();
    changed();
  }

  void LinkClient::savePhotos() {
    const std::vector<std::string> paths = m_photos.startSave();
    for (const std::string& path : paths) {
      m_proxy->callMethodAsync("SavePhoto")
          .onInterface(kInterface)
          .withArguments(m_photosDevice, path)
          .uponReplyInvoke([this, alive = std::weak_ptr<bool>(m_alive)](std::optional<sdbus::Error> error, std::string) {
            if (alive.expired()) return;
            if (error) kLog.warn("SavePhoto: {}", error->getMessage());
            m_photos.saveAnswered(!error);
            changed();
          });
    }
    if (!paths.empty()) changed();
  }

  void LinkClient::savePhotoTo(const std::string& path, const std::string& folder,
                               std::function<void(const std::string&)> done) {
    m_proxy->callMethodAsync("SavePhotoTo")
        .onInterface(kInterface)
        .withArguments(m_photosDevice, path, folder)
        .uponReplyInvoke([done = std::move(done)](std::optional<sdbus::Error> error, std::string saved) {
          if (error) kLog.warn("SavePhotoTo: {}", error->getMessage());
          done(error ? std::string() : saved);
        });
  }

  const std::vector<std::uint8_t>* LinkClient::thumbnail(const std::string& path) {
    return cachedImage(path, [&] {
      return m_proxy->callMethodAsync("Thumbnail").onInterface(kInterface).withArguments(m_photosDevice, path, std::uint16_t{256});
    });
  }

  const std::vector<std::uint8_t>* LinkClient::textImage(std::uint64_t part) {
    const std::string device = presence().phone.id;
    if (device.empty()) return nullptr;
    // Twice the bubble's 240 x 160, for 2x scales.
    return cachedImage(std::format("mms:{}:{}", device, part), [&] {
      return m_proxy->callMethodAsync("TextImage").onInterface(kInterface).withArguments(device, part, std::uint16_t{480});
    });
  }

  template <typename Ask> const std::vector<std::uint8_t>* LinkClient::cachedImage(const std::string& path, Ask ask) {
    if (const auto it = m_thumbs.find(path); it != m_thumbs.end()) {
      m_thumbUse.splice(m_thumbUse.begin(), m_thumbUse, it->second.use);
      return it->second.png.empty() ? nullptr : &it->second.png;
    }
    m_thumbUse.push_front(path);
    m_thumbs.emplace(path, Thumb{.png = {}, .use = m_thumbUse.begin()});
    ask().uponReplyInvoke([this, path, alive = std::weak_ptr<bool>(m_alive)](std::optional<sdbus::Error> error,
                                                                             std::vector<std::uint8_t> png) {
          if (alive.expired()) return;
          const auto entry = m_thumbs.find(path);
          if (entry == m_thumbs.end()) return;
          if (error) {
            kLog.debug("image {}: {}", path, error->getMessage());
            m_thumbUse.erase(entry->second.use);
            m_thumbs.erase(entry); // the next bind of its tile asks again
            return;
          }
          m_thumbBytes += png.size();
          entry->second.png = std::move(png);
          while (m_thumbBytes > kThumbBytes && m_thumbUse.size() > 1) {
            const auto old = m_thumbs.find(m_thumbUse.back());
            m_thumbBytes -= old->second.png.size();
            m_thumbs.erase(old);
            m_thumbUse.pop_back();
          }
          if (m_thumbnailReady) m_thumbnailReady();
        });
    return nullptr;
  }

  void LinkClient::openFolder(const std::string& path) {
    const std::string device = presence().phone.id;
    if (device.empty()) return;
    const std::uint64_t serial = m_files.go(path);
    changed();
    m_proxy->callMethodAsync("ListFiles")
        .onInterface(kInterface)
        .withArguments(device, path)
        .uponReplyInvoke([this, serial, alive = std::weak_ptr<bool>(m_alive)](
                             std::optional<sdbus::Error> error, std::vector<sdbus::Struct<std::string, bool, std::uint64_t, std::uint64_t>> list) {
          if (alive.expired()) return;
          if (error) {
            const std::string why = error->getName().ends_with(".Refused") ? error->getMessage() : std::string();
            kLog.warn("ListFiles: {}", error->getMessage());
            const std::string text = why == "not-allowed" || why == "denied"
                                         ? "Turn on Files for this desktop in Link on the phone."
                                     : why == "not-found" ? "This folder is no longer on the phone."
                                     : why == "busy"      ? "The phone is busy. Try again in a moment."
                                                          : "The phone did not answer.";
            if (m_files.failed(serial, text)) changed();
            return;
          }
          std::vector<FileEntry> entries;
          entries.reserve(list.size());
          for (const auto& e : list) {
            entries.push_back({.name = e.get<0>(), .folder = e.get<1>(), .size = e.get<2>(),
                               .mtimeS = static_cast<std::int64_t>(e.get<3>())});
          }
          if (m_files.listed(serial, std::move(entries))) changed();
        });
  }

  struct LinkClient::Download {
    std::string device;
    std::string phonePath;
    std::string local;
    struct Close {
      void operator()(std::FILE* file) const { std::fclose(file); }
    };
    std::unique_ptr<std::FILE, Close> file;
    std::uint64_t got = 0;
    std::chrono::steady_clock::time_point shown;
  };

  void LinkClient::saveFile(const std::string& name) {
    const auto entry = std::ranges::find(m_files.entries(), name, &FileEntry::name);
    const std::string device = presence().phone.id;
    if (entry == m_files.entries().end() || entry->folder || device.empty()) return;
    const std::string phonePath = m_files.childPath(name);
    if (!m_files.saveStarted(phonePath, entry->size)) return;
    auto download = std::make_shared<Download>();
    download->device = device;
    download->phonePath = phonePath;
    // A free name in the downloads folder: "name", then "name (1).ext", ...
    const std::filesystem::path dir = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD) != nullptr
                                          ? g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD)
                                          : std::filesystem::path(g_get_home_dir()) / "Downloads";
    const std::filesystem::path base(name);
    for (int n = 0; n < 1000 && !download->file; ++n) {
      const std::string candidate =
          n == 0 ? name : std::format("{} ({}){}", base.stem().string(), n, base.extension().string());
      download->local = (dir / candidate).string();
      download->file.reset(std::fopen(download->local.c_str(), "wbx"));
      if (!download->file && errno != EEXIST) break;
    }
    if (!download->file) {
      kLog.warn("save {}: cannot create a file in {}", name, dir.string());
      (void)m_files.saveFailed(phonePath);
      changed();
      return;
    }
    changed();
    readChunk(download);
  }

  void LinkClient::readChunk(const std::shared_ptr<Download>& download) {
    constexpr std::uint32_t kChunk = 1024 * 1024; // Link1 ReadFile's largest read
    m_proxy->callMethodAsync("ReadFile")
        .onInterface(kInterface)
        .withArguments(download->device, download->phonePath, download->got, kChunk)
        .uponReplyInvoke([this, download, alive = std::weak_ptr<bool>(m_alive)](std::optional<sdbus::Error> error,
                                                                                 std::vector<std::uint8_t> data) {
          if (alive.expired()) return;
          const bool wrote = !error && std::fwrite(data.data(), 1, data.size(), download->file.get()) == data.size();
          if (!wrote) {
            if (error) kLog.warn("ReadFile: {}", error->getMessage());
            download->file.reset();
            std::remove(download->local.c_str());
            (void)m_files.saveFailed(download->phonePath);
            changed();
            return;
          }
          download->got += data.size();
          if (data.size() < kChunk) {
            const bool closed = std::fclose(download->file.release()) == 0;
            if (closed) {
              (void)m_files.saveDone(download->phonePath, download->local);
            } else {
              std::remove(download->local.c_str());
              (void)m_files.saveFailed(download->phonePath);
            }
            changed();
            return;
          }
          (void)m_files.saveProgress(download->phonePath, download->got);
          // Progress repaints at most 4 times a second (rulebook §16).
          const auto now = std::chrono::steady_clock::now();
          if (now - download->shown >= std::chrono::milliseconds(250)) {
            download->shown = now;
            changed();
          }
          readChunk(download);
        });
  }

  void LinkClient::startMirror() {
    const std::string device = presence().phone.id;
    if (device.empty() || !mirrorStep(m_mirror, {.kind = MirrorEvent::Kind::Start})) return;
    m_mirrorDevice = device;
    m_mirrorCorner = 0;
    const std::uint64_t session = m_mirror.session;
    changed();
    // The phone's user has 60 s to allow capture; the default D-Bus timeout is shorter.
    m_proxy->callMethodAsync("OpenMirror")
        .onInterface(kInterface)
        .withArguments(device)
        .withTimeout(std::chrono::seconds(70))
        .uponReplyInvoke([this, session, alive = std::weak_ptr<bool>(m_alive)](std::optional<sdbus::Error> error, sdbus::UnixFd fd,
                                                                                std::uint32_t width, std::uint32_t height) {
          if (alive.expired()) return;
          if (error) {
            kLog.warn("OpenMirror: {}", error->getMessage());
            const std::string why = error->getName().ends_with(".Refused") ? "The phone did not allow screen sharing."
                                    : error->getName().ends_with(".NotConnected") ? "The phone is not connected."
                                                                                  : "The phone did not start sharing its screen.";
            if (mirrorStep(m_mirror, {.kind = MirrorEvent::Kind::Failed, .session = session, .error = why})) changed();
            return;
          }
          if (!mirrorStep(m_mirror, {.kind = MirrorEvent::Kind::Opened, .session = session, .width = width, .height = height})) {
            return; // stopped or restarted meanwhile: fd closes here
          }
          m_decoder = std::make_unique<MirrorDecoder>(
              fd.release(),
              [this](MirrorDecoder::Frame frame) {
                if (m_mirrorFrame) m_mirrorFrame(std::move(frame));
              },
              [this, session](std::string why) {
                const MirrorEvent event = why.empty() ? MirrorEvent{.kind = MirrorEvent::Kind::Ended, .session = session}
                                                      : MirrorEvent{.kind = MirrorEvent::Kind::Failed, .session = session, .error = why};
                if (mirrorStep(m_mirror, event)) {
                  m_decoder.reset();
                  changed();
                }
              });
          changed();
        });
  }

  void LinkClient::stopMirror() {
    if (!mirrorStep(m_mirror, {.kind = MirrorEvent::Kind::Stop})) return;
    m_decoder.reset();
    changed();
  }

  void LinkClient::mirrorAction(const std::string& action) {
    if (m_mirror.kind != Mirror::Kind::Showing) return;
    m_proxy->callMethodAsync("MirrorInput")
        .onInterface(kInterface)
        .withArguments(m_mirrorDevice, action, std::map<std::string, sdbus::Variant>{})
        .uponReplyInvoke([action](std::optional<sdbus::Error> error) {
          if (error) kLog.warn("MirrorInput {}: {}", action, error->getMessage());
        });
  }

  void LinkClient::mirrorKey(std::int32_t code) {
    if (m_mirror.kind != Mirror::Kind::Showing) return;
    for (const char* action : {"key-down", "key-up"}) {
      m_proxy->callMethodAsync("MirrorInput")
          .onInterface(kInterface)
          .withArguments(m_mirrorDevice, std::string(action),
                         std::map<std::string, sdbus::Variant>{{"code", sdbus::Variant(code)}, {"meta", sdbus::Variant(std::int32_t{0})}})
          .uponReplyInvoke([](std::optional<sdbus::Error> error) {
            if (error) kLog.warn("MirrorInput key: {}", error->getMessage());
          });
    }
  }

  void LinkClient::mirrorTouch(const std::string& action, std::int32_t x, std::int32_t y) {
    if (m_mirror.kind != Mirror::Kind::Showing) return;
    m_proxy->callMethodAsync("MirrorInput")
        .onInterface(kInterface)
        .withArguments(m_mirrorDevice, action, std::map<std::string, sdbus::Variant>{{"x", sdbus::Variant(x)}, {"y", sdbus::Variant(y)}})
        .uponReplyInvoke([](std::optional<sdbus::Error> error) {
          if (error) kLog.warn("MirrorInput touch: {}", error->getMessage());
        });
  }

  void LinkClient::setMirrorTarget(int width, int height) {
    if (m_decoder) m_decoder->setTarget(width, height);
  }

  void LinkClient::startPairing() {
    if (!pairStep(m_pair, {.kind = PairEvent::Kind::Start})) return;
    m_proxy->callMethodAsync("StartPairing")
        .onInterface(kInterface)
        .uponReplyInvoke([this, alive = std::weak_ptr<bool>(m_alive)](std::optional<sdbus::Error> error, std::string code,
                                                                       std::string uri) {
          if (alive.expired()) return;
          if (error) {
            kLog.warn("StartPairing: {}", error->getMessage());
            (void)pairStep(m_pair, {.kind = PairEvent::Kind::Failed, .error = error->getMessage()});
          } else {
            (void)pairStep(m_pair, {.kind = PairEvent::Kind::Started, .code = std::move(code), .uri = std::move(uri)});
          }
          changed();
        });
  }

  void LinkClient::connect(const std::string& deviceId) {
    m_proxy->callMethodAsync("Connect").onInterface(kInterface).withArguments(deviceId)
        .uponReplyInvoke([](std::optional<sdbus::Error> error) {
          if (error) kLog.warn("Connect: {}", error->getMessage());
        });
  }

  void LinkClient::dial(const std::string& deviceId, const std::string& number) {
    m_proxy->callMethodAsync("Dial").onInterface(kInterface).withArguments(deviceId, number)
        .uponReplyInvoke([](std::optional<sdbus::Error> error) {
          if (error) kLog.warn("Dial: {}", error->getMessage());
        });
  }

  void LinkClient::ring(const std::string& deviceId) {
    m_proxy->callMethodAsync("Ring").onInterface(kInterface).withArguments(deviceId, true)
        .uponReplyInvoke([](std::optional<sdbus::Error> error) {
          if (error) kLog.warn("Ring: {}", error->getMessage());
        });
  }

  void LinkClient::select(const std::string& deviceId) {
    if (deviceId == m_selected) return;
    m_selected = deviceId;
    std::error_code error;
    std::filesystem::create_directories(selectedFile().parent_path(), error);
    std::ofstream(selectedFile()) << deviceId << '\n';
    refresh();
    changed();
  }

  void LinkClient::sendTo(const std::string& number, const std::string& body) {
    m_proxy->callMethodAsync("SendText").onInterface(kInterface).withArguments(presence().phone.id, number, body)
        .uponReplyInvoke([](std::optional<sdbus::Error> error) {
          if (error) kLog.warn("SendText: {}", error->getMessage());
        });
  }

  void LinkClient::shareText(const std::string& deviceId, const std::string& text) {
    const bool link = (text.starts_with("http://") || text.starts_with("https://")) &&
                      std::ranges::none_of(text, [](unsigned char c) { return std::isspace(c) != 0; });
    m_proxy->callMethodAsync("Share").onInterface(kInterface)
        .withArguments(deviceId, std::string(link ? "link" : "text"), text)
        .uponReplyInvoke([](std::optional<sdbus::Error> error) {
          if (error) kLog.warn("Share: {}", error->getMessage());
        });
  }

  void LinkClient::sendFiles(const std::string& deviceId, const std::vector<std::filesystem::path>& paths) {
    std::vector<sdbus::Struct<sdbus::UnixFd, std::string>> files;
    for (const auto& path : paths) {
      const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY);
      if (fd < 0) {
        kLog.warn("SendFiles: open {}: {}", path.string(), std::strerror(errno));
        continue;
      }
      files.emplace_back(sdbus::UnixFd{fd, sdbus::adopt_fd}, path.filename().string());
    }
    if (files.empty()) return;
    m_proxy->callMethodAsync("SendFiles").onInterface(kInterface).withArguments(deviceId, files)
        .uponReplyInvoke([](std::optional<sdbus::Error> error, std::string /*transfer*/) {
          if (error) kLog.warn("SendFiles: {}", error->getMessage());
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
