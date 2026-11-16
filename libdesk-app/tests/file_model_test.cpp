#include "core/deferred_call.h"
#include "core/files/directory_watcher.h"
#include "core/files/file_model.h"
#include "core/timer_manager.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <filesystem>
#include <fstream>
#include <poll.h>
#include <print>
#include <span>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {
  int g_failures = 0;

  void expect(bool ok, const std::string& what) {
    if (!ok) {
      std::println(stderr, "file_model_test: {}", what);
      ++g_failures;
    }
  }

  // The app loop in miniature: the DeferredCall wake fd, the watcher and the timers, until done() or 3 s.
  template <typename F> bool pump(DirectoryWatcher& watcher, F done) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!done()) {
      if (std::chrono::steady_clock::now() > until) {
        return false;
      }
      std::vector<pollfd> fds{{.fd = DeferredCall::wakeFd(), .events = POLLIN, .revents = 0}};
      const std::size_t start = watcher.addPollFds(fds);
      ::poll(fds.data(), fds.size(), std::max(watcher.pollTimeoutMs(), 10));
      DeferredCall::drainWakeFd();
      for (auto& fn : DeferredCall::takePending()) {
        fn();
      }
      watcher.dispatch(fds, start);
      TimerManager::instance().tick();
    }
    return true;
  }

  std::vector<std::string> names(const FileModelView& view) {
    std::vector<std::string> out;
    for (const auto& row : view.rows()) {
      out.push_back(row->name);
    }
    return out;
  }

  std::string joined(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& s : v) {
      out += (out.empty() ? "" : ",") + s;
    }
    return out;
  }

  void write(const fs::path& path, std::size_t bytes) { std::ofstream(path) << std::string(bytes, 'x'); }
} // namespace

int main() {
  char tmpl[] = "/tmp/desk-file-model-XXXXXX";
  const fs::path root = ::mkdtemp(tmpl);
  const fs::path dir = root / "dir";
  const fs::path other = root / "other";
  fs::create_directories(dir / "zdir");
  fs::create_directories(other);
  write(dir / "b.txt", 30);
  write(dir / "a10.txt", 10);
  write(dir / "a2.txt", 20);
  write(dir / ".hidden", 1);

  DirectoryWatcher watcher;
  auto model = FileModel::open(dir, watcher);
  expect(FileModel::open(dir / "." , watcher) == model, "one model per directory per process");
  expect(pump(watcher, [&] { return model->status() == FileModel::Status::Ready; }), "the listing finishes");

  FileModelView view(model);
  expect(joined(names(view)) == "zdir,a2.txt,a10.txt,b.txt", "folders first, then natural name order: " + joined(names(view)));
  view.setShowHidden(true);
  expect(names(view).size() == 5 && names(view)[1] == ".hidden", "show hidden adds the dot file: " + joined(names(view)));
  view.setShowHidden(false);
  view.setSort({.key = FileSortKey::Size, .descending = true});
  expect(joined(names(view)) == "zdir,b.txt,a2.txt,a10.txt", "size descending keeps folders first: " + joined(names(view)));
  view.setSort({});
  view.setFilter("A1");
  expect(joined(names(view)) == "a10.txt", "the filter is a case-insensitive substring: " + joined(names(view)));
  view.setFilter("");

  // Watch events arrive as one Updated batch per flush; `modified` names the entries that existed and changed.
  std::vector<FileModel::Change> changes;
  auto token = model->subscribe([&](const FileModel::Change& c) { changes.push_back(c); });
  const auto sizeIs = [&](const std::string& name, std::uintmax_t size) {
    return pump(watcher, [&] {
      const auto* entry = model->find(name);
      return entry != nullptr && entry->size == size;
    });
  };
  const auto gone = [&](const std::string& name) { return pump(watcher, [&] { return model->find(name) == nullptr; }); };

  write(dir / "new.txt", 5);
  expect(sizeIs("new.txt", 5), "a created file is inserted with its size");
  changes.clear();

  write(dir / "new.txt", 50);
  expect(sizeIs("new.txt", 50), "a rewritten file carries the new size");
  expect(std::ranges::any_of(changes, [](const auto& c) { return std::ranges::contains(c.modified, "new.txt"); }),
         "a rewritten file is reported as modified");

  fs::rename(dir / "new.txt", dir / "renamed.txt");
  expect(sizeIs("renamed.txt", 50) && model->find("new.txt") == nullptr, "a rename inside the folder moves the entry");

  fs::rename(dir / "renamed.txt", other / "renamed.txt");
  expect(gone("renamed.txt"), "a move out of the folder removes the entry");

  fs::rename(other / "renamed.txt", dir / "back.txt");
  expect(sizeIs("back.txt", 50), "a move into the folder inserts the entry");

  fs::remove(dir / "b.txt");
  expect(gone("b.txt"), "a deleted file is removed");
  expect(joined(names(view)) == "zdir,a2.txt,a10.txt,back.txt", "the view follows the model: " + joined(names(view)));

  fs::remove_all(dir);
  expect(pump(watcher, [&] { return model->status() == FileModel::Status::Missing; }), "a deleted folder reads as missing");
  expect(model->entries().empty(), "a missing folder has no entries");

  auto missing = FileModel::open(root / "nope", watcher);
  expect(pump(watcher, [&] { return missing->status() == FileModel::Status::Missing; }), "a folder that never existed is missing");

  // Over the inline-stat limit the stat runs after the names; a reload and a drop mid-stat must stay safe.
  const fs::path big = root / "big";
  fs::create_directories(big);
  for (int i = 0; i < 1500; ++i) {
    std::ofstream(big / ("f" + std::to_string(i)));
  }
  auto bigModel = FileModel::open(big, watcher);
  expect(pump(watcher, [&] { return bigModel->status() == FileModel::Status::Ready; }), "the big listing finishes");
  int resets = 0;
  auto resetToken = bigModel->subscribe([&](const FileModel::Change& c) { resets += c.type == FileModel::Change::Type::Reset ? 1 : 0; });
  bigModel->reload();
  expect(pump(watcher, [&] { return resets == 1; }), "the reload publishes its names");
  expect(bigModel->entries().size() == 1500, "the reload lists every file");
  resetToken.reset();
  bigModel.reset();
  auto again = FileModel::open(big, watcher);
  expect(pump(watcher, [&] { return again->status() == FileModel::Status::Ready && !again->statPending(); }),
         "a new model of the folder finishes after the old one was dropped mid-stat");
  expect(std::ranges::none_of(again->entries(), [](const auto& e) { return e->pendingStat; }), "every entry has its stat");
  again.reset();

  // A listing big enough to split across threads still comes in name order with every name field.
  {
    const fs::path many = root / "many";
    fs::create_directories(many);
    for (int i = 0; i < 9000; ++i) {
      std::ofstream(many / std::format("{}{}{}", i % 3 == 0 ? "Ä" : i % 3 == 1 ? "b" : "A", 9000 - i, i % 2 ? ".jpg" : ""));
    }
    auto manyModel = FileModel::open(many, watcher);
    expect(pump(watcher, [&] { return manyModel->status() == FileModel::Status::Ready; }), "the threaded listing finishes");
    const auto& e = manyModel->entries();
    expect(e.size() == 9000, "the threaded listing has every file");
    const FileSort byName{.foldersFirst = false};
    expect(std::ranges::is_sorted(e, [&](const auto& a, const auto& b) { return FileModelView::before(*a, *b, byName); }),
           "the threaded listing is in name order");
    expect(std::ranges::all_of(e, [](const auto& f) { return !f->collateKey.empty() && !f->contentType.empty(); }),
           "every entry of the threaded listing has its key and type");
    expect(std::ranges::count_if(e, [](const auto& f) { return f->contentType == "image/jpeg"; }) == 4500,
           "types are guessed from the names");
  }

  // Model-based: after any sequence of sort, filter, hidden, file and reload events, every callback (a rebuild or an
  // in-place stat patch) leaves the view equal to a fresh view of the same model and settings.
  {
    const fs::path mixed = root / "mixed";
    fs::create_directories(mixed);
    for (int i = 0; i < 3000; ++i) {
      if (i % 50 == 0) {
        fs::create_directories(mixed / ("d" + std::to_string(i)));
      } else {
        write(mixed / ((i % 7 == 0 ? ".h" : "f") + std::to_string(i) + (i % 3 == 0 ? ".txt" : ".png")), static_cast<std::size_t>(i % 13));
      }
    }
    auto mixedModel = FileModel::open(mixed, watcher);
    FileModelView live(mixedModel);
    FileSort sort;
    bool hidden = false;
    std::string filter;
    int patches = 0;
    int checks = 0;
    const auto check = [&](const char* what) {
      FileModelView fresh(mixedModel);
      fresh.setSort(sort);
      fresh.setShowHidden(hidden);
      fresh.setFilter(filter);
      ++checks;
      if (fresh.rows() != live.rows()) {
        expect(false, std::format("{}: view differs from a fresh view ({} vs {} rows)", what, live.rows().size(),
                                  fresh.rows().size()));
      }
    };
    live.setChangedCallback([&] { check("rebuild"); });
    live.setRowsPatchedCallback([&](std::span<const std::size_t> rows) {
      ++patches;
      for (const std::size_t r : rows) {
        expect(r < live.rows().size(), "a patched row index is in range");
      }
      check("patch");
    });
    expect(pump(watcher, [&] { return mixedModel->status() == FileModel::Status::Ready && !mixedModel->statPending(); }),
           "the mixed folder lists");
    mixedModel->reload();
    expect(pump(watcher, [&] { return mixedModel->status() == FileModel::Status::Ready && !mixedModel->statPending(); }),
           "the mixed folder lists again");
    expect(patches > 0, "stat batches under a name sort patch rows in place");
    std::srand(7);
    const FileSortKey keys[] = {FileSortKey::Name, FileSortKey::Size, FileSortKey::Modified, FileSortKey::Kind};
    const char* filters[] = {"", "1", "PNG", "d"};
    int created = 0;
    for (int step = 0; step < 150; ++step) {
      switch (std::rand() % 8) { // 5-7 reload: each reload restarts the stat batches
      case 0:
        sort = {.key = keys[std::rand() % 4], .descending = std::rand() % 2 == 0, .foldersFirst = std::rand() % 2 == 0};
        live.setSort(sort);
        break;
      case 1:
        filter = filters[std::rand() % 4];
        live.setFilter(filter);
        break;
      case 2:
        hidden = !hidden;
        live.setShowHidden(hidden);
        break;
      case 3:
        write(mixed / ("n" + std::to_string(created++)), static_cast<std::size_t>(std::rand() % 40));
        break;
      case 4:
        fs::remove(mixed / ("f" + std::to_string(1 + std::rand() % 2999) + ".png"));
        break;
      default:
        mixedModel->reload();
        break;
      }
      if (std::rand() % 2 == 0) {
        pump(watcher, [&] { return mixedModel->status() == FileModel::Status::Ready && !mixedModel->statPending(); });
      }
    }
    expect(pump(watcher, [&] { return mixedModel->status() == FileModel::Status::Ready && !mixedModel->statPending(); }),
           "the mixed folder settles");
    check("settled");
    std::println("file_model_test: {} checks, {} patches", checks, patches);
  }

  fs::remove_all(root);
  return g_failures == 0 ? 0 : 1;
}
