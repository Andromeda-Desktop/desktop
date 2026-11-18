#pragma once

#include "platform/surface.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

class FrameStats;
class WaylandConnection;
struct ext_background_effect_surface_v1;
struct wl_callback;
struct wl_output;
struct wl_surface;
struct wp_fractional_scale_v1;
struct wp_viewport;

// A Surface backed by a wl_surface. Role classes (layer, popup, toplevel, subsurface) derive from it.
class WaylandSurface : public Surface {
public:
  using OutputChangedCallback = std::function<void(wl_output* output)>;

  explicit WaylandSurface(WaylandConnection& connection);
  ~WaylandSurface() override;

  void setOutputChangedCallback(OutputChangedCallback callback);
  void setInputRegion(const std::vector<InputRect>& rects) override;
  void setOpaqueRegion(const std::vector<InputRect>& rects) override;
  void setBlurRegion(const std::vector<InputRect>& rects) override;
  void setOpaque(bool opaque) override;
  void clearBlurRegion() override;

  [[nodiscard]] wl_surface* wlSurface() const noexcept { return m_surface; }

  static void handleFrameDone(void* data, wl_callback* callback, std::uint32_t callbackData);
  void onSurfaceOutputEnter(wl_surface* surface, wl_output* output);
  void onSurfaceOutputLeave(wl_surface* surface, wl_output* output);
  // True while a WaylandSurface owns this wl_surface; a destroyed one must not be passed to a protocol request.
  [[nodiscard]] static bool isLive(const wl_surface* surface);
  void onPreferredFractionalScale(std::uint32_t numerator);

protected:
  virtual bool createWlSurface();
  bool prepareBlurEffect();
  void destroySurface();

  [[nodiscard]] bool hasNativeSurface() const noexcept override { return m_surface != nullptr; }
  [[nodiscard]] std::unique_ptr<NativeEglWindow> createNativeWindow() override;
  [[nodiscard]] bool frameCallbackPending() const noexcept override { return m_frameCallback != nullptr; }
  void requestFrameCallback() override;
  void cancelFrameCallback() override;
  void commitNative() override;
  [[nodiscard]] bool scalesByViewport() const noexcept override;
  void applyNativeScale() override;
  [[nodiscard]] FrameStatsSink* frameStats(bool create) override;

  WaylandConnection& m_connection;
  wl_surface* m_surface = nullptr;

private:
  void initializeSurfaceScaleProtocol();
  void traceBlurRegion(std::string_view event, const std::vector<InputRect>& rects) const;

  OutputChangedCallback m_outputChangedCallback;
  wl_callback* m_frameCallback = nullptr;
  std::unique_ptr<FrameStats> m_frameStats;
  ext_background_effect_surface_v1* m_backgroundEffect = nullptr;
  wp_viewport* m_viewport = nullptr;
  wp_fractional_scale_v1* m_fractionalScale = nullptr;
  bool m_opaque = false;
};
