#include "theme/live_appearance_sync.h"

#include "compositors/compositor_detect.h"
#include "compositors/settings_control.h"
#include "compositors/shell_wayland_connection.h"
#include "design_tokens.h"
#include "render/animation/motion_service.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <array>
#include <format>

namespace {

  float parseUnit(const std::string& text, float fallback) {
    float value = fallback;
    std::from_chars(text.data(), text.data() + text.size(), value);
    return std::clamp(value, 0.0F, 1.0F);
  }

  std::string lowercase(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
  }

  // The compositor reports colors as #rrggbbaa; the shell uses #rrggbb for opaque ones. Compare in one form.
  std::string normalizeColor(std::string_view text) {
    std::string out = lowercase(text);
    if (out.size() == 9 && out[0] == '#' && out.ends_with("ff")) {
      out.resize(7);
    }
    return out;
  }

} // namespace

LiveAppearance readLiveAppearance(const std::function<std::string(std::string_view key)>& value) {
  float speed = 1.0F;
  const std::string speedText = value("animation.speed");
  std::from_chars(speedText.data(), speedText.data() + speedText.size(), speed);
  return LiveAppearance{
      .scheme = value("appearance.scheme"),
      .accent = normalizeColor(value("appearance.accent")),
      .glass =
          glass::Settings{
              .style = value("appearance.glass_style") == "liquid"    ? glass::Style::Liquid
                  : value("appearance.glass_style") == "clear"   ? glass::Style::Clear
                  : value("appearance.glass_style") == "frosted" ? glass::Style::Frosted
                                                                 : glass::Style::Tinted,
              .tint = parseUnit(value("appearance.glass"), glass::Settings{}.tint),
              .frost = parseUnit(value("appearance.frost"), glass::Settings{}.frost),
              .edgeReflection = value("appearance.edge_reflection") != "false",
              .liquidTint = parseUnit(value("appearance.liquid_tint"), glass::Settings{}.liquidTint),
              .liquidBlur = parseUnit(value("appearance.liquid_blur"), 0.1F),
              .lens = parseUnit(value("appearance.liquid"), 0.5F),
              .reduceTransparency = value("appearance.reduce_transparency") == "true",
          },
      .increaseContrast = value("appearance.increase_contrast") == "true",
      .reduceMotion = value("appearance.reduce_motion") == "true",
      .animationEnabled = value("animation.enabled") != "false",
      .animationSpeed = speed,
      .blurEnabled = value("appearance.blur.enabled") == "true",
      .developerDiagnostics = value("general.developer_diagnostics") == "true",
      .titleBarButtonsLeft = value("appearance.title_bar.button_position") == "left",
  };
}

void applyLiveMotion(const LiveAppearance& appearance) {
  auto& motion = MotionService::instance();
  motion.setSpeed(appearance.animationSpeed);
  motion.setEnabled(appearance.animationEnabled);
  motion.setReduced(appearance.reduceMotion);
}

LiveAppearanceSync::LiveAppearanceSync() = default;

LiveAppearanceSync::~LiveAppearanceSync() = default;

void LiveAppearanceSync::initialize(ShellWaylandConnection& wayland, ChangeCallback onChange) {
  m_onChange = std::move(onChange);
  const auto [name, version] = wayland.desktopSettingsGlobal();
  if (!compositors::isAndromeda() || name == 0) {
    return;
  }
  m_control = std::make_unique<SettingsControl>(wayland.registry(), name, version, [this]() { onSettingsChanged(); });
}

void LiveAppearanceSync::publish(std::string_view scheme, SchemeOrigin origin, std::string_view paletteAccent) {
  if (m_focusScheme.empty()) {
    m_scheme = std::string(scheme);
    m_schemeOrigin = origin;
  }
  m_accent = normalizeColor(paletteAccent);
  flush();
}

void LiveAppearanceSync::set(const std::string& key, const std::string& value) {
  if (m_control != nullptr && m_control->ready()) {
    m_control->set(key, value);
  }
}

void LiveAppearanceSync::setAccentOrigin(AccentOrigin origin, std::string_view wallpaperAccent) {
  m_accentOrigin = origin;
  m_wallpaperAccent = normalizeColor(wallpaperAccent);
  flush();
}

void LiveAppearanceSync::setFocusScheme(std::string scheme) {
  m_focusScheme = std::move(scheme);
  flush();
}

void LiveAppearanceSync::setCornerScale(float scale) {
  m_cornerScale = std::format("{:g}", scale);
  flush();
}

void LiveAppearanceSync::setUiScale(float scale) {
  m_uiScale = std::format("{:g}", scale);
  flush();
}

void LiveAppearanceSync::setLanguage(std::string_view language) {
  m_language = language;
  flush();
}

void LiveAppearanceSync::flush() {
  if (m_control == nullptr || !m_control->ready()) {
    return;
  }
  if (!m_cornerScale.empty() && m_cornerScale != m_layerCornerScale) {
    m_layerCornerScale = m_cornerScale;
    m_control->setInLayer("style", "appearance.corner_scale", m_cornerScale);
  }
  if (!m_uiScale.empty() && m_uiScale != m_layerUiScale) {
    m_layerUiScale = m_uiScale;
    m_control->setInLayer("style", "appearance.ui_scale", m_uiScale);
  }
  if (!m_language.empty() && m_language != m_layerLanguage) {
    m_layerLanguage = m_language;
    m_control->setInLayer("style", "appearance.language", m_language);
  }
  // Blur recipe per scheme (glass tokens): the user's scheme's in the "style" layer, a Focus scheme's in its layer.
  const auto recipe = [](std::string_view scheme) {
    const bool light = scheme == "light";
    return std::array<std::pair<std::string, std::string>, 3>{{
        {"appearance.blur.saturation", std::format("{:g}", light ? Tokens::blurLightSaturation : Tokens::blurSaturation)},
        {"appearance.blur.brightness", std::format("{:g}", light ? Tokens::blurLightBrightness : Tokens::blurBrightness)},
        {"appearance.blur.contrast", std::format("{:g}", light ? Tokens::blurLightContrast : Tokens::blurContrast)},
    }};
  };
  if (m_focusScheme != m_layerScheme) {
    m_layerScheme = m_focusScheme;
    if (m_focusScheme.empty()) {
      // Our view still holds the layer's values until the compositor answers; pushing now would save them as the
      // user's. The answer flushes again.
      m_control->clearLayer("focus");
      return;
    }
    m_control->setInLayer("focus", "appearance.scheme", m_focusScheme);
    for (const auto& [key, value] : recipe(m_focusScheme)) {
      m_control->setInLayer("focus", key, value);
    }
  }
  if (m_scheme.empty()) {
    return; // publish() has not run yet
  }
  dropShellSaves(recipe(m_scheme));
  if (m_focusScheme.empty()) {
    if (m_schemeOrigin == SchemeOrigin::Derived) {
      if (m_scheme != m_layerDerivedScheme) {
        m_layerDerivedScheme = m_scheme;
        m_control->setInLayer("style", "appearance.scheme", m_scheme);
      }
    } else {
      // The layer masks the saved key, so set it before the layer goes: the view then never shows a stale saved
      // scheme, which would count as another client's choice.
      const bool masked = !m_layerDerivedScheme.empty();
      if (masked) {
        m_sent.erase("appearance.scheme");
      }
      push("appearance.scheme", m_scheme, masked || m_control->value("appearance.scheme") != m_scheme);
      if (masked) {
        m_layerDerivedScheme.clear();
        m_control->setInLayer("style", "appearance.scheme", "");
      }
    }
    // The blur recipe follows the scheme in the "style" layer; a value the user saved is never masked.
    for (const auto& [key, value] : recipe(m_scheme)) {
      setStyleLayer(key, m_control->customized(key) ? std::string{} : value);
    }
  }
  // The theme's accent fills in while the user saved none; a wallpaper accent replaces even a saved one.
  if (m_accentOrigin == AccentOrigin::Wallpaper) {
    setStyleLayer("appearance.accent", m_wallpaperAccent);
  } else {
    setStyleLayer("appearance.accent", m_control->customized("appearance.accent") ? std::string{} : m_accent);
  }
}

void LiveAppearanceSync::setStyleLayer(const std::string& key, const std::string& value) {
  auto [it, inserted] = m_styleLayer.try_emplace(key);
  if (!inserted && it->second == value) {
    return;
  }
  it->second = value;
  m_control->setInLayer("style", key, value);
}

// Earlier shells saved the theme accent and the blur recipe as if the user had chosen them, so Settings marked those
// rows changed and a reset was written back. Before the first flush no layer of ours masks the saved values: a saved
// value equal to the one the shell derives is ours, and dropping it changes nothing on screen.
void LiveAppearanceSync::dropShellSaves(const std::array<std::pair<std::string, std::string>, 3>& recipe) {
  if (m_droppedShellSaves) {
    return;
  }
  m_droppedShellSaves = true;
  if (!m_focusScheme.empty()) {
    return; // the Focus layer masks the saved values
  }
  const auto drop = [this](const std::string& key, const std::string& derived) {
    const std::string saved = m_control->value(key);
    if (!derived.empty() && m_control->customized(key) && (saved == derived || normalizeColor(saved) == derived)) {
      m_control->set(key, "");
    }
  };
  for (const auto& [key, value] : recipe) {
    drop(key, value);
  }
  if (m_accentOrigin != AccentOrigin::Wallpaper) {
    drop("appearance.accent", m_accent);
  }
}

// Sends each wanted value once: a refused set (failed event) notifies again, and resending would loop until the
// compositor's buffer to this client overflows.
void LiveAppearanceSync::push(const std::string& key, const std::string& value, bool differs) {
  if (!differs) {
    m_sent.erase(key);
    return;
  }
  if (const auto it = m_sent.find(key); it != m_sent.end() && it->second == value) {
    return;
  }
  m_sent.insert_or_assign(key, value);
  m_control->set(key, value);
}

void LiveAppearanceSync::onSettingsChanged() {
  if (!m_control->ready()) {
    return;
  }
  if (m_onBatch) {
    m_onBatch(*m_control);
  }
  const SettingsControl& c = *m_control;
  const LiveAppearance next = readLiveAppearance([&c](std::string_view key) { return c.value(key); });
  // A new value that the shell did not publish is another client's choice: adopt it instead of writing ours back.
  // The first snapshot can be stale from an earlier session, so only a change after it counts.
  const bool external = m_focusScheme.empty() && m_layerScheme.empty() && !m_state.scheme.empty() && next.scheme != m_state.scheme && next.scheme != m_scheme
      && (next.scheme == "light" || next.scheme == "dark");
  if (external) {
    m_scheme = next.scheme;
  }
  flush();
  if (external && m_onExternalScheme) {
    m_onExternalScheme(next.scheme);
  }
  if (next == m_state) {
    return;
  }
  m_state = next;
  if (m_onChange) {
    m_onChange(m_state);
  }
}
