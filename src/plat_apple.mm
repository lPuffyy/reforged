// Apple platforms (iOS, macOS): audio output through AudioQueue. The runtime's mixer is a pull model (plat::AudioFn fills one chunk), which is exactly what an
// AudioQueue output callback wants. If no output device is available the call reports failure and the runtime carries on silently.
#ifdef __APPLE__
#import <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#import <AVFoundation/AVFoundation.h>
#endif
#include "plat.h"

namespace {
struct AudioCtx { plat::AudioFn fn; void* user; uint32_t frames; };

void Callback(void* user, AudioQueueRef queue, AudioQueueBufferRef buf) {
  AudioCtx* c = (AudioCtx*)user;
  c->fn((int16_t*)buf->mAudioData, c->user);
  buf->mAudioDataByteSize = c->frames * 4;   // 16-bit stereo
  AudioQueueEnqueueBuffer(queue, buf, 0, nullptr);
}
}

namespace plat {
bool AudioOutStart(uint32_t rate, uint32_t frames, uint32_t buffers, AudioFn fn, void* user) {
#if TARGET_OS_IPHONE
  NSError* err = nil;
  [[AVAudioSession sharedInstance] setCategory:AVAudioSessionCategoryPlayback error:&err];   // plays with the ring switch muted, like a game
  [[AVAudioSession sharedInstance] setActive:YES error:&err];
#endif
  AudioStreamBasicDescription fmt = {};
  fmt.mSampleRate = rate; fmt.mFormatID = kAudioFormatLinearPCM; fmt.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
  fmt.mBytesPerPacket = 4; fmt.mFramesPerPacket = 1; fmt.mBytesPerFrame = 4; fmt.mChannelsPerFrame = 2; fmt.mBitsPerChannel = 16;
  AudioCtx* ctx = new AudioCtx{fn, user, frames};
  AudioQueueRef queue = nullptr;
  if (AudioQueueNewOutput(&fmt, Callback, ctx, nullptr, nullptr, 0, &queue) != noErr) { delete ctx; return false; }
  for (uint32_t i = 0; i < buffers; i++) {
    AudioQueueBufferRef buf = nullptr;
    if (AudioQueueAllocateBuffer(queue, frames * 4, &buf) != noErr) { AudioQueueDispose(queue, true); delete ctx; return false; }
    Callback(ctx, queue, buf);   // prime the queue
  }
  if (AudioQueueStart(queue, nullptr) != noErr) { AudioQueueDispose(queue, true); delete ctx; return false; }
  return true;
}
}  // namespace plat
#endif
