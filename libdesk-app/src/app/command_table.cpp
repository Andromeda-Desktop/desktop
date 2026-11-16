#include "app/command_table.h"

#include "core/input/key_modifiers.h"
#include "core/log.h"

#include <algorithm>
#include <xkbcommon/xkbcommon.h>

namespace {
  constexpr Logger kLog("app-commands");

  // Shift turns "z" into "Z"; a chord written as Ctrl+Shift+Z must match either keysym.
  bool chordMatches(const KeyChord& chord, std::uint32_t sym, std::uint32_t modifiers) {
    return chord.modifiers == modifiers && xkb_keysym_to_lower(chord.sym) == xkb_keysym_to_lower(sym);
  }

  bool holds(const AppCommand& command, std::uint32_t sym, std::uint32_t modifiers) {
    return (command.shortcut.has_value() && chordMatches(*command.shortcut, sym, modifiers))
        || (command.alternate.has_value() && chordMatches(*command.alternate, sym, modifiers));
  }

  // Shift is part of typing a printable symbol such as "plus", but not of a letter or of Tab.
  bool shiftTypesSymbol(std::uint32_t sym, std::uint32_t modifiers) {
    return (modifiers & KeyMod::Shift) != 0 && xkb_keysym_to_utf32(sym) > 0x20
        && xkb_keysym_to_lower(sym) == xkb_keysym_to_upper(sym);
  }
} // namespace

void CommandTable::add(AppCommand command) {
  remove(command.id);
  for (std::optional<KeyChord>* slot : {&command.shortcut, &command.alternate}) {
    if (!slot->has_value()) {
      continue;
    }
    const KeyChord chord = **slot;
    const auto owner = std::ranges::find_if(m_commands, [&](const AppCommand& c) { return holds(c, chord.sym, chord.modifiers); });
    if (owner != m_commands.end()) {
      kLog.warn("shortcut {} of {} is held by {}; dropped", keyChordToString(chord), command.id, owner->id);
      slot->reset();
    }
  }
  m_commands.push_back(std::move(command));
}

void CommandTable::remove(std::string_view id) {
  std::erase_if(m_commands, [&](const AppCommand& c) { return c.id == id; });
}

const AppCommand* CommandTable::find(std::string_view id) const {
  const auto it = std::ranges::find_if(m_commands, [&](const AppCommand& c) { return c.id == id; });
  return it == m_commands.end() ? nullptr : &*it;
}

bool CommandTable::isEnabled(std::string_view id) const {
  const AppCommand* command = find(id);
  return command != nullptr && (!command->enabled || command->enabled());
}

bool CommandTable::run(std::string_view id) const {
  const AppCommand* command = find(id);
  if (command == nullptr || !command->run || (command->enabled && !command->enabled())) {
    return false;
  }
  // Copy: the command may replace the table entry while it runs.
  const auto run = command->run;
  run();
  return true;
}

const AppCommand* CommandTable::match(std::uint32_t sym, std::uint32_t modifiers) const {
  const auto find = [&](std::uint32_t mods) -> const AppCommand* {
    for (const AppCommand& command : m_commands) {
      if (holds(command, sym, mods) && (!command.enabled || command.enabled())) {
        return &command;
      }
    }
    return nullptr;
  };
  if (const AppCommand* exact = find(modifiers)) {
    return exact;
  }
  return shiftTypesSymbol(sym, modifiers) ? find(modifiers & ~KeyMod::Shift) : nullptr;
}

bool CommandTable::handleKey(std::uint32_t sym, std::uint32_t modifiers) const {
  const AppCommand* command = match(sym, modifiers);
  if (command == nullptr || !command->run) {
    return false;
  }
  // Copy: the command may replace the table entry while it runs.
  const auto run = command->run;
  run();
  return true;
}
