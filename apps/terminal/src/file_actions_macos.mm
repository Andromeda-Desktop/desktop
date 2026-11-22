#include "file_actions.h"

#import <AppKit/AppKit.h>

namespace term {

  namespace {
    NSArray<NSURL*>* urls(const FileActions::Paths& paths) {
      NSMutableArray<NSURL*>* out = [NSMutableArray arrayWithCapacity:paths.size()];
      for (const auto& path : paths) {
        [out addObject:[NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]]];
      }
      return out;
    }

    // Copies or moves on a background queue; done runs on the main queue.
    void transfer(const FileActions::Paths& paths, const std::filesystem::path& dest, bool move,
                  FileActions::Done done) {
      NSArray<NSURL*>* sources = urls(paths);
      NSURL* target = [NSURL fileURLWithPath:[NSString stringWithUTF8String:dest.c_str()] isDirectory:YES];
      auto* callback = new FileActions::Done(std::move(done));
      dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError* error = nil;
        for (NSURL* source in sources) {
          NSURL* to = [target URLByAppendingPathComponent:source.lastPathComponent];
          const BOOL ok = move ? [NSFileManager.defaultManager moveItemAtURL:source toURL:to error:&error]
                               : [NSFileManager.defaultManager copyItemAtURL:source toURL:to error:&error];
          if (!ok) {
            break;
          }
        }
        std::string message = error != nil ? std::string(error.localizedDescription.UTF8String) : std::string();
        dispatch_async(dispatch_get_main_queue(), ^{
          if (*callback) {
            (*callback)(message);
          }
          delete callback;
        });
      });
    }
  } // namespace

  struct FileActions::Impl {};

  FileActions::FileActions(SessionBus* /*bus*/) : m_impl(std::make_unique<Impl>()) {}

  FileActions::~FileActions() = default;

  void FileActions::copy(const Paths& paths, const std::filesystem::path& dest, Done done) {
    transfer(paths, dest, false, std::move(done));
  }

  void FileActions::move(const Paths& paths, const std::filesystem::path& dest, Done done) {
    transfer(paths, dest, true, std::move(done));
  }

  void FileActions::trash(const Paths& paths, Done done) {
    auto* callback = new Done(std::move(done));
    [NSWorkspace.sharedWorkspace recycleURLs:urls(paths)
                           completionHandler:^(NSDictionary<NSURL*, NSURL*>*, NSError* error) {
                             if (*callback) {
                               (*callback)(error != nil ? std::string(error.localizedDescription.UTF8String)
                                                        : std::string());
                             }
                             delete callback;
                           }];
  }

  void FileActions::rename(const std::filesystem::path& path, const std::string& name, Done done) {
    NSError* error = nil;
    NSString* from = [NSString stringWithUTF8String:path.c_str()];
    NSString* to = [[from stringByDeletingLastPathComponent] stringByAppendingPathComponent:[NSString stringWithUTF8String:name.c_str()]];
    [NSFileManager.defaultManager moveItemAtPath:from toPath:to error:&error];
    if (done) {
      done(error != nil ? std::string(error.localizedDescription.UTF8String) : std::string());
    }
  }

  void FileActions::showInFiles(const Paths& paths) {
    [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:urls(paths)];
  }

} // namespace term
