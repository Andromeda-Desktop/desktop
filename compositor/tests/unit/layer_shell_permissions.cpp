#include "layer/layer_shell_permissions.h"

#include "check.h"
#include "loop_pump.h"

#include <filesystem>
#include <optional>
#include <unistd.h>


namespace {
  // The store with each change awaited on the test loop.
  struct LayerShellPermissions {
    andromeda::LayerShellPermissions store;
    explicit LayerShellPermissions(std::filesystem::path file) : store(test::eventLoop(), std::move(file)) {}
    bool remember(const std::string& app) {
      return test::await<bool>([&](auto done) { store.remember(app, std::move(done)); });
    }
    bool revoke(std::string_view app) {
      return test::await<std::optional<bool>>([&](auto done) { store.revoke(app, std::move(done)); }).value_or(false);
    }
    [[nodiscard]] bool contains(const std::string& app) const { return store.contains(app); }
    [[nodiscard]] const auto& apps() const { return store.apps(); }
  };

  struct Dir {
    std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("andromeda-layer-permissions-" + std::to_string(getpid()));
    std::filesystem::path file = root / "state/layer-shell-permissions.json";
    Dir() { std::filesystem::remove_all(root); }
    ~Dir() { std::filesystem::remove_all(root); }
  };
} // namespace

ANDROMEDA_TEST(rememberedAppsSurviveARestartUntilRevoked) {
  Dir dir;
  {
    LayerShellPermissions permissions(dir.file);
    CHECK(permissions.remember("flatpak/com.discordapp.Discord"));
    CHECK(permissions.remember("flatpak/org.example.Other"));
  }
  {
    LayerShellPermissions permissions(dir.file);
    CHECK(permissions.contains("flatpak/com.discordapp.Discord"));
    CHECK(permissions.revoke("flatpak/com.discordapp.Discord"));
    CHECK(!permissions.revoke("flatpak/com.discordapp.Discord"));
  }
  {
    LayerShellPermissions permissions(dir.file);
    CHECK(!permissions.contains("flatpak/com.discordapp.Discord"));
    CHECK(permissions.contains("flatpak/org.example.Other"));
    CHECK(permissions.revoke("all"));
  }
  CHECK(LayerShellPermissions(dir.file).apps().empty());
}

int main() { return RUN_TESTS(); }
