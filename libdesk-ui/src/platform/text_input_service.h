#pragma once

#include "ui/text_input_client.h"

class Surface;

// The window system's input method: sends the focused client's state and delivers preedit and commits to it.
// Wayland: WaylandTextInputService (text-input-v3).
class TextInputService {
public:
  virtual ~TextInputService() = default;

  [[nodiscard]] virtual bool isAvailable() const noexcept = 0;
  virtual void setFocusedClient(
      Surface* surface, TextInputClient* client, bool acceptKeyboardFocusActivation = false,
      Surface* keyboardFocusParentSurface = nullptr
  ) = 0;
  virtual void clearFocusedClient(TextInputClient* client) = 0;
  virtual void notifyClientStateChanged(TextInputClient* client, TextInputChangeCause cause) = 0;
};
