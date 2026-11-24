#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <string_view>

struct sd_bus;
struct sd_bus_message;
struct sd_bus_slot;
struct wl_event_loop;

namespace andromeda::bus {

  enum class Kind : std::uint8_t { System, User };

  class Connection;

  // One consumer's share of the compositor's bus connection. All clients of one loop and kind share one sd_bus,
  // pumped on that loop; the last client closes it. A lost connection opens again after 2 s, then up to every 60 s:
  // onChange runs with nullptr at the loss and with the new bus at the reopen. Replies and kept slots never reach a
  // client that died.
  class Client {
  public:
    // nullptr: the connection was lost before an answer came.
    using Reply = std::function<void(sd_bus_message*)>;

    Client(wl_event_loop* loop, Kind kind);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // nullptr while not connected.
    [[nodiscard]] sd_bus* get() const;
    void onChange(std::function<void(sd_bus*)> changed);
    // A slot for sd_bus_match_* or sd_bus_add_object_vtable; this client unrefs it when it dies or the bus is lost.
    sd_bus_slot** keep();
    // Sends a method call; reply runs exactly once while this client lives. timeoutUs 0 is the sd-bus default.
    bool call(sd_bus_message* message, Reply reply, std::uint64_t timeoutUs = 0);
    // Re-arms the loop after a send that did not go through call().
    void sent();

  private:
    friend class Connection;
    void lost();

    std::shared_ptr<Connection> m_connection;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    std::list<sd_bus_slot*> m_slots;
    std::function<void(sd_bus*)> m_changed;
  };

  // Reads an a{sv} (GetAll, PropertiesChanged). For each entry, entry(name) reads the variant and returns true, or
  // returns false and the variant is skipped. False when the message holds no a{sv} or an entry is malformed.
  bool readDict(sd_bus_message* message, const std::function<bool(std::string_view name)>& entry);

} // namespace andromeda::bus
