#pragma once

#include <cstdint>

// The platform window that an EGL window surface draws into. The platform layer owns the native objects.
class NativeEglWindow {
public:
  virtual ~NativeEglWindow() = default;

  // Creates the native window on the first call and resizes it after that. Returns the EGLNativeWindowType as a
  // pointer, or nullptr when the platform cannot make the window.
  [[nodiscard]] virtual void* resize(std::uint32_t bufferWidth, std::uint32_t bufferHeight) = 0;
};
