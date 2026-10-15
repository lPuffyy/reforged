// The app shell: a window, a status line, and the runtime on a background thread. The runtime writes its files (log, virtual disk, console identity) under the
// app's Documents folder, which is also what Finder / iTunes file sharing exposes, so a disc image can be copied there.
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

  NSString* docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
  [[NSFileManager defaultManager] changeCurrentDirectoryPath:docs];

  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    // Without a game there is nothing to run yet: this starts the runtime, lets it initialise and reports that no game is installed.
    const char* args[] = {"reforged", "none", "none", "2"};
    int rc = ReforgedMain(4, (char**)args);
    dispatch_async(dispatch_get_main_queue(), ^{
      label.text = [NSString stringWithFormat:@"Reforged\nthe runtime started and stopped (code %d)\nno game is installed", rc];
    });
  });
  return YES;
}

@end
