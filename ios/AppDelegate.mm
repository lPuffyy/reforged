// The app shell: a window with a Metal-backed view for the game picture, an on-screen controller, a status line, and the runtime on a background thread.
// The runtime writes its files (log, virtual disk, console identity) under the app's Documents folder, which is also what Finder / iTunes file sharing exposes.
//
// What it runs is decided by what is inside the app bundle (the companion tool adds these to a personal copy of the app):
//   Frameworks/reforged_game.dylib  the translated game code (the runtime loads it itself)
//   title/default.xbe               the game's executable (the translated code was made from exactly this file)
//   Frameworks/MoltenVK.framework   the Vulkan-on-Metal layer used by the renderer (part of the generic app)
// The disc image is not part of the app: it is Documents/game.iso if present, otherwise the file the user picks in the Files app (opened in place, never
// copied; the choice is remembered).
#import "AppDelegate.h"
#import "TouchControls.h"
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

extern "C" int ReforgedMain(int argc, char** argv);
extern "C" void ReforgedSetMetalLayer(void* layer);

// A view whose backing layer is a CAMetalLayer, which the Vulkan renderer draws into.
@interface ReforgedView : UIView
@end
@implementation ReforgedView
+ (Class)layerClass { return [CAMetalLayer class]; }
@end

@interface AppDelegate () <UIDocumentPickerDelegate>
@property(strong, nonatomic) UILabel* label;
@property(strong, nonatomic) UIButton* chooseButton;
@property(strong, nonatomic) TouchControls* controls;
@property(nonatomic) BOOL started;
@property(copy, nonatomic) NSString* xbe;
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

  self.chooseButton = [UIButton buttonWithType:UIButtonTypeSystem];
  [self.chooseButton setTitle:@"Choose disc image..." forState:UIControlStateNormal];
  self.chooseButton.frame = CGRectMake(0, 0, 240, 44);
  self.chooseButton.center = CGPointMake(vc.view.bounds.size.width / 2, vc.view.bounds.size.height / 2 + 60);
  self.chooseButton.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin | UIViewAutoresizingFlexibleRightMargin | UIViewAutoresizingFlexibleTopMargin | UIViewAutoresizingFlexibleBottomMargin;
  self.chooseButton.hidden = YES;
  [self.chooseButton addTarget:self action:@selector(chooseDisc) forControlEvents:UIControlEventTouchUpInside];
  [vc.view addSubview:self.chooseButton];

  self.window.rootViewController = vc;
  [self.window makeKeyAndVisible];
  ReforgedSetMetalLayer((__bridge void*)layer);

  NSFileManager* fm = [NSFileManager defaultManager];
  NSString* docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
  [fm changeCurrentDirectoryPath:docs];
  // Debug switches: Documents/reforged.env, one KEY=VALUE per line (# starts a comment); each becomes an environment variable before the runtime starts.
  int envCount = 0; NSString* envNote = nil;
  {
    NSString* envText = [NSString stringWithContentsOfFile:[docs stringByAppendingPathComponent:@"reforged.env"] encoding:NSUTF8StringEncoding error:nil];
    for (NSString* raw in [envText componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]]) {
      NSString* line = [raw stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
      if (!line.length || [line hasPrefix:@"#"]) continue;
      NSRange eq = [line rangeOfString:@"="]; if (eq.location == NSNotFound || eq.location == 0) continue;
      setenv([[line substringToIndex:eq.location] UTF8String], [[line substringFromIndex:eq.location + 1] UTF8String], 1);
      envCount++;
    }
    envNote = envText ? [NSString stringWithFormat:@"reforged.env found: %d setting(s) applied", envCount] : @"no reforged.env in the app folder";
  }
  NSString* bundle = [[NSBundle mainBundle] bundlePath];
  self.xbe = [bundle stringByAppendingPathComponent:@"title/default.xbe"];
  NSString* module = [bundle stringByAppendingPathComponent:@"Frameworks/reforged_game.dylib"];
  BOOL haveGame = [fm fileExistsAtPath:self.xbe];
  BOOL haveModule = [fm fileExistsAtPath:module];

  if (!haveGame) {
    // Nothing to run: start the runtime briefly so it initialises and logs, and say what is missing.
    self.label.text = @"Reforged\nno game is installed\nuse the companion tool to build your copy";
    [self runWithIso:@"none" xbe:@"none" seconds:"2"];
    return YES;
  }
  if (envNote) setenv("REFORGED_ENV_NOTE", [envNote UTF8String], 1);
  setenv("XBOX_TITLE_DIR", [[bundle stringByAppendingPathComponent:@"title"] UTF8String], 1);
  if (!haveModule) { self.label.text = @"Reforged\nthe game module is missing"; return YES; }
  setenv("XBOX_LIFT", "1", 1);
  setenv("XBOX_LIFT_MODULE", [module UTF8String], 1);

  if ([fm fileExistsAtPath:[bundle stringByAppendingPathComponent:@"title/no-disc.txt"]]) { [self runWithIso:@"none" xbe:self.xbe seconds:"86400"]; return YES; }   // programs that need no disc (test programs, homebrew)
  NSString* iso = [docs stringByAppendingPathComponent:@"game.iso"];
  if ([fm fileExistsAtPath:iso]) { [self runWithIso:iso xbe:self.xbe seconds:"86400"]; return YES; }
  NSString* remembered = [self rememberedDisc];
  if (remembered) { [self runWithIso:remembered xbe:self.xbe seconds:"86400"]; return YES; }
  self.label.text = @"Reforged\nno disc image yet";
  self.chooseButton.hidden = NO;
  return YES;
}

// ---- disc image ----
- (NSString*)rememberedDisc {
  NSData* data = [[NSUserDefaults standardUserDefaults] dataForKey:@"discBookmark"]; if (!data) return nil;
  BOOL stale = NO; NSError* err = nil;
  NSURL* url = [NSURL URLByResolvingBookmarkData:data options:0 relativeToURL:nil bookmarkDataIsStale:&stale error:&err];
  if (!url || ![url startAccessingSecurityScopedResource]) return nil;   // access stays open for the life of the app: the runtime reads the file in place
  return url.path;
}

- (void)chooseDisc {
  UIDocumentPickerViewController* picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeItem, UTTypeData, UTTypeDiskImage] asCopy:NO];
  picker.delegate = self;
  [self.window.rootViewController presentViewController:picker animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
  NSURL* url = urls.firstObject; if (!url) return;
  if (![url startAccessingSecurityScopedResource]) { self.label.text = @"Reforged\ncould not open that file"; return; }
  NSError* err = nil;
  NSData* bookmark = [url bookmarkDataWithOptions:0 includingResourceValuesForKeys:nil relativeToURL:nil error:&err];
  if (bookmark) [[NSUserDefaults standardUserDefaults] setObject:bookmark forKey:@"discBookmark"];
  self.chooseButton.hidden = YES;
  [self runWithIso:url.path xbe:self.xbe seconds:"86400"];
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
      self.label.text = [NSString stringWithFormat:@"Reforged\nthe runtime stopped (code %d)", rc];
    });
  });
}

@end
