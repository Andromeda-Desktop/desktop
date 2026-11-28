#pragma once

#include <cstdint>

// An app's light or dark choice: System follows the desktop's color-scheme.
enum class ColorScheme : std::uint8_t { System, Light, Dark };
