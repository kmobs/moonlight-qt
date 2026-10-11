#include "macmenu.h"

#include "imgui/devui.h"
#include "streaming/session.h"

#import <Cocoa/Cocoa.h>

@interface MacMenuTarget: NSObject <NSMenuItemValidation>
- (void)disconnect:(id)sender;
- (void)disconnectAndClose:(id)sender;
- (void)toggleStats:(id)sender;
- (void)toggleGraphs:(id)sender;
- (void)toggleMetalHud:(id)sender;
- (void)toggleAdvancedControls:(id)sender;
- (void)hideAllOverlays:(id)sender;
@end

static void PushDisconnectEvent()
{
    SDL_Event event = {};
    event.type = SDL_QUIT;
    event.quit.timestamp = SDL_GetTicks();
    SDL_PushEvent(&event);
}

@implementation MacMenuTarget

- (void)disconnect:(id)sender
{
    (void) sender;
    if (Session::get() == nullptr) {
        return;
    }
    PushDisconnectEvent();
}

- (void)disconnectAndClose:(id)sender
{
    (void) sender;
    Session* session = Session::get();
    if (session == nullptr) {
        return;
    }
    session->setShouldExit(true);
    PushDisconnectEvent();
}

- (void)toggleStats:(id)sender
{
    (void) sender;
    Session* session = Session::get();
    if (session == nullptr) {
        return;
    }
    auto& overlays = session->getOverlayManager();
    overlays.setOverlayState(Overlay::OverlayDebug, !overlays.isOverlayEnabled(Overlay::OverlayDebug));
}

- (void)toggleGraphs:(id)sender
{
    (void) sender;
    if (Session::get() == nullptr) {
        return;
    }
    Stats::instance().SetShowGraphs(!Stats::instance().GetShowGraphs());
}

- (void)toggleMetalHud:(id)sender
{
    (void) sender;
    if (Session::get() == nullptr) {
        return;
    }
    DevUISettings::instance().SetConfig([](DevUIConfig& config) {
        config.showMetalHud = !config.showMetalHud;
    });
}

- (void)toggleAdvancedControls:(id)sender
{
    (void) sender;
    if (Session::get() == nullptr) {
        return;
    }
    DevUISettings::instance().Toggle();
}

- (void)hideAllOverlays:(id)sender
{
    (void) sender;
    Session* session = Session::get();
    if (session == nullptr) {
        return;
    }
    session->getOverlayManager().setOverlayState(Overlay::OverlayDebug, false);
    Stats::instance().SetShowGraphs(false);
    DevUISettings::instance().SetPanelOpen(false);
    DevUISettings::instance().SetConfig([](DevUIConfig& config) {
        config.showMetalHud = false;
    });
}

- (BOOL)validateMenuItem:(NSMenuItem*)item
{
    Session* session = Session::get();
    bool checked = false;
    if (session != nullptr) {
        if (item.action == @selector(toggleStats:)) {
            checked = session->getOverlayManager().isOverlayEnabled(Overlay::OverlayDebug);
        }
        else if (item.action == @selector(toggleGraphs:)) {
            checked = Stats::instance().GetShowGraphs();
        }
        else if (item.action == @selector(toggleMetalHud:)) {
            checked = DevUISettings::instance().GetConfig().showMetalHud;
        }
        else if (item.action == @selector(toggleAdvancedControls:)) {
            checked = DevUISettings::instance().IsVisible();
        }
    }
    item.state = checked ? NSControlStateValueOn : NSControlStateValueOff;
    return session != nullptr;
}

@end

static void AddMenuItem(NSMenu* menu, NSString* title, SEL action, MacMenuTarget* target)
{
    NSMenuItem* item = [[[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:@""] autorelease];
    item.target = target;
    [menu addItem:item];
}

static NSMenu* AddMenu(NSMenu* mainMenu, NSString* title)
{
    NSMenu* menu = [[[NSMenu alloc] initWithTitle:title] autorelease];
    NSMenuItem* item = [[[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""] autorelease];
    item.submenu = menu;
    [mainMenu addItem:item];
    return menu;
}

static void AddAboutMenuItem(NSMenu* mainMenu)
{
    NSString* appName = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleName"];
    if (appName.length == 0) {
        appName = [[NSProcessInfo processInfo] processName];
    }

    NSMenu* appMenu = mainMenu.numberOfItems > 0 ? [mainMenu itemAtIndex:0].submenu : nil;
    if (appMenu == nil) {
        appMenu = [[[NSMenu alloc] initWithTitle:appName] autorelease];
        NSMenuItem* appItem = [[[NSMenuItem alloc] initWithTitle:appName action:nil keyEquivalent:@""] autorelease];
        appItem.submenu = appMenu;
        [mainMenu insertItem:appItem atIndex:0];
    }

    if ([appMenu indexOfItemWithTarget:NSApp andAction:@selector(orderFrontStandardAboutPanel:)] >= 0) {
        return;
    }

    NSMenuItem* aboutItem = [[[NSMenuItem alloc] initWithTitle:[@"About " stringByAppendingString:appName]
                                                        action:@selector(orderFrontStandardAboutPanel:)
                                                 keyEquivalent:@""] autorelease];
    aboutItem.target = NSApp;
    [appMenu insertItem:aboutItem atIndex:0];
    if (appMenu.numberOfItems > 1 && ![appMenu itemAtIndex:1].separatorItem) {
        [appMenu insertItem:[NSMenuItem separatorItem] atIndex:1];
    }
}

void InitMacMenu()
{
    // Keep the action target alive for the application's lifetime.
    static MacMenuTarget* target = nil;
    if (target != nil) {
        return;
    }
    target = [[MacMenuTarget alloc] init];

    @autoreleasepool {
        NSMenu* mainMenu = NSApp.mainMenu;
        if (mainMenu == nil) {
            mainMenu = [[[NSMenu alloc] initWithTitle:@""] autorelease];
            NSApp.mainMenu = mainMenu;
        }

        AddAboutMenuItem(mainMenu);

        NSMenu* sessionMenu = AddMenu(mainMenu, @"Session");
        AddMenuItem(sessionMenu, @"Disconnect", @selector(disconnect:), target);
        AddMenuItem(sessionMenu, @"Disconnect and close", @selector(disconnectAndClose:), target);

        NSMenu* viewMenu = AddMenu(mainMenu, @"View");
        AddMenuItem(viewMenu, @"Stats", @selector(toggleStats:), target);
        AddMenuItem(viewMenu, @"Performance Graphs", @selector(toggleGraphs:), target);
        AddMenuItem(viewMenu, @"Metal HUD", @selector(toggleMetalHud:), target);
        AddMenuItem(viewMenu, @"Advanced Controls", @selector(toggleAdvancedControls:), target);
        AddMenuItem(viewMenu, @"Hide all overlays", @selector(hideAllOverlays:), target);
    }
}
