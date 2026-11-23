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
#include "ui/style.h"

#include <algorithm>
#include <linux/input-event-codes.h>
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

TerminalPane::TerminalPane(AppLoop& loop, AppWindow& window, const fs::path& dir, float height, Host host)
    : m_loop(loop), m_window(window), m_host(std::move(host)), m_height(std::max(height, kMinHeight)),
      m_maxHeight(m_height) {
  addSplit(dir, 0);
}

TerminalPane::~TerminalPane() {
  if (Surface* surface = m_window.surface(); surface != nullptr) {
    surface->setFrameTickCallback({});
    surface->setScaleChangedCallback({});
  }
}

bool TerminalPane::addSplit(const fs::path& dir, std::size_t at) {
  const std::weak_ptr<bool> alive = m_alive;
  const std::uint64_t id = ++m_lastId;
  auto view = std::make_unique<term::TerminalView>(term::TerminalView::Host{
      .requestFrame = [this] { requestFrame(); },
      .textures = [this]() -> TextureManager* {
        RenderContext& render = m_loop.renderContext();
        return render.glReady() ? &render.textureManager() : nullptr;
      },
      // Runs inside the view's frame(): removing the split there would free the view under its caller.
      .exited =
          [this, alive, id](int /*status*/) {
            kLog.info("terminal shell exited");
            DeferredCall::callLater([this, alive, id] {
              if (!alive.expired()) {
                removeSplit(id);
              }
            });
          },
      .modifiers = [this] { return m_loop.display().keyboardModifiers(); },
      .cwdChanged =
          [this, id](const std::string& cwd) {
            Split* split = splitOf(id);
            if (split == nullptr) {
              return;
            }
            split->cwd = cwd;
            kLog.info("terminal cwd {}", cwd);
            if (split == &active() && m_host.cwdChanged) {
              m_host.cwdChanged(cwd);
            }
          },
      .textBacking = [this] { return m_window.sceneTextBacking(); },
  });
  const std::string shell = shellPath();
  const term::ShellLaunch launch = term::shellLaunch(shell, term::shellIntegrationDir(), [](const char* name) {
    return std::getenv(name);
  });
  if (!view->start(launch.argv, dir.string(), launch.env)) {
    return false;
  }
  kLog.info("terminal started {} in {}", shell, dir.string());
  m_splits.insert(m_splits.begin() + static_cast<std::ptrdiff_t>(at), Split{.id = id, .view = std::move(view), .cwd = dir});
  m_active = at;
  return true;
}

TerminalPane::Split* TerminalPane::splitOf(std::uint64_t id) {
  const auto it = std::ranges::find(m_splits, id, &Split::id);
  return it == m_splits.end() ? nullptr : &*it;
}

void TerminalPane::removeSplit(std::uint64_t id) {
  const auto it = std::ranges::find(m_splits, id, &Split::id);
  if (it == m_splits.end()) {
    return;
  }
  const auto index = static_cast<std::size_t>(it - m_splits.begin());
  if (m_built) {
    m_retired.push_back(std::move(it->view)); // its nodes stay in the scene until the next build
  }
  m_splits.erase(it);
  if (m_splits.empty()) {
    if (m_host.exited) {
      m_host.exited();
    }
    return;
  }
  if (m_active > index || m_active == m_splits.size()) {
    --m_active;
  }
  m_focusWanted = true;
  m_window.requestRebuild();
}

void TerminalPane::split() {
  if (m_splits.empty() || m_splits.size() >= kMaxSplits) {
    return;
  }
  if (addSplit(active().cwd, m_active + 1)) {
    m_focusWanted = true;
    m_window.requestRebuild();
  }
}

void TerminalPane::closeActive() {
  if (!m_splits.empty()) {
    removeSplit(active().id);
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
      for (Split& split : m_splits) {
        split.view->frame();
      }
    }
  });
  surface->setScaleChangedCallback([this](float scale) {
    for (Split& split : m_splits) {
      split.view->setDeviceScale(scale);
    }
  });
  surface->requestFrameTick();
}

namespace {
  // Mockup .term card: margin 8, r8. The header strip above the cards is control_s high.
  float headerHeight() { return Tokens::sizeControlS + Tokens::spaceP4; }
} // namespace

std::unique_ptr<Node> TerminalPane::build(Renderer& renderer, float scale, float width, float maxHeight) {
  const float s = scale;
  m_scale = s;
  m_maxHeight = std::max(maxHeight, kMinHeight);
  m_height = std::clamp(m_height, kMinHeight, m_maxHeight);
  Surface* surface = m_window.surface();
  const float deviceScale = surface != nullptr ? surface->effectiveBufferScale() : 1.0F;
  if (m_built) {
    for (Split& split : m_splits) {
      split.view->sceneDestroyed(); // a rebuild freed the last build's nodes with the old scene
    }
  }
  if (!m_retired.empty()) {
    // After this build is in the scene: the old scene's focus loss still calls into the retired views.
    DeferredCall::callLater([this, alive = std::weak_ptr<bool>(m_alive)] {
      if (!alive.expired()) {
        m_retired.clear();
      }
    });
  }
  const float gap = kMargin * s;
  const auto count = static_cast<float>(m_splits.size());
  m_cardWidth = (width - 2.0F * gap - gap * (count - 1.0F)) / count;
  const float cardHeight = (m_height - 2.0F * kMargin - headerHeight()) * s;

  auto header = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP4 * s, .height = Tokens::sizeControlS * s});
  auto grip = ui::inputArea({.cursorShape = CursorShape::NsResize, .height = Tokens::sizeControlS * s, .flexGrow = 1.0F});
  grip->setOnPress([this](const InputArea::PointerData& data) {
    if (data.button != BTN_LEFT) {
      return;
    }
    if (data.pressed) {
      m_drag = Drag{.startSceneY = data.sceneY, .startHeight = m_height};
      return;
    }
    if (m_drag.has_value()) {
      m_drag.reset();
      if (m_host.resized) {
        m_host.resized(m_height);
      }
    }
  });
  grip->setOnMotion([this](const InputArea::PointerData& data) {
    if (m_drag.has_value()) {
      resizeTo(m_drag->startHeight + (m_drag->startSceneY - data.sceneY) / m_scale);
    }
  });
  header->addChild(std::move(grip));
  const auto headerButton = [s](std::string glyph, std::string tooltip, std::function<void()> action) {
    return ui::button({
        .glyph = std::move(glyph),
        .glyphSize = Tokens::sizeSymbol * s,
        .variant = ButtonVariant::Ghost,
        .tooltip = std::move(tooltip),
        .radius = Tokens::sizeControlS * s / 2.0F,
        .width = Tokens::sizeControlS * s,
        .height = Tokens::sizeControlS * s,
        .onClick = std::move(action),
    });
  };
  // A click rebuilds the pane: the button must not be freed inside its own handler.
  const auto later = [this](void (TerminalPane::*action)()) {
    return [this, action] {
      DeferredCall::callLater([this, action, alive = std::weak_ptr<bool>(m_alive)] {
        if (!alive.expired()) {
          (this->*action)();
        }
      });
    };
  };
  if (m_splits.size() < kMaxSplits) {
    header->addChild(headerButton("layout-columns", "Split Terminal", later(&TerminalPane::split)));
  }
  header->addChild(headerButton("close", m_splits.size() > 1 ? "Close This Terminal" : "Close Terminal",
                                later(&TerminalPane::closeActive)));

  auto cards = ui::row({.out = &m_cardsRow, .align = FlexAlign::Stretch, .gap = gap, .height = cardHeight});
  for (std::size_t i = 0; i < m_splits.size(); ++i) {
    Split& split = m_splits[i];
    auto card = ui::column({.out = &split.card, .align = FlexAlign::Stretch, .radius = 8.0F * s, .clipChildren = true,
                            .width = m_cardWidth, .height = cardHeight});
    card->addChild(split.view->build(renderer, s, deviceScale, m_cardWidth, cardHeight));
    split.view->setSize(m_cardWidth, cardHeight);
    split.view->setWindowActive(true);
    if (InputArea* area = split.view->focusArea(); area != nullptr) {
      area->setOnClick([this, id = split.id](const InputArea::PointerData&) {
        if (const auto it = std::ranges::find(m_splits, id, &Split::id); it != m_splits.end()) {
          m_active = static_cast<std::size_t>(it - m_splits.begin());
          focus();
        }
      });
    }
    cards->addChild(std::move(card));
  }
  m_built = true;
  auto outer = ui::column({.out = &m_outer, .align = FlexAlign::Stretch, .gap = Tokens::spaceP4 * s,
                           .padding = kMargin * s, .height = m_height * s});
  outer->addChild(std::move(header));
  outer->addChild(std::move(cards));
  if (m_focusWanted) {
    m_focusWanted = false;
    // After this build is in the scene: the dispatcher tracks only attached areas.
    DeferredCall::callLater([this, alive = std::weak_ptr<bool>(m_alive)] {
      if (!alive.expired() && m_built && !m_splits.empty() && m_host.focus) {
        m_host.focus(active().view->focusArea());
      }
    });
  }
  return outer;
}

void TerminalPane::resizeTo(float height) {
  const float next = std::clamp(height, kMinHeight, m_maxHeight);
  if (next == m_height || !m_built || m_outer == nullptr) {
    return;
  }
  m_height = next;
  const float s = m_scale;
  const float cardHeight = (m_height - 2.0F * kMargin - headerHeight()) * s;
  m_outer->setSize(m_outer->width(), m_height * s);
  m_cardsRow->setSize(m_cardsRow->width(), cardHeight);
  for (Split& split : m_splits) {
    split.card->setSize(m_cardWidth, cardHeight);
    split.view->setSize(m_cardWidth, cardHeight);
  }
  if (m_host.resizing) {
    m_host.resizing(m_height);
  }
  m_window.invalidate();
}

void TerminalPane::focus() {
  if (m_built && !m_splits.empty() && active().view->focusArea() != nullptr && m_host.focus) {
    m_host.focus(active().view->focusArea());
  } else {
    m_focusWanted = true; // hidden or not built yet: the next build takes the keys
  }
}

void TerminalPane::detach() {
  if (m_built) {
    for (Split& split : m_splits) {
      split.view->sceneDestroyed();
      split.card = nullptr;
    }
  }
  m_retired.clear(); // out of the scene: no callback reaches them any more
  m_outer = nullptr;
  m_cardsRow = nullptr;
  m_drag.reset();
  m_built = false;
  m_focusWanted = false;
}

bool TerminalPane::focused() const {
  return m_built && std::ranges::any_of(m_splits, [](const Split& split) {
    return split.view->focusArea() != nullptr && split.view->focusArea()->focused();
  });
}

bool TerminalPane::key(const KeyboardEvent& event) {
  if (!m_built) {
    return false;
  }
  for (Split& split : m_splits) {
    if (split.view->focusArea() != nullptr && split.view->focusArea()->focused()) {
      return split.view->key(event);
    }
  }
  return false;
}

void TerminalPane::followFolder(const fs::path& dir) {
  if (m_splits.empty() || dir.empty() || dir == active().cwd || !dir.is_absolute()) {
    return;
  }
  // A running job (vim, a build) owns the terminal: typing a cd into it would do harm.
  if (active().view->session().foregroundJob()) {
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
  active().cwd = dir;
  // Ctrl+E Ctrl+U clears a half-typed line; the leading space keeps the cd out of the history (ignorespace).
  sendText("\x05\x15 cd -- " + quoted(text) + "\r");
  kLog.info("terminal follows {}", text);
}

void TerminalPane::sendText(const std::string& text) {
  if (!m_splits.empty()) {
    active().view->sendText(text, false);
  }
}

std::string TerminalPane::screenText() {
  return m_splits.empty() ? std::string() : active().view->text(term::TerminalView::TextExtent::Screen);
}
