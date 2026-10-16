#import <UIKit/UIKit.h>

// The on-screen controller: two sticks, four face buttons, two triggers, and start/back. Multi-touch; reports to the runtime with InputSetHostPad().
@interface TouchControls : UIView
- (void)controllersChanged;   // re-check the connected controllers and the touch-controls setting (REFORGED_TOUCH)
@end
