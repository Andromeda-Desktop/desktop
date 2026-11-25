#pragma once

#include <chrono>
#include <string>
#include <string_view>

class PamAuthenticator {
public:
  struct Result {
    bool success = false;
    // PAM refused the credential itself (wrong password), not a PAM or helper error.
    bool rejected = false;
    // PAM's own text when it sent one (pam_faillock's lockout notice); else, for a failure that is not a rejection,
    // its error string. Empty for a plain wrong password.
    std::string message;
  };

  // A PAM stack that has not answered after `timeout` (a hung module) is killed and counts as a failure.
  [[nodiscard]] Result authenticateCurrentUser(
      std::string_view password, std::string_view service, std::string_view language,
      std::string_view startFailureMessage, std::chrono::milliseconds timeout = std::chrono::seconds(60)
  ) const;
  [[nodiscard]] static std::string currentUsername();
  [[nodiscard]] static int runHelperMode(int argc, char* argv[]);
};
