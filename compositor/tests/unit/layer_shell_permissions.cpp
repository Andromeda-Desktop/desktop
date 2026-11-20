#include "layer/layer_shell_permissions.h"

#include "check.h"

#include <filesystem>
#include <unistd.h>

using andromeda::LayerShellPermissions;

namespace {
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
