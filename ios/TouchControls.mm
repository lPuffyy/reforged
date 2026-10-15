#import "TouchControls.h"
#include "input.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include <map>

namespace {
enum Kind { kStickL, kStickR, kA, kB, kX, kY, kLT, kRT, kStart, kBack };
struct Control { Kind kind; CGPoint center; CGFloat radius; };
}

@implementation TouchControls {
  std::vector<Control> _controls;
  std::map<void*, int> _touchControl;       // touch -> index into _controls
  std::map<void*, CGPoint> _touchStart;     // stick touches: where the drag began
  NSMutableArray<CAShapeLayer*>* _layers;
  HostPad _pad;
}

- (instancetype)initWithFrame:(CGRect)frame {
  if ((self = [super initWithFrame:frame])) {
    self.multipleTouchEnabled = YES;
    self.backgroundColor = [UIColor clearColor];
    _layers = [NSMutableArray array];
    memset(&_pad, 0, sizeof _pad);
  }
  return self;
}

- (void)layoutSubviews {
  [super layoutSubviews];
  CGFloat w = self.bounds.size.width, h = self.bounds.size.height, s = std::min(w, h) / 380.f;   // sizes scale with the screen
  CGFloat r = 55 * s, b = 30 * s;
  _controls = {
    {kStickL, {110 * s + 40, h - 110 * s - 20}, r * 1.3},
    {kStickR, {w - 110 * s - 40, h - 80 * s - 20}, r * 1.3},
    {kA, {w - 70 * s - 20, h - 190 * s}, b}, {kB, {w - 20 * s - 20, h - 235 * s}, b},
    {kX, {w - 120 * s - 20, h - 235 * s}, b}, {kY, {w - 70 * s - 20, h - 280 * s}, b},
    {kLT, {70 * s + 20, 40 * s}, b * 1.3}, {kRT, {w - 70 * s - 20, 40 * s}, b * 1.3},
    {kBack, {w / 2 - 50 * s, h - 40 * s}, b * 0.7}, {kStart, {w / 2 + 50 * s, h - 40 * s}, b * 0.7},
  };
  for (CAShapeLayer* l in _layers) [l removeFromSuperlayer];
  [_layers removeAllObjects];
  for (const Control& c : _controls) {
    CAShapeLayer* l = [CAShapeLayer layer];
    l.path = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(c.center.x - c.radius, c.center.y - c.radius, 2 * c.radius, 2 * c.radius)].CGPath;
    l.fillColor = [UIColor colorWithWhite:1.0 alpha:0.12].CGColor;
    l.strokeColor = [UIColor colorWithWhite:1.0 alpha:0.35].CGColor;
    l.lineWidth = 2;
    [self.layer addSublayer:l];
    [_layers addObject:l];
  }
}

- (UIView*)hitTest:(CGPoint)point withEvent:(UIEvent*)event {
  for (const Control& c : _controls) if (hypot(point.x - c.center.x, point.y - c.center.y) <= c.radius * 1.5) return self;   // let the picture underneath get everything else
  return nil;
}

- (int)controlAt:(CGPoint)p {
  int best = -1; CGFloat bestD = 1e9;
  for (size_t i = 0; i < _controls.size(); i++) {
    CGFloat d = hypot(p.x - _controls[i].center.x, p.y - _controls[i].center.y);
    if (d <= _controls[i].radius * 1.5 && d < bestD) { best = (int)i; bestD = d; }
  }
  return best;
}

- (void)publish {
  InputSetHostPad(_pad);
  for (size_t i = 0; i < _controls.size(); i++) {
    bool down = false;
    for (auto& kv : _touchControl) if (kv.second == (int)i) down = true;
    _layers[i].fillColor = [UIColor colorWithWhite:1.0 alpha:down ? 0.35 : 0.12].CGColor;
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
