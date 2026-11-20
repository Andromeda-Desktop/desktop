#pragma once

#include "design_tokens.h"
#include "motion/desk_motion.h"
#include "protocols/glass_material.h"
#include "ui/signal.h"

#include <cstdint>
#include <optional>

enum class PaletteTone : std::uint8_t;

namespace Style {

  [[nodiscard]] inline desk::motion::SpringConfig spring(const Tokens::Spring& token) {
    return {.damping = token.damping, .stiffness = token.stiffness};
  }

  TOKENS_LIVE(barThicknessDefault, Tokens::componentIslandHeight);

  inline constexpr float borderWidth = 1.0F;
  inline constexpr float emphasizedBorderWidth = 3.0F;
  inline constexpr float focusRingWidth = 2.0F;
  inline constexpr float focusRingGap = 2.0F;
  inline constexpr float progressMarkerWidth = 2.0F;
  inline constexpr float disabledOutlineAlpha = 0.5F;

  inline constexpr const float& spaceXs = Tokens::spaceP4;
  inline constexpr const float& spaceSm = Tokens::spaceP8;
  inline constexpr const float& spaceMd = Tokens::spaceP12;
  inline constexpr const float& spaceLg = Tokens::spaceP16;

  inline constexpr const float& cardPadding = Tokens::spaceP12;
  inline constexpr const float& panelPadding = Tokens::spaceP12;

  // Default inner inset for bar widget capsules (logical px, before bar content scale): R5, symbol end and text end.
  inline constexpr const float& barCapsuleInsetSymbol = Tokens::componentIslandInsetSymbol;
  inline constexpr const float& barCapsuleInsetText = Tokens::componentIslandInsetText;
  inline constexpr float baseGlyphSize = 16.0F;

  inline constexpr const float& fontSizeMini = Tokens::typeCaption.size;
  inline constexpr const float& fontSizeCaption = Tokens::typeFootnote.size;
  inline constexpr const float& fontSizeBody = Tokens::typeBody.size;
  inline constexpr const float& fontSizeTitle = Tokens::typeHeadline.size;
  inline constexpr const float& fontSizeHeader = Tokens::typeTitle1.size;

  inline constexpr const float& controlHeightSm = Tokens::sizeControlS;
  inline constexpr const float& controlHeight = Tokens::sizeControlM;
  inline constexpr const float& controlHeightLg = Tokens::sizeControlL;
  // Body and footnote text on glass is one step heavier (400 -> 500); other weights stay.
  [[nodiscard]] inline int glassWeight(int weight) noexcept {
    return weight == 400 ? weight + Tokens::typeRulesGlassWeightAdd : weight;
  }
  inline constexpr float scrollWheelStep = 56.0F;
  // Pointer distance in logical px before an armed drag becomes active.
  inline constexpr const float& dragStartThreshold = Tokens::motionSlop;

  // Base scrollbar thickness at rest, and while the pointer is over it. The bar overlays the
  // content as it expands, so hovering never reflows the scroll view.
  inline constexpr const float& scrollbarWidth = Tokens::componentScrollbarWidth;
  inline constexpr const float& scrollbarHoverWidth = Tokens::componentScrollbarHoverWidth;
  inline constexpr const float& scrollbarGap = Tokens::componentScrollbarInset;
  // Shortest the thumb gets on a long document; it must stay a usable drag target.
  inline constexpr const float& scrollbarMinThumbHeight = Tokens::componentScrollbarMinLength;
  // Pointer margin on the content side of the bar, so a thin bar stays an easy grab target.
  inline constexpr const float& scrollbarHitSlop = Tokens::motionSlop;

  // Growth cap (logical px, before content scale) for menus/dropdowns that size to their content.
  inline constexpr const float& menuAutoMaxWidth = Tokens::componentMenuMaxWidth;

  // Window switcher carousel geometry and depth treatment.
  inline constexpr const float& windowSwitcherDimOpacity = Tokens::componentSwitcherDimOpacity;
  inline constexpr const float& windowSwitcherSelectedCardWidth = Tokens::componentSwitcherCardSelected;
  inline constexpr const float& windowSwitcherNearCardWidth = Tokens::componentSwitcherCardNear;
  inline constexpr const float& windowSwitcherFarCardWidth = Tokens::componentSwitcherCardFar;
  inline constexpr const float& windowSwitcherCompactCardWidth = Tokens::componentSwitcherCardCompact;
  inline constexpr const float& windowSwitcherPreviewAspect = Tokens::componentSwitcherPreviewAspect;
  TOKENS_LIVE(windowSwitcherCardOverlap, controlHeightLg + spaceLg);
  inline constexpr const float& windowSwitcherNarrowLayoutThreshold = Tokens::componentSwitcherNarrowThreshold;
  inline constexpr const float& windowSwitcherPreviewIconScale = Tokens::componentSwitcherPreviewIconScale;
  inline constexpr const float& windowSwitcherFallbackIconScale = Tokens::componentSwitcherFallbackIconScale;
  TOKENS_LIVE(windowSwitcherCaptionHeight, controlHeightLg);
  TOKENS_LIVE(windowSwitcherCaptionLineGap, -spaceXs);
  inline constexpr const float& windowSwitcherIncomingCardScale = Tokens::componentSwitcherIncomingScale;
  inline constexpr const float& windowSwitcherIncomingCardSlide = Tokens::componentSwitcherIncomingSlide;
  inline constexpr const float& windowSwitcherOutgoingCardSlide = Tokens::componentSwitcherOutgoingSlide;
  inline constexpr const float& windowSwitcherRevealScale = Tokens::componentSwitcherRevealScale;

  // One toggle size for every preset. Track height = thumb + 2 * inset; track width = thumb + 2 * inset + travel.
  inline constexpr const float& toggleThumbSizeSm = Tokens::componentToggleKnob;
  inline constexpr const float& toggleInsetSm = Tokens::componentToggleInset;
  TOKENS_LIVE(toggleTravelSm, Tokens::componentToggleWidth - Tokens::componentToggleKnob - 2.0F * Tokens::componentToggleInset);
  inline constexpr const float& toggleThumbSizeMd = Tokens::componentToggleKnob;
  inline constexpr const float& toggleInsetMd = Tokens::componentToggleInset;
  TOKENS_LIVE(toggleTravelMd, toggleTravelSm);
  inline constexpr const float& toggleThumbSizeLg = Tokens::componentToggleKnob;
  inline constexpr const float& toggleInsetLg = Tokens::componentToggleInset;
  TOKENS_LIVE(toggleTravelLg, toggleTravelSm);

  // Slider geometry.
  inline constexpr float sliderDefaultWidth = 180.0F;
  inline constexpr const float& sliderTrackHeight = Tokens::componentRangeTrack;
  inline constexpr const float& sliderThumbSize = Tokens::componentRangeKnob;
  inline constexpr float sliderHorizontalPadding = 2.0F;

  [[nodiscard]] float cornerRadiusScale() noexcept;
  void setCornerRadiusScale(float scale) noexcept;

  [[nodiscard]] bool buttonBordersEnabled() noexcept;
  void setButtonBordersEnabled(bool enabled);
  Signal<>& buttonBordersChanged();

  [[nodiscard]] bool inputBordersEnabled() noexcept;
  void setInputBordersEnabled(bool enabled);
  Signal<>& inputBordersChanged();

  [[nodiscard]] bool popupBordersEnabled() noexcept;
  void setPopupBordersEnabled(bool enabled);

  [[nodiscard]] bool rtl() noexcept;
  void setRtl(bool rtl) noexcept;

  [[nodiscard]] bool cardBordersEnabled() noexcept;
  void setCardBordersEnabled(bool enabled);

  [[nodiscard]] bool popupShadowsEnabled() noexcept;
  void setPopupShadowsEnabled(bool enabled);

  // The glass style and its sliders (glass::Settings); reduce transparency makes every glass surface solid.
  // compositorBlur: the compositor blurs and draws glass tints (Andromeda appearance.blur.enabled).
  void setGlass(const glass::Settings& settings, bool compositorBlur = false);
  // Emitted by setGlass when a glass setting changes, before the palette change that follows it:
  // surfaces set their glass::tone here so no paletteChanged handler reads a stale tone.
  Signal<>& glassChanged();
  // True when the compositor draws the glass tint a surface sends (dsk v9): the client then paints no material fill.
  [[nodiscard]] bool compositorDrawsTint() noexcept;
  // The active glass style.
  [[nodiscard]] glass::Style glassStyle() noexcept;
  // The one glass mapping (protocols/glass_material.h) for the current slider, scheme and reduce transparency.
  // Shell glass: the tone of glass::tone.
  [[nodiscard]] glass::Material glassMaterial(glass::Kind kind) noexcept;
  // Glass of a surface in `tone` (app windows: PaletteTone::Scheme).
  [[nodiscard]] glass::Material glassMaterial(glass::Kind kind, PaletteTone tone) noexcept;
  // Shell popover glass (glass::popover) in the island tone.
  [[nodiscard]] glass::Material popoverMaterial() noexcept;
  // Tint alpha for a configured `opacity` relative to glass::referenceOpacity (1 = solid).
  [[nodiscard]] float glassTint(glass::Kind kind, float opacity) noexcept;
  // The tint of a glass app window (scheme tone): the sheet tint.
  [[nodiscard]] float windowGlassTint() noexcept;
  [[nodiscard]] bool reduceTransparency() noexcept;

  [[nodiscard]] bool closeButtonsEnabled() noexcept;
  void setCloseButtonsEnabled(bool enabled);

  [[nodiscard]] float scaledRadius(float radius, float localScale = 1.0F) noexcept;
  [[nodiscard]] float scaledRadiusSm(float localScale = 1.0F) noexcept;
  [[nodiscard]] float scaledRadiusMd(float localScale = 1.0F) noexcept;
  [[nodiscard]] float scaledRadiusLg(float localScale = 1.0F) noexcept;
  // Menu and popover cards (component.menu.radius).
  [[nodiscard]] float scaledMenuRadius(float localScale = 1.0F) noexcept;
  [[nodiscard]] float scaledRadiusXl(float localScale = 1.0F) noexcept;

} // namespace Style
