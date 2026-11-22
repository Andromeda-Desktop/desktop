#include "vaults.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <glib.h>
#include <print>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {
  int runs(const fs::path& log) {
    std::ifstream in(log);
    int n = 0;
    for (std::string line; std::getline(in, line);) {
      ++n;
    }
    return n;
  }

  // Runs the loop until `calls` reaches `want`, at most 5 s.
  bool waitFor(GMainContext* ctx, const int& calls, int want) {
    const gint64 end = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    while (calls < want && g_get_monotonic_time() < end) {
      g_main_context_iteration(ctx, TRUE);
    }
    return calls >= want;
  }
} // namespace

int main() {
  char tmpl[] = "/tmp/files-vaults-XXXXXX";
  const fs::path root = ::mkdtemp(tmpl);
  ::setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
  fs::create_directories(root / "bin");
  const fs::path log = root / "runs";
  // Exit 12 is gocryptfs's wrong-password code: no mount, no guard process.
  {
    std::ofstream script(root / "bin" / "gocryptfs");
    script << "#!/bin/sh\ncat >/dev/null\necho run >>'" << log.string() << "'\nsleep 0.3\nexit 12\n";
  }
  fs::permissions(root / "bin" / "gocryptfs", fs::perms::owner_all);
  ::setenv("PATH", ((root / "bin").string() + ":" + ::getenv("PATH")).c_str(), 1);

  GMainContext* ctx = g_main_context_default();
  Vaults vaults("vaults-test.json");
  const Vaults::Vault vault{"v", root / "cipher", root / "mnt"};
  fs::create_directories(vault.cipher);

  int failures = 0;
  auto expect = [&failures](bool ok, const char* what) {
    if (!ok) {
      std::println(stderr, "vaults_test: {}", what);
      ++failures;
    }
  };

  int calls = 0;
  std::string first;
  std::string second;
  vaults.unlock(vault, "a", [&](const std::string& e) { first = e, ++calls; });
  vaults.unlock(vault, "a", [&](const std::string& e) { second = e, ++calls; });
  expect(waitFor(ctx, calls, 2), "both unlock callers get an answer");
  expect(runs(log) == 1, "two unlocks of one vault during a run start gocryptfs once");
  expect(first == "The password is not correct." && second == first, "the joined caller gets the run's result");

  vaults.unlock(vault, "b", [&](const std::string&) { ++calls; });
  expect(waitFor(ctx, calls, 3), "an unlock after the run ends gets an answer");
  expect(runs(log) == 2, "an unlock after the run ends starts a new run");

  std::error_code ec;
  fs::remove_all(root, ec);
  return failures == 0 ? 0 : 1;
}
