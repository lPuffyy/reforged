// The app shell: a window with a Metal-backed view for the game picture, an on-screen controller, and the runtime on a background thread.
// At launch it shows the game library (ios/LibraryViewController.mm). The runtime writes its files under the app's Documents folder, which is also what
// Finder / iTunes file sharing and the Files app expose: the log (reforged.log) at the top, and each game's own files in Documents/Games/<id>/.
//
// What can run is decided by what is inside the app bundle (the companion tool adds these to a personal copy of the app, see GameLibrary.h):
//   Games/games.json, Games/<id>/title/, Frameworks/game-<id>.dylib   one entry per installed game (older single-game apps: title/ and Frameworks/reforged_game.dylib)
//   Frameworks/MoltenVK.framework                                      the Vulkan-on-Metal layer used by the renderer (part of the generic app)
// The disc image is not part of the app: the user chooses it in the Files picker (opened in place, or copied into the app) or copies it into the app's folder.
#import "AppDelegate.h"
#import "TouchControls.h"
#import "GameLibrary.h"
#import "LibraryViewController.h"
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

extern "C" int ReforgedMain(int argc, char** argv);
extern "C" void ReforgedSetMetalLayer(void* layer);
extern "C" int ReforgedFpsTenths();
extern "C" void ReforgedSetPaused(int paused);

// A view whose backing layer is a CAMetalLayer, which the Vulkan renderer draws into.
@interface ReforgedView : UIView
@end
@implementation ReforgedView
+ (Class)layerClass { return [CAMetalLayer class]; }
@end

@interface AppDelegate () <RFLibraryDelegate>
@property(strong, nonatomic) UILabel* label;
@property(strong, nonatomic) UILabel* fpsLabel;
@property(strong, nonatomic) NSTimer* fpsTimer;
@property(strong, nonatomic) TouchControls* controls;
@property(strong, nonatomic) UINavigationController* libraryNav;
@property(nonatomic) BOOL started;
@property(copy, nonatomic) NSString* docs;
@property(copy, nonatomic) NSString* envNote;
@end

@implementation AppDelegate

- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)launchOptions {
  self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
  UIViewController* vc = [[UIViewController alloc] init];
  vc.view.backgroundColor = [UIColor blackColor];

  ReforgedView* picture = [[ReforgedView alloc] initWithFrame:vc.view.bounds];
  picture.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  CAMetalLayer* layer = (CAMetalLayer*)picture.layer;
  layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
  layer.contentsScale = UIScreen.mainScreen.scale;
  [vc.view addSubview:picture];

  self.controls = [[TouchControls alloc] initWithFrame:vc.view.bounds];
  self.controls.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  [vc.view addSubview:self.controls];

  self.label = [[UILabel alloc] initWithFrame:vc.view.bounds];
  self.label.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  self.label.textColor = [UIColor whiteColor];
  self.label.textAlignment = NSTextAlignmentCenter;
  self.label.numberOfLines = 0;
  self.label.userInteractionEnabled = NO;
  [vc.view addSubview:self.label];

  self.fpsLabel = [[UILabel alloc] initWithFrame:CGRectMake(0, 0, 110, 24)];
  self.fpsLabel.font = [UIFont monospacedDigitSystemFontOfSize:13 weight:UIFontWeightSemibold];
  self.fpsLabel.textColor = [UIColor whiteColor]; self.fpsLabel.textAlignment = NSTextAlignmentCenter;
  self.fpsLabel.backgroundColor = [UIColor colorWithWhite:0 alpha:0.55]; self.fpsLabel.layer.cornerRadius = 6; self.fpsLabel.clipsToBounds = YES;
  self.fpsLabel.userInteractionEnabled = NO; self.fpsLabel.hidden = YES;
  [vc.view addSubview:self.fpsLabel];

  self.window.rootViewController = vc;
  [self.window makeKeyAndVisible];
  ReforgedSetMetalLayer((__bridge void*)layer);

  NSFileManager* fm = [NSFileManager defaultManager];
  self.docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
  [fm changeCurrentDirectoryPath:self.docs];
  [RFSettings loadBundleDefaults:[[NSBundle mainBundle] bundlePath]];   // a relay built into this app by the companion (--relay), if any
  [self applyEnvironment];

  NSString* bundle = [[NSBundle mainBundle] bundlePath];
  NSArray<RFGame*>* games = [RFLibrary gamesInBundle:bundle documents:self.docs];
  if (!games.count) {
    // Nothing to run: start the runtime briefly so it initialises and logs, and say what is missing.
    self.label.text = @"Reforged\nno game is installed\nuse the companion tool to build your copy";
    setenv("REFORGED_LOG", [[self.docs stringByAppendingPathComponent:@"reforged.log"] UTF8String], 1);
    [self runWithIso:@"none" xbe:@"none" seconds:"2"];
    return YES;
  }
  if (getenv("REFORGED_AUTOSTART") && games.count == 1) {   // for automated tests: start the only game without showing the library
    NSString* disc = [RFLibrary discForGame:games.firstObject documents:self.docs how:nil];
    if (disc) { [self libraryWantsToPlay:games.firstObject disc:disc]; return YES; }
  }
  LibraryViewController* library = [[LibraryViewController alloc] initWithGames:games documents:self.docs];
  library.delegate = self;
  self.libraryNav = [[UINavigationController alloc] initWithRootViewController:library];
  self.libraryNav.modalPresentationStyle = UIModalPresentationFullScreen;
  dispatch_async(dispatch_get_main_queue(), ^{ [vc presentViewController:self.libraryNav animated:NO completion:nil]; });
  return YES;
}

// The runtime must not submit GPU work while the app is not in the foreground, and it rebuilds its window surface when the app comes back.
- (void)applicationWillResignActive:(UIApplication*)application { ReforgedSetPaused(1); }
- (void)applicationDidBecomeActive:(UIApplication*)application { ReforgedSetPaused(0); }

// Saved settings first, then Documents/reforged.env (one KEY=VALUE per line, # starts a comment; each becomes an environment variable), which wins.
- (void)applyEnvironment {
  [RFSettings applyToEnvironment];
  int envCount = 0; NSString* note = nil;
  NSString* envText = [NSString stringWithContentsOfFile:[self.docs stringByAppendingPathComponent:@"reforged.env"] encoding:NSUTF8StringEncoding error:nil];
  for (NSString* raw in [envText componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]]) {
    NSString* line = [raw stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (!line.length || [line hasPrefix:@"#"]) continue;
    NSRange eq = [line rangeOfString:@"="]; if (eq.location == NSNotFound || eq.location == 0) continue;
    setenv([[line substringToIndex:eq.location] UTF8String], [[line substringFromIndex:eq.location + 1] UTF8String], 1);
    envCount++;
  }
  note = envText ? [NSString stringWithFormat:@"reforged.env found: %d setting(s) applied", envCount] : @"no reforged.env in the app folder";
  self.envNote = note;
  setenv("REFORGED_ENV_NOTE", [note UTF8String], 1);
}

// ---- library callbacks ----
- (void)settingsChanged {
  [self applyEnvironment];
  [self.controls controllersChanged];
  [self updateFpsCounter];
}

// The frame counter: shown while a game runs when the setting is on.
- (void)updateFpsCounter {
  BOOL show = [RFSettings showFps] && self.started;
  self.fpsLabel.hidden = !show;
  if (show) {
    UIEdgeInsets in = self.window.safeAreaInsets;
    self.fpsLabel.frame = CGRectMake(MAX(in.left, 12) + 6, MAX(in.top, 8) + 4, 110, 24);
    if (!self.fpsTimer) {
      self.fpsTimer = [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer* t) {
        int f = ReforgedFpsTenths();
        self.fpsLabel.text = f > 0 ? [NSString stringWithFormat:@"%.1f fps", f / 10.0] : @"-- fps";
      }];
    }
  } else { [self.fpsTimer invalidate]; self.fpsTimer = nil; }
}

- (void)libraryWantsToPlay:(RFGame*)game disc:(NSString*)disc {
  if (self.started) return;
  [self applyEnvironment];   // settings changed in the library, and reforged.env must still win
  [RFLibrary prepareFolderForGame:game documents:self.docs];
  if (!game.legacy) [[NSFileManager defaultManager] changeCurrentDirectoryPath:game.folder];   // the runtime's own files (virtual disk, console identity, caches) are relative to the working folder
  setenv("REFORGED_LOG", [[self.docs stringByAppendingPathComponent:@"reforged.log"] UTF8String], 1);
  setenv("XBOX_TITLE_DIR", [game.titleDir UTF8String], 1);
  setenv("XBOX_LIFT", "1", 1);
  setenv("XBOX_LIFT_MODULE", [game.module UTF8String], 1);
  [self.controls controllersChanged];
  [self.libraryNav.presentingViewController dismissViewControllerAnimated:YES completion:nil];
  [self runWithIso:disc xbe:game.xbe seconds:"86400"];
  [UIApplication sharedApplication].idleTimerDisabled = YES;   // a sleeping screen suspends the app: the game (and any System Link connection) would stop
  [self updateFpsCounter];
}

// ---- runtime ----
- (void)runWithIso:(NSString*)iso xbe:(NSString*)xbe seconds:(const char*)seconds {
  if (self.started) return;
  self.started = YES;
  const bool running = [xbe hasSuffix:@".xbe"];
  if (running) self.label.hidden = YES;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    const char* args[] = {"reforged", [iso UTF8String], [xbe UTF8String], seconds};
    int rc = ReforgedMain(4, (char**)args);
    dispatch_async(dispatch_get_main_queue(), ^{
      self.label.hidden = NO;
      [UIApplication sharedApplication].idleTimerDisabled = NO;
      self.label.text = [NSString stringWithFormat:@"Reforged\nthe runtime stopped (code %d)\nclose the app and open it again to play", rc];
    });
  });
}

@end
