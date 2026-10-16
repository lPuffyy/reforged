#import "TouchControls.h"
#import <GameController/GameController.h>
#include "input.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include <map>

namespace {
enum Kind { kStickL, kStickR, kA, kB, kX, kY, kLT, kRT, kStart, kBack, kBlack, kWhite, kLThumb, kRThumb };
struct Control { Kind kind; CGPoint center; CGFloat radius; const char* label; };
}

@implementation TouchControls {
  std::vector<Control> _controls;
  std::map<void*, int> _touchControl;       // touch -> index into _controls
  std::map<void*, CGPoint> _touchStart;     // stick touches: where the drag began
  NSMutableArray<CAShapeLayer*>* _layers;
  NSMutableArray<CALayer*>* _extras;        // labels and stick knobs
  std::map<size_t, CAShapeLayer*> _knobs;   // stick control index -> knob
  HostPad _pad;
}

- (instancetype)initWithFrame:(CGRect)frame {
  if ((self = [super initWithFrame:frame])) {
    self.multipleTouchEnabled = YES;
    self.backgroundColor = [UIColor clearColor];
    _layers = [NSMutableArray array]; _extras = [NSMutableArray array];
    memset(&_pad, 0, sizeof _pad);
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(controllersChanged) name:GCControllerDidConnectNotification object:nil];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(controllersChanged) name:GCControllerDidDisconnectNotification object:nil];
    [self controllersChanged];
  }
  return self;
}

// The on-screen controls hide while a physical controller is connected and come back when it is gone. REFORGED_TOUCH=on keeps them always, =off hides them always.
- (void)controllersChanged {
  dispatch_async(dispatch_get_main_queue(), ^{
    const char* mode = getenv("REFORGED_TOUCH"); bool force = mode && !strcmp(mode, "on"), never = mode && !strcmp(mode, "off");
    bool pad = false; for (GCController* c in [GCController controllers]) if (c.extendedGamepad) { pad = true; break; }
    bool hide = never || (pad && !force);
    if (hide && !self.hidden) { self->_touchControl.clear(); self->_touchStart.clear(); memset(&self->_pad, 0, sizeof self->_pad); InputSetHostPad(self->_pad); }   // release anything that was held
    self.hidden = hide;
  });
}

- (void)layoutSubviews {
  [super layoutSubviews];
  CGFloat w = self.bounds.size.width, h = self.bounds.size.height, u = std::min(w, h) / 393.f;   // sizes scale with the screen
  UIEdgeInsets ins = self.safeAreaInsets;                                                      // keep clear of the notch and rounded corners
  CGFloat ml = std::max(26 * u, ins.left + 14), mr = std::max(26 * u, ins.right + 14), mb = std::max(22 * u, ins.bottom + 8), mt = std::max(14 * u, ins.top + 8);
  CGFloat rs = 58 * u, rb = 26 * u, sp = 56 * u;                                              // stick radius, button radius, face button spacing
  CGPoint L = {ml + rs + 6 * u, h - mb - rs}, R = {w - mr - rs - 6 * u, h - mb - rs}, F = {w - mr - 108 * u, h * 0.40f};
  _controls = {
    {kStickL, L, rs, ""}, {kStickR, R, rs, ""},
    {kY, {F.x, F.y - sp}, rb, "Y"}, {kA, {F.x, F.y + sp}, rb, "A"}, {kX, {F.x - sp, F.y}, rb, "X"}, {kB, {F.x + sp, F.y}, rb, "B"},
    {kLT, {ml + 34 * u, mt + 28 * u}, 28 * u, "LT"}, {kRT, {w - mr - 34 * u, mt + 28 * u}, 28 * u, "RT"},
    {kWhite, {ml + 34 * u, 100 * u}, 22 * u, "WHT"}, {kBlack, {w - mr - 34 * u, 100 * u}, 22 * u, "BLK"},
    {kLThumb, {L.x + rs + 28 * u, L.y - 20 * u}, 22 * u, "L3"}, {kRThumb, {R.x - rs - 28 * u, R.y - 20 * u}, 26 * u, "R3"},
    {kBack, {w / 2 - 52 * u, h - mb - 14 * u}, 20 * u, "BACK"}, {kStart, {w / 2 + 52 * u, h - mb - 14 * u}, 20 * u, "START"},
  };
  for (CAShapeLayer* l in _layers) [l removeFromSuperlayer];
  for (CALayer* l in _extras) [l removeFromSuperlayer];
  [_layers removeAllObjects]; [_extras removeAllObjects]; _knobs.clear();
  CGFloat scale = UIScreen.mainScreen.scale;
  for (size_t i = 0; i < _controls.size(); i++) {
    const Control& c = _controls[i];
    CAShapeLayer* l = [CAShapeLayer layer];
    l.path = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(c.center.x - c.radius, c.center.y - c.radius, 2 * c.radius, 2 * c.radius)].CGPath;
    l.fillColor = [UIColor colorWithWhite:1.0 alpha:0.10].CGColor;
    l.strokeColor = [UIColor colorWithWhite:1.0 alpha:0.38].CGColor;
    l.lineWidth = 2;
    [self.layer addSublayer:l]; [_layers addObject:l];
    bool stick = c.kind == kStickL || c.kind == kStickR;
    if (stick) {   // a knob that follows the thumb
      CAShapeLayer* k = [CAShapeLayer layer]; CGFloat kr = c.radius * 0.42;
      k.path = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(-kr, -kr, 2 * kr, 2 * kr)].CGPath;
      k.position = c.center; k.fillColor = [UIColor colorWithWhite:1.0 alpha:0.22].CGColor; k.strokeColor = [UIColor colorWithWhite:1.0 alpha:0.45].CGColor; k.lineWidth = 1.5;
      [self.layer addSublayer:k]; [_extras addObject:k]; _knobs[i] = k;
    } else if (c.label[0]) {
      CATextLayer* t = [CATextLayer layer]; CGFloat fs = (strlen(c.label) > 2 ? 11.0 : 16.0) * u * (c.radius > 24 * u ? 1.0 : 0.9);
      t.string = [NSString stringWithUTF8String:c.label]; t.fontSize = fs; t.font = (__bridge CFTypeRef)@"Helvetica-Bold"; t.alignmentMode = kCAAlignmentCenter;
      t.foregroundColor = [UIColor colorWithWhite:1.0 alpha:0.62].CGColor; t.contentsScale = scale;
      t.frame = CGRectMake(c.center.x - c.radius, c.center.y - fs * 0.62, 2 * c.radius, fs * 1.3);
      [self.layer addSublayer:t]; [_extras addObject:t];
    }
  }
}

- (UIView*)hitTest:(CGPoint)point withEvent:(UIEvent*)event {
  for (const Control& c : _controls) if (hypot(point.x - c.center.x, point.y - c.center.y) <= c.radius * ((c.kind == kStickL || c.kind == kStickR) ? 1.35 : 1.2)) return self;   // let the picture underneath get everything else
  return nil;
}

- (int)controlAt:(CGPoint)p {
  int best = -1; CGFloat bestD = 1e9;
  for (size_t i = 0; i < _controls.size(); i++) {
    CGFloat d = hypot(p.x - _controls[i].center.x, p.y - _controls[i].center.y);
    bool stk = _controls[i].kind == kStickL || _controls[i].kind == kStickR;
    if (d <= _controls[i].radius * (stk ? 1.35 : 1.2) && d < bestD) { best = (int)i; bestD = d; }
  }
  return best;
}

- (void)publish {
  InputSetHostPad(_pad);
  for (size_t i = 0; i < _controls.size(); i++) {
    bool down = false;
    for (auto& kv : _touchControl) if (kv.second == (int)i) down = true;
    _layers[i].fillColor = [UIColor colorWithWhite:1.0 alpha:down ? 0.35 : 0.10].CGColor;
    auto k = _knobs.find(i);
    if (k != _knobs.end()) {   // move the knob with the stick value
      bool left = _controls[i].kind == kStickL; CGFloat x = (left ? _pad.lx : _pad.rx) / 32767.0, y = (left ? _pad.ly : _pad.ry) / 32767.0, r = _controls[i].radius * 0.58;
      [CATransaction begin]; [CATransaction setDisableActions:YES]; k->second.position = CGPointMake(_controls[i].center.x + x * r, _controls[i].center.y - y * r); [CATransaction commit];
    }
  }
}

- (void)recompute {
  HostPad p; memset(&p, 0, sizeof p);
  for (auto& kv : _touchControl) {
    const Control& c = _controls[kv.second];
    switch (c.kind) {
      case kA: p.analog[PADA_A] = 255; break; case kB: p.analog[PADA_B] = 255; break;
      case kX: p.analog[PADA_X] = 255; break; case kY: p.analog[PADA_Y] = 255; break;
      case kLT: p.analog[PADA_LT] = 255; break; case kRT: p.analog[PADA_RT] = 255; break;
      case kStart: p.buttons |= PAD_START; break; case kBack: p.buttons |= PAD_BACK; break;
      case kBlack: p.analog[PADA_BLACK] = 255; break; case kWhite: p.analog[PADA_WHITE] = 255; break;
      case kLThumb: p.buttons |= PAD_LTHUMB; break; case kRThumb: p.buttons |= PAD_RTHUMB; break;
      default: break;
    }
  }
  _pad.buttons = p.buttons; memcpy(_pad.analog, p.analog, sizeof _pad.analog);
  [self publish];
}

- (void)setStick:(Kind)kind from:(UITouch*)t {
  void* key = (__bridge void*)t; const Control& c = _controls[_touchControl[key]];
  CGPoint pt = [t locationInView:self];
  CGFloat dx = (pt.x - c.center.x) / c.radius, dy = -(pt.y - c.center.y) / c.radius, m = hypot(dx, dy);
  if (m > 1) { dx /= m; dy /= m; }
  int16_t x = (int16_t)(dx * 32767), y = (int16_t)(dy * 32767);
  if (kind == kStickL) { _pad.lx = x; _pad.ly = y; } else { _pad.rx = x; _pad.ry = y; }
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  for (UITouch* t in touches) {
    int i = [self controlAt:[t locationInView:self]]; if (i < 0) continue;
    _touchControl[(__bridge void*)t] = i;
    if (_controls[i].kind == kStickL || _controls[i].kind == kStickR) [self setStick:_controls[i].kind from:t];
  }
  [self recompute];
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
  for (UITouch* t in touches) {
    auto it = _touchControl.find((__bridge void*)t); if (it == _touchControl.end()) continue;
    Kind k = _controls[it->second].kind;
    if (k == kStickL || k == kStickR) [self setStick:k from:t];
  }
  [self publish];
}

- (void)endTouches:(NSSet<UITouch*>*)touches {
  for (UITouch* t in touches) {
    auto it = _touchControl.find((__bridge void*)t); if (it == _touchControl.end()) continue;
    Kind k = _controls[it->second].kind;
    if (k == kStickL) { _pad.lx = 0; _pad.ly = 0; } else if (k == kStickR) { _pad.rx = 0; _pad.ry = 0; }
    _touchControl.erase(it);
  }
  [self recompute];
}
- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event { [self endTouches:touches]; }
- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event { [self endTouches:touches]; }

@end
