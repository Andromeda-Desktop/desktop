#include "compositors/andromeda/andromeda_event_handler.h"

#include "compositors/andromeda/andromeda_runtime.h"

#include <utility>

namespace compositors::andromeda {

  AndromedaEventHandler::AndromedaEventHandler(AndromedaRuntime& runtime, std::vector<std::string_view> families)
      : m_runtime(runtime), m_families(std::move(families)) {
    m_runtime.registerEventHandler(this);
  }

  AndromedaEventHandler::~AndromedaEventHandler() { m_runtime.unregisterEventHandler(this); }

} // namespace compositors::andromeda
