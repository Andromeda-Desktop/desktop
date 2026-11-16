#include "session_layout.h"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace term {

  SavedWindow saveLayout(const SessionList& list, const std::function<std::string(std::uint64_t)>& cwdOf) {
    SavedWindow saved;
    std::vector<std::uint64_t> ids;
    for (const SessionInfo& info : list.sessions()) {
      if (info.kind == SessionKind::Shell) {
        ids.push_back(info.id);
        saved.sessions.push_back({cwdOf(info.id), info.name});
      }
    }
    const auto indexOf = [&ids](std::uint64_t id) {
      return static_cast<std::size_t>(std::ranges::find(ids, id) - ids.begin());
    };
    for (const auto& split : list.splits()) {
      std::vector<std::size_t> panes;
      for (const std::uint64_t id : split) {
        if (const std::size_t i = indexOf(id); i < ids.size()) {
          panes.push_back(i);
        }
      }
      if (panes.size() >= 2) {
        saved.splits.push_back(std::move(panes));
      }
    }
    // recent() starts with the shown session; an unsaved one gives way to the most recent saved one.
    for (const std::uint64_t id : list.recent()) {
      if (const std::size_t i = indexOf(id); i < ids.size()) {
        saved.shown = i;
        break;
      }
    }
    return saved;
  }

  std::vector<SessionListEvent> restoreEvents(const SavedWindow& saved, std::span<const std::uint64_t> ids) {
    std::vector<SessionListEvent> events;
    for (std::size_t i = 0; i < saved.sessions.size() && i < ids.size(); ++i) {
      if (!saved.sessions[i].name.empty()) {
        events.emplace_back(session_event::Rename{ids[i], saved.sessions[i].name});
      }
    }
    for (const auto& split : saved.splits) {
      for (std::size_t k = 1; k < split.size(); ++k) {
        events.emplace_back(session_event::Join{.target = ids[split[k - 1]], .joining = ids[split[k]]});
      }
    }
    if (saved.shown < ids.size()) {
      events.emplace_back(session_event::Show{ids[saved.shown]});
    }
    return events;
  }

  nlohmann::json toJson(const std::vector<SavedWindow>& windows) {
    nlohmann::json out = nlohmann::json::array();
    for (const SavedWindow& window : windows) {
      nlohmann::json sessions = nlohmann::json::array();
      for (const SavedSession& session : window.sessions) {
        sessions.push_back({{"cwd", session.cwd}, {"name", session.name}});
      }
      out.push_back({{"sessions", std::move(sessions)}, {"splits", window.splits}, {"shown", window.shown}});
    }
    return out;
  }

  std::vector<SavedWindow> windowsFromJson(const nlohmann::json& json) {
    std::vector<SavedWindow> out;
    if (!json.is_array()) {
      return out;
    }
    for (const nlohmann::json& w : json) {
      if (!w.is_object() || !w.contains("sessions") || !w["sessions"].is_array()) {
        continue;
      }
      SavedWindow window;
      for (const nlohmann::json& s : w["sessions"]) {
        if (s.is_object()) {
          window.sessions.push_back({s.value("cwd", std::string()), s.value("name", std::string())});
        }
      }
      if (window.sessions.empty()) {
        continue;
      }
      const std::size_t count = window.sessions.size();
      if (w.contains("splits") && w["splits"].is_array()) {
        for (const nlohmann::json& split : w["splits"]) {
          std::vector<std::size_t> panes;
          if (split.is_array()) {
            for (const nlohmann::json& i : split) {
              if (i.is_number_unsigned() && i.get<std::size_t>() < count) {
                panes.push_back(i.get<std::size_t>());
              }
            }
          }
          if (panes.size() >= 2 && panes.size() == split.size()) {
            window.splits.push_back(std::move(panes));
          }
        }
      }
      const nlohmann::json shown = w.value("shown", nlohmann::json());
      window.shown = shown.is_number_unsigned() && shown.get<std::size_t>() < count ? shown.get<std::size_t>() : 0;
      out.push_back(std::move(window));
    }
    return out;
  }

} // namespace term
