#pragma once
#include "core/animation.h"
#include "scene/dsk_glass.h"
#include "scene/surface_blur.h"

#include <wayland-server-core.h>

#include <list>

struct wlr_xdg_popup;
struct wlr_xdg_popup_configure;
struct wlr_scene_tree;

namespace andromeda {

  class Output;
  class Server;

  // A shell popup (`server` given) draws the glass shape it declares over dsk_surface_v1, like a layer surface: true
  // rounded corners, its tint raised by the backdrop density. Other popups draw their client blur region.
  class Popup : public Animatable {
  public:
    explicit Popup(
        wlr_xdg_popup* popup, wlr_scene_tree* parentTree = nullptr, wlr_scene_tree* captureTree = nullptr,
        Server* server = nullptr
    );
    ~Popup() override;

    Popup(const Popup&) = delete;
    Popup& operator=(const Popup&) = delete;

    // Applies the current glass slider to every live popup's blur (a config reload does not commit them).
    static void applyGlassToAll();
    // Samples the backdrop of the shell popups on `output`; the soonest recheck in ms, 0 for none.
    static int pollBackdrops(const Output* output);

    [[nodiscard]] AnimationPhase animationPhase() const override { return AnimationPhase::Overlays; }
    bool tickAnimations(double nowMsec) override;
    [[nodiscard]] bool hasActiveAnimations() const override { return m_density.animating(); }
    [[nodiscard]] bool animatesOn(const Output* output) const override { return parentOutput() == output; }

  private:
    static void onCommit(wl_listener* listener, void* data);
    static void onReposition(wl_listener* listener, void* data);
    static void onConfigure(wl_listener* listener, void* data);
    static void onMap(wl_listener* listener, void* data);
    static void onUnmap(wl_listener* listener, void* data);
    static void onDestroy(wl_listener* listener, void* data);

    [[nodiscard]] glass::Material glassMaterial() const;
    [[nodiscard]] Output* parentOutput() const;
    // The dsk_surface_v1 state of this popup's surface when it declares a glass shape, else null.
    [[nodiscard]] const DskSurfaceManager::State* glassState() const;
    // Draws the declared shape; false when the popup declares none.
    bool updateShapeGlass(const SurfaceBlurOptions& options);
    int pollBackdrop();
    void densityChanged(bool glides);
    void handleCommit();
    void unconstrain();
    void handleReposition();
    void handleConfigure();
    void handleMap();
    void handleDestroy();
    void refreshWindowCapture() const;

    wlr_xdg_popup* m_popup = nullptr;
    Server* m_server = nullptr;
    SurfaceBlur m_blur;
    DskGlass m_glass;
    GlassDensity m_density;

    wl_listener m_commit{};
    wl_listener m_reposition{};
    wl_listener m_configure{};
    std::list<wlr_xdg_popup_configure> m_repositions;
    wl_listener m_map{};
    wl_listener m_unmap{};
    wl_listener m_destroy{};
  };

} // namespace andromeda
