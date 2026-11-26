#pragma once

#include "core/input/cursor_shape.h"
#include "capture/screenshot_image.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

enum class ScreenshotRegionModifier : std::uint8_t;

class RenderContext;
struct wl_surface;
class WaylandConnection;
struct KeyboardEvent;
struct PointerEvent;

namespace capture {

  enum class ConfirmAction { None, ForceClipboard, ForceSave };
  enum class DragMode {
    None,
    NewSelection,
    TopEdge,
    BottomEdge,
    LeftEdge,
    RightEdge,
    TopLeftCorner,
    TopRightCorner,
    BottomLeftCorner,
    BottomRightCorner,
    Move
  };

  class ScreenshotRegionOverlay {
  public:
    using CompleteCallback = std::function<void(std::optional<LogicalRect>, wl_output* output, ConfirmAction action)>;
    using FailureCallback = std::function<void(const std::string& message)>;

    ScreenshotRegionOverlay();
    ~ScreenshotRegionOverlay();

    void initialize(WaylandConnection& wayland, RenderContext* renderContext);
    void setCompleteCallback(CompleteCallback callback);
    void setFailureCallback(FailureCallback callback);
    void setConfirmKeybindLabels(std::string copyLabel, std::string saveLabel, std::string cancelLabel);
    void setFrozenScreenshots(std::vector<FrozenScreenshot> screenshots);
    void setRegionModifier(ScreenshotRegionModifier modifier);
    void setSelectionBorderWidth(float width);
    void setDimOpacity(float opacity);
    void setShowRegionHint(bool show);
    void setShowRegionDimensions(bool show);
    [[nodiscard]] std::vector<FrozenScreenshot> takeFrozenScreenshots();
    void begin(
        bool freezeScreen, bool fullscreenPick = false, bool confirmRegion = false,
        std::optional<LogicalRect> initialRegion = std::nullopt
    );
    void cancel();
    void cancelSelection();
    void onOutputChange();

    // Valid ≥2×2 selection left behind by cancelSelection (cleared on read).
    [[nodiscard]] std::optional<LogicalRect> takeAbandonedRegion();

    [[nodiscard]] bool isActive() const noexcept { return m_active; }
    [[nodiscard]] bool ownsSurface(const wl_surface* surface) const noexcept;
    [[nodiscard]] bool onPointerEvent(const PointerEvent& event);
    [[nodiscard]] bool onKeyboardEvent(const KeyboardEvent& event);
    // Seat modifier change; refreshes the gesture cursor without pointer motion.
    void onModifiersChanged();

  private:
    struct Instance;

    DragMode m_dragMode = DragMode::None;
    double m_moveOffsetX = 0.0;
    double m_moveOffsetY = 0.0;
    // Edge-to-pointer grab offset at press, so modifier resize follows the pointer relatively.
    double m_resizeBaseX = 0.0;
    double m_resizeBaseY = 0.0;
    double m_cursorGlobalX = 0.0;
    double m_cursorGlobalY = 0.0;

    void ensureSurfaces();
    void destroySurfaces();
    [[nodiscard]] bool surfacesMatchOutputs() const;
    void prepareFrame(Instance& instance, bool needsUpdate, bool needsLayout);
    void abortWithError(const std::string& message);
    void updateSelectionVisuals();
    // Stores the shape on every instance and pushes it to the compositor right away.
    void applyCursorShape(CursorShape shape);
    [[nodiscard]] bool modifierHeld() const;
    [[nodiscard]] bool confirmsSelection(std::uint32_t sym) const;
    void selectEntireDesktop();
    void completeSelection();
    void confirmPendingSelection(ConfirmAction action = ConfirmAction::None);
    void completeFullscreenPick(wl_output* output);
    [[nodiscard]] std::optional<LogicalRect> selectionRectIfValid() const;

    WaylandConnection* m_wayland = nullptr;
    RenderContext* m_renderContext = nullptr;
    CompleteCallback m_onComplete;
    FailureCallback m_onFailure;
    std::vector<std::unique_ptr<Instance>> m_instances;
    std::vector<FrozenScreenshot> m_frozenScreenshots;
    std::optional<LogicalRect> m_abandonedRegion;
    std::string m_copyKeybindLabel;
    std::string m_saveKeybindLabel;
    std::string m_cancelKeybindLabel;
    ScreenshotRegionModifier m_regionModifier{};
    bool m_spaceHeld = false;
    float m_selectionBorderWidth = 2.0F;
    float m_dimOpacity = 0.65F;
    bool m_showRegionHint = true;
    bool m_showRegionDimensions = true;
    bool m_active = false;
    bool m_freezeScreen = false;
    bool m_fullscreenPick = false;
    bool m_confirmRegion = false;
    bool m_confirming = false;
    bool m_dragging = false;
    std::uint32_t m_dragButton = 0;
    double m_startGlobalX = 0.0;
    double m_startGlobalY = 0.0;
    double m_currentGlobalX = 0.0;
    double m_currentGlobalY = 0.0;
    std::shared_ptr<int> m_alive = std::make_shared<int>(0);
  };

} // namespace capture
