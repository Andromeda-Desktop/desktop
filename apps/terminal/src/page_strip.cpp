#include "page_strip.h"

#include <cmath>
#include <set>

namespace term {

  PageStrip::PageStrip(PageStrip** out, std::unique_ptr<Node> live, std::size_t livePage, std::size_t count, float gap, Build build)
      : m_out(out), m_live(live.get()), m_livePage(livePage), m_count(count), m_gap(gap), m_build(std::move(build)),
        m_at(static_cast<float>(livePage)) {
    setClipChildren(true);
    m_live->setParticipatesInLayout(false);
    addChild(std::move(live));
    setPagePosition(m_at);
    *m_out = this;
  }

  PageStrip::~PageStrip() {
    if (m_out != nullptr && *m_out == this) *m_out = nullptr;
  }

  void PageStrip::setPagePosition(float pages) {
    m_at = pages;
    std::set<std::size_t> wanted;
    const auto want = [&](long page) {
      if (page >= 0 && static_cast<std::size_t>(page) < m_count && static_cast<std::size_t>(page) != m_livePage) {
        wanted.insert(static_cast<std::size_t>(page));
      }
    };
    const auto live = static_cast<long>(m_livePage);
    want(live - 1);
    want(live + 1);
    want(static_cast<long>(std::floor(pages)));
    want(static_cast<long>(std::ceil(pages)));
    bool added = false;
    for (auto it = m_built.begin(); it != m_built.end();) {
      if (wanted.contains(it->first)) {
        ++it;
        continue;
      }
      removeChild(it->second);
      it = m_built.erase(it);
    }
    for (const std::size_t page : wanted) {
      if (m_built.contains(page)) continue;
      auto node = m_build(page);
      node->setParticipatesInLayout(false);
      m_built.emplace(page, addChild(std::move(node)));
      added = true;
    }
    if (added) markLayoutDirty();
    place();
  }

  void PageStrip::doLayout(Renderer& renderer) {
    Flex::doLayout(renderer);
    arrangePages(renderer);
  }

  void PageStrip::doArrange(Renderer& renderer, const LayoutRect& rect) {
    Flex::doArrange(renderer, rect);
    arrangePages(renderer);
  }

  void PageStrip::arrangePages(Renderer& renderer) {
    const LayoutRect page{.x = 0.0F, .y = 0.0F, .width = width(), .height = height()};
    m_live->arrange(renderer, page);
    for (const auto& [index, node] : m_built) node->arrange(renderer, page);
    place();
  }

  void PageStrip::place() {
    const auto x = [&](std::size_t page) { return (static_cast<float>(page) - m_at) * (width() + m_gap); };
    m_live->setPosition(x(m_livePage), 0.0F);
    for (const auto& [index, node] : m_built) node->setPosition(x(index), 0.0F);
  }

} // namespace term
