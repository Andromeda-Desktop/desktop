#pragma once

#include "security/storage_key_provider.h"

#include <cstdint>
#include <functional>
#include <memory>

class SessionBus;

namespace security {

  struct SecretConsumerStates {
    StorageKeyState storageKey = StorageKeyState::Opening;
    bool calendarDeniedOrLocked = false;
    bool calendarRetryNeeded = false;
  };

  struct SecretServiceConsumers {
    std::function<SecretConsumerStates()> states;
    std::function<void()> retryStorageKey;
    std::function<void()> retryCalendar;
  };

  // Decides when Secret Service consumers retry: each at most once per fresh chance, which is a provider that claimed
  // the name or a default collection that went from locked to unlocked. It has no D-Bus: SecretServiceWatch feeds it
  // name changes, collection signals and lock probe answers, and runs the probes it requests.
  class SecretServiceRetries final {
  public:
    // Probing: a provider owns the name and no lock probe has answered since it claimed it.
    enum class State : std::uint8_t { NoProvider, Probing, Locked, Unlocked };

    SecretServiceRetries(bool providerOwned, std::function<void()> requestProbe, SecretServiceConsumers consumers);

    void providerChanged(bool owned);
    // A collection was created or changed: it may have locked or unlocked.
    void collectionChanged();
    // A lock probe answer; unlocked false also covers a missing or unreadable default collection.
    void probed(bool unlocked);
    // A consumer changed state; the Secret Service did not.
    void consumerChanged();

    [[nodiscard]] State state() const { return m_state; }

  private:
    void requestProbe();
    void retry(bool collectionUnlocked);

    std::function<void()> m_requestProbe;
    SecretServiceConsumers m_consumers;
    State m_state;
    // A consumer failed against the collection: an unchanged unlocked answer still retries a locked storage key.
    bool m_answerRequested = false;
    bool m_storageKeyRetried = false;
    bool m_calendarRetried = false;
  };

  // Watches the Secret Service provider name and its collections, and retries consumers through SecretServiceRetries:
  // a provider often claims the name, or unlocks its default collection, after the first lookups failed. Destroy it
  // before the bus.
  class SecretServiceWatch final {
  public:
    SecretServiceWatch();
    ~SecretServiceWatch();

    SecretServiceWatch(const SecretServiceWatch&) = delete;
    SecretServiceWatch& operator=(const SecretServiceWatch&) = delete;

    void start(SessionBus& bus, SecretServiceConsumers consumers);
    // A consumer changed state.
    void consumerChanged();

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace security
