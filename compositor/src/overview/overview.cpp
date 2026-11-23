#include "overview/overview_internal.h"
#include "server/dsk_surface.h"
#include <cstdlib>

namespace andromeda {

  namespace {
    int countBuffers(wlr_scene_tree* tree) {
      int count = 0;
      if (tree == nullptr) {
        return 0;
      }
      wlr_scene_node_for_each_buffer(
          &tree->node, [](wlr_scene_buffer*, int, int, void* data) { ++*static_cast<int*>(data); }, &count
      );
      return count;
    }
  } // namespace

  using namespace ovimpl;


  Overview::Overview(Server& server) : m_server(&server) {
    m_server->registerAnimatable(this);
    // An icon that finished loading joins the capsules on the next chrome layout.
    m_appIcons = std::make_unique<ov::AppIcons>(wl_display_get_event_loop(server.display()), [this] {
      if (!m_active) {
        return;
      }
      const View* liveTarget = liveTargetView();
      for (const auto& state : m_outputs) {
        layoutChrome(*state, liveTarget);
        state->output->requestFrame();
      }
    });
  }

  Overview::~Overview() {
    m_server->unregisterAnimatable(this);
    m_zoomAnim.snap(0.0);
    teardown();
    if (m_edgeTimer != nullptr) {
      wl_event_source_remove(m_edgeTimer);
    }
  }

  float Overview::desktopChromeAlpha() const {
    return static_cast<float>(std::clamp(1.0 - m_progress / kDesktopChromeFade, 0.0, 1.0));
  }

  double Overview::motionPixelsPerUnit(bool rows) const {
    // A window travels at most from its desktop rect, one output extent away per workspace, to its place; a row step
    // moves anything by at most one output extent. Whole pixels per unit keep the target on the pixel grid
    // finishSpringTail rounds against.
    double bound = 0.0;
    for (const auto& state : m_outputs) {
      const auto extent = static_cast<double>(std::max(state->outputBox.width, state->outputBox.height));
      const double steps = rows ? 1.0 : static_cast<double>(state->workspaceViews.size() + 1);
      bound = std::max(bound, extent * steps);
    }
    return std::ceil(bound);
  }

  // -: geometry

  bool Overview::refreshOutputBoxes(OutputState& state) const {
    wlr_box outputBox{};
    wlr_output_layout_get_box(m_server->outputLayout(), state.output->wlr(), &outputBox);
    if (outputBox.width <= 0 || outputBox.height <= 0) {
      return false;
    }
    state.outputBox = outputBox;
    state.usableBox = state.output->usableArea();
    if (state.usableBox.width <= 0 || state.usableBox.height <= 0) {
      state.usableBox = outputBox;
    }
    return true;
  }

  ov::Input Overview::placementInput(const OutputState& state, bool targets) const {
    ov::Input input;
    input.width = state.outputBox.width;
    input.height = state.outputBox.height;
    input.usable = localRect(state.usableBox, state.outputBox);
    input.ui = config().appearance.uiScale;
    // Strip rows leave 220 px for names, the filter and the neighbour peeks; `overview.zoom` caps the share.
    const double fill = input.height > 0 ? (input.height - 220.0) / input.height : 0.0;
    input.stripScale = std::clamp(fill, 0.1, std::clamp(config().overview.zoom, 0.1, 0.75));
    const WorkspaceGroup* group = state.output->workspaceGroup();
    input.horizontal = group != nullptr && group->workspaceAxis() == WorkspaceAxis::Horizontal;
    input.workspaces.resize(state.workspaceBackgrounds.size());
    for (size_t index = 0; group != nullptr && index < input.workspaces.size(); ++index) {
      if (const Workspace* workspace = group->workspaceAt(index)) {
        input.workspaces[index].arrangement = arrangementOf(workspace->layoutMode());
      }
    }
    for (size_t k = 0; k < state.cards.size(); ++k) {
      const Card& card = *state.cards[k];
      if (card.workspaceIndex >= input.workspaces.size() || !laidOut(card.view)) {
        continue;
      }
      wlr_box world = card.view->presentedBox();
      if (targets) {
        world.x = card.view->layoutTargetX();
        world.y = card.view->layoutTargetY();
      }
      input.workspaces[card.workspaceIndex].members.push_back(
          {static_cast<uint32_t>(k), localRect(world, state.outputBox), card.view->minimized()}
      );
    }
    return input;
  }

  ov::Fit Overview::workspaceFit(const OutputState& state, size_t index) {
    if (state.mainBox.has_value() && index == state.mainWorkspace) {
      return state.mainFit;
    }
    if (index < state.workspaceViews.size() && state.workspaceViews[index].shown) {
      return state.workspaceViews[index].fit;
    }
    return {1.0, static_cast<double>(state.outputBox.x), static_cast<double>(state.outputBox.y)};
  }

  bool Overview::workspaceViewContains(const OutputState& state, size_t index, double lx, double ly) {
    if (state.mainBox.has_value() && index == state.mainWorkspace && boxContains(*state.mainBox, lx, ly)) {
      return true;
    }
    return index < state.workspaceViews.size()
        && state.workspaceViews[index].shown
        && boxContains(state.workspaceViews[index].box, lx, ly);
  }

  double Overview::workspaceScale(const Workspace* workspace) const {
    for (const auto& state : m_outputs) {
      const WorkspaceGroup* group = state->output->workspaceGroup();
      if (workspace == nullptr || group == nullptr || workspace->group() != group || !state->placed) {
        continue;
      }
      // The settled scale, not the one mid-zoom: travel means the same before the opening has landed as after.
      const ov::Placement& placement = state->placement;
      const size_t index = workspace->index();
      if (placement.mainFit.has_value() && index == state->mainWorkspace) {
        return std::max(0.01, placement.mainFit->s);
      }
      for (const ov::Card& card : placement.cards) {
        if (card.workspace == index && state->placedInput.width > 0) {
          return std::max(0.01, card.rect.w / state->placedInput.width);
        }
      }
    }
    return 1.0;
  }

  // A placement cut in card-local pixels: its edges map onto the snapped content `box`, so an uncut side stays on the
  // box edge.
  static std::optional<wlr_box> cardClip(const wlr_box& world, const std::optional<ov::Insets>& cut, const wlr_box& box) {
    if (!cut.has_value()) {
      return std::nullopt;
    }
    const double sx = world.width > 0 ? static_cast<double>(box.width) / world.width : 1.0;
    const double sy = world.height > 0 ? static_cast<double>(box.height) / world.height : 1.0;
    const auto x0 = static_cast<int>(std::lround(cut->left * sx));
    const auto y0 = static_cast<int>(std::lround(cut->top * sy));
    const auto x1 = static_cast<int>(std::lround((world.width - cut->right) * sx));
    const auto y1 = static_cast<int>(std::lround((world.height - cut->bottom) * sy));
    return wlr_box{.x = x0, .y = y0, .width = std::max(0, x1 - x0), .height = std::max(0, y1 - y0)};
  }

  void Overview::layoutCard(Card& card, const View* liveTarget) {
    View* view = card.view;
    if (card.shadowTree != nullptr) {
      wlr_scene_node_set_enabled(&card.shadowTree->node, false);
    }
    const wlr_box& geometry = view->geometryBox();
    const wlr_box& world = view->presentedBox();
    // Not laid out yet (a tiled opener waiting for its arrange, a hidden tab sharing the shown tab's box), or faded
    // out by the placement: nothing to draw, nothing to hit.
    if (!laidOut(view) || (card.opacity < kHiddenOpacity && &card != dragCard())) {
      card.blur.hide();
      wlr_scene_node_set_enabled(&card.tree->node, false);
      syncCardEffects(card);
      return;
    }
    wlr_scene_node_set_enabled(&card.tree->node, true);

    const double z = card.scale;
    // A placed card keeps the box its edges snapped to: recomputing the size from the scale rounds it a second time
    // and steps an edge back. The size follows the presented box only when that box really changed since the output
    // was laid out (a client resize, more than the 1 px a second rounding moves), and for the drag, which owns the
    // dragged card's origin.
    const int scaledW = std::max(1, static_cast<int>(std::lround(world.width * z)));
    const int scaledH = std::max(1, static_cast<int>(std::lround(world.height * z)));
    if (&card == dragCard() || std::abs(scaledW - card.box.width) > 1 || std::abs(scaledH - card.box.height) > 1) {
      card.box.width = scaledW;
      card.box.height = scaledH;
    }
    card.clip = cardClip(world, card.cut, card.box);
    // A cut that leaves no whole pixel shows nothing; the scene reads an empty clip as none and would draw it all.
    if (card.clip.has_value() && (card.clip->width <= 0 || card.clip->height <= 0)) {
      card.blur.hide();
      wlr_scene_node_set_enabled(&card.tree->node, false);
      syncCardEffects(card);
      return;
    }
    const int contentW = card.box.width;
    const int contentH = card.box.height;
    wlr_scene_node_set_position(&card.tree->node, card.box.x, card.box.y);
    wlr_scene_tree_set_clip(card.tree, card.clip.has_value() ? &*card.clip : nullptr);
    layoutCardShadow(card, desktopChromeAlpha() * card.opacity);
    const float cardOpacity = (&card == dragCard() ? config().appearance.dragOpacity : 1.0F) * card.opacity;
    const float presentedOpacity = view->presentedOpacity() * cardOpacity;

    const int borderWidth = view->decorationBorderWidth();
    const int outerBorderWidth = view->decorationOuterBorderWidth();
    const int total = borderWidth + outerBorderWidth;
    // The live window's rule (View::surfaceRadius): a 0 px border still rounds the window.
    const bool decorated = view->decorated() && !view->currentFullscreen() && !view->maximizedToEdges();
    const int scaledRadius = static_cast<int>(std::lround(view->decorationCornerRadius() * z));
    const int outerRadius = decorated ? scaledRadius : 0;
    const auto scaledWidth = [z](int width) {
      return width > 0 ? std::max(1, static_cast<int>(std::lround(width * z))) : 0;
    };
    const int innerWidth = scaledWidth(borderWidth);
    const int outerWidth = scaledWidth(outerBorderWidth);
    const int surfaceRadius = nestedRadius(outerRadius, innerWidth + outerWidth);
    const bool borderVisible = decorated && innerWidth + outerWidth > 0;
    wlr_scene_node_set_enabled(&card.border->node, borderVisible);
    if (borderVisible) {
      card.borderPadding = scaledWidth(view->borderEffectPadding());
      applyBorderGeometry(
          card.border,
          makeBorderRing(
              contentW, contentH + static_cast<int>(std::lround(view->titleBarHeight() * z)), outerRadius, innerWidth,
              outerWidth, card.borderPadding
          ),
          innerWidth, outerWidth
      );
      wlr_scene_node_set_position(
          &card.border->node, card.border->node.x,
          card.border->node.y - static_cast<int>(std::lround(view->titleBarHeight() * z))
      );
      const std::array<float, 4> innerColor = tint(cardBorderColor(card, liveTarget), presentedOpacity);
      const std::array<float, 4> outerColor = tint(view->borderColors().outer, presentedOpacity);
      wlr_scene_border_set_colors(card.border, innerColor.data(), outerColor.data());
    }
    // Rulebook §11: a 2 px accent ring, 2 px outside the card's decorated edge and concentric with it.
    const bool ringShown = card.role == CardRole::Window && m_focusRing == FocusRing::Shown && interactive() &&
                           view == liveTarget && cardMatches(card);
    wlr_scene_node_set_enabled(&card.focusRing->node, ringShown);
    if (ringShown) {
      constexpr int kRingGap = 2;
      constexpr int kRingWidth = 2;
      const int titleHeight = static_cast<int>(std::lround(view->titleBarHeight() * z));
      const int inset = (borderVisible ? innerWidth + outerWidth : 0) + kRingGap;
      applyBorderGeometry(
          card.focusRing,
          makeBorderRing(
              contentW + 2 * inset, contentH + titleHeight + 2 * inset,
              outerRadius > 0 ? outerRadius + kRingGap + kRingWidth : 0, kRingWidth, 0
          ),
          kRingWidth, 0
      );
      wlr_scene_node_set_position(
          &card.focusRing->node, card.focusRing->node.x - inset, card.focusRing->node.y - inset - titleHeight
      );
      const std::array<float, 4> ring = tint(config().colors.accentPrimary, presentedOpacity);
      wlr_scene_border_set_colors(card.focusRing, ring.data(), ring.data());
    }
    layoutCardChrome(card, world, decorated ? total : 0, z, presentedOpacity);
    syncCardEffects(card);

    // Every surface of the card rounds against the card's content box, the same rule the live window uses, so a
    // client that draws its corners from a subsurface keeps them rounded in the thumbnail.
    const fx_corner_radii contentCorners = view->titleBarHeight() > 0
        ? corner_radii_new(0, 0, surfaceRadius, surfaceRadius)
        : corner_radii_all(surfaceRadius);
    const auto roundToCardBox = [surfaceRadius, contentW, contentH, contentCorners](wlr_scene_buffer* buffer) {
      const wlr_box cornerBox{-buffer->node.x, -buffer->node.y, contentW, contentH};
      wlr_scene_buffer_set_corner_radii(buffer, contentCorners);
      wlr_scene_buffer_set_corner_box(buffer, surfaceRadius > 0 ? &cornerBox : nullptr);
    };
    const double fx = static_cast<double>(contentW) / geometry.width;
    const double fy = static_cast<double>(contentH) / geometry.height;
    bool blurUpdated = false;
    for (const auto& entry : card.surfaces) {
      wlr_surface* surface = entry->surface;
      if (entry->buffer == nullptr) {
        continue;
      }
      // A client retires a subsurface with a null buffer, or by destroying its subsurface role. Either drops the
      // surface out of the commit walk; skipping its mirror would leave the last frame at the zoom it was laid out at.
      // A destroyed role emits no commit, so `mapped` catches it before the next walk does.
      if (!entry->inTree || !surface->mapped || surface->current.width <= 0 || surface->current.height <= 0) {
        wlr_scene_node_set_enabled(&entry->buffer->node, false);
        continue;
      }
      const float surfaceOpacity =
          entry->sourceBuffer != nullptr ? entry->sourceBuffer->opacity * cardOpacity : cardOpacity;
      wlr_scene_buffer_set_opacity(entry->buffer, surfaceOpacity);
      const pixman_region32_t* shape = entry->view->xwayland() ? m_server->xwayland()->surfaceShape(surface) : nullptr;
      if (!entry->isRoot) {
        // Card-local, edges snapped like the card's own, so a subsurface moves and scales with the card's edges.
        const ov::PixelBox sub = ov::snapEdges({
            (entry->sx - geometry.x) * fx,
            (entry->sy - geometry.y) * fy,
            surface->current.width * fx,
            surface->current.height * fy,
        });
        wlr_scene_node_set_enabled(&entry->buffer->node, true);
        wlr_scene_node_set_position(&entry->buffer->node, sub.x, sub.y);
        wlr_scene_buffer_set_dest_size(entry->buffer, sub.width, sub.height);
        applySurfaceShape(entry->buffer, surface, shape);
        roundToCardBox(entry->buffer);
        continue;
      }
      // Root surface: crop to the committed window geometry so CSD shadow padding never leaks into the thumbnail, then
      // scale the visible part of that region onto the card.
      wlr_fbox base{};
      wlr_surface_get_buffer_source_box(surface, &base);
      int width = 0;
      int height = 0;
      viewSurfaceSize(entry->sourceBuffer, surface, &width, &height);
      const double bx = base.width / width;
      const double by = base.height / height;
      wlr_fbox src{base.x + geometry.x * bx, base.y + geometry.y * by, geometry.width * bx, geometry.height * by};
      if (src.x < base.x) {
        src.width -= base.x - src.x;
        src.x = base.x;
      }
      if (src.y < base.y) {
        src.height -= base.y - src.y;
        src.y = base.y;
      }
      src.width = std::min(src.width, base.x + base.width - src.x);
      src.height = std::min(src.height, base.y + base.height - src.y);
      if (src.width <= 0 || src.height <= 0) {
        wlr_scene_node_set_enabled(&entry->buffer->node, false);
        continue;
      }
      wlr_scene_node_set_enabled(&entry->buffer->node, true);
      wlr_scene_node_set_position(&entry->buffer->node, 0, 0);
      wlr_scene_buffer_set_source_box(entry->buffer, &src);
      wlr_scene_buffer_set_dest_size(entry->buffer, contentW, contentH);
      applySurfaceShape(entry->buffer, surface, shape);
      roundToCardBox(entry->buffer);
      const wlr_box blurBox{0, 0, contentW, contentH};
      card.blur.setAlpha(1.0F);
      card.blur.setMaterial(configWindowGlass());
      // Only the overview background lies under a card, so its glass samples the cached blur whatever the window's
      // rule says; a thumb copy is too small to show blur and draws none.
      SurfaceBlurOptions blurOptions = view->blurOptions();
      blurOptions.optimized = true;
      if (card.role == CardRole::ThumbCopy) {
        blurOptions.enabled = false;
      }
      card.blur.update(
          card.tree, surface, blurBox, geometry, contentCorners, nullptr, blurOptions, entry->buffer->opacity,
          entry->buffer
      );
      blurUpdated = true;
    }
    if (!blurUpdated) {
      card.blur.hide();
    }
  }

  void Overview::layoutCardChrome(Card& card, const wlr_box& world, int borderInset, double zoom, float alpha) const {
    const Output* output = card.owner != nullptr ? card.owner->output : nullptr;
    ViewChromeGeometry geometry{
        .contentX = 0,
        .contentY = 0,
        .contentWidth = world.width,
        .contentHeight = world.height,
        .borderInset = borderInset,
        .cornerRadius = card.view->maximizedToEdges() ? 0 : card.view->decorationCornerRadius(),
        .titleBarHeight = card.view->titleBarHeight(),
        .scale = output != nullptr ? output->wlr()->scale : 1.0F,
        .suppressed = card.view->currentFullscreen(),
        .zoom = zoom,
    };
    const auto update = [&](const ViewChromeAttachment* live, std::unique_ptr<ViewChromeAttachment>& preview) {
      if (live == nullptr) {
        preview.reset();
        return;
      }
      if (preview == nullptr)
        preview = live->makePreview(card.tree);
      if (preview == nullptr)
        return;
      live->syncPreview(*preview);
      preview->setAlpha(alpha);
      preview->layout(geometry);
    };
    update(card.view->ssdFrame(), card.frame);
    geometry.suppressed = geometry.suppressed || card.view->maximizedToEdges();
    update(card.view->chromeAttachment(), card.chrome);
  }

  void Overview::layoutCardShadow(Card& card, float alpha) const {
    const double zoom = card.scale;
    const wlr_scene_shadow* source = card.view->shadowNode();
    if (alpha <= 0.0F || source == nullptr || !source->node.enabled || &card == dragCard()) {
      return;
    }
    wlr_scene_tree* parent = card.view->shadowPooled() ? card.owner->tileShadows : card.tree;
    if (parent == nullptr) {
      return;
    }
    if (card.shadowTree == nullptr) {
      card.shadowTree = wlr_scene_tree_create(parent);
      if (card.shadowTree == nullptr) {
        return;
      }
      card.shadow = wlr_scene_shadow_create(card.shadowTree, 0, 0, 0, 0.0F, source->color);
      if (card.shadow == nullptr) {
        destroyCardShadow(card);
        return;
      }
    } else if (card.shadowTree->node.parent != parent) {
      wlr_scene_node_reparent(&card.shadowTree->node, parent);
    }
    // Under the card the tree follows the card origin and sits below its content, the way the real container sits
    // under the frame; in the pool it carries the card origin itself.
    if (parent == card.tree) {
      wlr_scene_node_lower_to_bottom(&card.shadowTree->node);
      wlr_scene_node_set_position(&card.shadowTree->node, 0, 0);
    } else {
      wlr_scene_node_set_position(&card.shadowTree->node, card.box.x, card.box.y);
    }
    wlr_scene_node_set_enabled(&card.shadowTree->node, true);

    const auto scaled = [zoom](int value) { return static_cast<int>(std::lround(value * zoom)); };
    // Signed: a negative radius is a concave flare (fx_corner_radii).
    const auto scaledCorner = [zoom](int16_t value) { return static_cast<int16_t>(std::lround(value * zoom)); };
    // Edges map onto the card's snapped content box, so the hole meets the content exactly: sizes rounded on their
    // own leave a 1 px unshadowed seam on some frames.
    const wlr_box& world = card.view->presentedBox();
    const double sx = world.width > 0 ? static_cast<double>(card.box.width) / world.width : zoom;
    const double sy = world.height > 0 ? static_cast<double>(card.box.height) / world.height : zoom;
    const auto edgeX = [sx](int value) { return static_cast<int>(std::lround(value * sx)); };
    const auto edgeY = [sy](int value) { return static_cast<int>(std::lround(value * sy)); };
    const int left = edgeX(source->node.x);
    const int top = edgeY(source->node.y);
    wlr_scene_node_set_position(&card.shadow->node, left, top);
    wlr_scene_shadow_set_size(
        card.shadow, std::max(0, edgeX(source->node.x + source->width) - left),
        std::max(0, edgeY(source->node.y + source->height) - top)
    );
    wlr_scene_shadow_set_blur_sigma(card.shadow, static_cast<float>(source->blur_sigma * zoom));
    wlr_scene_shadow_set_corner_radius(card.shadow, scaled(source->corner_radius));
    const float color[4] = {source->color[0], source->color[1], source->color[2], source->color[3] * alpha};
    wlr_scene_shadow_set_color(card.shadow, color);
    const clipped_region& hole = source->clipped_region;
    wlr_scene_shadow_set_clipped_region(
        card.shadow,
        clipped_region{
            .area =
                {
                    .x = edgeX(source->node.x + hole.area.x) - left,
                    .y = edgeY(source->node.y + hole.area.y) - top,
                    .width = edgeX(source->node.x + hole.area.x + hole.area.width)
                        - edgeX(source->node.x + hole.area.x),
                    .height = edgeY(source->node.y + hole.area.y + hole.area.height)
                        - edgeY(source->node.y + hole.area.y),
                },
            .corners = {
                .top_left = scaledCorner(hole.corners.top_left),
                .top_right = scaledCorner(hole.corners.top_right),
                .bottom_right = scaledCorner(hole.corners.bottom_right),
                .bottom_left = scaledCorner(hole.corners.bottom_left),
            },
        }
    );
  }

  void Overview::destroyCardShadow(Card& card) {
    if (card.shadowTree != nullptr) {
      wlr_scene_node_destroy(&card.shadowTree->node);
    }
    card.shadowTree = nullptr;
    card.shadow = nullptr;
  }

  void Overview::layoutOutput(OutputState& state) {
    ANDROMEDA_ZONE("Overview::layoutOutput");
    if (!refreshOutputBoxes(state)) {
      return;
    }
    const wlr_box& outputBox = state.outputBox;

    // Cards moving in from other workspaces start past the output. One clip on this output's overview tree contains
    // every card, ring, and workspace background, so none needs to trim its own geometry. The dragged card is
    // reparented out to the unclipped overview root, which is what lets it span outputs.
    wlr_scene_tree_set_clip(state.tree, &outputBox);
    if (state.backgroundBlur.has_value() && m_progress > 0.001) {
      // Blur alpha is normalized to [0, 1]; clamp an overshooting progress. Full strength (clarity 0) samples the
      // cached wallpaper blur (one re-render per change, not one per frame); a strength below 1 re-blurs the whole
      // output live on every frame of the open. The open fades the blur in by alpha.
      state.backgroundBlur->setAlpha(static_cast<float>(std::clamp(m_progress, 0.0, 1.0)));
      state.backgroundBlur->update(
          m_server->overviewBlurTree(), nullptr, outputBox, outputBox, corner_radii_none(), nullptr,
          {.enabled = true, .optimized = config().appearance.blur.optimized, .always = true}
      );
    } else if (state.backgroundBlur.has_value()) {
      state.backgroundBlur->hide();
    }

    wlr_scene_node_set_position(&state.backgroundTint->node, outputBox.x, outputBox.y);
    wlr_scene_rect_set_size(state.backgroundTint, outputBox.width, outputBox.height);
    const std::array<float, 4> backgroundTint = tint(config().colors.overview.backgroundTint, m_progress);
    // The background blur draws the tint in its own pass, the same result as the rect over it without a second
    // full-output blend. While it fades in from its sharp source, that holds as long as no bottom layer shows above it.
    const bool tintable = state.backgroundBlur.has_value() && state.backgroundBlur->active() && backgroundTint[3] > 0.001F;
    const bool fadeTint = tintable
        && config().appearance.blur.optimized
        && (!m_server->shellLayerTree(ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM)->node.enabled
            || countBuffers(state.output->layerTree(ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM)) == 0);
    const bool blurTint = tintable && (m_progress >= 1.0 || fadeTint);
    if (state.backgroundBlur.has_value()) {
      const float amount = blurTint ? backgroundTint[3] : 0.0F;
      const std::array<float, 3> straight =
          blurTint ? std::array<float, 3>{backgroundTint[0] / amount, backgroundTint[1] / amount, backgroundTint[2] / amount}
                   : std::array<float, 3>{};
      state.backgroundBlur->setTintMix(straight, amount);
    }
    // A fully transparent tint leaves the wallpaper untouched.
    wlr_scene_node_set_enabled(&state.backgroundTint->node, backgroundTint[3] > 0.001F && !blurTint);
    wlr_scene_rect_set_color(state.backgroundTint, backgroundTint.data());

    refreshDesktop(state);

    // Placement is pure and settled; only its input changing re-runs it. An open or close in flight interpolates.
    const ov::Style style = configuredStyle();
    const double row = state.rowScroll.current();
    ov::Input input = placementInput(state, false);
    std::vector<uintptr_t>& structure = state.nextStructure;
    structure.assign({static_cast<uintptr_t>(style), input.workspaces.size()});
    for (const ov::Workspace& workspace : input.workspaces) {
      structure.push_back(static_cast<uintptr_t>(workspace.arrangement));
      structure.push_back(workspace.members.size());
      for (const ov::Member& member : workspace.members) {
        structure.push_back(reinterpret_cast<uintptr_t>(state.cards[member.id]->view));
        structure.push_back(member.minimized ? 1U : 0U);
      }
    }
    if (const WorkspaceGroup* group = state.output->workspaceGroup()) {
      for (size_t index = 0; index < group->workspaceCount(); ++index) {
        structure.push_back(reinterpret_cast<uintptr_t>(group->workspaceAt(index)));
      }
    }
    const bool reflowed = state.placed && (structure != state.structure || state.rowJumped);
    state.rowJumped = false;
    state.structure.swap(structure);
    if (!state.placed || style != state.placedStyle || row != state.placedRow || input != state.placedInput) {
      state.placement = ov::placeAt(style, input, row);
      state.placedInput = std::move(input);
      state.placedStyle = style;
      state.placedRow = row;
      state.placed = true;
      state.placeOf.assign(state.cards.size(), nullptr);
      for (const ov::WindowPlace& place : state.placement.windows) {
        if (place.id < state.placeOf.size()) {
          state.placeOf[place.id] = &place;
        }
      }
      state.thumbOf.assign(state.cards.size(), nullptr);
      for (const ov::WindowPlace& place : state.placement.currentThumb) {
        if (place.id < state.thumbOf.size()) {
          state.thumbOf[place.id] = &place;
        }
      }
    }
    const ov::Input& in = state.placedInput;
    const ov::Placement& placement = state.placement;
    const double p = m_progress;
    // Reduced motion (rulebook 7.8) moves nothing: workspace cards change place halfway, while the windows are faded out.
    const double travel = config().appearance.reduceMotion ? (p < 0.5 ? 0.0 : 1.0) : p;
    const ov::Rect output{0.0, 0.0, in.width, in.height};
    // Every piece goes straight from its desktop rect to its slot on the whole spring, overshoot included. Below 0
    // windows would grow past their desktop size.
    const double zoom = config().appearance.reduceMotion ? travel : std::max(p, 0.0);
    const auto travelRect = [zoom](const ov::Rect& start, const ov::Rect& slot) { return ov::lerpRect(start, slot, zoom); };
    const auto thumbAlpha = static_cast<float>(std::clamp(m_appearOpacity.current(), 0.0, 1.0));
    const auto toLayout = [&outputBox](ov::Rect rect) {
      rect.x += outputBox.x;
      rect.y += outputBox.y;
      return rect;
    };
    const auto toLocal = [&outputBox](ov::Rect rect) {
      rect.x -= outputBox.x;
      rect.y -= outputBox.y;
      return rect;
    };
    const WorkspaceGroup* group = state.output->workspaceGroup();

    state.cardsHorizontal = style != ov::Style::Strip || in.horizontal;
    state.workspaceViews.assign(state.workspaceBackgrounds.size(), WorkspaceView{});
    for (const ov::Card& card : placement.cards) {
      if (card.workspace >= state.workspaceViews.size()) {
        continue;
      }
      const Workspace* workspace = group != nullptr ? group->workspaceAt(card.workspace) : nullptr;
      const ov::Rect placed =
          workspace != nullptr ? toLocal(reflowRect(workspace, toLayout(card.rect), reflowed)) : card.rect;
      // A thumb stays in its slot and grows on the appear scale's spring, as other desktops' windows do, while it fades:
      // riding the zoom made it slide in from the frame.
      ov::Rect rect = card.kind == ov::CardKind::Thumb ? placed : travelRect(ov::cardDeskRect(in, card, row), placed);
      if (card.kind == ov::CardKind::Thumb) {
        const double grown = (m_appearScale.current() - kAppearScaleIn) / (1.0 - kAppearScaleIn);
        const double s = Tokens::motionOverviewPieceScale + (1.0 - Tokens::motionOverviewPieceScale) * grown;
        rect = {rect.x + rect.w * (1.0 - s) / 2, rect.y + rect.h * (1.0 - s) / 2, rect.w * s, rect.h * s};
      }
      const wlr_box box = layoutBox(rect, outputBox);
      state.workspaceViews[card.workspace] = {
          .box = box,
          .fit =
              {static_cast<double>(box.width) / outputBox.width, static_cast<double>(box.x),
               static_cast<double>(box.y)},
          .kind = card.kind,
          .shown = true,
          .opacity = workspace != nullptr ? reflowOpacity(workspace) : 1.0F,
          .placed = placed,
          .rect = rect,
      };
    }
    state.mainBox.reset();
    if (placement.main.has_value() && placement.mainFit.has_value() && !state.workspaceViews.empty()) {
      const ov::Fit& fit = *placement.mainFit;
      state.mainWorkspace =
          static_cast<size_t>(std::clamp(std::lround(row), 0L, static_cast<long>(state.workspaceViews.size() - 1)));
      state.mainBox = layoutBox(ov::lerpRect(output, *placement.main, zoom), outputBox);
      state.mainFit = {1.0 + (fit.s - 1.0) * zoom, outputBox.x + fit.ox * zoom, outputBox.y + fit.oy * zoom};
    }

    const auto& workspaceFill = config().colors.overview.workspaceBackground;
    // Thumbs appear in their slots with the windows of other desktops; the shown workspace grows with the zoom.
    const std::array<float, 4> backgroundColor = tint(workspaceFill, m_progress);
    const std::array<float, 4> thumbColor = tint(workspaceFill, thumbAlpha);
    // Lays out one mirrored background: its fill, then its desktop mirrors scaled by `z` into `full`.
    const auto layoutMirrored = [&](const WorkspaceBackground& background, const wlr_box& full, double z,
                                    int backgroundRadius, const std::array<float, 4>& fill, float mirrorAlpha,
                                    bool fillOver) {
      const auto spansOutput = [&outputBox](const wlr_box& box) {
        return box.x <= 0
            && box.y <= 0
            && box.x + box.width >= outputBox.width
            && box.y + box.height >= outputBox.height;
      };
      // A fill over the mirrors draws in their own pass as a tint when an opaque mirror covers the whole card.
      const bool folded =
          fillOver && mirrorAlpha >= 1.0F && std::ranges::any_of(background.mirrors, [&](const auto& mirror) {
            wlr_box box{};
            if (!desktopSourceBox(*mirror->source, box) || !spansOutput(box)) {
              return false;
            }
            pixman_box32_t whole{0, 0, box.width, box.height};
            return pixman_region32_contains_rectangle(&mirror->source->surface->opaque_region, &whole)
                == PIXMAN_REGION_IN;
          });
      const std::array<float, 4> mirrorTint = folded ? fill : std::array<float, 4>{};
      layoutWorkspaceBackground(background.fill, full, backgroundRadius, folded ? std::array<float, 4>{} : fill);
      if (background.mirrors.empty()) {
        return;
      }
      // The card is the viewport onto the mirrored stack: a partially anchored surface must not reach past it.
      wlr_scene_tree_set_clip(background.tree, &full);
      for (const auto& mirror : background.mirrors) {
        wlr_box box{};
        const bool visible = desktopSourceBox(*mirror->source, box);
        wlr_scene_node_set_enabled(&mirror->buffer->node, visible && mirrorAlpha > 0.0F);
        if (!visible) {
          continue;
        }
        wlr_scene_buffer_set_opacity(mirror->buffer, mirrorAlpha);
        wlr_scene_buffer_set_tint(mirror->buffer, mirrorTint.data());
        wlr_scene_node_set_position(
            &mirror->buffer->node, full.x + static_cast<int>(std::lround(box.x * z)),
            full.y + static_cast<int>(std::lround(box.y * z))
        );
        const int destWidth = std::max(1, static_cast<int>(std::lround(box.width * z)));
        const int destHeight = std::max(1, static_cast<int>(std::lround(box.height * z)));
        wlr_scene_buffer_set_dest_size(mirror->buffer, destWidth, destHeight);
        // Lets the scene skip the background beneath a wallpaper that declares itself opaque.
        setScaledOpaqueRegion(
            mirror->buffer, mirror->source->surface->opaque_region, box.width, box.height, destWidth, destHeight
        );
        if (const DskSurfaceManager* dsk = m_server->dskSurfaces()) {
          if (const DskSurfaceManager::State* dskState = dsk->state(mirror->source->surface)) {
            setScaledContentRegion(
                mirror->buffer, dskState->contentRegion, box.width, box.height, destWidth, destHeight
            );
          }
        }
        // Only a surface spanning the whole output takes the card's rounding; a smaller one keeps its own edges.
        wlr_scene_buffer_set_corner_radii(mirror->buffer, corner_radii_all(spansOutput(box) ? backgroundRadius : 0));
      }
    };
    for (size_t index = 0; index < state.workspaceBackgrounds.size(); ++index) {
      const WorkspaceBackground& background = state.workspaceBackgrounds[index];
      const WorkspaceView& view = state.workspaceViews[index];
      wlr_scene_node_set_enabled(&background.tree->node, view.shown);
      if (!view.shown) {
        continue;
      }
      const bool thumb = view.kind == ov::CardKind::Thumb;
      const double z = view.fit.s;
      const wlr_box& full = view.box;
      // A thumb keeps the desktop's corners scaled; a frame rounds from square to r20 as the desktop zooms out.
      const int backgroundRadius =
          static_cast<int>(std::lround(thumb ? config().appearance.cornerRadius * z : Tokens::radiusR20 * p));
      const float alpha = (thumb ? thumbAlpha : static_cast<float>(m_progress)) * view.opacity;
      layoutMirrored(
          background, full, z, backgroundRadius,
          view.opacity < 1.0F ? tint(workspaceFill, alpha) : (thumb ? thumbColor : backgroundColor),
          (thumb ? thumbAlpha : 1.0F) * view.opacity, false
      );
    }
    // Spaces: the current workspace's frame grows from the screen with the zoom; its wallpaper stays opaque so the
    // desktop never shows through, and the workspace fill fades in over it by progress.
    const bool framed = state.placedStyle == ov::Style::Spaces && state.mainBox.has_value() && state.frame.tree != nullptr;
    if (state.frame.tree != nullptr) {
      wlr_scene_node_set_enabled(&state.frame.tree->node, framed);
    }
    if (state.frameShadow != nullptr) {
      wlr_scene_node_set_enabled(&state.frameShadow->node, framed);
    }
    if (framed) {
      const wlr_box& box = *state.mainBox;
      const int radius = static_cast<int>(std::lround(Tokens::radiusR20 * p));
      // Mirrors are rebuilt on top of the fill; the frame's fill and rim lie over its wallpaper.
      wlr_scene_node_raise_to_top(&state.frame.fill->node);
      // The frame's wallpaper fills the frame: a Scrolling strip's mainFit is smaller than the frame's own scale.
      const double frameScale = static_cast<double>(box.width) / std::max(1, state.outputBox.width);
      layoutMirrored(state.frame, box, frameScale, radius, backgroundColor, 1.0F, true);
      const bool light = config().appearance.scheme == ColorScheme::Light;
      const auto& scheme = light ? Tokens::schemeLight : Tokens::schemeDark;
      const auto color = [](std::uint32_t value) {
        const auto channel = [value](int shift) { return static_cast<float>((value >> shift) & 0xFFU) / 255.0F; };
        return std::array<float, 4>{channel(24), channel(16), channel(8), channel(0)};
      };
      const float fade = static_cast<float>(std::clamp(m_progress, 0.0, 1.0));
      if (state.frameShadow != nullptr) {
        // E2 (0 6 16): the nearest elevation to the mockup's 0 8 24 .28; the hole keeps it out of the translucent frame.
        const auto& e2 = Tokens::elevationE2;
        const int sigma = static_cast<int>(std::lround(e2.keyBlur / 2.0F));
        const int offsetY = static_cast<int>(std::lround(e2.keyY));
        wlr_scene_node_set_position(&state.frameShadow->node, box.x - sigma, box.y - sigma + offsetY);
        wlr_scene_shadow_set_size(state.frameShadow, box.width + 2 * sigma, box.height + 2 * sigma);
        wlr_scene_shadow_set_blur_sigma(state.frameShadow, static_cast<float>(sigma));
        wlr_scene_shadow_set_corner_radius(state.frameShadow, radius);
        std::array<float, 4> shadow{};
        premultiplied(shadow.data(), color(scheme.shadow | 0xFFU), (light ? e2.keyLight : e2.keyDark) * fade);
        wlr_scene_shadow_set_color(state.frameShadow, shadow.data());
        wlr_scene_shadow_set_clipped_region(
            state.frameShadow,
            clipped_region{.area = {sigma, sigma - offsetY, box.width, box.height}, .corners = corner_radii_all(radius)}
        );
      }
      if (state.frameRim != nullptr) {
        wlr_scene_node_raise_to_top(&state.frameRim->node.parent->node);
        wlr_scene_node_set_position(&state.frameRim->node.parent->node, box.x, box.y);
        const int rim = std::max(1, static_cast<int>(std::lround(Tokens::lensRimWidth)));
        applyBorderGeometry(state.frameRim, makeBorderRing(box.width, box.height, radius, rim, 0), rim, 0);
        std::array<float, 4> edge{};
        premultiplied(edge.data(), color(scheme.edge), fade);
        wlr_scene_border_set_colors(state.frameRim, edge.data(), edge.data());
      }
    }

    const View* liveTarget = liveTargetView();
    // A window in a thumb rides it: the thumb's zoom travel and piece scale map the window's settled slot.
    const auto inThumb = [&](const ov::WindowPlace& place, const ov::Rect& placed) -> std::optional<ov::Rect> {
      if (place.workspace >= state.workspaceViews.size()) {
        return std::nullopt;
      }
      const WorkspaceView& thumb = state.workspaceViews[place.workspace];
      if (!thumb.shown || thumb.kind != ov::CardKind::Thumb || thumb.placed.w <= 0 || thumb.placed.h <= 0) {
        return std::nullopt;
      }
      const double sx = thumb.rect.w / thumb.placed.w;
      const double sy = thumb.rect.h / thumb.placed.h;
      return ov::Rect{thumb.rect.x + (placed.x - thumb.placed.x) * sx, thumb.rect.y + (placed.y - thumb.placed.y) * sy,
                      placed.w * sx, placed.h * sy};
    };
    // A ThumbCopy appears in its thumb as the other thumbs' windows do; it follows its window's search filter.
    const auto layoutThumbCopy = [&](Card& card, const ov::WindowPlace* place) {
      if (place != nullptr && card.thumb == nullptr) {
        card.thumb = makeCard(state, card.view, card.workspaceIndex, CardRole::ThumbCopy);
      }
      if (card.thumb == nullptr) {
        return;
      }
      Card& copy = *card.thumb;
      copy.opacity = 0.0F;
      if (place != nullptr) {
        const wlr_box& world = card.view->presentedBox();
        const ov::Rect placed = toLocal(reflowRect(card.view, toLayout(place->rect), reflowed));
        const ov::Rect rect = inThumb(*place, placed).value_or(placed);
        copy.box = layoutBox(rect, outputBox);
        copy.scale = world.width > 0 ? rect.w / world.width : 1.0;
        copy.opacity = static_cast<float>(
            ov::windowOpacity(*place, card.view->minimized(), 1.0) * std::clamp(m_appearOpacity.current(), 0.0, 1.0) *
            card.filter.current()
        );
        copy.cut = place->clip;
      }
      layoutCard(copy, liveTarget);
    };
    for (size_t k = 0; k < state.cards.size(); ++k) {
      Card& card = *state.cards[k];
      layoutThumbCopy(card, k < state.thumbOf.size() ? state.thumbOf[k] : nullptr);
      const ov::WindowPlace* place = k < state.placeOf.size() ? state.placeOf[k] : nullptr;
      if (place == nullptr) {
        card.opacity = 0.0F;
        layoutCard(card, liveTarget);
        continue;
      }
      const wlr_box& world = card.view->presentedBox();
      const ov::Rect placed = toLocal(reflowRect(card.view, toLayout(place->rect), reflowed));
      ov::Rect rect;
      double opacity = 1.0;
      if (growsFromDesk(state, card)) {
        const ov::Rect desk = ov::deskRect(in, ov::edgeRect(localRect(world, outputBox), in.width, in.height), place->workspace, row);
        opacity = ov::windowOpacity(*place, card.view->minimized(), p);
        if (config().appearance.reduceMotion) {
          // Reduced motion (rulebook 7.8): out at the window over the first half, in at the slot over the second.
          rect = p < 0.5 ? desk : placed;
          opacity *= std::clamp(std::abs(1.0 - 2.0 * p), 0.0, 1.0);
        } else {
          rect = travelRect(desk, placed);
        }
      } else if (const auto thumbRect = inThumb(*place, placed)) {
        rect = *thumbRect;
        opacity = ov::windowOpacity(*place, card.view->minimized(), 1.0) * std::clamp(m_appearOpacity.current(), 0.0, 1.0);
      } else {
        const double s = m_appearScale.current();
        rect = {placed.x + placed.w * (1.0 - s) / 2, placed.y + placed.h * (1.0 - s) / 2, placed.w * s, placed.h * s};
        opacity = ov::windowOpacity(*place, card.view->minimized(), 1.0) * std::clamp(m_appearOpacity.current(), 0.0, 1.0);
      }
      if (&card != dragCard()) {
        card.box = layoutBox(rect, outputBox);
        card.scale = world.width > 0 ? rect.w / world.width : 1.0;
      } else {
        // The pointer owns a dragged card, its scale and its origin.
        placeDragCard(card);
      }
      const double filter = m_filter.empty() || cardMatches(card) ? 1.0 : kFilterMissOpacity;
      if (filter != card.filter.target()) {
        drive(card.filter, filter, filter > card.filter.current() ? Tokens::springFade : Tokens::springFadeOut);
      }
      card.opacity =
          static_cast<float>(opacity * card.filter.current()) * (&card != dragCard() ? reflowOpacity(card.view) : 1.0F);
      card.cut = place->clip;
      layoutCard(card, liveTarget);
    }
    layoutChrome(state, liveTarget);
  }

  void Overview::layoutChrome(OutputState& state, const View* liveTarget) {
    if (state.chrome == nullptr) {
      return;
    }
    const WorkspaceGroup* group = state.output->workspaceGroup();
    m_chromeWorkspaces.clear();
    // Every name is built before any view into the list is taken, so no view outlives a reallocation.
    m_chromeNames.resize(state.workspaceViews.size());
    for (size_t index = 0; index < state.workspaceViews.size(); ++index) {
      const Workspace* workspace = group != nullptr ? group->workspaceAt(index) : nullptr;
      m_chromeNames[index].clear();
      if (workspace != nullptr) {
        // An unnamed workspace reads "Desktop 2", not a bare "2" (macOS, Windows).
        const bool named = workspace->named() || workspace->displayName() != workspace->name();
        m_chromeNames[index] = named ? workspace->displayName() : std::format("Desktop {}", workspace->displayName());
      }
    }
    std::vector<bool> holdsWindows(state.workspaceViews.size(), false);
    for (const auto& card : state.cards) {
      if (card->view != nullptr && card->workspaceIndex < holdsWindows.size()) {
        holdsWindows[card->workspaceIndex] = true;
      }
    }
    for (size_t index = 0; index < state.workspaceViews.size(); ++index) {
      const WorkspaceView& view = state.workspaceViews[index];
      const Workspace* workspace = group != nullptr ? group->workspaceAt(index) : nullptr;
      m_chromeWorkspaces.push_back({
          .name = m_chromeNames[index],
          .arrangement = workspace != nullptr ? arrangementLabel(workspace->layoutMode()) : std::string_view(),
          .card = view.box,
          .kind = view.kind,
          .shown = view.shown && workspace != nullptr,
          .empty = !holdsWindows[index],
      });
    }
    m_chromeWindows.clear();
    const auto addWindow = [&](const Card& card) {
      if (card.opacity <= 0.0F || card.view == nullptr) {
        return;
      }
      const char* title = card.view->title();
      wlr_box visible = card.box;
      if (card.clip.has_value()) {
        if (card.clip->width <= 0 || card.clip->height <= 0) {
          return;
        }
        visible = {card.box.x + card.clip->x, card.box.y + card.clip->y, card.clip->width, card.clip->height};
      }
      std::optional<wlr_box> within;
      const bool inMain = state.mainBox.has_value() && card.workspaceIndex == state.mainWorkspace;
      if (!inMain && card.workspaceIndex < state.workspaceViews.size()) {
        const WorkspaceView& holder = state.workspaceViews[card.workspaceIndex];
        // A thumb is a glance at the workspace; only full-size workspaces label their windows.
        if (holder.kind == ov::CardKind::Thumb) {
          return;
        }
        within = holder.box;
      }
      const char* appId = card.view->appId();
      const int iconPx = static_cast<int>(std::ceil(static_cast<double>(state.output->wlr()->scale))) * 18;
      m_chromeWindows.push_back({
          .key = card.view,
          .title = title != nullptr ? std::string_view(title) : std::string_view(),
          .icon = appId != nullptr ? m_appIcons->get(appId, std::max(18, iconPx)) : nullptr,
          .box = visible,
          .within = within,
          .opacity = card.opacity,
          .selected = card.view == liveTarget,
          .dragged = &card == dragCard(),
      });
    };
    // Capsules stack the way their cards do: walk the output tree bottom to top. A dragged card lives outside it.
    wlr_scene_node* node = nullptr;
    wl_list_for_each(node, &state.tree->children, link) {
      for (const auto& card : state.cards) {
        if (card->tree != nullptr && &card->tree->node == node) {
          addWindow(*card);
          break;
        }
      }
    }
    if (dragCard() != nullptr && dragCard()->owner == &state) {
      addWindow(*dragCard());
    }
    std::optional<ov::ChromeMenu> menu;
    if (m_menu.has_value() && m_menu->output == state.output && group != nullptr) {
      const Workspace* workspace = group->workspaceAt(m_menu->workspace);
      const auto row = workspace != nullptr ? std::ranges::find(kMenuModes, workspace->layoutMode()) : kMenuModes.end();
      menu = ov::ChromeMenu{
          .workspace = m_menu->workspace,
          .hovered = m_menu->hovered,
          .current = row != kMenuModes.end() ? static_cast<int>(row - kMenuModes.begin()) : -1,
      };
    }
    std::optional<ov::ChromeWindowMenu> windowMenu;
    if (m_windowMenu.has_value() && m_windowMenu->output == state.output) {
      windowMenu = ov::ChromeWindowMenu{
          .x = m_windowMenu->x,
          .y = m_windowMenu->y,
          .rows = m_windowMenu->rows,
          .hovered = m_windowMenu->hovered,
          .dangerFrom = static_cast<int>(m_windowMenu->rows.size()) - 1,
      };
    }
    const size_t current = m_chromeWorkspaces.empty()
        ? 0
        : static_cast<size_t>(
              std::clamp(std::lround(state.rowScroll.current()), 0L, static_cast<long>(m_chromeWorkspaces.size() - 1))
          );
    state.chrome->update({
        .style = state.placedStyle,
        .progress = m_progress,
        .thumbAlpha = static_cast<float>(std::clamp(m_appearOpacity.current(), 0.0, 1.0)),
        .filterAlpha = static_cast<float>(std::clamp(m_filterOpacity.current(), 0.0, 1.0)),
        .output = state.outputBox,
        .usable = state.usableBox,
        .scale = std::max(1.0, std::ceil(static_cast<double>(state.output->wlr()->scale))),
        .ui = config().appearance.uiScale,
        .placement = &state.placement,
        .current = current,
        .workspaces = m_chromeWorkspaces,
        .windows = m_chromeWindows,
        .titlesAlways = config().overview.titles == OverviewTitles::Always,
        .filter = m_filter,
        .noMatch = !m_filter.empty() && std::ranges::none_of(state.cards, [&](const auto& card) {
          return card->view != nullptr && card->workspaceIndex == current && cardMatches(*card);
        }),
        .hovered = m_hoverView,
        .closeHovered = m_closeHover,
        .menu = menu,
        .windowMenu = windowMenu,
        .renaming = m_rename.has_value() && group != nullptr && m_rename->workspace.get() == group->workspaceAt(current)
            ? std::optional<std::string_view>(m_rename->text)
            : std::nullopt,
    });
    // Cards rebuilt after the chrome was made land above it.
    state.chrome->raise();
    // Windows travelling to or from the desktop, or into the desktop being switched to, pass in front of the strip,
    // its thumbs and labels, and the windows leaving.
    const bool opening = !openSettled();
    const bool switching = !opening && state.rowScroll.animating();
    if (opening || switching) {
      const auto travels = [&](const Card& card) {
        return opening ? growsFromDesk(state, card)
                       : static_cast<long>(card.workspaceIndex) == std::lround(state.rowScroll.target());
      };
      std::vector<wlr_scene_node*> travelling;
      wlr_scene_node* child = nullptr;
      wl_list_for_each(child, &state.tree->children, link) {
        for (const auto& card : state.cards) {
          if (card->tree != nullptr && &card->tree->node == child && travels(*card) && card.get() != dragCard()) {
            travelling.push_back(child);
            break;
          }
        }
      }
      for (wlr_scene_node* node : travelling) {
        wlr_scene_node_raise_to_top(node);
      }
    }
  }

  bool Overview::tickedRecently() const {
    return m_lastTickMsec > 0.0 && static_cast<double>(m_server->animationClockMsec()) - m_lastTickMsec < 34.0;
  }

  void Overview::drive(AnimatedValue& value, double to, const Tokens::Spring& spring) {
    const auto& animation = config().animation;
    if (!animation.enabled || !animation.overview.enabled) {
      value.snap(to);
    } else {
      value.steerSpring(to, motionSpring(spring));
    }
  }

  ov::Rect Overview::reflowRect(const void* key, const ov::Rect& target, bool began) {
    const auto& animation = config().animation;
    const bool reduced = config().appearance.reduceMotion;
    const auto jumpFrom = [&target](const ov::Rect& last) {
      return std::array<double, 4>{last.x - target.x, last.y - target.y, last.w - target.w, last.h - target.h};
    };
    const auto startFade = [this](const void* fadeKey, const ov::Rect& from) {
      auto it = m_reflows.find(fadeKey);
      Crossfade* fade = it != m_reflows.end() ? std::get_if<Crossfade>(&it->second) : nullptr;
      if (fade == nullptr) {
        fade = &m_reflows[fadeKey].emplace<Crossfade>();
      }
      // A card still fading out keeps fading where it was; one fading in leaves from where it is now drawn.
      if (fade->phase == Crossfade::Phase::In) {
        fade->from = from;
        fade->phase = Crossfade::Phase::Out;
      } else if (fade->opacity.current() >= 1.0) {
        fade->from = from;
      }
      fade->opacity.steerSpring(0.0, motionSpring(Tokens::springFadeOut));
      scheduleFrames();
    };
    if (m_return.has_value() && m_return->view.get() == key) {
      const DropReturn back = *m_return;
      m_return.reset();
      if (back.fade) {
        m_reflows.erase(key);
        startFade(key, back.from);
      } else {
        // The release rect is the glide's start, the release velocity its speed.
        Glide& glide = m_reflows[key].emplace<Glide>();
        glide.spring = back.spring;
        const std::array<double, 4> jump = jumpFrom(back.from);
        const std::array<double, 4> velocity{back.vx, back.vy, 0.0, 0.0};
        for (size_t i = 0; i < jump.size(); ++i) {
          glide.offset[i].snap(jump[i]);
          glide.offset[i].settleSpring(0.0, glide.spring, velocity[i]);
        }
        scheduleFrames();
      }
    } else if (began && m_active && animation.enabled && (reduced || animation.overview.enabled)) {
      // Only a rect that jumped moves; one following its window's own layout spring is already smooth.
      if (const auto shown = m_shownRects.find(key); shown != m_shownRects.end()) {
        const std::array<double, 4> jump = jumpFrom(shown->second);
        if (std::ranges::any_of(jump, [](double d) { return std::abs(d) > 0.5; })) {
          if (reduced) {
            startFade(key, shown->second);
          } else {
            auto it = m_reflows.find(key);
            Glide* glide = it != m_reflows.end() ? std::get_if<Glide>(&it->second) : nullptr;
            const bool fresh = glide == nullptr;
            if (fresh) {
              glide = &m_reflows[key].emplace<Glide>();
              glide->spring = motionSpring(Tokens::springMove);
            }
            for (size_t i = 0; i < jump.size(); ++i) {
              // Keep the speed and the spring a running glide has; its offset now measures from the new place.
              AnimatedValue& offset = glide->offset[i];
              const double velocity = fresh ? 0.0 : offset.velocity();
              offset.snap(jump[i]);
              if (animation.overview.workspaceCurve.easing == Easing::Spring) {
                offset.settleSpring(0.0, glide->spring, velocity);
                if (!fresh && tickedRecently()) {
                  offset.startClockAt(m_lastTickMsec);
                }
              } else {
                offset.retarget(0.0, animation.overview.durationMs, animation.overview.workspaceCurve);
              }
            }
            scheduleFrames();
          }
        }
      }
    }
    ov::Rect shown = target;
    if (const auto motion = m_reflows.find(key); motion != m_reflows.end()) {
      if (const auto* glide = std::get_if<Glide>(&motion->second)) {
        const auto& o = glide->offset;
        shown = {
            target.x + o[0].current(), target.y + o[1].current(), target.w + o[2].current(), target.h + o[3].current()
        };
      } else if (const auto& fade = std::get<Crossfade>(motion->second); fade.phase == Crossfade::Phase::Out) {
        shown = fade.from;
      }
    }
    m_shownRects[key] = shown;
    return shown;
  }

  float Overview::reflowOpacity(const void* key) const {
    const auto motion = m_reflows.find(key);
    const auto* fade = motion != m_reflows.end() ? std::get_if<Crossfade>(&motion->second) : nullptr;
    return fade != nullptr ? static_cast<float>(std::clamp(fade->opacity.current(), 0.0, 1.0)) : 1.0F;
  }

  void Overview::applyPinnedOpacity(float alpha) const {
    // A window unpinned while the overview is up becomes a card, so it returns to full opacity underneath.
    for (const auto& view : m_server->registry().all()) {
      view->setOverviewOpacity(view->pinned() ? alpha : 1.0F);
    }
  }

  void Overview::activateLiveTarget() {
    if (View* target = liveTargetView()) {
      target->placementRoot()->inputDelegate()->applySeatFocus(false, false);
    } else {
      m_server->deactivateViews();
    }
  }

  View* Overview::liveTargetView() const {
    const Workspace* workspace = preferredWorkspace();
    return workspace != nullptr ? workspace->focusedView() : nullptr;
  }

  void Overview::syncCardEffects(Card& card) {
    if (card.role != CardRole::Window) {
      return;
    }
    View* view = card.view;
    Workspace* workspace = view->workspace();
    const BorderEffectGate cardGate{
        .focused = workspace != nullptr && workspace->focusedView() == view && &card != dragCard(),
        .decorated = card.border != nullptr && card.border->node.enabled && card.tree->node.enabled,
        .urgent = view->urgent(),
        .fullscreen = view->scheduledFullscreen(),
    };
    view->syncAnimationEffects(
        card.tree, card.border != nullptr ? &card.border->node : nullptr, &card.surfaceTree->node, &cardGate,
        card.owner->output, static_cast<float>(card.scale)
    );
  }

  void Overview::refreshEffectBindings() {
    if (m_active) {
      syncCardEffects();
    }
  }

  void Overview::syncCardEffects() {
    for (const auto& state : m_outputs) {
      for (const auto& card : state->cards) {
        syncCardEffects(*card);
      }
    }
  }

  std::array<float, 4> Overview::cardBorderColor(const Card& card, const View* liveTarget) const {
    const auto& border = card.view != nullptr ? card.view->borderColors() : config().colors.border;
    const Workspace* workspace = card.view != nullptr ? card.view->workspace() : nullptr;
    if (workspace == nullptr || workspace->focusedView() != card.view || &card == dragCard()) {
      return border.unfocused;
    }
    // No window holds the seat while the overview is up, so exactly one card wears the focused color: the one a focus
    // or close action would act on. Every other row still marks the card it would land on, with a weaker mix.
    if (card.view == liveTarget) {
      return border.focused;
    }
    return mixColor(border.unfocused, border.focused, kLandingTargetBlend);
  }

  void Overview::applyProgress() {
    for (const auto& state : m_outputs) {
      layoutOutput(*state);
    }
    // Pinned windows get no card: the real ones fade over the filmstrip instead of blinking at either end.
    const float chrome = desktopChromeAlpha();
    applyPinnedOpacity(chrome);
    wlr_scene_node_set_enabled(&m_server->pinnedTree()->node, chrome > 0.0F);
    scheduleFrames();
  }

  void Overview::scheduleFrames() const {
    for (const auto& state : m_outputs) {
      state->output->requestFrame();
    }
  }

  // -: open/close

  bool Overview::beginPresentation() {
    ANDROMEDA_ZONE("Overview::beginPresentation");
    if (m_active) {
      return true;
    }
    if (m_server->sessionLocked()) {
      return false;
    }
    // A data-device drag owns wlroots' pointer and keyboard grabs until the initiating button is released. Taking
    // overview input ownership now would hide that release from the drag and leave both grabs active.
    if (m_server->seat()->wlr()->drag != nullptr) {
      kLog.debug("overview open ignored during active client drag");
      return false;
    }
    using Clock = std::chrono::steady_clock;
    const auto phaseStart = Clock::now();
    auto phaseMark = phaseStart;
    std::string phases;
    const auto phase = [&](const char* name) {
      const auto now = Clock::now();
      phases += std::format(" {} {:.1f}", name, std::chrono::duration<double, std::milli>(now - phaseMark).count());
      phaseMark = now;
    };
    m_server->cursor()->resetMode();
    // An in-flight three-finger switch settles on the filmstrip instead of the hidden desktop slide.
    const Gestures::SwitchPick switchPick = m_server->gestures()->pickSwitchForOverview();
    for (const auto& output : m_server->outputs()) {
      WorkspaceGroup* group = output->workspaceGroup();
      if (group == nullptr) {
        continue;
      }
      group->slideFinish();
      // Settle the visual scroll onto the layout scroll so cards at progress 0
      // sit exactly where the real windows are.
      if (Workspace* workspace = group->active()) {
        workspace->arrange(false);
      }
    }
    // Before buildState: reconcileDynamic must not invalidate cards mid-build.
    if (switchPick.group != nullptr) {
      switchPick.group->activate(switchPick.target, false);
    }

    phase("settle");
    buildState();
    phase("build");
    if (m_outputs.empty()) {
      return false;
    }
    m_server->cursor()->resetWheelAccumulation();

    if (ScratchpadManager* scratchpad = m_server->scratchpadManager()) {
      scratchpad->hideAll();
    }

    m_active = true;
    m_closing = false;
    m_focusRing = FocusRing::Hidden;
    m_progress = 0.0;
    m_targetProgress = 0.0;
    m_zoomAnim.snap(0.0);
    m_appearOpacity.snap(0.0);
    m_filterOpacity.snap(0.0);
    m_appearScale.snap(config().appearance.reduceMotion ? 1.0 : kAppearScaleIn);
    m_pendingFocus = nullptr;
    m_pointerOutput = m_server->outputFromWlr(m_server->preferredOutput());
    m_server->notifyOverviewChanged();

    // Initialize every View's canonical presentation box before cards consume it. Hidden workspaces normally skip
    // scene presentation entirely, but while overview is active their hidden nodes carry the same position and size
    // state that would be shown if the workspace were active.
    for (const auto& output : m_server->outputs()) {
      if (WorkspaceGroup* group = output->workspaceGroup()) {
        for (size_t index = 0; index < group->workspaceCount(); ++index) {
          if (Workspace* workspace = group->workspaceAt(index)) {
            const auto arrangeStart = Clock::now();
            workspace->arrange(false);
            const double arrangeMs = std::chrono::duration<double, std::milli>(Clock::now() - arrangeStart).count();
            if (arrangeMs > 2.0) {
              kLog.debug(
                  "overview open: workspace {} arrange {:.1f} ms, {} windows", workspace->index(), arrangeMs,
                  workspace->allViews().size()
              );
            }
          }
        }
      }
    }
    if (switchPick.group != nullptr) {
      if (OutputState* state = stateFor(switchPick.group->output())) {
        // Read the row after activate: reconcileDynamic may have dropped the workspace the swipe left.
        const auto row = static_cast<double>(switchPick.target->index());
        state->rowScroll.snap(row + switchPick.offset);
        animateRow(*state, row, switchPick.velocity);
      }
    }
    classifyEntrance();
    phase("arrange");
    applyProgress();
    phase("layout");

    wlr_scene_node_set_enabled(&m_server->xdgTree()->node, false);
    wlr_scene_node_set_enabled(&m_server->fullscreenTree()->node, false);
    wlr_scene_node_set_enabled(&m_tree->node, true);
    // Bottom-layer surfaces are mirrored into every row, so the real ones step aside the way windows do. The
    // background layer stays: it is the blur source and what shows around the filmstrip.
    if (config().overview.workspaceWallpaper) {
      wlr_scene_node_set_enabled(&m_server->shellLayerTree(ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM)->node, false);
    }

    m_server->clearKeyboardFocus();
    activateLiveTarget();
    m_server->cursor()->clearPointerFocus();
    m_server->cursor()->clearConstraint();

    phase("hide");
    applyProgress();
    phase("layout2");
    if (std::chrono::duration<double, std::milli>(Clock::now() - phaseStart).count() > 4.0) {
      kLog.debug("overview open phases (ms):{}", phases);
    }
    // The wallpaper blur cache holds only the background layer, which the overview leaves as it is: open and close
    // reuse it instead of re-blurring the whole output.
    wlr_scene_node_set_enabled(&m_server->overviewBlurTree()->node, true);
    kLog.debug("overview opened on {} output(s)", m_outputs.size());
    for (const auto& state : m_outputs) {
      if (state->backgroundBlur.has_value()) {
        kLog.debug("overview background blur on {} enabled {}", state->output->wlr()->name, state->backgroundBlur->active());
      }
    }
    return true;
  }

  void Overview::open() {
    ANDROMEDA_ZONE("Overview::open");
    m_server->dismissSnapChrome();
    if (m_active) {
      if (ScratchpadManager* scratchpad = m_server->scratchpadManager()) {
        scratchpad->hideAll();
      }
      m_closing = false;
      m_server->notifyOverviewChanged();
      m_pendingFocus = nullptr;
      if (m_progress < 1.0 || m_targetProgress < 1.0) {
        startAnimation(1.0, false);
      }
      return;
    }
    if (!beginPresentation()) {
      return;
    }
    startAnimation(1.0, false);
  }

  void Overview::toggle() {
    if (m_active && !m_closing) {
      close();
    } else {
      open();
    }
  }

  void Overview::close() { beginClose(nullptr); }

  void Overview::closeToWorkspace(Workspace* workspace, View* focus) {
    if (!m_active || m_closing) {
      return;
    }
    if (workspace != nullptr && workspace->group() != nullptr && workspace->group()->active() != workspace) {
      // No slide: the real trees are hidden, the filmstrip is the transition.
      workspace->group()->activate(workspace, false);
    }
    if (focus != nullptr && focus->mapped()) {
      // Overview focus keeps keyboard input withheld, but it updates the workspace and starts any scrolling-column
      // reveal. Begin it before the closing zoom so both animations receive their first tick together. finishAnimation
      // repeats the focus after teardown to deliver keyboard focus once the real trees own input again.
      m_server->focusView(focus, FocusReason::PointerPress);
      if (focus->workspace() != nullptr && focus->workspace()->layoutConfig().scrolling.centerOnOverviewSelect) {
        focus->workspace()->centerFocusedColumn();
      }
    }
    beginClose(focus);
  }

  void Overview::beginClose(View* focus, std::optional<double> release) {
    if (!m_active || m_closing) {
      return;
    }
    m_fingerProgress.reset();
    m_server->cursor()->resetWheelAccumulation();
    // A close releases any navigation mid-gesture. Input events carry monotonic milliseconds, so the release sample
    // shares their clock and bleeds the speed of fingers that came to rest before the close.
    endNavigation(false, monotonicMsec(), m_navigationSource);
    if (dragCard() != nullptr) {
      endDrag(false);
    }
    endThumbDrag(false, 0, 0);
    hideDropHint();
    m_pointer.release();
    m_pressWorkspace = nullptr;
    m_hoverView = nullptr;
    m_closeHover = false;
    m_menu.reset();
    m_windowMenu.reset();
    finishRename(true);
    clearMiddlePress();
    m_pendingFocus = focus;
    for (const auto& state : m_outputs) {
      const WorkspaceGroup* group = state->output->workspaceGroup();
      if (group != nullptr && group->active() != nullptr) {
        // The zoom closes on `close`, or on `page` after a gesture release (startAnimation).
        animateRow(
            *state, static_cast<double>(group->active()->index()), std::nullopt,
            release.has_value() ? Tokens::springPage : Tokens::springClose
        );
      }
    }
    // Close mirrors the open: what the desktop will show returns to its rect, the rest fades in place. A close before
    // the open settled keeps the open's classes, which the cards are still drawn by.
    if (openSettled()) {
      classifyEntrance();
    }
    startAnimation(0.0, true, release);
  }

  void Overview::forceClose() {
    if (!m_active) {
      return;
    }
    m_zoomAnim.snap(0.0);
    for (const auto& state : m_outputs) {
      state->rowScroll.snap(state->rowScroll.current());
    }
    if (dragCard() != nullptr) {
      endDrag(false);
    }
    m_pendingFocus = nullptr;
    teardown();
    restoreFocus(nullptr);
  }

  void Overview::startAnimation(double target, bool closing, std::optional<double> release) {
    m_closing = closing;
    m_server->notifyOverviewChanged();
    m_targetProgress = target;
    const auto& animation = config().animation;
    const auto& overview = animation.overview;
    if (!animation.enabled || !overview.enabled) {
      m_zoomAnim.snap(target);
      m_progress = target;
      for (const auto& state : m_outputs) {
        state->rowScroll.snap(state->rowScroll.target());
      }
      updateAppear();
      finishAnimation();
      return;
    }
    // Rulebook 7.2: a gesture release settles on `page` with the fingers' speed; any other start opens on `open` or
    // closes on `close` from the speed the zoom already has, so a reversed toggle turns the same motion around (7.1.5).
    // Reduced motion fades, and a release settles on `close` without the fingers' speed (7.8).
    const bool reduced = config().appearance.reduceMotion;
    const Tokens::Spring& spring = release.has_value()
        ? (reduced ? Tokens::springClose : Tokens::springPage)
        : (closing ? (reduced ? Tokens::springFadeOut : Tokens::springClose)
                   : (reduced ? Tokens::springFade : Tokens::springZoom));
    const double releaseVelocity = release.has_value() && !reduced ? *release : 0.0;
    if (m_traceFrames) {
      kLog.debug(
          "overview trace release: {:.3f} -> {:.1f}, velocity {:.2f}/s, spring k {:.0f} z {:.2f}", m_progress, target,
          releaseVelocity, spring.stiffness, spring.damping
      );
    }
    if (!m_zoomAnim.animating()) {
      m_zoomAnim.snap(m_progress);
    }
    if (overview.curve.easing != Easing::Spring) {
      m_zoomAnim.retarget(target, overview.durationMs, overview.curve);
    } else if (release.has_value()) {
      // Physics mode, so the tail can end once it no longer moves a pixel.
      m_zoomAnim.settleSpring(target, motionSpring(spring), releaseVelocity);
      // A release hands the fingers' speed on: a clock that starts on the next tick would hold that frame still.
      // Only a recent tick: after frames stopped (a resting hand), the old clock would jump the spring ahead.
      if (releaseVelocity != 0.0 && tickedRecently()) {
        m_zoomAnim.startClockAt(m_lastTickMsec);
      }
    } else {
      m_zoomAnim.steerSpring(target, motionSpring(spring));
    }
    updateAppear();
    // Animations only tick from an output frame; kick one so an idle desktop starts the zoom.
    scheduleFrames();
  }

  void Overview::updateAppear(double startMsec) {
    const bool shown = m_active && !m_closing && m_progress >= Tokens::motionContentGate;
    const double opacity = shown ? 1.0 : 0.0;
    if (m_appearOpacity.target() == opacity) {
      return;
    }
    // Content follows the shape in, and leaves on the same frame as the close (rulebook 7.4).
    drive(m_appearOpacity, opacity, shown ? Tokens::springFade : Tokens::springFadeOut);
    if (!config().appearance.reduceMotion) {
      drive(m_appearScale, shown ? 1.0 : kAppearScaleOut, shown ? Tokens::springZoom : Tokens::springClose);
    }
    if (startMsec > 0.0) {
      for (AnimatedValue* value : {&m_appearOpacity, &m_appearScale}) {
        if (value->animating()) {
          value->startClockAt(startMsec);
        }
      }
    }
    scheduleFrames();
  }

  bool Overview::growsFromDesk(const OutputState& state, const Card& card) {
    // Strip zooms whole workspace cards out of the screen, so their windows ride along.
    return (state.placedStyle != ov::Style::Spaces && state.placedStyle != ov::Style::TaskView) || card.grows;
  }

  bool Overview::openSettled() const {
    // At rest both classes draw a card in its slot, so classifying again does not show.
    return m_active
        && !m_closing
        && m_progress == 1.0
        && !m_zoomAnim.animating()
        && !m_appearOpacity.animating()
        && !m_appearScale.animating();
  }

  void Overview::classifyEntrance() {
    for (const auto& state : m_outputs) {
      const long row = std::lround(state->rowScroll.target());
      for (const auto& card : state->cards) {
        card->grows = !card->view->minimized() && static_cast<long>(card->workspaceIndex) == row;
      }
    }
  }

  void Overview::animateRow(OutputState& state, double target, std::optional<double> release, const Tokens::Spring& spring) {
    const double releaseVelocity = release.value_or(0.0);
    const auto& animation = config().animation;
    const auto& overview = animation.overview;
    if (!animation.enabled || !overview.enabled) {
      state.rowScroll.snap(target);
      applyProgress();
      return;
    }
    if (config().appearance.reduceMotion) {
      // Reduced motion (rulebook 7.8): a gesture release or cancel settles on `close` without the fingers' speed; any
      // other move jumps the row, and what it moved crossfades from its old place.
      if (release.has_value()) {
        state.rowScroll.settleSpring(target, motionSpring(Tokens::springClose));
        scheduleFrames();
        return;
      }
      state.rowScroll.snap(target);
      state.rowJumped = true;
      applyProgress();
      if (dragCard() != nullptr) {
        retargetDrop(m_dragX, m_dragY);
      }
      return;
    }
    if (releaseVelocity != 0.0) {
      state.rowScroll.settleSpring(target, motionSpring(Tokens::springPage), releaseVelocity);
    } else {
      // A row already sliding keeps its speed toward the new row.
      state.rowScroll.steerSpring(target, motionSpring(spring));
    }
    // Animations only tick from an output frame; kick one so an idle desktop
    // starts both timelines from the same frame.
    scheduleFrames();
  }

  bool Overview::tickEdgePan(double nowMsec) {
    Workspace* workspace = m_edgePan != 0 ? m_edgeWorkspace.get() : nullptr;
    ScrollingLayout* scrolling = workspace != nullptr ? workspace->scrollingLayout() : nullptr;
    if (scrolling == nullptr || (dragCard() == nullptr && !m_thumbDragging)) {
      return false;
    }
    const double elapsed = m_edgePanLastMsec < 0 ? 0.0 : nowMsec - m_edgePanLastMsec;
    m_edgePanLastMsec = nowMsec;
    const double max = scrolling->maxScroll(workspace->scrollViewportExtent());
    const double next = std::clamp(scrolling->scroll() + m_edgePan * kEdgePanPxPerMs * elapsed, 0.0, max);
    if (next != scrolling->scroll()) {
      scrolling->setScroll(next);
      workspace->markArrange(false);
      if (dragCard() != nullptr) {
        retargetDrop(m_dragX, m_dragY);
      }
    }
    // Keep frames coming only while there is room to pan.
    return m_edgePan < 0 ? next > 0.0 : next < max;
  }

  bool Overview::tickAnimations(double nowMsec) {
    if (m_gestureOpenedAt) {
      kLog.debug(
          "overview swipe first frame {:.1f} ms after the open",
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - *m_gestureOpenedAt).count()
      );
      m_gestureOpenedAt.reset();
    }
    bool active = m_dropHint != nullptr && m_dropHint->tickAnimations(nowMsec);
    active = tickEdgePan(nowMsec) || active;
    const double lastTickMsec = std::exchange(m_lastTickMsec, nowMsec);
    const double lastProgress = m_progress;
    layoutDragCard();
    const bool zoomTicked = m_zoomAnim.tick(nowMsec);
    if (zoomTicked && m_zoomAnim.animating() && m_zoomAnim.curve().easing == Easing::Spring) {
      static_cast<void>(m_zoomAnim.finishSpringTail(motionPixelsPerUnit(false)));
    }
    if (zoomTicked) {
      m_progress = m_zoomAnim.current();
    }
    if (m_active) {
      // The content starts when the zoom crossed the gate, between the last frame and this one. Started at this frame,
      // it would lag a long frame behind, and on a frozen test clock (one step, then repeated instants) never move.
      const double gate = Tokens::motionContentGate;
      const bool crossed = zoomTicked && lastTickMsec > 0.0 && lastProgress < gate && m_progress >= gate;
      updateAppear(
          crossed ? lastTickMsec + (nowMsec - lastTickMsec) * (gate - lastProgress) / (m_progress - lastProgress) : 0.0
      );
    }
    // An opacity ends within one 8-bit level; a scale within a pixel of the largest card, an output.
    constexpr double kOpacityLevels = 255.0;
    const auto tickTail = [nowMsec](AnimatedValue& value, double pixelsPerUnit) {
      const bool ticked = value.tick(nowMsec);
      if (ticked && value.animating()) {
        static_cast<void>(value.finishSpringTail(pixelsPerUnit));
      }
      return ticked;
    };
    bool rowTicked = false;
    for (auto it = m_reflows.begin(); it != m_reflows.end();) {
      bool running = false;
      if (auto* glide = std::get_if<Glide>(&it->second)) {
        for (AnimatedValue& offset : glide->offset) {
          if (offset.tick(nowMsec)) {
            rowTicked = true;
            if (offset.animating() && offset.curve().easing == Easing::Spring) {
              static_cast<void>(offset.finishSpringTail(1.0));
            }
          }
          running = running || offset.animating();
        }
      } else {
        auto& fade = std::get<Crossfade>(it->second);
        rowTicked = tickTail(fade.opacity, kOpacityLevels) || rowTicked;
        if (!fade.opacity.animating() && fade.phase == Crossfade::Phase::Out) {
          // Out where it was, then in at its place.
          fade.phase = Crossfade::Phase::In;
          fade.opacity.steerSpring(1.0, motionSpring(Tokens::springFade));
          rowTicked = true;
        }
        running = fade.opacity.animating();
      }
      it = running ? std::next(it) : m_reflows.erase(it);
    }
    active = active || !m_reflows.empty();
    bool appearTicked = tickTail(m_appearOpacity, kOpacityLevels);
    appearTicked = tickTail(m_filterOpacity, kOpacityLevels) || appearTicked;
    appearTicked = tickTail(m_appearScale, motionPixelsPerUnit(true)) || appearTicked;
    for (const auto& state : m_outputs) {
      for (const auto& card : state->cards) {
        appearTicked = tickTail(card->filter, kOpacityLevels) || appearTicked;
        active = active || card->filter.animating();
      }
    }
    active = active || m_appearOpacity.animating() || m_filterOpacity.animating() || m_appearScale.animating();
    for (const auto& state : m_outputs) {
      const bool ticked = state->rowScroll.tick(nowMsec);
      if (ticked && state->rowScroll.animating() && state->rowScroll.curve().easing == Easing::Spring) {
        static_cast<void>(state->rowScroll.finishSpringTail(motionPixelsPerUnit(true)));
      }
      rowTicked = ticked || rowTicked;
      active = active || state->rowScroll.animating();
    }
    if (zoomTicked || rowTicked || appearTicked || m_cardPresentationDirty) {
      applyProgress();
    }
    if (rowTicked && dragCard() != nullptr) {
      // Rows slide under a held card while the edge steps them; the target follows what is under it.
      retargetDrop(m_dragX, m_dragY);
    }
    m_cardPresentationDirty = false;
    if (m_overviewEventPending) {
      m_overviewEventPending = false;
      m_server->notifyOverviewChanged();
    }
    // Cards mirror their views' animation slots, whose timelines keep running after card layout settles.
    if (m_active) {
      syncCardEffects();
    }
    for (const auto& state : m_outputs) {
      // The zoom owns the effect until it settles; only a row slide started after that drives it, so the progress
      // never jumps from one source to the other.
      const AnimatedValue& row = state->rowScroll;
      const bool rowOwns = !m_zoomAnim.animating() && row.transitionId() > m_zoomAnim.transitionId();
      bindAnimationEffect(
          &state->tree->node, AnimationEvent::Overview, rowOwns ? row : m_zoomAnim, m_closing ? -1.0F : 1.0F
      );
    }
    // ANDROMEDA_OVERVIEW_TRACE=1: one line per tick with every overview clock, for motion review (.agent/scratch/overview-motion).
    static const bool kTraceAll = std::getenv("ANDROMEDA_OVERVIEW_TRACE") != nullptr;
    if (kTraceAll && (m_active || m_zoomAnim.animating())) {
      kLog.info(
          "ovtrace {:.2f} p {:.4f} appear {:.4f} scale {:.4f} filter {:.4f} row {:.4f}", nowMsec, m_progress,
          m_appearOpacity.current(), m_appearScale.current(), m_filterOpacity.current(),
          m_outputs.empty() ? 0.0 : m_outputs.front()->rowScroll.current()
      );
    }
    if (m_traceFrames) {
      kLog.debug(
          "overview trace frame {:.1f}: drawn {:.3f}, finger {:.3f}, zoom {}", nowMsec, m_progress, m_fingerTarget,
          m_zoomAnim.animating()
      );
      m_traceFrames = m_zoomAnim.animating() || m_fingerProgress.has_value();
    }
    if (zoomTicked && !m_zoomAnim.animating()) {
      finishAnimation();
    }
    return m_zoomAnim.animating() || active;
  }

  bool Overview::hasActiveAnimations() const {
    return m_zoomAnim.animating()
        || !m_reflows.empty()
        || m_appearOpacity.animating()
        || m_filterOpacity.animating()
        || m_appearScale.animating()
        || std::ranges::any_of(
               m_outputs,
               [](const auto& state) {
                 return std::ranges::any_of(state->cards, [](const auto& card) { return card->filter.animating(); });
               }
        )
        || std::ranges::any_of(m_outputs, [](const auto& state) { return state->rowScroll.animating(); })
        || (m_dropHint != nullptr && m_dropHint->hasActiveAnimations());
  }

  void Overview::finishAnimation() {
    if (!m_closing) {
      applyProgress();
      return;
    }
    View* focus = m_pendingFocus.get();
    m_pendingFocus = nullptr;
    // The real trees stay hidden until teardown, so a workspace switch during the close runs as the row scroll. The
    // close still ends with the zoom, so binds keep their timing; a row still travelling hands its offset and velocity
    // to the real slide. beginClose retargets each row to the active workspace, so a row resting there is not carried.
    std::vector<std::tuple<WorkspaceGroup*, double, double>> carried;
    for (const auto& state : m_outputs) {
      const AnimatedValue& row = state->rowScroll;
      WorkspaceGroup* group = state->output->workspaceGroup();
      if (group != nullptr && row.animating() && row.from() != row.target()) {
        carried.emplace_back(group, std::clamp(row.current() - row.target(), -1.0, 1.0), row.velocity());
      }
    }
    teardown();
    // Real trees are visible again: settle each active workspace so window
    // positions match where the cards landed.
    for (const auto& output : m_server->outputs()) {
      if (WorkspaceGroup* group = output->workspaceGroup()) {
        if (Workspace* workspace = group->active()) {
          workspace->markArrange(false);
        }
      }
    }
    for (const auto& [group, offset, velocity] : carried) {
      if (group->slideBegin(offset < 0, offset > 0)) {
        group->slideApply(offset);
        group->slideSettle(0, velocity);
      }
    }
    restoreFocus(focus);
  }

  void Overview::restoreFocus(View* focus) {
    if (focus != nullptr && focus->mapped()) {
      m_server->focusView(focus, FocusReason::PointerPress);
    } else {
      m_server->refocus();
    }
    // Focus chrome changed while the windows were hidden (the open cleared it, focus returned during the close or
    // just now), so they reappear with the settled result.
    for (const auto& view : m_server->registry().all()) {
      view->settleFocusChrome();
    }
  }

  void Overview::teardown() {
    // Before the closed event: a stale gesture_progress would leave shell chrome following no fingers.
    m_fingerProgress.reset();
    m_server->cursor()->resetWheelAccumulation();
    m_return.reset();
    m_shownRects.clear();
    m_reflows.clear();
    cancelNavigation();
    clearMiddlePress();
    hideDropHint();
    stopEdgeScroll();
    for (const auto& state : m_outputs) {
      clearDesktop(*state);
      for (const auto& card : state->cards) {
        destroyCard(card.get());
      }
      state->cards.clear();
      if (state->backgroundBlur.has_value()) {
        state->backgroundBlur->destroy();
        state->backgroundBlur.reset();
      }
      if (state->tree != nullptr) {
        wlr_scene_node_destroy(&state->tree->node);
        state->tree = nullptr;
      }
      state->output->requestFrame();
    }
    m_outputs.clear();
    wlr_scene_node_set_enabled(&m_server->overviewBlurTree()->node, false);

    if (m_tree != nullptr) {
      wlr_scene_node_set_enabled(&m_tree->node, false);
    }
    wlr_scene_node_set_enabled(&m_server->xdgTree()->node, true);
    wlr_scene_node_set_enabled(&m_server->fullscreenTree()->node, true);
    wlr_scene_node_set_enabled(&m_server->pinnedTree()->node, true);
    if (m_active) {
      applyPinnedOpacity(1.0F);
    }
    wlr_scene_node_set_enabled(&m_server->shellLayerTree(ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM)->node, true);

    m_active = false;
    m_closing = false;
    m_progress = 0.0;
    m_targetProgress = 0.0;
    m_pointerOutput = nullptr;
    m_server->notifyOverviewChanged();
    endThumbDrag(false, 0, 0);
    m_pointer.release();
    m_pressWorkspace = nullptr;
    m_hoverView = nullptr;
    m_closeHover = false;
    m_menu.reset();
    m_rename.reset();
    m_dragSourceWorkspace = nullptr;
    m_dragSourceWidth.reset();
    m_drop = {};
    m_dropWorkspaceGroup = nullptr;
    m_cardPresentationDirty = false;
    m_gestureOpenedHere = false;
    m_filter.clear();
    m_server->reconcileDynamicWorkspaces();
    m_server->scheduleSuspensionSync();
  }

  // -: gesture

  double Overview::gestureGrab(int* heading, bool* caught) {
    if (heading != nullptr) {
      *heading = m_active && !m_closing && m_targetProgress > 0.5 ? 1 : 0;
    }
    if (caught != nullptr) {
      *caught = m_active && m_zoomAnim.animating();
    }
    if (!m_active) {
      return 0.0;
    }
    // A swipe from the settled overview may close it: classify for the row it shows.
    if (openSettled()) {
      classifyEntrance();
    }
    // A swipe catches the zoom where it is drawn, as niri and GNOME do: a running open or close stops there, and a
    // close turns back into an overview the fingers hold. Reading it as fully open or closed jumps the first frame.
    m_zoomAnim.snap(m_progress);
    m_targetProgress = m_progress;
    if (m_closing) {
      m_closing = false;
      m_server->notifyOverviewChanged();
    }
    return std::clamp(m_progress, 0.0, 1.0);
  }

  void Overview::gestureUpdate(double progress) {
    if (!m_active) {
      const auto buildStart = std::chrono::steady_clock::now();
      if (progress <= 0.0 || !beginPresentation()) {
        return;
      }
      m_gestureOpenedAt = std::chrono::steady_clock::now();
      kLog.debug(
          "overview swipe open built in {:.1f} ms",
          std::chrono::duration<double, std::milli>(*m_gestureOpenedAt - buildStart).count()
      );
      m_gestureOpenedHere = true;
      m_progress = 0.0;
    }
    for (const auto& state : m_outputs) {
      state->rowScroll.snap(state->rowScroll.current());
    }
    m_closing = false;
    m_fingerProgress = std::clamp(progress, 0.0, 1.0);
    m_overviewEventPending = true;
    // Already rubber-banded past either end by Gestures::overviewGestureProgress; a second band here made the
    // overscroll stiffer than the fingers.
    const double target = progress;
    m_zoomAnim.snap(target);
    m_fingerTarget = target;
    m_traceFrames = true;
    kLog.debug("overview trace event: finger {:.3f}, drawn {:.3f}", target, m_progress);
    // 1:1 with the fingers, as GNOME's SwipeTracker and niri's overview gesture.
    m_progress = target;
    m_targetProgress = m_progress;
    // Touchpads report faster than the display: lay the cards out once on the next frame, not per event.
    m_cardPresentationDirty = true;
    scheduleFrames();
  }

  void Overview::gestureEnd(int stop, double releaseVelocity) {
    if (!m_active) {
      return;
    }
    m_gestureOpenedHere = false;
    m_fingerProgress.reset();
    if (stop > 0) {
      m_pendingFocus = nullptr;
      startAnimation(1.0, false, releaseVelocity);
      m_server->notifyOverviewChanged();
      return;
    }
    // A close the fingers caught keeps the window it was closing to.
    beginClose(m_pendingFocus.get(), releaseVelocity);
  }

  // -: hooks

  void Overview::onViewMapped(View* view) {
    if (!m_active || view == nullptr || !view->mapped() || view->pinned()) {
      return;
    }
    if (view->attached()) {
      onViewAttachmentsChanged(view->placementRoot());
      return;
    }
    Workspace* workspace = view->workspace();
    OutputState* state = stateForWorkspace(workspace);
    if (state == nullptr || findCard(view) != nullptr) {
      return;
    }
    if (createCard(*state, view, workspace->index()) == nullptr) {
      return;
    }
    applyProgress();
  }

  void Overview::onViewAttachmentsChanged(View* parent) {
    if (!m_active || parent == nullptr) {
      return;
    }
    Card* card = findCard(parent->placementRoot());
    if (card == nullptr || card->owner == nullptr) {
      return;
    }
    // The card's own surfaces stay; the dialogs' are mirrored again from the current attachments.
    std::erase_if(card->surfaces, [card](const std::unique_ptr<CardSurface>& entry) {
      if (entry->view == card->view) {
        return false;
      }
      entry->outputSample.disconnect();
      entry->frameDone.disconnect();
      if (entry->buffer != nullptr) {
        wlr_scene_node_destroy(&entry->buffer->node);
      }
      return true;
    });
    syncCardSurfaces(card);
    layoutOutput(*card->owner);
    card->owner->output->requestFrame();
  }

  void Overview::onViewPinnedChanged(View* view) {
    if (!m_active || view == nullptr || !view->mapped()) {
      return;
    }
    // A pinned window shows through the overview only at the desktop chrome's alpha; a card mirrors full opacity.
    view->setOverviewOpacity(view->pinned() ? desktopChromeAlpha() : 1.0F);
    Card* card = findCard(view);
    OutputState* state = card != nullptr ? card->owner : stateForWorkspace(view->workspace());
    if (view->pinned()) {
      if (card == nullptr) {
        return;
      }
      if (m_middlePressCard.get() == card) {
        clearMiddlePress();
      }
      if (m_drop.view == view) {
        m_drop.view = nullptr;
        m_drop.edge = 0;
        hideDropHint();
      }
      dropCard(view);
    } else if (card == nullptr && state != nullptr && view->workspace() != nullptr) {
      createCard(*state, view, view->workspace()->index());
    }
    if (state != nullptr) {
      layoutOutput(*state);
      state->output->requestFrame();
    }
    applyProgress();
  }

  void Overview::onViewFloatingChanged(View* view) {
    if (!m_active || view == nullptr || !view->mapped() || view->pinned()) {
      return;
    }
    // Cards stack tiled, then floating (populateCards): rebuild so this one
    // moves to its new layer instead of keeping its old spot.
    rebuildCard(view);
    applyProgress();
  }

  void Overview::onViewUnmapped(View* view, bool animateClose) {
    if (!m_active || view == nullptr) {
      return;
    }
    if (m_pendingFocus == view) {
      m_pendingFocus = nullptr;
    }
    if (m_hoverView == view) {
      m_hoverView = nullptr;
      m_closeHover = false;
    }
    if (m_drop.view == view) {
      m_drop.view = nullptr;
      m_drop.edge = 0;
      hideDropHint();
    }
    Workspace* workspace = view->workspace();
    OutputState* state = stateForWorkspace(workspace);
    if (Card* card = findCard(view); card != nullptr && animateClose) {
      snapshotCardForClose(*card);
    }
    dropCard(view);
    // The overview keeps the focus chrome while it owns the seat, so a closed focused window hands focus to the nearest
    // survivor now rather than at zoom-out. Ask before layout detachment so the closing view still identifies its row
    // and column; the replacement is its predecessor, or the next neighbor at the leading edge.
    if (workspace != nullptr && workspace->focusedView() == view) {
      View* replacement = workspace->focusReplacementForRemoval(view);
      if (replacement != nullptr) {
        // The layout judges the pair once this column is gone, so the reveal this unblocks may only fit the survivor.
        workspace->noteRemovalOfFocusedColumn(workspace->layout().columnOf(view));
        if (workspace->active()) {
          m_server->focusView(replacement, FocusReason::Startup);
        } else {
          workspace->setFocusedView(replacement);
        }
      } else {
        workspace->setFocusedView(nullptr);
      }
    }
    if (state != nullptr) {
      layoutOutput(*state);
      state->output->requestFrame();
    }
    applyProgress();
  }

  void Overview::onViewWorkspaceChanged(View* view) {
    if (!m_active || view == nullptr || !view->mapped()) {
      return;
    }
    if (view->pinned()) {
      onViewPinnedChanged(view);
      return;
    }

    Card* card = findCard(view);
    Workspace* workspace = view->workspace();
    OutputState* target = stateForWorkspace(workspace);
    if (card == nullptr) {
      if (target != nullptr) {
        createCard(*target, view, workspace->index());
        layoutOutput(*target);
        target->output->requestFrame();
      }
      applyProgress();
      return;
    }

    OutputState* source = card->owner;
    if (target == nullptr) {
      dropCard(view);
      if (source != nullptr) {
        layoutOutput(*source);
        source->output->requestFrame();
      }
      applyProgress();
      return;
    }

    card->workspaceIndex = workspace->index();
    if (source == target) {
      layoutOutput(*target);
      target->output->requestFrame();
      applyProgress();
      return;
    }
    if (source == nullptr) {
      rebuildCard(view);
      applyProgress();
      return;
    }

    const auto it = std::ranges::find_if(source->cards, [card](const std::unique_ptr<Card>& candidate) {
      return candidate.get() == card;
    });
    if (it == source->cards.end()) {
      rebuildCard(view);
      applyProgress();
      return;
    }

    std::unique_ptr<Card> moved = std::move(*it);
    source->cards.erase(it);
    moved->owner = target;
    wlr_scene_node_reparent(&moved->tree->node, target->tree);
    if (moved->thumb != nullptr) {
      moved->thumb->owner = target;
      wlr_scene_node_reparent(&moved->thumb->tree->node, target->tree);
    }
    target->cards.push_back(std::move(moved));
    layoutOutput(*source);
    layoutOutput(*target);
    source->output->requestFrame();
    target->output->requestFrame();
    applyProgress();
  }

  void Overview::onWorkspaceActivated(WorkspaceGroup* group) {
    if (!m_active || group == nullptr || group->active() == nullptr) {
      return;
    }
    if (m_navigationOutput == group->output()) {
      cancelNavigation();
    }
    OutputState* target = stateFor(group->output());
    if (target == nullptr) {
      return;
    }
    const auto row = static_cast<double>(group->active()->index());
    target->activeWorkspaceIndex = group->active()->index();
    if (m_navigationReleasing
        || (std::abs(target->rowScroll.target() - row) < 0.001 && target->rowScroll.animating())) {
      return;
    }
    if (m_closing) {
      m_pendingFocus = nullptr;
    }
    animateRow(*target, row);
    applyProgress();
  }

  void Overview::onWorkspaceArranged(Workspace* workspace) {
    if (!m_active) {
      return;
    }
    if (OutputState* state = stateForWorkspace(workspace)) {
      layoutOutput(*state);
      state->output->requestFrame();
    }
  }

  void Overview::onWorkspaceInventoryChanged(WorkspaceGroup* group) {
    if (!m_active || group == nullptr) {
      return;
    }
    if (m_navigationOutput == group->output()) {
      cancelNavigation();
    }
    OutputState* state = stateFor(group->output());
    if (state == nullptr) {
      return;
    }
    // Renumbering rows on one side of the active workspace must shift the filmstrip scroll by the same amount,
    // otherwise every surviving card visibly jumps even though its workspace identity did not move.
    if (Workspace* active = group->active()) {
      const double delta = static_cast<double>(active->index()) - static_cast<double>(state->activeWorkspaceIndex);
      state->activeWorkspaceIndex = active->index();
      state->rowScroll.translate(delta);
    }
    syncWorkspaceRows(*state, *group);
    layoutOutput(*state);
    state->output->requestFrame();
    applyProgress();
  }

  void Overview::onFocusChanged() {
    if (!m_active) {
      return;
    }
    cancelNavigation();
    if (m_closing) {
      m_pendingFocus = nullptr;
    }
    applyProgress();
  }

  void Overview::onViewPresentationChanged(View* view) {
    if (!m_active || view == nullptr || stateForWorkspace(view->workspace()) == nullptr) {
      return;
    }
    m_cardPresentationDirty = true;
  }

  void Overview::onDesktopLayerChanged(Output* output) {
    if (!m_active) {
      return;
    }
    if (OutputState* state = stateFor(output)) {
      // layoutOutput re-resolves the stack before placing the rows.
      layoutOutput(*state);
      state->output->requestFrame();
    }
  }

  void Overview::onOutputRemoved(Output* output) {
    if (m_navigationOutput == output) {
      // The output may already be tearing down its workspace group.
      m_navigationOutput = nullptr;
      cancelNavigation();
    }
    if (!m_active) {
      return;
    }
    const auto it = std::ranges::find_if(m_outputs, [output](const std::unique_ptr<OutputState>& state) {
      return state->output == output;
    });
    if (it == m_outputs.end()) {
      return;
    }
    if (m_drop.workspace != nullptr && stateForWorkspace(m_drop.workspace) == it->get()) {
      hideDropHint();
      m_drop = {};
    }
    if (m_dropWorkspaceGroup != nullptr && m_dropWorkspaceGroup->output() == output) {
      hideDropHint();
      m_dropWorkspaceGroup = nullptr;
    }
    if (m_middleOutput == output) {
      clearMiddlePress();
    }
    clearDesktop(**it);
    for (const auto& card : (*it)->cards) {
      destroyCard(card.get());
    }
    (*it)->cards.clear();
    if ((*it)->tree != nullptr) {
      wlr_scene_node_destroy(&(*it)->tree->node);
    }
    m_outputs.erase(it);
    if (m_outputs.empty()) {
      forceClose();
    } else {
      applyProgress();
    }
  }

} // namespace andromeda
