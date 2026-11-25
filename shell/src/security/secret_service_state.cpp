#include "security/secret_service_state.h"

#include "core/log.h"

#include <utility>

namespace security {

  namespace {
    constexpr Logger kLog("secret-store");
  } // namespace

  SecretServiceRetries::SecretServiceRetries(
      bool providerOwned, std::function<void()> requestProbe, SecretServiceConsumers consumers
  )
      : m_requestProbe(std::move(requestProbe)), m_consumers(std::move(consumers)),
        m_state(providerOwned ? State::Probing : State::NoProvider) {}

  void SecretServiceRetries::providerChanged(bool owned) {
    m_state = owned ? State::Probing : State::NoProvider;
    m_answerRequested = false;
    requestProbe();
  }

  void SecretServiceRetries::collectionChanged() { requestProbe(); }

  void SecretServiceRetries::probed(bool unlocked) {
    const State from = m_state;
    const bool answerRequested = std::exchange(m_answerRequested, false);
    if (from == State::NoProvider) {
      return;
    }
    m_state = unlocked ? State::Unlocked : State::Locked;
    if (!unlocked && from != State::Probing) {
      return;
    }
    if (from == State::Unlocked) {
      if (answerRequested) {
        retry(true);
      }
      return;
    }
    // A provider that just claimed the name, or a collection that just unlocked, is a fresh chance for every consumer.
    if (unlocked) {
      kLog.info("secret service default collection unlocked");
    } else {
      kLog.info("secret service provider appeared");
    }
    m_storageKeyRetried = false;
    m_calendarRetried = false;
    retry(unlocked);
  }

  void SecretServiceRetries::consumerChanged() {
    if (m_state != State::Locked && m_state != State::Unlocked) {
      return;
    }
    retry(false);
    // The answer retries a locked storage key once the collection is unlocked, and catches a lock the signals missed.
    const SecretConsumerStates states = m_consumers.states();
    if (states.storageKey == StorageKeyState::DeniedOrLocked || states.calendarDeniedOrLocked) {
      m_answerRequested = true;
      requestProbe();
    }
  }

  void SecretServiceRetries::requestProbe() {
    if (m_state != State::NoProvider) {
      m_requestProbe();
    }
  }

  void SecretServiceRetries::retry(bool collectionUnlocked) {
    const SecretConsumerStates states = m_consumers.states();
    // A locked storage key is only worth reopening once the collection is unlocked: a lookup then reads silently,
    // whereas retrying while still locked would raise a second keyring prompt.
    if (!m_storageKeyRetried
        && (states.storageKey == StorageKeyState::Unavailable
            || (states.storageKey == StorageKeyState::DeniedOrLocked && collectionUnlocked))) {
      m_storageKeyRetried = true;
      kLog.info("secret service is running; reopening encrypted storage");
      m_consumers.retryStorageKey();
    }
    if (!m_calendarRetried && states.calendarRetryNeeded) {
      m_calendarRetried = true;
      kLog.info("secret service is running; reopening calendar credentials");
      m_consumers.retryCalendar();
    }
  }

} // namespace security
