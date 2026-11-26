#include "shell/dock/pinned_apps.h"
#include "system/internal_app_metadata.h"
#include "tests/test_check.h"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace internal_apps {

  std::optional<AppMetadata> metadataForAppId(std::string_view /*appId*/) { return std::nullopt; }

  void applyMetadataToDesktopEntry(DesktopEntry& /*entry*/) {}

} // namespace internal_apps

namespace {

  DesktopEntry sampleEntry() {
    DesktopEntry entry;
    entry.id = "sample-chat.desktop";
    entry.name = "Sample Chat";
    entry.nameLower = "sample chat";
    entry.startupWmClass = "SampleChat";
    entry.startupWmClassLower = "samplechat";
    entry.exec = "sample-chat";
    entry.icon = "sample-chat";
    return entry;
  }

  DesktopEntry pathStyleEntry() {
    DesktopEntry entry;
    entry.id = "/usr/share/applications/org.example.Mail.desktop";
    entry.name = "Example Mail";
    entry.nameLower = "example mail";
    entry.startupWmClass = "ExampleMail";
    entry.startupWmClassLower = "examplemail";
    return entry;
  }

} // namespace

int main() {
  const DesktopEntry chat = sampleEntry();

  TEST_CHECK(shell::dock::pinned_apps::matchesEntry(chat, "sample-chat.desktop"));
  TEST_CHECK(!shell::dock::pinned_apps::matchesEntry(chat, "SampleChat"));
  TEST_CHECK(!shell::dock::pinned_apps::matchesEntry(chat, "sample chat"));
  TEST_CHECK(!shell::dock::pinned_apps::matchesEntry(chat, "sample_chat_desktop"));
  TEST_CHECK(!shell::dock::pinned_apps::matchesEntry(chat, ""));
  TEST_CHECK(!shell::dock::pinned_apps::matchesEntry(chat, "calendar"));

  const DesktopEntry mail = pathStyleEntry();
  TEST_CHECK(!shell::dock::pinned_apps::matchesEntry(mail, "org.example.Mail"));
  TEST_CHECK(shell::dock::pinned_apps::matchesEntry(mail, "/usr/share/applications/org.example.Mail.desktop"));

  std::vector<std::string> pinned = {"calendar", "samplechat", "sample-chat.desktop"};
  TEST_CHECK(shell::dock::pinned_apps::containsEntry(pinned, chat));

  shell::dock::pinned_apps::removeEntry(pinned, chat);
  TEST_CHECK((pinned == std::vector<std::string>{"calendar", "samplechat"}));

  // Two menus (launcher, dock) read the pinned state when they open and act later on the live list.
  // Invariants: no duplicate ids, the last activated intent per app wins, and other apps' pins survive.
  std::vector<DesktopEntry> apps;
  for (const char* id : {"a.desktop", "b.desktop", "c.desktop"}) {
    DesktopEntry entry;
    entry.id = id;
    apps.push_back(entry);
  }
  std::mt19937 rng(7);
  for (int run = 0; run < 200; ++run) {
    std::vector<std::string> live = {"other.desktop"};
    std::map<std::string, bool> expected;
    struct Menu {
      std::size_t app = 0;
      bool pin = false;
      bool open = false;
    };
    std::array<Menu, 2> menus{};
    for (int step = 0; step < 40; ++step) {
      Menu& menu = menus[rng() % menus.size()];
      if (!menu.open || rng() % 3 == 0) {
        menu.app = rng() % apps.size();
        menu.pin = !shell::dock::pinned_apps::containsEntry(live, apps[menu.app]);
        menu.open = true;
        continue;
      }
      (void)shell::dock::pinned_apps::setEntryPinned(live, apps[menu.app], menu.pin);
      expected[apps[menu.app].id] = menu.pin;
      menu.open = false;
      for (const auto& [id, pin] : expected) {
        TEST_CHECK(std::ranges::count(live, id) == (pin ? 1 : 0));
      }
      TEST_CHECK(std::ranges::count(live, "other.desktop") == 1);
    }
  }

  return 0;
}
