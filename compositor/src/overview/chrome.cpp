#include "overview/chrome.h"

#include "config/config.h"
#include "scene/border_rect.h"
#include "scene/color.h"
#include "scene/surface_blur.h"
#include "scene/text_buffer.h"
#include "wlr.h"

#include <algorithm>
#include <cairo.h>
#include <cmath>
#include <format>

namespace andromeda::ov {

  namespace {
    // A Pango font string for a type token, in px so it does not depend on the DPI.
    std::string tokenFont(const Tokens::Type& type, double ui) {
      return std::format("{} {}px", Tokens::fontFamily, type.size * ui);
    }


    constexpr int kNameHeight = 22;
    constexpr int kNameGap = 6;
    constexpr int kFieldHeight = 36;
    constexpr int kCapsuleHeight = 28;
    constexpr int kCapsuleIcon = 18; // app icon in a title capsule (overview handoff)
    constexpr int kIconGap = 6;      // icon to title
    constexpr int kSearchIcon = 16;  // filter field glyph (overview mockup .ov-search .i)
    constexpr int kCapsuleGap = 6;
    constexpr int kCapsuleMaxWidth = 260;
    constexpr int kCapsuleMinCard = 60; // a card narrower than this shows no title
    constexpr int kCloseMinCard = 80;   // nor, narrower than this, a close button
    constexpr int kPlusRadius = 12;
    constexpr int kCloseSize = 24;
    constexpr int kCloseInset = 4; // the button centres this far inside the card's top-right corner
    constexpr std::array<float, 4> kCloseRed{0.898F, 0.282F, 0.302F, 1.0F};
    constexpr int kMenuWidth = 280;
    constexpr int kMenuPad = 6;
    constexpr int kMenuHeadHeight = 26;
    constexpr int kMenuRowHeight = 48;
    constexpr int kMenuGap = 6; // between the chip and the menu
    constexpr int kMenuRadius = 14;
    constexpr int kMenuArtWidth = 44;
    constexpr int kMenuArtHeight = 28;
    constexpr int kWindowMenuWidth = 240;
    constexpr int kWindowMenuRowHeight = 32;

    // One block of a row's drawing, in its 44 x 28 box (prototype MODE_SVG, cut to the box).
    struct ArtBlock {
      int x, y, w, h;
      float alpha;
    };
    struct MenuRow {
      std::string_view name;
      std::string_view description;
      std::array<ArtBlock, 4> art;
      size_t blocks;
    };
    constexpr std::array<MenuRow, kMenuRows> kMenu{{
        {"Free", "Windows stay where you put them. Snap to edges and layouts.",
         {{{3, 3, 22, 14, 1.0F}, {17, 10, 24, 15, 0.7F}}}, 2},
        {"Scrolling", "Columns on an endless strip. New windows open to the right.",
         {{{0, 3, 8, 22, 0.35F}, {11, 3, 14, 22, 1.0F}, {28, 3, 14, 22, 1.0F}}}, 3},
        {"Split", "Each new window splits the focused one in half.",
         {{{3, 3, 18, 22, 1.0F}, {23, 3, 18, 10, 1.0F}, {23, 15, 8, 10, 1.0F}, {33, 15, 8, 10, 1.0F}}}, 4},
        {"Main + Stack", "One large window, the rest stacked beside it.",
         {{{3, 3, 23, 22, 1.0F}, {28, 3, 13, 6, 1.0F}, {28, 11, 13, 6, 1.0F}, {28, 19, 13, 6, 1.0F}}}, 4},
    }};

    bool boxHolds(const wlr_box& box, double lx, double ly) {
      return lx >= box.x && lx < box.x + box.width && ly >= box.y && ly < box.y + box.height;
    }

    std::array<float, 4> withAlpha(std::array<float, 4> color, float alpha) {
      color[3] = alpha;
      return color;
    }

    std::string span(const std::array<float, 4>& color, std::string_view weight, std::string_view text) {
      return std::format("<span foreground='{}' weight='{}'>{}</span>", rgbaHex(color), weight, escapeMarkup(text));
    }

    // Tabler "search" (24-unit grid, stroke 2, round caps) at px device pixels square.
    wlr_buffer* searchIcon(int px, const std::array<float, 4>& color) {
      cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
      cairo_t* cr = cairo_create(surface);
      cairo_scale(cr, px / 24.0, px / 24.0);
      cairo_set_source_rgba(cr, color[0], color[1], color[2], color[3]);
      cairo_set_line_width(cr, 2.0);
      cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
      cairo_arc(cr, 10.0, 10.0, 7.0, 0.0, 2.0 * M_PI);
      cairo_stroke(cr);
      cairo_move_to(cr, 21.0, 21.0);
      cairo_line_to(cr, 15.0, 15.0);
      cairo_stroke(cr);
      cairo_destroy(cr);
      cairo_surface_flush(surface);
      wlr_buffer* buffer = imageBuffer(surface);
      cairo_surface_destroy(surface);
      return buffer;
    }

    void destroyNode(wlr_scene_tree*& tree) {
      if (tree != nullptr) {
        wlr_scene_node_destroy(&tree->node);
        tree = nullptr;
      }
    }

  } // namespace

  Chrome::Chrome(wlr_scene_tree* parent) : m_tree(wlr_scene_tree_create(parent)) {
    if (m_tree != nullptr) {
      m_filterGlass = std::make_unique<GlassPanel>(m_tree, glass::Kind::Shape);
      m_filterGlass->setBottomLayerOnly(config().appearance.blur.optimized);
      m_filterGlass->setEnabled(false);
    }
  }

  Chrome::~Chrome() {
    if (m_searchIcon != nullptr) {
      wlr_buffer_drop(m_searchIcon);
    }
  }

  void Chrome::raise() {
    if (m_tree != nullptr) {
      wlr_scene_node_raise_to_top(&m_tree->node);
    }
  }

  const void* Chrome::closeAt(double lx, double ly) const {
    if (m_closeKey == nullptr) {
      return nullptr;
    }
    const double r = u(kCloseSize) / 2.0;
    const double dx = lx - (m_closeBox.x + r);
    const double dy = ly - (m_closeBox.y + r);
    return dx * dx + dy * dy <= r * r ? m_closeKey : nullptr;
  }

  std::optional<size_t> Chrome::chipAt(double lx, double ly) const {
    for (const auto& [workspace, box] : m_chips) {
      if (boxHolds(box, lx, ly)) {
        return workspace;
      }
    }
    return std::nullopt;
  }

  int Chrome::menuRowAt(double lx, double ly) const {
    if (!m_menuBox.has_value()) {
      return -1;
    }
    for (int row = 0; row < kMenuRows; ++row) {
      if (boxHolds(m_menuRowBoxes[static_cast<size_t>(row)], lx, ly)) {
        return row;
      }
    }
    return -1;
  }

  bool Chrome::nameAt(double lx, double ly) const { return m_nameBox.has_value() && boxHolds(*m_nameBox, lx, ly); }

  bool Chrome::menuContains(double lx, double ly) const { return m_menuBox.has_value() && boxHolds(*m_menuBox, lx, ly); }

  bool Chrome::plusAt(double lx, double ly) const { return m_plus.alpha > 0.0F && boxHolds(m_plus.box, lx, ly); }

  int Chrome::windowMenuRowAt(double lx, double ly) const {
    if (!m_windowMenuBox.has_value()) {
      return -1;
    }
    for (size_t row = 0; row < m_windowMenuRows.size(); ++row) {
      if (boxHolds(m_windowMenuRows[row], lx, ly)) {
        return static_cast<int>(row);
      }
    }
    return -1;
  }

  bool Chrome::windowMenuContains(double lx, double ly) const {
    return m_windowMenuBox.has_value() && boxHolds(*m_windowMenuBox, lx, ly);
  }

  void Chrome::setLabel(Label& label, const std::string& markup, const LabelStyle& style, double scale) {
    label.used = true;
    std::string key = std::format(
        "{}|{}|{}|{}|{}|{}|{}|{}|{}", markup, style.font, scale, style.maxWidth, style.width, style.height, style.wrap,
        static_cast<const void*>(style.icon), style.iconSize
    );
    if (label.tree != nullptr && key == label.key) {
      return;
    }
    if (label.tree == nullptr) {
      label.tree = wlr_scene_tree_create(m_tree);
      if (label.tree == nullptr) {
        return;
      }
      wlr_scene_node_set_enabled(&label.tree->node, false);
      label.alpha = -1.0F;
    }
    if (label.text != nullptr) {
      wlr_scene_node_destroy(&label.text->node);
      label.text = nullptr;
    }
    const int lead = style.icon != nullptr ? style.iconSize + u(kIconGap) : 0;
    const int textMax = std::max(1, style.maxWidth - 2 * style.padX - lead);
    TextBufferResult rendered = renderTextBuffer({
        .markup = markup,
        .font = std::string(style.font),
        .maxWidth = textMax,
        .padding = 0,
        .scale = scale,
        .ellipsize = !style.wrap,
    });
    label.key = std::move(key);
    if (rendered.buffer == nullptr) {
      label.textWidth = label.textHeight = 0;
      return;
    }
    label.textWidth = rendered.logicalWidth;
    label.textHeight = rendered.logicalHeight;
    const bool capsule = style.height > 0;
    label.height = capsule ? style.height : rendered.logicalHeight;
    label.width = !capsule ? label.textWidth
                  : style.width > 0 ? style.width
                                    : std::max(lead + label.textWidth + 2 * style.padX, style.height);
    label.color = style.fill;
    if (capsule) {
      if (label.fill == nullptr) {
        label.fill = wlr_scene_rect_create(label.tree, 1, 1, std::array<float, 4>{}.data());
      }
      if (label.fill != nullptr) {
        wlr_scene_rect_set_size(label.fill, label.width, label.height);
        wlr_scene_rect_set_clipped_region(label.fill, clipped_region_get_default());
        wlr_scene_rect_set_corner_radii(label.fill, corner_radii_all(label.height / 2));
      }
    }
    label.text = wlr_scene_buffer_create(label.tree, rendered.buffer);
    wlr_buffer_drop(rendered.buffer);
    if (label.text == nullptr) {
      return;
    }
    label.text->point_accepts_input = [](wlr_scene_buffer*, double*, double*) { return false; };
    wlr_scene_buffer_set_dest_size(label.text, label.textWidth, label.textHeight);
    const int left = style.width > 0 ? style.padX : (label.width - lead - label.textWidth) / 2;
    wlr_scene_node_set_position(&label.text->node, left + lead, (label.height - label.textHeight) / 2);
    if (style.icon != nullptr) {
      if (label.icon == nullptr) {
        label.icon = wlr_scene_buffer_create(label.tree, style.icon);
      } else {
        wlr_scene_buffer_set_buffer(label.icon, style.icon);
      }
      if (label.icon != nullptr) {
        label.icon->point_accepts_input = [](wlr_scene_buffer*, double*, double*) { return false; };
        wlr_scene_buffer_set_dest_size(label.icon, style.iconSize, style.iconSize);
        wlr_scene_node_set_position(&label.icon->node, left, (label.height - style.iconSize) / 2);
      }
    } else if (label.icon != nullptr) {
      wlr_scene_node_destroy(&label.icon->node);
      label.icon = nullptr;
    }
    label.alpha = -1.0F; // the new buffer starts opaque
  }

  void Chrome::placeLabel(Label& label, int x, int y, float alpha) {
    if (label.tree == nullptr) {
      return;
    }
    const bool on = alpha > 0.004F && label.text != nullptr;
    wlr_scene_node_set_enabled(&label.tree->node, on);
    if (!on) {
      return;
    }
    wlr_scene_node_set_position(&label.tree->node, x, y);
    if (alpha == label.alpha) {
      return;
    }
    label.alpha = alpha;
    wlr_scene_buffer_set_opacity(label.text, alpha);
    if (label.icon != nullptr) {
      wlr_scene_buffer_set_opacity(label.icon, alpha);
    }
    if (label.fill != nullptr) {
      std::array<float, 4> fill{};
      premultiplied(fill.data(), label.color, alpha);
      wlr_scene_rect_set_color(label.fill, fill.data());
    }
  }

  void Chrome::ensureFrame(Frame& frame) {
    if (frame.tree != nullptr) {
      return;
    }
    frame.tree = wlr_scene_tree_create(m_tree);
    if (frame.tree == nullptr) {
      return;
    }
    const std::array<float, 4> clear{};
    frame.fill = wlr_scene_rect_create(frame.tree, 1, 1, clear.data());
    frame.ring = wlr_scene_border_create(frame.tree, clear.data(), clear.data());
    frame.box = {};
    frame.alpha = -1.0F;
  }

  void Chrome::placeFrame(
      Frame& frame, const wlr_box& box, int radius, int ringWidth, const std::array<float, 4>& fill,
      const std::array<float, 4>& ring, float alpha
  ) {
    ensureFrame(frame);
    if (frame.tree == nullptr) {
      return;
    }
    const bool on = alpha > 0.004F && box.width > 0 && box.height > 0;
    wlr_scene_node_set_enabled(&frame.tree->node, on);
    if (!on) {
      return;
    }
    wlr_scene_node_set_position(&frame.tree->node, box.x, box.y);
    if (box.width != frame.box.width || box.height != frame.box.height) {
      frame.box = box;
      if (frame.fill != nullptr) {
        wlr_scene_rect_set_size(frame.fill, box.width, box.height);
        wlr_scene_rect_set_clipped_region(frame.fill, clipped_region_get_default());
        wlr_scene_rect_set_corner_radii(frame.fill, corner_radii_all(radius));
      }
      if (frame.ring != nullptr) {
        applyBorderGeometry(frame.ring, makeBorderRing(box.width, box.height, radius, ringWidth, 0), ringWidth, 0);
      }
      frame.alpha = -1.0F;
    }
    frame.box.x = box.x;
    frame.box.y = box.y;
    if (alpha == frame.alpha) {
      return;
    }
    frame.alpha = alpha;
    std::array<float, 4> fillColor{};
    std::array<float, 4> ringColor{};
    premultiplied(fillColor.data(), fill, alpha);
    premultiplied(ringColor.data(), ring, alpha);
    if (frame.fill != nullptr) {
      wlr_scene_node_set_enabled(&frame.fill->node, fill[3] > 0.0F);
      wlr_scene_rect_set_color(frame.fill, fillColor.data());
    }
    if (frame.ring != nullptr) {
      wlr_scene_border_set_colors(frame.ring, ringColor.data(), ringColor.data());
    }
  }

  void Chrome::update(const ChromeFrame& frame) {
    m_ui = frame.ui;
    if (m_tree == nullptr || frame.placement == nullptr) {
      return;
    }
    const Placement& placement = *frame.placement;
    const auto& colors = config().colors;
    // Glass pieces are tint-only over the overview's cached wallpaper blur: no per-frame blur (overview handoff perf).
    // The bar islands' glass and ink: Clear glass is dark in both schemes.
    const bool light = configGlassLight(glass::Kind::Shape);
    const std::uint32_t tint = (light ? Tokens::schemeLight : Tokens::schemeDark).tintThick;
    const auto channel = [tint](int shift) { return static_cast<float>((tint >> shift) & 0xFFU) / 255.0F; };
    const std::array<float, 4> thick{channel(24), channel(16), channel(8), configGlass(glass::Kind::Shape).tint};
    // Ink and fills follow the scheme, as the glass tint does: colors.* hold one scheme's values.
    const Tokens::Scheme& scheme = light ? Tokens::schemeLight : Tokens::schemeDark;
    const auto rgba = [](std::uint32_t c) {
      return std::array<float, 4>{
          static_cast<float>((c >> 24) & 0xFFU) / 255.0F, static_cast<float>((c >> 16) & 0xFFU) / 255.0F,
          static_cast<float>((c >> 8) & 0xFFU) / 255.0F, static_cast<float>(c & 0xFFU) / 255.0F
      };
    };
    const std::array<float, 4> thin = rgba(scheme.tintThin);
    const std::array<float, 4> solid = rgba(scheme.solidRegular);
    const std::array<float, 4> l1 = rgba(scheme.label1);
    const std::array<float, 4> l2 = rgba(scheme.label2);
    const std::array<float, 4> l3 = rgba(scheme.label3);
    const double scale = frame.scale;
    const float shown = static_cast<float>(std::clamp(frame.progress, 0.0, 1.0));
    // One fade for every label: the thumbs' appear spring, so the overview arrives and leaves as one piece.
    const float late = frame.thumbAlpha;
    const int ox = frame.output.x;
    const int oy = frame.output.y;
    const auto px = [](double value) { return static_cast<int>(std::lround(value)); };
    // Strip chrome (the plus, the filter) rides the last thumb's zoom travel and piece scale, so the strip moves as one.
    const auto rideStrip = [&](const Rect& settled) {
      wlr_box box{ox + px(settled.x), oy + px(settled.y), px(settled.w), px(settled.h)};
      for (auto it = placement.cards.rbegin(); it != placement.cards.rend(); ++it) {
        if (it->kind != CardKind::Thumb || it->workspace >= frame.workspaces.size() || it->rect.w <= 0) {
          continue;
        }
        const ChromeWorkspace& thumb = frame.workspaces[it->workspace];
        if (thumb.shown && thumb.card.width > 0) {
          const double sx = thumb.card.width / it->rect.w;
          const double sy = thumb.card.height / it->rect.h;
          box = {px(thumb.card.x + (settled.x - it->rect.x) * sx), px(thumb.card.y + (settled.y - it->rect.y) * sy),
                 px(settled.w * sx), px(settled.h * sy)};
        }
        break;
      }
      return box;
    };

    m_chips.clear();
    for (Label& label : m_names) {
      label.used = false;
    }
    m_names.resize(std::max(m_names.size(), frame.workspaces.size()));
    m_nameBox.reset();
    for (size_t index = 0; index < frame.workspaces.size(); ++index) {
      const ChromeWorkspace& workspace = frame.workspaces[index];
      Label& label = m_names[index];
      if (!workspace.shown) {
        continue;
      }
      // A thumb's name is plain text centred under it; a row name carries its arrangement chip. The shown workspace's
      // name turns into a text field with an accent ring while it is renamed (F2); the caret ends the text.
      const bool renaming = frame.renaming.has_value() && index == frame.current;
      const bool chip = workspace.kind != CardKind::Thumb && !workspace.arrangement.empty() && !renaming;
      std::string markup = renaming ? span(l1, "600", *frame.renaming) + span(colors.accentPrimary, "normal", "▏")
                                    : span(l1, "600", workspace.name);
      if (chip) {
        markup += span(l2, "500", std::format("  ·  {}  ▾", workspace.arrangement));
      }
      const bool capsule = chip || renaming;
      setLabel(
          label, markup,
          {.font = tokenFont(Tokens::typeBody, m_ui),
           .padX = capsule ? 8 : 0,
           .height = capsule ? u(kNameHeight) : 0,
           .maxWidth = u(280),
           .fill = renaming ? thick : thin},
          scale
      );
      const wlr_box& card = workspace.card;
      const bool above = workspace.kind == CardKind::Frame || frame.style == Style::TaskView;
      const int x = workspace.kind == CardKind::Thumb ? card.x + (card.width - label.width) / 2 : card.x;
      const int y = above ? card.y - label.height - u(kNameGap) : card.y + card.height + u(kNameGap);
      placeLabel(label, x, y, shown);
      const wlr_box box{x, y, label.width, label.height};
      if (chip && shown > 0.5F) {
        m_chips.emplace_back(index, box);
      }
      if (index == frame.current) {
        m_nameBox = box;
        placeFrame(m_nameRing, box, box.height / 2, 2, {}, colors.accentPrimary, renaming ? shown : 0.0F);
      }
    }
    if (!m_nameBox.has_value()) {
      placeFrame(m_nameRing, {}, 0, 0, {}, {}, 0.0F);
    }
    for (Label& label : m_names) {
      if (!label.used && label.tree != nullptr) {
        wlr_scene_node_set_enabled(&label.tree->node, false);
      }
    }

    if (!frame.filter.empty()) {
      m_shownFilter = frame.filter;
    }
    const float filterAlpha = late * frame.filterAlpha;
    if (placement.filter.has_value() && filterAlpha > 0.004F) {
      // Fading out after the filter clears, the field keeps the last text instead of flashing a placeholder.
      const std::string markup = span(l1, "normal", m_shownFilter);
      const int iconPx = std::max(1, static_cast<int>(std::lround(u(kSearchIcon) * scale)));
      if (m_searchIcon == nullptr || m_searchIconPx != iconPx || m_searchIconColor != l2) {
        if (m_searchIcon != nullptr) {
          wlr_buffer_drop(m_searchIcon);
        }
        m_searchIcon = searchIcon(iconPx, l2);
        m_searchIconPx = iconPx;
        m_searchIconColor = l2;
      }
      setLabel(
          m_filter, markup,
          {.font = tokenFont(Tokens::typeBody, m_ui),
           .padX = u(14),
           .height = u(kFieldHeight),
           .maxWidth = u(kFilterWidth),
           .width = u(kFilterWidth),
           .fill = {},
           .icon = m_searchIcon,
           .iconSize = u(kSearchIcon)},
          scale
      );
      // A fixed place: the field only fades, it never rides the strip's travel.
      const int fx = ox + px(placement.filter->x);
      const int fy = oy + px(placement.filter->y);
      placeLabel(m_filter, fx, fy, filterAlpha);
      if (m_filterGlass != nullptr) {
        m_filterGlass->setEnabled(true);
        m_filterGlass->place({fx, fy, u(kFilterWidth), u(kFieldHeight)}, u(kFieldHeight) / 2, filterAlpha);
      }
    } else {
      placeLabel(m_filter, 0, 0, 0.0F);
      if (m_filterGlass != nullptr) {
        m_filterGlass->setEnabled(false);
      }
    }

    while (m_empties.size() < frame.workspaces.size()) {
      // Made before its label's lazy tree, so the glass sits under the text. It blurs the whole scene: the thumb's
      // wallpaper mirror under it is not the bottom layer.
      m_emptyGlass.push_back(std::make_unique<GlassPanel>(m_tree, glass::Kind::Shape));
      m_emptyGlass.back()->setEnabled(false);
      m_empties.emplace_back();
    }
    for (size_t index = 0; index < m_empties.size(); ++index) {
      Label& label = m_empties[index];
      GlassPanel& glass = *m_emptyGlass[index];
      const ChromeWorkspace* workspace = index < frame.workspaces.size() ? &frame.workspaces[index] : nullptr;
      if (workspace == nullptr || !workspace->shown || !workspace->empty || workspace->card.width <= 0) {
        placeLabel(label, 0, 0, 0.0F);
        glass.setEnabled(false);
        continue;
      }
      setLabel(
          label, span(l2, "normal", "No windows"),
          {.font = tokenFont(Tokens::typeFootnote, m_ui), .padX = u(10), .height = u(kNameHeight), .maxWidth = workspace->card.width},
          scale
      );
      const wlr_box& card = workspace->card;
      const int x = card.x + (card.width - label.width) / 2;
      const int y = card.y + (card.height - label.height) / 2;
      placeLabel(label, x, y, late);
      glass.setEnabled(late > 0.004F);
      glass.place({x, y, label.width, label.height}, label.height / 2, late);
    }

    const ChromeWorkspace* chosen = frame.current < frame.workspaces.size() ? &frame.workspaces[frame.current] : nullptr;
    if (frame.noMatch && chosen != nullptr && chosen->shown) {
      const wlr_box area = placement.main.has_value()
          ? wlr_box{ox + px(placement.main->x), oy + px(placement.main->y), px(placement.main->w), px(placement.main->h)}
          : chosen->card;
      setLabel(
          m_noMatch,
          span(l2, "normal", std::format("No windows match “{}”. Enter searches apps and files.", frame.filter)),
          {.font = tokenFont(Tokens::typeBody, m_ui), .maxWidth = std::max(1, area.width - u(48))}, scale
      );
      placeLabel(m_noMatch, area.x + (area.width - m_noMatch.width) / 2, area.y + (area.height - m_noMatch.height) / 2, late);
    } else {
      placeLabel(m_noMatch, 0, 0, 0.0F);
    }

    const int usableBottom = frame.usable.y + frame.usable.height;
    const ChromeWorkspace* current =
        frame.current < frame.workspaces.size() ? &frame.workspaces[frame.current] : nullptr;

    // The shown workspace's card carries a 2 px ink ring just outside it (overview.css .ov-ws.cur; the handoff's 3 px
    // accent is the hover ring, docs/design/deviations.md).
    if (current != nullptr && current->shown && current->card.width > 0) {
      const int width = 2;
      const wlr_box& card = current->card;
      const wlr_box box{card.x - width, card.y - width, card.width + 2 * width, card.height + 2 * width};
      placeFrame(m_current, box, u(kPlusRadius) + width, width, {}, l1, shown);
    } else {
      placeFrame(m_current, {}, u(kPlusRadius), 2, {}, {}, 0.0F);
    }

    if (placement.plus.has_value()) {
      const Rect& plus = *placement.plus;
      const wlr_box box = rideStrip(plus);
      placeFrame(m_plus, box, u(kPlusRadius), 2, {0.5F, 0.5F, 0.5F, 0.08F}, l3, frame.thumbAlpha);
      setLabel(m_plusMark, span(l2, "normal", "+"), {.font = tokenFont(Tokens::typeLarge, m_ui)}, scale);
      placeLabel(
          m_plusMark, box.x + (box.width - m_plusMark.width) / 2, box.y + (box.height - m_plusMark.height) / 2,
          frame.thumbAlpha
      );
    } else {
      placeFrame(m_plus, {}, u(kPlusRadius), 2, {}, {}, 0.0F);
      placeLabel(m_plusMark, 0, 0, 0.0F);
    }


    // Window title capsules, centred under their cards, in card stacking order.
    const int capsuleFloor = usableBottom - u(kCapsuleGap);
    for (auto& [key, label] : m_titles) {
      label.used = false;
    }
    for (const ChromeWindow& window : frame.windows) {
      if (window.key == nullptr) {
        continue;
      }
      Label& label = m_titles[window.key];
      setLabel(
          label, span(l1, "600", window.title.empty() ? std::string_view("Untitled") : window.title),
          {.font = tokenFont(Tokens::typeFootnote, m_ui),
           .padX = u(10),
           .height = u(kCapsuleHeight),
           .maxWidth = u(kCapsuleMaxWidth),
           .fill = thick,
           .icon = window.icon,
           .iconSize = u(kCapsuleIcon)},
          scale
      );
      const int centre = window.box.x + window.box.width / 2;
      // An off-screen Strip column sits beside its card; clamped in, its capsule would cover a neighbour's.
      const bool inCard = !window.within.has_value()
          || (centre >= window.within->x && centre < window.within->x + window.within->width);
      const bool wanted = (frame.titlesAlways || window.selected || window.key == frame.hovered)
          && inCard
          && !window.dragged
          && window.box.width > u(kCapsuleMinCard)
          && window.opacity > 0.45F;
      int x = window.box.x + (window.box.width - label.width) / 2;
      if (window.within.has_value()) {
        const wlr_box& card = *window.within;
        x = std::clamp(x, card.x, std::max(card.x, card.x + card.width - label.width));
      }
      // A card reaching down to the usable bottom, or to the bottom of its workspace card, carries its capsule over its own
      // bottom edge instead.
      const int floor =
          window.within.has_value() ? window.within->y + window.within->height - u(kCapsuleGap) : capsuleFloor;
      const int y = std::min(window.box.y + window.box.height + u(kCapsuleGap), floor - label.height);
      placeLabel(label, x, y, wanted ? late : 0.0F);
      if (label.tree != nullptr) {
        wlr_scene_node_raise_to_top(&label.tree->node);
      }
    }
    // The hovered card's close button, centred just inside its top-right corner, above every capsule.
    const ChromeWindow* closeFor = nullptr;
    for (const ChromeWindow& window : frame.windows) {
      if (window.key != nullptr && window.key == frame.hovered) {
        closeFor = &window;
      }
    }
    m_closeKey = nullptr;
    if (closeFor != nullptr
        && !closeFor->dragged
        && closeFor->box.width > u(kCloseMinCard)
        && closeFor->opacity > 0.45F
        && late > 0.5F) {
      const std::string closeFont = tokenFont(Tokens::typeBody, m_ui);
      const LabelStyle style{.font = closeFont, .height = u(kCloseSize), .maxWidth = u(kCloseSize), .fill = thick};
      setLabel(m_close, span(l1, "bold", "✕"), style, scale);
      LabelStyle hot = style;
      hot.fill = kCloseRed;
      setLabel(m_closeHot, span({1.0F, 1.0F, 1.0F, 1.0F}, "bold", "✕"), hot, scale);
      m_closeBox = {
          closeFor->box.x + closeFor->box.width - u(kCloseInset) - u(kCloseSize) / 2,
          closeFor->box.y + u(kCloseInset) - u(kCloseSize) / 2,
          u(kCloseSize),
          u(kCloseSize),
      };
      m_closeKey = closeFor->key;
      Label& on = frame.closeHovered ? m_closeHot : m_close;
      Label& off = frame.closeHovered ? m_close : m_closeHot;
      placeLabel(off, 0, 0, 0.0F);
      placeLabel(on, m_closeBox.x, m_closeBox.y, late);
      if (on.tree != nullptr) {
        wlr_scene_node_raise_to_top(&on.tree->node);
      }
    } else {
      placeLabel(m_close, 0, 0, 0.0F);
      placeLabel(m_closeHot, 0, 0, 0.0F);
    }

    // The arrangement menu, under its chip (above it when the output ends first), over everything else.
    std::optional<wlr_box> anchor;
    if (frame.menu.has_value()) {
      for (const auto& [workspace, box] : m_chips) {
        if (workspace == frame.menu->workspace) {
          anchor = box;
        }
      }
    }
    const auto raise = [](wlr_scene_tree* tree) {
      if (tree != nullptr) {
        wlr_scene_node_raise_to_top(&tree->node);
      }
    };
    m_menuBox.reset();
    if (anchor.has_value() && frame.workspaces.size() > frame.menu->workspace) {
      const int height = 2 * u(kMenuPad) + u(kMenuHeadHeight) + kMenuRows * u(kMenuRowHeight);
      const int right = frame.output.x + frame.output.width - u(kMenuPad) - u(kMenuWidth);
      const int x = std::max(frame.output.x + u(kMenuPad), std::min(anchor->x, right));
      int y = anchor->y + anchor->height + u(kMenuGap);
      if (y + height > frame.output.y + frame.output.height - u(kMenuPad)) {
        y = anchor->y - u(kMenuGap) - height;
      }
      m_menuBox = wlr_box{x, y, u(kMenuWidth), height};
      // No backdrop blur under this panel, so it is near solid: the cards' edges must not show through.
      placeFrame(m_menu, *m_menuBox, u(kMenuRadius), 1, withAlpha(solid, 0.97F), withAlpha(l1, 0.1F), 1.0F);
      raise(m_menu.tree);
      const std::string_view name = frame.workspaces[frame.menu->workspace].name;
      setLabel(m_menuHead, span(l2, "600", std::format("Arrange “{}”", name)), {.font = tokenFont(Tokens::typeCaption, m_ui), .maxWidth = u(240)}, scale);
      placeLabel(m_menuHead, x + u(kMenuPad) + u(10), y + u(kMenuPad) + (u(kMenuHeadHeight) - m_menuHead.height) / 2, 1.0F);
      raise(m_menuHead.tree);
      const int textX = u(kMenuPad) + u(10) + u(kMenuArtWidth) + u(10);
      for (size_t row = 0; row < kMenu.size(); ++row) {
        const MenuRow& item = kMenu[row];
        const wlr_box box{
            x + u(kMenuPad), y + u(kMenuPad) + u(kMenuHeadHeight) + static_cast<int>(row) * u(kMenuRowHeight),
            u(kMenuWidth) - 2 * u(kMenuPad), u(kMenuRowHeight)
        };
        m_menuRowBoxes[row] = box;
        if (static_cast<int>(row) == frame.menu->hovered) {
          placeFrame(m_menuHover, box, u(8), 1, withAlpha(l1, 0.08F), {}, 1.0F);
          raise(m_menuHover.tree);
        }
        const int artX = box.x + u(10);
        const int artY = box.y + (u(kMenuRowHeight) - u(kMenuArtHeight)) / 2;
        for (size_t block = 0; block < m_menuArt[row].size(); ++block) {
          Frame& art = m_menuArt[row][block];
          if (block >= item.blocks) {
            placeFrame(art, {}, 0, 0, {}, {}, 0.0F);
            continue;
          }
          const ArtBlock& b = item.art[block];
          placeFrame(art, {artX + u(b.x), artY + u(b.y), u(b.w), u(b.h)}, u(2), 1, colors.accentPrimary, {}, b.alpha);
          raise(art.tree);
        }
        std::string markup = span(l1, "600", item.name);
        if (static_cast<int>(row) == frame.menu->current) {
          markup += span(colors.accentPrimary, "bold", "  ✓");
        }
        markup += std::format(
            "\n<span size='small' foreground='{}'>{}</span>", rgbaHex(l2), escapeMarkup(item.description)
        );
        Label& label = m_menuLabels[row];
        setLabel(label, markup, {.font = tokenFont(Tokens::typeFootnote, m_ui), .maxWidth = u(kMenuWidth) - textX - u(kMenuPad) - u(10), .wrap = true}, scale);
        placeLabel(label, x + textX, box.y + (u(kMenuRowHeight) - label.height) / 2, 1.0F);
        raise(label.tree);
      }
    }
    if (!m_menuBox.has_value() || frame.menu->hovered < 0) {
      placeFrame(m_menuHover, {}, 0, 0, {}, {}, 0.0F);
    }
    if (!m_menuBox.has_value()) {
      placeFrame(m_menu, {}, 0, 0, {}, {}, 0.0F);
      placeLabel(m_menuHead, 0, 0, 0.0F);
      for (size_t row = 0; row < kMenu.size(); ++row) {
        placeLabel(m_menuLabels[row], 0, 0, 0.0F);
        for (Frame& art : m_menuArt[row]) {
          placeFrame(art, {}, 0, 0, {}, {}, 0.0F);
        }
      }
    }

    // Window menu: plain text rows at the pointer, kept on the output; same panel look as the arrangement menu.
    m_windowMenuBox.reset();
    const size_t windowRows = frame.windowMenu.has_value() ? frame.windowMenu->rows.size() : 0;
    if (m_windowMenuLabels.size() < windowRows) {
      m_windowMenuLabels.resize(windowRows);
      m_windowMenuRows.resize(windowRows);
    }
    if (windowRows > 0) {
      const ChromeWindowMenu& wm = *frame.windowMenu;
      const int height = 2 * u(kMenuPad) + static_cast<int>(windowRows) * u(kWindowMenuRowHeight);
      const int x = std::clamp(
          wm.x, frame.output.x + u(kMenuPad), frame.output.x + frame.output.width - u(kMenuPad) - u(kWindowMenuWidth)
      );
      const int y =
          std::clamp(wm.y, frame.output.y + u(kMenuPad), frame.output.y + frame.output.height - u(kMenuPad) - height);
      m_windowMenuBox = wlr_box{x, y, u(kWindowMenuWidth), height};
      placeFrame(
          m_windowMenu, *m_windowMenuBox, u(kMenuRadius), 1, withAlpha(solid, 0.97F), withAlpha(l1, 0.1F), 1.0F
      );
      raise(m_windowMenu.tree);
      for (size_t row = 0; row < windowRows; ++row) {
        const wlr_box box{
            x + u(kMenuPad), y + u(kMenuPad) + static_cast<int>(row) * u(kWindowMenuRowHeight), u(kWindowMenuWidth) - 2 * u(kMenuPad),
            u(kWindowMenuRowHeight)
        };
        m_windowMenuRows[row] = box;
        if (static_cast<int>(row) == wm.hovered) {
          placeFrame(m_windowMenuHover, box, 8, 1, withAlpha(l1, 0.08F), {}, 1.0F);
          raise(m_windowMenuHover.tree);
        }
        const bool danger = wm.dangerFrom >= 0 && static_cast<int>(row) >= wm.dangerFrom;
        Label& label = m_windowMenuLabels[row];
        setLabel(
            label, span(danger ? colors.error : l1, "normal", wm.rows[row]),
            {.font = tokenFont(Tokens::typeBody, m_ui), .maxWidth = u(kWindowMenuWidth) - 2 * u(kMenuPad) - u(24)}, scale
        );
        placeLabel(label, box.x + u(12), box.y + (u(kWindowMenuRowHeight) - label.height) / 2, 1.0F);
        raise(label.tree);
      }
    }
    if (!m_windowMenuBox.has_value() || frame.windowMenu->hovered < 0) {
      placeFrame(m_windowMenuHover, {}, 0, 0, {}, {}, 0.0F);
    }
    if (!m_windowMenuBox.has_value()) {
      placeFrame(m_windowMenu, {}, 0, 0, {}, {}, 0.0F);
    }
    for (size_t row = windowRows; row < m_windowMenuLabels.size(); ++row) {
      placeLabel(m_windowMenuLabels[row], 0, 0, 0.0F);
    }

    for (auto it = m_titles.begin(); it != m_titles.end();) {
      if (it->second.used) {
        ++it;
        continue;
      }
      destroyNode(it->second.tree);
      it = m_titles.erase(it);
    }
  }

} // namespace andromeda::ov
