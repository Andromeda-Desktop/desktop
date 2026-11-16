#pragma once

#include "render/scene/terminal_grid_node.h"

#include <optional>
#include <span>

// Empty when the row holds no right-to-left letter, so it draws as stored.
[[nodiscard]] std::optional<TerminalBidiRow> terminalBidi(std::span<const TerminalCell> row);
