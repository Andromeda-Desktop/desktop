#pragma once

#include "platform/surface.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

class DskShapeClient;
class DskSurfaceClient;
class FrameStats;
class WaylandConnection;
struct ext_background_effect_surface_v1;
struct wl_callback;
struct wl_output;
struct wl_surface;
struct wp_fractional_scale_v1;
struct wp_viewport;
struct xdg_surface;
struct zwlr_layer_surface_v1;

// A Surface backed by a wl_surface. Role classes (layer, popup, toplevel, subsurface) derive from it.
class WaylandSurface : public virtual Surface {
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
  bool setGlassCards(std::span<const GlassCard> cards) override;
  void clearGlassCard() override;

  [[nodiscard]] wl_surface* wlSurface() const noexcept { return m_surface; }
  // The role object a popup attaches to; null for roles that cannot parent a popup.
  [[nodiscard]] virtual xdg_surface* xdgSurface() const noexcept { return nullptr; }
  [[nodiscard]] virtual zwlr_layer_surface_v1* layerSurface() const noexcept { return nullptr; }

  static void handleFrameDone(void* data, wl_callback* callback, std::uint32_t callbackData);
  void onSurfaceOutputEnter(wl_surface* surface, wl_output* output);
  void onSurfaceOutputLeave(wl_surface* surface, wl_output* output);
  // True while a WaylandSurface owns this wl_surface; a destroyed one must not be passed to a protocol request.
  [[nodiscard]] static bool isLive(const wl_surface* surface);
  [[nodiscard]] static bool isLive(const Surface* surface) noexcept;
  // The WaylandSurface that owns surface; null for a destroyed or foreign wl_surface.
  [[nodiscard]] static WaylandSurface* fromWl(const wl_surface* surface);
  // Null for null or a non-Wayland Surface.
  [[nodiscard]] static wl_surface* wlOf(const Surface* surface);
  void onPreferredFractionalScale(std::uint32_t numerator);
  // The one dsk_surface_v1 of this surface (a second is a protocol error), made on first use; it feeds presence().
  // Null without our compositor or before createWlSurface.
  DskSurfaceClient* dsk();

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
  void hideCardShapes();
  void initializeSurfaceScaleProtocol();
  void traceBlurRegion(std::string_view event, const std::vector<InputRect>& rects) const;

  OutputChangedCallback m_outputChangedCallback;
  wl_callback* m_frameCallback = nullptr;
  std::unique_ptr<FrameStats> m_frameStats;
  // The one dsk_surface_v1 of m_surface (a second is a protocol error): glass card, secure flag and presence share it.
  std::unique_ptr<DskSurfaceClient> m_dsk;
  // One dsk shape per glass card, with the last value sent per property: an unchanged card sends nothing.
  struct CardShape;
  std::vector<CardShape> m_cardShapes;
  // Where the glass cards live: dsk shapes, or a blur region on a compositor without them (or without rotation or
  // corners).
  enum class CardMode : std::uint8_t { None, Shapes, Region };
  CardMode m_cardMode = CardMode::None;
  // The region last sent in CardMode::Region.
  std::vector<InputRect> m_cardRegion;
  ext_background_effect_surface_v1* m_backgroundEffect = nullptr;
  wp_viewport* m_viewport = nullptr;
  wp_fractional_scale_v1* m_fractionalScale = nullptr;
  bool m_opaque = false;
};
