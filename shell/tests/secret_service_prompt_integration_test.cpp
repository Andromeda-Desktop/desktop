// A locked item makes the secret store ask the Secret Service to unlock it and run the returned prompt. A stand-in
// service on a private bus (meson runs it under dbus-run-session) answers the prompt, dismisses it, or holds it open
// until the store cancels the lookup.
#include "security/secret_store.h"
#include "tests/test_check.h"

#include "tests/test_deferred.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <poll.h>
#include <sdbus-c++/sdbus-c++.h>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace {
  using Secret = sdbus::Struct<sdbus::ObjectPath, std::vector<std::uint8_t>, std::vector<std::uint8_t>, std::string>;
  using Paths = std::vector<sdbus::ObjectPath>;

  constexpr auto kServiceInterface = "org.freedesktop.Secret.Service";
  constexpr auto kPromptInterface = "org.freedesktop.Secret.Prompt";
  const sdbus::ObjectPath kCollection{"/org/freedesktop/secrets/collection/default"};
  const sdbus::ObjectPath kItem{"/org/freedesktop/secrets/collection/default/1"};
  const sdbus::ObjectPath kPrompt{"/org/freedesktop/secrets/prompt/1"};
  const sdbus::ObjectPath kSession{"/org/freedesktop/secrets/session/1"};
  const std::string kValue = "pw";

  enum class PromptReply { Accept, Dismiss, Hold };

  class FakeSecretService {
  public:
    FakeSecretService()
        : m_connection(sdbus::createSessionBusConnection(sdbus::ServiceName{"org.freedesktop.secrets"})),
          m_service(sdbus::createObject(*m_connection, sdbus::ObjectPath{"/org/freedesktop/secrets"})),
          m_collection(sdbus::createObject(*m_connection, kCollection)),
          m_item(sdbus::createObject(*m_connection, kItem)), m_prompt(sdbus::createObject(*m_connection, kPrompt)),
          m_session(sdbus::createObject(*m_connection, kSession)) {
      m_service
          ->addVTable(
              // No encrypted transport: the store must fall back to plain.
              sdbus::registerMethod("OpenSession")
                  .implementedAs([](const std::string& algorithm, const sdbus::Variant&) {
                    if (algorithm != "plain") {
                      throw sdbus::Error(sdbus::Error::Name{"org.freedesktop.DBus.Error.NotSupported"}, algorithm);
                    }
                    return std::tuple{sdbus::Variant{std::string{}}, kSession};
                  }),
              sdbus::registerMethod("ReadAlias").implementedAs([](const std::string&) { return kCollection; }),
              sdbus::registerMethod("SearchItems").implementedAs([this](const std::map<std::string, std::string>&) {
                std::scoped_lock lock(m_mutex);
                return m_locked ? std::tuple{Paths{}, Paths{kItem}} : std::tuple{Paths{kItem}, Paths{}};
              }),
              sdbus::registerMethod("Unlock").implementedAs([this](const Paths& objects) {
                std::scoped_lock lock(m_mutex);
                return m_locked ? std::tuple{Paths{}, kPrompt} : std::tuple{objects, sdbus::ObjectPath{"/"}};
              })
          )
          .forInterface(kServiceInterface);
      m_collection
          ->addVTable(
              sdbus::registerProperty("Locked").withGetter([this] {
                std::scoped_lock lock(m_mutex);
                return m_locked;
              }),
              sdbus::registerMethod("CreateItem")
                  .implementedAs([this](const std::map<std::string, sdbus::Variant>& properties, const Secret&, bool) {
                    std::scoped_lock lock(m_mutex);
                    if (m_locked) {
                      throw sdbus::Error(sdbus::Error::Name{"org.freedesktop.Secret.Error.IsLocked"}, "locked");
                    }
                    m_storedAttributes = properties.at("org.freedesktop.Secret.Item.Attributes")
                                             .get<std::map<std::string, std::string>>();
                    return std::tuple{kItem, sdbus::ObjectPath{"/"}};
                  })
          )
          .forInterface("org.freedesktop.Secret.Collection");
      m_item
          ->addVTable(sdbus::registerMethod("GetSecret").implementedAs([this](const sdbus::ObjectPath& session) {
            std::scoped_lock lock(m_mutex);
            if (m_locked) {
              throw sdbus::Error(sdbus::Error::Name{"org.freedesktop.Secret.Error.IsLocked"}, "locked");
            }
            return Secret{
                session, std::vector<std::uint8_t>{}, std::vector<std::uint8_t>(kValue.begin(), kValue.end()),
                std::string{"text/plain"}
            };
          }))
          .forInterface("org.freedesktop.Secret.Item");
      m_prompt
          ->addVTable(
              sdbus::registerMethod("Prompt").implementedAs([this](const std::string&) { onPrompt(); }),
              sdbus::registerMethod("Dismiss").implementedAs([this] {
                {
                  std::scoped_lock lock(m_mutex);
                  ++m_dismissed;
                }
                m_changed.notify_all();
                complete(true, Paths{});
              }),
              sdbus::registerSignal("Completed").withParameters<bool, sdbus::Variant>()
          )
          .forInterface(kPromptInterface);
      m_session->addVTable(
                   sdbus::registerMethod("Close").implementedAs([] {})
      ).forInterface("org.freedesktop.Secret.Session");
      m_connection->enterEventLoopAsync();
    }

    void reset(PromptReply reply) {
      std::scoped_lock lock(m_mutex);
      m_reply = reply;
      m_locked = true;
      m_prompted = 0;
      m_dismissed = 0;
      m_storedAttributes.clear();
    }

    std::string storedSchema() {
      std::scoped_lock lock(m_mutex);
      const auto found = m_storedAttributes.find("xdg:schema");
      return found == m_storedAttributes.end() ? std::string{} : found->second;
    }

    int prompted() {
      std::scoped_lock lock(m_mutex);
      return m_prompted;
    }

    // True when the counter reaches the value within the timeout.
    bool waitFor(int FakeSecretService::* counter, int value) {
      std::unique_lock lock(m_mutex);
      return m_changed.wait_for(lock, std::chrono::seconds(10), [&] { return this->*counter >= value; });
    }

    int m_prompted = 0;
    int m_dismissed = 0;

  private:
    void onPrompt() {
      PromptReply reply;
      {
        std::scoped_lock lock(m_mutex);
        ++m_prompted;
        reply = m_reply;
        if (reply == PromptReply::Accept) {
          m_locked = false;
        }
      }
      m_changed.notify_all();
      if (reply == PromptReply::Accept) {
        complete(false, Paths{kItem});
      } else if (reply == PromptReply::Dismiss) {
        complete(true, Paths{});
      }
    }

    void complete(bool dismissed, const Paths& unlocked) {
      m_prompt->emitSignal("Completed")
          .onInterface(kPromptInterface)
          .withArguments(dismissed, sdbus::Variant{unlocked});
    }

    std::mutex m_mutex;
    std::condition_variable m_changed;
    PromptReply m_reply = PromptReply::Accept;
    bool m_locked = true;
    std::map<std::string, std::string> m_storedAttributes;
    std::unique_ptr<sdbus::IConnection> m_connection;
    std::unique_ptr<sdbus::IObject> m_service;
    std::unique_ptr<sdbus::IObject> m_collection;
    std::unique_ptr<sdbus::IObject> m_item;
    std::unique_ptr<sdbus::IObject> m_prompt;
    std::unique_ptr<sdbus::IObject> m_session;
  };

  // Dispatches until the callback sets done: a cancelled operation also wakes the store.
  bool dispatchUntil(security::SecretStore& /*store*/, const bool& done) {
    return runDeferredUntil([&] { return done; }, std::chrono::seconds(10));
  }

  const security::SecretId kId{.scope = "storage", .owner = "encrypted-state", .name = "master-key"};

  struct Lookup {
    bool done = false;
    security::SecretStoreStatus status = security::SecretStoreStatus::BackendError;
    std::string value;
  };

  security::SecretStoreStatus store(security::SecretStore& store) {
    auto status = security::SecretStoreStatus::BackendError;
    bool done = false;
    (void)store.store(
        kId, security::SecureBuffer(std::span(reinterpret_cast<const std::uint8_t*>(kValue.data()), kValue.size())),
        "prompt test", [&](security::SecretStoreStatus result) {
          status = result;
          done = true;
        }
    );
    TEST_CHECK(dispatchUntil(store, done));
    return status;
  }

  security::SecretStoreOperation startLookup(security::SecretStore& store, Lookup& lookup) {
    return store.lookup(kId, [&lookup](security::SecretStoreStatus status, security::SecureBuffer buffer) {
      const auto bytes = buffer.bytes();
      lookup = {.done = true, .status = status, .value = std::string(bytes.begin(), bytes.end())};
    });
  }
} // namespace

int main() {
  FakeSecretService service;

  {
    service.reset(PromptReply::Accept);
    security::SecretStore store;
    Lookup lookup;
    (void)startLookup(store, lookup);
    TEST_CHECK(dispatchUntil(store, lookup.done));
    TEST_CHECK(lookup.status == security::SecretStoreStatus::Success);
    TEST_CHECK(lookup.value == kValue);
    TEST_CHECK(service.prompted() == 1);
  }

  {
    service.reset(PromptReply::Dismiss);
    security::SecretStore store;
    Lookup lookup;
    (void)startLookup(store, lookup);
    TEST_CHECK(dispatchUntil(store, lookup.done));
    TEST_CHECK(lookup.status == security::SecretStoreStatus::DeniedOrLocked);
    TEST_CHECK(service.prompted() == 1);
  }

  {
    service.reset(PromptReply::Hold);
    security::SecretStore store;
    Lookup lookup;
    const auto operation = startLookup(store, lookup);
    TEST_CHECK(service.waitFor(&FakeSecretService::m_prompted, 1));
    operation.cancel();
    TEST_CHECK(service.waitFor(&FakeSecretService::m_dismissed, 1));

    // The worker is free again after the cancel.
    service.reset(PromptReply::Accept);
    Lookup next;
    (void)startLookup(store, next);
    TEST_CHECK(dispatchUntil(store, next.done));
    TEST_CHECK(next.status == security::SecretStoreStatus::Success);
  }

  {
    service.reset(PromptReply::Accept);
    security::SecretStore secrets;
    TEST_CHECK(store(secrets) == security::SecretStoreStatus::Success);
    TEST_CHECK(service.prompted() == 1);
    TEST_CHECK(service.storedSchema() == security::SecretStoreSchemaName);
  }

  {
    service.reset(PromptReply::Dismiss);
    security::SecretStore secrets;
    TEST_CHECK(store(secrets) == security::SecretStoreStatus::Cancelled);
    TEST_CHECK(service.storedSchema().empty());
  }
  return 0;
}
