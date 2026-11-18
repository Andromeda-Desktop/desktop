#pragma once

#include <cstdint>

// A display output, stable while it is connected. Wayland: the wl_output registry global name.
enum class OutputId : std::uint32_t { None = 0 };
