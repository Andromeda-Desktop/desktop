#include "input/global_shortcut_store.h"

#include "check.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <sys/stat.h>

using andromeda::GlobalShortcutStore;
using andromeda::ShortcutChord;
using andromeda::ShortcutGrants;
namespace mod = andromeda::shortcut_mod;

namespace {

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

int main() { return RUN_TESTS(); }
