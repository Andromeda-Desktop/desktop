#include "browse_model.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace term::browse;
namespace fs = std::filesystem;

namespace {

int failures = 0;
unsigned seed = 0;
int step = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s (seed %u step %d)\n", __FILE__, __LINE__, #c, seed, step); ++failures; } } while (0)

// A small tree; folders hold folders and files, some hidden.
std::map<fs::path, std::vector<Row>> tree() {
  return {
      {"/", {{"home", true}, {"etc", true}, {".hidden", false}}},
      {"/home", {{"me", true}, {".cache", true}, {"notes.txt", false}}},
      {"/home/me", {{"a.txt", false}, {"b.png", false}, {"src", true}, {"é ü", false}, {".rc", false}}},
      {"/home/me/src", {}},
      {"/home/.cache", {{"x", false}}},
      {"/etc", {{"hosts", false}}},
  };
}

// The owner side: lists folders (late, out of order), keeps the filter and hidden flag the effects ask for.
struct Owner {
  std::map<fs::path, std::vector<Row>> fs = tree();
  std::string filter;
  bool hidden = false;
  std::deque<fs::path> pending; // listings not yet delivered
  bool closed = false;

  std::vector<Row> rowsOf(const fs::path& dir) const {
    std::vector<Row> out;
    const auto it = fs.find(dir);
    if (it == fs.end()) {
      return out;
    }
    for (const Row& row : it->second) {
      if ((hidden || row.name[0] != '.') && row.name.find(filter) != std::string::npos) {
        out.push_back(row);
      }
    }
    return out;
  }
};

const Key kKeys[] = {
    {0xff54, 0, 0}, {0xff52, 0, 0}, {0xff51, 0, 0}, {0xff53, 0, 0}, {0xff0d, 0, 0}, {0xff0d, 0, 2}, {0xff1b, 0, 0},
    {0xff08, 0, 0}, {0xff50, 0, 0}, {0xff57, 0, 0}, {0xff55, 0, 0}, {0xff56, 0, 0}, {'j', 'j', 0},  {'k', 'k', 0},
    {'h', 'h', 0},  {'l', 'l', 0},  {' ', ' ', 0},  {'/', '/', 0},  {'.', '.', 0},  {'o', 'o', 0},  {'i', 'i', 0},
    {'y', 'y', 0},  {'x', 'x', 0},  {'p', 'p', 0},  {'d', 'd', 0},  {'r', 'r', 0},  {'q', 'q', 0},  {'t', 't', 0},
    {'e', 'e', 0},  {0xe9, 0xe9, 0}, {'c', 'c', 2},  {'z', 'z', 0},  {'R', 'R', 1},  {'m', 'm', 0},
};

bool validName(const std::string& name) {
  return !name.empty() && name != "." && name != ".." && name.find('/') == std::string::npos;
}

void run(unsigned s) {
  seed = s;
  std::mt19937 rng(s);
  Owner owner;
  Model model("/home/me");
  const std::vector<fs::path> folders{"/home/me/src", "/etc", "/home", "/gone"};
  model.setFolders(folders);
  const auto apply = [&](std::vector<Effect> effects, bool idle) {
    for (std::size_t i = 0; i < effects.size(); ++i) {
      CHECK(!owner.closed);
      const Effect& e = effects[i];
      if (const auto* list = std::get_if<effect::List>(&e)) {
        owner.pending.push_back(list->dir);
      } else if (const auto* f = std::get_if<effect::Filter>(&e)) {
        owner.filter = f->text;
        owner.pending.push_back(model.dir());
      } else if (const auto* h = std::get_if<effect::ShowHidden>(&e)) {
        owner.hidden = h->show;
        owner.pending.push_back(model.dir());
      } else if (std::holds_alternative<effect::Cd>(e) || std::holds_alternative<effect::Insert>(e)) {
        // Text reaches the shell only as the mode closes, and a cd only at an idle prompt.
        CHECK(i + 1 < effects.size() && std::holds_alternative<effect::Close>(effects[i + 1]));
        CHECK(!std::holds_alternative<effect::Cd>(e) || idle);
      } else if (const auto* r = std::get_if<effect::Rename>(&e)) {
        CHECK(validName(r->name));
        CHECK(r->path.parent_path() == model.dir());
      } else if (const auto* many = std::get_if<effect::RenameMany>(&e)) {
        CHECK(!many->items.empty());
        for (const auto& [path, name] : many->items) {
          CHECK(validName(name));
          CHECK(path.parent_path() == model.dir());
        }
      } else if (std::holds_alternative<effect::Close>(e)) {
        owner.closed = true;
      }
    }
  };
  apply(model.start(), true);
  for (step = 0; step < 400 && !owner.closed; ++step) {
    const fs::path before = model.dir();
    if (!owner.pending.empty() && rng() % 3 != 0) {
      // Deliver one listing, sometimes not the oldest (out of order).
      const std::size_t at = rng() % owner.pending.size();
      const fs::path dir = owner.pending[at];
      owner.pending.erase(owner.pending.begin() + static_cast<std::ptrdiff_t>(at));
      const std::vector<Row> old = model.rows();
      model.listed(dir, owner.rowsOf(dir));
      if (dir != model.dir()) {
        CHECK(model.rows().size() == old.size()); // a late listing for another folder changes nothing
      }
    } else if (std::holds_alternative<BulkRename>(model.input()) && rng() % 2 == 0) {
      // The editor submits: names unchanged, one renamed, one line lost, an empty line or a slash.
      const auto names = std::get<BulkRename>(model.input()).names;
      std::string text;
      const auto how = static_cast<unsigned>(rng() % 5);
      for (std::size_t i = 0; i < names.size(); ++i) {
        if (how == 2 && i == 0) {
          continue;
        }
        std::string name = names[i];
        if (i == 0 && how == 1) {
          name = "renamed-" + name;
        } else if (i == 0 && how == 3) {
          name.clear();
        } else if (i == 0 && how == 4) {
          name += "/x";
        }
        text += name + "\n";
      }
      if (rng() % 3 == 0) {
        model.cancelBulk();
      } else {
        apply(model.bulkRenamed(text), true);
      }
    } else {
      const Key key = kKeys[rng() % std::size(kKeys)];
      const bool idle = rng() % 4 != 0;
      apply(model.key(key, idle), idle);
    }
    // Invariants in every reachable state.
    if (model.rows().empty()) {
      CHECK(model.cursor() == 0);
    } else {
      CHECK(model.cursor() < model.rows().size());
    }
    for (const std::string& mark : model.marks()) {
      CHECK(std::ranges::find(model.rows(), mark, &Row::name) != model.rows().end());
    }
    CHECK(model.filter() == owner.filter);
    CHECK(model.showHidden() == owner.hidden);
    if (model.dir() != before) {
      // A folder change comes from h, l or a jump to a visited folder.
      CHECK(model.dir() == before.parent_path() || model.dir().parent_path() == before ||
            std::ranges::contains(folders, model.dir()));
      CHECK(model.marks().empty());
      CHECK(model.filter().empty());
      CHECK(!model.loaded());
    }
    if (model.loaded() && owner.pending.empty()) {
      CHECK(model.rows().size() == owner.rowsOf(model.dir()).size());
    }
  }
}

// Going up puts the cursor on the folder just left; a cut is put once.
void examples() {
  seed = 0;
  step = 0;
  Owner owner;
  Model model("/home/me/src");
  model.listed("/home/me/src", {});
  auto out = model.key({0xff51, 0, 0}, true);
  CHECK(model.dir() == "/home/me");
  model.listed("/home/me", owner.rowsOf("/home/me"));
  CHECK(model.current() != nullptr && model.current()->name == "src");

  (void)model.key({'x', 'x', 0}, true);
  out = model.key({0xff51, 0, 0}, true);
  model.listed("/home", owner.rowsOf("/home"));
  out = model.key({'p', 'p', 0}, true);
  CHECK(out.size() == 1 && std::holds_alternative<effect::Move>(out[0]));
  CHECK(std::get<effect::Move>(out[0]).paths == std::vector<fs::path>{"/home/me/src"});
  CHECK(std::get<effect::Move>(out[0]).dest == "/home");
  out = model.key({'p', 'p', 0}, true);
  CHECK(out.size() == 1 && std::holds_alternative<effect::Hint>(out[0]));

  out = model.key({'q', 'q', 0}, false);
  CHECK(out.size() == 1 && std::holds_alternative<effect::Hint>(out[0]));
  out = model.key({'q', 'q', 0}, true);
  CHECK(out.size() == 2 && std::get<effect::Cd>(out[0]).dir == "/home");

  // A jump ranks a folder whose own name matches above letters spread over a path.
  model.setFolders({"/data/old/src", "/srv/docs"});
  (void)model.key({'z', 'z', 0}, true);
  (void)model.key({'d', 'd', 0}, true);
  (void)model.key({'o', 'o', 0}, true);
  const auto jump = model.jumpMatches();
  CHECK(jump.size() == 2 && jump.front() == "/srv/docs");
  (void)model.key({0xff0d, 0, 0}, true);
  CHECK(model.dir() == "/srv/docs");
}

} // namespace

int main() {
  examples();
  for (unsigned s = 1; s <= 300; ++s) {
    run(s);
  }
  if (failures == 0) {
    std::puts("browse_model_test: ok");
  }
  return failures == 0 ? 0 : 1;
}
