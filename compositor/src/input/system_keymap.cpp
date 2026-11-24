#include "input/system_keymap.h"

#include "core/log.h"

#include <memory>
#include <systemd/sd-bus.h>
#include <utility>

namespace andromeda {

  namespace {

    constexpr Logger kLog("keyboard");

    // localed is bus-activated, so the first call may wait for it to start.
    constexpr std::uint64_t kLocaledTimeoutUs = 2'000'000;

    // Reads GetAll's a{sv}; properties it does not report stay empty, as localed reports an unset field.
    bool readNames(sd_bus_message* reply, XkbNames& names) {
      if (sd_bus_message_is_method_error(reply, nullptr) != 0) {
        const sd_bus_error* error = sd_bus_message_get_error(reply);
        kLog.debug(
            "systemd-localed unavailable: {}", error != nullptr && error->message != nullptr ? error->message : "?"
        );
        return false;
      }
      return bus::readDict(reply, [&](std::string_view key) {
        std::string* field = key == "X11Layout" ? &names.layout
            : key == "X11Variant"               ? &names.variant
            : key == "X11Options"               ? &names.options
            : key == "X11Model"                 ? &names.model
                                                : nullptr;
        const char* text = nullptr;
        if (field == nullptr || sd_bus_message_read(reply, "v", "s", &text) < 0) {
          return false;
        }
        *field = text != nullptr ? text : "";
        return true;
      });
    }

  } // namespace

  SystemKeymapQuery::SystemKeymapQuery(wl_event_loop* loop) : m_bus(loop, bus::Kind::System) {}

  void SystemKeymapQuery::query(std::function<void(XkbNames)> done) {
    sd_bus_message* message = nullptr;
    if (m_bus.get() == nullptr
        || sd_bus_message_new_method_call(
               m_bus.get(), &message, "org.freedesktop.locale1", "/org/freedesktop/locale1",
               "org.freedesktop.DBus.Properties", "GetAll"
           ) < 0
        || sd_bus_message_append(message, "s", "org.freedesktop.locale1") < 0) {
      sd_bus_message_unref(message);
      kLog.debug("system bus unavailable; using the libxkbcommon default keymap");
      done({});
      return;
    }
    auto shared = std::make_shared<std::function<void(XkbNames)>>(std::move(done));
    const bool sent = m_bus.call(
        message,
        [shared](sd_bus_message* reply) {
          XkbNames names;
          if (reply == nullptr || !readNames(reply, names)) {
            names = {};
          }
          (*shared)(std::move(names));
        },
        kLocaledTimeoutUs
    );
    sd_bus_message_unref(message);
    if (!sent) {
      (*shared)({});
    }
  }

  XkbNames validSystemKeymap(XkbNames names) {
    if (names.empty()) {
      return {};
    }
    xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (context == nullptr) {
      return {};
    }
    const xkb_rule_names rules{
        .rules = nullptr,
        .model = names.model.empty() ? nullptr : names.model.c_str(),
        .layout = names.layout.empty() ? nullptr : names.layout.c_str(),
        .variant = names.variant.empty() ? nullptr : names.variant.c_str(),
        .options = names.options.empty() ? nullptr : names.options.c_str(),
    };
    xkb_keymap* keymap = xkb_keymap_new_from_names(context, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
    const bool valid = keymap != nullptr;
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    if (!valid) {
      kLog.warn(
          "ignoring systemd-localed keymap layout='{}' variant='{}' options='{}' model='{}' (invalid XKB "
          "configuration)",
          names.layout, names.variant, names.options, names.model
      );
      return {};
    }
    return names;
  }

} // namespace andromeda
