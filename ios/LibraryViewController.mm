#import "LibraryViewController.h"
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

@implementation LibraryViewController {
  NSArray<RFGame*>* _games;
  NSString* _docs;
  RFGame* _pickingFor;
  BOOL _pickingCopy;
}

- (instancetype)initWithGames:(NSArray<RFGame*>*)games documents:(NSString*)docs {
  if ((self = [super initWithStyle:UITableViewStyleInsetGrouped])) { _games = games; _docs = docs; self.title = @"Reforged"; }
  return self;
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"Settings" style:UIBarButtonItemStylePlain target:self action:@selector(openSettings)];
}

- (void)viewWillAppear:(BOOL)animated { [super viewWillAppear:animated]; [self.tableView reloadData]; }

- (void)openSettings {
  SettingsViewController* s = [[SettingsViewController alloc] initWithStyle:UITableViewStyleInsetGrouped];
  s.delegate = self.delegate; s.title = @"Settings";
  s.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone target:self action:@selector(closeSettings)];
  UINavigationController* nav = [[UINavigationController alloc] initWithRootViewController:s];
  [self presentViewController:nav animated:YES completion:nil];
}
- (void)closeSettings { [self dismissViewControllerAnimated:YES completion:nil]; }

// ---- table ----
- (NSInteger)numberOfSectionsInTableView:(UITableView*)tv { return 1; }
- (NSInteger)tableView:(UITableView*)tv numberOfRowsInSection:(NSInteger)s { return (NSInteger)_games.count; }
- (NSString*)tableView:(UITableView*)tv titleForHeaderInSection:(NSInteger)s { return _games.count == 1 ? @"Installed game" : @"Installed games"; }
- (NSString*)tableView:(UITableView*)tv titleForFooterInSection:(NSInteger)s {
  return @"Tap a game to play it. The disc image is not part of the app: choose it once with the (i) button, or copy the .iso into this app's folder with the Files app (On My iPhone > Reforged). To play a different game, close the app and open it again.";
}

- (UITableViewCell*)tableView:(UITableView*)tv cellForRowAtIndexPath:(NSIndexPath*)ip {
  UITableViewCell* c = [tv dequeueReusableCellWithIdentifier:@"game"];
  if (!c) c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"game"];
  RFGame* g = _games[ip.row]; NSString* how = nil;
  NSString* disc = [RFLibrary discForGame:g documents:_docs how:&how];
  c.textLabel.text = g.name;
  c.detailTextLabel.numberOfLines = 0;
  NSString* line2 = how ?: @"";
  c.detailTextLabel.text = g.details.length ? [NSString stringWithFormat:@"%@\n%@", g.details, line2] : line2;
  c.detailTextLabel.textColor = disc ? [UIColor secondaryLabelColor] : [UIColor systemOrangeColor];
  UIImage* icon = g.iconPath ? [UIImage imageWithContentsOfFile:g.iconPath] : nil;
  if (icon) {   // the game's own icon, shown 56 points wide with rounded corners
    CGSize sz = CGSizeMake(56, 56);
    c.imageView.image = [[[UIGraphicsImageRenderer alloc] initWithSize:sz] imageWithActions:^(UIGraphicsImageRendererContext* ctx) { [icon drawInRect:CGRectMake(0, 0, sz.width, sz.height)]; }];
    c.imageView.layer.cornerRadius = 10; c.imageView.clipsToBounds = YES;
  } else c.imageView.image = nil;
  c.accessoryType = UITableViewCellAccessoryDetailButton;
  return c;
}

- (void)tableView:(UITableView*)tv didSelectRowAtIndexPath:(NSIndexPath*)ip {
  [tv deselectRowAtIndexPath:ip animated:YES];
  RFGame* g = _games[ip.row];
  NSString* disc = [RFLibrary discForGame:g documents:_docs how:nil];
  if (disc) [self.delegate libraryWantsToPlay:g disc:disc]; else [self askForDiscFor:g];
}

- (void)tableView:(UITableView*)tv accessoryButtonTappedForRowWithIndexPath:(NSIndexPath*)ip {
  RFGame* g = _games[ip.row];
  UIAlertController* a = [UIAlertController alertControllerWithTitle:g.name message:nil preferredStyle:UIAlertControllerStyleActionSheet];
  [a addAction:[UIAlertAction actionWithTitle:@"Choose a disc image (open in place)" style:UIAlertActionStyleDefault handler:^(UIAlertAction* act) { [self pickDiscFor:g copy:NO]; }]];
  [a addAction:[UIAlertAction actionWithTitle:@"Choose a disc image (copy into the app)" style:UIAlertActionStyleDefault handler:^(UIAlertAction* act) { [self pickDiscFor:g copy:YES]; }]];
  [a addAction:[UIAlertAction actionWithTitle:@"Forget the chosen disc image" style:UIAlertActionStyleDestructive handler:^(UIAlertAction* act) { [RFLibrary forgetDiscForGame:g]; [self.tableView reloadData]; }]];
  [a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
  UITableViewCell* cell = [tv cellForRowAtIndexPath:ip];
  a.popoverPresentationController.sourceView = cell; a.popoverPresentationController.sourceRect = cell.bounds;
  [self presentViewController:a animated:YES completion:nil];
}

- (void)askForDiscFor:(RFGame*)g {
  UIAlertController* a = [UIAlertController alertControllerWithTitle:@"Choose the disc image"
      message:@"Open in place uses the file where it is (nothing is copied). If the file is greyed out in the picker, use \"Copy into the app\" (it needs free space for a second copy), or copy the .iso into this app's folder with the Files app."
      preferredStyle:UIAlertControllerStyleAlert];
  [a addAction:[UIAlertAction actionWithTitle:@"Open in place" style:UIAlertActionStyleDefault handler:^(UIAlertAction* act) { [self pickDiscFor:g copy:NO]; }]];
  [a addAction:[UIAlertAction actionWithTitle:@"Copy into the app" style:UIAlertActionStyleDefault handler:^(UIAlertAction* act) { [self pickDiscFor:g copy:YES]; }]];
  [a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
  [self presentViewController:a animated:YES completion:nil];
}

// ---- the Files picker ----
- (void)pickDiscFor:(RFGame*)g copy:(BOOL)copy {
  _pickingFor = g; _pickingCopy = copy;
  NSMutableArray<UTType*>* types = [NSMutableArray arrayWithObjects:UTTypeItem, UTTypeData, UTTypeDiskImage, nil];
  for (NSString* ext in @[@"iso", @"xiso"]) { UTType* t = [UTType typeWithFilenameExtension:ext]; if (t) [types addObject:t]; }
  UIDocumentPickerViewController* picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:types asCopy:copy];
  picker.delegate = self;
  [self presentViewController:picker animated:YES completion:nil];
}

- (void)problem:(NSString*)text {
  UIAlertController* a = [UIAlertController alertControllerWithTitle:@"That did not work" message:text preferredStyle:UIAlertControllerStyleAlert];
  [a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
  [self presentViewController:a animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
  NSURL* url = urls.firstObject; RFGame* g = _pickingFor; if (!url || !g) return;
  NSFileManager* fm = [NSFileManager defaultManager];
  if (_pickingCopy) {
    // the picker already copied the file into this app's temporary folder: check it, then move it into the game's folder
    NSString* bad = [RFLibrary problemWithDisc:url.path forGame:g];
    if (bad) { [fm removeItemAtURL:url error:nil]; [self problem:bad]; return; }
    [fm createDirectoryAtPath:g.folder withIntermediateDirectories:YES attributes:nil error:nil];
    NSString* dest = [g.folder stringByAppendingPathComponent:@"disc.iso"]; [fm removeItemAtPath:dest error:nil];
    NSError* err = nil;
    if (![fm moveItemAtPath:url.path toPath:dest error:&err] && ![fm copyItemAtPath:url.path toPath:dest error:&err]) { [self problem:[NSString stringWithFormat:@"Could not keep the copy: %@", err.localizedDescription]]; return; }
    [fm removeItemAtURL:url error:nil];
    [self.tableView reloadData]; return;
  }
  BOOL ok = [url startAccessingSecurityScopedResource];   // stays open: the runtime reads the file in place while the game runs
  if (!ok) { [self problem:[NSString stringWithFormat:@"iOS did not give this app access to \"%@\". Try \"Copy into the app\" instead, or copy the file into this app's folder with the Files app.", url.lastPathComponent]]; return; }
  NSString* bad = [RFLibrary problemWithDisc:url.path forGame:g];
  if (bad) { [url stopAccessingSecurityScopedResource]; [self problem:bad]; return; }
  NSError* err = nil;
  NSData* bm = [url bookmarkDataWithOptions:0 includingResourceValuesForKeys:nil relativeToURL:nil error:&err];
  if (!bm) { [url stopAccessingSecurityScopedResource]; [self problem:[NSString stringWithFormat:@"The file can be read but iOS would not remember it (%@). Use \"Copy into the app\" instead.", err.localizedDescription]]; return; }
  [RFLibrary rememberBookmark:bm forGame:g];
  [self.tableView reloadData];
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller {}

@end

// ---- settings ----
// Sections: 0 on-screen controls, 1 display, 2 diagnostics, 3 about
@implementation SettingsViewController

- (void)relayChanged:(UISwitch*)sw { [RFSettings setRelayEnabled:sw.on]; [self.delegate settingsChanged]; }
- (BOOL)textFieldShouldReturn:(UITextField*)tf { [tf resignFirstResponder]; return YES; }
- (void)textFieldDidEndEditing:(UITextField*)tf {
  if (tf.tag == 1) [RFSettings setRelayServer:tf.text ?: @""]; else if (tf.tag == 2) [RFSettings setRelayRoom:tf.text ?: @""];
  [self.delegate settingsChanged];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView*)tv { return 5; }
- (NSInteger)tableView:(UITableView*)tv numberOfRowsInSection:(NSInteger)s { return s == 0 ? 3 : (s == 1 ? 4 : (s == 2 ? 3 : (s == 3 ? 1 : 2))); }
- (NSString*)tableView:(UITableView*)tv titleForHeaderInSection:(NSInteger)s { return @[@"On-screen controls", @"Display", @"System Link over the Internet", @"Diagnostics", @"About"][s]; }
- (NSString*)tableView:(UITableView*)tv titleForFooterInSection:(NSInteger)s {
  if (s == 0) return @"Automatic hides the on-screen controls while a physical controller is connected and shows them again when it is gone.";
  if (s == 1) return @"Render resolution draws the game at a multiple of its original size and scales it down, which smooths jagged edges (it works like supersampling antialiasing) but uses more battery and can lower the frame rate; changes apply the next time a game starts. The frame counter appears in the top left corner while a game runs.";
  if (s == 2) return @"Connects to a relay server so System Link games work over the Internet. Everyone who plays together enters the same server address and the same room name, then uses the game's own System Link menu. Nothing is encrypted: use a private room name and play with people you trust. Applies the next time a game starts.";
  if (s == 3) return @"Saves a few frame images and a detailed log now and then (and whenever the picture suddenly goes black). Turn it on when you are asked to report a problem; the files appear in this app's folder.";
  return nil;
}

- (UITableViewCell*)tableView:(UITableView*)tv cellForRowAtIndexPath:(NSIndexPath*)ip {
  UITableViewCell* c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1 reuseIdentifier:nil];
  if (ip.section == 0) {
    NSArray* names = @[@"Automatic", @"Always show", @"Always hide"];
    c.textLabel.text = names[ip.row];
    c.accessoryType = [RFSettings touchMode] == ip.row ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
  } else if (ip.section == 1 && ip.row == 0) {
    c.textLabel.text = @"Show frame rate";
    UISwitch* sw = [UISwitch new]; sw.on = [RFSettings showFps]; [sw addTarget:self action:@selector(fpsChanged:) forControlEvents:UIControlEventValueChanged];
    c.accessoryView = sw; c.selectionStyle = UITableViewCellSelectionStyleNone;
  } else if (ip.section == 1) {
    NSArray* names = @[@"Original resolution (1x)", @"2x (smoother edges)", @"3x (smoothest, heaviest)"];
    c.textLabel.text = names[ip.row - 1];
    c.accessoryType = [RFSettings renderScale] == ip.row ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
  } else if (ip.section == 2) {
    if (ip.row == 0) {
      c.textLabel.text = @"Use a relay server";
      UISwitch* sw = [UISwitch new]; sw.on = [RFSettings relayEnabled]; [sw addTarget:self action:@selector(relayChanged:) forControlEvents:UIControlEventValueChanged];
      c.accessoryView = sw;
    } else {
      BOOL server = ip.row == 1;
      c.textLabel.text = server ? @"Server" : @"Room";
      UITextField* tf = [[UITextField alloc] initWithFrame:CGRectMake(0, 0, 260, 32)];
      tf.textAlignment = NSTextAlignmentRight; tf.autocapitalizationType = UITextAutocapitalizationTypeNone; tf.autocorrectionType = UITextAutocorrectionTypeNo;
      tf.placeholder = server ? @"relay.example.com:3075" : @"a private room name"; tf.text = server ? [RFSettings relayServer] : [RFSettings relayRoom];
      tf.tag = server ? 1 : 2; tf.returnKeyType = UIReturnKeyDone; tf.delegate = self; tf.keyboardType = server ? UIKeyboardTypeURL : UIKeyboardTypeDefault;
      c.accessoryView = tf;
    }
    c.selectionStyle = UITableViewCellSelectionStyleNone;
  } else if (ip.section == 3) {
    c.textLabel.text = @"Save diagnostic captures";
    UISwitch* sw = [UISwitch new]; sw.on = [RFSettings diagnostics]; [sw addTarget:self action:@selector(diagChanged:) forControlEvents:UIControlEventValueChanged];
    c.accessoryView = sw; c.selectionStyle = UITableViewCellSelectionStyleNone;
  } else if (ip.row == 0) {
    c.textLabel.text = @"Version"; c.detailTextLabel.text = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"";
    c.selectionStyle = UITableViewCellSelectionStyleNone;
  } else {
    c.textLabel.text = @"App folder"; c.detailTextLabel.text = @"Files > On My iPhone > Reforged";
    c.selectionStyle = UITableViewCellSelectionStyleNone;
  }
  return c;
}

- (void)tableView:(UITableView*)tv didSelectRowAtIndexPath:(NSIndexPath*)ip {
  [tv deselectRowAtIndexPath:ip animated:YES];
  if (ip.section == 0) {
    [RFSettings setTouchMode:ip.row];
    [tv reloadSections:[NSIndexSet indexSetWithIndex:0] withRowAnimation:UITableViewRowAnimationNone];
    [self.delegate settingsChanged];
  } else if (ip.section == 1 && ip.row >= 1) {
    [RFSettings setRenderScale:ip.row];
    [tv reloadSections:[NSIndexSet indexSetWithIndex:1] withRowAnimation:UITableViewRowAnimationNone];
    [self.delegate settingsChanged];
  }
}

- (void)fpsChanged:(UISwitch*)sw { [RFSettings setShowFps:sw.on]; [self.delegate settingsChanged]; }
- (void)diagChanged:(UISwitch*)sw { [RFSettings setDiagnostics:sw.on]; [self.delegate settingsChanged]; }

@end
