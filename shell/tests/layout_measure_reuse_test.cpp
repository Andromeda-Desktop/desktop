// A layout that reuses the measures of its pass places every node where a layout without reuse does, over random
// trees (separators follow their parent's direction), random edits between passes and edits made during the pass:
// inside the node being laid out (as a Button narrows its label on arrange), to another label's text and line height,
// to a box's direction, and to a node's size.
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "tests/test_check.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/separator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

std::unique_ptr<TextureManager> createDefaultTextureManager() { return nullptr; }

namespace {

  // Text is 0.5 em per byte wide and wraps at the max width into as many lines as it needs (up to maxLines).
  class StubRenderer : public Renderer {
  public:
    TextMetrics measureText(
        std::string_view text, float fontSize, const TextFont&, float maxWidth, int maxLines, TextAlign, TextEllipsize,
        bool
    ) override {
      const float full = static_cast<float>(text.size()) * fontSize * 0.5F;
      float width = full;
      float lines = 1.0F;
      if (maxWidth > 0.0F && full > maxWidth) {
        width = maxWidth;
        lines = std::ceil(full / maxWidth);
        if (maxLines > 0) {
          lines = std::min(lines, static_cast<float>(maxLines));
        }
      }
      const float bottom = fontSize * (lines - 1.0F) + 2.0F;
      return TextMetrics{
          .width = width, .right = width, .top = -fontSize, .bottom = bottom, .inkTop = -fontSize, .inkBottom = bottom,
          .lineCount = static_cast<int>(lines)
      };
    }
    TextMetrics measureFont(float fontSize, const TextFont&) override {
      return TextMetrics{.top = -fontSize, .bottom = 2.0F};
    }
    void measureTextCursorStops(
        std::string_view, float, const std::vector<std::size_t>&, std::vector<float>&, const TextFont&
    ) override {}
    void measureTextCursorStopsWrapped(
        std::string_view, float, const std::vector<std::size_t>&, float, std::vector<TextCursorStop>&, const TextFont&
    ) override {}
    TextMetrics measureGlyph(char32_t, float fontSize) override { return TextMetrics{.width = fontSize, .bottom = fontSize}; }
    TextureManager& textureManager() override { std::abort(); }
    [[nodiscard]] float renderScale() const noexcept override { return 1.0F; }
  };

  struct Tree;

  // A row with one label. Measure lets the label take any width; arrange narrows it to half the row, as Button does.
  // Arrange also edits a label and resizes a plain node elsewhere in the tree, by the step.
  class Mutator final : public Flex {
  public:
    Mutator(Tree& tree, std::size_t target);

  protected:
    LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
    void doArrange(Renderer& renderer, const LayoutRect& rect) override;

  private:
    Tree& m_tree;
    std::size_t m_target;
    Label* m_own;
  };

  struct Tree {
    int step = 0;
    long plainMeasures = 0;
    std::unique_ptr<Flex> root;
    std::vector<Label*> labels;
    std::vector<Flex*> boxes;
    std::vector<Node*> plains;
    std::vector<Node*> all;
  };

  class Plain final : public Node {
  public:
    explicit Plain(Tree& tree) : m_tree(tree) {}

  protected:
    LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override {
      ++m_tree.plainMeasures;
      return Node::doMeasure(renderer, constraints);
    }

  private:
    Tree& m_tree;
  };

  Mutator::Mutator(Tree& tree, std::size_t target) : m_tree(tree), m_target(target) {
    auto own = std::make_unique<Label>();
    own->setText(std::string(10 + target % 30, 'w'));
    own->setFontSize(12.0F);
    m_own = own.get();
    addChild(std::move(own));
  }

  LayoutSize Mutator::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
    m_own->setMaxWidth(0.0F);
    return Flex::doMeasure(renderer, constraints);
  }

  void Mutator::doArrange(Renderer& renderer, const LayoutRect& rect) {
    m_own->setMaxWidth(std::max(10.0F, rect.width * 0.5F));
    const int phase = (m_tree.step + static_cast<int>(m_target % 4)) % 4;
    if (!m_tree.labels.empty()) {
      m_tree.labels[m_target % m_tree.labels.size()]->setText(std::string(static_cast<std::size_t>(3 + phase * 9), 'm'));
      m_tree.labels[(m_target / 3) % m_tree.labels.size()]->setLineHeight(static_cast<float>(phase * 7));
    }
    m_tree.boxes[(m_target / 5) % m_tree.boxes.size()]->setDirection(
        phase % 2 == 0 ? FlexDirection::Horizontal : FlexDirection::Vertical
    );
    if (!m_tree.plains.empty()) {
      const auto side = static_cast<float>(4 + phase * 11);
      m_tree.plains[m_target % m_tree.plains.size()]->setSize(side, side);
    }
    Flex::doArrange(renderer, rect);
  }

  std::string text(std::mt19937& rng) { return std::string(1 + rng() % 40, 'x'); }

  void grow(Tree& tree, Flex& parent, int depth, std::mt19937& rng) {
    const int children = 1 + static_cast<int>(rng() % 4);
    for (int i = 0; i < children; ++i) {
      const unsigned kind = depth >= 4 ? rng() % 3 : rng() % 6;
      if (kind == 0) {
        auto label = std::make_unique<Label>();
        label->setText(text(rng));
        label->setFontSize(static_cast<float>(10 + rng() % 8));
        if (rng() % 2 == 0) {
          label->setMaxWidth(static_cast<float>(40 + rng() % 200));
        }
        label->setMaxLines(static_cast<int>(rng() % 3));
        label->setFlexGrow(rng() % 4 == 0 ? 1.0F : 0.0F);
        tree.labels.push_back(label.get());
        tree.all.push_back(parent.addChild(std::move(label)));
      } else if (kind == 1) {
        auto mutator = std::make_unique<Mutator>(tree, rng());
        tree.all.push_back(parent.addChild(std::move(mutator)));
      } else if (kind == 2) {
        auto plain = std::make_unique<Plain>(tree);
        plain->setSize(static_cast<float>(rng() % 30), static_cast<float>(rng() % 30));
        tree.plains.push_back(plain.get());
        tree.all.push_back(parent.addChild(std::move(plain)));
      } else if (kind == 3) {
        auto separator = std::make_unique<Separator>();
        separator->setThickness(static_cast<float>(1 + rng() % 3));
        separator->setSpacing(static_cast<float>(rng() % 9));
        tree.all.push_back(parent.addChild(std::move(separator)));
      } else {
        auto box = std::make_unique<Flex>();
        box->setDirection(rng() % 2 == 0 ? FlexDirection::Horizontal : FlexDirection::Vertical);
        box->setPadding(static_cast<float>(rng() % 9));
        box->setGap(static_cast<float>(rng() % 7));
        box->setAlign(static_cast<FlexAlign>(rng() % 3));
        box->setJustify(static_cast<FlexJustify>(rng() % 3));
        box->setWrap(rng() % 5 == 0);
        box->setFlexGrow(rng() % 3 == 0 ? 1.0F : 0.0F);
        box->setMinWidth(static_cast<float>(rng() % 60));
        box->setMinHeight(static_cast<float>(rng() % 30));
        if (rng() % 4 == 0) {
          box->setFillWidth(true);
        }
        Flex* raw = box.get();
        tree.boxes.push_back(raw);
        tree.all.push_back(parent.addChild(std::move(box)));
        grow(tree, *raw, depth + 1, rng);
      }
    }
  }

  Tree build(unsigned seed) {
    std::mt19937 rng(seed);
    Tree tree;
    tree.root = std::make_unique<Flex>();
    tree.root->setDirection(FlexDirection::Vertical);
    tree.boxes.push_back(tree.root.get());
    tree.all.push_back(tree.root.get());
    grow(tree, *tree.root, 0, rng);
    return tree;
  }

  // The same edit on both trees: the trees have the same shape, so indices match.
  void edit(Tree& tree, std::mt19937 rng) {
    switch (rng() % 5) {
    case 0:
      if (!tree.labels.empty()) {
        tree.labels[rng() % tree.labels.size()]->setText(text(rng));
      }
      break;
    case 1:
      tree.boxes[rng() % tree.boxes.size()]->setMinWidth(static_cast<float>(rng() % 120));
      break;
    case 2: {
      Node* node = tree.all[1 + rng() % (tree.all.size() - 1)];
      node->setVisible(!node->visible());
      break;
    }
    case 3: {
      Node* node = tree.all[1 + rng() % (tree.all.size() - 1)];
      node->setFlexGrow(node->flexGrow() > 0.0F ? 0.0F : 1.0F);
      break;
    }
    default:
      tree.boxes[rng() % tree.boxes.size()]->setGap(static_cast<float>(rng() % 12));
      break;
    }
  }

  bool samePlaces(const Tree& a, const Tree& b, unsigned seed, int step) {
    for (std::size_t i = 0; i < a.all.size(); ++i) {
      const Node& x = *a.all[i];
      const Node& y = *b.all[i];
      if (x.x() != y.x() || x.y() != y.y() || x.width() != y.width() || x.height() != y.height()) {
        std::fprintf(
            stderr, "seed %u step %d node %zu: reuse %.2f,%.2f %.2fx%.2f, fresh %.2f,%.2f %.2fx%.2f\n", seed, step, i,
            x.x(), x.y(), x.width(), x.height(), y.x(), y.y(), y.width(), y.height()
        );
        return false;
      }
    }
    return true;
  }

  // Measures its child twice, changes one measure input of the child, and measures it again in the same pass. The
  // first measure can size the child, so only the second one leaves a result to reuse.
  class Probe final : public Node {
  public:
    Probe(std::unique_ptr<Node> child, std::function<void(Node&)> change) : m_change(std::move(change)) {
      m_child = child.get();
      addChild(std::move(child));
    }
    LayoutSize changed{};

  protected:
    LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override {
      (void)m_child->measure(renderer, constraints);
      (void)m_child->measure(renderer, constraints);
      m_change(*m_child);
      changed = m_child->measure(renderer, constraints);
      return changed;
    }

  private:
    Node* m_child;
    std::function<void(Node&)> m_change;
  };

  bool sameAfterChange(
      Renderer& renderer, const std::function<std::unique_ptr<Node>()>& make, const std::function<void(Node&)>& change
  ) {
    LayoutSize sizes[2];
    for (const bool reuse : {true, false}) {
      Node::setMeasureReuse(reuse);
      Probe probe(make(), change);
      (void)probe.measure(renderer, LayoutConstraints{});
      sizes[reuse ? 0 : 1] = probe.changed;
    }
    Node::setMeasureReuse(true);
    return sizes[0].width == sizes[1].width && sizes[0].height == sizes[1].height;
  }

} // namespace

int main() {
  StubRenderer renderer;
  long reusedMeasures = 0;
  long freshMeasures = 0;
  for (unsigned seed = 1; seed <= 300; ++seed) {
    Tree reuse = build(seed);
    Tree fresh = build(seed);
    std::mt19937 steps(seed * 7919U);
    for (int step = 0; step < 12; ++step) {
      const float width = static_cast<float>(120 + steps() % 900);
      const float height = static_cast<float>(80 + steps() % 700);
      if (step > 0) {
        const auto editSeed = static_cast<unsigned>(steps());
        edit(reuse, std::mt19937(editSeed));
        edit(fresh, std::mt19937(editSeed));
      }
      reuse.step = step;
      fresh.step = step;
      reuse.root->setSize(width, height);
      fresh.root->setSize(width, height);
      Node::setMeasureReuse(true);
      reuse.root->layout(renderer);
      Node::setMeasureReuse(false);
      fresh.root->layout(renderer);
      if (!samePlaces(reuse, fresh, seed, step)) {
        TEST_CHECK(false);
        return EXIT_FAILURE;
      }
    }
    reusedMeasures += reuse.plainMeasures;
    freshMeasures += fresh.plainMeasures;
  }
  TEST_CHECK(reusedMeasures < freshMeasures);
  // A separator follows its parent's direction.
  TEST_CHECK(sameAfterChange(
      renderer,
      [] {
        auto box = std::make_unique<Flex>();
        box->setDirection(FlexDirection::Vertical);
        auto separator = std::make_unique<Separator>();
        separator->setThickness(3.0F);
        separator->setSpacing(4.0F);
        box->addChild(std::move(separator));
        return std::unique_ptr<Node>(std::move(box));
      },
      [](Node& box) { static_cast<Flex&>(box).setDirection(FlexDirection::Horizontal); }
  ));
  TEST_CHECK(sameAfterChange(
      renderer,
      [] {
        auto label = std::make_unique<Label>();
        label->setText("line");
        label->setFontSize(12.0F);
        return std::unique_ptr<Node>(std::move(label));
      },
      [](Node& label) { static_cast<Label&>(label).setLineHeight(30.0F); }
  ));
  Node::setMeasureReuse(true);
  return EXIT_SUCCESS;
}
