#pragma once

#include "util/secure_wipe.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace security {

  // The one holder of secrets (passwords, PSKs, keys): move-only, never logged, wiped when cleared or dropped.
  class SecureBuffer {
  public:
    SecureBuffer() = default;
    explicit SecureBuffer(std::size_t size)
        : m_data(size > 0 ? std::make_unique<std::uint8_t[]>(size) : nullptr), m_size(size) {}
    explicit SecureBuffer(std::span<const std::uint8_t> bytes) : SecureBuffer(bytes.size()) {
      std::ranges::copy(bytes, m_data.get());
    }
    explicit SecureBuffer(std::string_view text)
        : SecureBuffer(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())) {}
    ~SecureBuffer() { clear(); }

    SecureBuffer(const SecureBuffer&) = delete;
    SecureBuffer& operator=(const SecureBuffer&) = delete;

    SecureBuffer(SecureBuffer&& other) noexcept
        : m_data(std::move(other.m_data)), m_size(std::exchange(other.m_size, 0)) {}
    SecureBuffer& operator=(SecureBuffer&& other) noexcept {
      if (this != &other) {
        clear();
        m_data = std::move(other.m_data);
        m_size = std::exchange(other.m_size, 0);
      }
      return *this;
    }

    // Takes a secret that arrived in a std::string (a text field, a D-Bus argument) and wipes the string.
    [[nodiscard]] static SecureBuffer take(std::string& text) {
      SecureBuffer secret{std::string_view(text)};
      util::secureWipe(text);
      return secret;
    }

    [[nodiscard]] std::span<std::uint8_t> bytes() { return {m_data.get(), m_size}; }
    [[nodiscard]] std::span<const std::uint8_t> bytes() const { return {m_data.get(), m_size}; }
    [[nodiscard]] std::string_view text() const { return {reinterpret_cast<const char*>(m_data.get()), m_size}; }
    [[nodiscard]] std::size_t size() const { return m_size; }
    [[nodiscard]] bool empty() const { return m_size == 0; }

    void clear() {
      if (m_data != nullptr) {
        util::secureWipe(m_data.get(), m_size);
        m_data.reset();
        m_size = 0;
      }
    }

  private:
    std::unique_ptr<std::uint8_t[]> m_data;
    std::size_t m_size = 0;
  };

} // namespace security
