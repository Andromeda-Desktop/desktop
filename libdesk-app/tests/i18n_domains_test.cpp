// Model test: module catalogs added with addDomain resolve like the model after any order of domain adds and
// language changes: the active language's string when that catalog has the key, else English.
#include "i18n/i18n.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <print>
#include <random>
#include <string>
#include <vector>

#include <unistd.h>

namespace {
  const std::vector<std::string> kLanguages = {"en", "de", "fr"};
  constexpr int kDomains = 3;
  constexpr int kKeys = 4;

  struct Model {
    // [domain][language][key] -> text; English has every key.
    std::map<int, std::map<std::string, std::map<int, std::string>>> catalogs;
    std::vector<bool> added = std::vector<bool>(kDomains, false);
    std::string language = "en";
    unsigned seed = 0;
    // The Service is a process singleton: each seed adds domains under new names.
    [[nodiscard]] std::string domainName(int d) const { return "s" + std::to_string(seed) + "m" + std::to_string(d); }
  };

  void writeCatalogs(const std::filesystem::path& root, Model& model, std::mt19937& rng) {
    for (int d = 0; d < kDomains; ++d) {
      const auto dir = root / model.domainName(d);
      std::filesystem::create_directories(dir);
      for (const auto& lang : kLanguages) {
        std::ofstream out(dir / (lang + ".json"));
        out << "{\"group\": {";
        bool first = true;
        for (int k = 0; k < kKeys; ++k) {
          if (lang != "en" && rng() % 2 == 0) {
            continue;
          }
          const std::string text = lang + "-" + model.domainName(d) + "-" + std::to_string(k);
          model.catalogs[d][lang][k] = text;
          out << (first ? "" : ",") << "\"k" << k << "\": \"" << text << "\"";
          first = false;
        }
        out << "}}";
      }
    }
  }

  std::string expected(const Model& model, int d, int k) {
    if (!model.added[d]) {
      return "!!" + model.domainName(d) + ".group.k" + std::to_string(k) + "!!";
    }
    const auto& byLang = model.catalogs.at(d);
    if (const auto it = byLang.find(model.language); it != byLang.end() && it->second.contains(k)) {
      return it->second.at(k);
    }
    return byLang.at("en").at(k);
  }
} // namespace

int main() {
  ::unsetenv("LC_ALL");
  ::unsetenv("LC_MESSAGES");
  ::setenv("LANG", "en_US.UTF-8", 1);
  const auto root = std::filesystem::temp_directory_path() / ("i18n-domains-" + std::to_string(::getpid()));
  int failures = 0;
  for (unsigned seed = 1; seed <= 8 && failures == 0; ++seed) {
    std::mt19937 rng(seed);
    Model model;
    model.seed = seed;
    writeCatalogs(root, model, rng);
    auto& service = i18n::Service::instance();
    service.setLanguage("en");
    for (int step = 0; step < 25 && failures == 0; ++step) {
      if (rng() % 3 == 0) {
        const int d = static_cast<int>(rng() % kDomains);
        if (!model.added[d]) {
          service.addDomain(model.domainName(d), root / model.domainName(d));
          model.added[d] = true;
        }
      } else {
        model.language = kLanguages[rng() % kLanguages.size()];
        service.setLanguage(model.language);
      }
      for (int d = 0; d < kDomains; ++d) {
        for (int k = 0; k < kKeys; ++k) {
          const std::string got = i18n::tr(model.domainName(d) + ".group.k" + std::to_string(k));
          if (got != expected(model, d, k)) {
            std::println(
                stderr, "i18n_domains_test: seed {} step {} language {}: {}.k{} is '{}', want '{}'", seed, step,
                model.language, model.domainName(d), k, got, expected(model, d, k)
            );
            ++failures;
          }
        }
      }
    }
  }
  std::filesystem::remove_all(root);
  return failures == 0 ? 0 : 1;
}
