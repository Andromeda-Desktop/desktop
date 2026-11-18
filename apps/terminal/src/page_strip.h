#pragma once

#include "ui/controls/flex.h"

#include <cstddef>
#include <functional>
#include <map>
#include <memory>

namespace term {

  // Pages side by side in a clipped box one page wide: page i sits at (i - position) × (width + gap). The live page is the
  // owner's node; the other pages are built on demand and kept only while they show or sit next to the live page.
  class PageStrip : public Flex {
  public:
    using Build = std::function<std::unique_ptr<Node>(std::size_t page)>;

    // out: set to this strip, and back to null when the strip goes away.
    PageStrip(PageStrip** out, std::unique_ptr<Node> live, std::size_t livePage, std::size_t count, float gap, Build build);
    ~PageStrip() override;
    // The owner goes away before the scene: the strip stops writing to `out`.
    void forgetOwner() noexcept { m_out = nullptr; }

    // In pages; fractional while a finger or a slide moves the strip.
    void setPagePosition(float pages);

  protected:
    void doLayout(Renderer& renderer) override;
    void doArrange(Renderer& renderer, const LayoutRect& rect) override;

  private:
    void arrangePages(Renderer& renderer);
    void place();
    PageStrip** m_out;
    Node* m_live;
    std::size_t m_livePage;
    std::size_t m_count;
    float m_gap;
    Build m_build;
    float m_at;
    std::map<std::size_t, Node*> m_built;
  };

} // namespace term
