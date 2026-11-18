#pragma once

#include "core/sys/sys.h"

#include <cstddef>
#include <string>

namespace util {

  inline void secureWipe(void* data, std::size_t len) { sys::secureZero(data, len); }

  // Wipes the whole buffer, also the bytes between size() and capacity().
  inline void secureWipe(std::string& value) {
    value.resize(value.capacity());
    sys::secureZero(value.data(), value.size());
    value.clear();
  }

} // namespace util
