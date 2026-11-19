#include "view/popup.h"

#include "layer/layer_surface.h"
#include "output/output.h"
#include "scene/node.h"
#include "view/view.h"
#include "wlr.h"

#include <algorithm>
#include <cassert>
#include <vector>

namespace andromeda {

  namespace {
    std::vector<Popup*>& livePopups() {
      static std::vector<Popup*> popups;
      return popups;
    }
  } // namespace

  void Popup::applyGlassToAll() {
    for (Popup* popup : livePopups()) {
      popup->m_blur.setMaterial(configGlass(popup->glassKind()));
    }
  }

  Popup::Popup(wlr_xdg_popup* popup, wlr_scene_tree* parentTree, wlr_scene_tree* captureTree) : m_popup(popup) {
    if (parentTree == nullptr) {
      wlr_xdg_surface* parent = wlr_xdg_surface_try_from_wlr_surface(m_popup->parent);
      assert(parent != nullptr);
      parentTree = static_cast<wlr_scene_tree*>(parent->data);
    }
    m_popup->base->data = wlr_scene_xdg_surface_create(parentTree, m_popup->base);
    if (captureTree != nullptr) {
      wlr_scene_xdg_surface_create(captureTree, m_popup->base);
    }

    m_commit.notify = onCommit;
    wl_signal_add(&m_popup->base->surface->events.commit, &m_commit);
    m_reposition.notify = onReposition;
    wl_signal_add(&m_popup->events.reposition, &m_reposition);
    m_configure.notify = onConfigure;
    wl_signal_add(&m_popup->base->events.configure, &m_configure);
    m_map.notify = onMap;
    wl_signal_add(&m_popup->base->surface->events.map, &m_map);
    m_unmap.notify = onUnmap;
    wl_signal_add(&m_popup->base->surface->events.unmap, &m_unmap);
    m_destroy.notify = onDestroy;
    wl_signal_add(&m_popup->events.destroy, &m_destroy);
    livePopups().push_back(this);
  }

  Popup::~Popup() {
    std::erase(livePopups(), this);
    if (m_commit.link.next != nullptr) {
      wl_list_remove(&m_commit.link);
      wl_list_remove(&m_reposition.link);
      wl_list_remove(&m_configure.link);
      wl_list_remove(&m_map.link);
      wl_list_remove(&m_unmap.link);
      wl_list_remove(&m_destroy.link);
    }
  }

  void Popup::onCommit(wl_listener* listener, void* /*data*/) {
    Popup* self = wl_container_of(listener, self, m_commit); // NOLINT(modernize-use-auto)
    self->handleCommit();
  }

  void Popup::onReposition(wl_listener* listener, void* /*data*/) {
    Popup* self = wl_container_of(listener, self, m_reposition); // NOLINT(modernize-use-auto)
    self->handleReposition();
  }

  void Popup::onConfigure(wl_listener* listener, void* /*data*/) {
    Popup* self = wl_container_of(listener, self, m_configure); // NOLINT(modernize-use-auto)
    self->handleConfigure();
  }

  void Popup::onMap(wl_listener* listener, void* /*data*/) {
    Popup* self = wl_container_of(listener, self, m_map); // NOLINT(modernize-use-auto)
    if (View* view = View::fromSurface(self->m_popup->parent)) {
      view->watchPopupSurfaceTree(self->m_popup->base->surface);
    }
    self->refreshWindowCapture();
    self->handleMap();
  }

  void Popup::onUnmap(wl_listener* listener, void* /*data*/) {
    Popup* self = wl_container_of(listener, self, m_unmap); // NOLINT(modernize-use-auto)
    self->m_blur.hide();
    self->m_repositions.clear();
    self->refreshWindowCapture();
  }

  // The window capture tree holds every popup of the window: a new one shows in it unless this hides it again.
  void Popup::refreshWindowCapture() const {
    if (View* view = View::fromSurface(m_popup->parent)) {
      view->refreshCaptureHidden();
    }
  }

  void Popup::onDestroy(wl_listener* listener, void* /*data*/) {
    Popup* self = wl_container_of(listener, self, m_destroy); // NOLINT(modernize-use-auto)
    self->handleDestroy();
  }

  glass::Kind Popup::glassKind() const {
    // Shell popups (menus, tooltips) are parented to a layer surface and take the one island glass.
    return LayerSurface::fromSurface(m_popup->parent) != nullptr ? glass::Kind::Shape : glass::Kind::Menu;
  }

  void Popup::handleCommit() {
    const wlr_box& geometry = m_popup->base->geometry;
    SurfaceBlurOptions blurOptions;
    const pixman_region32_t* region = nullptr;
    wlr_surface* surface = m_popup->base->surface;
    if (LayerSurface* layer = LayerSurface::fromSurface(m_popup->parent)) {
      blurOptions = layer->popupBlurOptions();
      region = layer->clientBlurRegion(surface);
    } else if (View* view = View::fromSurface(m_popup->parent)) {
      blurOptions = view->popupBlurOptions();
      region = view->clientBlurRegion(surface);
    }
    // A popup lies over its parent's content, never on the bare wallpaper the optimized blur caches.
    blurOptions.optimized = false;
    // A client blur region turns popup blur on, as it does for windows; it is the exact shape, so no alpha test.
    if (region != nullptr && pixman_region32_not_empty(region)) {
      blurOptions.enabled = true;
      blurOptions.ignoreAlpha = 0.0F;
    } else {
      region = nullptr;
    }
    m_blur.setMaterial(configGlass(glassKind()));
    m_blur.update(
        static_cast<wlr_scene_tree*>(m_popup->base->data), surface, wlr_box{0, 0, geometry.width, geometry.height},
        geometry, corner_radii_none(), nullptr, blurOptions, 1.0F, nullptr, region
    );
    if (!m_popup->base->initial_commit) {
      return;
    }

    // New popups miss the parent's map/workspace-time scale notify; tell them the parent's output scale before the
    // first configure so scale-aware clients size and map input correctly from frame one.
    Output* output = nullptr;
    if (LayerSurface* layer = LayerSurface::fromSurface(m_popup->parent)) {
      output = layer->output();
    } else if (View* view = View::fromSurface(m_popup->parent)) {
      output = view->currentOutput();
    }
    if (output != nullptr) {
      wlr_xdg_surface_for_each_surface(m_popup->base, &Output::notifySurfaceScaleIter, output);
    }

    unconstrain();
  }

  void Popup::unconstrain() {
    if (LayerSurface* layer = LayerSurface::fromSurface(m_popup->parent)) {
      layer->unconstrainPopup(m_popup);
    } else if (View* view = View::fromSurface(m_popup->parent)) {
      view->unconstrainPopup(m_popup);
    }
    // Unconstraining schedules the configure, but it is skipped while the parent is not shown or has no output. A
    // client must not draw before its first configure, and some block until it arrives. A configure already
    // scheduled is reused, not sent twice.
    wlr_xdg_surface_schedule_configure(m_popup->base);
  }

  void Popup::handleReposition() {
    unconstrain();
    // Clients can wait for a reposition token before drawing. Keep each request's placement and token until wlroots
    // sends its configure, then schedule the next request through the same configure path.
    m_repositions.push_back(m_popup->scheduled);
    m_popup->scheduled = m_repositions.front();
  }

  void Popup::handleConfigure() {
    if (m_repositions.empty()) {
      return;
    }
    m_repositions.pop_front();
    if (!m_repositions.empty()) {
      m_popup->scheduled = m_repositions.front();
      unconstrain();
    }
  }

  void Popup::handleMap() {
    // Only a grabbing popup owns the keyboard. A toplevel's popup reaches it through its already focused window.
    if (m_popup->seat == nullptr) {
      return;
    }
    if (LayerSurface* layer = LayerSurface::fromSurface(m_popup->parent)) {
      layer->focusForPopupGrab();
    }
  }

  void Popup::handleDestroy() {
    wl_list_remove(&m_commit.link);
    wl_list_remove(&m_reposition.link);
    wl_list_remove(&m_configure.link);
    wl_list_remove(&m_map.link);
    wl_list_remove(&m_unmap.link);
    wl_list_remove(&m_destroy.link);
    m_commit.link.next = nullptr;
    m_reposition.link.next = nullptr;
    m_configure.link.next = nullptr;
    m_map.link.next = nullptr;
    m_unmap.link.next = nullptr;
    m_destroy.link.next = nullptr;
    delete this;
  }

} // namespace andromeda
