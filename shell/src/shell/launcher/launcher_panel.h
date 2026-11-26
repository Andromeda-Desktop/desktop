#pragma once

#include "core/timer_manager.h"
#include "launcher/launcher_provider.h"
#include "launcher/search_broker.h"
#include "launcher/usage_tracker.h"
#include "render/core/texture_handle.h"
#include "shell/launcher/launcher_view.h"
#include "shell/panel/panel.h"
#include "system/icon_resolver.h"
#include "ui/signal.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class ContextMenuPopup;
struct ContextMenuControlEntry;
class DragIconSurface;
class AccountsService;
class Button;
class Flex;
class Glyph;
class Image;
class Input;
class Label;
class LauncherResultAdapter;
class LauncherAppGridAdapter;
class LauncherStartCardAdapter;
struct LauncherStartCard;
class FilesProvider;
class KlipyProvider;
class LauncherSectionsAdapter;
class Renderer;
class Segmented;
class Separator;
class ScrollView;
class VirtualGridView;
class VirtualListView;
class ConfigService;
class AsyncTextureCache;

class LauncherPanel : public Panel {
public:
  LauncherPanel(ConfigService* config, AccountsService* accounts, AsyncTextureCache* asyncTextures);
  ~LauncherPanel() override;

  void addProvider(std::unique_ptr<LauncherProvider> provider);
  // Drop every dynamically-registered (plugin-backed) provider, so the enabled
  // plugin set can be re-applied without disturbing the built-in providers.
  void clearDynamicProviders();
  // Drop providers whose stable id starts with `prefix` (e.g. config-driven "dmenu.").
  void clearProvidersWithIdPrefix(std::string_view prefix);
  // Restrict the next open to a single provider (stdin/dmenu session). When set,
  // onInputChanged queries only that provider and skips prefix routing/overview.
  // Cleared on close.
  void setScopedProvider(std::string_view providerId, std::string_view placeholder = {});

  void create() override;
  void onOpen(std::string_view context) override;
  [[nodiscard]] std::string handoffContext() const override;
  void onClose() override;
  [[nodiscard]] bool interceptPointerEvent(const PointerEvent& event, float width, float height) override;
  void onIconThemeChanged() override;

  void clearUsage();
  void syncUsageOrderingState();

  // Invoked after a terminal close when the activation copied text and the provider
  // supports auto-paste. The host schedules virtual-keyboard paste (clipboard path).
  void setCopiedActivationCallback(std::function<void(bool isImage)> callback) { m_onCopiedActivation = std::move(callback); }

  void setOnAppLaunchCallback(std::function<void()> callback) { m_onAppLaunch = std::move(callback); }
  // Open windows of the app behind a desktop entry path, for the top hit's subtitle.
  void setWindowCounter(std::function<std::size_t(const std::string&)> counter) { m_windowCounter = std::move(counter); }
  // "Add to Desktop" in the app menu (shown while desktop icons are on) and app drags dropped on the desktop.
  void setAddToDesktopCallback(std::function<void(const std::filesystem::path&)> callback) {
    m_addToDesktop = std::move(callback);
  }

  [[nodiscard]] float preferredWidth() const override;
  [[nodiscard]] float preferredHeight() const override;
  [[nodiscard]] bool fitsContentHeight() const noexcept override { return usesBrowserLayout() || usesStartLayout(); }
  [[nodiscard]] float centerTopFraction() const noexcept override { return usesBrowserLayout() ? 0.2F : 0.0F; }
  [[nodiscard]] bool padsContent() const noexcept override { return !usesBrowserLayout() && !usesStartLayout(); }
  // Concept C: the search capsule, each mode drop and the results are separate glass pieces.
  [[nodiscard]] bool usesGlassPieces() const override { return usesBrowserLayout(); }
  // proto P19: the pieces grow out of the launcher button and return into it.
  [[nodiscard]] PiecePour piecePour() const override { return {.towardLeft = true, .motion = PourMotion::Scale}; }
  [[nodiscard]] float measureContentHeight(Renderer& renderer, float contentWidth) override;
  [[nodiscard]] LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::Exclusive; }
  [[nodiscard]] InputArea* initialFocusArea() const override;
  [[nodiscard]] bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override;
  [[nodiscard]] bool ignorePointerEvent() const override;
  [[nodiscard]] bool isContextActive(std::string_view context) const override;

private:
  friend class LauncherPanelTestAccess;

  enum ActiveCategoryType { All, Pinned, RecentlyUsed, Category, Other };
  enum class ResultsTransition : std::uint8_t { None, NavigationForward, NavigationBackward };

  struct CategoryFilterSlot {
    ActiveCategoryType type;
    std::size_t categoryIndex = 0;
  };

  struct BrowserViewState {
    ActiveCategoryType categoryType = All;
    std::size_t categorySlotIndex = 0;
    std::string category;
    std::string query;
    std::string providerViewId;
  };

  void onPanelCardOpacityChanged(float opacity) override;
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
  void onInputChanged(const std::string& text);
  void setQuery(const std::string& query);
  // Re-gather the current query, preserving the selected result by identity.
  void reapplyCurrentQuery();
  // A plugin provider delivered fresh async results — re-gather if the panel is open.
  void onProviderResultsChanged(const LauncherProvider& provider);
  void refreshResults();
  void activateAt(std::size_t index);
  void activateNewWindow(std::size_t index);
  void activateSelected();
  bool handleKeyEvent(std::uint32_t sym, std::uint32_t modifiers);
  void applyEmptyState();
  void bindDetailResult();
  [[nodiscard]] bool shouldUseDetailPresentation() const;
  [[nodiscard]] bool startsWithLauncherPrefix(std::string_view text) const;
  void applyProviderConfig(LauncherProvider& provider) const;
  void finishActivation(LauncherProvider& provider, const std::string& resultId, bool copied, bool appLaunch = false);
  [[nodiscard]] std::vector<LauncherResult> providerOverviewResults(std::string_view text) const;
  [[nodiscard]] bool openAppActionsMenu(std::size_t index, float anchorX, float anchorY);
  // Opens the launcher's one context menu at a point in panel coordinates.
  bool openMenu(
      std::vector<ContextMenuControlEntry> entries, float anchorX, float anchorY,
      std::function<void(const ContextMenuControlEntry&)> onActivate
  );
  void updateCategoryFilterModel(const std::vector<LauncherCategory>& categories);
  void rebuildCategoryFilter();
  void rebuildLauncherNavigation();
  [[nodiscard]] ActiveCategoryType defaultCategoryType() const;
  void applyDefaultView();
  void updateCategorySelectionChrome();
  void rebuildAppsCategoryList();
  void selectAppsCategory(ActiveCategoryType type, const std::string& category);
  [[nodiscard]] bool showAppsCategoryList() const;
  void updateLauncherOptionButtons();
  void setCategoryFilterVisible(bool visible);
  void setActiveCategorySlot(std::size_t slotIndex);
  void setActiveBrowserCategory(std::size_t categoryIndex);
  void applyActiveCategory();
  void syncLauncherListStyle();
  void syncLauncherViewLayout(Renderer* renderer = nullptr);
  void updatePageDots(std::size_t page, std::size_t pages);
  void queueResultsTransition(ResultsTransition transition);
  void animateResultsTransition();
  [[nodiscard]] bool shouldUseAppGrid() const;
  [[nodiscard]] bool supportsResultGrid() const;
  [[nodiscard]] bool shouldUseLauncherSections() const;
  void rebuildLauncherSections();
  void selectResult(std::size_t index, bool scrollToSelection);
  // Key navigation: the current item shows the focus ring until the next pointer press.
  void showKeyFocus();
  void refreshLauncherAppIconColorization();
  void updateLauncherGridMetrics(Renderer& renderer);
  void updatePinnedApplicationState();
  void applyPinnedApplicationOrder();
  void reorderPinnedApplication(std::string_view sourcePath, std::string_view targetPath);
  void syncLauncherAvatar(Renderer& renderer);
  void bindPreview(Renderer& renderer);
  void setMode(std::string_view providerId);
  void cycleMode(int direction);
  void setFilesScope(bool on);
  void setFilesKind(std::size_t index);
  void syncKindTabs();
  void syncModeTabs();
  void syncModeDrops();
  [[nodiscard]] bool modeDropOn(std::size_t index) const;
  void toggleModeDrop(std::size_t index);
  void syncEmojiStrip();
  void syncPackStrip();
  [[nodiscard]] bool pillExpanded() const;
  void activatePreviewAction();
  [[nodiscard]] FilesProvider* filesProvider() const;
  [[nodiscard]] KlipyProvider* stickersProvider() const;
  void setStickerPack(std::size_t index);
  void deleteSelectedClipboardEntry();
  void openSelectedClipboardImage();
  void copySelectedClipboardPaths();
  void activateEmojiVariant(std::size_t tone);
  void showProviderOverview();
  void showSessionActions();
  [[nodiscard]] bool usesBrowserLayout() const;
  // Start layout (Windows): Pinned and All around one search field, the account footer under them.
  [[nodiscard]] bool usesStartLayout() const;
  [[nodiscard]] std::string launcherPlaceholder() const;
  void createStartSheet(std::unique_ptr<Input> input, std::unique_ptr<Flex> body);
  // Shows the home (Pinned, All) for an empty query and the results list for a search.
  void syncStartSheet();
  void rebuildStartHome();
  void setStartView(ShellConfig::LauncherStartView view);
  void openStartViewMenu();
  [[nodiscard]] ShellConfig::LauncherStartView startView() const;
  [[nodiscard]] float startHomeHeight() const;

  std::vector<std::unique_ptr<LauncherProvider>> m_providers;
  std::vector<LauncherResult> m_results;
  std::vector<LauncherResult> m_allResults;
  UsageTracker m_usageTracker;
  SearchBroker m_searchBroker{m_providers, m_usageTracker, "/"};
  IconResolver m_iconResolver;
  // App icon lookups run on the SharedWorker after startup and after each close, so an open only reads the cache.
  void prefetchAppIcons();
  // Lays out the first page of the Emoji and Symbols modes once after startup, one string per tick: the first layout
  // of a script makes Pango find and load its fallback font (~8-13 ms each), which otherwise lands on the first open.
  void warmModeText();
  std::vector<std::pair<std::string, float>> m_textWarmup;
  Timer m_textWarmupTimer;
  Timer m_iconPrefetchTimer;
  std::uint64_t m_iconPrefetchKey = 0;
  bool m_iconPrefetching = false;
  std::shared_ptr<int> m_alive = std::make_shared<int>(0);

  struct AppsCategoryRow {
    ActiveCategoryType type;
    std::string category;
    Button* button = nullptr;
  };
  // The mode drops right of the capsule: Apps, Files, Clipboard, Emoji.
  struct ModeDrop {
    Flex* node = nullptr;
    Glyph* glyph = nullptr;
  };
  // Nodes of the tree create() builds. onClose drops the tree, so they reset together (m_ui = {}).
  struct Nodes {
    Flex* container = nullptr;
    Input* input = nullptr;
    Segmented* categoryFilter = nullptr;
    Flex* body = nullptr;
    Flex* resultsViewport = nullptr;
    Flex* appsCategoryList = nullptr;
    Flex* launcherToolbar = nullptr;
    Segmented* launcherNavigation = nullptr;
    Flex* startPinnedHead = nullptr;
    Button* startShowAll = nullptr;
    VirtualGridView* startPinnedGrid = nullptr;
    Flex* startAllHead = nullptr;
    Button* startViewButton = nullptr;
    Button* startBack = nullptr;
    VirtualGridView* startCardGrid = nullptr;
    Label* startName = nullptr;
    Label* startInitials = nullptr;
    Image* launcherAvatar = nullptr;
    Node* launcherAvatarFallback = nullptr;
    Button* launcherUsageButton = nullptr;
    Button* launcherGridButton = nullptr;
    Button* launcherPowerButton = nullptr;
    VirtualGridView* grid = nullptr;
    Flex* pageDotsRow = nullptr;
    VirtualListView* launcherSections = nullptr;
    ScrollView* detailScroll = nullptr;
    Label* detailSubtitle = nullptr;
    Label* detailBody = nullptr;
    Label* emptyLabel = nullptr;
    Flex* pillFooter = nullptr;
    Flex* resultsPiece = nullptr;
    // Media modes (DESIGN.md "Launcher media modes"): the provider the tabs scope the search to; empty = search.
    Flex* modeTabs = nullptr;
    Flex* emojiStrip = nullptr;
    Flex* packStrip = nullptr;
    // Direction C preview pane (right of the result list).
    Flex* previewPane = nullptr;
    Image* previewImage = nullptr;
    Glyph* previewGlyph = nullptr;
    Glyph* pillSearchGlyph = nullptr;
    Label* previewEmoji = nullptr;
    Flex* previewTones = nullptr;
    Label* previewTitle = nullptr;
    Label* previewKind = nullptr;
    Button* previewOpen = nullptr;
    Button* previewOpenImage = nullptr;
    Button* previewCopyPath = nullptr;
    Button* previewAction = nullptr;
    Button* previewDelete = nullptr;
    Label* previewBody = nullptr;
    Flex* previewBodyBox = nullptr;
    Image* previewShot = nullptr;
    Label* pillCount = nullptr;
    Label* pillOpenHint = nullptr;
    Flex* pillBackHint = nullptr;
    // Files view ("Show all files"): a "Files" token in the field; every query goes to Files only (50 rows, newest first).
    Flex* filesToken = nullptr;
    Label* filesTokenLabel = nullptr;
    // Kind chips of the Files scope (DESIGN.md "Show all" B, kind filter A).
    Flex* kindTabs = nullptr;
    Flex* pillKindHint = nullptr;
    std::vector<AppsCategoryRow> appsCategoryRows;
    std::vector<Flex*> pageDots;
    std::array<ModeDrop, 4> modeDrops{};
    std::vector<std::pair<std::string, Flex*>> modeTabItems;
    std::array<Label*, 5> previewToneLabels{};
    std::array<Flex*, 3> previewMetaRows{};
    std::array<Label*, 3> previewMetaKeys{};
    std::array<Label*, 3> previewMetaValues{};
    std::vector<Flex*> kindTabItems;
  };
  Nodes m_ui;
  std::vector<LauncherResult> m_startPinned;
  std::unique_ptr<LauncherAppGridAdapter> m_startPinnedAdapter;
  std::vector<LauncherStartCard> m_startCards;
  std::unique_ptr<LauncherStartCardAdapter> m_startCardAdapter;
  bool m_startShowAllPins = false;
  bool m_pageDotsChanged = false;
  bool m_anyProviderLoading = false;
  std::unique_ptr<LauncherResultAdapter> m_listAdapter;
  std::unique_ptr<LauncherAppGridAdapter> m_gridAdapter;
  std::unique_ptr<LauncherSectionsAdapter> m_launcherSectionsAdapter;

  std::string m_query;
  std::string m_resultProviderId;
  std::string m_scopedProviderId;
  std::string m_scopedPlaceholder;
  ActiveCategoryType m_activeCategoryType = All;
  std::string m_activeCategory;
  std::vector<LauncherCategory> m_currentCategories;
  std::vector<CategoryFilterSlot> m_categoryFilterSlots;
  bool m_hasRecentlyUsed = false;
  bool m_hasPinnedApplications = false;
  bool m_categoryFilterRebuildPending = false;
  bool m_applyDefaultViewPending = false;
  bool m_syncingLauncherNavigation = false;
  std::size_t m_selectedIndex = 0;
  std::size_t m_activeCategorySlotIndex = 0;
  bool m_browserMode = false;
  float m_modeTabsWidth = 0.0F; // the mode tabs' natural width, measured in the clipboard split
  std::string m_previewShotId;
  // Last liquid strength sent to the panel surface; -1 = none since open.
  // Frames of the animated GIF preview; the textures belong to the panel and unload on change or close.
  struct PreviewFrame {
    TextureHandle handle;
    std::chrono::milliseconds duration{0};
  };
  std::vector<PreviewFrame> m_previewFrames;
  std::size_t m_previewFrame = 0;
  std::string m_previewGifPath;
  Renderer* m_previewRenderer = nullptr;
  Timer m_previewFrameTimer;
  bool playPreviewGif(Renderer& renderer, const std::string& path);
  void stopPreviewGif();
  void showNextPreviewFrame();
  std::string m_previewIconPath;
  std::size_t m_kindIndex = 0; // index into kFileKinds
  std::size_t m_packIndex = 0;
  launcher_view::State<BrowserViewState> m_view;
  [[nodiscard]] bool filesScope() const noexcept { return m_view.is<launcher_view::Files>(); }
  [[nodiscard]] bool appsView() const noexcept { return m_view.is<launcher_view::Apps>(); }
  // Only the clipboard shows the list in a 360 pane beside a preview; every other list is full width.
  [[nodiscard]] bool previewSplit() const noexcept { return m_view.mediaProviderId() == "Clipboard"; }
  [[nodiscard]] std::string_view modeProviderId() const noexcept { return m_view.mediaProviderId(); }
  // A paste mode (clipboard, emoji, ...) shows the mode tabs and pastes; the power mode runs its rows.
  [[nodiscard]] bool pasteModeActive() const noexcept;
  [[nodiscard]] bool showingProviderOverview() const noexcept { return m_view.is<launcher_view::ProviderOverview>(); }
  // Leaves a provider view for plain search; the Files and overview views stay.
  void leaveProviderView();
  bool m_launcherShowIcons = true;
  bool m_launcherShowAppOriginIndicator = true;
  bool m_launcherCompact = false;
  bool m_launcherAppGrid = false;
  bool m_launcherListItemBackground = false;
  bool m_usingAppGrid = false;
  ResultsTransition m_pendingResultsTransition = ResultsTransition::None;
  bool m_resultsTransitionActive = false;
  float m_resultsTransitionBaseX = 0.0F;
  float m_launcherRowHeight = 0.0F;
  std::uint64_t m_desktopEntriesVersion = 0;
  ConfigService* m_config = nullptr;
  AccountsService* m_accounts = nullptr;
  AsyncTextureCache* m_asyncTextures = nullptr;
  std::string m_launcherAvatarPath;
  std::unique_ptr<ContextMenuPopup> m_actionsMenu;
  Signal<>::ScopedConnection m_appIconColorizeConn;
  std::function<void(bool isImage)> m_onCopiedActivation;
  std::function<void()> m_onAppLaunch;
  std::function<std::size_t(const std::string&)> m_windowCounter;
  std::function<void(const std::filesystem::path&)> m_addToDesktop;
  // An app row pressed and dragged out of the launcher becomes a system drag of its .desktop file (text/uri-list),
  // so the desktop makes a shortcut where it is dropped.
  bool m_dragArmed = false;
  std::optional<std::size_t> m_dragIndex;
  std::uint32_t m_dragSerial = 0;
  std::unique_ptr<DragIconSurface> m_dragIcon;
  std::unique_ptr<Node> m_dragImage;
  void startAppDrag(const LauncherResult& result);
};
