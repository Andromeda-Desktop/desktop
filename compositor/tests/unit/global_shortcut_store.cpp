#include "input/global_shortcut_store.h"

#include "check.h"
#include "loop_pump.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/stat.h>

using andromeda::ShortcutChord;
using andromeda::ShortcutGrants;
namespace mod = andromeda::shortcut_mod;

namespace {

  // The store with each update awaited on the test loop.
  struct GlobalShortcutStore {
    using Status = andromeda::GlobalShortcutStore::Status;
    andromeda::GlobalShortcutStore store;
    explicit GlobalShortcutStore(std::filesystem::path path) : store(test::eventLoop(), std::move(path)) {}
    std::optional<ShortcutGrants> load() { return store.load(); }
    std::optional<ShortcutGrants> update(std::string_view app, std::map<std::string, ShortcutChord> shortcuts) {
      return test::await<std::optional<ShortcutGrants>>([&](auto done) {
        store.update(app, std::move(shortcuts), std::move(done));
      });
    }
    [[nodiscard]] Status status() const { return store.status(); }
  };

  class Fixture {
  public:
    Fixture() {
      std::array<char, 64> pattern{};
      constexpr std::string_view prefix = "/tmp/andromeda-global-shortcuts.XXXXXX";
      std::ranges::copy(prefix, pattern.begin());
      if (mkdtemp(pattern.data()) == nullptr) {
        throw std::runtime_error("could not create a test directory");
      }
      directory = pattern.data();
      path = directory / "state/global-shortcuts.json";
    }
    ~Fixture() {
      std::error_code error;
      std::filesystem::remove_all(directory, error);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    void write(const std::string& bytes, mode_t mode) const {
      std::filesystem::create_directories(path.parent_path());
      std::ofstream(path) << bytes;
      chmod(path.c_str(), mode);
    }
    [[nodiscard]] std::string contents() const {
      std::ifstream in(path);
      return {std::istreambuf_iterator<char>(in), {}};
    }

    std::filesystem::path directory;
    std::filesystem::path path;
  };

  bool saveAll(GlobalShortcutStore& store, const ShortcutGrants& grants) {
    for (const auto& [app, shortcuts] : grants) {
      if (!store.update(app, shortcuts)) {
        return false;
      }
    }
    return true;
  }

  ShortcutGrants sample() {
    return {
        {"org.example.Player",
         {{"play", {.modifiers = mod::kCtrl | mod::kAlt, .keysym = XKB_KEY_m}},
          {"next", {.modifiers = mod::kLogo, .keysym = XKB_KEY_XF86AudioNext}}}},
        {"org.example.Notes", {{"new", {.modifiers = mod::kShift | mod::kLogo, .keysym = XKB_KEY_slash}}}},
    };
  }

} // namespace

ANDROMEDA_TEST(grantsSurviveRestart) {
  const Fixture fixture;
  GlobalShortcutStore first(fixture.path);
  CHECK(first.load().has_value());
  CHECK(saveAll(first, sample()));

  GlobalShortcutStore second(fixture.path);
  const auto loaded = second.load();
  CHECK(loaded.has_value());
  CHECK(*loaded == sample());
  CHECK((std::filesystem::status(fixture.path).permissions() & std::filesystem::perms::all)
        == (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write));
}

ANDROMEDA_TEST(missingFileIsNoGrants) {
  const Fixture fixture;
  GlobalShortcutStore store(fixture.path);
  const auto loaded = store.load();
  CHECK(loaded.has_value() && loaded->empty());
  CHECK(store.status() == GlobalShortcutStore::Status::Loaded);
}

ANDROMEDA_TEST(unreadableFileIsReportedAndNeverOverwritten) {
  const std::string valid = R"({"version":1,"grants":{"a":{"x":"Ctrl+M"}}})";
  const std::array<std::pair<std::string, mode_t>, 5> cases{{
      {valid, 0644},
      {"{not json", 0600},
      {R"({"version":2,"grants":{}})", 0600},
      {R"({"version":1,"grants":{"a":{"x":"Ctrl+Bogus_Key"}}})", 0600},
      {R"({"version":1,"grants":{"a":{}}})", 0600},
  }};
  for (const auto& [bytes, mode] : cases) {
    const Fixture fixture;
    fixture.write(bytes, mode);
    GlobalShortcutStore store(fixture.path);
    CHECK(!store.load().has_value());
    CHECK(store.status() == GlobalShortcutStore::Status::Unreadable);
    CHECK(!saveAll(store, sample()));
    CHECK(fixture.contents() == bytes);
  }
}

ANDROMEDA_TEST(symlinkIsUnreadable) {
  const Fixture fixture;
  const auto target = fixture.directory / "elsewhere.json";
  std::ofstream(target) << R"({"version":1,"grants":{}})";
  chmod(target.c_str(), 0600);
  std::filesystem::create_directories(fixture.path.parent_path());
  std::filesystem::create_symlink(target, fixture.path);
  GlobalShortcutStore store(fixture.path);
  CHECK(!store.load().has_value());
  CHECK(!saveAll(store, sample()));
  CHECK(std::filesystem::is_symlink(fixture.path));
}

ANDROMEDA_TEST(overCapsKeepsTheLastSavedFile) {
  const Fixture fixture;
  GlobalShortcutStore store(fixture.path);
  CHECK(store.load().has_value());
  CHECK(saveAll(store, sample()));
  const std::string saved = fixture.contents();

  std::map<std::string, ShortcutChord> tooMany;
  for (int index = 0; index < 65; ++index) {
    tooMany["id" + std::to_string(index)] = {.modifiers = mod::kCtrl, .keysym = XKB_KEY_a};
  }
  CHECK(!store.update("org.example.Many", tooMany));
  CHECK(fixture.contents() == saved);
}

ANDROMEDA_TEST(twoInstancesKeepEachOthersGrants) {
  const Fixture fixture;
  const auto grants = sample();
  GlobalShortcutStore session(fixture.path);
  GlobalShortcutStore nested(fixture.path);
  CHECK(session.load().has_value());
  CHECK(nested.load().has_value());

  CHECK(session.update("org.example.Player", grants.at("org.example.Player")).has_value());
  const auto merged = nested.update("org.example.Notes", grants.at("org.example.Notes"));
  CHECK(merged.has_value() && *merged == grants);

  const auto afterRevoke = session.update("org.example.Player", {});
  CHECK(afterRevoke.has_value() && afterRevoke->size() == 1 && afterRevoke->contains("org.example.Notes"));
  GlobalShortcutStore restarted(fixture.path);
  CHECK(restarted.load() == afterRevoke);

  CHECK(nested.update("", {}).has_value());
  const auto cleared = restarted.load();
  CHECK(cleared.has_value() && cleared->empty());
}

// Approvals and revokes queue up, results arrive late, and some saves fail (the lock file turns unsafe). After every
// save settles, the file holds exactly what the acknowledged saves wrote, and memory matches it except for an app
// whose last approval failed to save.
ANDROMEDA_TEST(queuedSavesKeepMemoryAndFileInStep) {
  for (uint32_t seed = 1; seed <= 30; ++seed) {
    const Fixture fixture;
    const auto lockFile = fixture.path.parent_path() / ".global-shortcuts.lock";
    andromeda::ShortcutRegistry registry;
    andromeda::ShortcutGrantSync sync(registry, test::eventLoop(), fixture.path);
    CHECK(sync.load());
    ShortcutGrants file;
    std::set<std::string> unsaved;
    // The last approval requested per app.
    ShortcutGrants approved;
    int pending = 0;
    std::mt19937 random(seed);
    for (int step = 0; step < 60; ++step) {
      const std::string app = "org.example.App" + std::to_string(random() % 4);
      switch (random() % 10) {
      case 0:
      case 1:
      case 2:
      case 3: {
        ShortcutGrants grants = registry.grants();
        grants[app] = {{"run", {.modifiers = mod::kCtrl, .keysym = static_cast<uint32_t>(XKB_KEY_a + random() % 26)}}};
        const auto value = grants[app];
        approved[app] = value;
        registry.setGrants(std::move(grants));
        ++pending;
        sync.persist(app, [&, app, value](bool saved) {
          --pending;
          if (saved) {
            file[app] = value;
            unsaved.erase(app);
          } else {
            unsaved.insert(app);
          }
        });
        break;
      }
      case 4:
      case 5:
      case 6: {
        ++pending;
        sync.revoke(app, [&, app](bool saved) {
          --pending;
          if (saved) {
            file.erase(app);
          }
          // Saved or not, memory now follows the file for this app.
          unsaved.erase(app);
        });
        CHECK(!registry.grants().contains(app));
        break;
      }
      case 7: {
        ++pending;
        sync.revoke("", [&](bool saved) {
          --pending;
          if (saved) {
            file.clear();
          }
          unsaved.clear();
        });
        CHECK(registry.grants().empty());
        break;
      }
      default:
        // Group-readable: the saver refuses the lock until it is private again.
        std::filesystem::create_directories(lockFile.parent_path());
        std::ofstream{lockFile, std::ios::app};
        chmod(lockFile.c_str(), random() % 2 == 0 ? 0644 : 0600);
      }
      for (uint32_t pumps = random() % 3; pumps > 0; --pumps) {
        wl_event_loop_dispatch(test::eventLoop(), 0);
      }
    }
    test::pumpUntil([&] { return pending == 0; });

    CHECK(GlobalShortcutStore(fixture.path).load() == file);
    std::set<std::string> apps;
    for (const ShortcutGrants* grants : std::array<const ShortcutGrants*, 2>{&registry.grants(), &file}) {
      for (const auto& [app, shortcuts] : *grants) {
        apps.insert(app);
      }
    }
    for (const std::string& app : apps) {
      if (!unsaved.contains(app)) {
        CHECK(registry.grants().contains(app) == file.contains(app));
        CHECK(!file.contains(app) || !registry.grants().contains(app) || registry.grants().at(app) == file.at(app));
      }
    }
    // A failed approval survives the other apps' later saves.
    for (const std::string& app : unsaved) {
      CHECK(registry.grants().contains(app) && registry.grants().at(app) == approved[app]);
    }
  }
}

int main() { return RUN_TESTS(); }
