#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <sys/types.h>

class PollLoop;

namespace xdpu {


  using PortalResults = std::map<std::string, sdbus::Variant>;
  using PortalResponse = sdbus::Result<uint32_t, PortalResults>;

  // The option's value, or nullopt when it is missing or has another type.
  template <typename T> std::optional<T> optionValue(const PortalResults& options, const std::string& key) {
    const auto it = options.find(key);
    if (it == options.end() || !it->second.containsValueOfType<T>()) {
      return std::nullopt;
    }
    try {
      return it->second.get<T>();
    } catch (const std::exception&) {
      return std::nullopt;
    }
  }

  class Request {
  public:
    using CloseHandler = std::function<void()>;

    Request(sdbus::IConnection& connection, std::string path, CloseHandler closeHandler);
    ~Request();

    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;
    Request(Request&&) = delete;
    Request& operator=(Request&&) = delete;

    const std::string& path() const;
    bool closed() const;
    void setCloseHandler(CloseHandler closeHandler);
    void close();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

  struct ProcessResult {
    std::string output;
    int exitStatus = -1;
    int termSignal = 0;
    bool exited = false;
    bool signaled = false;

    bool success() const;
    bool commandNotFound() const;
  };

  class AsyncProcess : public std::enable_shared_from_this<AsyncProcess> {
  public:
    using Callback = std::function<void(ProcessResult)>;

    static std::shared_ptr<AsyncProcess> start(PollLoop& loop, std::string command, std::string input, Callback callback);
    ~AsyncProcess();

    AsyncProcess(const AsyncProcess&) = delete;
    AsyncProcess& operator=(const AsyncProcess&) = delete;
    AsyncProcess(AsyncProcess&&) = delete;
    AsyncProcess& operator=(AsyncProcess&&) = delete;

    void terminate();
    pid_t pid() const;

  private:
    AsyncProcess(PollLoop& loop, std::string command, std::string input, Callback callback);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace xdpu
