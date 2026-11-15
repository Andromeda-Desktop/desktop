#pragma once

#include "settings_store.h"
#include "ui/controls/scroll_view.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class AppWindow;
class Flex;
class Node;
class Renderer;

namespace term {

  // The Preferences window: a page list with search on the side, grouped cards of rows in the content. Every
  // control writes the preferences file at once; the file's changes (also hand edits) rebuild the content.
  class PrefsWindow {
  public:
    PrefsWindow(AppWindow& window, SettingsStore& store);
    PrefsWindow(const PrefsWindow&) = delete;
    PrefsWindow& operator=(const PrefsWindow&) = delete;

  private:
    enum class Page : std::uint8_t { Appearance, Text, Terminal, Sessions, Notifications, Keyboard };
    struct Row {
      std::string label;
      std::string detail; // a second line under the label; empty for none
      std::function<std::unique_ptr<Node>(float scale, float width)> control; // width: the card's inner width
      bool wide = false; // the control fills the card; no label column
    };
    struct Group {
      Page page;
      std::string title; // empty: no heading
      std::vector<Row> rows;
    };

    [[nodiscard]] std::vector<Group> groups() const;
    [[nodiscard]] std::unique_ptr<Node> buildPage(float width, float height);
    [[nodiscard]] std::unique_ptr<Node> buildSidebar(float width);
    [[nodiscard]] std::unique_ptr<Node> preview(float scale, float width) const;
    [[nodiscard]] std::unique_ptr<Node> themeTiles(float scale, float width);
    [[nodiscard]] std::unique_ptr<Flex> card(const Group& group, float scale, float width) const;
    void change(const std::function<void(Settings&)>& edit);
    void showPage(Page page);
    void setQuery(std::string query);
    void refill();

    AppWindow& m_window;
    SettingsStore& m_store;
    Page m_page = Page::Appearance;
    std::string m_query;
    ScrollViewState m_scroll;
    Flex* m_slot = nullptr; // the content column refill() swaps
    float m_slotWidth = 0.0F;
    float m_slotHeight = 0.0F;
    std::vector<std::string> m_monoFonts; // families with fixed-width faces, sorted
    std::vector<std::string> m_shells;    // /etc/shells entries that exist
    Signal<>::ScopedConnection m_storeChanged;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  };

} // namespace term
