#include "config/change.h"
#include "config/config.h"
#include "config/config_watcher.h"
#include "config/store.h"
#include "core/log.h"
#include "core/main_loop_worker.h"
#include "desktop-unstable-v1-protocol.h"
#include "input/cursor.h"
#include "input/eis.h"
#include "input/gestures.h"
#include "input/input_capture.h"
#include "input/keyboard.h"
#include "input/keyboard_leds.h"
#include "input/seat.h"
#include "input/system_keymap.h"
#include "input/text_input.h"
#include "layer/layer_surface.h"
#include "layout/scrolling.h"
#include "lock/session_lock.h"
#include "output/display_store.h"
#include "output/stall_probe.h"
#include "output/identity.h"
#include "output/mirror.h"
#include "output/output.h"
#include "overview/overview.h"
#include "protocols/xdg_dirs.h"
#include "qos/controller.h"
#include "qos/holds.h"
#include "scene/effect_registry.h"
#include "scene/hint_rect.h"
#include "scene/quit_confirm.h"
#include "server/allocator.h"
#include "server/backend_manager.h"
#include "server/desktop_output_manager.h"
#include "server/desktop_settings_manager.h"
#include "server/desktop_shell.h"
#include "server/ipc.h"
#include "provider/provider.h"
#include "server/ipc_commands.h"

#include <nlohmann/json.hpp>
#include "server/global_shortcut_service.h"
#include "server/server.h"
#include "server/event_pacer.h"
#include "util/driver_threads.h"
#include "protocols/async_file_writer.h"
#include "core/screen_activity_recorder.h"
#include "server/server_events_internal.h"
#include "server/xdg_session.h"
#include "view/popup.h"
#include "view/view.h"
#include "wlr.h"
#include "workspace/scratchpad.h"
#include "workspace/workspace.h"
#include "xwayland/xwayland.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace andromeda {

  namespace {
    constexpr Logger kLog("server");
    using server_events::viewForToplevel;
    using server_events::applyPointerConfig;
  } // namespace

  void Server::applyConfig(const ConfigEffects& effects) {
    if (!effects.any()) {
      return;
    }
    scheduleQos();
    // Settle every interaction whose meaning depends on the workspace axis or the
    // layout objects about to be replaced, while those still exist.
    if (effects.workspaceLayout || effects.workspaceInventory || effects.outputState) {
      m_cursor->cancelLayoutInteraction();
      m_gestures->cancelForLayoutChange();
      for (const auto& output : m_outputs) {
        if (WorkspaceGroup* group = output->workspaceGroup()) {
          group->slideFinish();
        }
      }
    }
    if (effects.effects) {
      reconcileEffectSelections();
    }
    if (effects.animation || effects.effects) {
      effectRegistry().prepare(m_renderer);
    }
    if (effects.effects) {
      // The next frame re-arms the effect timer from the new max_fps.
      for (const auto& output : m_outputs) {
        if (output->effectEligible() > 0) {
          output->scheduleEffectFrame();
        }
      }
    }

    if (effects.sceneBlur) {
      applyBlurData();
      for (const auto& output : m_outputs) {
        output->markDirty(Dirty::LayerArrange);
      }
    }
    if (effects.input) {
      applyKeyboardConfig();
      for (const auto& pointer : m_pointers) {
        applyPointerConfig(pointer->device);
      }
      for (const auto& tablet : m_tabletDevices) {
        applyTabletConfig(*tablet);
      }
      for (const auto& touch : m_touchDevices) {
        applyTouchConfig(*touch);
      }
      for (const auto& pad : m_tabletPads) {
        applyTabletPadConfig(*pad);
      }
      m_seat->applyConfig();
      m_cursor->applyConfig();
      for (const auto& output : m_outputs) {
        output->applyCursorConfig();
      }
    }
    if (effects.outputState) {
      // A successful output-policy reload is the explicit boundary for
      // temporary output-management choices, including overrides retained for
      // a currently disconnected monitor.
      m_outputEnableOverrides.clear();
      applyConfiguredMirrors();
      m_deferOutputManagerConfig = true;
      for (const auto& output : m_outputs) {
        (void)output->applyOutputState();
      }
      m_deferOutputManagerConfig = false;
      for (const auto& output : m_outputs) {
        if (output->desktopEnabled()) {
          continue;
        }
        Output* fallback = nullptr;
        for (const auto& candidate : m_outputs) {
          if (candidate.get() != output.get() && candidate->desktopEnabled() && candidate->wlr()->enabled) {
            fallback = candidate.get();
            break;
          }
        }
        reassignOutputViews(output.get(), fallback);
      }
      scheduleDisplacedViewRestore();
      updateOutputManagerConfig();
      // A disabled output must not keep keyboard focus: pull it onto a live one.
      refocus();
      // Scale may have changed; every surface must hear about it or scale-aware
      // clients keep mapping input with the stale scale.
      refreshSurfaceScales();
    }
    if (effects.tearingPolicy) {
      for (const auto& output : m_outputs) {
        output->resetTearingState();
      }
    }
    if (effects.directScanoutPolicy) {
      for (const auto& output : m_outputs) {
        output->applyDirectScanoutConfig();
      }
    }
    if (effects.workspaceInventory) {
      for (const auto& output : m_outputs) {
        if (WorkspaceGroup* group = output->workspaceGroup()) {
          group->reconcileInventory();
        }
      }
    }
    if (effects.workspaceLayout) {
      for (const auto& output : m_outputs) {
        if (WorkspaceGroup* group = output->workspaceGroup()) {
          group->refreshLayouts();
        }
      }
      m_cursor->cancelStaleTiledResize();
    }
    if (effects.viewChrome) {
      for (View* view : sortedEffectViews()) {
        view->refreshConfigChrome();
      }
      Popup::applyGlassToAll();
      if (m_scratchpadManager != nullptr) {
        m_scratchpadManager->refreshBackdrops();
      }
      // Screen and cursor presets take their palette from [colors] too.
      m_effects.applyOutputEffects();
      markDirty(Dirty::Backdrop);
      if (sessionLocked()) {
        updateLockBlank();
      }
    }
    if (effects.effects && !effects.viewChrome) {
      for (View* view : sortedEffectViews()) {
        view->applyDynamicRules();
      }
    }
    if (effects.animation && m_scratchpadManager != nullptr) {
      m_scratchpadManager->applyConfig();
    }
    // A maximized scratchpad window follows the gap, border, and struts it is sized with; no workspace arrange reaches
    // it.
    if ((effects.workspaceLayout || effects.viewChrome) && m_scratchpadManager != nullptr) {
      m_scratchpadManager->refitMaximized();
    }
    if (effects.layerEffects) {
      for (const auto& layer : m_layerSurfaces) {
        if (layer->mapped()) {
          layer->applyConfig();
        }
      }
    }
    if (effects.dialogAttachment) {
      // Focus moves promote views in the registry, so walk a snapshot.
      std::vector<View*> views;
      views.reserve(m_registry.all().size());
      for (const auto& view : m_registry.all()) {
        views.push_back(view.get());
      }
      for (View* view : views) {
        if (view->mapped()) {
          view->reconcileAttachment();
        }
      }
    }
  }

  xkb_keymap* Server::compileKeymap(const XkbNames& names) {
    if (m_compiledKeymap == nullptr || names != m_compiledKeymapNames) {
      xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
      if (context == nullptr) {
        return nullptr;
      }
      const xkb_rule_names rules{
          .rules = nullptr,
          .model = names.model.empty() ? nullptr : names.model.c_str(),
          .layout = names.layout.empty() ? nullptr : names.layout.c_str(),
          .variant = names.variant.empty() ? nullptr : names.variant.c_str(),
          .options = names.options.empty() ? nullptr : names.options.c_str(),
      };
      xkb_keymap* keymap = xkb_keymap_new_from_names(context, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
      xkb_context_unref(context);
      if (keymap == nullptr) {
        return nullptr;
      }
      xkb_keymap_unref(m_compiledKeymap);
      m_compiledKeymap = keymap;
      m_compiledKeymapNames = names;
    }
    return xkb_keymap_ref(m_compiledKeymap);
  }

  void Server::applyKeyboardConfig() {
    // A reload reads the XKB files again.
    xkb_keymap_unref(m_compiledKeymap);
    m_compiledKeymap = nullptr;
    m_surfaceLayouts.clear();
    for (const auto& keyboard : m_keyboards) {
      keyboard->applyConfig();
      if (m_inputCapture != nullptr && !keyboard->virtualDevice()) {
        m_inputCapture->updateKeymap(keyboard->wlr());
      }
      // A rebuilt keymap can gain locks absent from its predecessor. Reapply
      // shared state after the per-device restoration has finished.
      refreshKeyboardLeds();
    }
    if (m_eisInput != nullptr) {
      m_eisInput->applyConfig();
    }
    if (m_keyboardLayoutSource != nullptr) {
      syncKeyboardLayout(m_keyboardLayoutSource);
      notifyKeyboardLayoutIpc();
    }
  }

  void Server::refreshSystemKeymapAsync() {
    if (!usesSystemXkbNames(config().input)) {
      if (adoptSystemKeymap({})) {
        applyKeyboardConfig();
      }
      return;
    }
    if (m_keymapWorker == nullptr) {
      m_keymapWorker = std::make_unique<MainLoopWorker>(wl_display_get_event_loop(m_display), "andro-keymap");
      m_keymapQuery = std::make_unique<SystemKeymapQuery>(wl_display_get_event_loop(m_display));
    }
    m_keymapQuery->query([this](XkbNames names) {
      m_keymapWorker->post<XkbNames>(
          [names = std::move(names)]() mutable { return validSystemKeymap(std::move(names)); },
          [this](XkbNames valid) {
            // A later reload may have turned the system keymap off while the read ran.
            if (usesSystemXkbNames(config().input) && adoptSystemKeymap(std::move(valid))) {
              applyKeyboardConfig();
            }
          }
      );
    });
  }

  bool Server::adoptSystemKeymap(XkbNames names) {
    if (names == m_systemKeymap) {
      return false;
    }
    if (!names.empty()) {
      kLog.info(
          "using the systemd-localed keymap: layout='{}' variant='{}' options='{}' model='{}'", names.layout,
          names.variant, names.options, names.model
      );
    }
    m_systemKeymap = std::move(names);
    return true;
  }

  void Server::handleConfigReload(const ConfigReloadResult& result) {
    cancelModifierTap();
    if (result.success) {
      StallProbe::setDeveloper(config().general.developerDiagnostics);
    }
    // A reload that changed nothing (the watcher seeing the compositor's own settings write) wakes no client.
    if (result.success && !result.change.any() && !result.effects.any()) {
      kLog.info("config reloaded (sections: none; effects: none)");
      if (m_desktopSettingsManager != nullptr) {
        m_desktopSettingsManager->settingsFileMaybeChanged();
      }
    } else if (result.success) {
      // The localed query is a D-Bus round trip plus a keymap compile (~10 ms on an HD 520 laptop): only an input edit
      // asks for it, and it runs on a worker.
      if (result.change.input) {
        refreshSystemKeymapAsync();
      }
      if (result.change.drm) {
        kLog.warn("DRM configuration changed; restart Andromeda to apply it");
      }
      if (result.change.keybinds) {
        m_bindCooldowns.clear();
      }
      if ((result.change.globalShortcuts || result.change.general || result.change.keybinds)
          && m_globalShortcutService != nullptr) {
        m_globalShortcutService->applyConfig();
      }
      if (result.change.scratchpads && m_scratchpadManager != nullptr) {
        m_scratchpadManager->reconcileConfig();
      }
      if (result.effects.invalidatesOverview()) {
        // A four-finger gesture must not reopen a presentation this close invalidates.
        m_gestures->cancelForLayoutChange();
        m_overview->forceClose();
      }
      applyConfig(result.effects);
      if (m_desktopSettingsManager != nullptr) {
        m_desktopSettingsManager->settingsChanged();
      }
      if (m_desktopOutputManager != nullptr) {
        m_desktopOutputManager->propertiesChanged();
      }
      if ((result.change.colors || result.change.appearance) && m_ipc != nullptr) {
        m_ipc->notifyThemeChanged();
      }
      const std::string changed = result.change.summary();
      const std::string effects = result.effects.summary();
      kLog.info(
          "config reloaded (sections: {}; effects: {})", changed.empty() ? "none" : changed,
          effects.empty() ? "none" : effects
      );
    }
    showConfigDiagnostics();
    if (m_configWatcher != nullptr) {
      m_configWatcher->watch(configWatchPaths());
    }
  }

    namespace {
    // A view that draws frames gets its callbacks from what draws it: a shown view from its outputs, including the
    // outgoing workspace mid-slide, an overview card or capture from its own frame, and a background_render view from
    // its own faster tick. A suspended one gets them only to commit a configure it still owes, such as a resize while
    // hidden. A view with no callback pending needs none.
    bool needsBackgroundFrame(const View& view) {
      return view.mapped() && !view.presenceFacts().drawsFrames && (!view.suspended() || view.configureOutstanding())
          && view.framePending();
    }
  } // namespace

  // Sends frame callbacks to toplevels off the rendered scene that could not be suspended (see
  // kBackgroundFrameIntervalMs).
  int Server::onBackgroundFrameTimer(void* data) {
    auto* self = static_cast<Server*>(data);
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);

        self->m_backgroundFrameArmed = false;
    for (const auto& view : self->m_registry.all()) {
      if (!needsBackgroundFrame(*view)) {
        continue;
      }
      view->forEachSurface(
          [](wlr_surface* surface, int /*sx*/, int /*sy*/, void* userData) {
            wlr_surface_send_frame_done(surface, static_cast<timespec*>(userData));
          },
          &now
      );
    }

    self->armBackgroundFrameTimer();
    return 0;
  }

  void Server::armBackgroundFrameTimer() {
    if (m_backgroundFrameTimer == nullptr) {
      return;
    }
        // Armed only while a hidden window waits for a callback, so an idle desktop gets no wakeup. A pending tick keeps its
    // deadline: a client that commits often must not push back the callbacks of the others.
    const bool wanted = std::ranges::any_of(m_registry.all(), [](const auto& view) { return needsBackgroundFrame(*view); });
    if (wanted == m_backgroundFrameArmed) {
      return;
    }
    m_backgroundFrameArmed = wanted;
    wl_event_source_timer_update(m_backgroundFrameTimer, wanted ? kBackgroundFrameIntervalMs : 0);
  }

  void Server::syncSuspendedViews() {
    for (const auto& view : m_registry.all()) {
            view->syncPresence();
    }
    if (m_dskSurfaces != nullptr) {
      for (const auto& layer : m_layerSurfaces) {
        m_dskSurfaces->sendPresenceTree(layer->layerSurface()->surface, layerPresence(*layer));
      }
    }
    armBackgroundRenderTimer();
  }

  namespace {
    // A hidden background_render window's callback interval: its output's refresh period, or 60 Hz when the output
    // reports none (headless and nested outputs); never faster than the display.fps_cap_hidden hold.
    int backgroundRenderIntervalMs(const View& view) {
      const Output* output = view.currentOutput();
      const int refreshMhz = output != nullptr ? output->wlr()->refresh : 0;
      const int interval = refreshMhz <= 0 ? 16 : std::max(1, (1'000'000 + refreshMhz / 2) / refreshMhz);
      const unsigned cap = qos::registry().hiddenFpsCap;
      return cap == 0 ? interval : std::max(interval, static_cast<int>((1000 + cap / 2) / cap));
    }

    bool needsBackgroundRender(const View& view) {
      return view.mapped() && view.backgroundRender() && view.presenceFacts().presence != Presence::Shown;
    }
  } // namespace

  int Server::onBackgroundRenderTimer(void* data) {
    auto* self = static_cast<Server*>(data);
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    for (const auto& view : self->m_registry.all()) {
      if (!needsBackgroundRender(*view)) {
        continue;
      }
      view->forEachSurface(
          [](wlr_surface* surface, int /*sx*/, int /*sy*/, void* userData) {
            wlr_surface_send_frame_done(surface, static_cast<timespec*>(userData));
          },
          &now
      );
    }
    self->m_backgroundRenderBase = std::chrono::steady_clock::now();
    self->armBackgroundRenderTimer();
    return 0;
  }

  void Server::armBackgroundRenderTimer() {
    armBackgroundFrameTimer();
    if (m_backgroundRenderTimer == nullptr) {
      return;
    }
    int interval = 0;
    for (const auto& view : m_registry.all()) {
      if (needsBackgroundRender(*view)) {
        const int candidate = backgroundRenderIntervalMs(*view);
        interval = interval == 0 ? candidate : std::min(interval, candidate);
      }
    }
    if (interval == 0) {
      m_backgroundRenderBase.reset();
      wl_event_source_timer_update(m_backgroundRenderTimer, 0);
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!m_backgroundRenderBase) {
      m_backgroundRenderBase = now;
    }
    const auto left =
        std::chrono::ceil<std::chrono::milliseconds>(*m_backgroundRenderBase + std::chrono::milliseconds(interval) - now);
    // Zero would disarm the timer: an overdue tick fires after 1 ms.
    wl_event_source_timer_update(m_backgroundRenderTimer, static_cast<int>(std::max<int64_t>(1, left.count())));
  }

  void Server::scheduleSuspensionSync() {
    if (m_stopping || m_suspensionSyncIdle != nullptr) {
      return;
    }
    m_suspensionSyncIdle = wl_event_loop_add_idle(wl_display_get_event_loop(m_display), onSuspensionSyncIdle, this);
    if (m_suspensionSyncIdle == nullptr) {
      kLog.error("failed to register suspension sync idle source");
    }
  }

  void Server::onSuspensionSyncIdle(void* data) {
    auto* server = static_cast<Server*>(data);
    server->m_suspensionSyncIdle = nullptr;
    server->syncSuspendedViews();
  }

    Presence Server::layerPresence(const LayerSurface& layer) const {
    const Output* output = layer.output();
    return layerPresenceOf(output != nullptr && output->dpmsOff(), sessionLocked());
  }

  // Opaque coverage changed: Occluded follows on the next idle sync.
  void Server::onSceneBufferVisibility(wl_listener* listener, void* /*data*/) {
    Server* self = wl_container_of(listener, self, m_sceneBufferVisibility); // NOLINT(modernize-use-auto)
    self->scheduleSuspensionSync();
  }

  std::optional<Presence> Server::surfacePresence(wlr_surface* surface) const {
    // A cached presence may be stale until the pending sync, which sends to every surface tree.
    if (m_suspensionSyncIdle != nullptr) {
      return std::nullopt;
    }
    if (const LayerSurface* layer = LayerSurface::fromSurface(surface)) {
      return layerPresence(*layer);
    }
    // A deferred opener has no presence yet: View::syncPresence computes it once the view is Mapped.
    const View* view = View::fromSurface(surface);
    if (view == nullptr || view->lifecycle() != Lifecycle::Mapped) {
      return std::nullopt;
    }
    return view->presenceFacts().presence;
  }

    bool Server::viewDrawnOffscreen(const View& view) const {
    // A capture of a capture-blocked window shows black, so it needs no frames from the window.
    return (m_overview != nullptr && m_overview->presentsLive(&view)) || (viewCaptured(view) && !view.captureBlocked());
  }

  bool Server::outputCaptureListening(const wlr_output* output) const {
    if (std::ranges::any_of(m_imageCopySessions, [output](const std::unique_ptr<ImageCopySessionWatch>& watch) {
          return watch->session != nullptr && watch->output == output;
        })) {
      return true;
    }
    if (m_screencopyManager != nullptr) {
      wlr_screencopy_frame_v1* frame = nullptr;
      wl_list_for_each(frame, &m_screencopyManager->frames, link) {
        if (frame->output == output) {
          return true;
        }
      }
    }
    if (m_exportDmabufManager != nullptr) {
      wlr_export_dmabuf_frame_v1* frame = nullptr;
      wl_list_for_each(frame, &m_exportDmabufManager->frames, link) {
        if (frame->output == output) {
          return true;
        }
      }
    }
    return false;
  }

  bool Server::viewCaptured(const View& view) const {
    return view.m_captureSource != nullptr
        && std::ranges::any_of(m_imageCopySessions, [&view](const std::unique_ptr<ImageCopySessionWatch>& watch) {
             return watch->session != nullptr && watch->session->source == view.m_captureSource;
           });
  }

  int Server::onStartupRulesTimer(void* data) {
    auto* self = static_cast<Server*>(data);
    for (const auto& view : self->m_registry.all()) {
      view->refreshStartupRuleEffects();
    }
    for (const auto& layer : self->m_layerSurfaces) {
      layer->refreshStartupRuleEffects();
    }
    return 0;
  }

  // Fires when the underlying GL context is invalidated (GPU reset, VRAM lost after suspend, driver-detected hang); the
  // renderer must be rebuilt, since GL calls into a dead context do nothing. Defer rebuilding until this signal and the
  // failed render call have both unwound.
  void Server::onRendererLost(wl_listener* listener, void* /*data*/) {
    Server* self;
    self = wl_container_of(listener, self, m_rendererLost);
    if (self->m_stopping || self->m_rendererRecoveryIdle != nullptr) {
      return;
    }
    self->m_rendererRecoveryIdle =
        wl_event_loop_add_idle(wl_display_get_event_loop(self->m_display), onRendererRecoveryIdle, self);
    if (self->m_rendererRecoveryIdle == nullptr) {
      kLog.error("could not defer renderer recovery, terminating");
      self->stop();
    }
  }

  void Server::onRendererRecoveryIdle(void* data) {
    auto* self = static_cast<Server*>(data);
    self->m_rendererRecoveryIdle = nullptr;
    if (!self->m_stopping) {
      self->recreateRenderer();
    }
  }

  void Server::startRenderThread() {
    const char* env = std::getenv("ANDROMEDA_RENDER_THREAD");
    if (env != nullptr && std::string_view(env) == "0") {
      return;
    }
    if (fx_renderer_start_render_thread(m_renderer, wl_display_get_event_loop(m_display))) {
      kLog.info("render thread started; {} GPU thread(s) share the main thread's real-time policy",
                shareSchedulingWithDriverThreads());
    } else {
      kLog.warn("render thread failed to start; submitting on the main thread");
    }
  }

  void Server::recreateRenderer() {
    kLog.warn("GPU context lost, recreating renderer");

    wlr_renderer* oldRenderer = m_renderer;
    wlr_allocator* oldAllocator = m_allocator;

    wlr_renderer* newRenderer = m_backendManager->createRenderer();
    if (newRenderer == nullptr) {
      kLog.error("could not recreate fx_renderer after GPU reset, terminating");
      stop();
      return;
    }
    wlr_allocator* newAllocator = createAllocator(m_backend, newRenderer);
    if (newAllocator == nullptr) {
      kLog.error("could not recreate allocator after GPU reset, terminating");
      wlr_renderer_destroy(newRenderer);
      stop();
      return;
    }
    if (!m_backendManager->verifyOpenDevices("after renderer recovery")) {
      wlr_allocator_destroy(newAllocator);
      wlr_renderer_destroy(newRenderer);
      kLog.error("renderer recovery opened an excluded GPU, terminating");
      stop();
      return;
    }

    // Rewire the lost signal onto the new renderer BEFORE swapping the pointers so that
    // a second reset during recreation is delivered.
    wl_list_remove(&m_rendererLost.link);
    wl_signal_add(&newRenderer->events.lost, &m_rendererLost);

    for (const auto& output : m_outputs) {
      if (auto* group = output->workspaceGroup()) {
        group->cancelReveal();
      }
    }
    m_renderer = newRenderer;
    m_allocator = newAllocator;
    startRenderThread();
    effectRegistry().prepare(m_renderer);
    // Rebind existing caches before the old renderer invalidates its programs.
    // A frozen clock may deduplicate every subsequent animation tick.
    for (const auto& view : m_registry.all()) {
      if (view->mapped()) {
        view->syncAnimationEffects();
      }
    }
    if (m_overview != nullptr) {
      m_overview->refreshEffectBindings();
    }

    // Point the compositor at the new renderer so clients' shm/dma-buf textures get
    // re-imported on next attach.
    wlr_compositor_set_renderer(m_compositor, newRenderer);

    // Re-init every output's render pipeline with the new renderer/allocator, and force
    // a fresh frame so damage tracking rebuilds from scratch.
    for (const auto& output : m_outputs) {
      output->dropFrameTimers();
      wlr_output* wlrOutput = output->wlr();
      wlr_output_init_render(wlrOutput, newAllocator, newRenderer);
      output->requestFrame();
    }

    wlr_allocator_destroy(oldAllocator);
    wlr_renderer_destroy(oldRenderer);

    kLog.info("renderer recreated");
  }

  void
  Server::onProtocolMessage(void* data, wl_protocol_logger_type direction, const wl_protocol_logger_message* message) {
    if (direction != WL_PROTOCOL_LOGGER_REQUEST
        || message == nullptr
        || message->resource == nullptr
        || message->message == nullptr
        || message->arguments_count != 1
        || message->arguments == nullptr) {
      return;
    }
    const char* resourceClass = wl_resource_get_class(message->resource);
    if (resourceClass == nullptr
        || std::string_view(resourceClass) != "xdg_toplevel"
        || std::string_view(message->message->name) != "set_parent") {
      return;
    }

    auto* server = static_cast<Server*>(data);
    wlr_xdg_toplevel* toplevel = wlr_xdg_toplevel_from_resource(message->resource);
    if (View* view = viewForToplevel(*server, toplevel)) {
      // wlroots has not handled the request yet, so the raw nullable object is
      // still available even when its toplevel has not mapped.
      view->recordOpeningParentRequest(message->arguments[0].o != nullptr);
    }
  }

  void Server::onNewIdleInhibitor(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newIdleInhibitor);
    auto* inhibitor = static_cast<wlr_idle_inhibitor_v1*>(data);
    auto* watch = new IdleInhibitorWatch();
    watch->server = self;
    watch->inhibitor = inhibitor;
    watch->destroy.notify = onIdleInhibitorDestroy;
    wl_signal_add(&inhibitor->events.destroy, &watch->destroy);
    self->updateIdleInhibit();
  }

  void Server::onIdleInhibitorDestroy(wl_listener* listener, void* /*data*/) {
    IdleInhibitorWatch* watch;
    watch = wl_container_of(listener, watch, destroy);
    Server* server = watch->server;
    const wlr_idle_inhibitor_v1* dying = watch->inhibitor;
    wl_list_remove(&watch->destroy.link);
    delete watch;
    server->updateIdleInhibit(dying);
  }

  void Server::onNewImageCopySession(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newImageCopySession);
    auto* session = static_cast<wlr_ext_image_copy_capture_session_v1*>(data);
    auto watch = std::make_unique<ImageCopySessionWatch>();
    watch->server = self;
    watch->session = session;
    watch->output = wlr_output_try_from_ext_image_capture_source_v1(session->source);
    watch->destroy.notify = onImageCopySessionDestroy;
    wl_signal_add(&session->events.destroy, &watch->destroy);
    self->m_imageCopySessions.push_back(std::move(watch));
    self->updatePresenting();
    self->scheduleSuspensionSync();
    self->refreshCaptureRuleEffects(session->source);
  }

  // The session's render lock is released after this signal; the frame it schedules runs from an idle, without it.
  void Server::onImageCopySessionDestroy(wl_listener* listener, void* /*data*/) {
    ImageCopySessionWatch* watch;
    watch = wl_container_of(listener, watch, destroy);
    Server* server = watch->server;
    wlr_ext_image_capture_source_v1* source = watch->session->source;
    wl_list_remove(&watch->destroy.link);
    std::erase_if(server->m_imageCopySessions, [watch](const std::unique_ptr<ImageCopySessionWatch>& entry) {
      return entry.get() == watch;
    });
    for (const auto& output : server->m_outputs) {
      output->scheduleEffectCaptureRelease();
    }
    server->updatePresenting();
    server->scheduleSuspensionSync();
    server->refreshCaptureRuleEffects(source);
  }

  // `match.is_cast_target` follows the sessions on a window's source. Output sources match no view and change nothing.
  // Closing a cast window ends its session from inside ~View, while the registry holds the dying entry as null.
  void Server::refreshCaptureRuleEffects(const wlr_ext_image_capture_source_v1* source) {
    for (const auto& view : m_registry.all()) {
      if (view != nullptr && view->m_captureSource == source) {
        view->refreshStateRuleEffects();
      }
    }
  }

  void Server::onNewShortcutsInhibitor(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_newShortcutsInhibitor);
    auto* inhibitor = static_cast<wlr_keyboard_shortcuts_inhibitor_v1*>(data);
    auto watch = std::make_unique<ShortcutsInhibitorWatch>();
    watch->server = self;
    watch->inhibitor = inhibitor;
    watch->destroy.notify = onShortcutsInhibitorDestroy;
    wl_signal_add(&inhibitor->events.destroy, &watch->destroy);
    self->m_shortcutsInhibitors.push_back(std::move(watch));
    self->refreshKeyboardShortcutsInhibitors();
    kLog.debug("keyboard shortcuts inhibitor {}", inhibitor->active ? "activated" : "blocked by window rule");
  }

  void Server::onShortcutsInhibitorDestroy(wl_listener* listener, void* /*data*/) {
    ShortcutsInhibitorWatch* watch;
    watch = wl_container_of(listener, watch, destroy);
    Server* server = watch->server;
    wl_list_remove(&watch->destroy.link);
    std::erase_if(server->m_shortcutsInhibitors, [watch](const std::unique_ptr<ShortcutsInhibitorWatch>& candidate) {
      return candidate.get() == watch;
    });
    kLog.debug("keyboard shortcuts inhibitor removed");
  }
  void Server::onPointerDestroy(wl_listener* listener, void* /*data*/) {
    PointerDevice* watch;
    watch = wl_container_of(listener, watch, destroy);
    Server* server = watch->server;
    if (server->m_inputCapture != nullptr) {
      server->m_inputCapture->forgetAbsoluteDevice(watch->device);
      if ((server->m_inputCapture->capabilities() & 2U) != 0) {
        server->m_inputCapture->suspend();
      }
    }
    wl_list_remove(&watch->destroy.link);
    std::erase_if(server->m_pointers, [watch](const std::unique_ptr<PointerDevice>& pointer) {
      return pointer.get() == watch;
    });
    server->inputDevicesChanged();
  }

  void Server::onWorkspaceCommit(wl_listener* listener, void* data) {
    Server* self;
    self = wl_container_of(listener, self, m_workspaceCommit);
    self->handleWorkspaceCommit(data);
  }

  void Server::handleWorkspaceCommit(void* data) {
    auto* event = static_cast<wlr_ext_workspace_v1_commit_event*>(data);
    wlr_ext_workspace_v1_request* request = nullptr;
    wl_list_for_each(request, event->requests, link) {
      switch (request->type) {
      case WLR_EXT_WORKSPACE_V1_REQUEST_CREATE_WORKSPACE: {
        WorkspaceGroup* group = workspaceGroupFromHandle(request->create_workspace.group);
        if (group != nullptr) {
          group->createWorkspace(request->create_workspace.name);
        }
        break;
      }
      case WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE: {
        if (Workspace* workspace = workspaceFromHandle(request->activate.workspace)) {
          workspace->group()->activate(workspace);
          refocus(workspace->group()->output());
        }
        break;
      }
      case WLR_EXT_WORKSPACE_V1_REQUEST_DEACTIVATE: {
        if (Workspace* workspace = workspaceFromHandle(request->deactivate.workspace)) {
          workspace->group()->deactivate(workspace);
        }
        break;
      }
      case WLR_EXT_WORKSPACE_V1_REQUEST_ASSIGN:
        // Workspaces stay bound to their output group.
        break;
      case WLR_EXT_WORKSPACE_V1_REQUEST_REMOVE:
        break;
      }
    }
  }

  Workspace* Server::workspaceFromHandle(wlr_ext_workspace_handle_v1* handle) const {
    if (handle == nullptr || handle->data == nullptr) {
      return nullptr;
    }
    return static_cast<Workspace*>(handle->data);
  }

  WorkspaceGroup* Server::workspaceGroupFromHandle(wlr_ext_workspace_group_handle_v1* handle) const {
    if (handle == nullptr || handle->data == nullptr) {
      return nullptr;
    }
    return static_cast<WorkspaceGroup*>(handle->data);
  }

  void Server::updateIdleInhibit(const wlr_idle_inhibitor_v1* dying) {
    bool inhibited = false;
    wlr_idle_inhibitor_v1* inhibitor;
    wl_list_for_each(inhibitor, &m_idleInhibitManager->inhibitors, link) {
      if (inhibitor == dying) {
        continue;
      }
      wlr_surface* root = wlr_surface_get_root_surface(inhibitor->surface);
      if (root == nullptr) {
        continue;
      }

      if (sessionLocked()) {
        wlr_session_lock_surface_v1* lockSurface = wlr_session_lock_surface_v1_try_from_wlr_surface(root);
        inhibited =
            lockSurface != nullptr && root->mapped && lockSurface->output != nullptr && lockSurface->output->enabled;
      } else if (View* view = View::fromSurface(root)) {
        int x = 0;
        int y = 0;
        inhibited = view->mapped() && wlr_scene_node_coords(&view->sceneTree()->node, &x, &y);
      } else if (wlr_layer_surface_v1* wlrLayer = wlr_layer_surface_v1_try_from_wlr_surface(root)) {
        auto* layer = static_cast<LayerSurface*>(wlrLayer->data);
        Output* output = layer != nullptr ? layer->output() : nullptr;
        int x = 0;
        int y = 0;
        inhibited = layer != nullptr
            && layer->mapped()
            && output != nullptr
            && output->wlr()->enabled
            && wlr_scene_node_coords(&layer->scene()->tree->node, &x, &y);
      }

      if (inhibited) {
        break;
      }
    }
    if (inhibited != m_idleInhibited) {
      m_idleInhibited = inhibited;
      kLog.debug("idle {}", inhibited ? "inhibited" : "no longer inhibited");
      if (m_screenActivity != nullptr) {
        m_screenActivity->setInhibited(inhibited);
      }
    }
    wlr_idle_notifier_v1_set_inhibited(m_idleNotifier, inhibited);
    // Every map, unmap and inhibitor change passes here.
    scheduleQos();
  }

  void Server::scheduleQos() {
    if (m_qos != nullptr) {
      m_qos->schedule();
    }
  }

  void Server::holdQosThawIntent(bool held) {
    if (m_qos != nullptr) {
      m_qos->holdThawIntent(held);
    }
  }

  void Server::notifyIdleActivity() {
    wlr_idle_notifier_v1_notify_activity(m_idleNotifier, m_seat->wlr());
    if (m_screenActivity != nullptr) {
      m_screenActivity->inputActivity();
    }
  }

  void Server::notifyInputActivity() {
    wakeDpmsOutputs();
    notifyIdleActivity();
  }

  void Server::wakeDpmsOutputs() {
    const bool anyPowered = std::ranges::any_of(m_outputs, [](const std::unique_ptr<Output>& output) {
      return output->desktopEnabled() && !output->dpmsOff();
    });
    if (anyPowered) {
      return;
    }
    for (const auto& output : m_outputs) {
      if (output->dpmsOff()) {
        (void)output->setPowered(true);
      }
    }
  }

  void Server::notifyOverviewChanged() {
    if (m_ipc != nullptr) {
      m_ipc->notifyOverviewChanged();
    }
  }

  void Server::notifySubmapChanged() {
    if (m_ipc != nullptr) {
      m_ipc->notifySubmapChanged();
    }
  }

  void Server::scheduleIpcWindowsEvent(bool sessionChanged) {
    if (sessionChanged) {
      scheduleSessionSave();
    }
    if (m_ipcWindowsPacer != nullptr) {
      m_ipcWindowsPacer->schedule();
    }
  }

  // docs/resilience.md §5: the window list is current at a crash, at most one write per second. After a log-out request
  // the user manager stops the apps before the compositor, so their unmaps must not reach session.json.
  void Server::scheduleSessionSave() {
    if (m_stopping || m_sessionSavePending) {
      return;
    }
    if ((!m_lockFileManaged || m_sessionExit != SessionExit::Idle) && (m_xdgSessions == nullptr || !m_xdgSessions->wantsFlush())) {
      return;
    }
    if (m_sessionSaveTimer == nullptr) {
      m_sessionSaveTimer = wl_event_loop_add_timer(
          wl_display_get_event_loop(m_display),
          [](void* data) {
            static_cast<Server*>(data)->saveSession();
            return 0;
          },
          this
      );
      if (m_sessionSaveTimer == nullptr) {
        return;
      }
    }
    if (!m_sessionSavePending) {
      m_sessionSavePending = true;
      wl_event_source_timer_update(m_sessionSaveTimer, 1000);
    }
  }

  void Server::saveSession() {
    m_sessionSavePending = false;
    if (m_xdgSessions != nullptr) {
      m_xdgSessions->flush();
    }
    if (!m_lockFileManaged || m_sessionExit != SessionExit::Idle) {
      return;
    }
    const std::string state = xdg::stateHome();
    const std::filesystem::path dir = state.empty() ? std::filesystem::path() : std::filesystem::path(state) / "andromeda";
    if (dir.empty()) {
      return;
    }
    nlohmann::json windows = IpcCommands::windows(*this, "")["ok"];
    // Restore reads `placement`: the workspace by its live position or name, as ids count creations.
    std::map<std::string_view, const View*> byId;
    for (const auto& view : views()) {
      if (view->mapped() && view->extForeignIdentifier() != nullptr) {
        byId.emplace(view->extForeignIdentifier(), view.get());
      }
    }
    for (nlohmann::json& window : windows) {
      if (const auto view = byId.find(window.value("id", "")); view != byId.end()) {
        window["placement"] = sessionPlacementJson(sessionPlacementOf(*view->second));
      }
    }
    writeFileAsync(dir / "session.json", nlohmann::json{{"version", 1}, {"windows", windows}}.dump());
  }

  void Server::emitIpcWindowsEvent() {
    m_ipc->notifyWindowsChanged();
    m_provider->notifyChanged(Provider::Family::Windows);
  }

  void Server::scheduleIpcWorkspacesEvent() {
    // Outputs report their active workspace and focus, which move with the workspace list.
    scheduleIpcOutputsEvent();
    if (m_ipcWorkspacesPacer != nullptr) {
      m_ipcWorkspacesPacer->schedule();
    }
  }

  void Server::emitIpcWorkspacesEvent() {
    m_ipc->notifyWorkspacesChanged();
    m_provider->notifyChanged(Provider::Family::Workspaces);
  }

  void Server::scheduleIpcLayersEvent() {
    if (m_ipcLayersPacer != nullptr) {
      m_ipcLayersPacer->schedule();
    }
  }

  void Server::emitIpcLayersEvent() { m_ipc->notifyLayersChanged(); }

  void Server::scheduleIpcOutputsEvent() {
    if (m_ipcOutputsPacer != nullptr) {
      m_ipcOutputsPacer->schedule();
    }
  }

  void Server::emitIpcOutputsEvent() {
    m_ipc->notifyOutputsChanged();
    m_provider->notifyChanged(Provider::Family::Outputs);
  }

} // namespace andromeda
