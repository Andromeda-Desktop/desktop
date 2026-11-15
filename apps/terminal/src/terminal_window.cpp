#include "terminal_window.h"

#include "app/app_loop.h"
#include "app/app_window.h"
#include "notifier.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "wayland/wayland_connection.h"

#include <pwd.h>
#include <unistd.h>

#include <cstdlib>
#include <string>

namespace term {

  namespace {
    constexpr Logger kLog("terminal");

    std::string loginShell() {
      if (const char* shell = std::getenv("SHELL"); shell != nullptr && *shell != '\0') {
        return shell;
      }
      if (const passwd* pw = getpwuid(getuid()); pw != nullptr && pw->pw_shell != nullptr) {
        return pw->pw_shell;
      }
      return "/bin/sh";
    }

    std::string homeDir() {
      const char* home = std::getenv("HOME");
      return home != nullptr ? home : "/";
    }
  } // namespace

  TerminalWindow::TerminalWindow(AppLoop& loop, AppWindow& window, Notifier& notifier, const std::string& cwd,
                                 std::function<void()> close)
      : m_loop(loop), m_window(window), m_notifier(notifier),
        m_view({.requestFrame = [this] { requestFrame(); }, .exited = [close = std::move(close)](int) { close(); },
                 .modifiers = [this] { return m_loop.wayland().keyboardModifiers(); },
                 .commandFinished =
                     [this](const std::string& summary, const std::string& body) {
                       m_notifier.notify(summary, body,
                                         [this, alive = std::weak_ptr<bool>(m_alive)](const std::string& token) {
                                           if (!alive.expired()) {
                                             m_window.activate(token);
                                           }
                                         });
                     },
                 .cwdChanged =
                     [this](const std::string& path) {
                       const std::string home = homeDir();
                       const bool inHome = path == home || path.starts_with(home + "/");
                       m_window.setTitle(inHome ? "~" + path.substr(home.size()) : path);
                     }}) {
    m_window.setContentBuilder([this](Renderer& renderer, float width, float height) {
      Surface* surface = m_window.surface();
      const float deviceScale = surface != nullptr ? surface->effectiveBufferScale() : 1.0F;
      auto node = m_view.build(renderer, m_window.uiScale(), deviceScale, width, height);
      // After this build is in the scene: the dispatcher focuses only attached areas.
      DeferredCall::callLater([this, alive = std::weak_ptr<bool>(m_alive)] {
        if (!alive.expired() && m_view.focusArea() != nullptr) {
          m_window.input().setFocus(m_view.focusArea());
        }
      });
      return node;
    });
    m_window.setFirstKeyHandler([this](const KeyboardEvent& event) {
      return m_view.focusArea() != nullptr && m_window.input().focusedArea() == m_view.focusArea() && m_view.key(event);
    });
    m_window.setActiveCallback([this](bool active) { m_view.setWindowActive(active); });
    m_window.setLayoutCallback([this] {
      const AppWindow::Rect rect = m_window.contentRect();
      m_view.setSize(rect.width, rect.height);
    });
    const std::string shell = loginShell();
    if (!m_view.start({shell, "-l"}, cwd.empty() ? homeDir() : cwd)) {
      kLog.warn("cannot start {}", shell);
    }
  }

  TerminalWindow::~TerminalWindow() {
    m_alive.reset();
    if (m_tickSurface != nullptr && m_tickSurface == m_window.surface()) {
      m_tickSurface->setFrameTickCallback(nullptr);
      m_tickSurface->setScaleChangedCallback(nullptr);
    }
  }

  void TerminalWindow::requestFrame() {
    Surface* surface = m_window.surface();
    if (surface == nullptr) {
      return;
    }
    if (surface != m_tickSurface) {
      m_tickSurface = surface;
      surface->setFrameTickCallback([this](float) {
        m_loop.renderContext().makeCurrent(m_window.surface()->renderTarget());
        m_view.frame();
      });
      surface->setScaleChangedCallback([this](float scale) { m_view.setDeviceScale(scale); });
    }
    surface->requestFrameTick();
  }

} // namespace term
