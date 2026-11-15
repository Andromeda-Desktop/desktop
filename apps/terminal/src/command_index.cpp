#include "command_index.h"

#include <algorithm>

namespace term {

namespace {

TrackedRef trackCursor(GhosttyTerminal terminal) {
  std::uint16_t x = 0;
  std::uint16_t y = 0;
  ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_CURSOR_X, &x);
  ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_CURSOR_Y, &y);
  GhosttyPoint point{};
  point.tag = GHOSTTY_POINT_TAG_ACTIVE;
  point.value.coordinate = {.x = x, .y = y};
  GhosttyTrackedGridRef ref = nullptr;
  if (ghostty_terminal_grid_ref_track(terminal, point, &ref) != GHOSTTY_SUCCESS) return {};
  return TrackedRef(ref);
}

void close(CommandEntry& entry, std::optional<int> exitCode, std::chrono::steady_clock::time_point now) {
  entry.phase = CommandEntry::Phase::Done;
  entry.exitCode = exitCode;
  entry.duration = now - entry.started;
}

} // namespace

std::optional<std::uint32_t> CommandIndex::row(GhosttyTrackedGridRef ref, GhosttyPointTag tag) {
  GhosttyPointCoordinate at{};
  if (ref == nullptr || ghostty_tracked_grid_ref_point(ref, tag, &at) != GHOSTTY_SUCCESS) return std::nullopt;
  return at.y;
}

CommandEntry* CommandIndex::start(GhosttyTerminal terminal) {
  TrackedRef ref = trackCursor(terminal);
  const auto at = row(ref.get(), GHOSTTY_POINT_TAG_SCREEN);
  if (!at) return nullptr;
  // A prompt with no command, or a row the new prompt overwrote (clear screen), names no command.
  std::erase_if(m_entries, [&](const CommandEntry& e) {
    const auto p = row(e.prompt.get(), GHOSTTY_POINT_TAG_SCREEN);
    return e.phase == CommandEntry::Phase::Prompt || !p || *p >= *at;
  });
  CommandEntry& entry = m_entries.emplace_back(CommandEntry{.prompt = std::move(ref)});
  GhosttyString pwd{};
  if (ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_PWD, &pwd) == GHOSTTY_SUCCESS && pwd.len > 0) {
    entry.cwdUri.assign(reinterpret_cast<const char*>(pwd.ptr), pwd.len);
  }
  return &entry;
}

void CommandIndex::onMark(GhosttyTerminal terminal, const CommandMark& mark,
                          std::chrono::steady_clock::time_point now) {
  CommandEntry* open = !m_entries.empty() && m_entries.back().phase != CommandEntry::Phase::Done ? &m_entries.back()
                                                                                                 : nullptr;
  switch (mark.kind) {
  case CommandMark::Kind::PromptStart:
    if (open != nullptr && open->phase == CommandEntry::Phase::Running) close(*open, std::nullopt, now);
    start(terminal);
    return;
  case CommandMark::Kind::InputStart: return;
  case CommandMark::Kind::OutputStart: {
    if (open != nullptr && open->phase == CommandEntry::Phase::Running) return;
    TrackedRef output = trackCursor(terminal);
    const auto at = row(output.get(), GHOSTTY_POINT_TAG_SCREEN);
    if (!at) return;
    // No 133;A before, or the cursor moved above it (screen cleared): the command starts here.
    if (open == nullptr || row(open->prompt.get(), GHOSTTY_POINT_TAG_SCREEN).value_or(*at + 1) > *at) {
      open = start(terminal);
      if (open == nullptr) return;
    }
    open->phase = CommandEntry::Phase::Running;
    open->output = std::move(output);
    open->command = mark.command;
    open->started = now;
    return;
  }
  case CommandMark::Kind::CommandEnd:
    if (open != nullptr && open->phase == CommandEntry::Phase::Running) close(*open, mark.exitCode, now);
    return;
  }
}

void CommandIndex::prune() {
  // Ghostty clamps a ref whose row scrolled out of a screen without history to the top row, so an older prompt
  // at or below a newer one left the terminal too.
  std::optional<std::uint32_t> below;
  for (auto it = m_entries.rbegin(); it != m_entries.rend();) {
    const auto p = row(it->prompt.get(), GHOSTTY_POINT_TAG_SCREEN);
    if (!p || (below && *p >= *below)) {
      it = std::make_reverse_iterator(m_entries.erase(std::next(it).base()));
    } else {
      below = p;
      ++it;
    }
  }
}

} // namespace term
