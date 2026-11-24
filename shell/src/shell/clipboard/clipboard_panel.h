#pragma once

#include "core/timer_manager.h"
#include "shell/panel/panel.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class Button;
class AsyncTextureCache;
class ClipboardService;
class Flex;
class Glyph;
class Image;
class Input;
class InputArea;
class Label;
class Renderer;
class ScrollView;
class ConfigService;
class ClipboardListAdapter;
class VirtualGridView;
struct ClipboardEntry;

class ClipboardPanel : public Panel {
public:
  ClipboardPanel(ClipboardService* clipboard, ConfigService* config, AsyncTextureCache* asyncTextures);
  ~ClipboardPanel() override;
  void setActivateCallback(std::function<void(const ClipboardEntry&)> callback);
  // Show in Files for a copied-files entry: the first path, selected in its folder.
  void setShowInFilesCallback(std::function<void(const std::filesystem::path&)> callback);

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;

  [[nodiscard]] float preferredWidth() const override { return scaled(720.0F); }
  [[nodiscard]] float preferredHeight() const override { return scaled(560.0F); }
  [[nodiscard]] LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::Exclusive; }
  [[nodiscard]] bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;
  [[nodiscard]] InputArea* initialFocusArea() const override;
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override;

private:
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;
  void schedulePreviewPayloadRefresh(bool debounced);
  void updateListState();
  void updatePreviewActions();
  void rebuildPreview(Renderer& renderer, float width, float height);
  void selectIndex(std::size_t index);
  void selectByStorageId(const std::string& storageId);
  void activateSelected();
  void togglePinSelected();
  void runImageAction();
  void openSelectedImage();
  void copySelectedPaths();
  void showSelectedInFiles();
  void requestClearUnpinnedHistory();
  void clearUnpinnedHistory();
  void clearAllHistory();
  void performClearUnpinnedHistory();
  void performClearAllHistory();
  bool handleKeyEvent(std::uint32_t sym, std::uint32_t modifiers);
  void scrollToSelected();
  void requestDeleteSelectedEntry();
  void deleteSelectedEntry();
  void performDeleteSelectedEntry();
  void resetDeleteConfirmation();
  void resetClearConfirmation();
  void applyFilter();
  void onFilterChanged(const std::string& text);
  [[nodiscard]] std::size_t selectedHistoryIndex() const;

  ClipboardService* m_clipboard = nullptr;
  std::function<void(const ClipboardEntry&)> m_activateCallback;
  std::function<void(const std::filesystem::path&)> m_showInFilesCallback;
  ConfigService* m_config = nullptr;
  AsyncTextureCache* m_asyncTextures = nullptr;

  InputArea* m_focusArea = nullptr;
  Flex* m_rootLayout = nullptr;
  Flex* m_sidebar = nullptr;
  Flex* m_sidebarHeaderRow = nullptr;
  Label* m_sidebarTitle = nullptr;
  Button* m_clearHistoryButton = nullptr;
  Button* m_clearKeepPinnedButton = nullptr;
  Flex* m_clearConfirmPanel = nullptr;
  Label* m_clearConfirmDesc = nullptr;
  Button* m_closeButton = nullptr;
  Input* m_filterInput = nullptr;
  VirtualGridView* m_listGrid = nullptr;
  Label* m_listEmptyLabel = nullptr;
  std::unique_ptr<ClipboardListAdapter> m_listAdapter;
  std::vector<std::size_t> m_filteredIndices;
  std::string m_filterQuery;

  Flex* m_previewCard = nullptr;
  Flex* m_previewHeaderRow = nullptr;
  Label* m_previewTitle = nullptr;
  Label* m_previewMeta = nullptr;
  Button* m_imageActionButton = nullptr;
  Button* m_openImageButton = nullptr;
  Button* m_copyPathButton = nullptr;
  Button* m_showInFilesButton = nullptr;
  Button* m_pinButton = nullptr;
  Button* m_copyButton = nullptr;
  Button* m_deleteEntryButton = nullptr;
  Flex* m_deleteConfirmPanel = nullptr;
  ScrollView* m_previewScrollView = nullptr;
  Flex* m_previewContent = nullptr;
  Image* m_previewImage = nullptr;
  Glyph* m_previewPlaceholder = nullptr;

  std::size_t m_selectedIndex = 0;
  std::size_t m_previewPayloadIndex = static_cast<std::size_t>(-1);
  std::size_t m_pendingPreviewPayloadIndex = static_cast<std::size_t>(-1);
  Timer m_previewPayloadDebounceTimer;
  Timer m_filterDebounceTimer;
  std::string m_pendingFilterQuery;
  std::string m_deleteConfirmStorageId;
  std::uint64_t m_lastChangeSerial = 0;
  bool m_clearConfirm = false;
  float m_lastWidth = 0.0F;
  float m_lastHeight = 0.0F;
  float m_lastPreviewWidth = -1.0F;
  float m_lastPreviewHeight = -1.0F;
  float m_listRowHeight = 0.0F;
  bool m_pendingScrollToSelected = false;
  // The text preview's payload read for m_previewLoadId.
  enum class PreviewLoad { None, Loading, Failed };
  PreviewLoad m_previewLoad = PreviewLoad::None;
  std::string m_previewLoadId;
  std::shared_ptr<int> m_alive = std::make_shared<int>(0);
};
