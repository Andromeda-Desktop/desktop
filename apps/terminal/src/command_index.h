#pragma once

#include <ghostty/vt.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace term {

struct CommandMark {
  enum class Kind : std::uint8_t { PromptStart, InputStart, OutputStart, CommandEnd };
  Kind kind;
  std::optional<int> exitCode;
  std::string command; // OutputStart: the command line, when the shell sends it
};

struct TrackedRefFree {
  void operator()(GhosttyTrackedGridRef ref) const { ghostty_tracked_grid_ref_free(ref); }
};
using TrackedRef = std::unique_ptr<std::remove_pointer_t<GhosttyTrackedGridRef>, TrackedRefFree>;

// One shell command from its OSC 133 marks. Phase: Prompt (133;A seen) -> Running (133;C) -> Done (133;D, or
// the next prompt when the shell sends no D).
struct CommandEntry {
  enum class Phase : std::uint8_t { Prompt, Running, Done };
  Phase phase = Phase::Prompt;
  TrackedRef prompt; // the cell of the 133;A cursor
  TrackedRef output; // the cell of the 133;C cursor; null in Prompt
  std::string cwdUri; // OSC 7 at the prompt
  // The git branch of cwdUri, read on the UI thread the first time the prompt row is shown.
  enum class Branch : std::uint8_t { Unread, None, Known };
  Branch branchState = Branch::Unread;
  std::string branch; // Known only
  std::string command;
  std::optional<int> exitCode;
  std::chrono::steady_clock::time_point started;
  std::chrono::steady_clock::duration duration{};
};

// The commands of one terminal, oldest first. Every call runs under the lock of the terminal that owns the refs.
// Invariants: only the last entry can be open (Prompt or Running); prompt rows strictly increase.
class CommandIndex {
public:
  void onMark(GhosttyTerminal terminal, const CommandMark& mark, std::chrono::steady_clock::time_point now);
  // Drops entries whose prompt left the terminal (scrollback trimmed or cleared, or no scrollback).
  void prune();
  const std::vector<CommandEntry>& entries() const { return m_entries; }
  std::vector<CommandEntry>& entries() { return m_entries; }

  // The ref's row in `tag` space, or nullopt when it has no value there.
  static std::optional<std::uint32_t> row(GhosttyTrackedGridRef ref, GhosttyPointTag tag);

private:
  CommandEntry* start(GhosttyTerminal terminal);

  std::vector<CommandEntry> m_entries;
};

} // namespace term
