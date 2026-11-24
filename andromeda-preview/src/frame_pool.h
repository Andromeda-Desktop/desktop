#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace preview {

  // Compositor buffers made for helper frames. The helper sends the same memory again (its memfd ring, the decoder's
  // dmabuf pool), so a released buffer whose key matches is attached again instead of made anew.
  template <typename Buffer, typename Owner> class FramePool {
  public:
    // Identity of the frame memory and its layout: equal keys describe the same pixels in the same place.
    using Key = std::vector<std::uint64_t>;
    // An idle buffer not attached during this many presents is destroyed.
    static constexpr std::uint64_t kWindow = 8;

    struct Entry {
      Owner* owner = nullptr;
      Buffer* buffer = nullptr;
      Key key;
      // The helper frame the compositor may still read; empty once it released the buffer.
      std::optional<std::uint32_t> frame;
      std::uint64_t lastUse = 0;
    };

    // Attaches `frame` to an idle buffer of `key`; null when there is none.
    Entry* reuse(const Key& key, std::uint32_t frame) {
      const auto it = std::ranges::find_if(m_entries, [&](const auto& e) { return !e->frame && e->key == key; });
      if (it == m_entries.end()) {
        return nullptr;
      }
      return use(**it, frame);
    }

    Entry* add(Owner* owner, Buffer* buffer, Key key, std::uint32_t frame) {
      m_entries.push_back(std::make_unique<Entry>(Entry{.owner = owner, .buffer = buffer, .key = std::move(key)}));
      return use(*m_entries.back(), frame);
    }

    // The compositor released the buffer: the frame it showed goes back to the helper.
    static std::optional<std::uint32_t> release(Entry& entry) { return std::exchange(entry.frame, std::nullopt); }

    // Destroys idle buffers outside the window; buffers the compositor holds stay.
    template <typename Destroy> void trim(Destroy destroy) {
      std::erase_if(m_entries, [&](const auto& e) {
        if (e->frame || e->lastUse + kWindow > m_uses) {
          return false;
        }
        destroy(e->buffer);
        return true;
      });
    }

    template <typename Destroy> void clear(Destroy destroy) {
      for (const auto& e : m_entries) {
        destroy(e->buffer);
      }
      m_entries.clear();
    }

    [[nodiscard]] std::size_t size() const noexcept { return m_entries.size(); }

  private:
    Entry* use(Entry& entry, std::uint32_t frame) {
      entry.frame = frame;
      entry.lastUse = ++m_uses;
      return &entry;
    }

    std::vector<std::unique_ptr<Entry>> m_entries;
    std::uint64_t m_uses = 0;
  };

} // namespace preview
