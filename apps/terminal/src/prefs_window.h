#pragma once

#include "settings_store.h"
#include "render/scene/input_area.h"
#include "ui/controls/scroll_view.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class AppWindow;
class Flex;
class Input;
class Node;
class Renderer;

namespace term {

  // The Preferences window: a live preview of the current settings fills the window; every setting sits in one
  // searchable column beside it, with the shortcut list as its one sub-page. Every control writes the preferences
  // file at once; the file's changes (also hand edits) rebuild the preview and the column.
  class PrefsWindow {
  public:
    // The Add theme tile's menu.
    struct ThemeActions {
      std::function<void()> createWithAgent; // a new window with an agent session that makes a theme with the user
      std::function<void()> openFolder;      // the themes folder in the file manager
      std::function<void()> importPackage;   // asks for a package folder, then calls importTheme with it
    };
    PrefsWindow(AppWindow& window, SettingsStore& store, ThemeActions actions);
    // Copies the package into the themes folder and uses it; a bad package shows an error card instead.
    void importTheme(const std::filesystem::path& folder);
    PrefsWindow(const PrefsWindow&) = delete;
    PrefsWindow& operator=(const PrefsWindow&) = delete;

  private:
    enum class Pane : std::uint8_t { Main, Keyboard };
    struct Row {
      std::string label;
      std::string detail; // a second line under the label; empty for none
      std::function<std::unique_ptr<Node>(float scale, float width)> control; // width: the card's inner width
      bool wide = false; // the control fills the card; no label column
    };
    struct Group {
      Pane pane;
      std::string title; // empty: no heading
      std::vector<Row> rows;
      bool bare = false;        // the rows sit on the window, not in a card (tile grids)
      bool themeErrors = false; // the theme and import error cards follow this group
    };

    [[nodiscard]] std::vector<Group> groups() const;
    [[nodiscard]] std::unique_ptr<Node> buildColumn(float width, float height);
    [[nodiscard]] std::unique_ptr<Node> preview(float scale, float width, float height) const;
    [[nodiscard]] std::unique_ptr<Node> themeTiles(float scale, float width);
    [[nodiscard]] std::unique_ptr<Node> materialTiles(float scale, float width);
    // The selected tile's swatch with the accent check badge at its bottom-right corner.
    [[nodiscard]] static std::unique_ptr<Node> checked(std::unique_ptr<Node> swatch, bool on, float scale);
    // A clickable choice tile: the swatch (tileW x tileH, ringed and checked when on) over its name.
    [[nodiscard]] static std::unique_ptr<Node> tile(std::unique_ptr<Flex> swatch, std::string_view name, bool on, float scale,
                                                    float tileW, float tileH,
                                                    std::function<void(const InputArea::PointerData&)> pick);
    [[nodiscard]] std::unique_ptr<Flex> card(const Group& group, float scale, float width) const;
    void change(const std::function<void(Settings&)>& edit);
    void showPane(Pane pane);
    void setQuery(std::string query);
    void refill();

    AppWindow& m_window;
    SettingsStore& m_store;
    ThemeActions m_actions;
    struct ImportError {
      std::string folder;
      SettingsError error;
    };
    std::optional<ImportError> m_importError; // the last import, when it failed
    Pane m_pane = Pane::Main;
    std::string m_query;
    Input* m_search = nullptr;
    ScrollViewState m_scroll;
    // refill() swaps the content of both slots; the search field above the column keeps its focus.
    Flex* m_previewSlot = nullptr;
    Flex* m_slot = nullptr;
    float m_previewWidth = 0.0F;
    float m_previewHeight = 0.0F;
    float m_slotWidth = 0.0F;
    float m_slotHeight = 0.0F;
    std::vector<std::string> m_monoFonts; // families with fixed-width faces, sorted
    std::vector<std::string> m_shells;    // /etc/shells entries that exist
    Signal<>::ScopedConnection m_storeChanged;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  };

} // namespace term
