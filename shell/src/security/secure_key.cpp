#include "security/secure_key.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <sodium.h>
#include <utility>

namespace security {
  namespace {
    static_assert(SecureKey::Size == crypto_aead_xchacha20poly1305_ietf_KEYBYTES);

    std::once_flag g_sodiumInitOnce;
    std::atomic<bool> g_sodiumReady{false};
  } // namespace

  bool initializeSecurityPrimitives() {
    std::call_once(g_sodiumInitOnce, [] { g_sodiumReady.store(sodium_init() >= 0); });
    return g_sodiumReady.load();
  }

  bool securityPrimitivesReady() { return g_sodiumReady.load(); }

  SecureKey::SecureKey(SecureBuffer buffer) : m_buffer(std::move(buffer)) {}

  std::optional<SecureKey> SecureKey::fromBuffer(SecureBuffer buffer) {
    if (buffer.size() != Size) {
      return std::nullopt;
    }
    return SecureKey(std::move(buffer));
  }

  std::optional<SecureKey> SecureKey::fromBytes(std::span<const std::uint8_t> bytes) {
    return fromBuffer(SecureBuffer(bytes));
  }

  std::optional<SecureKey> SecureKey::generate() {
    if (!securityPrimitivesReady()) {
      return std::nullopt;
    }
    SecureBuffer buffer(Size);
    randombytes_buf(buffer.bytes().data(), buffer.size());
    return SecureKey(std::move(buffer));
  }

  std::span<const std::uint8_t, SecureKey::Size> SecureKey::bytes() const {
    return std::span<const std::uint8_t, Size>(m_buffer.bytes().data(), Size);
  }

} // namespace security
