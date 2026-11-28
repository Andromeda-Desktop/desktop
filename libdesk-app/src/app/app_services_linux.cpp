#include "app/app_services.h"

#include "a11y/atspi_bridge.h"
#include "a11y/atspi_poll_source.h"
#include "app/app_loop.h"
#include "app/appearance.h"
#include "app/single_instance.h"
#include "core/log.h"
#include "core/process/app_zygote.h"
#include "dbus/session_bus.h"
#include "dbus/session_bus_poll_source.h"

#include <cctype>
#include <dlfcn.h>
#include <format>
#include <sdbus-c++/sdbus-c++.h>

namespace {
  constexpr Logger kLog("app");

  class SessionServices final : public AppServices {
  public:
    SessionServices(const std::string& appId, std::string appName, bool singleInstance, Callbacks callbacks)
        : m_name(std::move(appName)), m_callbacks(std::move(callbacks)) {
      try {
        m_bus = std::make_unique<SessionBus>();
        // The bus ends with the session: quit through the loop so the state is flushed.
        m_bus->setDisconnectedCallback([this] {
          kLog.warn("session bus lost; quitting");
          m_callbacks.sessionLost();
        });
      } catch (const std::exception& e) {
        kLog.warn("no session bus ({}); single instance is off", e.what());
      }
      m_instance = std::make_unique<SingleInstance>(singleInstance ? m_bus.get() : nullptr, appId);
    }

    bool claim(const std::vector<std::string>& uris, const std::string& action) override {
      return m_instance->claim(
                 uris,
                 {.activate = m_callbacks.activate, .open = m_callbacks.open, .action = m_callbacks.action}, action
             ) != SingleInstance::Role::Forwarded;
    }

    void attach(AppLoop& loop) override {
      if (m_bus == nullptr) {
        return;
      }
      m_busSource = std::make_unique<SessionBusPollSource>(*m_bus);
      loop.addSource(m_busSource.get());
      m_appearance = std::make_unique<Appearance>(*m_bus, m_callbacks.appearanceChanged);
      // Screen readers (Orca) see the app's windows: AppWindow surfaces add their scene roots.
      AtspiBridge::instance().start(m_bus->connection(), m_name);
      m_atspi = std::make_unique<AtspiPollSource>();
      loop.addSource(m_atspi.get());
    }

    void detach(AppLoop& loop) override {
      loop.removeSource(m_busSource.get());
      if (m_atspi != nullptr) {
        loop.removeSource(m_atspi.get());
        AtspiBridge::instance().stop();
      }
    }

    void processPendingEvents() override {
      if (m_bus != nullptr) {
        m_bus->processPendingEvents();
      }
    }

    [[nodiscard]] bool hasAppearance() const override { return m_appearance != nullptr; }
    [[nodiscard]] float uiScale() const override { return m_appearance->uiScale(); }
    [[nodiscard]] float textScale() const override { return m_appearance->textScale(); }
    [[nodiscard]] bool holdMotion() const override { return m_appearance->holdMotion(); }
    void setColorScheme(ColorScheme scheme) override {
      if (m_appearance != nullptr) m_appearance->setScheme(scheme);
    }
    [[nodiscard]] SessionBus* bus() override { return m_bus.get(); }

    void showInFolder(const std::vector<std::string>& paths) override {
      if (m_bus == nullptr) return;
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
      if (m_fileManager == nullptr) {
        m_fileManager = sdbus::createProxy(m_bus->connection(), sdbus::ServiceName{"org.freedesktop.FileManager1"},
                                           sdbus::ObjectPath{"/org/freedesktop/FileManager1"});
      }
      m_fileManager->callMethodAsync("ShowItems").onInterface("org.freedesktop.FileManager1").withArguments(uris, std::string())
          .uponReplyInvoke([](std::optional<sdbus::Error> error) {
            if (error) kLog.warn("ShowItems: {}", error->getMessage());
          });
    }

  private:
    std::string m_name;
    Callbacks m_callbacks;
    std::unique_ptr<SessionBus> m_bus;
    std::unique_ptr<SessionBusPollSource> m_busSource;
    std::unique_ptr<sdbus::IProxy> m_fileManager;
    std::unique_ptr<Appearance> m_appearance; // after the bus: it uses it
    std::unique_ptr<AtspiPollSource> m_atspi;
    std::unique_ptr<SingleInstance> m_instance;
  };
} // namespace

std::unique_ptr<AppServices> createAppServices(
    const std::string& appId, const std::string& appName, bool singleInstance, AppServices::Callbacks callbacks
) {
  return std::make_unique<SessionServices>(appId, appName, singleInstance, std::move(callbacks));
}

bool forwardLaunch(const std::string& appId, const std::vector<std::string>& uris, const std::string& action) {
  try {
    SessionBus bus;
    SingleInstance instance(&bus, appId);
    return instance.forwardIfRunning(uris, action);
  } catch (const std::exception& e) {
    kLog.debug("no forward before start ({})", e.what());
  }
  return false;
}

std::vector<std::string> serveAppZygote(
    const std::string& name, const std::function<void()>& warmup,
    const std::function<bool(const std::vector<std::string>& argv)>& accept
) {
  return process::zygote::serve(name, [&warmup] {
    warmup();
    // Mesa's EGL vendor and its gallium driver: loading and relocating them is ~30 ms of each child's EGL init. Mesa
    // starts no thread on load; serve() checks that. Other vendors are not loaded here.
    dlopen("libEGL_mesa.so.0", RTLD_NOW | RTLD_LOCAL);
  }, [&accept](const process::zygote::Request& request) { return accept(request.argv); }).argv;
}
