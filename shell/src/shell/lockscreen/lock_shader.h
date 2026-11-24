#pragma once

#include "ipc/pixel_stream_server.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <chrono>
#include <cstdint>
#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

class GlSharedContext;

// The lock helper's one raw GL context, in the renderer's share group. Every LockShader draws in it, so raw GL
// state never mixes with the renderer's cached state and any number of slots costs one context.
class LockGlContext {
public:
  // Throws std::runtime_error when the context cannot be made.
  explicit LockGlContext(GlSharedContext& shared);
  ~LockGlContext();
  LockGlContext(const LockGlContext&) = delete;
  LockGlContext& operator=(const LockGlContext&) = delete;

  [[nodiscard]] EGLDisplay display() const noexcept { return m_display; }
  [[nodiscard]] EGLContext context() const noexcept { return m_context; }
  bool makeCurrent() const;

private:
  EGLDisplay m_display = EGL_NO_DISPLAY;
  EGLContext m_context = EGL_NO_CONTEXT;
};

// Draws a fragment program into two frame buffers in the lock helper: a user shader (Shadertoy style:
// `void mainImage(out vec4 fragColor, in vec2 fragCoord)` with iResolution, iTime, iTimeDelta, iFrame), or a
// copy of a renderer frame. iChannel0 shows the image set with setChannel0* (a video frame), top row first;
// iChannelResolution[0] is its size.
class LockShader {
public:
  // Both throw std::runtime_error when the framebuffer or the program cannot be made (a compile error included).
  [[nodiscard]] static std::unique_ptr<LockShader> shadertoy(LockGlContext& gl, const std::string& source,
                                                             std::uint32_t width, std::uint32_t height);
  // Copies iChannel0, a renderer frame (bottom row first, premultiplied alpha) set with setChannel0Texture.
  [[nodiscard]] static std::unique_ptr<LockShader> copy(LockGlContext& gl, std::uint32_t width, std::uint32_t height);
  ~LockShader();
  LockShader(const LockShader&) = delete;
  LockShader& operator=(const LockShader&) = delete;

  // Draws the next frame into frame buffer `index` (0 or 1) and waits until the GPU is done.
  bool draw(std::size_t index);
  // Shows a single-plane RGBA8 dmabuf as iChannel0 until the next call. The caller keeps the
  // buffer alive until then. False when the driver cannot import it.
  bool setChannel0Dmabuf(const PixelStreamServer::Dmabuf& buffer);
  // Uploads top-down RGBA8 rows (stride = width * 4) as iChannel0.
  bool setChannel0Pixels(std::span<const std::uint8_t> rgba, std::uint32_t width, std::uint32_t height);
  // Samples `texture`, a texture of the share group the caller keeps alive, as iChannel0 until the next setChannel0*.
  void setChannel0Texture(GLuint texture) noexcept { m_sampled = texture; }
  // Copies frame buffer `index` into `rgba` as top-down RGBA8 rows (width * height * 4).
  bool read(std::size_t index, std::span<std::uint8_t> rgba);
  // The two frame buffers as dmabufs, in index order; empty when the driver cannot export them.
  [[nodiscard]] const std::vector<PixelStreamServer::Dmabuf>& dmabufs() const noexcept { return m_dmabufs; }
  [[nodiscard]] std::uint32_t width() const noexcept { return m_width; }
  [[nodiscard]] std::uint32_t height() const noexcept { return m_height; }

private:
  LockShader(LockGlContext& gl, const std::string& fragment, std::uint32_t width, std::uint32_t height);
  void release() noexcept;
  void releaseChannelImage() noexcept;
  void exportDmabufs();

  LockGlContext& m_gl;
  std::array<GLuint, 2> m_textures{};
  std::array<GLuint, 2> m_framebuffers{};
  std::vector<PixelStreamServer::Dmabuf> m_dmabufs;
  GLuint m_channel = 0;
  // The texture draw binds as iChannel0: m_channel or a renderer frame.
  GLuint m_sampled = 0;
  EGLImage m_channelImage = EGL_NO_IMAGE;
  std::uint32_t m_channelWidth = 0;
  std::uint32_t m_channelHeight = 0;
  GLuint m_program = 0;
  GLint m_position = -1;
  GLint m_resolution = -1;
  GLint m_time = -1;
  GLint m_timeDelta = -1;
  GLint m_frame = -1;
  GLint m_channel0 = -1;
  GLint m_channelResolution = -1;
  std::uint32_t m_width = 0;
  std::uint32_t m_height = 0;
  int m_frameCount = 0;
  std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point m_last = m_start;
};
