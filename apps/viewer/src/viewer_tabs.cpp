#include "viewer_tabs.h"

#include "app/app_loop.h"
#include "app/app_window.h"
#include "core/deferred_call.h"
#include "cursor-shape-v1-client-protocol.h"
#include "render/scene/accessible.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "viewer_window.h"
#include "wayland/drag_and_drop.h"
#include "wayland/wayland_connection.h"

#include <gio/gio.h>
#include <sstream>

namespace viewer {

  ViewerTabs::ViewerTabs(AppLoop& loop, AppWindow& window) : m_loop(loop), m_window(window) {
    m_window.setContentBuilder([this](Renderer& renderer, float width, float height) {
      return build(renderer, width, height);
    });
    m_window.setKeyHandler([this](const KeyboardEvent& event) {
      ViewerWindow* tab = active();
      return tab != nullptr && tab->onKey(event);
    });
    m_window.setLayoutCallback([this] {
      if (ViewerWindow* tab = active()) {
        tab->onLayout();
      }
    });
    m_window.setCloseGuard([this] {
      if (!firstUnsaved()) {
        return true;
      }
      continueClose();
      return false;
    });
  }

  ViewerTabs::~ViewerTabs() {
    if (m_dropSurface != nullptr) {
      if (DragAndDrop* dnd = m_loop.wayland().dragAndDrop()) {
        dnd->clearTarget(m_dropSurface);
      }
    }
  }

  void ViewerTabs::registerDropTarget() {
    wl_surface* surface = m_window.wlSurface();
    DragAndDrop* dnd = m_loop.wayland().dragAndDrop();
    if (surface == nullptr || dnd == nullptr || surface == m_dropSurface) {
      return;
    }
    m_dropSurface = surface;
    dnd->setAcceptedTypes({"text/uri-list"});
    dnd->setTarget(surface, {
        .enter = [](const std::vector<std::string>&) { return true; },
        .motion = [](float, float) -> std::uint32_t { return DragAndDrop::Copy; },
        .leave = [] {},
        .drop =
            [this](float, float, std::uint32_t, const std::string& mime, std::string data) {
              if (mime != "text/uri-list") {
                return;
              }
              std::istringstream lines(data);
              for (std::string line; std::getline(lines, line);) {
                if (!line.empty() && line.back() == '\r') {
                  line.pop_back();
                }
                if (line.empty() || line.front() == '#') {
                  continue;
                }
                if (char* path = g_filename_from_uri(line.c_str(), nullptr, nullptr)) {
                  add(path);
                  g_free(path);
                }
              }
            },
    });
  }

  void ViewerTabs::add(const std::filesystem::path& path) {
    auto tab = std::make_unique<ViewerWindow>(m_loop, m_window);
    tab->setTitleChanged([this] { updateTitle(); });
    tab->open(path);
    if (ViewerWindow* old = active() != nullptr ? active() : m_blank.get()) {
      old->detach();
    }
    const std::size_t at = m_tabs.empty() ? 0 : m_active + 1;
    m_tabs.insert(m_tabs.begin() + static_cast<std::ptrdiff_t>(at), std::move(tab));
    m_active = at;
    updateTitle();
    m_window.requestRebuild();
  }

  ViewerWindow* ViewerTabs::active() const { return m_active < m_tabs.size() ? m_tabs[m_active].get() : nullptr; }

  void ViewerTabs::select(std::size_t index) {
    if (index >= m_tabs.size() || index == m_active) {
      return;
    }
    m_tabs[m_active]->detach();
    m_active = index;
    updateTitle();
    m_window.requestRebuild();
  }

  void ViewerTabs::step(int direction) {
    if (m_tabs.size() < 2) {
      return;
    }
    const auto n = static_cast<long>(m_tabs.size());
    select(static_cast<std::size_t>(((static_cast<long>(m_active) + direction) % n + n) % n));
  }

  void ViewerTabs::closeActive() {
    ViewerWindow* tab = active();
    if (tab == nullptr) {
      return;
    }
    if (!tab->unsaved()) {
      remove(m_active);
      return;
    }
    tab->askSaveBeforeClose([this, tab, alive = std::weak_ptr<bool>(m_alive)] {
      if (alive.expired()) {
        return;
      }
      if (const auto it = std::ranges::find(m_tabs, tab, &std::unique_ptr<ViewerWindow>::get); it != m_tabs.end()) {
        remove(static_cast<std::size_t>(it - m_tabs.begin()));
      }
    });
  }

  std::optional<std::size_t> ViewerTabs::firstUnsaved() const {
    for (std::size_t i = 0; i < m_tabs.size(); ++i) {
      if (m_tabs[i]->unsaved()) {
        return i;
      }
    }
    return std::nullopt;
  }

  void ViewerTabs::continueClose() {
    const auto unsaved = firstUnsaved();
    if (!unsaved) {
      m_window.finishClose();
      return;
    }
    select(*unsaved);
    ViewerWindow* tab = m_tabs[*unsaved].get();
    tab->askSaveBeforeClose([this, tab, alive = std::weak_ptr<bool>(m_alive)] {
      if (alive.expired()) {
        return;
      }
      // Answered: this tab no longer holds the window open; the next unsaved one asks.
      if (const auto it = std::ranges::find(m_tabs, tab, &std::unique_ptr<ViewerWindow>::get); it != m_tabs.end()) {
        if (m_tabs.size() > 1) {
          remove(static_cast<std::size_t>(it - m_tabs.begin()));
        } else {
          m_window.finishClose();
          return;
        }
      }
      continueClose();
    });
  }

  void ViewerTabs::remove(std::size_t index) {
    if (m_tabs.size() <= 1) {
      m_window.finishClose();
      return;
    }
    m_tabs.erase(m_tabs.begin() + static_cast<std::ptrdiff_t>(index));
    if (index < m_active || m_active >= m_tabs.size()) {
      m_active = m_active == 0 ? 0 : m_active - 1;
    }
    updateTitle();
    m_window.requestRebuild();
  }

  void ViewerTabs::updateTitle() {
    if (const ViewerWindow* tab = active()) {
      m_window.setTitle(tab->title());
    }
    // The strip shows every tab's title and dirty mark.
    if (m_tabs.size() >= 2) {
      m_window.requestRebuild();
    }
  }

  std::unique_ptr<Node> ViewerTabs::build(Renderer& renderer, float width, float height) {
    registerDropTarget();
    ViewerWindow* tab = active();
    if (tab == nullptr) {
      if (!m_blank) {
        m_blank = std::make_unique<ViewerWindow>(m_loop, m_window);
      }
      return m_blank->build(renderer, width, height);
    }
    if (m_tabs.size() < 2 || m_window.fullscreen()) {
      return tab->build(renderer, width, height);
    }
    const float s = m_window.uiScale();
    const float strip = Tokens::spaceP40 * s;
    auto root = ui::column({.align = FlexAlign::Stretch, .width = width, .height = height});
    root->addChild(buildStrip());
    root->addChild(tab->build(renderer, width, height - strip));
    return root;
  }

  // The Files tab strip (file_browser.cpp refreshTabs): 40 high, padding 4 8, gap 4; tab 32 high r8, padding 0 8, gap 8;
  // off = fill_1 label_2, on = lens + 0.5 lens edge label_1.
  std::unique_ptr<Node> ViewerTabs::buildStrip() {
    const float s = m_window.uiScale();
    auto strip = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP4 * s, .minHeight = Tokens::spaceP40 * s,
                          .maxHeight = Tokens::spaceP40 * s});
    strip->setPadding(Tokens::spaceP4 * s, Tokens::spaceP8 * s);
    const auto later = [this](auto fn) {
      DeferredCall::callLater([this, fn, alive = std::weak_ptr<bool>(m_alive)] {
        if (!alive.expired()) {
          fn(*this);
        }
      });
    };
    for (std::size_t i = 0; i < m_tabs.size(); ++i) {
      const bool on = i == m_active;
      auto tab = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s});
      tab->setPadding(0.0F, Tokens::spaceP8 * s);
      tab->setRadius(Style::scaledRadius(Tokens::radiusR8, s));
      tab->setFill(colorSpecFromRole(on ? ColorRole::Lens : ColorRole::Fill1));
      if (on) {
        tab->setBorder(colorSpecFromRole(ColorRole::LensEdge), 0.5F * s);
      }
      tab->setMinHeight(Tokens::sizeControlM * s);
      tab->setMaxHeight(Tokens::sizeControlM * s);
      tab->setFlexGrow(1.0F);
      tab->setClipChildren(true);
      const ColorSpec ink = colorSpecFromRole(on ? ColorRole::Label1 : ColorRole::Label2);
      const std::string title = m_tabs[i]->title();
      tab->addChild(ui::glyph({.glyph = std::string("file"), .glyphSize = Tokens::sizeSymbolInline * s, .color = ink}));
      tab->addChild(ui::label({.text = title, .fontSize = Style::fontSizeCaption * s, .fontWeight = FontWeight::Medium,
                               .color = ink, .maxLines = 1, .ellipsize = TextEllipsize::Middle, .flexGrow = 1.0F}));
      auto area = ui::inputArea({.cursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT, .participatesInLayout = false});
      area->setOnClick([later, i](const InputArea::PointerData&) { later([i](ViewerTabs& t) { t.select(i); }); });
      Accessible& a = area->accessibleState();
      a.role = AccessibleRole::Tab;
      a.name = title;
      a.selected = on;
      tab->addChild(std::move(area));
      Button* close = nullptr;
      tab->addChild(ui::button({
          .out = &close,
          .glyph = std::string("x"),
          .glyphSize = Tokens::sizeSymbolInline * s,
          .variant = ButtonVariant::Ghost,
          .tooltip = std::string("Close Tab"),
          .radius = Tokens::spaceP20 * s / 2.0F,
          .width = Tokens::spaceP20 * s,
          .height = Tokens::spaceP20 * s,
          .onClick = [later, i] {
            later([i](ViewerTabs& t) {
              t.select(i);
              t.closeActive();
            });
          },
      }));
      close->setZIndex(1); // above the tab's input area
      strip->addChild(std::move(tab));
    }
    return strip;
  }

} // namespace viewer
