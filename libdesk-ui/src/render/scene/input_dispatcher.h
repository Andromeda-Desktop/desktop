#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "core/input/cursor_shape.h"
#include "render/scene/input_area.h"

class InputArea;
class Node;
class TextInputService;
struct wl_surface;

class InputDispatcher {
public:
  using CursorShapeCallback = std::function<void(std::uint32_t serial, CursorShape shape)>;
  using HoverChangeCallback = std::function<void(InputArea* oldArea, InputArea* newArea)>;
  using FocusChangeCallback = std::function<void(InputArea* oldArea, InputArea* newArea)>;
  // Every button press at scene x, y, before it reaches its area.
  using PressCallback = std::function<void(float x, float y)>;
  struct TabFocusSnapshot {
    std::optional<std::size_t> index;
    std::optional<std::string> key;
  };

  InputDispatcher() = default;
  ~InputDispatcher();
  InputDispatcher(const InputDispatcher&) = delete;
  InputDispatcher& operator=(const InputDispatcher&) = delete;

  // The dispatcher whose scene root is `node` or one of its ancestors (assistive-technology focus requests).
  [[nodiscard]] static InputDispatcher* owning(const Node* node);
  // True inside the focus loss that a scene teardown (a window rebuild) sends, not a user's blur.
  [[nodiscard]] static bool inSceneTeardown() noexcept { return s_sceneTeardown; }
  // True after Tab moved focus, until the next pointer press: the focus ring shows only then (rulebook 11).
  [[nodiscard]] static bool focusVisible() noexcept { return s_focusVisible; }

  void setSceneRoot(Node* root);
  void setCursorShapeCallback(CursorShapeCallback callback);
  void setHoverChangeCallback(HoverChangeCallback callback);
  void setFocusChangeCallback(FocusChangeCallback callback);
  void setPressCallback(PressCallback callback);
  void setTextInputContext(
      wl_surface* surface, TextInputService* service, bool keyboardFocusActivation = false,
      wl_surface* keyboardFocusParentSurface = nullptr
  );

  // Dispatch Wayland pointer events into the scene graph
  void pointerEnter(float x, float y, std::uint32_t serial);
  void pointerLeave();
  void pointerMotion(float x, float y, std::uint32_t serial);
  void syncPointerHover();
  // Returns true if the event was consumed by a scene widget
  bool pointerButton(
      float x, float y, std::uint32_t button, bool pressed, std::uint32_t serial, std::uint32_t time, bool touch
  );
  // The end of a touchpad scroll goes to the captured or hovered area, or the nearest ancestor that takes it.
  bool pointerAxisStop(std::uint32_t axis);
  bool pointerAxis(
      float x, float y, std::uint32_t axis, std::uint32_t axisSource, double value, std::int32_t discrete,
      std::int32_t value120, float lines, std::uint32_t axisGestureSerial = 0
  );
  // A touchpad pinch goes to the nearest area under (x, y), or its ancestor, that handles pinches.
  void pointerPinch(float x, float y, const InputArea::PinchData& data);
  void cancelPointerCapture();
  // Drops hover, capture and focus held by areas no longer in the scene (a removed subtree, before it is freed).
  void forgetDetached() { pruneDetachedAreas(); }

  // Dispatch keyboard events to the focused area
  void keyEvent(std::uint32_t sym, std::uint32_t utf32, std::uint32_t modifiers, bool pressed, bool preedit = false);

  // Focus management
  void setFocus(InputArea* area);
  // Swaps the text-input (IME) client of `area`; a focused area moves the input method to the new client at once.
  void setTextInputClient(InputArea& area, TextInputClient* client);
  void stashTabFocus();
  void restoreStashedTabFocus();
  [[nodiscard]] TabFocusSnapshot captureTabFocus() const;
  void restoreTabFocus(TabFocusSnapshot snapshot);
  [[nodiscard]] bool cycleTabFocus(bool reverse);
  [[nodiscard]] bool cycleTabFocusInSubtree(Node* subtree, bool reverse);
  [[nodiscard]] InputArea* inputAreaAt(float x, float y);
  [[nodiscard]] InputArea* firstTabFocusUnder(Node* subtree) const;
  [[nodiscard]] InputArea* lastTabFocusUnder(Node* subtree) const;
  [[nodiscard]] InputArea* focusedArea() const noexcept { return m_focusedArea; }
  [[nodiscard]] InputArea* hoveredArea() const noexcept { return m_hoveredArea; }
  [[nodiscard]] bool pointerCaptured() const noexcept { return m_capturedArea != nullptr; }

private:
  static inline bool s_sceneTeardown = false;
  static inline bool s_focusVisible = false;
  InputArea* findInputAreaAt(float x, float y);
  [[nodiscard]] bool isAttachedToScene(const InputArea* area) const;
  void pruneDetachedAreas();
  void updateHover(float x, float y, std::uint32_t serial);
  void updateCursor(std::uint32_t serial);
  void trackArea(InputArea* area);
  void clearTextInputFocus(InputArea* area);
  void syncTextInputFocus();

  struct TouchScrollGesture {
    bool pending = false;
    float pressX = 0.0F;
    float pressY = 0.0F;
    std::uint32_t button = 0;
    std::uint32_t serial = 0;
    std::uint32_t time = 0;
    InputArea* verticalArea = nullptr;
    InputArea* horizontalArea = nullptr;
  };

  Node* m_sceneRoot = nullptr;
  CursorShapeCallback m_cursorShapeCallback;
  HoverChangeCallback m_hoverChangeCallback;
  FocusChangeCallback m_focusChangeCallback;
  PressCallback m_pressCallback;
  wl_surface* m_textInputSurface = nullptr;
  TextInputService* m_textInputService = nullptr;
  bool m_textInputKeyboardFocusActivation = false;
  wl_surface* m_textInputKeyboardFocusParentSurface = nullptr;
  InputArea* m_hoveredArea = nullptr;
  InputArea* m_focusedArea = nullptr;
  InputArea* m_capturedArea = nullptr; // held while any button is pressed
  TouchScrollGesture m_touchGesture;
  // A finger scroll under an area that locks the scroll axis: Deciding holds the motion until one axis passes the
  // slop; then only that axis goes on until its stop or a pause.
  enum class AxisLock : std::uint8_t { Idle, Deciding, Vertical, Horizontal };
  AxisLock m_axisLock = AxisLock::Idle;
  std::array<double, 2> m_axisLockHeld{};
  std::chrono::steady_clock::time_point m_axisLockLast;
  // False: the event is held or dropped. value: the held motion is added when the axis wins.
  bool passAxisLock(InputArea* target, std::uint32_t axis, std::uint32_t axisSource, double& value);
  std::optional<std::size_t> m_stashedTabFocusIndex;
  std::optional<std::string> m_stashedTabFocusKey;
  std::uint32_t m_lastSerial = 0;
  float m_lastPointerX = 0.0F;
  float m_lastPointerY = 0.0F;
  bool m_hasPointerPosition = false;
  bool m_cancelingPointerCapture = false;
  std::shared_ptr<void> m_lifetime = std::make_shared<int>(0);
};
