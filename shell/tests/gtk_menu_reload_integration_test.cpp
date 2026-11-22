#include "shell/bar/widgets/gtk_menu_client.h"
#include "tests/test_check.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <thread>
#include <vector>

namespace {

  using ItemProps = std::map<std::string, sdbus::Variant>;
  using MenuGroup = sdbus::Struct<std::uint32_t, std::uint32_t, std::vector<ItemProps>>;
  using Link = sdbus::Struct<std::uint32_t, std::uint32_t>;
  using ActionInfo = sdbus::Struct<bool, sdbus::Signature, std::vector<sdbus::Variant>>;
  using MenuChange = sdbus::Struct<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::vector<ItemProps>>;

  const sdbus::ServiceName kService{"org.andromeda.test.GtkApp"};
  const sdbus::ObjectPath kMenuPath{"/menubar"};
  const sdbus::ObjectPath kAppPath{"/app"};

  // A GTK app: File > Quit over org.gtk.Menus, app.quit over org.gtk.Actions. While held, Start replies wait.
  class FakeGtkApp {
  public:
    FakeGtkApp()
        : connection(sdbus::createSessionBusConnection(kService)), menus(sdbus::createObject(*connection, kMenuPath)),
          actions(sdbus::createObject(*connection, kAppPath)) {
      menus
          ->addVTable(
              sdbus::registerMethod("Start").implementedAs(
                  [this](sdbus::Result<std::vector<MenuGroup>>&& result, std::vector<std::uint32_t> groups) {
                    const std::lock_guard lock(mutex);
                    if (held) {
                      waiting.emplace_back(std::move(result), std::move(groups));
                    } else {
                      result.returnResults(reply(groups));
                    }
                  }
              ),
              sdbus::registerMethod("End").implementedAs([](std::vector<std::uint32_t>) {}),
              sdbus::registerSignal("Changed").withParameters<std::vector<MenuChange>>()
          )
          .forInterface("org.gtk.Menus");
      actions
          ->addVTable(
              sdbus::registerMethod("DescribeAll").implementedAs([] {
                return std::map<std::string, ActionInfo>{{"quit", ActionInfo{true, sdbus::Signature{""}, {}}}};
              }),
              sdbus::registerSignal("Changed")
                  .withParameters<
                      std::vector<std::string>, std::map<std::string, bool>, std::map<std::string, sdbus::Variant>,
                      std::map<std::string, ActionInfo>>()
          )
          .forInterface("org.gtk.Actions");
      connection->enterEventLoopAsync();
    }

    ~FakeGtkApp() { connection->leaveEventLoop(); }

    FakeGtkApp(const FakeGtkApp&) = delete;
    FakeGtkApp& operator=(const FakeGtkApp&) = delete;

    static std::vector<MenuGroup> reply(const std::vector<std::uint32_t>& groups) {
      std::vector<MenuGroup> out;
      for (const std::uint32_t group : groups) {
        if (group == 0) {
          out.emplace_back(0U, 0U, std::vector<ItemProps>{{{"label", sdbus::Variant{std::string{"File"}}},
                                                          {":submenu", sdbus::Variant{Link{1U, 0U}}}}});
        } else if (group == 1) {
          out.emplace_back(1U, 0U, std::vector<ItemProps>{{{"label", sdbus::Variant{std::string{"Quit"}}},
                                                          {"action", sdbus::Variant{std::string{"app.quit"}}}}});
        }
      }
      return out;
    }

    void hold() {
      const std::lock_guard lock(mutex);
      held = true;
    }

    [[nodiscard]] std::size_t waitingStarts() {
      const std::lock_guard lock(mutex);
      return waiting.size();
    }

    void release() {
      const std::lock_guard lock(mutex);
      held = false;
      for (auto& [result, groups] : waiting) {
        result.returnResults(reply(groups));
      }
      waiting.clear();
    }

    void emitMenusChanged() {
      menus->emitSignal("Changed").onInterface("org.gtk.Menus").withArguments(std::vector<MenuChange>{});
    }

    void emitQuitDisabled() {
      actions->emitSignal("Changed")
          .onInterface("org.gtk.Actions")
          .withArguments(
              std::vector<std::string>{}, std::map<std::string, bool>{{"quit", false}},
              std::map<std::string, sdbus::Variant>{}, std::map<std::string, ActionInfo>{}
          );
    }

    std::unique_ptr<sdbus::IConnection> connection;
    std::unique_ptr<sdbus::IObject> menus;
    std::unique_ptr<sdbus::IObject> actions;
    std::mutex mutex;
    bool held = false;
    std::vector<std::pair<sdbus::Result<std::vector<MenuGroup>>, std::vector<std::uint32_t>>> waiting;
  };

  template <typename Predicate> void drainUntil(sdbus::IConnection& bus, Predicate&& predicate) {
    for (int attempt = 0; attempt < 2000; ++attempt) {
      while (bus.processPendingEvent()) {
      }
      if (predicate()) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    noctalia::test::failCheck("timed out waiting for D-Bus state");
  }

  [[nodiscard]] bool showsFileQuit(const GtkMenuClient& client) {
    const DbusMenuNode& root = client.root();
    return root.children.size() == 1 && root.children[0].label == "File" && root.children[0].children.size() == 1
        && root.children[0].children[0].label == "Quit";
  }

  // An actions Changed while a menus reload waits for Start rebuilds the last complete tree, not a partial one.
  void actionsChangeDuringReloadKeepsTheTree() {
    FakeGtkApp app;
    const auto bus = sdbus::createSessionBusConnection();
    int changes = 0;
    bool emptyShown = false;
    std::optional<GtkMenuClient> client;
    client.emplace(*bus, std::string{kService}, std::string{kMenuPath}, std::string{kAppPath}, std::string{}, [&] {
      ++changes;
      emptyShown = emptyShown || client->root().children.empty();
    });
    drainUntil(*bus, [&] { return client->loaded() && showsFileQuit(*client); });
    TEST_CHECK(!emptyShown);

    app.hold();
    app.emitMenusChanged();
    drainUntil(*bus, [&] { return app.waitingStarts() == 1; });
    const int before = changes;
    app.emitQuitDisabled();
    drainUntil(*bus, [&] { return changes > before; });
    TEST_CHECK(showsFileQuit(*client));
    TEST_CHECK(!client->root().children[0].children[0].enabled);
    TEST_CHECK(!emptyShown);

    app.release();
    drainUntil(*bus, [&] { return changes > before + 1; });
    TEST_CHECK(showsFileQuit(*client));
    client.reset();
  }

} // namespace

int main() {
  actionsChangeDuringReloadKeepsTheTree();
  return 0;
}
