#include "render/scene/node.h"

#include "render/scene/accessible.h"

#include "core/ui_phase.h"
#include "render/animation/animation_manager.h"
#include "render/core/mat3.h"

#include <algorithm>
#include <ranges>
#include <utility>

namespace {


  Mat3 computeWorldTransform(const Node* node) {
    Mat3 world = Mat3::identity();
    for (const Node* current = node; current != nullptr; current = current->parent()) {
      world = current->localTransform() * world;
    }
    return world;
  }

} // namespace

LayoutConstraints LayoutConstraints::unconstrained() noexcept { return {}; }

LayoutConstraints LayoutConstraints::available(float width, float height) noexcept {
  LayoutConstraints constraints;
  constraints.setMaxWidth(width);
  constraints.setMaxHeight(height);
  return constraints;
}

LayoutConstraints LayoutConstraints::exact(float width, float height) noexcept {
  LayoutConstraints constraints;
  constraints.setExactWidth(width);
  constraints.setExactHeight(height);
  return constraints;
}

void LayoutConstraints::setMaxWidth(float width) noexcept {
  maxWidth = std::max(0.0F, width);
  hasMaxWidth = true;
}

void LayoutConstraints::setMaxHeight(float height) noexcept {
  maxHeight = std::max(0.0F, height);
  hasMaxHeight = true;
}

void LayoutConstraints::setExactWidth(float width) noexcept {
  minWidth = std::max(0.0F, width);
  setMaxWidth(minWidth);
}

void LayoutConstraints::setExactHeight(float height) noexcept {
  minHeight = std::max(0.0F, height);
  setMaxHeight(minHeight);
}

bool LayoutConstraints::hasExactWidth() const noexcept { return hasMaxWidth && minWidth == maxWidth; }

bool LayoutConstraints::hasExactHeight() const noexcept { return hasMaxHeight && minHeight == maxHeight; }

float LayoutConstraints::constrainWidth(float width) const noexcept {
  float constrained = std::max(width, minWidth);
  if (hasMaxWidth) {
    constrained = std::min(constrained, maxWidth);
  }
  return constrained;
}

float LayoutConstraints::constrainHeight(float height) const noexcept {
  float constrained = std::max(height, minHeight);
  if (hasMaxHeight) {
    constrained = std::min(constrained, maxHeight);
  }
  return constrained;
}

LayoutSize LayoutConstraints::constrain(LayoutSize size) const noexcept {
  return LayoutSize{.width = constrainWidth(size.width), .height = constrainHeight(size.height)};
}

Node::Node(NodeType type) : m_type(type) {}

namespace {
  std::uint64_t nextAccessibleSerial() {
    static std::uint64_t serial = 0;
    return ++serial;
  }
} // namespace

void Node::setAccessible(const Accessible& accessible) {
  const std::uint64_t serial = m_accessible != nullptr ? m_accessible->serial : nextAccessibleSerial();
  m_accessible = std::make_unique<Accessible>(accessible);
  m_accessible->serial = serial;
}

Accessible& Node::accessibleState() {
  if (m_accessible == nullptr) {
    m_accessible = std::make_unique<Accessible>();
    m_accessible->serial = nextAccessibleSerial();
  }
  return *m_accessible;
}

Node::~Node() {
  // Ensure any animation still targeting this node is cancelled before the node goes away.
  // Call sites that pass `this` as the animation owner get lifetime safety for free.
  if (m_animationManager != nullptr) {
    m_animationManager->cancelForOwner(this);
  }
}

void Node::layout(Renderer& renderer) {
  uiAssertNotRendering("Node::layout");
  m_localLayoutDirty = false;
  doLayout(renderer);
}

LayoutSize Node::measure(Renderer& renderer, const LayoutConstraints& constraints) {
  uiAssertNotRendering("Node::measure");
  return doMeasure(renderer, constraints);
}

void Node::arrange(Renderer& renderer, const LayoutRect& rect) {
  uiAssertNotRendering("Node::arrange");
  m_arranging = true;
  m_sizeAssignedByLayout = true;
  doArrange(renderer, rect);
  m_arranging = false;
  m_sizeAssignedByLayout = true;
}

void Node::invalidateGpuResources(Renderer& renderer, std::uint64_t generation) {
  uiAssertNotRendering("Node::invalidateGpuResources");
  if (m_gpuResourceGeneration != generation) {
    doInvalidateGpuResources(renderer);
    m_gpuResourceGeneration = generation;
  }
  for (auto& child : m_children) {
    child->invalidateGpuResources(renderer, generation);
  }
}

void Node::rebindRenderer(Renderer& renderer) {
  doRebindRenderer(renderer);
  for (auto& child : m_children) {
    child->rebindRenderer(renderer);
  }
}

bool Node::containsScenePoint(float sceneX, float sceneY) const {
  float localX = 0.0F;
  float localY = 0.0F;
  return pointInsideNode(this, sceneX, sceneY, localX, localY, false);
}

void Node::doLayout(Renderer& renderer) {
  for (auto& child : m_children) {
    if (child->visible()) {
      child->layout(renderer);
    }
  }
}

LayoutSize Node::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  doLayout(renderer);
  return constraints.constrain(LayoutSize{.width = width(), .height = height()});
}

void Node::doArrange(Renderer& renderer, const LayoutRect& rect) {
  setPosition(rect.x, rect.y);
  setSize(rect.width, rect.height);
  doLayout(renderer);
}

void Node::doInvalidateGpuResources(Renderer& renderer) { (void)renderer; }

bool Node::containsLocalPoint(float localX, float localY, bool includeHitOutset) const {
  if (!includeHitOutset) {
    return localX >= 0.0F && localX < width() && localY >= 0.0F && localY < height();
  }
  const HitTestOutset outset = hitTestOutset();
  return localX >= -outset.left
      && localX < width() + outset.right
      && localY >= -outset.top
      && localY < height() + outset.bottom;
}

bool Node::pointInsideNode(
    const Node* node, float sceneX, float sceneY, float& localX, float& localY, bool includeHitOutset
) {
  if (node == nullptr) {
    return false;
  }

  const Mat3 inverse = computeWorldTransform(node).inverse();
  const Vec2 local = inverse.transformPoint(sceneX, sceneY);
  localX = local.x;
  localY = local.y;
  return node->containsLocalPoint(localX, localY, includeHitOutset);
}

Mat3 Node::localTransform() const {
  // Most nodes only translate: skip the rotation (sin, cos) and four matrix products. Exact: R(0) and S(1) are identity.
  if (m_rotation == 0.0F && m_scaleX == 1.0F && m_scaleY == 1.0F) {
    return Mat3::translation(m_x, m_y);
  }
  const float cx = transformOriginX();
  const float cy = transformOriginY();
  return Mat3::translation(m_x, m_y) * Mat3::translation(cx, cy) * Mat3::rotation(m_rotation)
      * Mat3::scale(m_scaleX, m_scaleY) * Mat3::translation(-cx, -cy);
}

void Node::setPosition(float x, float y) {
  if (m_x == x && m_y == y) {
    return;
  }
  m_x = x;
  m_y = y;
  markPaintDirty();
}

void Node::setSize(float width, float height) {
  if (!m_arranging) {
    m_sizeAssignedByLayout = false;
  }
  if (m_width == width && m_height == height) {
    return;
  }
  m_width = width;
  m_height = height;
  markLayoutDirty();
}

void Node::setFrameSize(float width, float height) {
  if (!m_arranging) {
    m_sizeAssignedByLayout = false;
  }
  if (m_width == width && m_height == height) {
    return;
  }
  m_width = width;
  m_height = height;
  markPaintDirty();
}

void Node::setRotation(float radians) {
  if (m_rotation == radians) {
    return;
  }
  m_rotation = radians;
  markPaintDirty();
}

void Node::setScale(float scale) { setScale(scale, scale); }

void Node::setScale(float scaleX, float scaleY) {
  if (m_scaleX == scaleX && m_scaleY == scaleY) {
    return;
  }
  m_scaleX = scaleX;
  m_scaleY = scaleY;
  markPaintDirty();
}

void Node::setTransformOrigin(float x, float y) {
  if (m_hasTransformOrigin && m_transformOriginX == x && m_transformOriginY == y) {
    return;
  }
  m_hasTransformOrigin = true;
  m_transformOriginX = x;
  m_transformOriginY = y;
  markPaintDirty();
}

void Node::setOpacity(float opacity) {
  if (m_opacity == opacity) {
    return;
  }
  m_opacity = opacity;
  markPaintDirty();
}

void Node::setFlexGrow(float grow) {
  if (m_flexGrow == grow) {
    return;
  }
  m_flexGrow = grow;
  markLayoutDirty();
}

void Node::setVisible(bool visible) {
  if (m_visible == visible) {
    return;
  }
  m_visible = visible;
  markLayoutDirty();
  markPaintDirty();
}

void Node::setParticipatesInLayout(bool participatesInLayout) {
  if (m_participatesInLayout == participatesInLayout) {
    return;
  }
  uiAssertSceneMutationAllowed("Node::setParticipatesInLayout");
  m_participatesInLayout = participatesInLayout;
  markLayoutDirty();
}

void Node::setClipChildren(bool clipChildren) {
  if (m_clip.has_value() == clipChildren) {
    return;
  }
  m_clip = clipChildren ? std::optional<Radii>(Radii{}) : std::nullopt;
  markPaintDirty();
}

void Node::setClipRadii(const Radii& radii) {
  if (m_clip == radii) {
    return;
  }
  m_clip = radii;
  markPaintDirty();
}

void Node::setPaintContained(bool paintContained) { m_paintContained = paintContained; }

void Node::setHitTestVisible(bool hitTestVisible) { m_hitTestVisible = hitTestVisible; }

void Node::setHitTestOutset(const HitTestOutset& outset) {
  const HitTestOutset clamped{
      .left = std::max(0.0F, outset.left),
      .top = std::max(0.0F, outset.top),
      .right = std::max(0.0F, outset.right),
      .bottom = std::max(0.0F, outset.bottom),
  };
  if (m_hitTestOutset.left == clamped.left
      && m_hitTestOutset.top == clamped.top
      && m_hitTestOutset.right == clamped.right
      && m_hitTestOutset.bottom == clamped.bottom) {
    return;
  }
  m_hitTestOutset = clamped;
}

void Node::setZIndex(std::int32_t zIndex) {
  if (m_zIndex == zIndex) {
    return;
  }
  m_zIndex = zIndex;
  markPaintDirty();
}

void Node::setExcludeSubtreeFromTabOrder(bool exclude) noexcept { m_excludeSubtreeFromTabOrder = exclude; }

void Node::setAnimationManager(AnimationManager* mgr) {
  m_animationManager = mgr;
  for (auto& child : m_children) {
    child->setAnimationManager(mgr);
  }
}

void Node::setPopupContext(SelectPopupContext* ctx) {
  m_popupContext = ctx;
  for (auto& child : m_children) {
    child->setPopupContext(ctx);
  }
}

void Node::setInvalidationCallback(std::function<void(NodeInvalidation)> callback) {
  m_invalidationCallback = std::move(callback);
}

void Node::attachChild(Node* child) {
  const PaletteTone detachedTone = child->paletteTone();
  child->m_parent = this;
  // A local layout marked while the subtree was detached must still reach the root's local pass.
  if (child->m_localLayoutInSubtree) {
    for (Node* node = this; node != nullptr && !node->m_localLayoutInSubtree; node = node->m_parent) {
      node->m_localLayoutInSubtree = true;
    }
  }
  if (m_animationManager != nullptr && child->m_animationManager == nullptr) {
    child->setAnimationManager(m_animationManager);
  }
  if (m_popupContext != nullptr && child->m_popupContext == nullptr) {
    child->setPopupContext(m_popupContext);
  }
  // Controls resolve colours before they are attached; a subtree that lands under another tone resolves again.
  if (child->paletteTone() != detachedTone) {
    child->notifyPaletteTone();
  }
}

void Node::setPaletteTone(std::optional<PaletteTone> tone) {
  if (m_paletteToneFade.has_value()) {
    if (m_animationManager != nullptr) {
      m_animationManager->cancel(m_paletteToneFade->id);
    }
    m_paletteToneFade.reset();
    if (m_paletteTone == tone) {
      notifyPaletteTone();
      return;
    }
  }
  if (m_paletteTone == tone) {
    return;
  }
  const PaletteTone before = paletteTone();
  m_paletteTone = tone;
  if (paletteTone() != before) {
    notifyPaletteTone();
  }
}

void Node::fadePaletteTone(PaletteTone tone, const desk::motion::SpringConfig& spring) {
  const PaletteTone before = paletteTone();
  if (m_animationManager == nullptr || (before == tone && !m_paletteToneFade.has_value())) {
    setPaletteTone(tone);
    return;
  }
  if (before == tone) {
    return;
  }
  // A reversal mid-fade runs back from where the colours are now.
  float start = 0.0F;
  if (m_paletteToneFade.has_value()) {
    m_animationManager->cancel(m_paletteToneFade->id);
    start = m_paletteToneFade->from == tone ? 1.0F - m_paletteToneFade->progress : 0.0F;
  }
  m_paletteTone = tone;
  m_paletteToneFade = PaletteToneFade{.from = before, .progress = start, .id = 0};
  // With motion off, drive() may complete at once and end the fade.
  const std::uint32_t id = m_animationManager->drive(
      0, start, 1.0F, spring,
      [this](float t) {
        m_paletteToneFade->progress = t;
        notifyPaletteTone();
        markPaintDirty();
      },
      [this] {
        m_paletteToneFade.reset();
        notifyPaletteTone();
        markPaintDirty();
      },
      this, 0.0F, Unit::Progress
  );
  if (m_paletteToneFade.has_value()) {
    m_paletteToneFade->id = id;
  }
  notifyPaletteTone();
  markPaintDirty();
}

std::optional<Node::PaletteToneFade> Node::paletteToneFade() const noexcept {
  for (const Node* node = this; node != nullptr; node = node->m_parent) {
    if (node->m_paletteTone.has_value()) {
      return node->m_paletteToneFade;
    }
  }
  return std::nullopt;
}

PaletteTone Node::paletteTone() const noexcept {
  for (const Node* node = this; node != nullptr; node = node->m_parent) {
    if (node->m_paletteTone.has_value()) {
      return *node->m_paletteTone;
    }
  }
  return PaletteTone::Scheme;
}

void Node::notifyPaletteTone() {
  onPaletteToneChanged();
  for (auto& child : m_children) {
    if (!child->m_paletteTone.has_value()) {
      child->notifyPaletteTone();
    }
  }
}

Node* Node::addChild(std::unique_ptr<Node> child) {
  uiAssertSceneMutationAllowed("Node::addChild");
  auto* raw = child.get();
  attachChild(raw);
  m_children.push_back(std::move(child));
  markLayoutDirty();
  raw->markPaintDirty();
  return raw;
}

Node* Node::insertChildAt(std::size_t index, std::unique_ptr<Node> child) {
  uiAssertSceneMutationAllowed("Node::insertChildAt");
  auto* raw = child.get();
  attachChild(raw);
  if (index >= m_children.size()) {
    m_children.push_back(std::move(child));
  } else {
    m_children.insert(m_children.begin() + static_cast<std::ptrdiff_t>(index), std::move(child));
  }
  markLayoutDirty();
  raw->markPaintDirty();
  return raw;
}

std::unique_ptr<Node> Node::removeChild(Node* child) {
  uiAssertSceneMutationAllowed("Node::removeChild");
  auto it = std::ranges::find_if(m_children, [child](const auto& ptr) { return ptr.get() == child; });

  if (it == m_children.end()) {
    return nullptr;
  }

  auto removed = std::move(*it);
  Node* root = this;
  while (root->m_parent != nullptr) {
    root = root->m_parent;
  }
  if (root->m_pendingDamage == nullptr) {
    root->m_pendingDamage = std::make_unique<DamageRegion>();
  }
  takePaintedBounds(removed.get(), *root->m_pendingDamage);
  m_children.erase(it);
  removed->m_parent = nullptr;
  markLayoutDirty();
  return removed;
}

void Node::markPaintDirty() {
  m_selfPaintDirty = true;
  propagatePaintDirty();
}

void Node::markPaintDirty(const DamageRect& local) {
  if (!m_selfPaintDirty) {
    if (!m_partialDamage) {
      m_partialDamage = std::make_unique<DamageRegion>();
    }
    m_partialDamage->add(local);
  }
  propagatePaintDirty();
}

void Node::takePaintedBounds(Node* subtree, DamageRegion& out) {
  if (!subtree->m_paintedSubtree) {
    return;
  }
  subtree->m_paintedSubtree = false;
  if (subtree->m_paintedBounds) {
    out.add(*subtree->m_paintedBounds);
    subtree->m_paintedBounds.reset();
  }
  subtree->m_paintedClip.reset();
  if (subtree->m_childArea) {
    out.add(*subtree->m_childArea);
    subtree->m_childArea.reset();
  }
  for (auto& child : subtree->m_children) {
    takePaintedBounds(child.get(), out);
  }
}

DamageRegion Node::takePendingDamage() {
  if (m_pendingDamage == nullptr) {
    return {};
  }
  DamageRegion damage = *m_pendingDamage;
  m_pendingDamage->clear();
  return damage;
}

void Node::markLayoutDirty() {
  propagateLayoutDirty();
  propagatePaintDirty();
}

void Node::markLocalLayoutDirty() {
  m_localLayoutDirty = true;
  for (Node* node = this; node != nullptr && !node->m_localLayoutInSubtree; node = node->m_parent) {
    node->m_localLayoutInSubtree = true;
  }
  propagatePaintDirty();
}

void Node::layoutLocalPending(Renderer& renderer) {
  if (!m_localLayoutInSubtree) {
    return;
  }
  m_localLayoutInSubtree = false;
  if (!visible()) {
    return;
  }
  if (m_localLayoutDirty) {
    layout(renderer);
  }
  for (auto& child : m_children) {
    child->layoutLocalPending(renderer);
  }
}

void Node::propagatePaintDirty() {
  if (m_paintDirty) {
    return;
  }
  m_paintDirty = true;
  if (m_parent != nullptr) {
    m_parent->propagatePaintDirty();
  } else {
    notifyInvalidated(NodeInvalidation::Paint);
  }
}

void Node::propagateLayoutDirty() {
  // A node outside its parent's layout cannot move its siblings: it lays out alone, not the whole tree.
  if (m_parent != nullptr && !m_participatesInLayout) {
    markLocalLayoutDirty();
    return;
  }
  if (m_layoutDirty) {
    return;
  }
  m_layoutDirty = true;
  if (m_parent != nullptr) {
    m_parent->propagateLayoutDirty();
  } else {
    notifyInvalidated(NodeInvalidation::Layout);
  }
}

void Node::notifyInvalidated(NodeInvalidation invalidation) {
  if (m_invalidationCallback) {
    m_invalidationCallback(invalidation);
  }
}

void Node::clearDirty() {
  m_selfPaintDirty = false;
  if (m_partialDamage) {
    m_partialDamage->clear();
  }
  m_paintDirty = false;
  m_layoutDirty = false;
  m_localLayoutDirty = false;
  m_localLayoutInSubtree = false;
  for (auto& child : m_children) {
    if (child->paintDirty() || child->layoutDirty()) {
      child->clearDirty();
    }
  }
}

Node* Node::hitTest(Node* root, float x, float y) { return hitTestImpl(root, x, y, true, Mat3::identity()); }

Node* Node::hitTestStrict(Node* root, float x, float y) { return hitTestImpl(root, x, y, false, Mat3::identity()); }

Node* Node::hitTestImpl(Node* node, float px, float py, bool allowOverflow, const Mat3& parentTransform) {
  if (node == nullptr || !node->m_visible || !node->m_hitTestVisible) {
    return nullptr;
  }

  const Mat3 worldTransform = parentTransform * node->localTransform();
  const Vec2 local = worldTransform.inverse().transformPoint(px, py);
  const bool inside = node->containsLocalPoint(local.x, local.y, true);

  if ((!allowOverflow || node->m_clip.has_value()) && !inside) {
    return nullptr;
  }

  // Fast path: children are already in zIndex order (the common case). Skip
  // allocating/sorting a side vector and iterate the child list directly in
  // reverse. Only fall back to the sorted copy when there's an out-of-order
  // pair, which removes a per-node heap allocation from every pointer event.
  auto& children = node->m_children;
  bool childrenSorted = true;
  for (std::size_t i = 1; i < children.size(); ++i) {
    if (children[i]->zIndex() < children[i - 1]->zIndex()) {
      childrenSorted = false;
      break;
    }
  }

  // Traverse children in reverse (topmost first).
  // Children are allowed to overflow parent bounds (needed for menus/popovers).
  if (childrenSorted) {
    for (const auto& child : std::views::reverse(children)) {
      auto* hit = hitTestImpl(child.get(), px, py, allowOverflow, worldTransform);
      if (hit != nullptr) {
        return hit;
      }
    }
  } else {
    std::vector<Node*> orderedChildren;
    orderedChildren.reserve(children.size());
    for (auto& child : children) {
      orderedChildren.push_back(child.get());
    }
    std::ranges::stable_sort(orderedChildren, [](const Node* a, const Node* b) { return a->zIndex() < b->zIndex(); });
    for (Node* child : std::views::reverse(orderedChildren)) {
      auto* hit = hitTestImpl(child, px, py, allowOverflow, worldTransform);
      if (hit != nullptr) {
        return hit;
      }
    }
  }

  return inside ? node : nullptr;
}

void Node::absolutePosition(const Node* node, float& outX, float& outY) {
  const Vec2 topLeft = computeWorldTransform(node).transformPoint(0.0F, 0.0F);
  outX = topLeft.x;
  outY = topLeft.y;
}

void Node::mapToScene(const Node* node, float localX, float localY, float& outSceneX, float& outSceneY) {
  const Vec2 scenePoint = computeWorldTransform(node).transformPoint(localX, localY);
  outSceneX = scenePoint.x;
  outSceneY = scenePoint.y;
}

bool Node::mapFromScene(const Node* node, float sceneX, float sceneY, float& outLocalX, float& outLocalY) {
  if (node == nullptr) {
    outLocalX = 0.0F;
    outLocalY = 0.0F;
    return false;
  }

  return pointInsideNode(node, sceneX, sceneY, outLocalX, outLocalY, false);
}

void Node::transformedBounds(
    const Node* node, const Mat3& world, float& outLeft, float& outTop, float& outRight, float& outBottom
) {
  // No rotation or skew: two corners give the box, the same values as the four-corner walk.
  if (world.m[1] == 0.0F && world.m[3] == 0.0F) {
    const float x1 = world.m[0] * node->width() + world.m[6];
    const float y1 = world.m[4] * node->height() + world.m[7];
    outLeft = std::min(world.m[6], x1);
    outRight = std::max(world.m[6], x1);
    outTop = std::min(world.m[7], y1);
    outBottom = std::max(world.m[7], y1);
    return;
  }
  const Vec2 corners[] = {
      world.transformPoint(0.0F, 0.0F),
      world.transformPoint(node->width(), 0.0F),
      world.transformPoint(node->width(), node->height()),
      world.transformPoint(0.0F, node->height()),
  };

  outLeft = corners[0].x;
  outTop = corners[0].y;
  outRight = outLeft;
  outBottom = outTop;

  for (const Vec2 corner : corners) {
    outLeft = std::min(outLeft, corner.x);
    outTop = std::min(outTop, corner.y);
    outRight = std::max(outRight, corner.x);
    outBottom = std::max(outBottom, corner.y);
  }
}

void Node::transformedBounds(const Node* node, float& outLeft, float& outTop, float& outRight, float& outBottom) {
  transformedBounds(node, computeWorldTransform(node), outLeft, outTop, outRight, outBottom);
}
