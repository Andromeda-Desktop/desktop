#include "dbus/accounts/accounts_service.h"

#include "core/lazy_library.h"
#include "core/log.h"
#include "dbus/system_bus.h"
#include "dbus/system_interactive_call.h"
#include "util/secure_wipe.h"

#include <crypt.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <pwd.h>
#include <sdbus-c++/Error.h>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <string>
#include <systemd/sd-bus.h>
#include <unistd.h>
#include <utility>

// clang-format off
#define LIBCRYPT_FUNCTIONS(X) \
  X(crypt_gensalt_ra) X(crypt_r)
// clang-format on
LAZY_LIBRARY(LibcryptApi, libcrypt, LAZY_SONAME_LIBCRYPT, LIBCRYPT_FUNCTIONS)

namespace {

  using dbus::callInteractive;

  constexpr Logger kLog("accounts");

  const sdbus::ServiceName kAccountsBusName{"org.freedesktop.Accounts"};
  const sdbus::ObjectPath kAccountsObjectPath{"/org/freedesktop/Accounts"};
  constexpr auto kAccountsInterface = "org.freedesktop.Accounts";
  constexpr auto kUserInterface = "org.freedesktop.Accounts.User";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";

} // namespace

AccountsService::AccountsService(SystemBus& bus) : m_bus(bus) {
  m_sessionUid = ::getuid();
  if (m_sessionUid == static_cast<uid_t>(-1)) {
    throw sdbus::Error(
        sdbus::Error::Name{"org.freedesktop.DBus.Error.Failed"}, "failed to resolve session uid via getuid()"
    );
  }

  m_accountsProxy = sdbus::createProxy(m_bus.connection(), kAccountsBusName, kAccountsObjectPath);
  // A failed lookup is retried when accounts-daemon starts again.
  m_ownerWatch = NameOwnerWatch::changes(
      m_bus.connection(), std::string(kAccountsBusName),
      [this](const std::string& /*name*/, const std::string& /*oldOwner*/, const std::string& newOwner) {
        if (!newOwner.empty()) {
          findSessionUser();
        }
      });
  findSessionUser();
}

void AccountsService::findSessionUser() {
  m_userProxy.reset();
  m_pendingUserProxy.reset();
  const std::weak_ptr<int> alive = m_aliveToken;
  m_accountsProxy->callMethodAsync("FindUserById")
      .onInterface(kAccountsInterface)
      .withArguments(static_cast<std::int64_t>(m_sessionUid))
      .uponReplyInvoke([this, alive](std::optional<sdbus::Error> error, sdbus::ObjectPath userPath) {
        if (alive.expired()) {
          return;
        }
        if (error.has_value()) {
          kLog.info("accounts service disabled: {}", error->getMessage());
          return;
        }
        bindUser(userPath);
      });
}

void AccountsService::bindUser(const std::string& userPath) {
  m_pendingUserProxy = sdbus::createProxy(m_bus.connection(), kAccountsBusName, sdbus::ObjectPath{userPath});
  const std::weak_ptr<int> alive = m_aliveToken;
  const sdbus::IProxy* pending = m_pendingUserProxy.get();
  m_pendingUserProxy->callMethodAsync("GetAll")
      .onInterface(kPropertiesInterface)
      .withArguments(std::string(kUserInterface))
      .uponReplyInvoke([this, alive, pending](
                           std::optional<sdbus::Error> error, std::map<std::string, sdbus::Variant> properties
                       ) {
        // A newer lookup replaced this proxy.
        if (alive.expired() || pending != m_pendingUserProxy.get()) {
          return;
        }
        std::optional<std::uint64_t> uid;
        std::string iconFile;
        try {
          if (!error.has_value()) {
            if (const auto it = properties.find("Uid"); it != properties.end()) {
              uid = it->second.get<std::uint64_t>();
            }
            if (const auto it = properties.find("IconFile"); it != properties.end()) {
              iconFile = it->second.get<std::string>();
            }
          }
        } catch (const sdbus::Error& e) {
          error = e;
        }
        if (error.has_value() || uid != m_sessionUid) {
          kLog.warn(
              "accounts service disabled: {}",
              error.has_value() ? error->getMessage() : "user Uid does not match session uid"
          );
          return;
        }
        m_userProxy = std::move(m_pendingUserProxy);
        m_userProxy->uponSignal("PropertiesChanged")
            .onInterface(kPropertiesInterface)
            .call([this](
                      const std::string& interfaceName, const std::map<std::string, sdbus::Variant>& changedProperties,
                      const std::vector<std::string>& /*invalidatedProperties*/
                  ) {
              if (interfaceName != kUserInterface) {
                return;
              }
              const auto it = changedProperties.find("IconFile");
              if (it == changedProperties.end()) {
                return;
              }
              try {
                emitChangedIfNeeded(it->second.get<std::string>());
              } catch (const sdbus::Error& e) {
                kLog.warn("failed to decode IconFile property change: {}", e.what());
              }
            });
        kLog.info("session user avatar path: {}", iconFile.empty() ? "<unset>" : iconFile);
        emitChangedIfNeeded(iconFile);
      });
}

void AccountsService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

bool AccountsService::setIconFile(std::string_view filename) {
  if (filename.empty()) {
    kLog.warn("SetIconFile rejected: filename is empty");
    return false;
  }
  if (m_userProxy == nullptr) {
    kLog.warn("SetIconFile rejected: user proxy is unavailable");
    return false;
  }

  // Async: accounts-daemon copies and checks the image, which can take seconds on a slow home.
  const std::weak_ptr<int> alive = m_aliveToken;
  sdbus::IProxy* proxy = m_userProxy.get();
  const auto current = [this, alive, proxy] { return !alive.expired() && proxy == m_userProxy.get(); };
  try {
    proxy->callMethodAsync("SetIconFile")
        .onInterface(kUserInterface)
        .withArguments(std::string(filename))
        .uponReplyInvoke([this, proxy, current](std::optional<sdbus::Error> error) {
          if (!current()) {
            return;
          }
          if (error.has_value()) {
            kLog.warn("SetIconFile failed: {}", error->getMessage());
            return;
          }
          proxy->getPropertyAsync("IconFile")
              .onInterface(kUserInterface)
              .uponReplyInvoke([this, current](std::optional<sdbus::Error> readError, sdbus::Variant value) {
                if (!current()) {
                  return;
                }
                std::string iconFile;
                try {
                  if (!readError.has_value()) {
                    iconFile = value.get<std::string>();
                  }
                } catch (const sdbus::Error&) {
                }
                // The same path with new content still changes the avatar.
                if (iconFile == m_iconFile && m_changeCallback) {
                  m_changeCallback();
                } else {
                  emitChangedIfNeeded(iconFile);
                }
              });
        });
    return true;
  } catch (const sdbus::Error& e) {
    kLog.warn("SetIconFile failed: {}", e.what());
    return false;
  }
}

void AccountsService::emitChangedIfNeeded(std::string_view nextIconFile) {
  const std::string next(nextIconFile);
  if (next == m_iconFile) {
    return;
  }
  m_iconFile = next;
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void AccountsService::createUser(
    const std::string& login, const std::string& realName, bool admin, const std::string& password,
    std::function<void(std::string)> done
) {
  const char* salt = libcrypt().crypt_gensalt_ra(nullptr, 0, nullptr, 0);
  crypt_data data{};
  const char* hashed = salt != nullptr ? libcrypt().crypt_r(password.c_str(), salt, &data) : nullptr;
  std::free(const_cast<char*>(salt));
  const std::string hash = hashed != nullptr && hashed[0] != '*' ? std::string(hashed) : std::string{};
  util::secureWipe(&data, sizeof(data));
  if (hash.empty()) {
    done("could not hash the password");
    return;
  }
  SystemBus& bus = m_bus;
  callInteractive(
      bus,
      "org.freedesktop.Accounts", kAccountsObjectPath.c_str(), kAccountsInterface, "CreateUser",
      [&](sd_bus_message* m) {
        return sd_bus_message_append(m, "ssi", login.c_str(), realName.c_str(), static_cast<std::int32_t>(admin ? 1 : 0));
      },
      [&bus, hash, login, done = std::move(done)](sd_bus_message* created, std::string error) {
        if (created == nullptr) {
          kLog.warn("CreateUser failed: {}", error);
          done(std::move(error));
          return;
        }
        const char* userPath = nullptr;
        if (sd_bus_message_read(created, "o", &userPath) < 0 || userPath == nullptr) {
          done("CreateUser returned no user path");
          return;
        }
        const std::string userObject = userPath;
        callInteractive(
            bus,
            "org.freedesktop.Accounts", userObject.c_str(), kUserInterface, "SetPassword",
            [&hash](sd_bus_message* m) { return sd_bus_message_append(m, "ss", hash.c_str(), ""); },
            [&bus, login, done](sd_bus_message* set, std::string passwordError) {
              if (set != nullptr) {
                done({});
                return;
              }
              kLog.warn("SetPassword failed: {}", passwordError);
              // An account without a password cannot log in and blocks a retry with the same name: remove it.
              const passwd* pw = getpwnam(login.c_str());
              if (pw == nullptr) {
                done(std::move(passwordError));
                return;
              }
              callInteractive(
                  bus,
                  "org.freedesktop.Accounts", kAccountsObjectPath.c_str(), kAccountsInterface, "DeleteUser",
                  [pw](sd_bus_message* m) {
                    return sd_bus_message_append(m, "xb", static_cast<std::int64_t>(pw->pw_uid), 1);
                  },
                  [done, passwordError](sd_bus_message* deleted, std::string deleteError) {
                    if (deleted == nullptr) {
                      kLog.warn("DeleteUser after a failed SetPassword failed: {}", deleteError);
                    }
                    done(passwordError);
                  }
              );
            }
        );
      }
  );
}

void AccountsService::deleteUser(uid_t uid, std::function<void(std::string)> done) {
  callInteractive(
      m_bus,
      "org.freedesktop.Accounts", kAccountsObjectPath.c_str(), kAccountsInterface, "DeleteUser",
      [uid](sd_bus_message* m) { return sd_bus_message_append(m, "xb", static_cast<std::int64_t>(uid), 1); },
      [done = std::move(done)](sd_bus_message* deleted, std::string error) {
        if (deleted == nullptr) {
          kLog.warn("DeleteUser failed: {}", error);
        }
        done(deleted == nullptr ? std::move(error) : std::string{});
      }
  );
}
