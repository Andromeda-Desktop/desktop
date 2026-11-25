#include "protocols/toml.h"
#include "scripting/luau_host.h"
#include "scripting/script_api_context.h"

#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <print>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

  namespace fs = std::filesystem;

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "plugin_file_privacy_test: {}", message);
    }
    return condition;
  }

  unsigned mode(const fs::path& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {
      return 0;
    }
    return st.st_mode & 07777;
  }

} // namespace

int main() {
  ::umask(022);
  char templ[] = "/tmp/noctalia_plugin_file_privacy_XXXXXX";
  const char* made = ::mkdtemp(templ);
  if (!expect(made != nullptr, "mkdtemp")) {
    return 1;
  }
  const fs::path root(made);
  ::setenv("ANDROMEDA_STATE_HOME", root.c_str(), 1);

  // A data dir left world-readable by an older release must be tightened.
  const fs::path dataDir = root / "andromeda" / "shell" / "plugins" / "data" / "test" / "plugin";
  fs::create_directories(dataDir);
  fs::permissions(dataDir, fs::perms(0755), fs::perm_options::replace);
  const fs::path shared = root / "shared.txt";
  {
    const int fd = ::open(shared.c_str(), O_WRONLY | O_CREAT, 0644);
    ::close(fd);
  }
  ::chmod(shared.c_str(), 0644);

  scripting::ScriptApiContext api;
  api.setConfigSnapshot(std::make_shared<const toml::table>(toml::parse("[shell]\noffline_mode = true")));
  LuauHost host(api, "test/plugin:service");

  // No [permissions]: a write outside pluginDataDir fails, also through a symlink inside it.
  fs::create_symlink(shared, dataDir / "escape.txt");
  const std::string denied = R"(
local dir = assert(noctalia.pluginDataDir())
local ok, err = pcall(noctalia.writeFile, ")"
      + shared.string()
      + R"(", "x")
assert(not ok and string.find(err, "files_write", 1, true), err)
ok, err = pcall(noctalia.writeFile, dir .. "/escape.txt", "x")
assert(not ok and string.find(err, "files_write", 1, true), err)
ok, err = pcall(noctalia.readFile, ")"
      + shared.string()
      + R"(")
assert(not ok and string.find(err, "files_read", 1, true), err)
)";
  bool ok = expect(host.exec("=denied", denied.c_str()), "writes outside the data dir should be denied");
  host.setPermissions({.filesWrite = {root.string()}});

  const std::string source = R"(
local dir = assert(noctalia.pluginDataDir())
assert(noctalia.writeFile(dir .. "/history.json", "private"))
assert(noctalia.writeFile(")"
      + shared.string()
      + R"(", "shared"))
)";
  ok = expect(host.exec("=privacy", source.c_str()), "script should run") && ok;
  ok = expect(mode(dataDir) == 0700, "pluginDataDir should be owner-only") && ok;
  ok = expect(mode(dataDir / "history.json") == 0600, "new file should be created 0600") && ok;
  ok = expect(mode(shared) == 0644, "existing file should keep its mode") && ok;

  fs::remove_all(root);
  return ok ? 0 : 1;
}
