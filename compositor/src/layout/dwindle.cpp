#include "layout/dwindle.h"

#include "config/config.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>

// wlr_box and WLR_EDGE_* only; see the note in scrolling.cpp.
extern "C" {
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
}

namespace andromeda {

  namespace {

    struct DwindleSnapshot final : LayoutSnapshot {
      struct SavedNode {
        DwindleLayout::Node::Type type = DwindleLayout::Node::Leaf;
        std::unique_ptr<SavedNode> left;
        std::unique_ptr<SavedNode> right;
        double ratio = 0.5;
        bool locked = false;
        LayoutMemberId member = 0;
      };

      [[nodiscard]] LayoutMode mode() const override { return LayoutMode::Dwindle; }
      [[nodiscard]] size_t memberCount() const override { return members; }

      std::unique_ptr<SavedNode> root;
      size_t members = 0;
    };

  } // namespace

  void DwindleLayout::rebuildFlatColumns() {
    m_flatColumns.clear();
    for (const tiling::TileKey key : m_tree.keys()) {
      Column col;
      col.views.push_back(viewOf(key));
      col.heightWeights.push_back(1.0);
      col.widthFrac = 0.5;
      m_flatColumns.push_back(std::move(col));
    }
  }

  bool DwindleLayout::swapLeafViews(Node* first, Node* second) {
    if (first == nullptr
        || second == nullptr
        || first == second
        || first->type != Node::Leaf
        || second->type != Node::Leaf) {
      return false;
    }
    View* firstView = viewOf(first->key);
    View* secondView = viewOf(second->key);
    std::swap(first->key, second->key);
    for (LayoutTarget& target : m_targets) {
      if (target.view == firstView) {
        target.view = secondView;
      } else if (target.view == secondView) {
        target.view = firstView;
      }
    }
    rebuildFlatColumns();
    return true;
  }

  // Public Layout interface
  int DwindleLayout::columnOf(const View* view) const {
    for (size_t i = 0; i < m_flatColumns.size(); ++i) {
      if (!m_flatColumns[i].views.empty() && m_flatColumns[i].views[0] == view) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  int DwindleLayout::rowOf(const View* /*view*/) const { return 0; }

  LayoutCapture DwindleLayout::captureState() const {
    auto snapshot = std::make_shared<DwindleSnapshot>();
    LayoutCapture capture{.snapshot = snapshot, .members = {}};
    const auto saveNode = [&capture](auto&& self, const Node* node) -> std::unique_ptr<DwindleSnapshot::SavedNode> {
      if (node == nullptr) {
        return nullptr;
      }
      auto saved = std::make_unique<DwindleSnapshot::SavedNode>();
      saved->type = node->type;
      saved->ratio = node->ratio;
      saved->locked = node->locked;
      if (node->type == Node::Leaf) {
        const auto id = static_cast<LayoutMemberId>(capture.members.size());
        capture.members.push_back({.id = id, .view = viewOf(node->key)});
        saved->member = id;
      } else {
        saved->left = self(self, node->left.get());
        saved->right = self(self, node->right.get());
      }
      return saved;
    };
    snapshot->root = saveNode(saveNode, m_tree.root());
    snapshot->members = capture.members.size();
    return capture;
  }

  bool DwindleLayout::restoreState(const LayoutSnapshot& base, std::span<const LayoutMember> members) {
    const auto* snapshot = dynamic_cast<const DwindleSnapshot*>(&base);
    if (snapshot == nullptr || !m_tree.empty()) {
      return false;
    }
    const std::optional<std::vector<View*>> resolved = resolveLayoutMembers(snapshot->memberCount(), members);
    if (!resolved) {
      return false;
    }

    const auto restoreNode =
        [&resolved](auto&& self, const DwindleSnapshot::SavedNode* saved, Node* parent) -> std::unique_ptr<Node> {
      if (saved == nullptr) {
        return nullptr;
      }
      if (saved->type == Node::Leaf) {
        View* view = (*resolved)[static_cast<size_t>(saved->member)];
        if (view == nullptr) {
          return nullptr;
        }
        auto node = std::make_unique<Node>();
        node->type = Node::Leaf;
        node->parent = parent;
        node->key = keyOf(view);
        return node;
      }

      auto node = std::make_unique<Node>();
      node->type = saved->type;
      node->parent = parent;
      node->ratio = saved->ratio;
      node->locked = saved->locked;
      node->left = self(self, saved->left.get(), node.get());
      node->right = self(self, saved->right.get(), node.get());
      if (node->left == nullptr) {
        if (node->right != nullptr) {
          node->right->parent = parent;
        }
        return std::move(node->right);
      }
      if (node->right == nullptr) {
        node->left->parent = parent;
        return std::move(node->left);
      }
      return node;
    };

    m_tree.setRoot(restoreNode(restoreNode, snapshot->root.get(), nullptr));
    m_targets.clear();
    rebuildFlatColumns();
    return true;
  }

  void DwindleLayout::insertView(View* view, int columnIndex) {
    if (view == nullptr || findNode(view) != nullptr) {
      return;
    }
    if (m_tree.empty()) {
      m_tree.addRoot(keyOf(view));
      rebuildFlatColumns();
      return;
    }
    const int count = static_cast<int>(m_flatColumns.size());
    const int gap = std::clamp(columnIndex, 0, count);
    if (gap == 0) {
      m_tree.splitLeaf(m_tree.leafAt(0), keyOf(view), Node::AutoSplit, /*newFirst=*/true);
    } else {
      m_tree.splitLeaf(m_tree.leafAt(gap - 1), keyOf(view), Node::AutoSplit, /*newFirst=*/false);
    }
    rebuildFlatColumns();
  }

  void DwindleLayout::insertViewToward(View* view, int columnIndex, double cx, double cy) {
    const int gap = std::clamp(columnIndex, 0, static_cast<int>(m_flatColumns.size()));
    Node* target = gap > 0 ? m_tree.leafAt(gap - 1) : nullptr;
    if (view == nullptr || findNode(view) != nullptr || target == nullptr || target->area.width <= 0 || target->area.height <= 0) {
      insertView(view, columnIndex);
      return;
    }
    m_tree.splitLeafToward(target, keyOf(view), cx, cy);
    rebuildFlatColumns();
  }

  void DwindleLayout::insertViewIntoColumn(View* view, int columnIndex, int /*rowIndex*/) {
    if (view == nullptr || findNode(view) != nullptr) {
      return;
    }
    if (m_tree.empty()) {
      m_tree.addRoot(keyOf(view));
      rebuildFlatColumns();
      return;
    }
    const int count = static_cast<int>(m_flatColumns.size());
    Node* target = m_tree.leafAt(std::clamp(columnIndex, 0, count - 1));
    if (target != nullptr && target->type == Node::Leaf) {
      m_tree.splitLeaf(target, keyOf(view), Node::AutoSplit, /*newFirst=*/false);
    }
    rebuildFlatColumns();
  }

  bool DwindleLayout::consume(View* view, int direction) {
    Node* node = findNode(view);
    if ((direction != -1 && direction != 1) || node == nullptr) {
      return false;
    }
    Node* destination = findNode(andromeda::directionalNeighbor(m_targets, view, true, direction));
    return swapLeafViews(node, destination);
  }

  bool DwindleLayout::expel(View* view, int direction) { return consume(view, direction); }

  bool DwindleLayout::moveViewVertical(View* view, int direction) { return moveView(view, false, direction); }

  bool DwindleLayout::toggleSplit(View* view) { return m_tree.toggleSplit(findNode(view)); }

  bool DwindleLayout::moveView(View* view, bool horizontal, int direction) {
    Node* node = findNode(view);
    if ((direction != -1 && direction != 1) || node == nullptr || node->parent == nullptr) {
      return false;
    }
    const tiling::TileBox& root = m_tree.root()->area;
    const wlr_box content{.x = root.x, .y = root.y, .width = root.width, .height = root.height};
    const tiling::TileBox& box = node->area;
    if (content.width <= 0 || content.height <= 0 || box.width <= 0 || box.height <= 0) {
      return false;
    }

    // The first pixel across the gap from the middle of the edge the view leaves through. Outside the content area
    // there is no tile to enter, and the caller falls back to its output or workspace action.
    const int gap = m_config->totalGap;
    double focalX = box.x + (box.width / 2.0);
    double focalY = box.y + (box.height / 2.0);
    if (horizontal) {
      focalX = direction < 0 ? box.x - gap - 1 : box.x + box.width + gap;
    } else {
      focalY = direction < 0 ? box.y - gap - 1 : box.y + box.height + gap;
    }
    if (focalX < content.x
        || focalX >= content.x + content.width
        || focalY < content.y
        || focalY >= content.y + content.height) {
      return false;
    }

    // Toward a lone sibling across the parent's own split, the view trades sides with it.
    Node* parent = node->parent;
    const bool first = parent->left.get() == node;
    const Node* sibling = first ? parent->right.get() : parent->left.get();
    if (parent->type == (horizontal ? Node::HSplit : Node::VSplit)
        && first == (direction > 0)
        && sibling != nullptr
        && sibling->type == Node::Leaf) {
      std::swap(parent->left, parent->right);
      parent->ratio = 0.5;
      arrangeArea(content);
      return true;
    }

    // Otherwise the view leaves its split, which hands its area back to its sibling, and splits the tile it enters.
    m_tree.detach(node);
    arrangeArea(content);
    const LayoutTarget* entered = nullptr;
    double nearest = std::numeric_limits<double>::infinity();
    for (const LayoutTarget& target : m_targets) {
      const double dx = std::max({target.x - focalX, 0.0, focalX - (target.x + target.width - 1)});
      const double dy = std::max({target.y - focalY, 0.0, focalY - (target.y + target.height - 1)});
      if (const double distance = (dx * dx) + (dy * dy); distance < nearest) {
        nearest = distance;
        entered = &target;
      }
    }
    m_tree.splitLeafToward(findNode(entered->view), keyOf(view), focalX, focalY);
    arrangeArea(content);
    return true;
  }

  bool DwindleLayout::swapViews(View* a, View* b) { return swapLeafViews(findNode(a), findNode(b)); }

  void DwindleLayout::removeView(View* view) {
    Node* node = findNode(view);
    if (node == nullptr) {
      return;
    }
    m_tree.detach(node);
    std::erase_if(m_targets, [view](const LayoutTarget& t) { return t.view == view; });
    rebuildFlatColumns();
  }

  std::optional<TileSlot> DwindleLayout::slotOf(const View* view) const {
    const Node* node = findNode(view);
    if (node == nullptr) {
      return std::nullopt;
    }
    return m_tree.placeOf(node);
  }

  bool DwindleLayout::insertAtSlot(View* view, const TileSlot& slot) {
    const auto* place = std::get_if<tiling::DwindleTree::Place>(&slot);
    if (place == nullptr || view == nullptr || findNode(view) != nullptr) {
      return false;
    }
    m_tree.insertAt(keyOf(view), *place);
    rebuildFlatColumns();
    return true;
  }

  void DwindleLayout::moveColumn(int from, int to) {
    const int count = static_cast<int>(m_flatColumns.size());
    if (from < 0 || from >= count) {
      return;
    }
    const int destination = std::clamp(to, 0, count - 1);
    if (from == destination) {
      return;
    }
    Node* a = m_tree.leafAt(from);
    Node* b = m_tree.leafAt(destination);
    swapLeafViews(a, b);
  }

  void DwindleLayout::arrange(const wlr_box& usable) {
    const int edgePad = m_config->edgePad;
    arrangeArea({
        .x = usable.x + edgePad,
        .y = usable.y + edgePad,
        .width = std::max(1, usable.width - 2 * edgePad),
        .height = std::max(1, usable.height - 2 * edgePad),
    });
  }

  void DwindleLayout::arrangeArea(const wlr_box& area) {
    m_targets.clear();
    const tiling::TileBox box{.x = area.x, .y = area.y, .width = area.width, .height = area.height};
    for (const tiling::Tile& tile : m_tree.arrange(box, m_config->totalGap, m_config->dwindle.preserveSplit)) {
      m_targets.push_back(
          {.view = viewOf(tile.key), .x = tile.box.x, .y = tile.box.y, .width = tile.box.width, .height = tile.box.height}
      );
    }
    rebuildFlatColumns();
  }

  Layout::InitialSize DwindleLayout::initialSize(
      const wlr_box& usable, bool /*wantMaximize*/, std::optional<double> /*ruleExtent*/,
      std::optional<int> /*ruleExtentPx*/, const View* splitAnchor
  ) const {
    const wlr_box content = contentArea(usable);
    // Scrolling extents do not affect a splitting layout. The first leaf owns the whole area.
    if (m_flatColumns.empty()) {
      return {.width = content.width, .height = content.height};
    }
    // A focused insert splits the anchor leaf. Without a tiled anchor, the caller appends and splits the last leaf.
    const Node* target = findNode(splitAnchor);
    if (target == nullptr || target->type != Node::Leaf) {
      target = m_tree.leafAt(static_cast<int>(m_flatColumns.size()) - 1);
    }
    const int width = target != nullptr && target->area.width > 0 ? target->area.width : content.width;
    const int height = target != nullptr && target->area.height > 0 ? target->area.height : content.height;
    const auto half = [gap = m_config->totalGap](int span) {
      return std::max(1, static_cast<int>(std::lround(0.5 * span)) - gap / 2);
    };
    if (width >= height) {
      return {.width = half(width), .height = height};
    }
    return {.width = width, .height = half(height)};
  }

  wlr_box DwindleLayout::targetBox(const View* view) const {
    const auto it = std::ranges::find_if(m_targets, [view](const LayoutTarget& t) { return t.view == view; });
    if (it == m_targets.end()) {
      return {};
    }
    return {.x = it->x, .y = it->y, .width = it->width, .height = it->height};
  }

  int DwindleLayout::leafIndexAt(double cx, double cy) const {
    for (int i = 0; i < static_cast<int>(m_targets.size()); ++i) {
      const LayoutTarget& t = m_targets[i];
      if (cx >= t.x && cx < t.x + t.width && cy >= t.y && cy < t.y + t.height) {
        return i;
      }
    }
    return -1;
  }

  wlr_box DwindleLayout::targetBoxByIndex(int index) const {
    if (index < 0 || index >= static_cast<int>(m_targets.size())) {
      return {};
    }
    const LayoutTarget& t = m_targets[static_cast<size_t>(index)];
    return {.x = t.x, .y = t.y, .width = t.width, .height = t.height};
  }

  std::optional<View*> DwindleLayout::focusHorizontalLeaf(const View* view, int direction) const {
    return andromeda::directionalNeighbor(m_targets, view, true, direction);
  }

  std::optional<View*> DwindleLayout::focusVerticalLeaf(const View* view, int direction) const {
    return andromeda::directionalNeighbor(m_targets, view, false, direction);
  }

  std::vector<View*> DwindleLayout::focusPeers(const View* from, const View* target) const {
    std::vector<View*> peers;
    for (const tiling::TileKey key : m_tree.peers(findNode(from), findNode(target))) {
      peers.push_back(viewOf(key));
    }
    return peers;
  }

  bool DwindleLayout::cycleWidth(int columnIndex, int direction) {
    // Each step reads the splits afresh: applying a fraction can move outer splits, which the cached products miss.
    return cycleFractionPreset(
        m_config->extentPresets, widthFraction(columnIndex), direction,
        [&](double fraction) { return setWidthFraction(columnIndex, fraction); },
        [&] { return widthFraction(columnIndex); }
    );
  }

  bool DwindleLayout::toggleFullWidth(int columnIndex) {
    Node* node = m_tree.leafAt(columnIndex);
    const std::vector<Split> axisSplits = m_tree.splits(node, Node::HSplit);
    if (axisSplits.empty()) {
      return false;
    }
    double current = 1.0;
    double maximum = 1.0;
    for (const Split& split : axisSplits) {
      current *= tiling::DwindleTree::splitShare(split);
      maximum *= 0.9;
    }
    if (current >= maximum - kFractionEpsilon) {
      tiling::DwindleTree::applyFraction(axisSplits, 0.5);
      return false;
    }
    tiling::DwindleTree::applyFraction(axisSplits, 1.0);
    return true;
  }

  bool DwindleLayout::isFullWidth(int columnIndex) const {
    const std::vector<Split> axisSplits = m_tree.splits(m_tree.leafAt(columnIndex), Node::HSplit);
    if (axisSplits.empty()) {
      return false;
    }
    double current = 1.0;
    double maximum = 1.0;
    for (const Split& split : axisSplits) {
      current *= tiling::DwindleTree::splitShare(split);
      maximum *= 0.9;
    }
    return current >= maximum - kFractionEpsilon;
  }

  bool DwindleLayout::setWidthFraction(int columnIndex, double fraction) {
    return tiling::DwindleTree::applyFraction(m_tree.splits(m_tree.leafAt(columnIndex), Node::HSplit), fraction);
  }

  void DwindleLayout::clearFullWidthState(int /*columnIndex*/) {}

  double DwindleLayout::widthFraction(int columnIndex) const {
    const std::vector<Split> axisSplits = m_tree.splits(m_tree.leafAt(columnIndex), Node::HSplit);
    if (axisSplits.empty()) {
      return 1.0;
    }
    double fraction = 1.0;
    for (const Split& split : axisSplits) {
      fraction *= tiling::DwindleTree::splitShare(split);
    }
    return fraction;
  }

  double DwindleLayout::heightFraction(const View* view) const {
    const std::vector<Split> axisSplits = m_tree.splits(findNode(view), Node::VSplit);
    if (axisSplits.empty()) {
      return 1.0;
    }
    double fraction = 1.0;
    for (const Split& split : axisSplits) {
      fraction *= tiling::DwindleTree::splitShare(split);
    }
    return fraction;
  }

  bool DwindleLayout::setHeightFraction(View* view, double fraction) {
    return tiling::DwindleTree::applyFraction(m_tree.splits(findNode(view), Node::VSplit), fraction);
  }

  // Drag-and-drop directional insertion
  void DwindleLayout::insertViewSplitOnView(View* newView, View* targetView, uint32_t edge) {
    if (newView == nullptr || findNode(newView) != nullptr) {
      return;
    }
    Node* target = findNode(targetView);
    if (target == nullptr || target->type != Node::Leaf) {
      return;
    }
    m_tree.splitOn(target, keyOf(newView), edge);
    rebuildFlatColumns();
  }

  // Interactive resize
  DwindleLayout::Node* DwindleLayout::boundaryNode(const View* view, uint32_t edge) const {
    return m_tree.boundary(findNode(view), edge);
  }

  uint32_t DwindleLayout::resizableEdges(const View* view) const {
    uint32_t edges = 0;
    for (const uint32_t edge : {WLR_EDGE_LEFT, WLR_EDGE_RIGHT, WLR_EDGE_TOP, WLR_EDGE_BOTTOM}) {
      if (boundaryNode(view, edge) != nullptr) {
        edges |= edge;
      }
    }
    return edges;
  }

  bool DwindleLayout::resizeBoundary(const View* view, uint32_t edge, double* outRatio, double* outSpan) const {
    Node* parent = boundaryNode(view, edge);
    if (parent == nullptr) {
      return false;
    }
    const bool horizontal = parent->type == Node::HSplit;
    const double span = horizontal ? parent->area.width : parent->area.height;
    if (span <= 0) {
      return false;
    }
    if (outRatio != nullptr) {
      *outRatio = parent->ratio;
    }
    if (outSpan != nullptr) {
      *outSpan = span;
    }
    return true;
  }

  bool DwindleLayout::setResizeBoundary(View* view, uint32_t edge, double ratio) {
    Node* parent = boundaryNode(view, edge);
    if (parent == nullptr) {
      return false;
    }
    parent->ratio = std::clamp(ratio, 0.05, 0.95);
    return true;
  }

  namespace {

    // Layout-owned resize session: nudges the split ratios behind the grabbed
    // horizontal/vertical edges by (delta / boundary span).
    class DwindleResizeGrab : public ResizeGrab {
    public:
      DwindleResizeGrab(
          DwindleLayout* layout, View* view, bool hActive, uint32_t hEdge, double hRatio, double hSpan, bool vActive,
          uint32_t vEdge, double vRatio, double vSpan
      )
          : m_layout(layout), m_view(view), m_hActive(hActive), m_hEdge(hEdge), m_hRatio(hRatio), m_hSpan(hSpan),
            m_vActive(vActive), m_vEdge(vEdge), m_vRatio(vRatio), m_vSpan(vSpan) {}

      void applyDelta(double dx, double dy, const wlr_box& /*usable*/) override {
        if (m_hActive && m_hSpan > 0) {
          m_layout->setResizeBoundary(m_view, m_hEdge, m_hRatio + dx / m_hSpan);
        }
        if (m_vActive && m_vSpan > 0) {
          m_layout->setResizeBoundary(m_view, m_vEdge, m_vRatio + dy / m_vSpan);
        }
      }

      [[nodiscard]] const Layout* ownerLayout() const override { return m_layout; }

    private:
      DwindleLayout* m_layout;
      View* m_view;
      bool m_hActive;
      uint32_t m_hEdge;
      double m_hRatio;
      double m_hSpan;
      bool m_vActive;
      uint32_t m_vEdge;
      double m_vRatio;
      double m_vSpan;
    };

  } // namespace

  uint32_t DwindleLayout::resizeEdgesAt(const View* view, double cx, double cy) const {
    const wlr_box box = targetBox(view);
    if (box.width <= 0 || box.height <= 0) {
      return 0;
    }
    return sanitizeResizeEdges(view, resizeEdgesForPoint(box, cx, cy));
  }

  uint32_t DwindleLayout::sanitizeResizeEdges(const View* view, uint32_t edges) const {
    return edges & resizableEdges(view);
  }

  std::unique_ptr<ResizeGrab> DwindleLayout::beginResize(View* view, uint32_t edges, const wlr_box& /*usable*/) {
    bool hActive = false;
    bool vActive = false;
    uint32_t hEdge = 0;
    uint32_t vEdge = 0;
    double hRatio = 0;
    double hSpan = 0;
    double vRatio = 0;
    double vSpan = 0;
    double ratio = 0;
    double span = 0;
    if ((edges & WLR_EDGE_LEFT) != 0 && resizeBoundary(view, WLR_EDGE_LEFT, &ratio, &span)) {
      hActive = true;
      hEdge = WLR_EDGE_LEFT;
      hRatio = ratio;
      hSpan = span;
    } else if ((edges & WLR_EDGE_RIGHT) != 0 && resizeBoundary(view, WLR_EDGE_RIGHT, &ratio, &span)) {
      hActive = true;
      hEdge = WLR_EDGE_RIGHT;
      hRatio = ratio;
      hSpan = span;
    }
    if ((edges & WLR_EDGE_TOP) != 0 && resizeBoundary(view, WLR_EDGE_TOP, &ratio, &span)) {
      vActive = true;
      vEdge = WLR_EDGE_TOP;
      vRatio = ratio;
      vSpan = span;
    } else if ((edges & WLR_EDGE_BOTTOM) != 0 && resizeBoundary(view, WLR_EDGE_BOTTOM, &ratio, &span)) {
      vActive = true;
      vEdge = WLR_EDGE_BOTTOM;
      vRatio = ratio;
      vSpan = span;
    }
    if (!hActive && !vActive) {
      return nullptr;
    }
    return std::make_unique<DwindleResizeGrab>(
        this, view, hActive, hEdge, hRatio, hSpan, vActive, vEdge, vRatio, vSpan
    );
  }

} // namespace andromeda
