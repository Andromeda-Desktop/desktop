// The authenticator re-execs /proc/self/exe as its PAM helper: here that is this test, which plays a helper that
// never answers, as a PAM module that hangs does.
#include "auth/pam_authenticator.h"
#include "tests/test_check.h"

#include <cerrno>
#include <chrono>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char* argv[]) {
  if (argc > 1 && std::string_view(argv[1]) == "pam-helper") {
    ::sleep(30);
    return 0;
  }

  const auto start = std::chrono::steady_clock::now();
  const PamAuthenticator::Result result =
      PamAuthenticator{}.authenticateCurrentUser("secret", "login", "C", "helper failed", std::chrono::milliseconds(300));
  const auto took = std::chrono::steady_clock::now() - start;

  TEST_CHECK(took < std::chrono::seconds(5));
  TEST_CHECK(!result.success);
  TEST_CHECK(!result.rejected);
  TEST_CHECK(result.message == "helper failed");
  // The hung helper was stopped and reaped: no child is left.
  TEST_CHECK(::waitpid(-1, nullptr, WNOHANG) == -1 && errno == ECHILD);
  return 0;
}
