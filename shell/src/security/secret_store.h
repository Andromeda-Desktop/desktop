#pragma once

#include "security/secure_buffer.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>

class SessionBus;

namespace security {

  inline constexpr std::string_view SecretStoreSchemaName = "dev.noctalia.Secret";

  struct SecretId {
    std::string scope;
    std::string owner;
    std::string name;

    bool operator==(const SecretId&) const = default;
  };

  struct SecretStoreAttributes {
    std::string application;
    std::string scope;
    std::string owner;
    std::string name;
    std::string version;

    bool operator==(const SecretStoreAttributes&) const = default;
  };

  [[nodiscard]] SecretStoreAttributes secretStoreAttributes(const SecretId& id);
  [[nodiscard]] bool isValidSecretId(const SecretId& id);

  enum class SecretStoreStatus {
    Success,
    NotFound,
    Unavailable,
    Cancelled,
    DeniedOrLocked,
    BackendError,
  };

  enum class SecretStoreErrorCategory {
    None,
    ProviderUnavailable,
    Cancelled,
    Locked,
    PermissionDenied,
    Protocol,
    InvalidRequest,
    Other,
  };

  enum class SecretStoreCollectionState {
    Unknown,
    Missing,
    Locked,
    Unlocked,
  };

  struct SecretStoreBackendResult {
    SecretStoreStatus status = SecretStoreStatus::BackendError;
    SecretStoreErrorCategory errorCategory = SecretStoreErrorCategory::Other;
    SecretStoreCollectionState defaultCollectionState = SecretStoreCollectionState::Unknown;
    SecureBuffer value;
  };

  class SecretStoreCancellation {
  public:
    void cancel();
    [[nodiscard]] bool cancelled() const;

    void setCancelCallback(const std::function<void()>& callback);
    void clearCancelCallback();

  private:
    mutable std::mutex m_mutex;
    bool m_cancelled = false;
    std::function<void()> m_cancelCallback;
  };

  class SecretStoreBackend {
  public:
    virtual ~SecretStoreBackend() = default;

    virtual SecretStoreBackendResult probe(SecretStoreCancellation& cancellation) = 0;
    virtual SecretStoreBackendResult
    lookup(const SecretStoreAttributes& attributes, SecretStoreCancellation& cancellation) = 0;
    virtual SecretStoreBackendResult store(
        const SecretStoreAttributes& attributes, std::span<const std::uint8_t> value, const std::string& label,
        SecretStoreCancellation& cancellation
    ) = 0;
    virtual SecretStoreBackendResult
    erase(const SecretStoreAttributes& attributes, SecretStoreCancellation& cancellation) = 0;
    // The worker goes idle: drop connections kept between operations.
    virtual void release() {}
  };

  class SecretStoreOperation {
  public:
    SecretStoreOperation() = default;

    void cancel() const;
    [[nodiscard]] bool cancelled() const;

  private:
    friend class SecretStore;
    explicit SecretStoreOperation(std::weak_ptr<SecretStoreCancellation> cancellation);

    std::weak_ptr<SecretStoreCancellation> m_cancellation;
  };

  enum class SecretServiceEvent : std::uint8_t { ProviderAppeared, CollectionUnlocked };

  class SecretStore final {
  public:
    using LookupCallback = std::function<void(SecretStoreStatus, SecureBuffer)>;
    using StatusCallback = std::function<void(SecretStoreStatus)>;

    SecretStore();
    explicit SecretStore(std::unique_ptr<SecretStoreBackend> backend);
    ~SecretStore();

    SecretStore(const SecretStore&) = delete;
    SecretStore& operator=(const SecretStore&) = delete;

    SecretStoreOperation lookup(const SecretId& id, LookupCallback callback);
    SecretStoreOperation store(const SecretId& id, SecureBuffer value, std::string label, StatusCallback callback);
    SecretStoreOperation erase(const SecretId& id, StatusCallback callback);
    SecretStoreOperation retryAvailabilityCheck(StatusCallback callback = {});

    [[nodiscard]] std::optional<SecretStoreStatus> availabilityStatus() const;

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
  };

  // Watches the Secret Service provider name and its collections. A provider often claims the name, or unlocks its
  // default collection, after the first lookups failed; the callback reports each so consumers can retry. Destroy
  // it before the bus.
  class SecretServiceWatch final {
  public:
    using Callback = std::function<void(SecretServiceEvent)>;

    SecretServiceWatch();
    ~SecretServiceWatch();

    SecretServiceWatch(const SecretServiceWatch&) = delete;
    SecretServiceWatch& operator=(const SecretServiceWatch&) = delete;

    void start(SessionBus& bus, Callback callback);
    // Asks once whether the default collection is unlocked; CollectionUnlocked follows when it is.
    void probeCollection();
    [[nodiscard]] bool providerOwned() const;

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
  };

  // Changes the gnome-keyring login keyring's password, as gnome-control-center does. True also when nothing needs
  // it: no Secret Service, no login keyring, or another service.
  [[nodiscard]] bool changeLoginKeyringPassword(std::span<const std::uint8_t> current, std::span<const std::uint8_t> next);

} // namespace security
