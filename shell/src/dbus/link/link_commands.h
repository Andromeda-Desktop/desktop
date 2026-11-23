#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>

// The last system command this shell sent a phone, until it succeeds; a phone with none has no entry.
struct LinkCommandState {
  enum class Phase : std::uint8_t { Pending, Failed };
  Phase phase = Phase::Pending;
  std::string command;
  std::uint32_t value = 0;
  // The daemon's or the phone's reason, while Failed.
  std::string error;
  // Matches a reply to the command it answers.
  std::uint64_t serial = 0;
};

// Each phone's last system command. A send makes it Pending, replacing any other; its error reply makes it Failed,
// its success removes it, and a dismiss removes a Failed one. A reply to a replaced command changes nothing.
class LinkCommands {
public:
  enum class Answer : std::uint8_t { Stale, Failed, Succeeded };

  std::uint64_t send(const std::string& device, std::string command, std::uint32_t value) {
    const std::uint64_t serial = ++m_serial;
    m_states[device] = LinkCommandState{.command = std::move(command), .value = value, .serial = serial};
    return serial;
  }

  // On Succeeded, `done` gets the command, so its value can stand until the phone's status confirms it.
  Answer answered(
      const std::string& device, std::uint64_t serial, std::optional<std::string> error, LinkCommandState* done = nullptr
  ) {
    const auto it = m_states.find(device);
    if (it == m_states.end() || it->second.serial != serial || it->second.phase != LinkCommandState::Phase::Pending) {
      return Answer::Stale;
    }
    if (error.has_value()) {
      it->second.phase = LinkCommandState::Phase::Failed;
      it->second.error = std::move(*error);
      return Answer::Failed;
    }
    if (done != nullptr) {
      *done = std::move(it->second);
    }
    m_states.erase(it);
    return Answer::Succeeded;
  }

  // True when it dropped a Failed command.
  bool dismiss(const std::string& device) {
    const auto it = m_states.find(device);
    if (it == m_states.end() || it->second.phase != LinkCommandState::Phase::Failed) {
      return false;
    }
    m_states.erase(it);
    return true;
  }

  void clear() { m_states.clear(); }

  [[nodiscard]] const LinkCommandState* state(const std::string& device) const {
    const auto it = m_states.find(device);
    return it != m_states.end() ? &it->second : nullptr;
  }

private:
  std::map<std::string, LinkCommandState> m_states;
  std::uint64_t m_serial = 0;
};
