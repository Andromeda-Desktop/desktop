#include "server/apps_bus.h"
#include "protocols/bus_names.h"

#include "config/config.h"
#include "config/managed_settings.h"
#include "config/resolve.h"
#include "core/log.h"
#include "qos/holds.h"

#include <wayland-server-core.h>

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

#include <systemd/sd-bus.h>

namespace andromeda {

  namespace {
    constexpr Logger kLog("apps-bus");
    // A headless E2E compositor owns another name while the live session holds the real one.
    const char* busName() {
      const char* name = std::getenv("ANDROMEDA_COMPOSITOR_BUS_NAME");
      return name != nullptr && name[0] != '\0' ? name : "org.andromeda.Compositor1";
    }
    constexpr const char* kPath = "/org/andromeda/Compositor1/Apps";
    constexpr const char* kInterface = "org.andromeda.Compositor1.Apps";
    constexpr const char* kAppRow = "(sbbsbdt)";
    constexpr int kSampleMs = 30'000;
    constexpr std::uint64_t kWatchUs = 120'000'000;

    std::uint64_t monotonicUs() {
      timespec now{};
      clock_gettime(CLOCK_MONOTONIC, &now);
      return static_cast<std::uint64_t>(now.tv_sec) * 1'000'000 + static_cast<std::uint64_t>(now.tv_nsec) / 1000;
    }

    const char* tierName(qos::Tier tier) {
      switch (tier) {
      case qos::Tier::Focus:
        return "T0";
      case qos::Tier::Visible:
        return "T1";
      case qos::Tier::Hidden:
        return "T2";
      case qos::Tier::HiddenLong:
        return "T3";
      }
      return "?";
    }

    std::optional<qos::Tier> tierFromName(std::string_view name) {
      if (name == "T1") {
        return qos::Tier::Visible;
      }
      if (name == "T2") {
        return qos::Tier::Hidden;
      }
      if (name == "T3") {
        return qos::Tier::HiddenLong;
      }
      return std::nullopt;
    }

    std::optional<std::uint64_t> readUsageUs(const std::string& dir) {
      std::ifstream in(dir + "/cpu.stat");
      std::string key;
      std::uint64_t value = 0;
      while (in >> key >> value) {
        if (key == "usage_usec") {
          return value;
        }
      }
      return std::nullopt;
    }

    std::uint64_t readRss(const std::string& dir) {
      std::ifstream in(dir + "/memory.current");
      std::uint64_t bytes = 0;
      in >> bytes;
      return bytes;
    }
  } // namespace

  AppsBus::AppsBus(wl_event_loop* loop, std::function<void()> reloadConfig, std::function<void()> refreshCapChanged)
      : m_reloadConfig(std::move(reloadConfig)), m_refreshCapChanged(std::move(refreshCapChanged)),
        m_bus(loop, bus::Kind::User) {
    if (m_bus.get() == nullptr) {
      kLog.info("no session bus: org.andromeda.Compositor1.Apps is off");
    }
    m_sampleTimer = loop != nullptr ? wl_event_loop_add_timer(loop, onSample, this) : nullptr;
    m_bus.onChange([this](sd_bus* bus) {
      if (bus != nullptr) {
        publish();
      }
    });
    if (m_bus.get() != nullptr) {
      publish();
    }
  }

  AppsBus::~AppsBus() {
    if (m_sampleTimer != nullptr) {
      wl_event_source_remove(m_sampleTimer);
    }
  }

  // Names that left while the bus was gone are stale: ListNames fills the set again.
  void AppsBus::publish() {
    static const sd_bus_vtable kVtable[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_METHOD("HoldTier", "sss", "s", onHoldTier, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("HoldFreeze", "ss", "s", onHoldFreeze, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("SetAppRule", "ss", "", onSetAppRule, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("AppRules", "", "a(ss)", onAppRules, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("BuiltInAppRules", "", "a(ss)", onBuiltInAppRules, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("SetHiddenFpsCap", "u", "", onSetHiddenFpsCap, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("SetRefreshCap", "u", "", onSetRefreshCap, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("SetFreezeHidden", "b", "", onSetFreezeHidden, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("Release", "s", "", onRelease, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("List", "", "a(sbbsbdt)", onList, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_SIGNAL("AppStats", "a(sbbsbdt)", 0),
        SD_BUS_PROPERTY("Presenting", "b", getPresenting, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("Idle", "b", getIdle, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("Activity", "s", getActivity, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_VTABLE_END,
    };
    const auto named = [](sd_bus_message* reply, void* /*data*/, sd_bus_error* /*error*/) -> int {
      if (sd_bus_message_is_method_error(reply, nullptr) != 0) {
        kLog.info("{} is taken: Compositor1.Apps is off in this compositor", busName());
      }
      return 0;
    };
    sd_bus* bus = m_bus.get();
    qos::registry().busNames.clear();
    sd_bus_message* list = nullptr;
    const bool published =
        sd_bus_add_object_vtable(bus, m_bus.keep(), kPath, kInterface, kVtable, this) >= 0
        && sd_bus_match_signal_async(bus, m_bus.keep(), busname::kDaemon, busname::kDaemonPath,
                                     busname::kDaemon, "NameOwnerChanged", onOwnerChanged, nullptr, this)
            >= 0
        && sd_bus_message_new_method_call(bus, &list, busname::kDaemon, busname::kDaemonPath,
                                          busname::kDaemon, "ListNames") >= 0
        && m_bus.call(list, [this](sd_bus_message* reply) {
             if (reply != nullptr) {
               onNames(reply, this, nullptr);
             }
           })
        && sd_bus_request_name_async(bus, m_bus.keep(), busName(), 0, named, nullptr) >= 0;
    sd_bus_message_unref(list);
    if (!published) {
      kLog.warn("Compositor1.Apps: could not set up the bus object");
    }
    m_bus.sent();
  }

  void AppsBus::changed() {
    if (const auto& callback = qos::registry().changed) {
      callback();
    }
  }

  void AppsBus::watch() {
    m_watchedUntilUs = monotonicUs() + kWatchUs;
    if (!m_sampling) {
      m_sampling = true;
      sample();
      wl_event_source_timer_update(m_sampleTimer, kSampleMs);
    }
  }

  void AppsBus::setPresenting(bool presenting) {
    if (presenting == m_presenting) {
      return;
    }
    m_presenting = presenting;
    if (m_bus.get() != nullptr) {
      (void)sd_bus_emit_properties_changed(m_bus.get(), kPath, kInterface, "Presenting", nullptr);
      m_bus.sent();
    }
  }

  void AppsBus::setIdle(bool idle) {
    if (idle == m_idle) {
      return;
    }
    m_idle = idle;
    if (m_bus.get() != nullptr) {
      (void)sd_bus_emit_properties_changed(m_bus.get(), kPath, kInterface, "Idle", nullptr);
      m_bus.sent();
    }
  }

  void AppsBus::setActivity(std::string_view activity) {
    if (activity == m_activity) {
      return;
    }
    m_activity = activity;
    if (m_bus.get() != nullptr) {
      (void)sd_bus_emit_properties_changed(m_bus.get(), kPath, kInterface, "Activity", nullptr);
      m_bus.sent();
    }
  }

  int AppsBus::getActivity(sd_bus* /*bus*/, const char* /*path*/, const char* /*interface*/,
                           const char* /*property*/, sd_bus_message* reply, void* data, sd_bus_error* /*error*/) {
    return sd_bus_message_append(reply, "s", std::string(static_cast<AppsBus*>(data)->m_activity).c_str());
  }

  int AppsBus::getIdle(sd_bus* /*bus*/, const char* /*path*/, const char* /*interface*/, const char* /*property*/,
                       sd_bus_message* reply, void* data, sd_bus_error* /*error*/) {
    return sd_bus_message_append(reply, "b", static_cast<int>(static_cast<AppsBus*>(data)->m_idle));
  }

  int AppsBus::getPresenting(sd_bus* /*bus*/, const char* /*path*/, const char* /*interface*/,
                             const char* /*property*/, sd_bus_message* reply, void* data, sd_bus_error* /*error*/) {
    return sd_bus_message_append(reply, "b", static_cast<int>(static_cast<AppsBus*>(data)->m_presenting));
  }

  int AppsBus::onSample(void* data) {
    auto* self = static_cast<AppsBus*>(data);
    if (self->m_bus.get() == nullptr) {
      return 0;
    }
    if (qos::registry().holds.empty() && monotonicUs() > self->m_watchedUntilUs) {
      self->m_sampling = false;
      self->m_samples.clear();
      return 0;
    }
    self->sample();
    sd_bus_message* signal = nullptr;
    if (sd_bus_message_new_signal(self->m_bus.get(), &signal, kPath, kInterface, "AppStats") >= 0
        && self->appendApps(signal) >= 0) {
      (void)sd_bus_send(self->m_bus.get(), signal, nullptr);
    }
    sd_bus_message_unref(signal);
    self->m_bus.sent();
    wl_event_source_timer_update(self->m_sampleTimer, kSampleMs);
    return 0;
  }

  void AppsBus::sample() {
    const auto& apps = qos::registry().apps;
    std::erase_if(m_samples, [&](const auto& entry) { return !apps.contains(entry.first); });
    const std::uint64_t now = monotonicUs();
    for (const auto& [unit, app] : apps) {
      const std::optional<std::uint64_t> usage = readUsageUs(app.dir);
      if (!usage) {
        continue;
      }
      Sample& last = m_samples[unit];
      if (last.atUs != 0 && now > last.atUs && *usage >= last.usageUs) {
        last.cpuPct = 100.0 * static_cast<double>(*usage - last.usageUs) / static_cast<double>(now - last.atUs);
      }
      last.usageUs = *usage;
      last.atUs = now;
      last.rssBytes = readRss(app.dir);
    }
  }

  int AppsBus::appendApps(sd_bus_message* message) {
    int r = sd_bus_message_open_container(message, 'a', kAppRow);
    for (const auto& [unit, app] : qos::registry().apps) {
      const auto it = m_samples.find(unit);
      const double cpu = it != m_samples.end() ? it->second.cpuPct : 0.0;
      const std::uint64_t rss = it != m_samples.end() ? it->second.rssBytes : 0;
      if (r >= 0) {
        r = sd_bus_message_append(message, kAppRow, unit.c_str(), static_cast<int>(app.visible),
                                  static_cast<int>(app.focused), tierName(app.tier), static_cast<int>(app.frozen), cpu,
                                  rss);
      }
    }
    return r < 0 ? r : sd_bus_message_close_container(message);
  }

  int AppsBus::onHoldTier(sd_bus_message* message, void* data, sd_bus_error* error) {
    auto* self = static_cast<AppsBus*>(data);
    const char* unit = nullptr;
    const char* tierText = nullptr;
    const char* reason = nullptr;
    if (const int r = sd_bus_message_read(message, "sss", &unit, &tierText, &reason); r < 0) {
      return r;
    }
    const std::optional<qos::Tier> tier = tierFromName(tierText);
    if (!tier) {
      return sd_bus_error_set(error, SD_BUS_ERROR_INVALID_ARGS, "tier must be T1, T2 or T3");
    }
    qos::Registry& registry = qos::registry();
    const std::optional<std::string> id =
        registry.holds.holdTier(sd_bus_message_get_sender(message), unit, *tier, reason, registry.apps.contains(unit));
    if (!id) {
      return sd_bus_error_setf(error, "org.andromeda.Compositor1.Error.UnknownApp", "%s is not an app scope", unit);
    }
    self->watch();
    self->changed();
    return sd_bus_reply_method_return(message, "s", id->c_str());
  }

  int AppsBus::onHoldFreeze(sd_bus_message* message, void* data, sd_bus_error* error) {
    auto* self = static_cast<AppsBus*>(data);
    const char* unit = nullptr;
    const char* reason = nullptr;
    if (const int r = sd_bus_message_read(message, "ss", &unit, &reason); r < 0) {
      return r;
    }
    qos::Registry& registry = qos::registry();
    const std::optional<std::string> id =
        registry.holds.holdFreeze(sd_bus_message_get_sender(message), unit, reason, registry.apps.contains(unit));
    if (!id) {
      return sd_bus_error_setf(error, "org.andromeda.Compositor1.Error.UnknownApp", "%s is not an app scope", unit);
    }
    self->watch();
    self->changed();
    return sd_bus_reply_method_return(message, "s", id->c_str());
  }

  // An app's CPU rule from Settings (docs/qos.md §3): normal, never_throttle or freeze_when_hidden, saved as a
  // [[window_rule]] in the settings file.
  int AppsBus::onSetAppRule(sd_bus_message* message, void* data, sd_bus_error* error) {
    auto* self = static_cast<AppsBus*>(data);
    const char* appId = nullptr;
    const char* rule = nullptr;
    if (const int r = sd_bus_message_read(message, "ss", &appId, &rule); r < 0) {
      return r;
    }
    const std::string_view qos = rule;
    if (*appId == '\0' || (qos != "normal" && qos != "never_throttle" && qos != "freeze_when_hidden")) {
      return sd_bus_error_set(error, SD_BUS_ERROR_INVALID_ARGS, "an app id and normal, never_throttle or freeze_when_hidden");
    }
    const std::filesystem::path file = managedSettingsFile(configRootPath());
    if (!saveManagedAppQos(file, appId, qos)) {
      return sd_bus_error_setf(error, SD_BUS_ERROR_IO_ERROR, "cannot write %s", file.c_str());
    }
    if (self->m_reloadConfig) {
      self->m_reloadConfig();
    }
    return sd_bus_reply_method_return(message, "");
  }

  int AppsBus::onAppRules(sd_bus_message* message, void* /*data*/, sd_bus_error* /*error*/) {
    std::ifstream in(managedSettingsFile(configRootPath()));
    const std::string document{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    sd_bus_message* reply = nullptr;
    int r = sd_bus_message_new_method_return(message, &reply);
    r = r < 0 ? r : sd_bus_message_open_container(reply, 'a', "(ss)");
    for (const auto& [app, qos] : documentAppQos(document)) {
      r = r < 0 ? r : sd_bus_message_append(reply, "(ss)", app.c_str(), qos.c_str());
    }
    r = r < 0 ? r : sd_bus_message_close_container(reply);
    r = r < 0 ? r : sd_bus_send(nullptr, reply, nullptr);
    sd_bus_message_unref(reply);
    return r;
  }

  // (app id regex, rule): what an app with no AppRules entry gets.
  int AppsBus::onBuiltInAppRules(sd_bus_message* message, void* /*data*/, sd_bus_error* /*error*/) {
    sd_bus_message* reply = nullptr;
    int r = sd_bus_message_new_method_return(message, &reply);
    r = r < 0 ? r : sd_bus_message_open_container(reply, 'a', "(ss)");
    for (const BuiltInQosRule& rule : builtInQosRules()) {
      const std::string pattern(rule.appIdPattern);
      const std::string name(rule.name);
      r = r < 0 ? r : sd_bus_message_append(reply, "(ss)", pattern.c_str(), name.c_str());
    }
    r = r < 0 ? r : sd_bus_message_close_container(reply);
    r = r < 0 ? r : sd_bus_send(nullptr, reply, nullptr);
    sd_bus_message_unref(reply);
    return r;
  }

  int AppsBus::onSetRefreshCap(sd_bus_message* message, void* data, sd_bus_error* /*error*/) {
    auto* self = static_cast<AppsBus*>(data);
    std::uint32_t hz = 0;
    if (const int r = sd_bus_message_read(message, "u", &hz); r < 0) {
      return r;
    }
    qos::Registry& registry = qos::registry();
    const bool changed = registry.refreshCap != hz;
    registry.refreshCap = hz;
    registry.refreshCapOwner = hz == 0 ? "" : sd_bus_message_get_sender(message);
    if (changed && self->m_refreshCapChanged) {
      self->m_refreshCapChanged();
    }
    return sd_bus_reply_method_return(message, "");
  }

  int AppsBus::onSetHiddenFpsCap(sd_bus_message* message, void* /*data*/, sd_bus_error* /*error*/) {
    std::uint32_t hz = 0;
    if (const int r = sd_bus_message_read(message, "u", &hz); r < 0) {
      return r;
    }
    qos::Registry& registry = qos::registry();
    registry.hiddenFpsCap = hz;
    registry.fpsCapOwner = hz == 0 ? "" : sd_bus_message_get_sender(message);
    return sd_bus_reply_method_return(message, "");
  }

  int AppsBus::onSetFreezeHidden(sd_bus_message* message, void* data, sd_bus_error* /*error*/) {
    auto* self = static_cast<AppsBus*>(data);
    int on = 0;
    if (const int r = sd_bus_message_read(message, "b", &on); r < 0) {
      return r;
    }
    qos::Registry& registry = qos::registry();
    registry.freezeHidden = on != 0;
    registry.freezeHiddenOwner = on != 0 ? sd_bus_message_get_sender(message) : "";
    self->changed();
    return sd_bus_reply_method_return(message, "");
  }

  int AppsBus::onRelease(sd_bus_message* message, void* data, sd_bus_error* error) {
    auto* self = static_cast<AppsBus*>(data);
    const char* id = nullptr;
    if (const int r = sd_bus_message_read(message, "s", &id); r < 0) {
      return r;
    }
    if (!qos::registry().holds.release(id, sd_bus_message_get_sender(message))) {
      return sd_bus_error_setf(error, "org.andromeda.Compositor1.Error.NoHold", "no hold %s of this caller", id);
    }
    self->changed();
    return sd_bus_reply_method_return(message, "");
  }

  int AppsBus::onList(sd_bus_message* message, void* data, sd_bus_error* /*error*/) {
    auto* self = static_cast<AppsBus*>(data);
    self->watch();
    sd_bus_message* reply = nullptr;
    int r = sd_bus_message_new_method_return(message, &reply);
    if (r >= 0) {
      r = self->appendApps(reply);
    }
    if (r >= 0) {
      r = sd_bus_send(nullptr, reply, nullptr);
    }
    sd_bus_message_unref(reply);
    return r;
  }

  int AppsBus::onNames(sd_bus_message* reply, void* data, sd_bus_error* /*error*/) {
    if (sd_bus_message_is_method_error(reply, nullptr) != 0 || sd_bus_message_enter_container(reply, 'a', "s") < 0) {
      return 0;
    }
    const char* name = nullptr;
    while (sd_bus_message_read(reply, "s", &name) > 0) {
      if (name[0] != ':') {
        qos::registry().busNames.emplace(name);
      }
    }
    static_cast<AppsBus*>(data)->changed();
    return 0;
  }

  int AppsBus::onOwnerChanged(sd_bus_message* message, void* data, sd_bus_error* /*error*/) {
    auto* self = static_cast<AppsBus*>(data);
    const char* name = nullptr;
    const char* oldOwner = nullptr;
    const char* newOwner = nullptr;
    if (sd_bus_message_read(message, "sss", &name, &oldOwner, &newOwner) < 0) {
      return 0;
    }
    if (name[0] != ':') {
      if (newOwner[0] == '\0') {
        qos::registry().busNames.erase(name);
      } else {
        qos::registry().busNames.emplace(name);
      }
      self->changed();
    }
    if (name[0] == ':' && newOwner[0] == '\0' && qos::registry().holds.releaseOwner(name)) {
      self->changed();
    }
    if (name[0] == ':' && newOwner[0] == '\0' && qos::registry().refreshCapOwner == name) {
      qos::registry().refreshCap = 0;
      qos::registry().refreshCapOwner.clear();
      if (self->m_refreshCapChanged) {
        self->m_refreshCapChanged();
      }
    }
    if (name[0] == ':' && newOwner[0] == '\0' && qos::registry().freezeHiddenOwner == name) {
      qos::registry().freezeHidden = false;
      qos::registry().freezeHiddenOwner.clear();
      self->changed();
    }
    if (name[0] == ':' && newOwner[0] == '\0' && qos::registry().fpsCapOwner == name) {
      qos::registry().hiddenFpsCap = 0;
      qos::registry().fpsCapOwner.clear();
    }
    return 0;
  }

} // namespace andromeda
