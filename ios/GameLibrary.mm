#import "GameLibrary.h"
#import <CommonCrypto/CommonDigest.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <net/if.h>

extern "C" uint8_t* ReforgedDiscReadRootFile(const char* isoPath, const char* fileName, uint32_t* outSize);

static NSString* Sha256Hex(NSData* data) {
  uint8_t digest[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256(data.bytes, (CC_LONG)data.length, digest);
  NSMutableString* s = [NSMutableString stringWithCapacity:64];
  for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) [s appendFormat:@"%02x", digest[i]];
  return s;
}

// The program's title as stored in its certificate (40 UTF-16 characters), or nil.
static NSString* XbeTitleName(NSData* xbe) {
  if (xbe.length < 0x200) return nil;
  const uint8_t* b = (const uint8_t*)xbe.bytes;
  uint32_t base, cert; memcpy(&base, b + 0x104, 4); memcpy(&cert, b + 0x118, 4);
  if (cert < base || cert - base + 0x0C + 80 > xbe.length) return nil;
  NSString* name = [[NSString alloc] initWithBytes:b + (cert - base) + 0x0C length:80 encoding:NSUTF16LittleEndianStringEncoding];
  NSCharacterSet* nul = [NSCharacterSet characterSetWithRange:NSMakeRange(0, 1)];
  NSRange z = [name rangeOfCharacterFromSet:nul]; if (z.location != NSNotFound) name = [name substringToIndex:z.location];
  name = [name stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
  return name.length ? name : nil;
}

static NSData* DiscExecutable(NSString* path) {
  uint32_t n = 0; uint8_t* buf = ReforgedDiscReadRootFile(path.fileSystemRepresentation, "default.xbe", &n);
  if (!buf) return nil;
  return [NSData dataWithBytesNoCopy:buf length:n freeWhenDone:YES];
}

@implementation RFGame
@end

@implementation RFLibrary

+ (NSArray<RFGame*>*)gamesInBundle:(NSString*)bundle documents:(NSString*)docs {
  NSFileManager* fm = [NSFileManager defaultManager];
  NSMutableArray<RFGame*>* games = [NSMutableArray array];
  NSData* manifest = [NSData dataWithContentsOfFile:[bundle stringByAppendingPathComponent:@"Games/games.json"]];
  if (manifest) {
    NSDictionary* root = [NSJSONSerialization JSONObjectWithData:manifest options:0 error:nil];
    for (NSDictionary* e in ([root isKindOfClass:[NSDictionary class]] ? root[@"games"] : @[])) {
      NSString* gid = e[@"id"]; if (![gid isKindOfClass:[NSString class]] || gid.length < 16) continue;
      RFGame* g = [RFGame new];
      g.gameId = gid;
      g.name = [e[@"name"] isKindOfClass:[NSString class]] && [e[@"name"] length] ? e[@"name"] : [NSString stringWithFormat:@"Game %@", [gid substringToIndex:8]];
      g.module = [bundle stringByAppendingPathComponent:e[@"module"] ?: @""];
      g.titleDir = [bundle stringByAppendingPathComponent:e[@"title"] ?: @""];
      g.xbe = [g.titleDir stringByAppendingPathComponent:@"default.xbe"];
      if ([e[@"icon"] isKindOfClass:[NSString class]]) g.iconPath = [bundle stringByAppendingPathComponent:e[@"icon"]];
      NSMutableArray* bits = [NSMutableArray array];
      if ([e[@"titleId"] isKindOfClass:[NSString class]]) [bits addObject:[@"Title ID " stringByAppendingString:e[@"titleId"]]];
      if ([e[@"regions"] isKindOfClass:[NSArray class]] && [e[@"regions"] count]) [bits addObject:[e[@"regions"] componentsJoinedByString:@"/"]];
      if ([e[@"built"] isKindOfClass:[NSString class]]) [bits addObject:[@"build " stringByAppendingString:e[@"built"]]];
      g.details = [bits componentsJoinedByString:@" \u00B7 "];
      g.noDisc = [e[@"noDisc"] boolValue];
      g.migrateOldState = [e[@"legacyState"] boolValue];
      g.folder = [[docs stringByAppendingPathComponent:@"Games"] stringByAppendingPathComponent:[gid substringToIndex:16]];
      if ([fm fileExistsAtPath:g.xbe] && [fm fileExistsAtPath:g.module]) [games addObject:g];
    }
    return games;
  }
  // an older single-game app
  NSString* xbePath = [bundle stringByAppendingPathComponent:@"title/default.xbe"];
  NSData* xbe = [NSData dataWithContentsOfFile:xbePath];
  if (xbe) {
    RFGame* g = [RFGame new];
    g.gameId = Sha256Hex(xbe);
    g.name = XbeTitleName(xbe) ?: @"Game";
    g.module = [bundle stringByAppendingPathComponent:@"Frameworks/reforged_game.dylib"];
    g.titleDir = [bundle stringByAppendingPathComponent:@"title"];
    g.xbe = xbePath;
    g.folder = docs;
    g.legacy = YES;
    NSUserDefaults* ud = [NSUserDefaults standardUserDefaults];   // the disc remembered by earlier versions of the app belongs to this game
    NSData* oldBm = [ud dataForKey:@"discBookmark"];
    if (oldBm && ![ud dataForKey:[@"disc-" stringByAppendingString:g.gameId]]) [ud setObject:oldBm forKey:[@"disc-" stringByAppendingString:g.gameId]];
    g.noDisc = [fm fileExistsAtPath:[bundle stringByAppendingPathComponent:@"title/no-disc.txt"]];
    if ([fm fileExistsAtPath:g.module]) [games addObject:g];
  }
  return games;
}

+ (NSString*)bookmarkKey:(RFGame*)g { return [@"disc-" stringByAppendingString:g.gameId]; }

+ (NSString*)discForGame:(RFGame*)g documents:(NSString*)docs how:(NSString**)how {
  NSFileManager* fm = [NSFileManager defaultManager];
  if (g.noDisc) { if (how) *how = @"no disc needed"; return @"none"; }
  // 1. a file the user chose in the Files picker (opened in place)
  NSData* bm = [[NSUserDefaults standardUserDefaults] dataForKey:[self bookmarkKey:g]];
  if (bm) {
    BOOL stale = NO; NSURL* url = [NSURL URLByResolvingBookmarkData:bm options:0 relativeToURL:nil bookmarkDataIsStale:&stale error:nil];
    if (url && [url startAccessingSecurityScopedResource] && [fm fileExistsAtPath:url.path]) { if (how) *how = [NSString stringWithFormat:@"chosen file: %@", url.lastPathComponent]; return url.path; }
  }
  // 2. a copy kept in the game's own folder
  NSString* copy = [g.folder stringByAppendingPathComponent:@"disc.iso"];
  if ([fm fileExistsAtPath:copy]) { if (how) *how = @"copy inside the app"; return copy; }
  // 3. an image in the app's Documents folder (Files app / Finder file sharing) whose main executable is this game's
  NSUserDefaults* ud = [NSUserDefaults standardUserDefaults];
  NSString* onlyImage = nil; int images = 0;
  for (NSString* name in [[fm contentsOfDirectoryAtPath:docs error:nil] sortedArrayUsingSelector:@selector(compare:)]) {
    NSString* ext = name.pathExtension.lowercaseString;
    if (!([ext isEqualToString:@"iso"] || [ext isEqualToString:@"xiso"])) continue;
    NSString* path = [docs stringByAppendingPathComponent:name];
    NSDictionary* attr = [fm attributesOfItemAtPath:path error:nil];
    NSString* key = [NSString stringWithFormat:@"iso:%@:%llu:%.0f", name, [attr fileSize], [[attr fileModificationDate] timeIntervalSince1970]];
    NSString* id_ = [ud stringForKey:key];
    if (!id_) { NSData* x = DiscExecutable(path); id_ = x ? Sha256Hex(x) : @"unreadable"; [ud setObject:id_ forKey:key]; }
    if ([id_ isEqualToString:g.gameId]) { if (how) *how = [NSString stringWithFormat:@"found in the app folder: %@", name]; return path; }
    onlyImage = path; images++;
  }
  // 4. an older single-game app with a disc image it cannot identify: use it as before
  if (g.legacy && images == 1 && onlyImage) { if (how) *how = [NSString stringWithFormat:@"found in the app folder: %@", onlyImage.lastPathComponent]; return onlyImage; }
  if (how) *how = @"no disc image yet";
  return nil;
}

+ (NSString*)problemWithDisc:(NSString*)path forGame:(RFGame*)g {
  if (g.noDisc) return nil;
  NSData* x = DiscExecutable(path);
  if (!x) return @"That file is not a disc image this app can read (it should be an .iso of the original disc).";
  if (![Sha256Hex(x) isEqualToString:g.gameId]) return [NSString stringWithFormat:@"That disc image is not the one %@ was built from (a different game, or a different version of it).", g.name];
  return nil;
}

+ (void)rememberBookmark:(NSData*)bookmark forGame:(RFGame*)g { [[NSUserDefaults standardUserDefaults] setObject:bookmark forKey:[self bookmarkKey:g]]; }

+ (void)forgetDiscForGame:(RFGame*)g {
  [[NSUserDefaults standardUserDefaults] removeObjectForKey:[self bookmarkKey:g]];
  [[NSFileManager defaultManager] removeItemAtPath:[g.folder stringByAppendingPathComponent:@"disc.iso"] error:nil];
}

+ (void)prepareFolderForGame:(RFGame*)g documents:(NSString*)docs {
  if (g.legacy) return;
  NSFileManager* fm = [NSFileManager defaultManager];
  [fm createDirectoryAtPath:g.folder withIntermediateDirectories:YES attributes:nil error:nil];
  if (!g.migrateOldState) return;
  // Files an older single-game app left in Documents (virtual disk, console identity, shader cache) belong to this game: move them in once.
  NSString* oldWork = [docs stringByAppendingPathComponent:@"work"];
  NSString* newWork = [g.folder stringByAppendingPathComponent:@"work"];
  if ([fm fileExistsAtPath:oldWork] && ![fm fileExistsAtPath:newWork]) [fm moveItemAtPath:oldWork toPath:newWork error:nil];
  NSString* oldCache = [docs stringByAppendingPathComponent:@"pipeline_cache.bin"];
  NSString* newCache = [g.folder stringByAppendingPathComponent:@"pipeline_cache.bin"];
  if ([fm fileExistsAtPath:oldCache] && ![fm fileExistsAtPath:newCache]) [fm moveItemAtPath:oldCache toPath:newCache error:nil];
}

@end

@implementation RFSettings

// The project's own relay: preset in every copy of the app so System Link over the Internet needs only the switch (it is off until the user turns it on).
static NSString* const kDefaultRelayServer = @"relay.puffsco.com:3075";
static NSDictionary* gBundleRelay;   // {server, room, enabled} from Games/games.json, or nil
+ (void)loadBundleDefaults:(NSString*)bundle {
  NSData* data = [NSData dataWithContentsOfFile:[bundle stringByAppendingPathComponent:@"Games/games.json"]]; if (!data) return;
  NSDictionary* root = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
  NSDictionary* r = [root isKindOfClass:[NSDictionary class]] ? root[@"relay"] : nil;
  if ([r isKindOfClass:[NSDictionary class]] && [r[@"server"] isKindOfClass:[NSString class]] && [r[@"server"] length]) gBundleRelay = r;
}

+ (NSInteger)touchMode { return [[NSUserDefaults standardUserDefaults] integerForKey:@"touchMode"]; }
+ (void)setTouchMode:(NSInteger)mode { [[NSUserDefaults standardUserDefaults] setInteger:mode forKey:@"touchMode"]; [self applyToEnvironment]; }
+ (NSInteger)renderScale { NSInteger v = [[NSUserDefaults standardUserDefaults] integerForKey:@"renderScale"]; return v < 1 ? 1 : (v > 3 ? 3 : v); }
+ (void)setRenderScale:(NSInteger)scale { [[NSUserDefaults standardUserDefaults] setInteger:scale forKey:@"renderScale"]; [self applyToEnvironment]; }
+ (BOOL)showFps { return [[NSUserDefaults standardUserDefaults] boolForKey:@"showFps"]; }
+ (void)setShowFps:(BOOL)on { [[NSUserDefaults standardUserDefaults] setBool:on forKey:@"showFps"]; }
+ (BOOL)relayEnabled { NSUserDefaults* ud = [NSUserDefaults standardUserDefaults]; if ([ud objectForKey:@"relayEnabled"]) return [ud boolForKey:@"relayEnabled"]; return gBundleRelay ? [gBundleRelay[@"enabled"] boolValue] : NO; }
+ (void)setRelayEnabled:(BOOL)on { [[NSUserDefaults standardUserDefaults] setBool:on forKey:@"relayEnabled"]; [self applyToEnvironment]; }
+ (NSString*)relayServer { NSString* v = [[NSUserDefaults standardUserDefaults] stringForKey:@"relayServer"]; return v.length ? v : (gBundleRelay[@"server"] ?: kDefaultRelayServer); }
+ (void)setRelayServer:(NSString*)server { [[NSUserDefaults standardUserDefaults] setObject:[server stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]] forKey:@"relayServer"]; [self applyToEnvironment]; }
+ (NSString*)relayRoom { NSString* v = [[NSUserDefaults standardUserDefaults] stringForKey:@"relayRoom"]; return v.length ? v : ([gBundleRelay[@"room"] isKindOfClass:[NSString class]] ? gBundleRelay[@"room"] : @""); }
+ (void)setRelayRoom:(NSString*)room { [[NSUserDefaults standardUserDefaults] setObject:[room stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]] forKey:@"relayRoom"]; [self applyToEnvironment]; }
+ (BOOL)relayHosting { return [[NSUserDefaults standardUserDefaults] boolForKey:@"relayHosting"]; }
+ (void)setRelayHosting:(BOOL)on { [[NSUserDefaults standardUserDefaults] setBool:on forKey:@"relayHosting"]; [self applyToEnvironment]; }
+ (NSString*)addressesToShare {
  NSMutableArray* lines = [NSMutableArray array]; struct ifaddrs* list = NULL;
  if (getifaddrs(&list) == 0) {
    for (struct ifaddrs* a = list; a; a = a->ifa_next) {
      if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || !(a->ifa_flags & IFF_UP) || (a->ifa_flags & IFF_LOOPBACK)) continue;
      char ip[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &((struct sockaddr_in*)a->ifa_addr)->sin_addr, ip, sizeof ip);
      NSString* name = [NSString stringWithUTF8String:a->ifa_name]; NSString* kind = [name hasPrefix:@"en"] ? @"Wi-Fi" : ([name hasPrefix:@"utun"] && !strncmp(ip, "100.", 4) ? @"Tailscale" : nil);
      if (kind) [lines addObject:[NSString stringWithFormat:@"%@: %s:3075", kind, ip]];
    }
    freeifaddrs(list);
  }
  return lines.count ? [lines componentsJoinedByString:@"\n"] : @"no network address found (connect to Wi-Fi)";
}
+ (BOOL)diagnostics { return [[NSUserDefaults standardUserDefaults] boolForKey:@"diagnostics"]; }
+ (void)setDiagnostics:(BOOL)on { [[NSUserDefaults standardUserDefaults] setBool:on forKey:@"diagnostics"]; [self applyToEnvironment]; }

+ (void)applyToEnvironment {
  NSInteger t = [self touchMode];
  if (t == 1) setenv("REFORGED_TOUCH", "on", 1); else if (t == 2) setenv("REFORGED_TOUCH", "off", 1); else unsetenv("REFORGED_TOUCH");
  if ([self renderScale] > 1) setenv("XBOX_RES_SCALE", [[NSString stringWithFormat:@"%ld", (long)[self renderScale]] UTF8String], 1); else unsetenv("XBOX_RES_SCALE");
  if ([self relayHosting]) setenv("XBOX_RELAY_HOST", "3075", 1); else unsetenv("XBOX_RELAY_HOST");
  if ([self relayEnabled] && [self relayServer].length) {
    setenv("XBOX_RELAY", [[self relayServer] UTF8String], 1);
    if ([self relayRoom].length) setenv("XBOX_RELAY_ROOM", [[self relayRoom] UTF8String], 1); else unsetenv("XBOX_RELAY_ROOM");
  } else { unsetenv("XBOX_RELAY"); unsetenv("XBOX_RELAY_ROOM"); }
  if ([self diagnostics]) {
    // every 2400 frames, and whenever the picture suddenly goes black: log two frames draw by draw and save their images
    setenv("XBOX_DRAWLOG", "every=2400", 1); setenv("XBOX_FRAMEDUMP", "sync", 1); setenv("XBOX_DARKWATCH", "1", 1); setenv("XBOX_DRAWTEX", "size:103x19", 1);
    setenv("XBOX_CRYPTO_SELFTEST", "1", 1); setenv("XBOX_NET_LOG", "1", 1); setenv("XBOX_NET_PCAP", "net.pcap", 1);   // network and crypto checks for System Link problems
  } else {
    unsetenv("XBOX_DRAWLOG"); unsetenv("XBOX_FRAMEDUMP"); unsetenv("XBOX_DARKWATCH"); unsetenv("XBOX_DRAWTEX"); unsetenv("XBOX_CRYPTO_SELFTEST"); unsetenv("XBOX_NET_LOG"); unsetenv("XBOX_NET_PCAP");
  }
}

@end
