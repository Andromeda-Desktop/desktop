#include "security/secret_service_state.h"

#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <optional>
#include <print>
#include <random>
#include <string_view>
#include <utility>

namespace {
  using security::SecretConsumerStates;
  using security::SecretServiceConsumers;
  using security::SecretServiceRetries;
  using security::StorageKeyState;

  enum class CalendarState : std::uint8_t { Opening, Ready, Unavailable, DeniedOrLocked };

  // Drives SecretServiceRetries with random event sequences against a fake Secret Service. The fakes stand in for
  // D-Bus only: the deferred queue for DeferredCall, the coalescing probe for SecretCollectionProbe. Probe answers
  // and consumer retries complete in any order relative to user and provider events.
  class Model {
  public:
    explicit Model(std::uint32_t seed) : m_rng(seed) {
      m_provider = coin();
      m_unlocked = m_provider && coin();
      m_storage = m_provider && coin() ? StorageKeyState::DeniedOrLocked : StorageKeyState::Unavailable;
      m_calendar = coin() ? CalendarState::Unavailable : CalendarState::Ready;
      m_calendarMissingTokens = pick(4) == 0;
      m_retries.emplace(
          m_provider, [this] { m_deferred.emplace_back([this] { requestProbe(); }); },
          SecretServiceConsumers{
              .states = [this] { return states(); },
              .retryStorageKey = [this] { retryStorageKey(); },
              .retryCalendar = [this] { retryCalendar(); },
          }
      );
      m_retries->collectionChanged();
    }

    bool run(std::uint32_t seed) {
      for (int step = 0; step < 80 && m_ok; ++step) {
        randomEvent();
      }
      settle();
      // Liveness: once everything has answered, a reachable unlocked collection leaves no consumer behind.
      if (m_ok && m_provider && m_unlocked) {
        check(m_storage == StorageKeyState::Ready, "storage key not reopened after the collection unlocked");
        check(m_calendarMissingTokens || m_calendar == CalendarState::Ready, "calendar not retried after unlock");
      }
      if (!m_ok) {
        std::println(stderr, "secret_service_state_test: seed {}: {}", seed, m_failure);
      }
      return m_ok;
    }

  private:
    bool coin() { return pick(2) == 0; }
    int pick(int n) { return std::uniform_int_distribution<int>(0, n - 1)(m_rng); }

    void check(bool condition, std::string_view failure) {
      if (!condition && m_ok) {
        m_ok = false;
        m_failure = failure;
      }
    }

    void randomEvent() {
      switch (pick(12)) {
      case 0: // A provider claims the name: started, restarted or replaced; it may come up unlocked (PAM).
        m_provider = true;
        m_unlocked = coin();
        ++m_freshChances;
        nameChanged(true);
        break;
      case 1:
        if (m_provider) {
          m_provider = false;
          m_unlocked = false;
          nameChanged(false);
        }
        break;
      case 2:
        if (m_provider && !m_unlocked) {
          unlock();
        }
        break;
      case 3: // gnome-keyring and KeePassXC send CollectionChanged on lock.
        if (m_provider && m_unlocked) {
          m_unlocked = false;
          m_retries->collectionChanged();
        }
        break;
      case 4: // An item changed: CollectionChanged without a lock change.
        if (m_provider) {
          m_retries->collectionChanged();
        }
        break;
      case 5: // The calendar refreshes a token: a lookup that can fail on a locked collection.
        if (m_calendar == CalendarState::Ready && !m_calendarInFlight) {
          m_calendarInFlight = true;
          completeCalendar();
        }
        break;
      case 6:
      case 7:
        runDeferred();
        break;
      case 8:
        deliverProbe();
        break;
      case 9:
        completeStorage();
        break;
      default:
        completeCalendar();
        break;
      }
    }

    void settle() {
      for (int i = 0; i < 1000 && m_ok; ++i) {
        if (!m_deferred.empty()) {
          runDeferred();
        } else if (m_probeInFlight) {
          deliverProbe();
        } else if (m_storageInFlight) {
          completeStorage();
        } else if (m_calendarInFlight) {
          completeCalendar();
        } else {
          return;
        }
      }
      check(false, "retries never settle");
    }

    void unlock() {
      m_unlocked = true;
      ++m_freshChances;
      m_retries->collectionChanged();
    }

    // SecretServiceWatch::Impl and SecretCollectionProbe: a name change invalidates the probe in flight; a request
    // while one is in flight reruns it after the answer.
    void nameChanged(bool owned) {
      m_probeInFlight = false;
      m_probeRerun = false;
      m_retries->providerChanged(owned);
    }

    void requestProbe() {
      if (m_probeInFlight) {
        m_probeRerun = true;
      } else {
        m_probeInFlight = true;
      }
    }

    void deliverProbe() {
      if (!m_probeInFlight) {
        return;
      }
      m_probeInFlight = false;
      const bool rerun = std::exchange(m_probeRerun, false);
      m_lastAnswerUnlocked = m_provider && m_unlocked;
      m_retries->probed(m_lastAnswerUnlocked);
      if (rerun) {
        requestProbe();
      }
    }

    // Application: consumer states and retries.
    SecretConsumerStates states() const {
      return {
          .storageKey = m_storage,
          .calendarDeniedOrLocked = m_calendar == CalendarState::DeniedOrLocked,
          .calendarRetryNeeded = m_calendar == CalendarState::Unavailable
              || m_calendar == CalendarState::DeniedOrLocked || m_calendarMissingTokens,
      };
    }

    void retryStorageKey() {
      check(m_storage != StorageKeyState::DeniedOrLocked || m_lastAnswerUnlocked,
            "locked storage key retried while the collection answered locked");
      check(m_storageRetryChance != m_freshChances, "storage key retried twice without a fresh chance");
      m_storageRetryChance = m_freshChances;
      m_deferred.emplace_back([this] {
        m_storage = StorageKeyState::Opening;
        m_storageInFlight = true;
        m_retries->consumerChanged();
      });
    }

    void retryCalendar() {
      check(m_calendarRetryChance != m_freshChances, "calendar retried twice without a fresh chance");
      m_calendarRetryChance = m_freshChances;
      m_deferred.emplace_back([this] {
        m_calendar = CalendarState::Opening;
        m_calendarInFlight = true;
        m_retries->consumerChanged();
      });
    }

    void runDeferred() {
      if (m_deferred.empty()) {
        return;
      }
      auto call = std::move(m_deferred.front());
      m_deferred.pop_front();
      call();
    }

    // A lookup against a locked collection prompts; the user either unlocks there or dismisses it.
    bool lookupSucceeds() {
      if (m_provider && !m_unlocked && coin()) {
        unlock();
      }
      return m_provider && m_unlocked;
    }

    void completeStorage() {
      if (!std::exchange(m_storageInFlight, false)) {
        return;
      }
      m_storage = !m_provider        ? StorageKeyState::Unavailable
          : lookupSucceeds()         ? StorageKeyState::Ready
                                     : StorageKeyState::DeniedOrLocked;
      m_retries->consumerChanged();
    }

    void completeCalendar() {
      if (!std::exchange(m_calendarInFlight, false)) {
        return;
      }
      m_calendar = !m_provider ? CalendarState::Unavailable
          : lookupSucceeds()   ? CalendarState::Ready
                               : CalendarState::DeniedOrLocked;
      m_retries->consumerChanged();
    }

    std::mt19937 m_rng;
    bool m_ok = true;
    std::string_view m_failure;

    bool m_provider = false;
    bool m_unlocked = false;
    int m_freshChances = 0;

    std::optional<SecretServiceRetries> m_retries;
    bool m_probeInFlight = false;
    bool m_probeRerun = false;
    bool m_lastAnswerUnlocked = false;
    std::deque<std::function<void()>> m_deferred;

    StorageKeyState m_storage = StorageKeyState::Unavailable;
    bool m_storageInFlight = false;
    int m_storageRetryChance = -1;
    CalendarState m_calendar = CalendarState::Ready;
    bool m_calendarMissingTokens = false;
    bool m_calendarInFlight = false;
    int m_calendarRetryChance = -1;
  };
} // namespace

int main() {
  int failures = 0;
  for (std::uint32_t seed = 1; seed <= 20000 && failures < 5; ++seed) {
    Model model(seed);
    failures += model.run(seed) ? 0 : 1;
  }
  return failures == 0 ? 0 : 1;
}
