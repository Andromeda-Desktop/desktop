#pragma once

#include "dbus/name_owner_watch.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <sys/types.h>

class SystemBus;

namespace sdbus {
  class IProxy;
}

class AccountsService {
public:
  using ChangeCallback = std::function<void()>;

  explicit AccountsService(SystemBus& bus);

  void setChangeCallback(ChangeCallback callback);

  [[nodiscard]] uid_t sessionUid() const noexcept { return m_sessionUid; }
  [[nodiscard]] const std::string& iconFile() const noexcept { return m_iconFile; }
  // The session user's full name; empty until AccountsService answers or when the user has none.
  [[nodiscard]] const std::string& realName() const noexcept { return m_realName; }

  [[nodiscard]] bool setIconFile(std::string_view filename);

  // Creates a login user, then sets its password. Async: the polkit prompt comes from this process.
  // `done` gets an empty string on success, else the error text.
  // Deletes a user and its home folder. Async for the same polkit reason as createUser.
  void deleteUser(uid_t uid, std::function<void(std::string)> done);
  void createUser(
      const std::string& login, const std::string& realName, bool admin, const std::string& password,
      std::function<void(std::string)> done
  );

private:
  void findSessionUser();
  void bindUser(const std::string& userPath);
  void emitChangedIfNeeded(std::string_view nextIconFile, std::string_view nextRealName);

  SystemBus& m_bus;
  std::unique_ptr<sdbus::IProxy> m_accountsProxy;
  std::unique_ptr<sdbus::IProxy> m_pendingUserProxy;
  std::unique_ptr<sdbus::IProxy> m_userProxy;
  uid_t m_sessionUid = 0;
  std::string m_iconFile;
  std::string m_realName;
  ChangeCallback m_changeCallback;
  std::shared_ptr<int> m_aliveToken = std::make_shared<int>(0);
  NameOwnerWatch m_ownerWatch;
};
