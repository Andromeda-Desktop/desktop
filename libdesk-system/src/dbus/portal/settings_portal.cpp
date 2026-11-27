#include "dbus/portal/settings_portal.h"

#include "core/log.h"
#include "dbus/session_bus.h"

#include <chrono>
#include <map>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

  constexpr Logger kLog("settings-portal");

  const sdbus::ServiceName kBusName{"org.freedesktop.impl.portal.desktop.andromeda.shell"};
  const sdbus::ObjectPath kObjectPath{"/org/freedesktop/portal/desktop"};
  constexpr auto kInterface = "org.freedesktop.impl.portal.Settings";
  constexpr auto kAppearance = "org.freedesktop.appearance";
  constexpr auto kAndromeda = "org.andromeda.appearance";
  constexpr auto kWmPreferences = "org.gnome.desktop.wm.preferences";
  constexpr auto kTheme = "theme";
  constexpr std::uint32_t kVersion = 2;

  using Values = SettingsPortal::Values;
  using Value = std::variant<std::uint32_t, double, std::string, std::array<double, 3>>;

  struct Setting {
    const char* ns;
    const char* key;
    Value (*get)(const Values&);
  };

  constexpr Setting kSettings[] = {
      {kAppearance, "color-scheme", [](const Values& v) -> Value { return v.colorScheme; }},
      {kAppearance, "accent-color", [](const Values& v) -> Value { return v.accent; }},
      {kAppearance, "contrast", [](const Values& v) -> Value { return v.contrast; }},
      {kAppearance, "reduced-motion", [](const Values& v) -> Value { return v.reducedMotion; }},
      {kAndromeda, "ui-scale", [](const Values& v) -> Value { return v.uiScale; }},
      {kAndromeda, "text-scale", [](const Values& v) -> Value { return v.textScale; }},
      {kAndromeda, "corner-radius-scale", [](const Values& v) -> Value { return v.cornerRadiusScale; }},
      {kAndromeda, "glass", [](const Values& v) -> Value { return v.glass; }},
      {kAndromeda, "frost", [](const Values& v) -> Value { return v.frost; }},
      {kAndromeda, "liquid", [](const Values& v) -> Value { return v.liquid; }},
      {kAndromeda, "reduce-transparency", [](const Values& v) -> Value { return v.reduceTransparency; }},
      {kAndromeda, "compositor-blur", [](const Values& v) -> Value { return v.compositorBlur; }},
      {kAndromeda, "glass-style", [](const Values& v) -> Value { return v.glassStyle; }},
      {kAndromeda, "edge-reflection", [](const Values& v) -> Value { return v.edgeReflection; }},
      {kAndromeda, "liquid-tint", [](const Values& v) -> Value { return v.liquidTint; }},
      {kAndromeda, "liquid-blur", [](const Values& v) -> Value { return v.liquidBlur; }},
      {kAndromeda, "language", [](const Values& v) -> Value { return v.language; }},
      {kWmPreferences, "button-layout", [](const Values& v) -> Value { return v.buttonLayout; }},
  };

  static_assert(std::size(kSettings) <= 32, "SettingsPortal keeps one bit per key in a std::uint32_t");

  sdbus::Variant toVariant(const Value& value) {
    return std::visit(
        [](const auto& v) {
          if constexpr (std::is_same_v<std::decay_t<decltype(v)>, std::array<double, 3>>) {
            return sdbus::Variant{sdbus::Struct<double, double, double>{v[0], v[1], v[2]}};
          } else {
            return sdbus::Variant{v};
          }
        },
        value
    );
  }

  // A namespace pattern matches exactly, or as a prefix when it ends in '*'; no patterns means every namespace.
  bool wanted(const std::vector<std::string>& patterns, const std::string& ns) {
    if (patterns.empty()) {
      return true;
    }
    for (const std::string& pattern : patterns) {
      if (pattern == ns || (pattern.ends_with('*') && ns.starts_with(pattern.substr(0, pattern.size() - 1)))) {
        return true;
      }
    }
    return false;
  }

} // namespace

SettingsPortal::SettingsPortal(SessionBus& bus)
    : m_bus(bus),
      m_instance(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count()
      )) {
  m_object = sdbus::createObject(m_bus.connection(), kObjectPath);
  m_object
      ->addVTable(
          sdbus::registerMethod("ReadAll")
              .withInputParamNames("namespaces")
              .withOutputParamNames("value")
              .implementedAs([this](const std::vector<std::string>& namespaces) {
                std::map<std::string, std::map<std::string, sdbus::Variant>> all;
                for (const Setting& s : kSettings) {
                  if (wanted(namespaces, s.ns)) {
                    all[s.ns][s.key] = toVariant(s.get(m_published));
                  }
                }
                if (wanted(namespaces, kAndromeda)) {
                  all[kAndromeda][kTheme] = themeVariant();
                }
                return all;
              }),
          sdbus::registerMethod("Read")
              .withInputParamNames("namespace", "key")
              .withOutputParamNames("value")
              .implementedAs([this](const std::string& ns, const std::string& key) {
                if (ns == kAndromeda && key == kTheme) {
                  return themeVariant();
                }
                for (const Setting& s : kSettings) {
                  if (ns == s.ns && key == s.key) {
                    return toVariant(s.get(m_published));
                  }
                }
                throw sdbus::Error(
                    sdbus::Error::Name{"org.freedesktop.portal.Error.NotFound"}, "Requested setting not found"
                );
              }),
          sdbus::registerSignal("SettingChanged")
              .withParameters<std::string, std::string, sdbus::Variant>("namespace", "key", "value"),
          sdbus::registerProperty("version").withGetter([]() { return kVersion; })
      )
      .forInterface(kInterface);
  m_bus.connection().requestName(kBusName);
  kLog.info("serving {} as {}", kInterface, static_cast<const std::string&>(kBusName));
}

SettingsPortal::~SettingsPortal() {
  try {
    m_bus.connection().releaseName(kBusName);
  } catch (const sdbus::Error& e) {
    kLog.debug("release {} failed: {}", static_cast<const std::string&>(kBusName), e.what());
  }
}

void SettingsPortal::stage(std::string_view key) {
  for (std::size_t i = 0; i < std::size(kSettings); ++i) {
    if (key == kSettings[i].key) {
      m_stagedKeys |= 1U << i;
    }
  }
}

void SettingsPortal::setDark(bool dark) {
  m_staged.colorScheme = dark ? 1 : 2;
  stage("color-scheme");
}

void SettingsPortal::setAccent(double r, double g, double b) {
  m_staged.accent = {r, g, b};
  stage("accent-color");
}

void SettingsPortal::setHighContrast(bool high) {
  m_staged.contrast = high ? 1 : 0;
  stage("contrast");
}

void SettingsPortal::setReducedMotion(bool reduce) {
  m_staged.reducedMotion = reduce ? 1 : 0;
  stage("reduced-motion");
}

void SettingsPortal::setLanguage(const std::string& language) {
  m_staged.language = language;
  stage("language");
}

void SettingsPortal::setButtonsLeft(bool left) {
  m_staged.buttonLayout = left ? "close,minimize,maximize:" : ":minimize,maximize,close";
  stage("button-layout");
}

void SettingsPortal::setScales(double uiScale, double textScale, double cornerRadiusScale) {
  m_staged.uiScale = uiScale;
  m_staged.textScale = textScale;
  m_staged.cornerRadiusScale = cornerRadiusScale;
  for (const char* key : {"ui-scale", "text-scale", "corner-radius-scale"}) {
    stage(key);
  }
}

void SettingsPortal::setGlass(double glass, double frost, double liquid, bool reduceTransparency, bool compositorBlur) {
  m_staged.glass = glass;
  m_staged.frost = frost;
  m_staged.liquid = liquid;
  m_staged.reduceTransparency = reduceTransparency ? 1 : 0;
  m_staged.compositorBlur = compositorBlur ? 1 : 0;
  for (const char* key : {"glass", "frost", "liquid", "reduce-transparency", "compositor-blur"}) {
    stage(key);
  }
}

void SettingsPortal::setGlassStyle(std::uint32_t style, bool edgeReflection, double liquidTint, double liquidBlur) {
  m_staged.glassStyle = style;
  m_staged.edgeReflection = edgeReflection ? 1 : 0;
  m_staged.liquidTint = liquidTint;
  m_staged.liquidBlur = liquidBlur;
  for (const char* key : {"glass-style", "edge-reflection", "liquid-tint", "liquid-blur"}) {
    stage(key);
  }
}

void SettingsPortal::publish() {
  if (m_staged == m_published && m_stagedKeys == m_publishedKeys) {
    return;
  }
  const Values previous = std::exchange(m_published, m_staged);
  m_publishedKeys = m_stagedKeys;
  ++m_generation;
  for (const Setting& s : kSettings) {
    if (s.get(previous) != s.get(m_published)) {
      emitChanged(s.ns, s.key, toVariant(s.get(m_published)));
    }
  }
  emitChanged(kAndromeda, kTheme, themeVariant());
}

sdbus::Variant SettingsPortal::themeVariant() const {
  std::map<std::string, std::map<std::string, sdbus::Variant>> byNamespace;
  for (std::size_t i = 0; i < std::size(kSettings); ++i) {
    const Setting& s = kSettings[i];
    if ((m_publishedKeys & (1U << i)) != 0 && std::string_view(s.ns) != kWmPreferences) {
      byNamespace[s.ns][s.key] = toVariant(s.get(m_published));
    }
  }
  std::map<std::string, sdbus::Variant> theme;
  theme["instance"] = sdbus::Variant{m_instance};
  theme["generation"] = sdbus::Variant{m_generation};
  for (auto& [ns, values] : byNamespace) {
    theme[ns] = sdbus::Variant{std::move(values)};
  }
  return sdbus::Variant{std::move(theme)};
}

void SettingsPortal::emitChanged(const char* ns, const char* key, const sdbus::Variant& value) {
  m_object->emitSignal("SettingChanged").onInterface(kInterface).withArguments(std::string(ns), std::string(key), value);
}
