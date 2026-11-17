#include "input/global_shortcut_store.h"

#include "check.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
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
  CHECK(first.save(sample()));

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
    CHECK(!store.save(sample()));
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
  CHECK(!store.save(sample()));
  CHECK(std::filesystem::is_symlink(fixture.path));
}

ANDROMEDA_TEST(overCapsKeepsTheLastSavedFile) {
  const Fixture fixture;
  GlobalShortcutStore store(fixture.path);
  CHECK(store.load().has_value());
  CHECK(store.save(sample()));
  const std::string saved = fixture.contents();

  ShortcutGrants tooMany = sample();
  for (int index = 0; index < 65; ++index) {
    tooMany["org.example.Many"]["id" + std::to_string(index)] = {.modifiers = mod::kCtrl, .keysym = XKB_KEY_a};
  }
  CHECK(!store.save(tooMany));
  CHECK(fixture.contents() == saved);
}

int main() { return RUN_TESTS(); }
