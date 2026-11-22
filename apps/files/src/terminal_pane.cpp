#include "terminal_pane.h"

#include "app/app_loop.h"
#include "app/app_window.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "platform/input_events.h"
#include "platform/surface.h"
#include "render/scene/input_area.h"
#include "shell_integration.h"
#include "ui/builders.h"

#include <algorithm>
#include <cstdlib>
#include <glib.h>
#include <pwd.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

  constexpr Logger kLog("terminal");
  constexpr float kMargin = 8.0F;

  std::string shellPath() {
    if (const char* env = std::getenv("SHELL"); env != nullptr && *env == '/' && ::access(env, X_OK) == 0) {
      return env;
    }
    if (const passwd* pw = ::getpwuid(::getuid()); pw != nullptr && pw->pw_shell != nullptr && *pw->pw_shell == '/') {
      return pw->pw_shell;
    }
    return "/bin/sh";
  }

  std::string quoted(const std::string& path) {
    std::string out = "'";
    for (const char c : path) {
      out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    }
    return out + "'";
  }

} // namespace

TerminalPane::TerminalPane(AppLoop& loop, AppWindow& window, const fs::path& dir, Host host)
    : m_loop(loop), m_window(window), m_host(std::move(host)) {
  const std::weak_ptr<bool> alive = m_alive;
  m_view = std::make_unique<term::TerminalView>(term::TerminalView::Host{
      .requestFrame = [this] { requestFrame(); },
      .textures = [this]() -> TextureManager* {
        RenderContext& render = m_loop.renderContext();
        return render.glReady() ? &render.textureManager() : nullptr;
      },
      // Runs inside the view's frame(): the owner may destroy the pane, so it hears of the exit afterwards.
      .exited =
          [this, alive](int /*status*/) {
            kLog.info("terminal shell exited");
            m_running = false;
            DeferredCall::callLater([this, alive] {
              if (!alive.expired() && m_host.exited) {
                m_host.exited();
              }
            });
          },
      .modifiers = [this] { return m_loop.display().keyboardModifiers(); },
      .cwdChanged =
          [this](const std::string& cwd) {
            m_cwd = cwd;
            kLog.info("terminal cwd {}", cwd);
            if (m_host.cwdChanged) {
              m_host.cwdChanged(cwd);
            }
          },
      .textBacking = [this] { return m_window.sceneTextBacking(); },
  });
  const std::string shell = shellPath();
  const term::ShellLaunch launch = term::shellLaunch(shell, term::shellIntegrationDir(), [](const char* name) {
    return std::getenv(name);
  });
  m_running = m_view->start(launch.argv, dir.string(), launch.env);
  if (m_running) {
    m_cwd = dir;
    kLog.info("terminal started {} in {}", shell, dir.string());
  }
}

TerminalPane::~TerminalPane() {
  if (Surface* surface = m_window.surface(); surface != nullptr) {
    surface->setFrameTickCallback({});
    surface->setScaleChangedCallback({});
  }
}

void TerminalPane::requestFrame() {
  m_window.noteSceneActivity();
  Surface* surface = m_window.surface();
  if (surface == nullptr) {
    return;
  }
  // Set on each request: the window may have a new surface since the last one.
  surface->setFrameTickCallback([this](float) {
    if (Surface* s = m_window.surface(); s != nullptr) {
      m_loop.renderContext().makeCurrent(s->renderTarget());
      m_view->frame();
    }
  });
  surface->setScaleChangedCallback([this](float scale) { m_view->setDeviceScale(scale); });
  surface->requestFrameTick();
}

std::unique_ptr<Node> TerminalPane::build(Renderer& renderer, float scale, float width) {
  const float s = scale;
  const float innerWidth = width - 2.0F * kMargin * s;
  const float innerHeight = (kHeight - 2.0F * kMargin) * s;
  Surface* surface = m_window.surface();
  const float deviceScale = surface != nullptr ? surface->effectiveBufferScale() : 1.0F;
  if (m_built) {
    m_view->sceneDestroyed(); // a rebuild freed the last build's nodes with the old scene
  }
  auto card = ui::column({.align = FlexAlign::Stretch, .radius = 8.0F * s, .clipChildren = true, .width = innerWidth,
                          .height = innerHeight});
  card->addChild(m_view->build(renderer, s, deviceScale, innerWidth, innerHeight));
  m_built = true;
  m_view->setSize(innerWidth, innerHeight);
  m_view->setWindowActive(true);
  if (InputArea* area = m_view->focusArea(); area != nullptr) {
    area->setOnClick([this](const InputArea::PointerData&) { focus(); });
  }
  auto outer = ui::column({.align = FlexAlign::Stretch, .padding = kMargin * s, .height = kHeight * s});
  outer->addChild(std::move(card));
  if (m_focusWanted) {
    m_focusWanted = false;
    // After this build is in the scene: the dispatcher tracks only attached areas.
    DeferredCall::callLater([this, alive = std::weak_ptr<bool>(m_alive)] {
      if (!alive.expired() && m_built && m_host.focus) {
        m_host.focus(m_view->focusArea());
      }
    });
  }
  return outer;
}

void TerminalPane::focus() {
  if (m_built && m_view->focusArea() != nullptr && m_host.focus) {
    m_host.focus(m_view->focusArea());
  } else {
    m_focusWanted = true; // hidden or not built yet: the next build takes the keys
  }
}

void TerminalPane::detach() {
  if (m_built) {
    m_view->sceneDestroyed();
  }
  m_built = false;
  m_focusWanted = false;
}

bool TerminalPane::focused() const {
  return m_built && m_view->focusArea() != nullptr && m_view->focusArea()->focused();
}

bool TerminalPane::key(const KeyboardEvent& event) { return focused() && m_view->key(event); }

void TerminalPane::followFolder(const fs::path& dir) {
  if (!m_running || dir.empty() || dir == m_cwd || !dir.is_absolute()) {
    return;
  }
  // A running job (vim, a build) owns the terminal: typing a cd into it would do harm.
  if (m_view->session().foregroundJob()) {
    kLog.debug("terminal busy, no cd to {}", dir.string());
    return;
  }
  // The path is typed into the shell: a control byte in a folder name (Ctrl+U, Return) would edit the line and run
  // what follows it, and invalid UTF-8 can carry C1 controls. Such a folder is not followed.
  const std::string text = dir.string();
  const bool control = std::ranges::any_of(text, [](char c) {
    const auto b = static_cast<unsigned char>(c);
    return b < 0x20 || b == 0x7f;
  });
  if (control || !g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr)) {
    kLog.warn("terminal does not follow a folder whose name has control characters");
    return;
  }
  m_cwd = dir;
  // Ctrl+E Ctrl+U clears a half-typed line; the leading space keeps the cd out of the history (ignorespace).
  sendText("\x05\x15 cd -- " + quoted(text) + "\r");
  kLog.info("terminal follows {}", text);
}

void TerminalPane::sendText(const std::string& text) { m_view->sendText(text, false); }

std::string TerminalPane::screenText() { return m_view->text(term::TerminalView::TextExtent::Screen); }
