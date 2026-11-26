#include "app/app_notifier.h"

#include "core/log.h"

#import <Foundation/Foundation.h>
#import <UserNotifications/UserNotifications.h>

#include <charconv>
#include <cstring>
#include <map>
#include <set>

// Calls onResponse(identifier, action) on the main queue, where the app loop runs.
@interface AndromedaNotifierDelegate : NSObject <UNUserNotificationCenterDelegate>
@property(nonatomic, copy) void (^onResponse)(NSString* identifier, NSString* action);
@end

@implementation AndromedaNotifierDelegate
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler {
  completionHandler(UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionList);
}

- (void)userNotificationCenter:(UNUserNotificationCenter*)center
    didReceiveNotificationResponse:(UNNotificationResponse*)response
             withCompletionHandler:(void (^)(void))completionHandler {
  NSString* identifier = response.notification.request.identifier;
  NSString* action = response.actionIdentifier;
  dispatch_async(dispatch_get_main_queue(), ^{
    if (self.onResponse != nil) {
      self.onResponse(identifier, action);
    }
  });
  completionHandler();
}
@end

namespace {
  constexpr Logger kLog("notify");

  NSString* toNs(const std::string& s) {
    NSString* out = [NSString stringWithUTF8String:s.c_str()];
    return out != nil ? out : @"";
  }
  std::string fromNs(NSString* s) { return s.UTF8String != nullptr ? std::string(s.UTF8String) : std::string(); }
} // namespace

struct AppNotifier::Impl {
  struct Entry {
    ActionFn onAction;
    std::function<void()> onClosed;
  };

  UNUserNotificationCenter* center = nil; // nil outside an app bundle, where the center throws
  AndromedaNotifierDelegate* delegate = nil;
  std::map<std::string, Entry> entries; // by request identifier
  std::set<std::string> categories;     // registered button sets
  std::uint64_t next = 0;
  std::string appName;

  explicit Impl(std::string name) : appName(std::move(name)) {
    if (NSBundle.mainBundle.bundleIdentifier == nil) {
      kLog.warn("notifications unavailable: not running from an app bundle");
      return;
    }
    center = UNUserNotificationCenter.currentNotificationCenter;
    delegate = [AndromedaNotifierDelegate new];
    delegate.onResponse = ^(NSString* identifier, NSString* action) { respond(fromNs(identifier), action); };
    center.delegate = delegate;
    [center requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound)
                          completionHandler:^(BOOL granted, NSError* error) {
                            if (!granted) {
                              kLog.warn("notifications not allowed: {}", fromNs(error.localizedDescription));
                            }
                          }];
  }

  ~Impl() {
    if (center != nil) {
      delegate.onResponse = nil;
      center.delegate = nil;
    }
  }

  void respond(const std::string& identifier, NSString* action) {
    auto entry = entries.extract(identifier);
    if (entry.empty()) {
      return;
    }
    int button = 0;
    if ([action isEqualToString:UNNotificationDefaultActionIdentifier]) {
      button = 0;
    } else if ([action isEqualToString:UNNotificationDismissActionIdentifier]
               || std::from_chars(action.UTF8String, action.UTF8String + std::strlen(action.UTF8String), button).ec
                      != std::errc{}) {
      button = -1;
    }
    if (button >= 0 && entry.mapped().onAction) {
      entry.mapped().onAction(std::string{}, button);
    }
    // The center removes a notification once it is answered, as a freedesktop server closes it.
    if (entry.mapped().onClosed) {
      entry.mapped().onClosed();
    }
  }

  // A category per button set; the dismiss action reports a close.
  NSString* category(const std::vector<std::string>& buttons) {
    std::string id = "andromeda";
    for (const auto& b : buttons) {
      id += '\x1f' + b;
    }
    if (categories.insert(id).second) {
      NSMutableArray<UNNotificationAction*>* actions = [NSMutableArray array];
      for (std::size_t i = 0; i < buttons.size(); ++i) {
        [actions addObject:[UNNotificationAction actionWithIdentifier:toNs(std::to_string(i + 1))
                                                                title:toNs(buttons[i])
                                                              options:UNNotificationActionOptionForeground]];
      }
      UNNotificationCategory* added = [UNNotificationCategory categoryWithIdentifier:toNs(id)
                                                                            actions:actions
                                                                  intentIdentifiers:@[]
                                                                            options:UNNotificationCategoryOptionCustomDismissAction];
      [center getNotificationCategoriesWithCompletionHandler:^(NSSet<UNNotificationCategory*>* registered) {
        [center setNotificationCategories:[registered setByAddingObject:added]];
      }];
    }
    return toNs(id);
  }

  void notify(const AppNotice& notice, const std::string& key, ActionFn onAction, std::function<void()> onClosed) {
    if (center == nil) {
      return;
    }
    const std::string identifier = key.empty() ? "n:" + std::to_string(++next) : "k:" + key;
    UNMutableNotificationContent* content = [UNMutableNotificationContent new];
    content.title = toNs(notice.title.empty() ? appName : notice.title);
    content.body = toNs(notice.body);
    content.categoryIdentifier = category(notice.buttons);
    content.interruptionLevel = notice.urgency == 0   ? UNNotificationInterruptionLevelPassive
                                : notice.urgency == 2 ? UNNotificationInterruptionLevelTimeSensitive
                                                      : UNNotificationInterruptionLevelActive;
    entries[identifier] = Entry{.onAction = std::move(onAction), .onClosed = std::move(onClosed)};
    UNNotificationRequest* request = [UNNotificationRequest requestWithIdentifier:toNs(identifier)
                                                                          content:content
                                                                          trigger:nil];
    [center addNotificationRequest:request
             withCompletionHandler:^(NSError* error) {
               if (error != nil) {
                 kLog.warn("notify: {}", fromNs(error.localizedDescription));
               }
             }];
  }

  void close(const std::string& key) {
    const std::string identifier = "k:" + key;
    auto entry = entries.extract(identifier);
    if (entry.empty()) {
      return;
    }
    [center removeDeliveredNotificationsWithIdentifiers:@[ toNs(identifier) ]];
    if (entry.mapped().onClosed) {
      entry.mapped().onClosed();
    }
  }
};

// desk_app.h reaches ui/style.h, whose namespace Style collides with MacTypes.h's Style: app_notifier_macos_app.cpp.
std::string deskAppName(const DeskApp& app);

AppNotifier::AppNotifier(DeskApp& app) : m_impl(std::make_unique<Impl>(deskAppName(app))) {}
AppNotifier::~AppNotifier() = default;

void AppNotifier::notify(const AppNotice& notice, const std::string& key, ActionFn onAction, std::function<void()> onClosed) {
  m_impl->notify(notice, key, std::move(onAction), std::move(onClosed));
}

void AppNotifier::close(const std::string& key) { m_impl->close(key); }

