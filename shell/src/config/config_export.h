#pragma once

#include "config/config_types.h"
#include "protocols/toml.h"

namespace config_export {

  [[nodiscard]] toml::table serialize(const Config& config);

} // namespace config_export
