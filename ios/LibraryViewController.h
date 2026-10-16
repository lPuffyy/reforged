#import <UIKit/UIKit.h>
#import "GameLibrary.h"

@protocol RFLibraryDelegate <NSObject>
- (void)libraryWantsToPlay:(RFGame*)game disc:(NSString*)disc;
- (void)settingsChanged;
@end

// The screen shown at launch: the installed games and the disc image each one uses. Tapping a game starts it (or asks for its disc image first).
@interface LibraryViewController : UITableViewController <UIDocumentPickerDelegate>
@property(weak, nonatomic) id<RFLibraryDelegate> delegate;
- (instancetype)initWithGames:(NSArray<RFGame*>*)games documents:(NSString*)docs;
@end

@interface SettingsViewController : UITableViewController
@property(weak, nonatomic) id<RFLibraryDelegate> delegate;
@end
