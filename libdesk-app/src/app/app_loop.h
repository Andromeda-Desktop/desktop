#pragma once

#include "core/poll_loop.h"
#include "core/poll_source.h"
#include "render/gl_shared_context.h"
#include "render/render_context.h"
#include "platform/key_repeat_poll_source.h"
#include "platform/display.h"

#include <string>
#include <memory>

class WaylandConnection;

#include <chrono>
#include <functional>
#include <future>
#include <vector>

// The run loop of a standalone libdesk-ui app: one Wayland connection, one GL context, one render context, and
// poll sources. DeferredCall, TimerManager and key repeat are built in.
class AppLoop {
public:
  AppLoop();
  ~AppLoop();

  AppLoop(const AppLoop&) = delete;
  AppLoop& operator=(const AppLoop&) = delete;

  // Connects Wayland, starts GL and the render context. False when no compositor answers.
  // appId names the GL program preload list; empty: no preload.
  bool initialize(bool sharedGl, const std::string& appId = {});

  // Wayland builds only; apps that still call Wayland directly (files, viewer, player).
  [[nodiscard]] WaylandConnection& wayland() noexcept;
  [[nodiscard]] Display& display() noexcept { return *m_display; }
  [[nodiscard]] const Display& display() const noexcept { return *m_display; }
  [[nodiscard]] GlSharedContext& gl() noexcept { return m_gl; }
  [[nodiscard]] RenderContext& renderContext() noexcept { return m_render; }

  // The caller keeps ownership; remove a source before it is destroyed.
  void addSource(PollSource* source);
  void removeSource(PollSource* source);
  // For an FdWatch, which joins and leaves the loop itself.
  [[nodiscard]] PollLoop& pollLoop() noexcept { return m_loop; }

  // Runs on a compositor hangup inside run(): true when the app reconnected, false to end run() with 0.
  void setDisconnectHandler(std::function<bool()> handler) { m_onDisconnect = std::move(handler); }
  // Drops GL and the dead connection, connects to the compositor that took over the socket and restarts GL.
  // False when none answers within the timeout. Windows must be closed first.
  bool reconnect(std::chrono::milliseconds timeout);
  // After a GL context reset: abandons the dead objects and makes a new context. Throws when that fails.
  void recoverGraphics();

  // Runs until quit() or a hangup the disconnect handler does not recover. Returns the quit code, 0 on hangup,
  // 1 on a Wayland error.
  int run();
  void quit(int code = 0);

private:

  std::unique_ptr<Display> m_display;
  GlSharedContext m_gl;
  RenderContext m_render;
  std::future<void> m_eglReady; // EGL is initialized; m_egl also covers the program preload after it
  std::future<void> m_egl; // declared after m_gl and m_display: its destructor waits for the worker that uses them
  KeyRepeatPollSource m_keyRepeat;
  PollLoop m_loop;
  std::function<bool()> m_onDisconnect;
  bool m_sharedGl = true;
};
