#include "input/input_capture_permissions.h"

#include "check.h"
#include "loop_pump.h"
#include "server/client_trust.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using andromeda::InputCaptureGrant;
using andromeda::InputPermissionKind;

namespace {
  // A grant callback whose grant always reaches its client.
  template <class Done> auto delivered(Done done) {
    return [done = std::move(done)](std::optional<InputCaptureGrant> grant) {
      done(std::move(grant));
      return true;
    };
  }

  // The store with each change awaited on the test loop.
  struct InputCapturePermissions {
    andromeda::InputCapturePermissions store;
    explicit InputCapturePermissions(std::filesystem::path path) : store(test::eventLoop(), std::move(path)) {}
    std::optional<InputCaptureGrant> issue(
        std::string_view application, uint32_t capabilities, uint32_t persistMode, uint64_t peer,
        InputPermissionKind kind = InputPermissionKind::Capture
    ) {
      return test::await<std::optional<InputCaptureGrant>>([&](auto done) {
        store.issue(application, capabilities, persistMode, peer, kind, delivered(std::move(done)));
      });
    }
    std::optional<InputCaptureGrant> restore(
        std::string_view token, std::string_view application, uint32_t capabilities, uint32_t persistMode,
        uint64_t peer, InputPermissionKind kind = InputPermissionKind::Capture
    ) {
      return test::await<std::optional<InputCaptureGrant>>([&](auto done) {
        store.restore(token, application, capabilities, persistMode, peer, kind, delivered(std::move(done)));
      });
    }
    bool revoke(std::string_view id) {
      return test::await<bool>([&](auto done) { store.revoke(id, std::move(done)); });
    }
    std::expected<std::string, std::string> addBinary(std::string_view path) {
      return test::await<std::expected<std::string, std::string>>([&](auto done) {
        store.addBinary(path, std::move(done));
      });
    }
    bool removeBinary(std::string_view path) {
      return test::await<bool>([&](auto done) { store.removeBinary(path, std::move(done)); });
    }
    void expirePeer(uint64_t peer) { store.expirePeer(peer); }
    auto list() { return store.list(); }
    auto binaries() { return store.binaries(); }
  };

  constexpr std::string_view kId = "11111111111111111111111111111111";
  constexpr std::string_view kToken = "2222222222222222222222222222222222222222222222222222222222222222";

  std::string record(std::string_view id = kId, std::string_view token = kToken, uint64_t capabilities = 3) {
    return std::format(
        R"({{"id":"{}","token":"{}","application":"org.test.Capture","capabilities":{}}})", id, token, capabilities
    );
  }

  std::string document(std::string_view records) { return std::format(R"({{"version":1,"grants":[{}]}})", records); }

  class Fixture {
  public:
    Fixture() {
      std::array<char, 64> pattern{};
      constexpr std::string_view prefix = "/tmp/andromeda-capture-permissions.XXXXXX";
      std::ranges::copy(prefix, pattern.begin());
      if (mkdtemp(pattern.data()) == nullptr) {
        throw std::runtime_error("could not create a private permission test directory");
      }
      directory = pattern.data();
      path = directory / "state/permissions.json";
    }
    ~Fixture() {
      std::error_code error;
      std::filesystem::remove_all(directory, error);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    void write(std::string_view bytes, mode_t permissions = 0600) const {
      std::filesystem::create_directories(path.parent_path());
      std::ofstream output(path, std::ios::binary | std::ios::trunc);
      output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      output.close();
      if (!output || chmod(path.c_str(), permissions) != 0) {
        throw std::runtime_error("could not seed the private permission test store");
      }
    }

    [[nodiscard]] std::string read() const {
      std::ifstream input(path, std::ios::binary);
      return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    std::filesystem::path directory;
    std::filesystem::path path;
  };

  void rejectedStore(const Fixture& fixture) {
    const std::string before = fixture.read();
    InputCapturePermissions permissions(fixture.path);
    CHECK(!permissions.restore(kToken, "org.test.Capture", 3, 2, 11));
    CHECK(!permissions.issue("org.test.New", 3, 2, 11));
    CHECK(fixture.read() == before);
  }
} // namespace

ANDROMEDA_TEST(ephemeralRestoreRotatesTheTokenAndExpiresOnlyItsOwningPeer) {
  Fixture fixture;
  InputCapturePermissions permissions(fixture.path);
  const auto first = permissions.issue("org.test.First", 3, 1, 11);
  const auto second = permissions.issue("org.test.Second", 2, 1, 22);
  CHECK(first && second);
  if (!first || !second) {
    return;
  }
  CHECK(!std::filesystem::exists(fixture.path));
  CHECK(!permissions.restore(first->token, "org.test.First", 3, 1, 22));
  const auto rotated = permissions.restore(first->token, "org.test.First", 1, 1, 11);
  CHECK(rotated.has_value());
  if (!rotated) {
    return;
  }
  CHECK_EQ(rotated->id, first->id);
  CHECK(rotated->token != first->token);
  CHECK(!permissions.restore(first->token, "org.test.First", 1, 1, 11));
  InputCapturePermissions anotherInstance(fixture.path);
  CHECK(!anotherInstance.restore(rotated->token, "org.test.First", 1, 1, 11));
  permissions.expirePeer(22);
  CHECK(!permissions.restore(second->token, "org.test.Second", 2, 1, 22));
  const auto surviving = permissions.restore(rotated->token, "org.test.First", 3, 1, 11);
  CHECK(surviving.has_value());
  if (surviving) {
    permissions.expirePeer(11);
    CHECK(!permissions.restore(surviving->token, "org.test.First", 3, 1, 11));
  }
  CHECK(permissions.list().empty());
}

ANDROMEDA_TEST(durableRestoreRefreshesStaleInstancesAndRejectsConsumedCredentials) {
  Fixture fixture;
  InputCapturePermissions issuer(fixture.path);
  InputCapturePermissions stale(fixture.path);
  const auto grant = issuer.issue("org.test.Capture", 3, 2, 11);
  CHECK(grant.has_value());
  if (!grant) {
    return;
  }
  const auto restored = stale.restore(grant->token, "org.test.Capture", 2, 2, 22);
  CHECK(restored.has_value());
  if (!restored) {
    return;
  }
  CHECK_EQ(restored->id, grant->id);
  CHECK(restored->token != grant->token);
  CHECK(!issuer.restore(grant->token, "org.test.Capture", 3, 2, 11));
  stale.expirePeer(22);
  InputCapturePermissions reopened(fixture.path);
  const auto next = reopened.restore(restored->token, "org.test.Capture", 1, 2, 33);
  CHECK(next.has_value());
  CHECK(!stale.restore(restored->token, "org.test.Capture", 2, 2, 22));
  if (next) {
    CHECK(next->token != restored->token);
    CHECK_EQ(next->id, grant->id);
  }
}

ANDROMEDA_TEST(failedScopeUpgradesDoNotConsumeAnOtherwiseValidCredential) {
  Fixture fixture;
  InputCapturePermissions permissions(fixture.path);
  CHECK(!permissions.issue("org.test.Capture", 0, 2, 11));
  CHECK(!permissions.issue("org.test.Capture", 4, 2, 11));
  CHECK(!permissions.issue("org.test.Capture", 9, 2, 11));
  CHECK(!permissions.issue("org.test.Capture", 5, 2, 11));
  CHECK(!permissions.issue("org.test.Capture", 9, 2, 11, InputPermissionKind::Emulate));
  CHECK(permissions.issue("org.test.Remote", 5, 2, 11, InputPermissionKind::Emulate));
  CHECK(!permissions.issue("org.test.Capture", 1, 3, 11));
  CHECK(!permissions.issue("org.test.Capture", 1, 0, 11));
  CHECK(!permissions.issue("org.test.Capture", 1, 2, 0));
  CHECK(!permissions.issue(std::string(257, 'a'), 1, 2, 11));
  const auto grant = permissions.issue("org.test.Capture", 1, 1, 11);
  CHECK(grant.has_value());
  if (!grant) {
    return;
  }
  CHECK(!permissions.restore(grant->token, "org.test.Other", 1, 1, 11));
  CHECK(!permissions.restore(grant->token, "org.test.Capture", 3, 1, 11));
  CHECK(!permissions.restore(grant->token, "org.test.Capture", 5, 1, 11));
  CHECK(!permissions.restore(grant->token, "org.test.Capture", 1, 2, 11));
  CHECK(!permissions.restore(grant->token, "org.test.Capture", 1, 1, 0));
  CHECK(!permissions.restore(std::string(64, 'z'), "org.test.Capture", 1, 1, 11));
  CHECK(permissions.restore(grant->token, "org.test.Capture", 1, 1, 11).has_value());
}

ANDROMEDA_TEST(blankApplicationIdsRequireIndependentTokensAndSupportIndividualRevocation) {
  Fixture fixture;
  InputCapturePermissions permissions(fixture.path);
  InputCapturePermissions stale(fixture.path);
  const auto first = permissions.issue("", 3, 2, 11);
  const auto second = permissions.issue("", 3, 2, 22);
  CHECK(first && second);
  if (!first || !second) {
    return;
  }
  CHECK(first->id != second->id);
  CHECK(first->token != second->token);
  CHECK(!stale.restore(first->token, "org.test.Named", 3, 2, 33));
  CHECK(stale.revoke(first->id));
  CHECK(!permissions.restore(first->token, "", 3, 2, 11));
  CHECK(!permissions.revoke("unknown-permission"));
  const auto remaining = permissions.restore(second->token, "", 3, 2, 44);
  CHECK(remaining.has_value());
  CHECK(stale.revoke("all"));
  if (remaining) {
    CHECK(!permissions.restore(remaining->token, "", 3, 2, 44));
  }
  CHECK(permissions.list().empty());
}

ANDROMEDA_TEST(downgradingDurablePermissionRemovesTheDurableCredential) {
  Fixture fixture;
  InputCapturePermissions permissions(fixture.path);
  const auto durable = permissions.issue("org.test.Capture", 3, 2, 11);
  CHECK(durable.has_value());
  if (!durable) {
    return;
  }
  const auto ephemeral = permissions.restore(durable->token, "org.test.Capture", 3, 1, 22);
  CHECK(ephemeral.has_value());
  if (!ephemeral) {
    return;
  }
  CHECK_EQ(ephemeral->persistMode, 1U);
  InputCapturePermissions reopened(fixture.path);
  CHECK(reopened.list().empty());
  CHECK(!reopened.restore(durable->token, "org.test.Capture", 3, 2, 11));
  CHECK(!reopened.restore(ephemeral->token, "org.test.Capture", 3, 1, 22));
  permissions.expirePeer(22);
  CHECK(!permissions.restore(ephemeral->token, "org.test.Capture", 3, 1, 22));
}

ANDROMEDA_TEST(restoringForOneUseConsumesPermissionWithoutIssuingAnotherToken) {
  Fixture fixture;
  InputCapturePermissions permissions(fixture.path);
  const auto durable = permissions.issue("org.test.Capture", 3, 2, 11);
  CHECK(durable.has_value());
  if (!durable) {
    return;
  }
  const auto once = permissions.restore(durable->token, "org.test.Capture", 3, 0, 22);
  CHECK(once.has_value());
  if (once) {
    CHECK_EQ(once->persistMode, 0U);
    CHECK(once->token.empty());
  }
  CHECK(!permissions.restore(durable->token, "org.test.Capture", 3, 2, 11));
  CHECK(permissions.revoke(durable->id));
  InputCapturePermissions reopened(fixture.path);
  CHECK(reopened.list().empty());
}

ANDROMEDA_TEST(durableCredentialsUsePrivateFilesAndTightenTheirOwnedDirectory) {
  Fixture fixture;
  std::filesystem::create_directories(fixture.path.parent_path());
  CHECK_EQ(chmod(fixture.path.parent_path().c_str(), 0755), 0);
  InputCapturePermissions permissions(fixture.path);
  const auto grant = permissions.issue("org.test.Capture", 3, 2, 11);
  CHECK(grant.has_value());
  struct stat file{};
  struct stat directory{};
  CHECK_EQ(stat(fixture.path.c_str(), &file), 0);
  CHECK_EQ(stat(fixture.path.parent_path().c_str(), &directory), 0);
  CHECK_EQ(file.st_mode & 0777, 0600U);
  CHECK_EQ(directory.st_mode & 0777, 0700U);
  CHECK_EQ(file.st_uid, geteuid());
  if (grant) {
    CHECK(permissions.revoke(grant->id));
    InputCapturePermissions reopened(fixture.path);
    CHECK(!reopened.restore(grant->token, "org.test.Capture", 3, 2, 11));
  }
}

ANDROMEDA_TEST(durableTransactionsCannotBypassTheStoreLockOrConsumeTokensWhileBusy) {
  Fixture fixture;
  fixture.write(document(record()));
  InputCapturePermissions permissions(fixture.path);
  const auto lockPath = fixture.path.parent_path() / ".input-capture-permissions.lock";
  const int lock = open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  CHECK(lock >= 0);
  if (lock < 0) {
    return;
  }
  CHECK_EQ(flock(lock, LOCK_EX | LOCK_NB), 0);
  CHECK(!permissions.restore(kToken, "org.test.Capture", 3, 2, 11));
  CHECK(!permissions.issue("org.test.New", 3, 2, 11));
  CHECK(!permissions.revoke(kId));
  CHECK_EQ(close(lock), 0);
  CHECK(permissions.restore(kToken, "org.test.Capture", 3, 2, 11).has_value());
}

ANDROMEDA_TEST(malformedStoresNeverPartiallyAuthorizeOrGetOverwrittenByNewGrants) {
  constexpr std::string_view otherToken = "3333333333333333333333333333333333333333333333333333333333333333";
  const std::array documents{
      std::string{"{"},
      std::string{R"({"version":2,"grants":[]})"},
      std::string{R"({"version":1,"grants":{}})"},
      document(record() + "," + record(kId, otherToken)),
      document(record() + "," + record("44444444444444444444444444444444", otherToken, 4)),
      document(record("short-id")),
      document(record(kId, kToken, 0x100000003)),
      document(record(kId, kToken, 7)),
  };
  for (const auto& bytes : documents) {
    Fixture fixture;
    fixture.write(bytes);
    rejectedStore(fixture);
  }
}

ANDROMEDA_TEST(insecureOversizedAndNonregularStoresCannotRestoreCredentials) {
  {
    Fixture fixture;
    fixture.write(document(record()), 0644);
    rejectedStore(fixture);
  }
  {
    Fixture fixture;
    fixture.write(document(record()) + std::string(256 * 1024, ' '));
    rejectedStore(fixture);
  }
  {
    Fixture fixture;
    std::filesystem::create_directories(fixture.path.parent_path());
    CHECK_EQ(mkfifo(fixture.path.c_str(), 0600), 0);
    InputCapturePermissions permissions(fixture.path);
    CHECK(!permissions.restore(kToken, "org.test.Capture", 3, 2, 11));
    CHECK(!permissions.issue("org.test.Capture", 3, 2, 11));
    CHECK(std::filesystem::is_fifo(fixture.path));
  }
}

ANDROMEDA_TEST(symlinkStoresAndDirectoriesCannotAuthorizeOrReplaceTheirTargets) {
  {
    Fixture fixture;
    const std::string bytes = document(record());
    fixture.write(bytes);
    const auto target = fixture.path.parent_path() / "target.json";
    std::filesystem::rename(fixture.path, target);
    std::filesystem::create_symlink(target, fixture.path);
    rejectedStore(fixture);
    CHECK(std::filesystem::is_symlink(fixture.path));
  }
  {
    Fixture fixture;
    fixture.write(document(record()));
    const auto alias = fixture.directory / "alias";
    std::filesystem::create_directory_symlink(fixture.path.parent_path(), alias);
    InputCapturePermissions permissions(alias / fixture.path.filename());
    CHECK(!permissions.restore(kToken, "org.test.Capture", 3, 2, 11));
    CHECK(!permissions.issue("org.test.New", 3, 2, 11));
    CHECK(fixture.read() == document(record()));
  }
}

// Grants whose token nobody holds any more (the portal row was revoked, the app was removed) must not block new ones:
// a full store drops the durable grant used least recently, also across a reload.
ANDROMEDA_TEST(aFullStoreDropsTheGrantUsedLeastRecently) {
  Fixture fixture;
  std::vector<std::string> tokens;
  {
    InputCapturePermissions permissions(fixture.path);
    for (int index = 0; index < 128; ++index) {
      const auto grant = permissions.issue("org.test.Capture", 3, 2, 11, InputPermissionKind::Emulate);
      CHECK(grant.has_value());
      tokens.push_back(grant ? grant->token : std::string{});
    }
    const auto used = permissions.restore(tokens[0], "org.test.Capture", 3, 2, 11, InputPermissionKind::Emulate);
    CHECK(used.has_value());
    tokens[0] = used ? used->token : std::string{};
  }
  InputCapturePermissions reloaded(fixture.path);
  CHECK(reloaded.issue("org.test.New", 3, 2, 22, InputPermissionKind::Emulate).has_value());
  CHECK(!reloaded.restore(tokens[1], "org.test.Capture", 3, 2, 11, InputPermissionKind::Emulate));
  CHECK(reloaded.restore(tokens[0], "org.test.Capture", 3, 2, 11, InputPermissionKind::Emulate).has_value());
  CHECK(reloaded.restore(tokens[2], "org.test.Capture", 3, 2, 11, InputPermissionKind::Emulate).has_value());
  CHECK_EQ(reloaded.list().size(), 128U);
}

namespace {
  // An ELF copy of this test binary.
  void writeFile(const std::filesystem::path& path, mode_t mode) {
    std::filesystem::copy_file("/proc/self/exe", path, std::filesystem::copy_options::overwrite_existing);
    if (chmod(path.c_str(), mode) != 0) {
      throw std::runtime_error("could not set a test binary's mode");
    }
  }

  std::string binariesDocument(std::string_view binaries) {
    return std::format(R"({{"version":2,"grants":[{}],"binaries":[{}]}})", record(), binaries);
  }
} // namespace

// Settings > Privacy: random add, remove, restart and a second compositor sharing the store, from an empty store or a
// version 1 file. After every step each instance lists exactly the binaries the model holds, a client of each binary
// gets the User grant exactly when it is listed, and the durable capture grant still restores.
ANDROMEDA_TEST(trustedBinariesFollowAddRemoveRestartAndASecondInstance) {
  for (uint32_t seed = 1; seed <= 20; ++seed) {
    Fixture fixture;
    if (seed % 2 == 0) {
      fixture.write(document(record()));
    }
    const auto bin = fixture.directory / "bin";
    std::filesystem::create_directories(bin);
    writeFile(bin / "a", 0700);
    writeFile(bin / "b", 0755);
    writeFile(bin / "plain", 0644);
    std::filesystem::create_symlink(bin / "a", bin / "alias");
    const std::string a = (bin / "a").native();
    const std::string b = (bin / "b").native();
    const std::array<std::string, 6> candidates{
        a, b, (bin / "plain").native(), (bin / "alias").native(), (bin / "missing").native(), bin.native()
    };

    std::array<std::optional<InputCapturePermissions>, 2> instances;
    instances[0].emplace(fixture.path);
    instances[1].emplace(fixture.path);
    std::vector<std::string> model;
    std::mt19937 random(seed);
    for (int step = 0; step < 60; ++step) {
      auto& store = *instances[random() % 2];
      const std::string& path = candidates[random() % candidates.size()];
      switch (random() % 3) {
      case 0: {
        const auto added = store.addBinary(path);
        const std::string expected = path == candidates[3] ? a : path;
        const bool executable = expected == a || expected == b;
        CHECK_EQ(added.has_value(), executable);
        if (added) {
          CHECK_EQ(*added, expected);
          if (!std::ranges::contains(model, expected)) {
            model.push_back(expected);
          }
        }
        break;
      }
      case 1: {
        const std::string listed = path == candidates[3] ? a : path;
        CHECK_EQ(store.removeBinary(path), std::ranges::contains(model, listed));
        std::erase(model, listed);
        break;
      }
      default:
        instances[random() % 2].emplace(fixture.path);
      }
      for (auto& instance : instances) {
        const auto listed = instance->binaries();
        CHECK(listed == model);
        for (const auto& exe : {a, b}) {
          const auto grant = andromeda::grantFor({.exe = exe, .greeterMode = false, .userBinaries = listed, .testBinaries = {}});
          CHECK(grant == (std::ranges::contains(model, exe) ? andromeda::Grant::User : andromeda::Grant::None));
        }
      }
    }
    if (seed % 2 == 0) {
      CHECK(instances[0]->restore(kToken, "org.test.Capture", 3, 2, 11).has_value());
    }
  }
}

ANDROMEDA_TEST(aTrustedBinaryListIsCappedAndABadEntryRefusesTheStore) {
  {
    Fixture fixture;
    const auto bin = fixture.directory / "bin";
    std::filesystem::create_directories(bin);
    InputCapturePermissions permissions(fixture.path);
    for (int index = 0; index <= 64; ++index) {
      writeFile(bin / std::to_string(index), 0700);
      CHECK_EQ(permissions.addBinary((bin / std::to_string(index)).native()).has_value(), index < 64);
    }
    CHECK_EQ(InputCapturePermissions(fixture.path).binaries().size(), 64U);
  }
  {
    Fixture fixture;
    const auto bin = fixture.directory / "bin";
    std::filesystem::create_directories(bin);
    std::ofstream(bin / "script") << "#!/bin/sh\n";
    chmod((bin / "script").c_str(), 0700);
    writeFile(bin / "app", 0700);
    InputCapturePermissions permissions(fixture.path);
    CHECK(permissions.addBinary((bin / "script").native()).error() == "script");
    CHECK(permissions.addBinary("bin/app").error() == "not-absolute");
    for (const std::string_view runner : {"python3.12", "ld-linux-x86-64.so.2", "electron33", "sh"}) {
      writeFile(bin / runner, 0700);
      const auto added = permissions.addBinary((bin / runner).native());
      CHECK(!added && added.error() == "runs-other-code");
    }
    writeFile(bin / "node-red", 0700);
    CHECK(permissions.addBinary((bin / "node-red").native()).has_value());
    CHECK(permissions.removeBinary((bin / "node-red").native()));
    CHECK(permissions.addBinary((bin / "app").native()).has_value());
    CHECK(fixture.read().contains(R"("version":2)"));
    CHECK(permissions.removeBinary((bin / "app").native()));
    // An empty list writes version 1, which an older compositor still reads.
    CHECK(fixture.read().contains(R"("version":1)") && !fixture.read().contains("binaries"));
  }
  for (const std::string_view binaries : {R"(1)", R"("bin/a")", R"("")", R"("/bin/a","/bin/a")"}) {
    Fixture fixture;
    fixture.write(binariesDocument(binaries));
    rejectedStore(fixture);
    InputCapturePermissions permissions(fixture.path);
    CHECK(permissions.binaries().empty());
  }
  Fixture fixture;
  fixture.write(binariesDocument(R"("/bin/a")"));
  InputCapturePermissions permissions(fixture.path);
  CHECK(permissions.binaries() == std::vector<std::string>{"/bin/a"});
  CHECK(permissions.restore(kToken, "org.test.Capture", 3, 2, 11).has_value());
}

// Results arrive late (the loop is pumped at random) and a restart drops the results still queued. An acknowledged
// revoke stays gone from the file and its token never restores; every other acknowledged grant stays on file.
ANDROMEDA_TEST(queuedChangesSettleToTheFileAndAcknowledgedRevokesStayGone) {
  for (uint32_t seed = 1; seed <= 30; ++seed) {
    const Fixture fixture;
    struct Known {
      std::string token;
      std::string application;
      bool revoked = false;
    };
    // Grants by id. An id whose change was queued when the store restarted is uncertain: its result was dropped.
    std::map<std::string, Known> known;
    std::set<std::string> uncertain;
    enum class Op : uint8_t { Issue, Restore, Revoke, RevokeAll };
    struct Pending {
      Op op;
      std::string id;
    };
    std::map<int, Pending> pending;
    int nextOp = 0;
    auto store = std::make_unique<andromeda::InputCapturePermissions>(test::eventLoop(), fixture.path);
    std::mt19937 random(seed);

    // Grants with no change queued: a queued restore rotates the token this step would send.
    const auto live = [&] {
      std::vector<std::string> ids;
      if (std::ranges::any_of(pending, [](const auto& entry) { return entry.second.op == Op::RevokeAll; })) {
        return ids;
      }
      for (const auto& [id, grant] : known) {
        const bool queued = std::ranges::any_of(pending, [&](const auto& entry) { return entry.second.id == id; });
        if (!grant.revoked && !uncertain.contains(id) && !queued) {
          ids.push_back(id);
        }
      }
      return ids;
    };
    for (int step = 0; step < 80; ++step) {
      const int op = nextOp++;
      const std::vector<std::string> ids = live();
      std::string application;
      switch (random() % 6) {
      case 0:
      case 1:
        pending[op] = {Op::Issue, {}};
        application = "org.test.App" + std::to_string(random() % 4);
        store->issue(
            application, 3, 2, 11, InputPermissionKind::Capture,
            [&, op, application](std::optional<InputCaptureGrant> grant) {
              pending.erase(op);
              CHECK(grant.has_value());
              if (grant) {
                known[grant->id] = {grant->token, application, false};
              }
              return true;
            }
        );
        break;
      case 2: {
        if (ids.empty()) {
          break;
        }
        const std::string id = ids[random() % ids.size()];
        pending[op] = {Op::Restore, id};
        // list() takes the store lock on this thread while the saver may be writing.
        CHECK(std::ranges::contains(store->list(), id, &andromeda::InputCapturePermission::id));
        store->restore(
            known[id].token, known[id].application, 3, 2, 11, InputPermissionKind::Capture,
            [&, op, id](std::optional<InputCaptureGrant> grant) {
              pending.erase(op);
              CHECK(grant.has_value() && grant->id == id);
              if (grant) {
                known[id].token = grant->token;
              }
              return true;
            }
        );
        break;
      }
      case 3:
      case 4: {
        if (ids.empty()) {
          break;
        }
        const std::string id = ids[random() % ids.size()];
        pending[op] = {Op::Revoke, id};
        store->revoke(id, [&, op, id](bool revoked) {
          pending.erase(op);
          CHECK(revoked);
          known[id].revoked = revoked;
        });
        break;
      }
      default:
        if (random() % 3 == 0) {
          pending[op] = {Op::RevokeAll, {}};
          store->revoke("all", [&, op](bool revoked) {
            pending.erase(op);
            CHECK(revoked);
            // Results arrive in request order: every grant known now was issued before this revoke.
            for (auto& [id, grant] : known) {
              grant.revoked = true;
            }
          });
        } else {
          for (const auto& [seq, change] : pending) {
            if (change.op == Op::RevokeAll) {
              for (const auto& [id, grant] : known) {
                uncertain.insert(id);
              }
            } else if (!change.id.empty()) {
              uncertain.insert(change.id);
            }
          }
          pending.clear();
          // The old store finishes its queued saves before the next one loads, as across a compositor restart.
          store.reset();
          store = std::make_unique<andromeda::InputCapturePermissions>(test::eventLoop(), fixture.path);
        }
      }
      for (uint32_t pumps = random() % 3; pumps > 0; --pumps) {
        wl_event_loop_dispatch(test::eventLoop(), 0);
      }
    }
    test::pumpUntil([&] { return pending.empty(); });

    const auto ids = [&] {
      std::set<std::string> listed;
      for (const auto& permission : store->list()) {
        listed.insert(permission.id);
      }
      return listed;
    };
    std::set<std::string> memory;
    {
      // While another holder has the lock, list() answers from memory instead of the file.
      const andromeda::private_state::Lock held(fixture.path, ".input-capture-permissions.lock");
      CHECK(static_cast<bool>(held));
      memory = ids();
    }
    const std::set<std::string> stored = ids();
    CHECK(memory == stored);
    for (const auto& [id, grant] : known) {
      if (grant.revoked) {
        CHECK(!stored.contains(id));
        CHECK(!test::await<std::optional<InputCaptureGrant>>([&](auto done) {
          store->restore(grant.token, grant.application, 3, 2, 11, InputPermissionKind::Capture, delivered(std::move(done)));
        }));
      } else if (!uncertain.contains(id)) {
        CHECK(stored.contains(id));
      }
    }
  }
}

// A grant whose client is gone when the store answers is undone: an issued one leaves the file, and a durable restore
// gives the old token back, so the app can still use it.
ANDROMEDA_TEST(aGrantThatReachesNoClientIsUndone) {
  const Fixture fixture;
  InputCapturePermissions permissions(fixture.path);
  const auto kept = permissions.issue("org.test.Capture", 3, 2, 11);
  CHECK(kept.has_value());
  if (!kept) {
    return;
  }
  bool answered = false;
  permissions.store.restore(
      kept->token, "org.test.Capture", 3, 2, 11, InputPermissionKind::Capture,
      [&](std::optional<InputCaptureGrant> grant) {
        answered = true;
        CHECK(grant.has_value());
        return false;
      }
  );
  test::pumpUntil([&] { return answered; });
  std::optional<InputCaptureGrant> dropped;
  permissions.store.issue("org.test.Other", 3, 2, 11, InputPermissionKind::Capture, [&](auto grant) {
    dropped = grant;
    return false;
  });
  test::pumpUntil([&] { return dropped.has_value(); });
  // Queued after the put-back and the revoke, so it sees their result.
  CHECK(permissions.restore(kept->token, "org.test.Capture", 3, 2, 11).has_value());
  CHECK(!InputCapturePermissions(fixture.path).restore(dropped->token, "org.test.Other", 3, 2, 11));
}

int main() { return RUN_TESTS(); }

// Capture and emulation grants share one store: a token restores only its own kind, also across a reload, and a
// stored emulation grant never loads as capture.
ANDROMEDA_TEST(aTokenRestoresOnlyItsOwnKindAcrossReloads) {
  Fixture fixture;
  std::string emulateToken;
  {
    InputCapturePermissions permissions(fixture.path);
    const auto emulate = permissions.issue("org.test.Capture", 3, 2, 11, InputPermissionKind::Emulate);
    const auto capture = permissions.issue("org.test.Capture", 3, 2, 11, InputPermissionKind::Capture);
    CHECK(emulate && capture);
    emulateToken = emulate->token;
    CHECK(!permissions.restore(capture->token, "org.test.Capture", 3, 2, 11, InputPermissionKind::Emulate));
  }
  InputCapturePermissions reloaded(fixture.path);
  CHECK(!reloaded.restore(emulateToken, "org.test.Capture", 3, 2, 11, InputPermissionKind::Capture));
  const auto list = reloaded.list();
  CHECK(std::ranges::count(list, InputPermissionKind::Emulate, &andromeda::InputCapturePermission::kind) == 1);
  CHECK(reloaded.restore(emulateToken, "org.test.Capture", 1, 2, 11, InputPermissionKind::Emulate));
}
