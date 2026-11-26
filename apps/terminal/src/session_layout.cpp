#include "session_layout.h"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace term {

  namespace {
    using Tree = andromeda::tiling::DwindleTree;

    // A shape over exactly these panes (keys index + 1) that builds a tree.
    bool validShape(const std::vector<Tree::ShapeNode>& shape, const std::vector<std::size_t>& panes) {
      std::vector<andromeda::tiling::TileKey> keys;
      for (const Tree::ShapeNode& node : shape) {
        if (node.type == Tree::Node::Leaf) keys.push_back(node.key);
      }
      std::vector<andromeda::tiling::TileKey> want;
      for (const std::size_t i : panes) want.push_back(i + 1);
      if (panes.size() > kMaxPanes) return false;
      std::ranges::sort(keys);
      std::ranges::sort(want);
      Tree tree;
      return keys == want && tree.setShape(shape);
    }
  } // namespace

  SavedWindow saveLayout(const SessionList& list, const std::function<std::string(std::uint64_t)>& cwdOf,
                         const std::function<const Tree*(std::uint64_t)>& treeOf) {
    SavedWindow saved;
    std::vector<std::uint64_t> ids;
    for (const SessionInfo& info : list.sessions()) {
      if (info.kind == SessionKind::Shell) {
        ids.push_back(info.id);
        saved.sessions.push_back({cwdOf(info.id), info.name});
      }
    }
    const auto indexOf = [&ids](std::uint64_t id) {
      return static_cast<std::size_t>(std::ranges::find(ids, id) - ids.begin());
    };
    for (const auto& split : list.splits()) {
      std::vector<std::size_t> panes;
      for (const std::uint64_t id : split) {
        if (const std::size_t i = indexOf(id); i < ids.size()) {
          panes.push_back(i);
        }
      }
      if (panes.size() < 2) {
        continue;
      }
      std::optional<std::vector<Tree::ShapeNode>> shape;
      if (const Tree* tree = treeOf(split.front()); tree != nullptr) {
        shape = tree->shape();
        for (Tree::ShapeNode& node : *shape) {
          if (node.type == Tree::Node::Leaf) node.key = indexOf(node.key) + 1;
        }
        if (!validShape(*shape, panes)) {
          shape.reset();
        }
      }
      saved.splits.push_back({std::move(panes), std::move(shape)});
    }
    // recent() starts with the shown session; an unsaved one gives way to the most recent saved one.
    for (const std::uint64_t id : list.recent()) {
      if (const std::size_t i = indexOf(id); i < ids.size()) {
        saved.shown = i;
        break;
      }
    }
    return saved;
  }

  std::vector<SessionListEvent> restoreEvents(const SavedWindow& saved, std::span<const std::uint64_t> ids) {
    std::vector<SessionListEvent> events;
    for (std::size_t i = 0; i < saved.sessions.size() && i < ids.size(); ++i) {
      if (!saved.sessions[i].name.empty()) {
        events.emplace_back(session_event::Rename{ids[i], saved.sessions[i].name});
      }
    }
    for (const SavedSplit& split : saved.splits) {
      for (std::size_t k = 1; k < split.panes.size(); ++k) {
        events.emplace_back(session_event::Join{.target = ids[split.panes[k - 1]], .joining = ids[split.panes[k]]});
      }
    }
    if (saved.shown < ids.size()) {
      events.emplace_back(session_event::Show{ids[saved.shown]});
    }
    return events;
  }

  std::vector<Tree> restoreTrees(const SavedWindow& saved, std::span<const std::uint64_t> ids) {
    std::vector<Tree> trees;
    for (const SavedSplit& split : saved.splits) {
      if (!split.shape || std::ranges::any_of(split.panes, [&ids](std::size_t i) { return i >= ids.size(); })) {
        continue;
      }
      std::vector<Tree::ShapeNode> shape = *split.shape;
      for (Tree::ShapeNode& node : shape) {
        if (node.type == Tree::Node::Leaf) node.key = ids[node.key - 1];
      }
      if (Tree tree; tree.setShape(shape)) {
        trees.push_back(std::move(tree));
      }
    }
    return trees;
  }

  nlohmann::json toJson(const std::vector<SavedWindow>& windows) {
    nlohmann::json out = nlohmann::json::array();
    for (const SavedWindow& window : windows) {
      nlohmann::json sessions = nlohmann::json::array();
      for (const SavedSession& session : window.sessions) {
        sessions.push_back({{"cwd", session.cwd}, {"name", session.name}});
      }
      nlohmann::json splits = nlohmann::json::array();
      nlohmann::json shapes = nlohmann::json::array();
      for (const SavedSplit& split : window.splits) {
        splits.push_back(split.panes);
        nlohmann::json shape = nullptr;
        for (const Tree::ShapeNode& node : split.shape.value_or(std::vector<Tree::ShapeNode>{})) {
          shape.push_back({static_cast<int>(node.type), node.ratio, node.locked, node.key});
        }
        shapes.push_back(std::move(shape));
      }
      out.push_back({{"sessions", std::move(sessions)}, {"splits", std::move(splits)}, {"shapes", std::move(shapes)}, {"shown", window.shown}});
      if (window.name) out.back()["name"] = *window.name;
    }
    return out;
  }

  std::vector<SavedWindow> windowsFromJson(const nlohmann::json& json) {
    std::vector<SavedWindow> out;
    if (!json.is_array()) {
      return out;
    }
    for (const nlohmann::json& w : json) {
      if (!w.is_object() || !w.contains("sessions") || !w["sessions"].is_array()) {
        continue;
      }
      SavedWindow window;
      for (const nlohmann::json& s : w["sessions"]) {
        if (s.is_object()) {
          window.sessions.push_back({s.value("cwd", std::string()), s.value("name", std::string())});
        }
      }
      if (window.sessions.empty()) {
        continue;
      }
      const std::size_t count = window.sessions.size();
      const nlohmann::json shapes = w.value("shapes", nlohmann::json::array());
      if (w.contains("splits") && w["splits"].is_array()) {
        for (std::size_t n = 0; n < w["splits"].size(); ++n) {
          const nlohmann::json& split = w["splits"][n];
          std::vector<std::size_t> panes;
          if (split.is_array()) {
            for (const nlohmann::json& i : split) {
              if (i.is_number_unsigned() && i.get<std::size_t>() < count) {
                panes.push_back(i.get<std::size_t>());
              }
            }
          }
          if (panes.size() < 2 || panes.size() != split.size()) {
            continue;
          }
          std::optional<std::vector<Tree::ShapeNode>> shape;
          if (shapes.is_array() && n < shapes.size() && shapes[n].is_array()) {
            shape.emplace();
            for (const nlohmann::json& node : shapes[n]) {
              if (!node.is_array() || node.size() != 4 || !node[0].is_number_unsigned() || !node[1].is_number() ||
                  !node[2].is_boolean() || !node[3].is_number_unsigned() || node[0].get<unsigned>() > Tree::Node::VSplit) {
                shape.reset();
                break;
              }
              shape->push_back({.type = static_cast<Tree::Node::Type>(node[0].get<unsigned>()), .ratio = node[1].get<double>(),
                               .locked = node[2].get<bool>(), .key = node[3].get<andromeda::tiling::TileKey>()});
            }
          }
          if (shape && !validShape(*shape, panes)) {
            shape.reset();
          }
          window.splits.push_back({std::move(panes), std::move(shape)});
        }
      }
      const nlohmann::json shown = w.value("shown", nlohmann::json());
      if (const nlohmann::json name = w.value("name", nlohmann::json()); name.is_string() && !name.get<std::string>().empty()) {
        window.name = name.get<std::string>();
      }
      window.shown = shown.is_number_unsigned() && shown.get<std::size_t>() < count ? shown.get<std::size_t>() : 0;
      out.push_back(std::move(window));
    }
    return out;
  }

  std::vector<ClosedItem> closedItems(const SessionList& list, std::span<const std::uint64_t> ids, std::uint32_t window,
                                      const std::optional<std::string>& windowName,
                                      const std::function<std::string(std::uint64_t)>& cwdOf,
                                      const std::function<const andromeda::tiling::DwindleTree*(std::uint64_t)>& treeOf) {
    std::vector<ClosedItem> items;
    const auto& sessions = list.sessions();
    const auto closing = [&ids](std::uint64_t id) { return std::ranges::find(ids, id) != ids.end(); };
    if (std::ranges::all_of(sessions, [&](const SessionInfo& s) { return closing(s.id); })) {
      if (SavedWindow saved = saveLayout(list, cwdOf, treeOf); !saved.sessions.empty()) {
        saved.name = windowName;
        items.emplace_back(std::move(saved));
      }
      return items;
    }
    using Tree = andromeda::tiling::DwindleTree;
    for (auto it = sessions.rbegin(); it != sessions.rend(); ++it) {
      const std::uint64_t id = it->id;
      if (!closing(id) || it->kind != SessionKind::Shell) {
        continue;
      }
      ClosedSession closed{.id = id, .window = window, .session = {cwdOf(id), it->name}};
      const auto next = std::find_if(it.base(), sessions.end(), [&](const SessionInfo& s) { return !closing(s.id); });
      closed.before = next != sessions.end() ? next->id : 0;
      // A pane beside it in the tree that is there when it comes back: still open, or closed with it and earlier in the
      // list (the stack brings that one back first).
      const auto place = [&sessions](std::uint64_t key) { return std::ranges::find(sessions, key, &SessionInfo::id) - sessions.begin(); };
      const auto back = [&](std::uint64_t key) { return !closing(key) || place(key) < place(id); };
      const Tree* tree = treeOf(id);
      const Tree::Node* leaf = tree != nullptr ? tree->find(id) : nullptr;
      // Up from its leaf: the first subtree beside it with such a pane; the side is taken of that subtree.
      for (const Tree::Node* child = leaf; child != nullptr && child->parent != nullptr && closed.splitWith == 0; child = child->parent) {
        const Tree::Node* parent = child->parent;
        const bool first = parent->left.get() == child;
        std::vector<const Tree::Node*> todo{first ? parent->right.get() : parent->left.get()};
        const Tree::Node* other = nullptr;
        while (!todo.empty() && other == nullptr) {
          const Tree::Node* node = todo.back();
          todo.pop_back();
          if (node == nullptr) continue;
          if (node->type == Tree::Node::Leaf) {
            if (back(node->key)) other = node;
            continue;
          }
          todo.push_back(node->right.get());
          todo.push_back(node->left.get());
        }
        if (other == nullptr) continue;
        const bool across = parent->type == Tree::Node::HSplit ||
                            (parent->type == Tree::Node::AutoSplit && parent->area.width >= parent->area.height);
        closed.splitWith = other->key;
        closed.edge = across ? (first ? andromeda::tiling::EdgeLeft : andromeda::tiling::EdgeRight)
                             : (first ? andromeda::tiling::EdgeTop : andromeda::tiling::EdgeBottom);
        closed.shape = tree->shape();
      }
      items.emplace_back(std::move(closed));
    }
    return items;
  }

  void renameClosed(ClosedItem& item, std::uint64_t from, std::uint64_t to) {
    auto* closed = std::get_if<ClosedSession>(&item);
    if (closed == nullptr) {
      return;
    }
    if (closed->before == from) closed->before = to;
    if (closed->splitWith == from) closed->splitWith = to;
    for (auto& node : closed->shape) {
      if (node.type == andromeda::tiling::DwindleTree::Node::Leaf && node.key == from) node.key = to;
    }
  }

  ReopenSteps reopenSteps(const SessionList& list, const ClosedSession& closed, std::uint64_t id) {
    ReopenSteps steps;
    if (!closed.session.name.empty()) {
      steps.events.emplace_back(session_event::Rename{id, closed.session.name});
    }
    if (closed.before != 0 && list.find(closed.before) != nullptr) {
      steps.events.emplace_back(session_event::Move{.id = id, .before = closed.before});
    }
    const auto* split = closed.splitWith != 0 ? list.splitOf(closed.splitWith) : nullptr;
    if (closed.splitWith == 0 || list.find(closed.splitWith) == nullptr || (split != nullptr && split->size() >= kMaxPanes)) {
      return steps;
    }
    ReopenJoin join{.target = closed.splitWith, .edge = closed.edge, .shape = closed.shape};
    for (auto& node : join.shape) {
      if (node.type == andromeda::tiling::DwindleTree::Node::Leaf && node.key == closed.id) node.key = id;
    }
    steps.events.emplace_back(session_event::Join{.target = closed.splitWith, .joining = id,
                                                  .before = closed.edge == andromeda::tiling::EdgeLeft || closed.edge == andromeda::tiling::EdgeTop});
    steps.join = std::move(join);
    return steps;
  }

} // namespace term
