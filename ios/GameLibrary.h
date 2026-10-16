#import <Foundation/Foundation.h>

// The games installed in this copy of the app, the disc image each one uses, and the app's saved settings.
//
// Bundle layout written by the companion tool (one entry per game in Games/games.json):
//   Games/games.json                        {"version":1,"games":[{"id":"<sha256 of default.xbe>","name":"...","module":"Frameworks/game-<id16>.dylib","title":"Games/<id16>/title","noDisc":false,"legacyState":false}]}
//   Games/<id16>/title/                     default.xbe and the per-program files the runtime reads (patches.txt, dsound_hle.txt, ...)
//   Frameworks/game-<id16>.dylib            the translated game code
// An older single-game app has no manifest: title/default.xbe and Frameworks/reforged_game.dylib. It is shown as one game and keeps its old file locations.

@interface RFGame : NSObject
@property(copy) NSString* gameId;        // SHA-256 of the program's main executable (lower-case hex)
@property(copy) NSString* name;          // shown in the library
@property(copy) NSString* module;        // absolute path of the translated code
@property(copy) NSString* titleDir;      // absolute path of the per-program files
@property(copy) NSString* xbe;           // absolute path of the main executable
@property(copy) NSString* iconPath;      // absolute path of the game's icon image (from its own executable), or nil
@property(copy) NSString* details;       // one line about the game: title id, regions, build date
@property(copy) NSString* folder;        // working folder: the game's own files (virtual disk, console identity, caches) live here
@property(assign) BOOL noDisc;           // test programs and homebrew that need no disc image
@property(assign) BOOL legacy;           // an older single-game app: the working folder is the app's Documents folder itself
@property(assign) BOOL migrateOldState;  // files from an older single-game layout still sit in Documents and belong to this game
@end

@interface RFLibrary : NSObject
+ (NSArray<RFGame*>*)gamesInBundle:(NSString*)bundle documents:(NSString*)docs;
// The disc image this game will use (a path), or nil. Looks at: the remembered choice, a copy saved in the game's folder, any .iso/.xiso in Documents whose
// main executable matches, and (single-game apps) Documents/game.iso. `how` receives a short description for the screen.
+ (NSString*)discForGame:(RFGame*)game documents:(NSString*)docs how:(NSString**)how;
// Checks that a disc image holds this game's executable; returns nil if it does, otherwise a message to show.
+ (NSString*)problemWithDisc:(NSString*)path forGame:(RFGame*)game;
+ (void)rememberBookmark:(NSData*)bookmark forGame:(RFGame*)game;
+ (void)forgetDiscForGame:(RFGame*)game;
+ (void)prepareFolderForGame:(RFGame*)game documents:(NSString*)docs;   // creates the folder and moves files of an older layout into it, once
@end

// Settings the user can change in the app. They are saved, and applied to the environment the runtime reads before it starts.
// (A Documents/reforged.env file still wins, for debugging.)
@interface RFSettings : NSObject
+ (NSInteger)touchMode;                  // 0 automatic (hide when a controller is connected), 1 always show, 2 always hide
+ (void)setTouchMode:(NSInteger)mode;
+ (NSInteger)renderScale;                // 1 original resolution, 2 or 3 times that (smoother edges, heavier)
+ (void)setRenderScale:(NSInteger)scale;
+ (BOOL)showFps;                        // an on-screen frame counter while the game runs
+ (void)setShowFps:(BOOL)on;
+ (BOOL)relayEnabled;                   // System Link over the Internet through a relay server
+ (void)setRelayEnabled:(BOOL)on;
+ (NSString*)relayServer;                // host or host:port (default port 3075)
+ (void)setRelayServer:(NSString*)server;
+ (NSString*)relayRoom;                  // everyone who plays together uses the same room name
+ (void)setRelayRoom:(NSString*)room;
+ (BOOL)diagnostics;                     // save occasional frame captures and detailed logs (help when reporting a problem)
+ (void)setDiagnostics:(BOOL)on;
+ (void)applyToEnvironment;
@end
