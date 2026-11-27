#import <AppKit/AppKit.h>

#include <string>
#include <vector>

void showInFinder(const std::vector<std::string>& paths) {
  NSMutableArray<NSURL*>* urls = [NSMutableArray arrayWithCapacity:paths.size()];
  for (const std::string& path : paths) {
    [urls addObject:[NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]]];
  }
  [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:urls];
}
