#include "command_index.h"

#include <cstdio>
#include <random>
#include <string>

using namespace term;
using Phase = CommandEntry::Phase;

namespace {

int failures = 0;
unsigned seed = 0;
int step = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s (seed %u step %d)\n", __FILE__, __LINE__, #c, seed, step); ++failures; } } while (0)

void write(GhosttyTerminal t, const std::string& s) {
  ghostty_terminal_vt_write(t, reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

void checkInvariants(const CommandIndex& index) {
  const auto& e = index.entries();
  std::optional<std::uint32_t> last;
  for (std::size_t i = 0; i < e.size(); ++i) {
    if (i + 1 < e.size()) CHECK(e[i].phase == Phase::Done);
    const auto p = CommandIndex::row(e[i].prompt.get(), GHOSTTY_POINT_TAG_SCREEN);
    CHECK(p.has_value());
    if (p && last) CHECK(*p > *last);
    last = p;
    CHECK((e[i].phase == Phase::Prompt) == (e[i].output == nullptr));
    if (const auto o = CommandIndex::row(e[i].output.get(), GHOSTTY_POINT_TAG_SCREEN); o && p) CHECK(*o >= *p);
  }
}

void run(std::size_t scrollbackBytes) {
  std::mt19937 rng(seed);
  GhosttyTerminal t = nullptr;
  ghostty_terminal_new(nullptr, &t, 40, 6);
  ghostty_terminal_set(t, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_BYTES, &scrollbackBytes);
  CommandIndex index;
  auto now = std::chrono::steady_clock::time_point{};
  for (step = 0; step < 400; ++step) {
    now += std::chrono::milliseconds(std::uniform_int_distribution(0, 2000)(rng));
    const int kind = std::uniform_int_distribution(0, 9)(rng);
    const bool wasRunning = !index.entries().empty() && index.entries().back().phase == Phase::Running;
    if (kind < 4) {
      const auto lines = std::uniform_int_distribution(0, 8)(rng);
      for (int i = 0; i < lines; ++i) write(t, "out " + std::to_string(i) + "\r\n");
      if (std::uniform_int_distribution(0, 30)(rng) == 0) write(t, "\x1b[H\x1b[2J");
      if (std::uniform_int_distribution(0, 60)(rng) == 0) write(t, "\x1b[3J");
    } else {
      const auto markKind = static_cast<CommandMark::Kind>(kind % 4);
      CommandMark mark{.kind = markKind, .exitCode = {}, .command = "cmd" + std::to_string(step)};
      if (markKind == CommandMark::Kind::CommandEnd) mark.exitCode = std::uniform_int_distribution(0, 2)(rng);
      index.onMark(t, mark, now);
      if (markKind == CommandMark::Kind::PromptStart) write(t, "$ ");
      const auto* back = index.entries().empty() ? nullptr : &index.entries().back();
      switch (markKind) {
      case CommandMark::Kind::PromptStart: CHECK(back != nullptr && back->phase == Phase::Prompt); break;
      case CommandMark::Kind::OutputStart:
        CHECK(back != nullptr && back->phase == Phase::Running);
        if (!wasRunning) CHECK(back->command == mark.command && back->started == now);
        break;
      case CommandMark::Kind::CommandEnd:
        if (wasRunning) CHECK(back->phase == Phase::Done && back->exitCode == mark.exitCode
                              && back->started + back->duration == now);
        break;
      case CommandMark::Kind::InputStart: break;
      }
    }
    index.prune();
    checkInvariants(index);
  }
  ghostty_terminal_free(t);
  // Refs outlive the terminal: they report no value and still free.
  index.prune();
  CHECK(index.entries().empty());
}

} // namespace

int main() {
  for (seed = 1; seed <= 40; ++seed) run(seed % 2 == 0 ? 0 : 1'000'000);
  if (failures == 0) std::puts("command_index: ok");
  return failures == 0 ? 0 : 1;
}
