#pragma once

#include "ui/controls/context_menu.h"
#include "ui/palette.h"

#include "core/timer_manager.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "wayland/toplevel_surface.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

class AppLoop;
class Button;
class ContextMenuPopup;
class CommandTable;
class Flex;
class Glyph;
class Image;
class Label;
class Renderer;
class ShaderSceneNode;
class Subsurface;
class StateStore;
struct KeyboardEvent;
struct PinchEvent;
struct PointerEvent;

// A first-party app window (docs/apps.md §2, rulebook §9.12): the 48 px app header with back / forward, title and
// window buttons, over the app's content. Size and maximized state persist in the app's StateStore under
// /window/<stateKey>/.
namespace desk {
  class ForeignParent;
}

class AppWindow {
public:
  // Glass: the compositor blurs what lies below the whole window; the sidebar draws the Sheet tint over it and the main
  // pane the thick tint. Content that paints its own background must make it translucent to show the glass.
  enum class Material : std::uint8_t { Solid, Glass };

  struct Config {
    std::string appId;
    std::string title;
    std::string stateKey = "main";
    std::uint32_t width = 960; // logical px before ui scale, used when no state is saved
    std::uint32_t height = 640;
    std::uint32_t minWidth = 480;
    std::uint32_t minHeight = 320;
    // The window keeps width x height (min = max size, no saved size): compositors float it like a dialog.
    bool fixedSize = false;
    bool navigation = false; // back / forward buttons in the header (commands go.back, go.forward)
    float sidebarWidth = 0.0F; // logical px before ui scale; 0 = no sidebar
    Material material = Material::Solid;
    std::string parentWindow;  // portal form "wayland:<xdg-foreign handle>": a dialog of another app's window
    bool transparentContent = false; // the content area shows what lies below the window surface (a video subsurface)
    bool contentCard = false; // the content is a rounded card inset card_gap on the sidebar material
    bool headerWhenFullscreen = true;
    bool header = true; // false: a chromeless window (picture in picture); the content is the whole window
    bool moreMenu = false; // a More (…) circle before the window buttons: the commands by group (File, Edit, View, Go)
  };

  // Builds the content under the header; called on open, on requestRebuild() and when the scale changes.
  using ContentBuilder = std::function<std::unique_ptr<Node>(Renderer& renderer, float width, float height)>;
  // The full-height sidebar left of the header and content (rulebook §9.12, Settings-style opaque sidebar).
  using SidebarBuilder = std::function<std::unique_ptr<Node>(Renderer& renderer, float width, float height)>;

  AppWindow(AppLoop& loop, CommandTable& commands, StateStore& state, Config config);
  ~AppWindow();

  AppWindow(const AppWindow&) = delete;
  AppWindow& operator=(const AppWindow&) = delete;

  bool open();
  void close();
  [[nodiscard]] bool isOpen() const noexcept { return m_surface != nullptr && m_surface->isRunning(); }

  void setContentBuilder(ContentBuilder builder) { m_builder = std::move(builder); }
  void setSidebarBuilder(SidebarBuilder builder) { m_sidebarBuilder = std::move(builder); }
  // App controls in the header between the title and the window buttons (rulebook 9.12: view switch, search).
  void setHeaderAccessoryBuilder(std::function<std::unique_ptr<Node>()> builder) {
    m_headerAccessory = std::move(builder);
  }
  // floor: the accessory never folds below it; past it the title ellipsizes instead. keep: the accessory keeps this
  // while the title ellipsizes down to its minimum; above it the accessory only gets room the full title leaves.
  struct HeaderAccessoryWidths {
    float floor = 0.0F;
    float keep = 0.0F;
  };
  void setHeaderAccessoryWidths(std::function<HeaderAccessoryWidths(Renderer&)> widths) {
    m_headerAccessoryWidths = std::move(widths);
  }
  // The Solid main pane's plane, header included: the content's surface, so the header and the content read as one
  // plane. nullopt = solid_regular. Glass keeps its sheet tint.
  void setMainFill(std::optional<ColorSpec> fill);
  // A theme scene behind the whole window, with no blur. The sidebar and header take the text tint: the least surface
  // alpha at which label 2 keeps 4.5:1 over every sampled scene colour (rulebook 5.3). The main pane takes the light
  // `tint` when the content backs its own text (setSceneContentBacked), else the text tint.
  struct Scene {
    struct Shader {
      std::string source; // the fragment shader body (ShaderSceneNode contract)
      bool operator==(const Shader&) const = default;
    };
    struct Picture {
      std::string path; // an image file, drawn to cover the window; it never moves
      std::int64_t stamp = 0;
      bool operator==(const Picture&) const = default;
    };
    using Backdrop = std::variant<Shader, Picture>;
    Backdrop backdrop;
    Color surface;
    Color text;
    Color accent;
    Color label2;
    bool dark = false;
    float tint = 1.0F; // the surface's alpha over the scene where nothing needs it for contrast
    bool operator==(const Scene&) const = default;
  };
  void setScene(std::optional<Scene> scene);
  // Input, output or any change the user sees: the scene moves for scene_idle_ms after the last one, while the window is
  // active. Keys and pointer events in this window count already.
  void noteSceneActivity();
  // Reduced motion or battery saver: the scene stays on its current frame until this turns false.
  void setSceneHeld(bool held);
  // The content draws a backing under its own text (sceneTextBacking), so the main pane can stay light.
  void setSceneContentBacked(bool backed);
  // The surface alpha that, over the main pane's light tint, gives text the text tint; 0 without a scene.
  [[nodiscard]] float sceneTextBacking() const noexcept;
  [[nodiscard]] float headerHeight() const noexcept; // surface px
  [[nodiscard]] AnimationManager& animations() noexcept { return m_animations; }
  // Keys no command took and no text field owns (type-ahead, arrows in a list). True = handled.
  void setKeyHandler(std::function<bool(const KeyboardEvent&)> handler) { m_keyHandler = std::move(handler); }
  // Every key (press, repeat, release) before the window commands, when no menu, popover or sheet is open.
  // True = handled. A terminal uses it: its Ctrl and Alt chords belong to the program it runs.
  void setFirstKeyHandler(std::function<bool(const KeyboardEvent&)> handler) { m_firstKeyHandler = std::move(handler); }
  // A window-modal sheet (rulebook 9.10): a scrim over the window and the card centred on it. The builder runs on
  // each scene build; Escape calls onCancel. Window keys and commands wait while it shows.
  void presentSheet(std::function<std::unique_ptr<Node>(float scale)> builder, std::function<void()> onCancel);
  void dismissSheet();
  // Collapses or restores the sidebar on spring.page (the Files Gallery is full-bleed); the header stays in place.
  void setSidebarVisible(bool visible);
  [[nodiscard]] bool sidebarVisible() const noexcept { return m_sidebarVisible; }
  // Asks for a new window size (logical px before ui scale). False when the compositor sizes the window: then the
  // size becomes the minimum, so the compositor grows it.
  bool requestSize(std::uint32_t width, std::uint32_t height);
  // Saves the current geometry under the old key, then remembers size and state under this one.
  void setStateKey(std::string stateKey);
  [[nodiscard]] bool hasSheet() const noexcept { return static_cast<bool>(m_sheetBuilder); }
  void setClosedCallback(std::function<void()> callback) { m_onClosed = std::move(callback); }
  // Runs when the compositor activates or deactivates the window (xdg_toplevel activated state).
  void setActiveCallback(std::function<void(bool active)> callback) { m_onActive = std::move(callback); }
  void setTitle(std::string title);
  // contentCard: the caption's status symbol and state footnote beside the title; empty parts hide.
  struct Caption {
    std::string symbol;
    std::string state;
    bool danger = false; // a failure: symbol and state in Danger
    bool spinning = false; // a spinner replaces the symbol: work runs
    bool operator==(const Caption&) const = default;
  };
  void setCaption(Caption caption);
  // contentCard: a menu button ends the caption; it calls `open` with the scene point below it. Set before the first build.
  void setCaptionMenu(std::function<void(float sceneX, float sceneY)> open) { m_captionMenu = std::move(open); }
  // contentCard: the content draws its own cards (a split view), so the window draws no main card and no header. The
  // content puts buildWindowButtons() and buildHeaderAccessory() in its captions. Rebuilds the window.
  void setContentCards(bool on);
  [[nodiscard]] bool contentCards() const noexcept { return m_contentCards; }
  [[nodiscard]] std::unique_ptr<Node> buildWindowButtons();
  [[nodiscard]] std::unique_ptr<Node> buildHeaderAccessory() { return m_headerAccessory ? m_headerAccessory() : nullptr; }
  // An interactive move from a pointer press with this serial.
  void beginMove(std::uint32_t serial);
  // The edit menu of a text field in this window; false when the field is not in it.
  bool openTextMenu(class Input& input, float sceneX, float sceneY);
  // A click on the header title (Files: type a location, as Dolphin's editable path). Applies from the next rebuild.
  void setTitleClick(std::function<void()> click) { m_titleClick = std::move(click); }
  // Search mode (Files): the header hides back / forward, the title and More; the accessory takes the row.
  void setHeaderSearchMode(bool on);
  void requestRebuild();
  // Updates the back / forward buttons from their commands without a rebuild.
  void refreshNavigation();
  // Repaints or relayouts after a node changed in place.
  void invalidate();
  // Runs change with the GL context current (texture uploads outside a frame), then repaints what it changed.
  void updateInPlace(const std::function<void(Renderer&)>& change);
  // Raises the window for a launch that carried an xdg-activation token.
  void activate(const std::string& token);
  // A node over everything that follows the pointer (a dragged item's ghost); input passes through it. A rebuild drops
  // it, and a rebuild waits while a button is down.
  void setDragGhost(std::unique_ptr<Node> ghost);
  // The ghost's top-left at a scene point.
  void moveDragGhost(float sceneX, float sceneY);
  void clearDragGhost();
  // A context menu at a point in window (scene) coordinates; `activated` gets the entry id, after the menu closed.
  void openContextMenu(std::vector<ContextMenuControlEntry> entries, float sceneX, float sceneY,
                       std::function<void(std::int32_t id)> activated);
  // A popover under `anchor` (a node of this window), `width` x `height` logical px before ui scale.
  void openPopover(const Node* anchor, float width, float height, std::function<std::unique_ptr<Node>(float scale)> content);
  void refreshPopover();
  // The open popover takes a new anchor and size in place (same grab), then builds its content again.
  void resizePopover(const Node* anchor, float width, float height);
  void invalidatePopover(); // its nodes changed in place: relayout and repaint, no rebuild
  void closePopover();
  [[nodiscard]] bool popoverOpen() const noexcept;
  void toggleMaximized();
  void toggleFullscreen();
  [[nodiscard]] bool fullscreen() const noexcept;
  // The content area in surface coordinates (logical px), as of the last layout.
  struct Rect {
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
  };
  [[nodiscard]] Rect contentRect() const noexcept { return m_contentRect; }
  // After each layout (size, scale or fullscreen change).
  void setLayoutCallback(std::function<void()> callback) { m_onLayout = std::move(callback); }

  [[nodiscard]] wl_surface* wlSurface() const noexcept;
  [[nodiscard]] Material material() const noexcept { return m_config.material; }
  void setMaterial(Material material);
  // This window's surface or its open menu popup: the app routes that surface's events here.
  [[nodiscard]] bool ownsSurface(wl_surface* surface) const noexcept;
  [[nodiscard]] float uiScale() const noexcept { return m_uiScale; }
  [[nodiscard]] InputDispatcher& input() noexcept { return m_input; }
  [[nodiscard]] ToplevelSurface* surface() noexcept { return m_surface.get(); }

  // The app routes events here only for this window's surface (DeskApp::routePointer, routeKey).
  void onPointerEvent(const PointerEvent& event);
  void onPinchEvent(const PinchEvent& event);
  void onAxisStop(std::uint32_t axis);
  void onKeyboardEvent(const KeyboardEvent& event);

  // Builds the scene before the loop runs, while EGL loads: the build needs no GL.
  void prepareFirstFrame() { prepareFrame(true); }

private:
  void attachSheet(Renderer& renderer, float width, float height, float margin);
  void detachSheet();
  void prepareFrame(bool needsLayout);
  void buildScene(Renderer& renderer, float width, float height);
  [[nodiscard]] std::unique_ptr<Flex> buildHeader();
  void saveGeometry();
  // xdg_toplevel resize edges under the pointer, 0 outside the band or while maximized.
  [[nodiscard]] std::uint32_t resizeEdgesAt(float x, float y) const;
  void onClosedByCompositor();
  [[nodiscard]] std::string key(const char* field) const;
  void openMoreMenu();
  void applyHeaderSearchMode();

  AppLoop& m_loop;
  CommandTable& m_commands;
  StateStore& m_state;
  Config m_config;
  ContentBuilder m_builder;
  SidebarBuilder m_sidebarBuilder;
  std::function<std::unique_ptr<Node>()> m_headerAccessory;
  std::function<HeaderAccessoryWidths(Renderer&)> m_headerAccessoryWidths;
  std::function<bool(const KeyboardEvent&)> m_keyHandler;
  std::function<bool(const KeyboardEvent&)> m_firstKeyHandler;
  std::function<void(bool)> m_onActive;
  std::function<std::unique_ptr<Node>(float)> m_sheetBuilder;
  std::function<void()> m_sheetCancel;
  Node* m_sheetCard = nullptr;
  Node* m_overlay = nullptr; // the last root child: holds the drag ghost
  Node* m_ghost = nullptr;
  Node* m_sheetShade = nullptr;
  class InputArea* m_sheetScrim = nullptr;
  bool m_sheetHandoff = false; // dismissed this turn: a sheet presented now replaces it in place, without a new fade
  float m_sheetMix = 1.0F; // open motion: card opacity, scale 0.96 -> 1 (M31)
  std::uint32_t m_sheetAnim = 0;
  std::function<void()> m_onClosed;
  std::function<void()> m_onLayout;
  bool m_sheetAttach = false; // a sheet waits to join the scene without a rebuild
  bool m_sheetDetach = false; // a closed sheet waits to leave it
  Rect m_contentRect;
  float m_uiScale = 1.0F;

  std::unique_ptr<ToplevelSurface> m_surface;
  std::unique_ptr<desk::ForeignParent> m_foreignParent;
  std::unique_ptr<Node> m_root;
  Label* m_titleLabel = nullptr;
  Glyph* m_captionSymbol = nullptr;
  class Spinner* m_captionSpinner = nullptr;
  Label* m_captionState = nullptr;
  Caption m_caption;
  std::function<void(float, float)> m_captionMenu;
  void applyCaption();
  std::function<void()> m_titleClick;
  bool m_compact = false; // narrow window: icon-rail sidebar
  void swapSidebar(Renderer& renderer, float height);
  Node* m_titleRow = nullptr;
  bool m_headerSearch = false;
  bool m_contentCards = false;
  bool m_buildingHeader = false;
  Button* m_maximizeButton = nullptr;
  std::array<Button*, 2> m_navButtons{};
  Button* m_moreButton = nullptr;
  Flex* m_sidebarNode = nullptr;
  Flex* m_mainNode = nullptr;
  std::optional<ColorSpec> m_mainFill;
  std::optional<Scene> m_scene;
  ShaderSceneNode* m_sceneNode = nullptr; // null for a Picture backdrop
  Image* m_scenePicture = nullptr;
  float m_sceneSeconds = 0.0F;
  std::optional<Scene> m_sceneMeasured; // the scene the tints below were measured for
  float m_sceneTextTint = 1.0F;
  bool m_sceneContentBacked = false;
  Flex* m_headerNode = nullptr;
  void measureScene();
  void applySceneFills();
  void applyMainFill();
  std::chrono::steady_clock::time_point m_sceneStepped{}; // when the scene clock last advanced
  std::chrono::steady_clock::time_point m_sceneAwakeUntil{};
  bool m_activated = false;
  bool m_sceneHeld = false;
  bool m_sceneTicking = false; // a scene step waits on its present or the step timer
  Timer m_sceneTimer;
  // The scene draws on its own subsurface below the window at reduced resolution: its frames repaint only that layer,
  // and the window's own frames stay as cheap as without a scene.
  std::unique_ptr<Subsurface> m_sceneLayer;
  std::unique_ptr<Node> m_sceneRoot;
  Node* m_sceneClip = nullptr;
  void syncSceneLayer(float width, float height, float margin, float radius);
  void sceneTick();
  void wakeScene();
  float m_sidebarMix = 1.0F; // 0 = collapsed
  float m_cardGap = 0.0F; // contentCard: the inset around the main pane, in surface px
  std::vector<InputRect> m_blur; // the blur region last sent; empty = none
  void syncBlur();
  bool m_sidebarVisible = true;
  std::uint32_t m_sidebarAnim = 0;
  std::unique_ptr<ContextMenuPopup> m_menu;
  std::unique_ptr<class SelectDropdownPopup> m_select;
  std::unique_ptr<class AppPopover> m_popover;
  AnimationManager m_animations;
  InputDispatcher m_input;
  bool m_rebuild = true;
  bool m_buttonDown = false;
  int m_buttonsHeld = 0;
  bool m_pointerInside = false;
  bool m_inResizeBand = false;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // deferred lambdas skip a destroyed window
};
