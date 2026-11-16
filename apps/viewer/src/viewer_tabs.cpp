#include "viewer_tabs.h"

#include "app/app_loop.h"
#include "app/app_window.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
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

#include <algorithm>
#include <format>
#include <gio/gio.h>
#include <sstream>

namespace viewer {

  ViewerTabs::ViewerTabs(AppLoop& loop, AppWindow& window, StateStore& state)
      : m_loop(loop), m_window(window), m_state(state), m_split(window, splitCallbacks()) {
    // A job that ends gives its room to the other shown documents' queues.
    m_budget.released = [this](ViewerWindow* from) {
      DeferredCall::callLater([this, from, alive = std::weak_ptr<bool>(m_alive)] {
        if (alive.expired()) {
          return;
        }
        for (const std::uint64_t id : m_set.visible()) {
          if (ViewerWindow* doc = m_docs.at(id).get(); doc != from) {
            doc->resume();
          }
        }
      });
    };
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
      for (const std::uint64_t id : m_set.visible()) {
        m_docs.at(id)->onLayout();
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
    auto tab = std::make_unique<ViewerWindow>(m_loop, m_window, m_state, m_budget);
    tab->setTitleChanged([this] { updateTitle(); });
    tab->open(path);
    if (ViewerWindow* old = active() != nullptr ? active() : m_blank.get()) {
      old->detach();
    }
    if (m_set.tabs().empty() && !m_immersive) {
      m_window.setSidebarVisible(true);
    }
    const std::uint64_t id = m_nextId++;
    m_docs.emplace(id, std::move(tab));
    m_set.apply(tab_event::Open{id});
    changed();
  }

  ViewerWindow* ViewerTabs::active() const {
    const auto id = m_set.focused();
    return id ? m_docs.at(*id).get() : nullptr;
  }

  std::optional<std::uint64_t> ViewerTabs::idOf(const ViewerWindow* tab) const {
    const auto it = std::ranges::find_if(m_docs, [tab](const auto& d) { return d.second.get() == tab; });
    return it != m_docs.end() ? std::optional(it->first) : std::nullopt;
  }

  void ViewerTabs::select(std::uint64_t id) {
    ViewerWindow* old = active();
    const auto* split = m_set.splitOf(id);
    const bool inPlace = m_split.split() && split != nullptr && m_set.focused() && split == m_set.splitOf(*m_set.focused());
    if (m_set.focused() == id || !m_set.apply(tab_event::Focus{id})) {
      return;
    }
    // A pane of the shown split: the ring, caption tools and sidebar move; no pane rebuilds.
    if (inPlace) {
      old->blur();
      m_split.setFocused(id, true);
      updateTitle();
      m_window.rebuildSidebar();
      return;
    }
    old->detach();
    changed();
  }

  void ViewerTabs::step(int direction) {
    const auto& tabs = m_set.tabs();
    if (tabs.size() < 2) {
      return;
    }
    const auto n = static_cast<long>(tabs.size());
    const auto at = static_cast<long>(std::ranges::find(tabs, *m_set.focused()) - tabs.begin());
    select(tabs[static_cast<std::size_t>(((at + direction) % n + n) % n)]);
  }

  void ViewerTabs::closeActive() {
    ViewerWindow* tab = active();
    if (tab == nullptr) {
      return;
    }
    if (!tab->unsaved()) {
      remove(*m_set.focused());
      return;
    }
    tab->askSaveBeforeClose([this, tab, alive = std::weak_ptr<bool>(m_alive)] {
      if (alive.expired()) {
        return;
      }
      if (const auto id = idOf(tab)) {
        remove(*id);
      }
    });
  }

  std::optional<std::uint64_t> ViewerTabs::firstUnsaved() const {
    const auto& tabs = m_set.tabs();
    const auto it = std::ranges::find_if(tabs, [this](std::uint64_t id) { return m_docs.at(id)->unsaved(); });
    return it != tabs.end() ? std::optional(*it) : std::nullopt;
  }

  void ViewerTabs::continueClose() {
    const auto unsaved = firstUnsaved();
    if (!unsaved) {
      m_window.finishClose();
      return;
    }
    select(*unsaved);
    ViewerWindow* tab = m_docs.at(*unsaved).get();
    tab->askSaveBeforeClose([this, tab, alive = std::weak_ptr<bool>(m_alive)] {
      if (alive.expired()) {
        return;
      }
      // Answered: this tab no longer holds the window open; the next unsaved one asks.
      if (const auto id = idOf(tab)) {
        if (m_set.tabs().size() > 1) {
          remove(*id);
        } else {
          m_window.finishClose();
          return;
        }
      }
      continueClose();
    });
  }

  void ViewerTabs::remove(std::uint64_t id) {
    if (m_set.tabs().size() <= 1) {
      m_window.finishClose();
      return;
    }
    m_split.noteClosing(id);
    m_set.apply(tab_event::Close{id});
    m_docs.erase(id);
    changed();
  }

  void ViewerTabs::changed() {
    updateTitle();
    m_window.setContentCards(m_set.visible().size() > 1);
    m_window.requestRebuild();
  }

  void ViewerTabs::clickRow(std::uint64_t id) {
    const auto focused = m_set.focused();
    if ((m_loop.wayland().keyboardModifiers() & KeyMod::Ctrl) == 0U || !focused || id == *focused) {
      select(id);
      return;
    }
    if (m_set.splitOf(id) != nullptr && m_set.splitOf(id) == m_set.splitOf(*focused)) {
      m_set.apply(tab_event::Leave{id});
      changed();
      return;
    }
    ViewerWindow* old = active();
    if (m_set.splitOf(id) == nullptr && m_set.apply(tab_event::Join{.target = *focused, .joining = id})) {
      m_split.pendJoin({.target = *focused, .joining = id});
      old->detach();
      changed();
      return;
    }
    select(id);
  }

  SplitView::Callbacks ViewerTabs::splitCallbacks() {
    return {
        .splitOf = [this](std::uint64_t id) { return m_set.splitOf(id); },
        .content = [this](Renderer& renderer, std::uint64_t id, float width,
                          float height) { return m_docs.at(id)->build(renderer, width, height); },
        .caption = [](std::uint64_t) { return SplitView::Caption{}; },
        .surface = [](std::uint64_t) { return colorSpecFromRole(ColorRole::SolidRegular); },
        .focus = [this](std::uint64_t id) { select(id); },
        .minimum =
            [this] {
              const float s = m_window.uiScale();
              return std::pair{Tokens::componentViewerSplitMinWidth * s, Tokens::componentViewerSplitMinHeight * s};
            },
        // A pane's fit width follows its new size.
        .settle = [this](std::uint64_t, float, float) { m_window.requestRebuild(); },
        .ring = [] { return true; },
    };
  }

  void ViewerTabs::updateTitle() {
    if (const ViewerWindow* tab = active()) {
      m_window.setTitle(tab->title());
      m_window.setCaption({.state = tab->unsaved() ? "Edited" : ""});
    }
    bool texts = false;
    for (const std::uint64_t id : m_set.visible()) {
      const ViewerWindow& doc = *m_docs.at(id);
      texts = m_split.setCaption(id, doc.title(), doc.unsaved() ? "Edited" : "", colorSpecFromRole(ColorRole::Label2)) ||
              texts;
    }
    if (texts) {
      m_window.updateInPlace([this](Renderer& renderer) { m_split.fitCaptions(renderer); });
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
    if (active() == nullptr) {
      m_split.reset();
      if (!m_blank) {
        m_blank = std::make_unique<ViewerWindow>(m_loop, m_window, m_state, m_budget);
      }
      return m_blank->build(renderer, width, height);
    }
    auto slot = ui::column({.align = FlexAlign::Stretch, .width = width, .height = height});
    m_split.fill(renderer, slot.get(), width, height, m_set.visible(), *m_set.focused(), m_window.contentCards());
    if (m_split.split()) {
      for (const std::uint64_t id : m_set.visible()) {
        const ViewerWindow& doc = *m_docs.at(id);
        m_split.setCaption(id, doc.title(), doc.unsaved() ? "Edited" : "", colorSpecFromRole(ColorRole::Label2));
      }
      m_split.fitCaptions(renderer);
    }
    return slot;
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
    if (m_set.tabs().size() >= 2 || !tab->isPdf()) {
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
    const auto& tabs = m_set.tabs();
    const bool cut = tabs.size() > kShown + 1 && !m_allDocuments;
    auto group = ui::column({.align = FlexAlign::Stretch, .gap = Tokens::spaceP4 * s});
    auto head = ui::row({.align = FlexAlign::Center});
    head->setPadding(0.0F, Tokens::spaceP8 * s);
    const ColorSpec footnote = colorSpecFromRole(ColorRole::Label2);
    head->addChild(ui::label({.text = "Documents", .fontSize = Tokens::typeFootnote.size * s,
                              .fontWeight = FontWeight::SemiBold, .color = footnote, .flexGrow = 1.0F}));
    head->addChild(ui::label({.text = std::to_string(tabs.size()), .fontSize = Tokens::typeFootnote.size * s,
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
    std::vector<std::uint64_t> rows(tabs.begin(), cut ? tabs.begin() + kShown : tabs.end());
    if (cut && std::ranges::find(rows, *m_set.focused()) == rows.end()) {
      rows.back() = *m_set.focused();
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
      area->setSize(width, Tokens::componentSidebarRow * s);
      area->setOnClick([later, run](const InputArea::PointerData&) { later(run); });
      Accessible& a = area->accessibleState();
      a.role = AccessibleRole::Tab;
      a.name = std::move(name);
      a.selected = on;
      r->addChild(std::move(area));
      group->addChild(std::move(r));
    };
    for (const std::uint64_t id : rows) {
      const std::string title = m_docs.at(id)->title();
      row("file", title, id == m_set.focused(), ColorRole::Label1, [id](ViewerTabs& t) { t.clickRow(id); }, title);
    }
    if (tabs.size() > kShown + 1) {
      const std::string text = cut ? std::format("{} more", tabs.size() - rows.size()) : std::string("Show fewer");
      row("dots", text, false, ColorRole::Label2, [](ViewerTabs& t) {
        t.m_allDocuments = !t.m_allDocuments;
        t.m_window.requestRebuild();
      }, text);
    }
    return group;
  }

} // namespace viewer
