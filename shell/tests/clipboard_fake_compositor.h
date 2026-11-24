#pragma once

// A fake data-control compositor for ClipboardService tests: serves the payload a client would write into the
// receive pipe, and records the selections the shell claims.

#include "security/secret_store.h"
#include "clipboard/clipboard_service.h"

#include <cerrno>
#include <cstdint>
#include <poll.h>
#include <span>
#include <string>
#include <unistd.h>
#include <vector>

namespace clipboard_test {

class UnavailableSecretBackend final : public security::SecretStoreBackend {
  public:
    security::SecretStoreBackendResult probe(security::SecretStoreCancellation&) override { return unavailable(); }

    security::SecretStoreBackendResult
    lookup(const security::SecretStoreAttributes&, security::SecretStoreCancellation&) override {
      return unavailable();
    }

    security::SecretStoreBackendResult store(
        const security::SecretStoreAttributes&, std::span<const std::uint8_t>, const std::string&,
        security::SecretStoreCancellation&
    ) override {
      return unavailable();
    }

    security::SecretStoreBackendResult
    erase(const security::SecretStoreAttributes&, security::SecretStoreCancellation&) override {
      return unavailable();
    }

  private:
    static security::SecretStoreBackendResult unavailable() {
      return {
          .status = security::SecretStoreStatus::Unavailable,
          .errorCategory = security::SecretStoreErrorCategory::ProviderUnavailable,
      };
    }
  };

  // Stands in for the compositor: serves the payload a client would write into
  // the receive pipe, and records the selections the shell claims.
  struct FakeCompositor {
    std::vector<std::uint8_t> payload;
    std::size_t written = 0;
    int writeFd = -1;
    std::vector<std::string> offeredMimeTypes;
    int claims = 0;
    int sources = 0;
  };

  inline FakeCompositor* gFake = nullptr;
  inline int gManager = 0;
  inline int gDevice = 0;
  inline int gSource = 0;
  inline wl_seat* const gSeat = reinterpret_cast<wl_seat*>(&gManager);

  inline void* fakeGetDataDevice(void*, wl_seat*) { return &gDevice; }
  inline void fakeDestroyDevice(void*) {}
  inline int fakeAddDeviceListener(void*, const void*, void*) { return 0; }

  inline void* fakeCreateDataSource(void*) {
    ++gFake->sources;
    gFake->offeredMimeTypes.clear();
    return &gSource;
  }

  inline void fakeDestroySource(void*) {}
  inline int fakeAddSourceListener(void*, const void*, void*) { return 0; }
  inline void fakeSourceOffer(void*, const char* mimeType) { gFake->offeredMimeTypes.emplace_back(mimeType); }
  inline void fakeDeviceSetSelection(void*, void*) { ++gFake->claims; }
  inline void fakeDestroyOffer(void*) {}
  inline int fakeAddOfferListener(void*, const void*, void*) { return 0; }

  // Serves the payload the way a real client does: the pipe is non-blocking and
  // holds 64 KiB, so anything larger has to be written as the reader drains it.
  // EOF is signalled by closing this side once everything is out.
  inline void pumpWrites() {
    FakeCompositor& fake = *gFake;
    if (fake.writeFd < 0) {
      return;
    }
    while (fake.written < fake.payload.size()) {
      const ssize_t n = ::write(fake.writeFd, fake.payload.data() + fake.written, fake.payload.size() - fake.written);
      if (n > 0) {
        fake.written += static_cast<std::size_t>(n);
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return;
      }
      break;
    }
    ::close(fake.writeFd);
    fake.writeFd = -1;
  }

  // The service closes its own copy of the write end right after this returns,
  // so the duplicate here is what keeps the pipe open until the payload is out.
  inline void fakeOfferReceive(void*, const char*, int fd) {
    gFake->written = 0;
    gFake->writeFd = ::dup(fd);
    pumpWrites();
  }

  inline const DataControlOps& fakeOps() {
    static const DataControlOps ops = {
        .managerInterfaceName = "fake_data_control",
        .bindManager = nullptr,
        .destroyManager = nullptr,
        .getDataDevice = fakeGetDataDevice,
        .destroyDevice = fakeDestroyDevice,
        .addDeviceListener = fakeAddDeviceListener,
        .createDataSource = fakeCreateDataSource,
        .destroySource = fakeDestroySource,
        .addSourceListener = fakeAddSourceListener,
        .sourceOffer = fakeSourceOffer,
        .deviceSetSelection = fakeDeviceSetSelection,
        .destroyOffer = fakeDestroyOffer,
        .addOfferListener = fakeAddOfferListener,
        .offerReceive = fakeOfferReceive,
    };
    return ops;
  }

  inline void drainReads(ClipboardService& clipboard) {
    for (int attempt = 0; attempt < 20000; ++attempt) {
      std::vector<pollfd> fds;
      const std::size_t count = clipboard.addPollFds(fds);
      if (count == 0) {
        if (gFake->writeFd >= 0) {
          ::close(gFake->writeFd);
          gFake->writeFd = -1;
        }
        clipboard.dispatchPollEvents(fds, 0, 0);
        return;
      }
      const int ready = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), 500);
      if (ready <= 0) {
        return;
      }
      clipboard.dispatchPollEvents(fds, 0, count);
      pumpWrites();
    }
  }

  // Plays out one copy: a client offers a type, becomes the selection owner and
  // serves its bytes.
  inline void simulateCopy(ClipboardService& clipboard, void* offer, const char* mimeType, std::vector<std::uint8_t> payload) {
    gFake->payload = std::move(payload);
    gFake->written = 0;
    clipboard.handleDataOffer(offer);
    clipboard.handleOfferMimeType(offer, mimeType);
    clipboard.handleSelection(offer);
    drainReads(clipboard);
  }

} // namespace clipboard_test
