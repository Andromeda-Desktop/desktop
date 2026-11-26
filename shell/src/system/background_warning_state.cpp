#include "system/background_warning_state.h"

#include "config/atomic_file.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <utility>

namespace fs = std::filesystem;

BackgroundWarningState::BackgroundWarningState(std::string path) : m_path(std::move(path)) {
  std::ifstream in(m_path);
  if (!in) {
    return;
  }
  const nlohmann::json json = nlohmann::json::parse(in, nullptr, false);
  if (!json.is_object()) {
    return;
  }
  if (const auto it = json.find("notified"); it != json.end() && it->is_object()) {
    for (const auto& [key, value] : it->items()) {
      if (value.is_number_integer()) {
        m_notified[key] = value.get<std::int64_t>();
      }
    }
  }
  if (const auto it = json.find("ignored"); it != json.end() && it->is_array()) {
    for (const auto& value : *it) {
      if (value.is_string()) {
        m_ignored.insert(value.get<std::string>());
      }
    }
  }
}

bool BackgroundWarningState::alreadyNotified(const std::string& key, std::int64_t mtime) const {
  const auto it = m_notified.find(key);
  return it != m_notified.end() && it->second == mtime;
}

void BackgroundWarningState::markNotified(const std::string& key, std::int64_t mtime) {
  m_notified[key] = mtime;
  save();
}

void BackgroundWarningState::setIgnored(const std::string& rowKey, bool ignored) {
  if (ignored ? m_ignored.insert(rowKey).second : m_ignored.erase(rowKey) > 0) {
    save();
  }
}

void BackgroundWarningState::save() const {
  const nlohmann::json json{{"notified", m_notified}, {"ignored", m_ignored}};
  writeTextFileAtomicAsync(m_path, json.dump(2));
}

std::int64_t fileMtime(const std::string& path) {
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0 ? static_cast<std::int64_t>(st.st_mtime) : 0;
}
