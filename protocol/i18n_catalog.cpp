#include "i18n_catalog.h"

#include <fstream>
#include <nlohmann/json.hpp>
#include <system_error>

namespace i18n {

  namespace {
    void flatten(const nlohmann::json& node, const std::string& prefix, Catalog& out) {
      for (auto it = node.begin(); it != node.end(); ++it) {
        std::string path = prefix.empty() ? it.key() : prefix + "." + it.key();
        if (it->is_object()) {
          flatten(*it, path, out);
        } else if (it->is_string()) {
          out.insert_or_assign(std::move(path), it->get<std::string>());
        }
      }
    }
  } // namespace

  CatalogRead readCatalogFile(const std::filesystem::path& path, Catalog& out, std::string_view prefix,
                              std::string* error) {
    std::ifstream file(path);
    if (!file.is_open()) {
      return CatalogRead::Missing;
    }
    try {
      const auto json = nlohmann::json::parse(file);
      if (!json.is_object()) {
        if (error != nullptr) {
          *error = "not a JSON object";
        }
        return CatalogRead::Invalid;
      }
      flatten(json, std::string(prefix), out);
      return CatalogRead::Ok;
    } catch (const std::exception& e) {
      if (error != nullptr) {
        *error = e.what();
      }
      return CatalogRead::Invalid;
    }
  }

  std::filesystem::path catalogDir(const std::filesystem::path& sourceDir, const std::filesystem::path& installedDir) {
    std::error_code ec;
    return std::filesystem::exists(sourceDir / "en.json", ec) ? sourceDir : installedDir;
  }

  namespace detail {
    // Linear search over args: they almost always hold 3 entries or fewer.
    std::string interpolate(std::string_view tmpl, std::span<const Pair> args) {
      std::string out;
      out.reserve(tmpl.size());
      const std::size_t n = tmpl.size();
      std::size_t i = 0;
      while (i < n) {
        const char c = tmpl[i];
        if (c != '{') {
          out.push_back(c);
          ++i;
          continue;
        }
        const std::size_t end = tmpl.find('}', i + 1);
        if (end == std::string_view::npos) {
          out.append(tmpl.substr(i));
          break;
        }
        const std::string_view name = tmpl.substr(i + 1, end - i - 1);
        bool matched = false;
        for (const auto& p : args) {
          if (p.first == name) {
            out.append(p.second);
            matched = true;
            break;
          }
        }
        if (!matched) {
          out.append(tmpl.substr(i, end - i + 1));
        }
        i = end + 1;
      }
      return out;
    }
  } // namespace detail

} // namespace i18n
