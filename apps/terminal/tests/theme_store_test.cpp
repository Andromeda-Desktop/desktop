#include "theme_store.h"

#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <unistd.h>

using namespace term;

namespace {

int failures = 0;
unsigned seed = 0;
int step = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s (seed %u step %d)\n", __FILE__, __LINE__, #c, seed, step); ++failures; } } while (0)

std::string toml(const ThemePackage& p) {
  auto set = [](const char* name, const ThemeAnchorSet& a) {
    return std::format("[{}]\nsurface = \"#{:06x}\"\ntext = \"#{:06x}\"\naccent = \"#{:06x}\"\n", name, a.surface,
                       a.text, a.accent);
  };
  std::string out = "name = \"" + p.name + "\"\n";
  if (!p.font.empty()) {
    out += "font = \"" + p.font + "\"\n";
  }
  return out + set("light", p.light) + set("dark", p.dark);
}

// What a folder holds, and so what the store must report for it.
struct Folder {
  std::optional<ThemePackage> valid; // theme.toml parses to this; nullopt: no file or a bad one
};

} // namespace

int main() {
  const char* bad[] = {
      "name = \"x\"\n",                                                                       // no twins
      "name = \"x\"\n[light]\nsurface = \"#fff\"\ntext = \"#000000\"\naccent = \"#000000\"\n", // short colour
      "name = \"\"\n",
      "nme = \"x\"\n",
      "[light\n",
  };
  const char* ids[] = {"nord", "paper", "neon-2", "dark", "Bad Name", "x_y"};
  for (const unsigned s : {1U, 2U, 3U}) {
    seed = s;
    std::mt19937 rng(s);
    auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / std::format("theme-store-test-{}", getpid()) / std::format("themes-{}", s);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::map<std::string, Folder> model;
    std::map<std::string, ThemePackage> lastGood;
    std::vector<ThemeEntry> entries;
    for (step = 0; step < 400; ++step) {
      const std::string id = ids[pick(0, 5)];
      const std::filesystem::path folder = dir / id;
      switch (pick(0, 4)) {
      case 0: { // write a valid package
        auto anchors = [&] {
          auto c = [&] { return static_cast<std::uint32_t>(pick(0, 0xffffff)); };
          return ThemeAnchorSet{c(), c(), c()};
        };
        ThemePackage p{.name = "Theme " + std::to_string(pick(0, 9)), .light = anchors(), .dark = anchors(),
                       .font = pick(0, 1) != 0 ? "Fira Code" : ""};
        std::filesystem::create_directories(folder);
        std::ofstream(folder / "theme.toml") << toml(p);
        std::filesystem::remove(folder / "scene.glsl");
        std::filesystem::remove(folder / "background.png");
        const int backdrop = pick(0, 3); // none, scene, picture, both
        if (backdrop == 1 || backdrop == 3) {
          std::ofstream(folder / "scene.glsl") << "vec4 scene(vec2 uv) { return vec4(uv, 0.0, 1.0); }\n";
          p.backdrop = ThemePackage::Shader{"vec4 scene(vec2 uv) { return vec4(uv, 0.0, 1.0); }\n"};
        }
        if (backdrop == 2 || backdrop == 3) {
          std::ofstream(folder / "background.png") << "png";
          const auto stamp = std::filesystem::last_write_time(folder / "background.png").time_since_epoch().count();
          p.backdrop = ThemePackage::Picture{(folder / "background.png").string(), static_cast<std::int64_t>(stamp)};
        }
        model[id] = {backdrop == 3 ? std::nullopt : std::optional(p)};
        break;
      }
      case 1: // write a bad file
        std::filesystem::create_directories(folder);
        std::ofstream(folder / "theme.toml") << bad[pick(0, 4)];
        model[id] = {std::nullopt};
        break;
      case 2: // remove the file, keep the folder
        if (model.contains(id)) {
          std::filesystem::remove(folder / "theme.toml");
          model[id].valid.reset();
        }
        break;
      case 4: { // import a folder from elsewhere, good or bad, under any name
        const char* names[] = {"My Theme", "nord", "dark", "--x--", "\xc3\x9cber"};
        const std::filesystem::path source = dir.parent_path() / std::format("theme-src-{}-{}", getpid(), s) / names[pick(0, 4)];
        std::filesystem::remove_all(source.parent_path());
        std::filesystem::create_directories(source);
        const bool good = pick(0, 1) != 0;
        const ThemePackage p{.name = "Imported", .light = {0xffffff, 0x101010, 0x2060c0}, .dark = {0x101010, 0xf0f0f0, 0x60a0ff}};
        std::ofstream(source / "theme.toml") << (good ? toml(p) : bad[pick(0, 4)]);
        std::ofstream(source / "notes.txt") << "not part of a package";
        const auto before = scanThemes(dir, entries);
        const auto result = importThemeFolder(source, dir);
        std::filesystem::remove_all(source.parent_path());
        CHECK(std::holds_alternative<std::string>(result) == good);
        for (const auto& item : std::filesystem::directory_iterator(dir.parent_path())) {
          CHECK(!item.path().filename().string().starts_with(".theme-import-"));
        }
        if (const auto* newId = std::get_if<std::string>(&result)) {
          CHECK(isThemeId(*newId) && !builtinTheme(*newId) && !model.contains(*newId));
          CHECK(!std::filesystem::exists(dir / *newId / "notes.txt"));
          model[*newId] = {p};
        } else {
          CHECK(scanThemes(dir, entries) == before);
        }
        break;
      }
      default: // remove the folder
        std::filesystem::remove_all(folder);
        model.erase(id);
        lastGood.erase(id);
        break;
      }
      entries = scanThemes(dir, entries);

      CHECK(entries.size() == model.size());
      auto m = model.begin();
      for (const ThemeEntry& e : entries) {
        if (m == model.end()) {
          break;
        }
        CHECK(e.id == m->first);
        const bool usable = isThemeId(e.id) && !builtinTheme(e.id);
        if (usable && m->second.valid) {
          const auto* v = std::get_if<ThemeEntry::Valid>(&e.state);
          CHECK(v != nullptr && v->package == *m->second.valid);
          lastGood[e.id] = *m->second.valid;
        } else {
          const auto* inv = std::get_if<ThemeEntry::Invalid>(&e.state);
          CHECK(inv != nullptr && !inv->error.message.empty());
          if (inv != nullptr) {
            const auto good = lastGood.find(e.id);
            CHECK(inv->lastGood == (good != lastGood.end() ? std::optional(good->second) : std::nullopt));
          }
        }
        ++m;
      }
    }
    std::filesystem::remove_all(dir.parent_path());
  }
  if (failures != 0) {
    std::fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  return 0;
}
