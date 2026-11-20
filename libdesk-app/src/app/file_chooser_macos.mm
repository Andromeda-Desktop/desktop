#include "app/file_chooser.h"

#import <AppKit/AppKit.h>

#include <cstdlib>
#include <string_view>

namespace desk {

  namespace {
    NSString* toNs(const std::string& s) {
      NSString* out = [NSString stringWithUTF8String:s.c_str()];
      return out != nil ? out : @"";
    }

    // parentWindow "cocoa:<windowNumber>" makes the panel a sheet on that window.
    NSWindow* parentOf(const std::string& parentWindow) {
      constexpr std::string_view kPrefix = "cocoa:";
      if (!parentWindow.starts_with(kPrefix)) {
        return nil;
      }
      return [NSApp windowWithWindowNumber:std::atol(parentWindow.c_str() + kPrefix.size())];
    }

    std::function<void(std::vector<std::filesystem::path>)>
    takeFront(std::function<void(std::optional<std::filesystem::path>)> picked) {
      return [picked = std::move(picked)](std::vector<std::filesystem::path> paths) {
        picked(paths.empty() ? std::nullopt : std::optional(std::move(paths.front())));
      };
    }
  } // namespace

  struct FileChooser::Impl {
    std::uint64_t seq = 0;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    // AppKit runs the completion handler on the main thread, where the app loop runs.
    void run(NSSavePanel* panel, const std::string& parentWindow, const std::string& title,
             std::function<void(std::vector<std::filesystem::path>)> picked) {
      const std::uint64_t mine = ++seq;
      panel.message = toNs(title);
      panel.title = toNs(title);
      std::weak_ptr<bool> weak = alive;
      void (^done)(NSModalResponse) = ^(NSModalResponse response) {
        if (weak.expired() || mine != seq) {
          return;
        }
        std::vector<std::filesystem::path> paths;
        if (response == NSModalResponseOK) {
          if ([panel isKindOfClass:[NSOpenPanel class]]) {
            for (NSURL* url in static_cast<NSOpenPanel*>(panel).URLs) {
              paths.emplace_back(url.fileSystemRepresentation);
            }
          } else if (panel.URL != nil) {
            paths.emplace_back(panel.URL.fileSystemRepresentation);
          }
        }
        picked(std::move(paths));
      };
      if (NSWindow* parent = parentOf(parentWindow)) {
        [panel beginSheetModalForWindow:parent completionHandler:done];
      } else {
        [panel beginWithCompletionHandler:done];
      }
    }

    static NSOpenPanel* openPanel(bool directory, bool multiple) {
      NSOpenPanel* panel = [NSOpenPanel openPanel];
      panel.canChooseDirectories = directory;
      panel.canChooseFiles = !directory;
      panel.allowsMultipleSelection = multiple;
      panel.canCreateDirectories = directory;
      return panel;
    }
  };

  FileChooser::FileChooser(SessionBus* /*bus*/) : m_impl(std::make_unique<Impl>()) {}

  FileChooser::~FileChooser() = default;

  void FileChooser::chooseFolder(const std::string& parentWindow, const std::string& title,
                                 std::function<void(std::optional<std::filesystem::path>)> picked) {
    m_impl->run(Impl::openPanel(true, false), parentWindow, title, takeFront(std::move(picked)));
  }

  void FileChooser::openFile(const std::string& parentWindow, const std::string& title,
                             std::function<void(std::optional<std::filesystem::path>)> picked) {
    m_impl->run(Impl::openPanel(false, false), parentWindow, title, takeFront(std::move(picked)));
  }

  void FileChooser::openFiles(const std::string& parentWindow, const std::string& title,
                              std::function<void(std::vector<std::filesystem::path>)> picked) {
    m_impl->run(Impl::openPanel(false, true), parentWindow, title, std::move(picked));
  }

  void FileChooser::saveFile(const std::string& parentWindow, const std::string& title, const std::string& currentName,
                             std::function<void(std::optional<std::filesystem::path>)> picked) {
    NSSavePanel* panel = [NSSavePanel savePanel];
    panel.nameFieldStringValue = toNs(currentName);
    panel.canCreateDirectories = YES;
    m_impl->run(panel, parentWindow, title, takeFront(std::move(picked)));
  }

} // namespace desk
