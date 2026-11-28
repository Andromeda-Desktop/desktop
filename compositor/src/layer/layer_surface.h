#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "config/config.h"
#include "core/animation.h"
#include "scene/node.h"
#include "scene/dsk_glass.h"
#include "scene/glass_mask.h"
#include "scene/surface_blur.h"
#include "scene/surface_shadow.h"

#include <wayland-server-core.h>

struct wlr_layer_surface_v1;
struct wlr_scene_layer_surface_v1;
struct wlr_scene_tree;
struct wlr_scene_rect;
struct wlr_scene_border;
struct wlr_box;
struct wlr_surface;
struct wlr_xdg_popup;

namespace andromeda {

  class Output;
  class Server;

  class LayerSurface : public SceneNode, public Animatable {
  public:
    // Resolve a layer owner through subsurfaces and nested XDG popups.
    [[nodiscard]] static LayerSurface* fromSurface(wlr_surface* surface);

    LayerSurface(Server& server, wlr_layer_surface_v1* layerSurface);
    ~LayerSurface();

    LayerSurface(const LayerSurface&) = delete;
    LayerSurface& operator=(const LayerSurface&) = delete;

    [[nodiscard]] wlr_layer_surface_v1* layerSurface() const { return m_layerSurface; }
    [[nodiscard]] wlr_scene_layer_surface_v1* scene() const { return m_scene; }
    [[nodiscard]] bool mapped() const { return m_mapped; }
    // "sandbox_engine/app_id" of a sandboxed client that needs the user's answer, else empty.
    // A sandboxed app's surface gets no configure, so it cannot map, until the user allows layer-shell for the app.
    [[nodiscard]] bool awaitingAccess() const { return m_access == Access::Awaiting; }
    [[nodiscard]] const std::string& sandboxApp() const { return m_sandboxApp; }
    void setSandboxApp(std::string app) { m_sandboxApp = std::move(app); }
    void awaitAccess() { m_access = Access::Awaiting; }
    void grantAccess();
    [[nodiscard]] bool exclusiveKeyboard() const;
    [[nodiscard]] bool acceptsKeyboard() const;
    [[nodiscard]] bool hasKeyboardFocus() const;
    // Status surfaces (bars, island, OSD, notification toasts) stay above the other surfaces of their layer, so a panel
    // that opens later never covers a banner. A cover (the screenshot freeze) stays above status surfaces: it shows a
    // still of the whole screen.
    enum class Stacking : std::uint8_t { Normal, Status, Cover };
    [[nodiscard]] Stacking stacking() const;
    [[nodiscard]] bool statusSurface() const { return stacking() == Stacking::Status; }
    // Raises this surface to the top of its layer.
    void raise();
    [[nodiscard]] SurfaceBlurOptions blurOptions() const;
    [[nodiscard]] bool blurActive() const { return m_blur.active(); }
    // Reads a finished backdrop sample of the liquid blur and moves the density toward it. Returns the ms until it needs
    // a new poll (a GPU sample pending), or 0.
    int pollBackdrop();
    // Moves the density toward the tone of the last backdrop sample.
    // Sends the density target to the client and draws the tint at the current density; `glides` asks for a frame.
    void densityChanged(bool glides);
    // dsk_surface_v1.backdrop_luminance (v21) after a sample, when the probe is on and the value moved a visible step.
    void sendLuminance();
    // Sends the density to the blur nodes as a backdrop pre-mix toward the tint.
    void applyDensity();
    [[nodiscard]] SurfaceBlurOptions popupBlurOptions() const;
    // The ext-background-effect blur region a client set on `surface` (one of its popups), or null.
    [[nodiscard]] const pixman_region32_t* clientBlurRegion(wlr_surface* surface) const;

    void focus();
    // A grabbing xdg_popup of this layer mapped. wlroots routes the grab's keys to the seat's focused surface and
    // never moves focus itself, so an on-demand layer that does not hold the keyboard yet takes it here.
    void focusForPopupGrab();
    void unconstrainPopup(wlr_xdg_popup* popup);
    // Push the owning output's scale to every surface of this layer surface
    // (fractional-scale + preferred buffer scale, popups included).
    void notifyOutputScale();
    [[nodiscard]] Output* output() const;

    [[nodiscard]] AnimationPhase animationPhase() const override { return AnimationPhase::Overlays; }
    bool tickAnimations(double nowMsec) override;
    [[nodiscard]] bool hasActiveAnimations() const override;
    [[nodiscard]] bool animatesOn(const Output* output) const override;

  private:
    friend class Server;

    static void onMap(wl_listener* listener, void* data);
    static void onUnmap(wl_listener* listener, void* data);
    static void onCommit(wl_listener* listener, void* data);
    static void onDestroy(wl_listener* listener, void* data);
    static void onNewPopup(wl_listener* listener, void* data);
    static void onSceneUpdate(wl_listener* listener, void* data);
    static void onSceneDestroy(wl_listener* listener, void* data);

    void handleMap();
    void handleUnmap();
    void handleCommit();
    void handleDestroy();
    void handleNewPopup(void* data);
    void reparentToLayer(uint32_t layer);
    void syncPopupTree();
    // Tell an open overview to re-resolve this output's mirrored background stack.
    void notifyDesktopStack();
    void applyBackgroundOpaqueRegion();
    void applyVisibleRegion();
    void updateBlur();
    void applyConfig();
    void refreshStartupRuleEffects();
    void applyFadeAlpha();
    // Applies dsk_surface_v1 translate, scale, clip and radius to the surface tree.
    void applySurfaceMotion();
    [[nodiscard]] bool showsOnOutput(double opacity) const;
    void applyStroke(wlr_scene_tree* tree, const wlr_box* clip, int radius);
    [[nodiscard]] wlr_scene_tree* surfaceTree() const;
    void beginCloseAnimation(wlr_scene_tree* tree);

    Server* m_server = nullptr;
    wlr_layer_surface_v1* m_layerSurface = nullptr;
    wlr_scene_layer_surface_v1* m_scene = nullptr;
    wlr_scene_tree* m_popupTree = nullptr;
    SurfaceBlur m_blur;
    GlassMask m_glassMask;
    // Blur nodes for the 2nd and later shapes of a client blur region (bar islands in one surface).
    std::vector<SurfaceBlur> m_shapeBlurs;
    // The surface's dsk_surface_v1 glass; shapes without their own material take it.
    DskGlass m_glass;
    float m_backdropLuminance = 0.5F; // the last probe sample, 0 to 1
    std::optional<float> m_sentLuminance; // the last backdrop_luminance sent; nullopt = none since the probe turned on
    // Per blur node (m_blur, then m_shapeBlurs): the last backdrop sample and the node's area in px; m_backdropLuminance
    // is their area-weighted mean (the shadow), so every glass shape of a surface takes one tone.
    std::vector<std::optional<fx_backdrop_luminance>> m_nodeLuminance;
    std::vector<int> m_nodeArea;
    // Per blur node (m_blur, then m_shapeBlurs) material from dsk_shape_v1 (v10); empty = the surface material.
    struct ShapeMaterial {
      DskGlass glass;
      float opacity = 1.0F;
      bool solo = false;
    };
    std::vector<ShapeMaterial> m_shapeMaterials;
    // Per m_shapeBlurs entry: its shape's opacity, multiplied into the fade alpha.
    std::vector<float> m_shapeBlurOpacity;
    // m_blur's shape opacity when it draws shapes[0], else 1.
    float m_blurOpacity = 1.0F;
    // Raises the tint over a same-tone backdrop; rises only while mapped.
    GlassDensity m_density;
    // The last backdrop range (darkest, brightest) sampled while the surface's dsk motion ran; applied at settle.
    std::optional<std::pair<float, float>> m_heldDensitySample;
    // Surface opacity (layer fade x dsk opacity): the whole glass (tint, rim, lens) fades with its surface.
    float m_glassAlpha = 1.0F;
    float m_blurFade = 1.0F;
    SurfaceShadow m_shadow; // dsk_surface_v1 elevation
    float m_shadowOpacity = 1.0F; // the surface's dsk Opacity, 0 to 1, applied to m_shadow
    std::vector<std::unique_ptr<SurfaceShadow>> m_shapeShadows; // dsk_shape_v1 elevation, one per shape
    // dsk_surface_v1 stroke: the hairline the client border cannot draw while the clip cuts the client's sheet edge.
    wlr_scene_border* m_stroke = nullptr;
    std::vector<wlr_scene_rect*> m_shapeFills; // dsk_shape_v1 flat shapes (fill alpha > 0), reused per frame
    float m_surfaceAlpha = 1.0F;
    ResolvedLayerRule m_rule;
    AnimatedValue m_fade{1.0};
    bool m_mapped = false;
    enum class Access { Granted, Awaiting };
    Access m_access = Access::Granted;
    std::string m_sandboxApp;

    wl_listener m_map{};
    wl_listener m_unmap{};
    wl_listener m_commit{};
    wl_listener m_destroy{};
    wl_listener m_newPopup{};
    wl_listener m_sceneUpdate{};
    wl_listener m_sceneDestroy{};
  };

} // namespace andromeda
