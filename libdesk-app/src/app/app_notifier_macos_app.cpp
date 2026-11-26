#include "app/desk_app.h"

#include <string>

std::string deskAppName(const DeskApp& app) { return app.info().name; }
