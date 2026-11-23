#include "core/log.h"
#include "core/process/process.h"
#include "dbus/link/link_service.h"

#include <sdbus-c++/IProxy.h>

namespace {

  constexpr Logger kLog("link");
  constexpr auto kLinkInterface = "org.andromeda.Link1";

} // namespace

void LinkService::applyDeviceState(const std::map<std::string, sdbus::Variant>& properties) {
  if (const auto it = properties.find("DeviceInfo"); it != properties.end()) {
    try {
      m_info.clear();
      for (const auto& [id, fields] : it->second.get<std::map<std::string, std::map<std::string, sdbus::Variant>>>()) {
        LinkDeviceInfo& info = m_info[id];
        if (const auto seen = fields.find("last_seen"); seen != fields.end()) {
          info.lastSeen = seen->second.get<std::int64_t>();
        }
        if (const auto transport = fields.find("transport"); transport != fields.end()) {
          info.transport = transport->second.get<std::string>();
        }
      }
    } catch (const sdbus::Error& e) {
      kLog.warn("malformed DeviceInfo: {}", e.what());
    }
  }
  if (const auto it = properties.find("DeviceControls"); it != properties.end()) {
    try {
      m_controls.clear();
      using Controls = sdbus::Struct<std::int32_t, std::string, std::int32_t, std::int32_t>;
      for (const auto& [id, controls] : it->second.get<std::map<std::string, Controls>>()) {
        m_controls[id] = LinkControls{
            .mediaVolume = controls.get<0>(),
            .ringer = controls.get<1>(),
            .dnd = controls.get<2>(),
        };
      }
    } catch (const sdbus::Error& e) {
      kLog.warn("malformed DeviceControls: {}", e.what());
    }
  }
  if (const auto it = properties.find("DeviceToggles"); it != properties.end()) {
    try {
      m_toggles.clear();
      using Toggles = sdbus::Struct<std::int32_t, std::int32_t, std::int32_t>;
      for (const auto& [id, toggles] : it->second.get<std::map<std::string, Toggles>>()) {
        m_toggles[id] = LinkToggles{.wifi = toggles.get<0>(), .bluetooth = toggles.get<1>(), .torch = toggles.get<2>()};
      }
    } catch (const sdbus::Error& e) {
      kLog.warn("malformed DeviceToggles: {}", e.what());
    }
  }
}

const LinkDeviceInfo* LinkService::info(const std::string& deviceId) const {
  const auto it = m_info.find(deviceId);
  return it != m_info.end() ? &it->second : nullptr;
}

const LinkControls* LinkService::controls(const std::string& deviceId) const {
  const auto it = m_controls.find(deviceId);
  return it != m_controls.end() ? &it->second : nullptr;
}

const LinkToggles* LinkService::toggles(const std::string& deviceId) const {
  const auto it = m_toggles.find(deviceId);
  return it != m_toggles.end() ? &it->second : nullptr;
}

const LinkCommandState* LinkService::commandState(const std::string& deviceId) const {
  const auto it = m_commandStates.find(deviceId);
  return it != m_commandStates.end() ? &it->second : nullptr;
}

void LinkService::dismissCommandError(const std::string& deviceId) {
  const auto it = m_commandStates.find(deviceId);
  if (it != m_commandStates.end() && it->second.phase == LinkCommandState::Phase::Failed) {
    m_commandStates.erase(it);
    notify();
  }
}

void LinkService::sendSystemCommand(const std::string& deviceId, const std::string& command, std::uint32_t value) {
  const std::uint64_t serial = ++m_commandSerial;
  m_commandStates[deviceId] = LinkCommandState{.command = command, .value = value, .serial = serial};
  notify();
  m_link->callMethodAsync("SendSystemCommand")
      .onInterface(kLinkInterface)
      .withArguments(deviceId, command, value)
      .uponReplyInvoke([this, deviceId, serial](std::optional<sdbus::Error> error) {
        commandAnswered(deviceId, serial, error.has_value() ? std::optional(error->getMessage()) : std::nullopt);
      });
}

void LinkService::commandAnswered(const std::string& deviceId, std::uint64_t serial, std::optional<std::string> error) {
  const auto it = m_commandStates.find(deviceId);
  // A newer command, or a disconnect, replaced this one.
  if (it == m_commandStates.end() || it->second.serial != serial ||
      it->second.phase != LinkCommandState::Phase::Pending) {
    return;
  }
  if (error.has_value()) {
    kLog.warn("SendSystemCommand {} failed: {}", it->second.command, *error);
    it->second.phase = LinkCommandState::Phase::Failed;
    it->second.error = std::move(*error);
    notify();
    return;
  }
  // The phone ran it; its next status confirms it, and this keeps the button from flicking back until then.
  const auto value = static_cast<std::int32_t>(it->second.value);
  const std::string& command = it->second.command;
  if (const auto toggles = m_toggles.find(deviceId); toggles != m_toggles.end()) {
    if (command == "set-wifi") {
      toggles->second.wifi = value;
    } else if (command == "set-bluetooth") {
      toggles->second.bluetooth = value;
    } else if (command == "set-torch") {
      toggles->second.torch = value;
    }
  }
  if (const auto controls = m_controls.find(deviceId); controls != m_controls.end() && command == "set-dnd") {
    controls->second.dnd = value;
  }
  m_commandStates.erase(it);
  notify();
}

void LinkService::openApp() {
  if (!process::runAsync(std::vector<std::string>{"andromeda-link-app"})) {
    kLog.warn("could not start andromeda-link-app");
  }
}
