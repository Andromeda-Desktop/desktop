// The control transport on Linux: org.andromeda.Terminal1 on the session bus, a typed face over controlCall().
#include "terminal_app.h"
#include "terminal_control.h"

#include "dbus/session_bus.h"

#include <nlohmann/json.hpp>
#include <sdbus-c++/sdbus-c++.h>

namespace term {

  namespace {

    using Entry = std::map<std::string, sdbus::Variant>;

    // The D-Bus types of the control records: ids are t, other numbers u, lists of ids at.
    sdbus::Variant toVariant(const std::string& key, const nlohmann::json& value) {
      if (value.is_boolean()) {
        return sdbus::Variant{value.get<bool>()};
      }
      if (value.is_number()) {
        return key == "id" ? sdbus::Variant{value.get<std::uint64_t>()} : sdbus::Variant{value.get<std::uint32_t>()};
      }
      if (value.is_array()) {
        return sdbus::Variant{value.get<std::vector<std::uint64_t>>()};
      }
      return sdbus::Variant{value.get<std::string>()};
    }

    nlohmann::json toJson(const sdbus::Variant& value) {
      const std::string type = value.peekValueType();
      if (type == "t") {
        return value.get<std::uint64_t>();
      }
      if (type == "u") {
        return value.get<std::uint32_t>();
      }
      if (type == "b") {
        return value.get<bool>();
      }
      if (type == "at") {
        return value.get<std::vector<std::uint64_t>>();
      }
      if (type == "as") {
        return value.get<std::vector<std::string>>();
      }
      if (type == "s") {
        return value.get<std::string>();
      }
      throw control::Error(control::kInvalidArgs, "unsupported type " + type);
    }

    std::vector<Entry> toEntries(const nlohmann::json& records) {
      std::vector<Entry> out;
      for (const auto& record : records) {
        Entry entry;
        for (const auto& [key, value] : record.items()) {
          entry.emplace(key, toVariant(key, value));
        }
        out.push_back(std::move(entry));
      }
      return out;
    }

    nlohmann::json toJson(const Entry& entry) {
      nlohmann::json out = nlohmann::json::object();
      for (const auto& [key, value] : entry) {
        out[key] = toJson(value);
      }
      return out;
    }

    class DbusServer final : public control::Server {
    public:
      explicit DbusServer(std::unique_ptr<sdbus::IObject> object) : m_object(std::move(object)) {}
      void emitSignal(const char* name) override { m_object->emitSignal(name).onInterface(control::kInterface); }

    private:
      std::unique_ptr<sdbus::IObject> m_object;
    };

  } // namespace

  void TerminalApp::exportControl() {
    if (bus() == nullptr) {
      return;
    }
    // Runs a control call; a control::Error becomes the D-Bus error of the same name.
    const auto run = [this](const char* method, nlohmann::json args) {
      try {
        return controlCall(method, args);
      } catch (const control::Error& e) {
        throw sdbus::Error(sdbus::Error::Name{e.name}, e.what());
      }
    };
    auto object = sdbus::createObject(bus()->connection(), sdbus::ObjectPath{control::kPath});
    object
        ->addVTable(
            sdbus::registerMethod("ListSessions").withOutputParamNames("sessions").implementedAs([run] {
              return toEntries(run("ListSessions", nlohmann::json::array()));
            }),
            sdbus::registerMethod("ListWorkspaces").withOutputParamNames("workspaces").implementedAs([run] {
              return toEntries(run("ListWorkspaces", nlohmann::json::array()));
            }),
            sdbus::registerMethod("Open")
                .withInputParamNames("options")
                .withOutputParamNames("id")
                .implementedAs([run](const Entry& options) {
                  return run("Open", nlohmann::json::array({toJson(options)})).get<std::uint64_t>();
                }),
            sdbus::registerMethod("SendText")
                .withInputParamNames("id", "text", "paste")
                .implementedAs([run](std::uint64_t id, const std::string& text, bool paste) {
                  run("SendText", {id, text, paste});
                }),
            sdbus::registerMethod("GetText")
                .withInputParamNames("id", "extent")
                .withOutputParamNames("text")
                .implementedAs([run](std::uint64_t id, const std::string& extent) {
                  return run("GetText", {id, extent}).get<std::string>();
                }),
            sdbus::registerMethod("Focus").withInputParamNames("id").implementedAs([run](std::uint64_t id) {
              run("Focus", {id});
            }),
            sdbus::registerMethod("Close").withInputParamNames("id").implementedAs([run](std::uint64_t id) {
              run("Close", {id});
            }),
            sdbus::registerMethod("Rename")
                .withInputParamNames("id", "name")
                .implementedAs([run](std::uint64_t id, const std::string& name) { run("Rename", {id, name}); }),
            sdbus::registerSignal("SessionsChanged"), sdbus::registerSignal("WorkspacesChanged"))
        .forInterface(control::kInterface);
    m_control = std::make_unique<DbusServer>(std::move(object));
  }

  nlohmann::json control::call(const std::string& method, const nlohmann::json& args) {
    try {
      auto bus = sdbus::createSessionBusConnection();
      auto proxy = sdbus::createProxy(*bus, sdbus::ServiceName{kService}, sdbus::ObjectPath{kPath});
      if (method == "ListSessions" || method == "ListWorkspaces") {
        std::vector<Entry> entries;
        proxy->callMethod(method).onInterface(kInterface).storeResultsTo(entries);
        nlohmann::json out = nlohmann::json::array();
        for (const Entry& entry : entries) {
          out.push_back(toJson(entry));
        }
        return out;
      }
      if (method == "Open") {
        Entry options;
        for (const auto& [key, value] : args.at(0).items()) {
          if (value.is_array()) {
            options.emplace(key, sdbus::Variant{value.get<std::vector<std::string>>()});
          } else {
            options.emplace(key, toVariant(key, value));
          }
        }
        std::uint64_t id = 0;
        proxy->callMethod(method).onInterface(kInterface).withArguments(options).storeResultsTo(id);
        return id;
      }
      const auto id = args.at(0).get<std::uint64_t>();
      if (method == "SendText") {
        proxy->callMethod(method).onInterface(kInterface).withArguments(id, args.at(1).get<std::string>(), args.at(2).get<bool>());
      } else if (method == "GetText") {
        std::string text;
        proxy->callMethod(method).onInterface(kInterface).withArguments(id, args.at(1).get<std::string>()).storeResultsTo(text);
        return text;
      } else if (method == "Rename") {
        proxy->callMethod(method).onInterface(kInterface).withArguments(id, args.at(1).get<std::string>());
      } else {
        proxy->callMethod(method).onInterface(kInterface).withArguments(id);
      }
      return nullptr;
    } catch (const sdbus::Error& e) {
      throw Error(e.getName(), e.getMessage());
    }
  }

} // namespace term
