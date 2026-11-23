#include "dbus/link/link_commands.h"
#include "tests/test_check.h"

#include <array>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace {

  // What each phone's last command must be, from the events alone.
  struct Expected {
    enum class Status : std::uint8_t { None, Pending, Failed };
    Status status = Status::None;
    std::uint64_t serial = 0;
    std::string command;
    std::uint32_t value = 0;
    std::string error;
  };

  struct Sent {
    std::string device;
    std::uint64_t serial = 0;
  };

  void checkMatches(const LinkCommands& commands, const std::string& device, const Expected& expected) {
    const LinkCommandState* state = commands.state(device);
    if (expected.status == Expected::Status::None) {
      TEST_CHECK(state == nullptr);
      return;
    }
    TEST_CHECK(state != nullptr);
    TEST_CHECK(state->serial == expected.serial);
    TEST_CHECK(state->command == expected.command);
    TEST_CHECK(state->value == expected.value);
    TEST_CHECK(
        state->phase ==
        (expected.status == Expected::Status::Pending ? LinkCommandState::Phase::Pending : LinkCommandState::Phase::Failed)
    );
    TEST_CHECK(state->error == expected.error);
  }

  // Random sends, replies in any order (each reply once, some never), dismisses and clears across two phones: only
  // the reply to a phone's newest pending command changes it, a success hands back that command, and a dismiss drops
  // only a Failed one.
  void randomEvents(std::uint32_t seed) {
    std::mt19937 random(seed);
    const std::array<std::string, 2> devices{"a", "b"};
    const std::array<std::string, 4> kinds{"set-wifi", "set-bluetooth", "set-torch", "set-dnd"};
    LinkCommands commands;
    std::array<Expected, 2> expected{};
    std::vector<Sent> unanswered;
    for (int step = 0; step < 400; ++step) {
      const std::size_t d = random() % devices.size();
      switch (random() % 6) {
      case 0:
      case 1: {
        const std::string kind = kinds[random() % kinds.size()];
        const std::uint32_t value = random() % 2;
        const std::uint64_t serial = commands.send(devices[d], kind, value);
        expected[d] = {.status = Expected::Status::Pending, .serial = serial, .command = kind, .value = value};
        unanswered.push_back({devices[d], serial});
        break;
      }
      case 2:
      case 3: {
        if (unanswered.empty()) {
          break;
        }
        const std::size_t pick = random() % unanswered.size();
        const Sent reply = unanswered[pick];
        unanswered.erase(unanswered.begin() + static_cast<std::ptrdiff_t>(pick));
        const std::size_t r = reply.device == devices[0] ? 0 : 1;
        const bool fails = random() % 2 == 0;
        const std::optional<std::string> error =
            fails ? std::optional<std::string>("refused " + std::to_string(reply.serial)) : std::nullopt;
        LinkCommandState done;
        const LinkCommands::Answer answer = commands.answered(reply.device, reply.serial, error, &done);
        const bool current = expected[r].status == Expected::Status::Pending && expected[r].serial == reply.serial;
        if (!current) {
          TEST_CHECK(answer == LinkCommands::Answer::Stale);
        } else if (fails) {
          TEST_CHECK(answer == LinkCommands::Answer::Failed);
          expected[r].status = Expected::Status::Failed;
          expected[r].error = *error;
        } else {
          TEST_CHECK(answer == LinkCommands::Answer::Succeeded);
          TEST_CHECK(done.serial == reply.serial);
          TEST_CHECK(done.command == expected[r].command);
          TEST_CHECK(done.value == expected[r].value);
          expected[r] = {};
        }
        break;
      }
      case 4: {
        const bool failed = expected[d].status == Expected::Status::Failed;
        TEST_CHECK(commands.dismiss(devices[d]) == failed);
        if (failed) {
          expected[d] = {};
        }
        break;
      }
      default:
        if (random() % 8 == 0) {
          commands.clear();
          expected = {};
        }
        break;
      }
      for (std::size_t i = 0; i < devices.size(); ++i) {
        checkMatches(commands, devices[i], expected[i]);
      }
    }
  }

} // namespace

int main() {
  for (std::uint32_t seed = 1; seed <= 200; ++seed) {
    randomEvents(seed);
  }
  return 0;
}
