#include "calendar/calendar_service.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "i18n/i18n_service.h"
#include "net/http_client.h"
#include "security/secret_store.h"
#include "security/storage_key_provider.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <poll.h>
#include <print>
#include <string>
#include <unistd.h>

namespace {

  int g_failures = 0;

  void expect(bool condition, const std::string& message) {
    if (!condition) {
      std::println(stderr, "calendar_stale_refresh_test: FAIL: {}", message);
      ++g_failures;
    }
  }

  class UnavailableBackend final : public security::SecretStoreBackend {
  public:
    security::SecretStoreBackendResult probe(security::SecretStoreCancellation&) override { return unavailable(); }
    security::SecretStoreBackendResult
    lookup(const security::SecretStoreAttributes&, security::SecretStoreCancellation&) override {
      return unavailable();
    }
    security::SecretStoreBackendResult store(
        const security::SecretStoreAttributes&, std::span<const std::uint8_t>, const std::string&,
        security::SecretStoreCancellation&
    ) override {
      return unavailable();
    }
    security::SecretStoreBackendResult
    erase(const security::SecretStoreAttributes&, security::SecretStoreCancellation&) override {
      return unavailable();
    }

  private:
    static security::SecretStoreBackendResult unavailable() {
      return {
          .status = security::SecretStoreStatus::Unavailable,
          .errorCategory = security::SecretStoreErrorCategory::ProviderUnavailable,
      };
    }
  };

  void writeConfig(const std::filesystem::path& path, const std::filesystem::path& vdir, bool enabled) {
    std::ofstream file(path);
    file << "[calendar]\nenabled = " << (enabled ? "true" : "false") << "\n[calendar.account.work]\ntype = \"vdir\"\n"
         << "path = \"" << vdir.string() << "\"\n";
  }

  std::string icsTimestamp(std::chrono::system_clock::time_point tp) {
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm utc{};
    gmtime_r(&t, &utc);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &utc);
    return buf;
  }

  // Returns the number of events after the vdir reply arrives; the config change (if any) lands before it.
  std::size_t eventsAfterLateReply(const std::filesystem::path& root, bool disableMidRefresh) {
    const auto configPath = root / "config" / "andromeda" / "shell" / "config.toml";
    const auto vdir = root / "vdir";
    writeConfig(configPath, vdir, true);

    ConfigService config;
    HttpClient http;
    http.setOfflineMode(true);
    security::SecretStore store{std::make_unique<UnavailableBackend>()};
    security::StorageKeyProvider keys{store};
    CalendarService calendar(config, http, store, keys);
    calendar.initialize();

    const CalendarConfig::Account account = config.config().calendar.accounts.at(0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!calendar.accountStatus(account).phase.has_value()) {
      if (std::chrono::steady_clock::now() > deadline) {
        expect(false, "refresh did not start");
        return 0;
      }
      calendar.tick();
      if (calendar.accountStatus(account).phase.has_value()) {
        break;
      }
      for (auto& call : DeferredCall::takePending()) {
        call();
      }
      pollfd pfd{.fd = DeferredCall::wakeFd(), .events = POLLIN, .revents = 0};
      if (poll(&pfd, 1, 10) > 0) {
        DeferredCall::drainWakeFd();
      }
    }

    if (disableMidRefresh) {
      writeConfig(configPath, vdir, false);
      config.forceReload();
    }
    // The worker reply is still queued in DeferredCall: only this pump delivers it.
    const auto replyBy = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::size_t delivered = 0;
    while (delivered == 0 && std::chrono::steady_clock::now() < replyBy) {
      auto calls = DeferredCall::takePending();
      for (auto& call : calls) {
        call();
      }
      delivered = calls.size();
      pollfd pfd{.fd = DeferredCall::wakeFd(), .events = POLLIN, .revents = 0};
      if (delivered == 0 && poll(&pfd, 1, 10) > 0) {
        DeferredCall::drainWakeFd();
      }
    }
    expect(delivered == 1, "exactly the worker reply was delivered after the refresh started");
    return calendar.snapshot().events.size();
  }

} // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() / ("noctalia-calendar-stale-test-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "config" / "andromeda" / "shell");
  std::filesystem::create_directories(root / "state");
  std::filesystem::create_directories(root / "cache");
  std::filesystem::create_directories(root / "vdir" / "cal");
  ::setenv("ANDROMEDA_CONFIG_HOME", (root / "config").c_str(), 1);
  ::setenv("ANDROMEDA_STATE_HOME", (root / "state").c_str(), 1);
  ::setenv("XDG_CACHE_HOME", (root / "cache").c_str(), 1);
  i18n::Service::instance().init("en");

  const auto start = std::chrono::system_clock::now() + std::chrono::hours(24);
  {
    std::ofstream ics(root / "vdir" / "cal" / "e.ics");
    ics << "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:stale-1\r\nSUMMARY:Late\r\nDTSTART:"
        << icsTimestamp(start) << "\r\nDTEND:" << icsTimestamp(start + std::chrono::hours(1))
        << "\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
  }

  expect(eventsAfterLateReply(root, false) == 1, "enabled calendar should publish the vdir event");
  std::filesystem::remove_all(root / "cache");
  std::filesystem::create_directories(root / "cache");
  expect(eventsAfterLateReply(root, true) == 0, "a reply after the calendar was disabled must not publish events");

  std::filesystem::remove_all(root);
  return g_failures == 0 ? 0 : 1;
}
