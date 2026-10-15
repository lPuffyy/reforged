// The app shell: a window, a status line, and the runtime on a background thread. The runtime writes its files (log, virtual disk, console identity) under the
// app's Documents folder, which is also what Finder / iTunes file sharing exposes, so a disc image can be copied there.
//
// What it runs is decided by what is inside the app bundle (the companion tool adds these to a personal copy of the app):
//   Frameworks/reforged_game.dylib  the translated game code (the runtime loads it itself)
//   title/default.xbe               the game's executable (the translated code was made from exactly this file)
// and by an optional disc image at Documents/game.iso.
#import "AppDelegate.h"

extern "C" int ReforgedMain(int argc, char** argv);

@implementation AppDelegate

- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)launchOptions {
  self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
  UIViewController* vc = [[UIViewController alloc] init];
  vc.view.backgroundColor = [UIColor blackColor];
  UILabel* label = [[UILabel alloc] initWithFrame:vc.view.bounds];
  label.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  label.textColor = [UIColor whiteColor];
  label.textAlignment = NSTextAlignmentCenter;
  label.numberOfLines = 0;
  label.text = @"Reforged\nstarting...";
  [vc.view addSubview:label];
  self.window.rootViewController = vc;
  [self.window makeKeyAndVisible];

  NSFileManager* fm = [NSFileManager defaultManager];
  NSString* docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
  [fm changeCurrentDirectoryPath:docs];
  NSString* bundle = [[NSBundle mainBundle] bundlePath];
  NSString* xbe = [bundle stringByAppendingPathComponent:@"title/default.xbe"];
  NSString* module = [bundle stringByAppendingPathComponent:@"Frameworks/reforged_game.dylib"];
  NSString* iso = [docs stringByAppendingPathComponent:@"game.iso"];
  BOOL haveGame = [fm fileExistsAtPath:xbe];
  BOOL haveModule = [fm fileExistsAtPath:module];
  BOOL haveIso = [fm fileExistsAtPath:iso];

  if (haveGame) {
    setenv("XBOX_TITLE_DIR", [[bundle stringByAppendingPathComponent:@"title"] UTF8String], 1);
    if (haveModule) { setenv("XBOX_LIFT", "1", 1); setenv("XBOX_LIFT_MODULE", [module UTF8String], 1); }
  }
  label.text = !haveGame ? @"Reforged\nno game is installed\nuse the companion tool to build your copy"
             : !haveModule ? @"Reforged\nthe game module is missing"
             : haveIso ? @"Reforged\nrunning" : @"Reforged\nrunning (no disc image in Documents/game.iso)";

  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    // Without a game this just starts the runtime, lets it initialise and reports that nothing is installed.
    NSString* disc = haveIso ? iso : @"none";
    const char* args[] = {"reforged", [disc UTF8String], haveGame ? [xbe UTF8String] : "none", haveGame ? "86400" : "2"};
    int rc = ReforgedMain(4, (char**)args);
    dispatch_async(dispatch_get_main_queue(), ^{
      label.text = [NSString stringWithFormat:@"Reforged\nthe runtime stopped (code %d)", rc];
    });
  });
  return YES;
}

@end
