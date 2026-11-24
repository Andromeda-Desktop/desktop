// Model test for clipboard persistence: random captures, pins, removals, clears, retention toggles, key re-syncs and
// recovery resets, with storage jobs delivered at random points. After the jobs settle, memory and disk agree: the
// manifest decrypts and lists the history in order, every payload file is listed and decrypts to its entry.

#include "security/encrypted_file_store.h"
#include "security/storage_key_provider.h"
#include "clipboard/clipboard_service.h"
#include "core/deferred_call.h"
#include "tests/clipboard_fake_compositor.h"
#include "tests/test_deferred.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <print>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {
  using namespace clipboard_test;
  namespace fs = std::filesystem;

  constexpr int kSeeds = 64;
  constexpr int kOps = 200;

  int gFailures = 0;

  bool expect(bool condition, const std::string& message, unsigned seed) {
    if (!condition) {
      std::println(stderr, "clipboard_persistence_model_test: seed {}: {}", seed, message);
      ++gFailures;
    }
    return condition;
  }

  void runSomeDeferred(std::mt19937& rng) {
    auto calls = DeferredCall::takePending();
    const std::size_t keep = calls.empty() ? 0 : rng() % (calls.size() + 1);
    for (std::size_t i = 0; i < keep; ++i) {
      calls[i]();
    }
    for (std::size_t i = keep; i < calls.size(); ++i) {
      DeferredCall::callLater(std::move(calls[i]));
    }
  }

  void checkInvariants(ClipboardService& clipboard, security::StorageKeyProvider& keys, const fs::path& stateHome,
                       unsigned seed) {
    if (!expect(runDeferredUntil([&clipboard] { return !clipboard.persistenceBusy(); }), "jobs did not settle", seed)
        || !expect(clipboard.persistenceState() == ClipboardPersistenceState::Ready, "state is not Ready", seed)) {
      return;
    }
    const fs::path dir = stateHome / "andromeda/shell/clipboard";
    const auto key = keys.deriveKey(security::StorageKeyPurpose::ClipboardHistory);
    if (!expect(key.has_value(), "no key", seed)) {
      return;
    }
    const auto manifest = security::readEncryptedFile(
        dir / "index.enc", *key, security::EncryptionContext{.purpose = "clipboard-manifest-v1", .objectId = "index"},
        64U * 1024U * 1024U
    );
    std::vector<std::string> manifestIds;
    std::vector<bool> manifestPins;
    if (manifest.status == security::EncryptedReadStatus::NotFound) {
      // Nothing persisted yet: the history may only hold nothing.
      expect(clipboard.history().empty(), "history without a manifest", seed);
    } else {
      if (!expect(manifest.succeeded(), "manifest does not decrypt", seed)) {
        return;
      }
      const auto json = nlohmann::json::parse(manifest.plaintext.begin(), manifest.plaintext.end());
      for (const auto& item : json.at("entries")) {
        manifestIds.push_back(item.at("id").get<std::string>());
        manifestPins.push_back(item.value("pinned", false));
      }
    }
    std::vector<std::string> historyIds;
    std::vector<bool> historyPins;
    for (const auto& entry : clipboard.history()) {
      historyIds.push_back(entry.storageId);
      historyPins.push_back(entry.pinned);
    }
    expect(manifestIds == historyIds, "manifest ids differ from history", seed);
    expect(manifestPins == historyPins, "manifest pins differ from history", seed);

    std::set<std::string> files;
    std::error_code ec;
    for (const auto& file : fs::directory_iterator(dir / "entries", ec)) {
      if (file.path().extension() == ".enc") {
        files.insert(file.path().stem().string());
      }
    }
    const std::set<std::string> listed(manifestIds.begin(), manifestIds.end());
    std::string diff;
    for (const auto& id : files) {
      diff += listed.contains(id) ? "" : " extra:" + id;
    }
    for (const auto& id : listed) {
      diff += files.contains(id) ? "" : " missing:" + id;
    }
    expect(diff.empty(), "payload files differ from manifest:" + diff, seed);
    for (const auto& entry : clipboard.history()) {
      const auto payload = security::readEncryptedFile(
          dir / "entries" / (entry.storageId + ".enc"), *key,
          security::EncryptionContext{.purpose = "clipboard-payload-v1", .objectId = entry.storageId},
          64U * 1024U * 1024U
      );
      if (!expect(payload.succeeded() && payload.plaintext.size() == entry.byteSize, "payload does not decrypt", seed)) {
        continue;
      }
      if (entry.payloadLoaded) {
        expect(std::ranges::equal(payload.plaintext, entry.data.span()), "payload bytes differ", seed);
      }
    }
  }

  void runSeed(unsigned seed, const fs::path& root, const fs::path& keyFile) {
    const fs::path stateHome = root / std::to_string(seed);
    ::setenv("ANDROMEDA_STATE_HOME", stateHome.c_str(), 1);
    std::mt19937 rng(seed);
    FakeCompositor fake;
    gFake = &fake;
    int offers[kOps] = {};

    security::SecretStore store{std::make_unique<UnavailableSecretBackend>()};
    security::StorageKeyProvider keys{store};
    ClipboardService clipboard{keys};
    keys.setChangeCallback([&clipboard] { clipboard.syncPersistence(); });
    (void)clipboard.bind(&gManager, &fakeOps(), gSeat);
    keys.configure(StorageKeySource::File, keyFile.string(), false);
    bool retention = true;

    for (int op = 0; op < kOps; ++op) {
      const std::size_t size = clipboard.history().size();
      switch (rng() % 12) {
      case 0:
      case 1:
      case 2:
      case 3: {
        const std::string text = "seed" + std::to_string(seed) + "-op" + std::to_string(op);
        simulateCopy(clipboard, &offers[op], "text/plain;charset=utf-8", {text.begin(), text.end()});
        break;
      }
      case 4:
        if (size > 0) {
          const std::size_t index = rng() % size;
          (void)clipboard.setEntryPinned(index, !clipboard.history()[index].pinned);
        }
        break;
      case 5:
        if (size > 0) {
          (void)clipboard.removeHistoryEntry(rng() % size);
        }
        break;
      case 6:
        if (rng() % 3 == 0) {
          clipboard.clearHistory();
        } else {
          clipboard.clearUnpinnedHistory();
        }
        break;
      case 7:
        clipboard.syncPersistence();
        break;
      case 8:
        if (rng() % 4 == 0) {
          clipboard.clearEncryptedPersistenceForRecovery([&keys](bool cleared) {
            if (cleared) {
              keys.resetAfterEncryptedDataCleared([](bool) {});
            }
          });
        }
        break;
      case 9:
        retention = !retention;
        clipboard.setHistoryRetentionEnabled(retention);
        break;
      default:
        runSomeDeferred(rng);
        break;
      }
      if (retention && rng() % 16 == 0) {
        checkInvariants(clipboard, keys, stateHome, seed);
      }
    }
    clipboard.setHistoryRetentionEnabled(true);
    checkInvariants(clipboard, keys, stateHome, seed);
    gFake = nullptr;
  }

} // namespace

int main() {
  if (!security::initializeSecurityPrimitives()) {
    std::println(stderr, "clipboard_persistence_model_test: libsodium init failed");
    return 1;
  }
  const auto serial = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path root = fs::temp_directory_path() / ("clipboard-persistence-model-" + std::to_string(serial));
  fs::create_directories(root);
  const fs::path keyFile = root / "key";
  std::ofstream(keyFile) << "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\n";

  for (unsigned seed = 1; seed <= kSeeds; ++seed) {
    runSeed(seed, root, keyFile);
  }
  ::unsetenv("ANDROMEDA_STATE_HOME");
  fs::remove_all(root);
  return gFailures == 0 ? 0 : 1;
}
