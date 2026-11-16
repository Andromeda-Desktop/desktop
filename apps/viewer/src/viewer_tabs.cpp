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

#include <format>
#include <gio/gio.h>
#include <sstream>

namespace viewer {

  ViewerTabs::ViewerTabs(AppLoop& loop, AppWindow& window, StateStore& state)
      : m_loop(loop), m_window(window), m_state(state) {
    m_window.setContentBuilder([this](Renderer& renderer, float width, float height) {
      return build(renderer, width, height);
    });
    m_window.setHeaderAccessoryBuilder([this]() -> std::unique_ptr<Node> {
      ViewerWindow* tab = active() != nullptr ? active() : m_blank.get();
      auto caption = tab != nullptr ? tab->buildCaption() : ui::row({});
      m_caption = caption.get();
      return caption;
    });
    // Page, zoom and tools keep their width; the title ellipsizes first.
    m_window.setHeaderAccessoryWidths([this](Renderer& renderer) {
      // The children, not the row: fit() sets the row's minimum width.
      auto* row = dynamic_cast<Flex*>(m_caption);
      if (row == nullptr) {
        return AppWindow::HeaderAccessoryWidths{};
      }
      float width = 0.0F;
      int shown = 0;
      for (const auto& child : row->children()) {
        if (child->visible() && child->participatesInLayout()) {
          width += child->measure(renderer, {}).width;
          ++shown;
        }
      }
      if (shown > 1) {
        width += row->gap() * static_cast<float>(shown - 1);
      }
      return AppWindow::HeaderAccessoryWidths{.floor = width, .keep = width};
    });
    m_window.setSidebarBuilder([this](Renderer& renderer, float width, float /*height*/) {
      return buildSidebar(renderer, width);
    });
    // The page column lays out once, at the sidebar's rest width; the caption shows Find while it is hidden.
    m_window.setSidebarSettled([this] { m_window.requestRebuild(); });
    m_window.setSidebarVisible(false);
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
    auto tab = std::make_unique<ViewerWindow>(m_loop, m_window, m_state);
    tab->setTitleChanged([this] { updateTitle(); });
    tab->open(path);
    if (ViewerWindow* old = active() != nullptr ? active() : m_blank.get()) {
      old->detach();
    }
    if (m_tabs.empty() && !m_immersive) {
      m_window.setSidebarVisible(true);
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
      m_window.setCaption({.state = tab->unsaved() ? "Edited" : ""});
    }
  }

  void ViewerTabs::syncImmersive() {
    if (m_window.fullscreen() == m_immersive) {
      return;
    }
    m_immersive = m_window.fullscreen();
    // Not inside the build that noticed it: the sidebar spring drives the nodes that build makes.
    DeferredCall::callLater([this, immersive = m_immersive, alive = std::weak_ptr<bool>(m_alive)] {
      if (alive.expired()) {
        return;
      }
      if (immersive) {
        m_sidebarBeforeImmersive = m_window.sidebarVisible();
        m_window.setSidebarVisible(false);
      } else {
        m_window.setSidebarVisible(m_sidebarBeforeImmersive);
      }
    });
  }

  std::unique_ptr<Node> ViewerTabs::build(Renderer& renderer, float width, float height) {
    registerDropTarget();
    syncImmersive();
    ViewerWindow* tab = active();
    if (tab == nullptr) {
      if (!m_blank) {
        m_blank = std::make_unique<ViewerWindow>(m_loop, m_window, m_state);
      }
      return m_blank->build(renderer, width, height);
    }
    return tab->build(renderer, width, height);
  }

  // Concept C (.agent/DESIGN-viewer.md "Chrome v2"): search, Documents, then the switch and its list. The order is fixed:
  // nothing above the switch moves when a search opens.
  std::unique_ptr<Node> ViewerTabs::buildSidebar(Renderer& renderer, float width) {
    const float s = m_window.uiScale();
    const float pad = Tokens::spaceP8 * s;
    auto side = ui::column({.align = FlexAlign::Stretch, .gap = pad, .padding = pad, .width = width});
    ViewerWindow* tab = active();
    if (tab == nullptr) {
      return side;
    }
    if (auto search = tab->buildSearch()) {
      side->addChild(std::move(search));
    }
    if (m_tabs.size() >= 2 || !tab->isPdf()) {
      side->addChild(buildDocuments(width - 2.0F * pad));
    }
    if (auto list = tab->buildSidebarList(renderer, width - 2.0F * pad)) {
      side->addChild(std::move(list));
    }
    return side;
  }

  // One 32 row per document; past 4, the first 3 and a "N more" row that steps to the next one.
  std::unique_ptr<Node> ViewerTabs::buildDocuments(float width) {
    const float s = m_window.uiScale();
    constexpr std::size_t kShown = 3;
    const bool cut = m_tabs.size() > kShown + 1 && !m_allDocuments;
    auto group = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP4 * s});
    auto head = ui::row({.align = FlexAlign::Center});
    head->setPadding(0.0F, Tokens::spaceP8 * s);
    const ColorSpec footnote = colorSpecFromRole(ColorRole::Label2);
    head->addChild(ui::label({.text = "Documents", .fontSize = Tokens::typeFootnote.size * s,
                              .fontWeight = FontWeight::SemiBold, .color = footnote, .flexGrow = 1.0F}));
    head->addChild(ui::label({.text = std::to_string(m_tabs.size()), .fontSize = Tokens::typeFootnote.size * s,
                              .fontWeight = FontWeight::Medium, .color = footnote}));
    group->addChild(std::move(head));
    const auto later = [this](auto fn) {
      DeferredCall::callLater([this, fn, alive = std::weak_ptr<bool>(m_alive)] {
        if (!alive.expired()) {
          fn(*this);
        }
      });
    };
    // The active document always shows: past the cut it takes the last shown row.
    std::vector<std::size_t> rows;
    for (std::size_t i = 0; i < m_tabs.size() && (!cut || i < kShown); ++i) {
      rows.push_back(i);
    }
    if (cut && m_active >= kShown) {
      rows.back() = m_active;
    }
    const auto row = [&](std::string glyph, std::string text, bool on, ColorRole ink, std::function<void(ViewerTabs&)> run,
                         std::string name) {
      auto r = ui::row({.align = FlexAlign::Center, .gap = Tokens::spaceP8 * s,
                        .fill = on ? colorSpecFromRole(ColorRole::Accent, isResolvedLightTheme() ? 0.32F : 0.24F)
                                   : clearColorSpec(),
                        .radius = Style::scaledRadius(Tokens::componentSidebarRowRadius, s),
                        .width = width, .height = Tokens::componentSidebarRow * s});
      r->setPadding(0.0F, Tokens::spaceP8 * s);
      r->addChild(ui::glyph({.glyph = std::move(glyph), .glyphSize = Tokens::sizeSymbolInline * s,
                             .color = colorSpecFromRole(ColorRole::Label2)}));
      r->addChild(ui::label({.text = std::move(text), .fontSize = Style::fontSizeCaption * s,
                             .fontWeight = on ? FontWeight::SemiBold : FontWeight::Normal,
                             .color = colorSpecFromRole(ink), .maxLines = 1, .ellipsize = TextEllipsize::End,
                             .flexGrow = 1.0F}));
      auto area = ui::inputArea({.cursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER, .participatesInLayout = false});
      area->setOnClick([later, run](const InputArea::PointerData&) { later(run); });
      Accessible& a = area->accessibleState();
      a.role = AccessibleRole::Tab;
      a.name = std::move(name);
      a.selected = on;
      r->addChild(std::move(area));
      group->addChild(std::move(r));
    };
    for (const std::size_t i : rows) {
      const std::string title = m_tabs[i]->title();
      row("file", title, i == m_active, ColorRole::Label1, [i](ViewerTabs& t) { t.select(i); }, title);
    }
    if (m_tabs.size() > kShown + 1) {
      const std::string text = cut ? std::format("{} more", m_tabs.size() - rows.size()) : std::string("Show fewer");
      row("dots", text, false, ColorRole::Label2, [](ViewerTabs& t) {
        t.m_allDocuments = !t.m_allDocuments;
        t.m_window.requestRebuild();
      }, text);
    }
    return group;
  }

} // namespace viewer
