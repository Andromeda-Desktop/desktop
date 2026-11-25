#pragma once

#include "security/secure_buffer.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace security {

  [[nodiscard]] bool initializeSecurityPrimitives();
  [[nodiscard]] bool securityPrimitivesReady();

  class SecureKey {
  public:
    static constexpr std::size_t Size = 32;

    SecureKey(const SecureKey&) = delete;
    SecureKey& operator=(const SecureKey&) = delete;
    SecureKey(SecureKey&&) noexcept = default;
    SecureKey& operator=(SecureKey&&) noexcept = default;

    [[nodiscard]] static std::optional<SecureKey> fromBuffer(SecureBuffer buffer);
    [[nodiscard]] static std::optional<SecureKey> fromBytes(std::span<const std::uint8_t> bytes);
    [[nodiscard]] static std::optional<SecureKey> generate();

    [[nodiscard]] std::span<const std::uint8_t, Size> bytes() const;

  private:
    explicit SecureKey(SecureBuffer buffer);

    SecureBuffer m_buffer;
  };

} // namespace security
