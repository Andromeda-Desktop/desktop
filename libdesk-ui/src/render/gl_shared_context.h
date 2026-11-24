#pragma once

#include <string>
#include <EGL/egl.h>
#include <functional>
#include <string_view>
#include <utility>
#include <vector>

enum class RenderGraphicsResetStatus;

// Reset status of the GL context current on this thread. NoError when the driver exposes no reset query.
[[nodiscard]] RenderGraphicsResetStatus currentGlContextResetStatus();

// Owns the EGLDisplay, the chosen EGLConfig, and a root surfaceless EGLContext
// that is the share parent for every other EGLContext in the shell
// (RenderContext instances, ...). By routing all contexts
// through this share group, GL textures uploaded in any of them are usable
// from every other context in the group — which lets LockSurface reuse
// wallpaper textures resident in VRAM without re-decoding.
class Display;

class GlSharedContext {
public:
  GlSharedContext() = default;
  ~GlSharedContext();

  GlSharedContext(const GlSharedContext&) = delete;
  GlSharedContext& operator=(const GlSharedContext&) = delete;

  void initialize(EGLNativeDisplayType display, bool createSharedContext = true);
  void initialize(const Display& display, bool createSharedContext = true);
  // For processes without a compositor connection; render with offscreen targets only.
  void initializeSurfaceless();
  void cleanup();
  void recreateRootContext();
  // On the init thread after initialize(): loads the programs appId used at its last start into the root context.
  // Calls started() once a ShaderProgram::create() would wait for the loads, or at once when nothing loads.
  void preloadPrograms(const std::string& appId, const std::function<void()>& started) const;

  [[nodiscard]] EGLDisplay display() const noexcept { return m_display; }
  [[nodiscard]] EGLConfig config() const noexcept { return m_config; }
  [[nodiscard]] EGLContext rootContext() const noexcept { return m_rootContext; }
  [[nodiscard]] bool hasSharedContext() const noexcept { return m_rootContext != EGL_NO_CONTEXT; }
  [[nodiscard]] bool resetNotificationEnabled() const noexcept { return m_resetNotificationEnabled; }
  [[nodiscard]] bool videoMemoryPurgeNotificationEnabled() const noexcept {
    return m_videoMemoryPurgeNotificationEnabled;
  }

  [[nodiscard]] EGLContext createContext(EGLContext shareContext, std::string_view label);

  // Bind the root context surfacelessly. No-op (returns true) when shared context is disabled.
  // Returns false if eglMakeCurrent failed (e.g. context lost on resume); callers skip GPU work.
  bool makeCurrentSurfaceless() const;

  // Single sink for resets seen on any context of this display (root, RenderContext).
  // Drivers report resets per hardware context, so every owner polls its own context and reports here.
  void setGraphicsResetCallback(std::function<void(RenderGraphicsResetStatus)> callback) {
    m_graphicsResetCallback = std::move(callback);
  }
  void reportGraphicsReset(RenderGraphicsResetStatus status, std::string_view source) const;

private:
  void initializeDisplay(EGLint surfaceType, bool createSharedContext);
  void buildContextAttributes();
  [[nodiscard]] EGLContext createContextWithCurrentAttributes(EGLContext shareContext) const;
  void usePlainContextAttributes() noexcept;

  EGLDisplay m_display = EGL_NO_DISPLAY;
  EGLConfig m_config = nullptr;
  EGLContext m_rootContext = EGL_NO_CONTEXT;
  EGLContext m_spareContext = EGL_NO_CONTEXT; // made on the init worker; the first createContext takes it
  std::vector<EGLint> m_contextAttributes;
  bool m_contextAttributesRobust = false;
  bool m_resetNotificationEnabled = false;
  bool m_videoMemoryPurgeNotificationEnabled = false;
  bool m_sharedContextEnabled = false;
  std::function<void(RenderGraphicsResetStatus)> m_graphicsResetCallback;
  mutable bool m_graphicsResetReported = false;
};
