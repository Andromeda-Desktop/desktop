#include "admin_session.h"

#include "i18n/i18n.h"

#include <map>
#include <print>
#include <random>
#include <string>
#include <vector>

// Model-based: random event sequences (requests, the ready line, replies in and out of order, junk, exits, close)
// drive one session; after every event the invariants below must hold.
namespace {
  int g_failures = 0;

  void expect(bool ok, const std::string& what, unsigned seed, int step) {
    if (!ok) {
      std::println(stderr, "admin_session_test: seed {} step {}: {}", seed, step, what);
      ++g_failures;
    }
  }

  using Phase = AdminSession::Phase;
} // namespace

int main() {
  i18n::Service::instance().addDomain("files", ANDROMEDA_TRANSLATIONS_DIR);
  i18n::Service::instance().setLanguage("en");
  for (unsigned seed = 1; seed <= 2000; ++seed) {
    std::mt19937 rng(seed);
    std::vector<nlohmann::json> written;
    std::map<int, int> answered; // request number -> reply count
    std::map<int, bool> answeredOk;
    int requests = 0;
    AdminSession session([&](const std::string& line) { written.push_back(nlohmann::json::parse(line)); }, {});
    std::size_t repliedLines = 0; // written lines the fake helper answered
    bool sawClosed = false;
    for (int step = 0; step < 40; ++step) {
      const Phase before = session.phase();
      switch (std::uniform_int_distribution<int>(0, 9)(rng)) {
      case 0:
      case 1:
      case 2: {
        const int n = requests++;
        session.request({{"op", "list"}, {"path", "/etc"}}, [&, n](const nlohmann::json& r) {
          ++answered[n];
          answeredOk[n] = r.value("ok", false);
        });
        break;
      }
      case 3:
        session.onLine(R"({"ready":true})");
        break;
      case 4:
      case 5:
        if (repliedLines < written.size()) {
          const auto id = written[repliedLines++]["id"].get<std::uint64_t>();
          session.onLine(nlohmann::json{{"id", id}, {"ok", true}, {"count", 1}}.dump());
        }
        break;
      case 6:
        session.onLine(R"({"id":999999,"ok":true})");
        break;
      case 7:
        session.onLine("not json");
        break;
      case 8:
        session.onExit(std::uniform_int_distribution<int>(0, 1)(rng) == 0 ? 126 : 1);
        break;
      default:
        session.close();
        break;
      }
      const Phase now = session.phase();
      if (now == Phase::Failed || now == Phase::Closed) {
        repliedLines = written.size(); // the helper is gone: a request that was out is abandoned
      }
      sawClosed = sawClosed || now == Phase::Closed;
      expect(!sawClosed || now == Phase::Closed, "Closed is final", seed, step);
      expect(before != Phase::Failed || now == Phase::Failed || now == Phase::Closed, "Failed only goes to Closed",
             seed, step);
      expect((now == Phase::Failed) == !session.failure().empty(), "a failure reason exactly while Failed", seed, step);
      expect(written.size() - repliedLines <= 1, "at most one request out at a time", seed, step);
      expect((now == Phase::Busy) == (written.size() > repliedLines), "Busy exactly while a request is out", seed,
             step);
      for (const auto& [n, count] : answered) {
        expect(count == 1, std::format("request {} answered {} times", n, count), seed, step);
      }
      if (now == Phase::Failed || now == Phase::Closed) {
        expect(static_cast<int>(answered.size()) == requests, "every request is answered once the session ends", seed,
               step);
      }
    }
    // Ids go out in order, one per written line.
    for (std::size_t i = 1; i < written.size(); ++i) {
      expect(written[i]["id"].get<std::uint64_t>() > written[i - 1]["id"].get<std::uint64_t>(), "ids increase", seed,
             0);
    }
  }

  // A dismissed password dialog names itself; a request after that fails at once.
  AdminSession s([](const std::string&) {}, {});
  s.onExit(126);
  bool failedAtOnce = false;
  s.request({{"op", "list"}, {"path", "/"}}, [&](const nlohmann::json& r) { failedAtOnce = !r.value("ok", true); });
  expect(s.phase() == Phase::Failed && s.failure() == "No administrator password was given." && failedAtOnce,
         "126 = dismissed", 0, 0);
  if (g_failures == 0) {
    std::println("admin_session_test: ok");
  }
  return g_failures == 0 ? 0 : 1;
}
