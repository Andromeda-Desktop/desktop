#include "dbus/remote_desktop.h"

#include "dbus/caller.h"
#include "dbus/name_owner_watch.h"
#include "dbus/request.h"
#include "dbus/screencast.h"
#include "dbus/session.h"
#include "dbus/session_table.h"
#include "loop/loop.h"
#include "protocols/bus_names.h"
#include "wayland/wayland.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <libei.h>
#include <linux/input-event-codes.h>
#include <map>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>
#include <set>
#include <string>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon.h>

namespace xdpu {

  namespace {

    constexpr char kInterface[] = "org.freedesktop.impl.portal.RemoteDesktop";
    constexpr char kClipboardInterface[] = "org.freedesktop.impl.portal.Clipboard";
    constexpr char kSessionPrefix[] = "/org/freedesktop/portal/desktop/session/";
    constexpr char kRestoreVendor[] = "andromeda-rd";
    // Version 1 data: a{sv} with "token" (s, the compositor grant) and optional "sources" (ScreenCast restore data).
    constexpr uint32_t kRestoreVersion = 1;
    // Portal device types; the compositor capability bits use the same values.
    constexpr uint32_t kKeyboard = 1;
    constexpr uint32_t kPointer = 2;
    constexpr uint32_t kAvailableDeviceTypes = kKeyboard | kPointer;
    // Compositor capability bit: the consent also covers the session's clipboard.
    constexpr uint32_t kClipboard = 4;
    // SetSelection limits, and transfers an app may leave unanswered per session (the oldest closes first).
    constexpr size_t kMaxMimeTypes = 256;
    constexpr size_t kMaxMimeTypeLength = 4095;
    constexpr size_t kMaxTransfers = 16;
    // Events an app sends before the EIS handshake ends wait here; more are dropped.
    constexpr size_t kMaxQueued = 512;

    using RestoreData = sdbus::Struct<std::string, uint32_t, sdbus::Variant>;

    // The frontend's escaped sender in /org/freedesktop/portal/desktop/session/<sender>/<token>; the path otherwise.
    std::string applicationInstance(const std::string& path) {
      if (!path.starts_with(kSessionPrefix)) {
        return path;
      }
      const std::string rest = path.substr(sizeof(kSessionPrefix) - 1);
      const size_t slash = rest.find('/');
      return slash == std::string::npos || slash == 0 ? path : rest.substr(0, slash);
    }

    // A portal-owned libei sender on one connect_eis fd: Notify* calls become EIS events.
    class EiSender {
    public:
      using Op = std::function<void(EiSender&)>;

      EiSender(Loop& loop, uint32_t capabilities, std::function<void()> failed)
          : m_loop(loop), m_capabilities(capabilities), m_failed(std::move(failed)) {}

      ~EiSender() {
        if (m_watch != 0) {
          m_loop.removeFd(m_watch);
        }
        for (ei_device* device : m_devices) {
          ei_device_unref(device);
        }
        dropKeymap();
        if (m_xkb != nullptr) {
          xkb_context_unref(m_xkb);
        }
        if (m_context != nullptr) {
          ei_unref(m_context);
        }
      }

      EiSender(const EiSender&) = delete;
      EiSender& operator=(const EiSender&) = delete;

      // fd < 0: the compositor refused; queued and later events are dropped.
      void connect(int fd) {
        if (fd < 0) {
          fail("the compositor refused an EIS connection");
          return;
        }
        m_context = ei_new_sender(this);
        ei_configure_name(m_context, "xdg-desktop-portal-andromeda");
        if (ei_setup_backend_fd(m_context, fd) != 0) {
          fail("libei setup failed");
          return;
        }
        m_watch = m_loop.addFd(ei_get_fd(m_context), EPOLLIN, [this](uint32_t) { dispatch(); });
      }

      void submit(Op op) {
        switch (m_state) {
        case State::Connecting:
          if (m_queued.size() < kMaxQueued) {
            m_queued.push_back(std::move(op));
          }
          return;
        case State::Ready:
          op(*this);
          return;
        case State::Failed:
          return;
        }
      }

      // The emulating device with this capability; nullptr while paused (locked session, consent panel).
      ei_device* device(ei_device_capability capability) const {
        for (ei_device* device : m_devices) {
          if (ei_device_has_capability(device, capability) && m_emulating.contains(device)) {
            return device;
          }
        }
        return nullptr;
      }

      void frame(ei_device* device) { ei_device_frame(device, ei_now(m_context)); }

      // Moves to a point given as fractions of the output whose EIS region carries `mappingId` (the output name).
      // Region units are the compositor's (Xwayland pixels when it runs), so the fractions keep the stream scale out.
      void absolute(const std::string& mappingId, double fx, double fy) {
        ei_device* device = this->device(EI_DEVICE_CAP_POINTER_ABSOLUTE);
        if (device == nullptr) {
          return;
        }
        for (size_t i = 0; ei_region* region = ei_device_get_region(device, i); ++i) {
          const char* id = ei_region_get_mapping_id(region);
          if (id != nullptr && mappingId == id) {
            ei_device_pointer_motion_absolute(
                device, ei_region_get_x(region) + fx * ei_region_get_width(region),
                ei_region_get_y(region) + fy * ei_region_get_height(region)
            );
            frame(device);
            return;
          }
        }
      }

      // The evdev keycode and whether Shift produces the keysym (levels 0 and 1 of layout 0), from the EIS keyboard's
      // current keymap.
      std::optional<std::pair<uint32_t, bool>> keycodeFor(ei_device* keyboard, xkb_keysym_t keysym) {
        if (m_keymap == nullptr && !loadKeymap(keyboard)) {
          return std::nullopt;
        }
        const xkb_keycode_t min = xkb_keymap_min_keycode(m_keymap);
        const xkb_keycode_t max = xkb_keymap_max_keycode(m_keymap);
        for (xkb_level_index_t level = 0; level < 2; ++level) {
          for (xkb_keycode_t code = min; code <= max; ++code) {
            const xkb_keysym_t* syms = nullptr;
            const int count = xkb_keymap_key_get_syms_by_level(m_keymap, code, 0, level, &syms);
            for (int i = 0; i < count; ++i) {
              if (syms[i] == keysym && code >= 8) {
                return std::pair{code - 8, level == 1};
              }
            }
          }
        }
        return std::nullopt;
      }

    private:
      enum class State : uint8_t { Connecting, Ready, Failed };

      void fail(const char* why) {
        if (m_state == State::Failed) {
          return;
        }
        std::fprintf(stderr, "remote desktop: %s\n", why);
        m_state = State::Failed;
        m_queued.clear();
        m_failed();
      }

      void dropKeymap() {
        if (m_keymap != nullptr) {
          xkb_keymap_unref(m_keymap);
          m_keymap = nullptr;
        }
      }

      bool loadKeymap(ei_device* keyboard) {
        ei_keymap* keymap = ei_device_keyboard_get_keymap(keyboard);
        if (keymap == nullptr || ei_keymap_get_type(keymap) != EI_KEYMAP_TYPE_XKB) {
          return false;
        }
        const size_t size = ei_keymap_get_size(keymap);
        void* data = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, ei_keymap_get_fd(keymap), 0);
        if (data == MAP_FAILED) {
          return false;
        }
        m_xkb = m_xkb != nullptr ? m_xkb : xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        m_keymap = xkb_keymap_new_from_string(
            m_xkb, static_cast<const char*>(data), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS
        );
        munmap(data, size);
        return m_keymap != nullptr;
      }

      bool hasEveryDevice() const {
        return ((m_capabilities & kKeyboard) == 0 || device(EI_DEVICE_CAP_KEYBOARD) != nullptr)
            && ((m_capabilities & kPointer) == 0 || device(EI_DEVICE_CAP_POINTER) != nullptr);
      }

      void dispatch() {
        ei_dispatch(m_context);
        while (ei_event* event = ei_get_event(m_context)) {
          ei_device* device = ei_event_get_device(event);
          switch (ei_event_get_type(event)) {
          case EI_EVENT_SEAT_ADDED:
            ei_seat_bind_capabilities(
                ei_event_get_seat(event), EI_DEVICE_CAP_POINTER, EI_DEVICE_CAP_POINTER_ABSOLUTE,
                EI_DEVICE_CAP_KEYBOARD, EI_DEVICE_CAP_BUTTON, EI_DEVICE_CAP_SCROLL, nullptr
            );
            break;
          case EI_EVENT_DEVICE_ADDED:
            m_devices.push_back(ei_device_ref(device));
            break;
          case EI_EVENT_DEVICE_REMOVED:
            // A keymap change replaces the keyboard device; the next keysym reads the new keymap.
            if (ei_device_has_capability(device, EI_DEVICE_CAP_KEYBOARD)) {
              dropKeymap();
            }
            m_emulating.erase(device);
            std::erase_if(m_devices, [device](ei_device* candidate) {
              if (candidate == device) {
                ei_device_unref(candidate);
                return true;
              }
              return false;
            });
            break;
          case EI_EVENT_DEVICE_RESUMED:
            ei_device_start_emulating(device, ++m_sequence);
            m_emulating.insert(device);
            break;
          case EI_EVENT_DEVICE_PAUSED:
            m_emulating.erase(device);
            break;
          case EI_EVENT_DISCONNECT:
            fail("the compositor ended the EIS connection");
            break;
          default:
            break;
          }
          ei_event_unref(event);
        }
        if (m_state == State::Connecting && hasEveryDevice()) {
          m_state = State::Ready;
          for (auto& op : std::exchange(m_queued, {})) {
            op(*this);
          }
        }
      }

      Loop& m_loop;
      uint32_t m_capabilities;
      std::function<void()> m_failed;
      State m_state = State::Connecting;
      ei* m_context = nullptr;
      int m_watch = 0;
      uint32_t m_sequence = 0;
      std::vector<ei_device*> m_devices;
      std::set<ei_device*> m_emulating;
      std::vector<Op> m_queued;
      xkb_context* m_xkb = nullptr;
      xkb_keymap* m_keymap = nullptr;
    };

  } // namespace

  struct RemoteDesktopPortal::Impl {
    // One RemoteDesktop session; the shared Session object holds its ScreenCast part and its owner.
    // Start: Open -> Consent (compositor panel or restored grant) -> Sources (when SelectSources ran) -> Started.
    struct Remote {
      enum class Phase : uint8_t { Open, Consent, Sources, Started, Ended };
      // Off -> Requested (RequestClipboard before Start) -> Enabled (the consent covered it). Only Enabled acts.
      enum class ClipboardAccess : uint8_t { Off, Requested, Enabled };

      std::string appId;
      std::string instance;
      std::string parentWindow;
      Phase phase = Phase::Open;
      uint32_t types = kAvailableDeviceTypes;
      uint32_t persistMode = 0;
      std::string restoreToken;
      std::vector<Session::Selection> restoreSources;
      uint32_t capabilities = 0;
      // Set by grant before started; empty when nothing was remembered.
      uint32_t grantMode = 0;
      std::string grantToken;
      std::shared_ptr<Request> request;
      std::move_only_function<void(uint32_t, PortalResults)> startReply;
      std::function<void()> cancelSources;
      std::unique_ptr<WaylandContext::RemoteInputSession> input;
      // Waiting connect_eis consumers by request id.
      std::map<uint32_t, std::move_only_function<void(int fd)>> connects;
      std::unique_ptr<EiSender> sender;
      ClipboardAccess clipboard = ClipboardAccess::Off;
      // The compositor blocks emulated input (lock, inactive session): no clipboard data moves.
      bool paused = false;
      // The own selection source (WaylandContext::Clipboard id), or 0.
      uint64_t source = 0;
    };

    // One answer the app owes for a SelectionTransfer: the reader's pipe until SelectionWrite hands it out.
    struct Transfer {
      std::string path;
      uint64_t source = 0;
      std::optional<sdbus::UnixFd> fd;
    };

    Loop& loop;
    sdbus::IConnection& connection;
    sdbus::IObject& object;
    SessionTable& sessions;
    ScreenCastPortal& screencast;
    WaylandContext& wayland;
    std::map<std::string, std::unique_ptr<Remote>> remotes;
    // Application instances that hold mode-one grants; their grants expire when the app leaves the bus.
    std::map<std::string, NameOwnerWatch> scopedInstances;
    // Exists while a session has clipboard access.
    std::unique_ptr<WaylandContext::Clipboard> clipboard;
    std::map<uint32_t, Transfer> transfers;
    uint32_t nextSerial = 1;

    Impl(
        Loop& loop, sdbus::IConnection& connection, sdbus::IObject& object, SessionTable& sessions,
        ScreenCastPortal& screencast, WaylandContext& wayland
    )
        : loop(loop), connection(connection), object(object), sessions(sessions), screencast(screencast),
          wayland(wayland) {
      sessions.addClosedListener([this](Session& session) { end(session.path()); });
      registerVTable();
      registerClipboard();
    }

    ~Impl() { remotes.clear(); }

    std::string sender() const { return object.getCurrentlyProcessedMessage().getSender(); }

    // The instance is the frontend's escaped unique name of the app (":1.42" -> "1_42").
    void watchScope(const std::string& instance) {
      if (scopedInstances.contains(instance) || instance.find('/') != std::string::npos) {
        return;
      }
      std::string name = ":" + instance;
      std::ranges::replace(name, '_', '.');
      scopedInstances.emplace(
          instance,
          NameOwnerWatch::changes(connection, name, [this, instance](const std::string&, const std::string&, const std::string& owner) {
            if (!owner.empty()) {
              return;
            }
            wayland.expireRemoteInputScope(instance);
            loop.addTimer(0, [this, instance]() { scopedInstances.erase(instance); });
          })
      );
    }

    // The session's state when the current call comes from the sender that created it.
    Remote* remoteFor(const std::string& path) {
      const auto it = remotes.find(path);
      return it == remotes.end() || !sessions.find(path, sender()) ? nullptr : it->second.get();
    }

    Remote* live(const std::string& path) {
      const auto it = remotes.find(path);
      return it == remotes.end() || it->second->phase == Remote::Phase::Ended ? nullptr : it->second.get();
    }

    void replyNow(PortalResponse&& result, const std::string& handle, uint32_t response, PortalResults results = {}) {
      auto request = std::make_shared<Request>(connection, handle, []() {});
      result.returnResults(response, results);
      loop.addTimer(0, [request = std::move(request)]() mutable { request.reset(); });
    }

    // Destroys the compositor session after the current Wayland event: its handlers may be running.
    void retire(std::unique_ptr<WaylandContext::RemoteInputSession> input) {
      if (input) {
        loop.addTimer(0, [input = std::shared_ptr(std::move(input))]() {});
      }
    }

    void createSession(
        PortalResponse&& result, const std::string& handle, const std::string& path, const std::string& appId
    ) {
      requirePortalFrontend(connection);
      if (!sessions.create(path, appId, sender())) {
        replyNow(std::move(result), handle, 2);
        return;
      }
      auto remote = std::make_unique<Remote>();
      remote->appId = appId;
      remote->instance = applicationInstance(path);
      remotes[path] = std::move(remote);
      replyNow(std::move(result), handle, 0);
    }

    void selectDevices(
        PortalResponse&& result, const std::string& handle, const std::string& path, const PortalResults& options
    ) {
      requirePortalFrontend(connection);
      Remote* remote = remoteFor(path);
      if (remote == nullptr || remote->phase != Remote::Phase::Open) {
        replyNow(std::move(result), handle, 2);
        return;
      }
      const uint32_t types = optionValue<uint32_t>(options, "types").value_or(kAvailableDeviceTypes);
      remote->types = (types & kAvailableDeviceTypes) == 0 ? kAvailableDeviceTypes : types & kAvailableDeviceTypes;
      remote->persistMode = std::min<uint32_t>(optionValue<uint32_t>(options, "persist_mode").value_or(0), 2);
      remote->restoreToken.clear();
      remote->restoreSources.clear();
      if (const auto restore = optionValue<RestoreData>(options, "restore_data")) {
        const auto& [vendor, version, data] = *restore;
        if (vendor == kRestoreVendor && version == kRestoreVersion && data.containsValueOfType<PortalResults>()) {
          const auto fields = data.get<PortalResults>();
          remote->restoreToken = optionValue<std::string>(fields, "token").value_or(std::string{});
          if (const auto sources = fields.find("sources"); sources != fields.end()) {
            remote->restoreSources = restoreSelections(sources->second);
          }
        }
      }
      replyNow(std::move(result), handle, 0);
    }

    void start(
        PortalResponse&& result, const std::string& handle, const std::string& path, const std::string& parentWindow
    ) {
      requirePortalFrontend(connection);
      Remote* remote = remoteFor(path);
      if (remote == nullptr || remote->phase != Remote::Phase::Open || !sessions.find(path)->beginStart()) {
        replyNow(std::move(result), handle, 2);
        return;
      }
      remote->parentWindow = parentWindow;
      remote->startReply = [result = std::move(result)](uint32_t response, PortalResults results) mutable {
        result.returnResults(response, results);
      };
      remote->request = std::make_shared<Request>(connection, handle, [this, path]() { cancelStart(path); });
      remote->phase = Remote::Phase::Consent;
      remote->input = wayland.createRemoteInputSession({
          .grant =
              [this, path](uint32_t mode, std::string token) {
                if (Remote* remote = live(path)) {
                  remote->grantMode = mode;
                  remote->grantToken = std::move(token);
                }
              },
          .started = [this, path](uint32_t status, uint32_t capabilities) { started(path, status, capabilities); },
          .paused = [this, path](bool paused) { pause(path, paused); },
          .eisFd = [this, path](uint32_t request, int fd) { answerConnect(path, request, fd); },
          .result = [this, path](uint32_t request, uint32_t) { answerConnect(path, request, -1); },
          .closed = [this, path]() { closeLater(path); },
      });
      if (!remote->input) {
        std::fprintf(stderr, "remote desktop: the compositor offers no andromeda_remote_input_manager_v1\n");
        finishStart(path, *remote, 2);
        return;
      }
      remote->input->setPersistence(remote->persistMode, remote->restoreToken, remote->instance);
      remote->input->start(
          remote->appId, remote->types | (remote->clipboard == Remote::ClipboardAccess::Requested ? kClipboard : 0)
      );
    }

    // The compositor answered: on success, share the screen next when the app also selected sources.
    void started(std::string path, uint32_t status, uint32_t capabilities) {
      Remote* remote = live(path);
      if (remote == nullptr || remote->phase != Remote::Phase::Consent) {
        return;
      }
      if (status != 0) {
        finishStart(path, *remote, status == 1 ? 1 : 2);
        return;
      }
      if (remote->grantMode == 1) {
        watchScope(remote->instance);
      }
      remote->capabilities = capabilities & remote->types;
      if (remote->clipboard == Remote::ClipboardAccess::Requested && (capabilities & kClipboard) != 0) {
        enableClipboard(*remote);
      }
      const auto session = sessions.find(path);
      if (!session || !session->sourcesSelected()) {
        finishStart(path, *remote, 0);
        return;
      }
      if (!remote->restoreSources.empty()) {
        session->setRestoreSelections(remote->restoreSources);
      }
      remote->phase = Remote::Phase::Sources;
      remote->cancelSources =
          screencast.startSources(session, remote->parentWindow, [this, path](uint32_t response, PortalResults results) {
            Remote* remote = live(path);
            if (remote == nullptr || remote->phase != Remote::Phase::Sources) {
              return;
            }
            remote->cancelSources = nullptr;
            PortalResults streams;
            if (const auto found = results.find("streams"); found != results.end()) {
              streams.emplace("streams", found->second);
            }
            finishStart(path, *remote, response, std::move(streams));
          });
    }

    void finishStart(const std::string& path, Remote& remote, uint32_t response, PortalResults results = {}) {
      const auto session = sessions.find(path);
      if (response == 0) {
        remote.phase = Remote::Phase::Started;
        results.emplace("devices", sdbus::Variant(remote.capabilities));
        if (remote.clipboard == Remote::ClipboardAccess::Enabled) {
          results.emplace("clipboard_enabled", sdbus::Variant(true));
          // After the Start reply: the frontend relays clipboard signals only once it saw clipboard_enabled.
          loop.addTimer(0, [this, path]() { announceSelection(path); });
        }
        if (!remote.grantToken.empty()) {
          PortalResults data{{"token", sdbus::Variant(remote.grantToken)}};
          if (session && session->sourcesSelected()) {
            data.emplace("sources", session->restoreDataVariant());
          }
          results.emplace(
              "restore_data",
              sdbus::Variant(RestoreData{std::string{kRestoreVendor}, kRestoreVersion, sdbus::Variant(data)})
          );
          results.emplace("persist_mode", sdbus::Variant(remote.grantMode));
        }
      } else {
        remote.phase = Remote::Phase::Ended;
        retire(std::move(remote.input));
        if (remote.clipboard == Remote::ClipboardAccess::Enabled) {
          remote.clipboard = Remote::ClipboardAccess::Off;
          releaseClipboard();
        }
        if (const auto cancel = std::exchange(remote.cancelSources, nullptr)) {
          cancel();
        }
        // The ScreenCast part may already capture: a refused Start shares nothing.
        if (session) {
          session->clearStreams();
        }
      }
      if (auto reply = std::exchange(remote.startReply, nullptr)) {
        reply(response, std::move(results));
      }
      if (auto request = std::exchange(remote.request, nullptr)) {
        loop.addTimer(0, [request = std::move(request)]() mutable { request.reset(); });
      }
    }

    void cancelStart(const std::string& path) {
      Remote* remote = live(path);
      if (remote != nullptr && (remote->phase == Remote::Phase::Consent || remote->phase == Remote::Phase::Sources)) {
        finishStart(path, *remote, 1);
      }
    }

    // Closes the session after the current event (revoked grant, compositor quit, lost EIS connection).
    void closeLater(const std::string& path) {
      loop.addTimer(0, [this, path]() {
        if (const auto session = sessions.find(path)) {
          session->closeByBackend();
        }
      });
    }

    // The session closed: the compositor ends its grant use and disconnects its EIS clients when `input` goes.
    void end(const std::string& path) {
      const auto it = remotes.find(path);
      if (it == remotes.end()) {
        return;
      }
      Remote& remote = *it->second;
      finishStart(path, remote, 1);
      for (auto& [request, consumer] : std::exchange(remote.connects, {})) {
        (void)request;
        consumer(-1);
      }
      dropSource(remote);
      remotes.erase(it);
      releaseClipboard();
    }

    void enableClipboard(Remote& remote) {
      if (!clipboard) {
        createClipboard();
        if (!clipboard) {
          std::fprintf(stderr, "remote desktop: the compositor offers no ext_data_control_manager_v1\n");
          return;
        }
      }
      remote.clipboard = Remote::ClipboardAccess::Enabled;
    }

    void createClipboard() {
      clipboard = wayland.createClipboard({
          .selection = [this](const std::vector<std::string>&, uint64_t) { announceSelection({}); },
          .send = [this](uint64_t source, const std::string& mimeType, int fd) { transfer(source, mimeType, fd); },
          .cancelled =
              [this](uint64_t source) {
                for (auto& [path, remote] : remotes) {
                  (void)path;
                  if (remote->source == source) {
                    dropSource(*remote);
                  }
                }
              },
          // Sessions refuse clipboard calls until the next enableClipboard makes a new device; no retry loop when
          // the compositor ends every device.
          .finished =
              [this]() {
                loop.addTimer(0, [this]() {
                  if (!clipboard || !clipboard->ended()) {
                    return;
                  }
                  for (auto& [path, remote] : remotes) {
                    (void)path;
                    dropSource(*remote);
                  }
                  clipboard.reset();
                });
              },
      });
    }

    // Ends the data-control device when no session uses the clipboard.
    void releaseClipboard() {
      if (clipboard && std::ranges::none_of(remotes, [](const auto& entry) {
            return entry.second->clipboard == Remote::ClipboardAccess::Enabled;
          })) {
        loop.addTimer(0, [this]() {
          if (std::ranges::none_of(remotes, [](const auto& entry) {
                return entry.second->clipboard == Remote::ClipboardAccess::Enabled;
              })) {
            clipboard.reset();
          }
        });
      }
    }

    // A session that may use the clipboard now: started, consented and not paused.
    Remote* clipboardRemote(const std::string& path) {
      Remote* remote = remoteFor(path);
      return remote != nullptr
              && remote->phase == Remote::Phase::Started
              && remote->clipboard == Remote::ClipboardAccess::Enabled
              && !remote->paused
              && clipboard
          ? remote
          : nullptr;
    }

    // Tells one session (path) or every active session (empty path) the current selection.
    void announceSelection(const std::string& only) {
      if (!clipboard) {
        return;
      }
      for (const auto& [path, remote] : remotes) {
        if ((!only.empty() && path != only)
            || remote->phase != Remote::Phase::Started
            || remote->clipboard != Remote::ClipboardAccess::Enabled
            || remote->paused) {
          continue;
        }
        const PortalResults options{
            {"mime_types", sdbus::Variant(clipboard->mimeTypes())},
            {"session_is_owner", sdbus::Variant(remote->source != 0 && remote->source == clipboard->owner())},
        };
        object.emitSignal("SelectionOwnerChanged")
            .onInterface(kClipboardInterface)
            .withArguments(sdbus::ObjectPath{path}, options);
      }
    }

    void dropSource(Remote& remote) {
      const uint64_t source = std::exchange(remote.source, 0);
      if (source == 0) {
        return;
      }
      std::erase_if(transfers, [source](const auto& entry) { return entry.second.source == source; });
      if (clipboard) {
        clipboard->withdraw(source);
      }
    }

    void pause(const std::string& path, bool paused) {
      Remote* remote = live(path);
      if (remote == nullptr || remote->paused == paused) {
        return;
      }
      remote->paused = paused;
      std::fprintf(stderr, "remote desktop: %s %s\n", path.c_str(), paused ? "paused" : "resumed");
      if (paused) {
        std::erase_if(transfers, [&path](const auto& entry) { return entry.second.path == path; });
      } else {
        announceSelection(path);
      }
    }

    void transfer(uint64_t source, const std::string& mimeType, int fd) {
      sdbus::UnixFd pipe{fd, sdbus::adopt_fd};
      const auto found =
          std::ranges::find_if(remotes, [source](const auto& entry) { return entry.second->source == source; });
      if (found == remotes.end() || found->second->paused) {
        return;
      }
      const std::string& path = found->first;
      size_t owed = 0;
      for (auto it = transfers.rbegin(); it != transfers.rend(); ++it) {
        if (it->second.path == path && ++owed >= kMaxTransfers) {
          transfers.erase(std::next(it).base());
          break;
        }
      }
      uint32_t serial = nextSerial++;
      while (serial == 0 || transfers.contains(serial)) {
        serial = nextSerial++;
      }
      transfers.emplace(serial, Transfer{.path = path, .source = source, .fd = std::move(pipe)});
      object.emitSignal("SelectionTransfer")
          .onInterface(kClipboardInterface)
          .withArguments(sdbus::ObjectPath{path}, mimeType, serial);
    }

    void requestClipboard(const std::string& path) {
      Remote* remote = remoteFor(path);
      if (remote != nullptr
          && remote->phase == Remote::Phase::Open
          && remote->clipboard == Remote::ClipboardAccess::Off) {
        remote->clipboard = Remote::ClipboardAccess::Requested;
      }
    }

    static sdbus::Error clipboardError(const char* message) {
      return sdbus::Error(sdbus::Error::Name{"org.freedesktop.DBus.Error.Failed"}, message);
    }

    void setSelection(const std::string& path, const PortalResults& options) {
      Remote* remote = clipboardRemote(path);
      if (remote == nullptr) {
        throw clipboardError("Clipboard not enabled");
      }
      const auto types =
          optionValue<std::vector<std::string>>(options, "mime_types").value_or(std::vector<std::string>{});
      if (types.size() > kMaxMimeTypes || std::ranges::any_of(types, [](const std::string& type) {
            return type.empty() || type.size() > kMaxMimeTypeLength;
          })) {
        throw clipboardError("Invalid mime types");
      }
      // The new source replaces the old one before the old one goes, so the selection is never empty between them.
      const uint64_t source = types.empty() ? 0 : clipboard->offer(types);
      if (!types.empty() && source == 0) {
        throw clipboardError("Clipboard unavailable");
      }
      dropSource(*remote);
      remote->source = source;
    }

    sdbus::UnixFd selectionWrite(const std::string& path, uint32_t serial) {
      const auto found = transfers.find(serial);
      if (clipboardRemote(path) == nullptr
          || found == transfers.end()
          || found->second.path != path
          || !found->second.fd) {
        throw clipboardError("Unknown transfer");
      }
      sdbus::UnixFd fd = std::move(*found->second.fd);
      found->second.fd.reset();
      return fd;
    }

    void selectionWriteDone(const std::string& path, uint32_t serial) {
      if (const auto found = transfers.find(serial);
          found != transfers.end() && found->second.path == path && remoteFor(path) != nullptr) {
        transfers.erase(found);
      }
    }

    sdbus::UnixFd selectionRead(const std::string& path, const std::string& mimeType) {
      if (clipboardRemote(path) == nullptr) {
        throw clipboardError("Clipboard not enabled");
      }
      const int fd = clipboard->receive(mimeType);
      if (fd < 0) {
        throw clipboardError("The selection does not offer this type");
      }
      return sdbus::UnixFd{fd, sdbus::adopt_fd};
    }

    void registerClipboard() {
      using Path = sdbus::ObjectPath;
      object
          .addVTable(
              sdbus::registerMethod("RequestClipboard")
                  .implementedAs([this](const Path& session, const PortalResults&) { requestClipboard(session); })
                  .withInputParamNames("session_handle", "options"),
              sdbus::registerMethod("SetSelection")
                  .implementedAs([this](const Path& session, const PortalResults& options) {
                    setSelection(session, options);
                  })
                  .withInputParamNames("session_handle", "options"),
              sdbus::registerMethod("SelectionWrite")
                  .implementedAs([this](const Path& session, uint32_t serial) {
                    return selectionWrite(session, serial);
                  })
                  .withInputParamNames("session_handle", "serial")
                  .withOutputParamNames("fd"),
              sdbus::registerMethod("SelectionWriteDone")
                  .implementedAs([this](const Path& session, uint32_t serial, bool) {
                    selectionWriteDone(session, serial);
                  })
                  .withInputParamNames("session_handle", "serial", "success"),
              sdbus::registerMethod("SelectionRead")
                  .implementedAs([this](const Path& session, const std::string& mimeType) {
                    return selectionRead(session, mimeType);
                  })
                  .withInputParamNames("session_handle", "mime_type")
                  .withOutputParamNames("fd"),
              sdbus::registerSignal("SelectionOwnerChanged")
                  .withParameters<Path, PortalResults>("session_handle", "options"),
              sdbus::registerSignal("SelectionTransfer")
                  .withParameters<Path, std::string, uint32_t>("session_handle", "mime_type", "serial"),
              sdbus::registerProperty("version").withGetter([]() { return uint32_t{1}; })
          )
          .forInterface(kClipboardInterface);
    }

    void answerConnect(const std::string& path, uint32_t request, int fd) {
      Remote* remote = live(path);
      const auto it = remote == nullptr ? decltype(Remote::connects)::iterator{} : remote->connects.find(request);
      if (remote == nullptr || it == remote->connects.end()) {
        if (fd >= 0) {
          close(fd);
        }
        return;
      }
      auto consumer = std::move(it->second);
      remote->connects.erase(it);
      consumer(fd);
    }

    void connectToEis(sdbus::Result<sdbus::UnixFd>&& result, const std::string& path) {
      Remote* remote = remoteFor(path);
      if (remote == nullptr || remote->phase != Remote::Phase::Started || !remote->input) {
        result.returnError(sdbus::Error(sdbus::Error::Name{"org.freedesktop.DBus.Error.Failed"}, "Session not started"));
        return;
      }
      remote->connects.emplace(remote->input->connectEis(), [result = std::move(result)](int fd) mutable {
        if (fd < 0) {
          result.returnError(sdbus::Error(sdbus::Error::Name{"org.freedesktop.DBus.Error.Failed"}, "EIS refused"));
          return;
        }
        result.returnResults(sdbus::UnixFd{fd, sdbus::adopt_fd});
      });
    }

    // Runs `op` on the session's sender once it has every granted device; checks the owner and the device type.
    void notify(const std::string& path, uint32_t type, EiSender::Op op) {
      Remote* remote = remoteFor(path);
      if (remote == nullptr || remote->phase != Remote::Phase::Started || (remote->capabilities & type) == 0) {
        return;
      }
      if (!remote->sender) {
        remote->sender = std::make_unique<EiSender>(loop, remote->capabilities, [this, path]() { closeLater(path); });
        remote->connects.emplace(remote->input->connectEis(), [sender = remote->sender.get()](int fd) {
          sender->connect(fd);
        });
      }
      remote->sender->submit(std::move(op));
    }

    void pointerAbsolute(const std::string& path, uint32_t stream, double x, double y) {
      const auto session = sessions.find(path);
      if (!session) {
        return;
      }
      for (const auto& result : session->streamResults()) {
        if (result.nodeId != stream) {
          continue;
        }
        if (result.sourceType != 1 || result.width <= 0 || result.height <= 0) {
          std::fprintf(stderr, "remote desktop: absolute motion is only supported on monitor streams\n");
          return;
        }
        const double fx = std::clamp(x / result.width, 0.0, 1.0);
        const double fy = std::clamp(y / result.height, 0.0, 1.0);
        notify(path, kPointer, [output = result.mappingId, fx, fy](EiSender& sender) {
          sender.absolute(output, fx, fy);
        });
        return;
      }
    }

    void keysym(const std::string& path, int32_t keysym, bool pressed) {
      notify(path, kKeyboard, [keysym, pressed](EiSender& sender) {
        ei_device* device = sender.device(EI_DEVICE_CAP_KEYBOARD);
        if (device == nullptr) {
          return;
        }
        const auto key = sender.keycodeFor(device, static_cast<xkb_keysym_t>(keysym));
        if (!key) {
          std::fprintf(stderr, "remote desktop: keysym 0x%x is not in the keymap\n", static_cast<unsigned>(keysym));
          return;
        }
        const auto [code, shift] = *key;
        if (shift && pressed) {
          ei_device_keyboard_key(device, KEY_LEFTSHIFT, true);
        }
        ei_device_keyboard_key(device, code, pressed);
        if (shift && !pressed) {
          ei_device_keyboard_key(device, KEY_LEFTSHIFT, false);
        }
        sender.frame(device);
      });
    }

    void registerVTable() {
      using Path = sdbus::ObjectPath;
      object
          .addVTable(
              sdbus::registerMethod("CreateSession")
                  .implementedAs([this](
                                     PortalResponse&& result, const Path& handle, const Path& session,
                                     const std::string& appId, const PortalResults&
                                 ) { createSession(std::move(result), handle, session, appId); })
                  .withInputParamNames("handle", "session_handle", "app_id", "options")
                  .withOutputParamNames("response", "results"),
              sdbus::registerMethod("SelectDevices")
                  .implementedAs([this](
                                     PortalResponse&& result, const Path& handle, const Path& session,
                                     const std::string&, const PortalResults& options
                                 ) { selectDevices(std::move(result), handle, session, options); })
                  .withInputParamNames("handle", "session_handle", "app_id", "options")
                  .withOutputParamNames("response", "results"),
              sdbus::registerMethod("Start")
                  .implementedAs([this](
                                     PortalResponse&& result, const Path& handle, const Path& session,
                                     const std::string&, const std::string& parentWindow, const PortalResults&
                                 ) { start(std::move(result), handle, session, parentWindow); })
                  .withInputParamNames("handle", "session_handle", "app_id", "parent_window", "options")
                  .withOutputParamNames("response", "results"),
              sdbus::registerMethod("ConnectToEIS")
                  .implementedAs([this](
                                     sdbus::Result<sdbus::UnixFd>&& result, const Path& session, const std::string&,
                                     const PortalResults&
                                 ) { connectToEis(std::move(result), session); })
                  .withInputParamNames("session_handle", "app_id", "options")
                  .withOutputParamNames("fd"),
              sdbus::registerMethod("NotifyPointerMotion")
                  .implementedAs([this](const Path& session, const PortalResults&, double dx, double dy) {
                    notify(session, kPointer, [dx, dy](EiSender& sender) {
                      if (ei_device* device = sender.device(EI_DEVICE_CAP_POINTER)) {
                        ei_device_pointer_motion(device, dx, dy);
                        sender.frame(device);
                      }
                    });
                  })
                  .withInputParamNames("session_handle", "options", "dx", "dy"),
              sdbus::registerMethod("NotifyPointerMotionAbsolute")
                  .implementedAs([this](const Path& session, const PortalResults&, uint32_t stream, double x, double y) {
                    pointerAbsolute(session, stream, x, y);
                  })
                  .withInputParamNames("session_handle", "options", "stream", "x", "y"),
              sdbus::registerMethod("NotifyPointerButton")
                  .implementedAs([this](const Path& session, const PortalResults&, int32_t button, uint32_t state) {
                    notify(session, kPointer, [button, state](EiSender& sender) {
                      if (ei_device* device = sender.device(EI_DEVICE_CAP_BUTTON)) {
                        ei_device_button_button(device, static_cast<uint32_t>(button), state != 0);
                        sender.frame(device);
                      }
                    });
                  })
                  .withInputParamNames("session_handle", "options", "button", "state"),
              sdbus::registerMethod("NotifyPointerAxis")
                  .implementedAs([this](const Path& session, const PortalResults& options, double dx, double dy) {
                    const bool finish = optionValue<bool>(options, "finish").value_or(false);
                    notify(session, kPointer, [dx, dy, finish](EiSender& sender) {
                      if (ei_device* device = sender.device(EI_DEVICE_CAP_SCROLL)) {
                        ei_device_scroll_delta(device, dx, dy);
                        if (finish) {
                          ei_device_scroll_stop(device, true, true);
                        }
                        sender.frame(device);
                      }
                    });
                  })
                  .withInputParamNames("session_handle", "options", "dx", "dy"),
              sdbus::registerMethod("NotifyPointerAxisDiscrete")
                  .implementedAs([this](const Path& session, const PortalResults&, uint32_t axis, int32_t steps) {
                    // libei counts 120 per wheel detent.
                    const int32_t value = steps * 120;
                    notify(session, kPointer, [axis, value](EiSender& sender) {
                      if (ei_device* device = sender.device(EI_DEVICE_CAP_SCROLL)) {
                        ei_device_scroll_discrete(device, axis == 1 ? value : 0, axis == 0 ? value : 0);
                        sender.frame(device);
                      }
                    });
                  })
                  .withInputParamNames("session_handle", "options", "axis", "steps"),
              sdbus::registerMethod("NotifyKeyboardKeycode")
                  .implementedAs([this](const Path& session, const PortalResults&, int32_t keycode, uint32_t state) {
                    notify(session, kKeyboard, [keycode, state](EiSender& sender) {
                      if (ei_device* device = sender.device(EI_DEVICE_CAP_KEYBOARD)) {
                        ei_device_keyboard_key(device, static_cast<uint32_t>(keycode), state != 0);
                        sender.frame(device);
                      }
                    });
                  })
                  .withInputParamNames("session_handle", "options", "keycode", "state"),
              sdbus::registerMethod("NotifyKeyboardKeysym")
                  .implementedAs([this](const Path& session, const PortalResults&, int32_t sym, uint32_t state) {
                    keysym(session, sym, state != 0);
                  })
                  .withInputParamNames("session_handle", "options", "keysym", "state"),
              sdbus::registerProperty("AvailableDeviceTypes").withGetter([]() { return kAvailableDeviceTypes; }),
              sdbus::registerProperty("version").withGetter([]() { return uint32_t{2}; })
          )
          .forInterface(kInterface);
    }
  };

  RemoteDesktopPortal::RemoteDesktopPortal(
      Loop& loop, sdbus::IConnection& connection, sdbus::IObject& object, SessionTable& sessions,
      ScreenCastPortal& screencast, WaylandContext& wayland
  )
      : m_impl(std::make_unique<Impl>(loop, connection, object, sessions, screencast, wayland)) {}

  RemoteDesktopPortal::~RemoteDesktopPortal() = default;

} // namespace xdpu
