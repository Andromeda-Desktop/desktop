#pragma once

#include "ui/controls/search_picker.h"
#include "ui/dialogs/dialog_popup_host.h"
#include "platform/popup.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class UiSettings;
class Flex;
class RenderContext;
class Display;
struct KeyboardEvent;
struct PointerEvent;
class Surface;

struct SearchPickerPopupRequest {
  // Set to open over a window (the settings window). Unset opens over the layer surface
  // that received the last input, resolved through LayerPopupHostRegistry.
  std::optional<PopupParent> parent;
  std::string title;
  std::vector<SearchPickerOption> options;
  std::string selectedValue;
  std::string placeholder;
  std::string emptyText;
  float scale = 1.0F;
};

class SearchPickerPopup final : public DialogPopupHost {
public:
  using SelectCallback = std::function<void(const std::string& value)>;

  SearchPickerPopup() = default;
  ~SearchPickerPopup() override;

  void initialize(Display& display, const UiSettings& settings, RenderContext& renderContext);
  void initialize(
      Display& display, const UiSettings& settings, RenderContext& renderContext,
      LayerPopupHostRegistry& popupHosts
  );

  void setOnSelect(SelectCallback callback);
  void setOnDismissed(std::function<void()> callback);

  void open(SearchPickerPopupRequest request);
  void close();

  [[nodiscard]] bool isOpen() const noexcept;
  [[nodiscard]] bool onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);
  [[nodiscard]] Surface* surface() const noexcept;

protected:
  void populateContent(Node* contentParent, std::uint32_t width, std::uint32_t height) override;
  void layoutSheet(float contentWidth, float contentHeight) override;
  void cancelToFacade() override;
  [[nodiscard]] InputArea* initialFocusArea() override;
  void onSheetClose() override;

private:
  // Guard token for deferred callbacks that run on the next main-loop tick.
  // Callbacks capture a weak_ptr so they can detect destruction without
  // relying on a raw this pointer staying valid.
  std::shared_ptr<void> m_aliveGuard = std::make_shared<int>(0);

  float m_scale = 1.0F;
  std::string m_title;
  std::vector<SearchPickerOption> m_options;
  std::string m_selectedValue;
  std::string m_placeholder;
  std::string m_emptyText;

  Flex* m_root = nullptr;
  SearchPicker* m_searchPicker = nullptr;

  SelectCallback m_onSelect;
  std::function<void()> m_onDismissed;
};

// App-wide entry point for surfaces without their own SearchPickerPopup (layer-shell panels).
// The application registers its layer-parented popup at startup.
class SearchPickerDialog {
public:
  static void setPopup(SearchPickerPopup* popup) noexcept;
  // Opens over the layer surface that received the last input; `request.parent` must be unset.
  // Returns false when no popup is registered or the popup could not open.
  static bool open(SearchPickerPopupRequest request, SearchPickerPopup::SelectCallback onSelect);
};
