#include "security/secret_store.h"
#include "protocols/crash_capture.h"
#include "util/secure_wipe.h"
#include "util/thread_name.h"

#include "core/log.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <poll.h>
#include <sdbus-c++/sdbus-c++.h>
#include <string_view>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace security {
  namespace {
    constexpr Logger kLog("secret-store");
    constexpr auto kRepeatedErrorLogInterval = std::chrono::seconds(30);
    constexpr auto kWorkerIdleExit = std::chrono::seconds(30);

    enum class OperationKind { Probe, Lookup, Store, Erase };

    const char* operationName(OperationKind kind) {
      switch (kind) {
      case OperationKind::Probe:
        return "probe";
      case OperationKind::Lookup:
        return "lookup";
      case OperationKind::Store:
        return "store";
      case OperationKind::Erase:
        return "erase";
      }
      return "unknown";
    }

    const char* statusName(SecretStoreStatus status) {
      switch (status) {
      case SecretStoreStatus::Success:
        return "success";
      case SecretStoreStatus::NotFound:
        return "not-found";
      case SecretStoreStatus::Unavailable:
        return "unavailable";
      case SecretStoreStatus::Cancelled:
        return "cancelled";
      case SecretStoreStatus::DeniedOrLocked:
        return "denied-or-locked";
      case SecretStoreStatus::BackendError:
        return "backend-error";
      }
      return "unknown";
    }

    const char* categoryName(SecretStoreErrorCategory category) {
      switch (category) {
      case SecretStoreErrorCategory::None:
        return "none";
      case SecretStoreErrorCategory::ProviderUnavailable:
        return "provider-unavailable";
      case SecretStoreErrorCategory::Cancelled:
        return "cancelled";
      case SecretStoreErrorCategory::Locked:
        return "locked";
      case SecretStoreErrorCategory::PermissionDenied:
        return "permission-denied";
      case SecretStoreErrorCategory::Protocol:
        return "protocol";
      case SecretStoreErrorCategory::InvalidRequest:
        return "invalid-request";
      case SecretStoreErrorCategory::Other:
        return "other";
      }
      return "unknown";
    }

    const char* collectionStateName(SecretStoreCollectionState state) {
      switch (state) {
      case SecretStoreCollectionState::Unknown:
        return "unknown";
      case SecretStoreCollectionState::Missing:
        return "missing";
      case SecretStoreCollectionState::Locked:
        return "locked";
      case SecretStoreCollectionState::Unlocked:
        return "unlocked";
      }
      return "unknown";
    }

    SecretStoreBackendResult cancelledResult() {
      return {
          .status = SecretStoreStatus::Cancelled,
          .errorCategory = SecretStoreErrorCategory::Cancelled,
      };
    }

    SecretStoreBackendResult invalidRequestResult() {
      return {
          .status = SecretStoreStatus::BackendError,
          .errorCategory = SecretStoreErrorCategory::InvalidRequest,
      };
    }

    constexpr auto kCallTimeout = std::chrono::seconds(10);
    constexpr const char* kSecretsName = "org.freedesktop.secrets";
    constexpr const char* kServiceInterface = "org.freedesktop.Secret.Service";
    constexpr const char* kCollectionInterface = "org.freedesktop.Secret.Collection";
    constexpr const char* kItemInterface = "org.freedesktop.Secret.Item";
    constexpr const char* kPromptInterface = "org.freedesktop.Secret.Prompt";
    constexpr const char* kSessionInterface = "org.freedesktop.Secret.Session";
    constexpr const char* kNoObject = "/";

    // (session, parameters, value, content type)
    using Secret = sdbus::Struct<sdbus::ObjectPath, std::vector<std::uint8_t>, std::vector<std::uint8_t>, std::string>;
    using Paths = std::vector<sdbus::ObjectPath>;

    SecretStoreBackendResult makeResult(SecretStoreStatus status, SecretStoreErrorCategory category) {
      return {.status = status, .errorCategory = category};
    }

    SecretStoreBackendResult lockedResult() {
      return makeResult(SecretStoreStatus::DeniedOrLocked, SecretStoreErrorCategory::Locked);
    }

    std::map<std::string, std::string> makeAttributes(const SecretStoreAttributes& attributes) {
      return {
          // libsecret adds the schema name on store and search; keep it so secrets stored by libsecret still match.
          {"xdg:schema", std::string(SecretStoreSchemaName)},
          {"application", attributes.application},
          {"scope", attributes.scope},
          {"owner", attributes.owner},
          {"name", attributes.name},
          {"version", attributes.version},
      };
    }

    SecretStoreBackendResult resultFromError(const sdbus::Error& error) {
      const std::string_view name = error.getName();
      if (name == "org.freedesktop.Secret.Error.IsLocked") {
        return lockedResult();
      }
      if (name == "org.freedesktop.DBus.Error.AccessDenied") {
        return makeResult(SecretStoreStatus::DeniedOrLocked, SecretStoreErrorCategory::PermissionDenied);
      }
      if (name == "org.freedesktop.DBus.Error.ServiceUnknown" || name == "org.freedesktop.DBus.Error.NameHasNoOwner"
          || name == "org.freedesktop.DBus.Error.Spawn.ServiceNotFound") {
        return makeResult(SecretStoreStatus::Unavailable, SecretStoreErrorCategory::ProviderUnavailable);
      }
      if (name.starts_with("org.freedesktop.Secret.Error.")) {
        return makeResult(SecretStoreStatus::BackendError, SecretStoreErrorCategory::Protocol);
      }
      return makeResult(SecretStoreStatus::BackendError, SecretStoreErrorCategory::Other);
    }

    struct OperationCancelled {};

    // Owned by the cancel callback too: SecretStoreCancellation::cancel() may run a copy after clearCancelCallback().
    class CancelEvent {
    public:
      CancelEvent() : m_fd(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {
        if (m_fd < 0) {
          throw sdbus::createError(errno, "eventfd");
        }
      }
      ~CancelEvent() { close(m_fd); }
      CancelEvent(const CancelEvent&) = delete;
      CancelEvent& operator=(const CancelEvent&) = delete;

      void signal() const {
        const std::uint64_t one = 1;
        [[maybe_unused]] const auto written = write(m_fd, &one, sizeof(one));
      }
      [[nodiscard]] int fd() const { return m_fd; }

    private:
      int m_fd;
    };

    // One private session-bus connection per operation, driven on the worker thread. Every call waits in poll() on the
    // bus and the cancel event, so a cancel ends a pending call or an open prompt at once.
    class SecretService {
    public:
      explicit SecretService(int cancelFd)
          : m_cancelFd(cancelFd), m_bus(sdbus::createSessionBusConnection()),
            m_service(proxy(sdbus::ObjectPath{"/org/freedesktop/secrets"})) {
        sdbus::Variant output;
        call(*m_service, kServiceInterface, "OpenSession", std::string{"plain"}, sdbus::Variant{std::string{}})
            >> output >> m_session;
      }

      ~SecretService() {
        try {
          proxy(m_session)->callMethod("Close").onInterface(kSessionInterface).dontExpectReply();
        } catch (const sdbus::Error&) {
        }
      }

      SecretService(const SecretService&) = delete;
      SecretService& operator=(const SecretService&) = delete;

      sdbus::ObjectPath defaultCollection() {
        sdbus::ObjectPath path;
        call(*m_service, kServiceInterface, "ReadAlias", std::string{"default"}) >> path;
        return path;
      }

      SecretStoreCollectionState inspectDefaultCollection() {
        try {
          const sdbus::ObjectPath path = defaultCollection();
          if (path == kNoObject) {
            return SecretStoreCollectionState::Missing;
          }
          sdbus::Variant locked;
          call(*proxy(path), "org.freedesktop.DBus.Properties", "Get", std::string{kCollectionInterface},
               std::string{"Locked"})
              >> locked;
          return locked.get<bool>() ? SecretStoreCollectionState::Locked : SecretStoreCollectionState::Unlocked;
        } catch (const sdbus::Error&) {
          return SecretStoreCollectionState::Unknown;
        }
      }

      SecretStoreBackendResult withDefaultCollectionState(SecretStoreBackendResult result) {
        result.defaultCollectionState = inspectDefaultCollection();
        return result;
      }

      std::pair<Paths, Paths> search(const std::map<std::string, std::string>& attributes) {
        Paths unlocked;
        Paths locked;
        call(*m_service, kServiceInterface, "SearchItems", attributes) >> unlocked >> locked;
        return {std::move(unlocked), std::move(locked)};
      }

      // The objects the service unlocked; nullopt when the user dismissed the prompt.
      std::optional<Paths> unlock(const Paths& objects) {
        Paths unlocked;
        sdbus::ObjectPath prompt;
        call(*m_service, kServiceInterface, "Unlock", objects) >> unlocked >> prompt;
        if (prompt != kNoObject) {
          auto result = runPrompt(prompt);
          if (!result.has_value()) {
            return std::nullopt;
          }
          unlocked = result->get<Paths>();
        }
        return unlocked;
      }

      Secret getSecret(const sdbus::ObjectPath& item) {
        Secret secret;
        call(*proxy(item), kItemInterface, "GetSecret", m_session) >> secret;
        return secret;
      }

      // The default collection path, created when missing; nullopt when the user dismissed the prompt.
      std::optional<sdbus::ObjectPath> createDefaultCollection() {
        const std::map<std::string, sdbus::Variant> properties{
            {"org.freedesktop.Secret.Collection.Label", sdbus::Variant{std::string{"Default keyring"}}},
        };
        sdbus::ObjectPath collection;
        sdbus::ObjectPath prompt;
        call(*m_service, kServiceInterface, "CreateCollection", properties, std::string{"default"})
            >> collection >> prompt;
        if (prompt == kNoObject) {
          return collection;
        }
        auto result = runPrompt(prompt);
        if (!result.has_value()) {
          return std::nullopt;
        }
        return result->get<sdbus::ObjectPath>();
      }

      // False when the user dismissed the prompt.
      bool createItem(
          const sdbus::ObjectPath& collection, const std::map<std::string, std::string>& attributes,
          std::span<const std::uint8_t> bytes, const std::string& label
      ) {
        const std::map<std::string, sdbus::Variant> properties{
            {"org.freedesktop.Secret.Item.Label", sdbus::Variant{label}},
            {"org.freedesktop.Secret.Item.Attributes", sdbus::Variant{attributes}},
        };
        Secret secret{
            m_session, std::vector<std::uint8_t>{}, std::vector<std::uint8_t>(bytes.begin(), bytes.end()),
            std::string{"application/octet-stream"}
        };
        sdbus::ObjectPath item;
        sdbus::ObjectPath prompt;
        try {
          call(*proxy(collection), kCollectionInterface, "CreateItem", properties, secret, true) >> item >> prompt;
        } catch (...) {
          wipe(secret);
          throw;
        }
        wipe(secret);
        return prompt == kNoObject || runPrompt(prompt).has_value();
      }

      // False when the user dismissed the prompt.
      bool deleteItem(const sdbus::ObjectPath& item) {
        sdbus::ObjectPath prompt;
        call(*proxy(item), kItemInterface, "Delete") >> prompt;
        return prompt == kNoObject || runPrompt(prompt).has_value();
      }

      static void wipe(Secret& secret) {
        auto& value = std::get<2>(secret);
        util::secureWipe(value.data(), value.size());
      }

    private:
      std::unique_ptr<sdbus::IProxy> proxy(const sdbus::ObjectPath& path) {
        return sdbus::createProxy(*m_bus, sdbus::ServiceName{kSecretsName}, path);
      }

      template <typename... Args>
      sdbus::MethodReply call(sdbus::IProxy& target, const char* interface, const char* method, const Args&... args) {
        auto message = target.createMethodCall(sdbus::InterfaceName{interface}, sdbus::MethodName{method});
        (message << ... << args);
        std::optional<sdbus::MethodReply> reply;
        std::optional<sdbus::Error> error;
        const auto slot = target.callMethodAsync(
            message,
            [&reply, &error](sdbus::MethodReply value, std::optional<sdbus::Error> failure) {
              if (failure.has_value()) {
                error = std::move(failure);
              } else {
                reply = std::move(value);
              }
            },
            kCallTimeout, sdbus::return_slot
        );
        waitUntil([&reply, &error] { return reply.has_value() || error.has_value(); });
        if (error.has_value()) {
          throw *error;
        }
        return std::move(*reply);
      }

      // The prompt's result, or nullopt when the user dismissed it. A cancel dismisses the prompt.
      std::optional<sdbus::Variant> runPrompt(const sdbus::ObjectPath& path) {
        auto prompt = proxy(path);
        std::optional<std::pair<bool, sdbus::Variant>> completed;
        prompt->uponSignal("Completed").onInterface(kPromptInterface).call([&completed](bool dismissed, sdbus::Variant result) {
          completed.emplace(dismissed, std::move(result));
        });
        try {
          call(*prompt, kPromptInterface, "Prompt", std::string{});
          waitUntil([&completed] { return completed.has_value(); });
        } catch (const OperationCancelled&) {
          try {
            prompt->callMethod("Dismiss").onInterface(kPromptInterface).dontExpectReply();
          } catch (const sdbus::Error&) {
          }
          throw;
        }
        if (completed->first) {
          return std::nullopt;
        }
        return std::move(completed->second);
      }

      template <typename Predicate> void waitUntil(const Predicate& done) {
        while (true) {
          while (m_bus->processPendingEvent()) {
          }
          if (done()) {
            return;
          }
          const auto data = m_bus->getEventLoopPollData();
          std::array<pollfd, 3> fds{{
              {.fd = data.fd, .events = data.events, .revents = 0},
              {.fd = data.eventFd, .events = POLLIN, .revents = 0},
              {.fd = m_cancelFd, .events = POLLIN, .revents = 0},
          }};
          if (poll(fds.data(), fds.size(), data.getPollTimeout()) < 0 && errno != EINTR) {
            throw sdbus::createError(errno, "poll");
          }
          if ((fds[2].revents & POLLIN) != 0) {
            throw OperationCancelled{};
          }
        }
      }

      int m_cancelFd;
      std::unique_ptr<sdbus::IConnection> m_bus;
      std::unique_ptr<sdbus::IProxy> m_service;
      sdbus::ObjectPath m_session;
    };

    template <typename Function>
    SecretStoreBackendResult withService(SecretStoreCancellation& cancellation, Function&& function) {
      if (cancellation.cancelled()) {
        return cancelledResult();
      }
      SecretStoreBackendResult result;
      try {
        const auto cancelEvent = std::make_shared<CancelEvent>();
        cancellation.setCancelCallback([cancelEvent] { cancelEvent->signal(); });
        try {
          SecretService service(cancelEvent->fd());
          result = function(service);
        } catch (const OperationCancelled&) {
          result = cancelledResult();
        }
      } catch (const sdbus::Error& error) {
        result = resultFromError(error);
      }
      cancellation.clearCancelCallback();
      return result;
    }

    class SecretServiceBackend final : public SecretStoreBackend {
    public:
      SecretStoreBackendResult probe(SecretStoreCancellation& cancellation) override {
        return withService(cancellation, [](SecretService&) {
          return makeResult(SecretStoreStatus::Success, SecretStoreErrorCategory::None);
        });
      }

      SecretStoreBackendResult
      lookup(const SecretStoreAttributes& attributes, SecretStoreCancellation& cancellation) override {
        return withService(cancellation, [&attributes](SecretService& service) {
          try {
            return lookupIn(service, makeAttributes(attributes));
          } catch (const sdbus::Error& error) {
            return service.withDefaultCollectionState(resultFromError(error));
          }
        });
      }

      SecretStoreBackendResult store(
          const SecretStoreAttributes& attributes, std::span<const std::uint8_t> bytes, const std::string& label,
          SecretStoreCancellation& cancellation
      ) override {
        // Empty secrets are refused: lookup treats an empty secret as an unopened KWallet wallet.
        if (bytes.empty()) {
          return invalidRequestResult();
        }
        return withService(cancellation, [&attributes, bytes, &label](SecretService& service) {
          try {
            std::optional<sdbus::ObjectPath> collection = service.defaultCollection();
            if (*collection == kNoObject) {
              collection = service.createDefaultCollection();
            } else if (service.inspectDefaultCollection() != SecretStoreCollectionState::Unlocked) {
              const auto unlocked = service.unlock({*collection});
              if (unlocked.has_value() && unlocked->empty()) {
                return service.withDefaultCollectionState(lockedResult());
              }
              if (!unlocked.has_value()) {
                collection.reset();
              }
            }
            // A dismissed prompt is the user's cancel.
            if (!collection.has_value() || !service.createItem(*collection, makeAttributes(attributes), bytes, label)) {
              return cancelledResult();
            }
            return makeResult(SecretStoreStatus::Success, SecretStoreErrorCategory::None);
          } catch (const sdbus::Error& error) {
            return service.withDefaultCollectionState(resultFromError(error));
          }
        });
      }

      SecretStoreBackendResult
      erase(const SecretStoreAttributes& attributes, SecretStoreCancellation& cancellation) override {
        return withService(cancellation, [&attributes](SecretService& service) {
          auto [items, locked] = service.search(makeAttributes(attributes));
          if (items.empty() && locked.empty()) {
            return makeResult(SecretStoreStatus::NotFound, SecretStoreErrorCategory::None);
          }
          bool remainsLocked = false;
          if (!locked.empty()) {
            const auto unlocked = service.unlock(locked);
            if (!unlocked.has_value()) {
              return cancelledResult();
            }
            items.insert(items.end(), unlocked->begin(), unlocked->end());
            remainsLocked = unlocked->size() < locked.size();
          }
          for (const auto& item : items) {
            if (!service.deleteItem(item)) {
              return cancelledResult();
            }
          }
          return remainsLocked ? lockedResult() : makeResult(SecretStoreStatus::Success, SecretStoreErrorCategory::None);
        });
      }

    private:
      static SecretStoreBackendResult
      lookupIn(SecretService& service, const std::map<std::string, std::string>& attributes) {
        // KWallet's ksecretd answers GetSecret on a wallet it has not opened yet with an empty secret
        // instead of an error, and counts each such request toward its "repeated failed attempts"
        // warning. Stored secrets are never empty, so an empty one means the wallet must be opened.
        bool emptySecretFound = false;
        const auto loadUnlocked = [&service, &emptySecretFound](
                                      const Paths& items, bool& lockedItemFound
                                  ) -> std::optional<SecretStoreBackendResult> {
          for (const auto& item : items) {
            Secret secret;
            try {
              secret = service.getSecret(item);
            } catch (const sdbus::Error& error) {
              SecretStoreBackendResult errorResult = resultFromError(error);
              if (errorResult.errorCategory == SecretStoreErrorCategory::Locked) {
                // A locked item does not rule out a readable match further down the list.
                lockedItemFound = true;
                continue;
              }
              return errorResult;
            }
            const auto& value = std::get<2>(secret);
            if (value.empty()) {
              emptySecretFound = true;
              lockedItemFound = true;
              continue;
            }
            SecretStoreBackendResult found = makeResult(SecretStoreStatus::Success, SecretStoreErrorCategory::None);
            found.value = SecureBuffer(std::span<const std::uint8_t>(value));
            SecretService::wipe(secret);
            return found;
          }
          return std::nullopt;
        };

        auto [unlocked, locked] = service.search(attributes);
        bool lockedItemFound = !locked.empty();
        if (auto result = loadUnlocked(unlocked, lockedItemFound); result.has_value()) {
          return std::move(*result);
        }
        if (!lockedItemFound || emptySecretFound) {
          // No matching item surfaced. A locked default collection cannot be enumerated, so an
          // empty result there is indistinguishable from "absent" until it is unlocked. Unlock it
          // and retry; a reachable (unlocked or missing) collection is reported absent without a
          // prompt. An empty secret means the provider has not opened the wallet even though it
          // may report it unlocked, so ask it to open the wallet regardless.
          if (!emptySecretFound) {
            const auto collectionState = service.inspectDefaultCollection();
            if (collectionState != SecretStoreCollectionState::Locked) {
              SecretStoreBackendResult notFound = makeResult(SecretStoreStatus::NotFound, SecretStoreErrorCategory::None);
              notFound.defaultCollectionState = collectionState;
              return notFound;
            }
          }
          const sdbus::ObjectPath collection = service.defaultCollection();
          const auto unlocked = collection == kNoObject ? std::nullopt : service.unlock({collection});
          if (!unlocked.has_value() || unlocked->empty()) {
            return service.withDefaultCollectionState(lockedResult());
          }
        }

        auto [items, stillLocked] = service.search(attributes);
        bool remainsLocked = false;
        if (!stillLocked.empty()) {
          const auto unlocked = service.unlock(stillLocked);
          if (unlocked.has_value()) {
            items.insert(items.end(), unlocked->begin(), unlocked->end());
          }
          remainsLocked = !unlocked.has_value() || unlocked->size() < stillLocked.size();
        }
        if (auto result = loadUnlocked(items, remainsLocked); result.has_value()) {
          return std::move(*result);
        }
        if (!remainsLocked) {
          // The collection is reachable and simply holds no matching secret.
          return service.withDefaultCollectionState(
              makeResult(SecretStoreStatus::NotFound, SecretStoreErrorCategory::None)
          );
        }
        return service.withDefaultCollectionState(lockedResult());
      }
    };

    std::unique_ptr<SecretStoreBackend> makeSecretServiceBackend() { return std::make_unique<SecretServiceBackend>(); }
  } // namespace

  SecretStoreAttributes secretStoreAttributes(const SecretId& id) {
    return {
        .application = "noctalia",
        .scope = id.scope,
        .owner = id.owner,
        .name = id.name,
        .version = "1",
    };
  }

  bool isValidSecretId(const SecretId& id) {
    if (id.owner.empty()) {
      return false;
    }
    if (id.scope == "calendar") {
      return id.name == "password" || id.name == "refresh-token";
    }
    if (id.scope == "storage") {
      return id.owner == "encrypted-state" && id.name == "master-key";
    }
    return false;
  }

  void SecretStoreCancellation::cancel() {
    std::function<void()> callback;
    {
      std::scoped_lock lock(m_mutex);
      if (m_cancelled) {
        return;
      }
      m_cancelled = true;
      callback = m_cancelCallback;
    }
    if (callback) {
      callback();
    }
  }

  bool SecretStoreCancellation::cancelled() const {
    std::scoped_lock lock(m_mutex);
    return m_cancelled;
  }

  void SecretStoreCancellation::setCancelCallback(const std::function<void()>& callback) {
    bool cancelNow = false;
    {
      std::scoped_lock lock(m_mutex);
      cancelNow = m_cancelled;
      if (!cancelNow) {
        m_cancelCallback = callback;
      }
    }
    if (cancelNow && callback) {
      callback();
    }
  }

  void SecretStoreCancellation::clearCancelCallback() {
    std::scoped_lock lock(m_mutex);
    m_cancelCallback = {};
  }

  SecretStoreOperation::SecretStoreOperation(std::weak_ptr<SecretStoreCancellation> cancellation)
      : m_cancellation(std::move(cancellation)) {}

  void SecretStoreOperation::cancel() const {
    if (auto cancellation = m_cancellation.lock()) {
      cancellation->cancel();
    }
  }

  bool SecretStoreOperation::cancelled() const {
    if (auto cancellation = m_cancellation.lock()) {
      return cancellation->cancelled();
    }
    return true;
  }

  class SecretStore::Impl {
  public:
    struct Request {
      OperationKind kind = OperationKind::Probe;
      SecretId id;
      SecretStoreAttributes attributes;
      SecureBuffer value;
      std::string label;
      LookupCallback lookupCallback;
      StatusCallback statusCallback;
      std::shared_ptr<SecretStoreCancellation> cancellation;
      bool valid = true;
    };

    struct Completion {
      OperationKind kind = OperationKind::Probe;
      SecretId id;
      SecretStoreBackendResult result;
      LookupCallback lookupCallback;
      StatusCallback statusCallback;
      std::shared_ptr<SecretStoreCancellation> cancellation;
    };

    explicit Impl(std::unique_ptr<SecretStoreBackend> backend) : m_backend(std::move(backend)) {
      if (m_backend == nullptr) {
        throw std::invalid_argument("SecretStore requires a backend");
      }
      m_eventFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
      if (m_eventFd < 0) {
        throw std::runtime_error("failed to create SecretStore eventfd");
      }
    }

    ~Impl() {
      std::shared_ptr<SecretStoreCancellation> active;
      {
        std::scoped_lock lock(m_queueMutex);
        m_shutdown = true;
        for (auto& request : m_requests) {
          request.cancellation->cancel();
        }
        m_requests.clear();
        active = m_activeCancellation;
      }
      if (active != nullptr) {
        active->cancel();
      }
      m_queueCv.notify_all();
      if (m_worker.joinable()) {
        m_worker.join();
      }
      {
        std::scoped_lock lock(m_completionMutex);
        m_completions.clear();
      }
      ::close(m_eventFd);
      m_eventFd = -1;
    }

    SecretStoreOperation enqueue(Request request) {
      auto cancellation = request.cancellation;
      {
        std::scoped_lock lock(m_queueMutex);
        if (m_shutdown) {
          cancellation->cancel();
          return SecretStoreOperation(cancellation);
        }
        m_requests.push_back(std::move(request));
        if (!m_workerRunning) {
          if (m_worker.joinable()) {
            m_worker.join();
          }
          m_worker = std::thread([this]() { workerLoop(); });
          m_workerRunning = true;
        }
      }
      m_queueCv.notify_one();
      return SecretStoreOperation(cancellation);
    }

    void addPollFd(std::vector<pollfd>& fds) const { fds.push_back({.fd = m_eventFd, .events = POLLIN, .revents = 0}); }

    void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
      if (startIdx >= fds.size() || (fds[startIdx].revents & (POLLIN | POLLERR | POLLHUP)) == 0) {
        return;
      }
      drainEventFd();

      std::deque<Completion> completions;
      {
        std::scoped_lock lock(m_completionMutex);
        completions = std::move(m_completions);
        m_completions.clear();
      }
      for (auto& completion : completions) {
        if (completion.cancellation->cancelled()) {
          continue;
        }
        updateAvailability(completion.result.status);
        logResult(completion);
        if (completion.kind == OperationKind::Lookup) {
          if (completion.lookupCallback) {
            completion.lookupCallback(completion.result.status, std::move(completion.result.value));
          }
        } else if (completion.statusCallback) {
          completion.statusCallback(completion.result.status);
        }
      }
    }

    [[nodiscard]] std::optional<SecretStoreStatus> availabilityStatus() const { return m_availabilityStatus; }

  private:
    void workerLoop() {
      andromeda::crash::installThreadStack();
      nameThisThread("dsk-secret");
      while (true) {
        Request request;
        {
          std::unique_lock lock(m_queueMutex);
          if (!m_queueCv.wait_for(lock, kWorkerIdleExit, [this]() { return m_shutdown || !m_requests.empty(); })) {
            m_workerRunning = false;
            return;
          }
          if (m_shutdown) {
            return;
          }
          request = std::move(m_requests.front());
          m_requests.pop_front();
          m_activeCancellation = request.cancellation;
        }

        SecretStoreBackendResult result;
        if (!request.valid) {
          result = invalidRequestResult();
        } else if (request.cancellation->cancelled()) {
          result = cancelledResult();
        } else {
          try {
            switch (request.kind) {
            case OperationKind::Probe:
              result = m_backend->probe(*request.cancellation);
              break;
            case OperationKind::Lookup:
              result = m_backend->lookup(request.attributes, *request.cancellation);
              break;
            case OperationKind::Store:
              result =
                  m_backend->store(request.attributes, request.value.bytes(), request.label, *request.cancellation);
              break;
            case OperationKind::Erase:
              result = m_backend->erase(request.attributes, *request.cancellation);
              break;
            }
          } catch (const std::exception& e) {
            kLog.warn("secret backend threw: {}", e.what());
            result = {
                .status = SecretStoreStatus::BackendError,
                .errorCategory = SecretStoreErrorCategory::Other,
            };
          } catch (...) {
            kLog.warn("secret backend threw a non-standard exception");
            result = {
                .status = SecretStoreStatus::BackendError,
                .errorCategory = SecretStoreErrorCategory::Other,
            };
          }
        }

        {
          std::scoped_lock lock(m_queueMutex);
          m_activeCancellation.reset();
          if (m_shutdown) {
            return;
          }
        }
        {
          std::scoped_lock lock(m_completionMutex);
          m_completions.push_back(
              Completion{
                  .kind = request.kind,
                  .id = std::move(request.id),
                  .result = std::move(result),
                  .lookupCallback = std::move(request.lookupCallback),
                  .statusCallback = std::move(request.statusCallback),
                  .cancellation = std::move(request.cancellation),
              }
          );
        }
        signalMain();
      }
    }

    void signalMain() const {
      constexpr std::uint64_t one = 1;
      const ssize_t written = ::write(m_eventFd, &one, sizeof(one));
      if (written < 0 && errno != EAGAIN) {
        kLog.warn("failed to signal SecretStore eventfd: errno={}", errno);
      }
    }

    void drainEventFd() const {
      std::uint64_t ignored = 0;
      while (::read(m_eventFd, &ignored, sizeof(ignored)) > 0) {
      }
    }

    void updateAvailability(SecretStoreStatus status) {
      m_availabilityStatus = status == SecretStoreStatus::NotFound ? SecretStoreStatus::Success : status;
    }

    void logResult(const Completion& completion) {
      const auto status = completion.result.status;
      if (status == SecretStoreStatus::Success) {
        return;
      }
      if (status == SecretStoreStatus::NotFound) {
        kLog.debug(
            "operation={} scope={} status={} default-collection={}", operationName(completion.kind),
            completion.id.scope, statusName(status), collectionStateName(completion.result.defaultCollectionState)
        );
        return;
      }
      if (status == SecretStoreStatus::Cancelled) {
        kLog.debug(
            "operation={} scope={} status={} category={} default-collection={}", operationName(completion.kind),
            completion.id.scope, statusName(status), categoryName(completion.result.errorCategory),
            collectionStateName(completion.result.defaultCollectionState)
        );
        return;
      }

      const auto now = std::chrono::steady_clock::now();
      const bool repeatedStatus = m_lastLoggedStatus.has_value() && *m_lastLoggedStatus == status;
      if (repeatedStatus && now - m_lastErrorLog < kRepeatedErrorLogInterval) {
        return;
      }
      m_lastLoggedStatus = status;
      m_lastErrorLog = now;
      kLog.warn(
          "operation={} scope={} status={} category={} default-collection={}", operationName(completion.kind),
          completion.id.scope, statusName(status), categoryName(completion.result.errorCategory),
          collectionStateName(completion.result.defaultCollectionState)
      );
    }

    std::unique_ptr<SecretStoreBackend> m_backend;
    int m_eventFd = -1;
    std::thread m_worker;
    bool m_workerRunning = false; // guarded by m_queueMutex

    mutable std::mutex m_queueMutex;
    std::condition_variable m_queueCv;
    std::deque<Request> m_requests;
    std::shared_ptr<SecretStoreCancellation> m_activeCancellation;
    bool m_shutdown = false;

    std::mutex m_completionMutex;
    std::deque<Completion> m_completions;

    std::optional<SecretStoreStatus> m_availabilityStatus;
    std::optional<SecretStoreStatus> m_lastLoggedStatus;
    std::chrono::steady_clock::time_point m_lastErrorLog;
  };

  SecretStore::SecretStore() : SecretStore(makeSecretServiceBackend()) {}

  SecretStore::SecretStore(std::unique_ptr<SecretStoreBackend> backend)
      : m_impl(std::make_unique<Impl>(std::move(backend))) {}

  SecretStore::~SecretStore() = default;

  SecretStoreOperation SecretStore::lookup(const SecretId& id, LookupCallback callback) {
    auto cancellation = std::make_shared<SecretStoreCancellation>();
    const bool valid = isValidSecretId(id);
    return m_impl->enqueue(
        Impl::Request{
            .kind = OperationKind::Lookup,
            .id = id,
            .attributes = secretStoreAttributes(id),
            .lookupCallback = std::move(callback),
            .cancellation = std::move(cancellation),
            .valid = valid,
        }
    );
  }

  SecretStoreOperation
  SecretStore::store(const SecretId& id, SecureBuffer value, std::string label, StatusCallback callback) {
    auto cancellation = std::make_shared<SecretStoreCancellation>();
    const bool valid = isValidSecretId(id);
    return m_impl->enqueue(
        Impl::Request{
            .kind = OperationKind::Store,
            .id = id,
            .attributes = secretStoreAttributes(id),
            .value = std::move(value),
            .label = std::move(label),
            .statusCallback = std::move(callback),
            .cancellation = std::move(cancellation),
            .valid = valid,
        }
    );
  }

  SecretStoreOperation SecretStore::erase(const SecretId& id, StatusCallback callback) {
    auto cancellation = std::make_shared<SecretStoreCancellation>();
    const bool valid = isValidSecretId(id);
    return m_impl->enqueue(
        Impl::Request{
            .kind = OperationKind::Erase,
            .id = id,
            .attributes = secretStoreAttributes(id),
            .statusCallback = std::move(callback),
            .cancellation = std::move(cancellation),
            .valid = valid,
        }
    );
  }

  SecretStoreOperation SecretStore::retryAvailabilityCheck(StatusCallback callback) {
    auto cancellation = std::make_shared<SecretStoreCancellation>();
    return m_impl->enqueue(
        Impl::Request{
            .kind = OperationKind::Probe,
            .statusCallback = std::move(callback),
            .cancellation = std::move(cancellation),
        }
    );
  }

  std::optional<SecretStoreStatus> SecretStore::availabilityStatus() const { return m_impl->availabilityStatus(); }

  void SecretStore::doAddPollFds(std::vector<pollfd>& fds) { m_impl->addPollFd(fds); }

  void SecretStore::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) { m_impl->dispatch(fds, startIdx); }

} // namespace security
