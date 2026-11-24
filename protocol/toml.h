#pragma once

// toml++ without the warnings its header raises under the project flags.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#include <toml++/toml.hpp>
#pragma GCC diagnostic pop
