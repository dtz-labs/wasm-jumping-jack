// Jumping Jack - native macOS host (Cocoa + AudioToolbox) for the C game core.
#import <Cocoa/Cocoa.h>
#import <AudioToolbox/AudioToolbox.h>
#import <QuartzCore/QuartzCore.h>
#include <os/lock.h>

#include "../src/game.h"

static NSString *const kHiscoreKey = @"hiscore";

// ------------------------------------------------------------------ audio

#define SAMPLE_RATE 44100
#define RING_SIZE 16384
#define RING_MAX_FILL 4096 // cap latency at ~90 ms
#define AQ_BUFFERS 3
#define AQ_FRAMES 1024

static float gRing[RING_SIZE];
static int gRingRead, gRingCount;
static os_unfair_lock gRingLock = OS_UNFAIR_LOCK_INIT;
static AudioQueueRef gQueue;
static BOOL gMuted;

static void ringPush(const float *src, int n)
{
    os_unfair_lock_lock(&gRingLock);
    for (int i = 0; i < n; i++) {
        if (gRingCount >= RING_MAX_FILL) { // drop oldest to keep latency low
            gRingRead = (gRingRead + 1) % RING_SIZE;
            gRingCount--;
        }
        gRing[(gRingRead + gRingCount) % RING_SIZE] = gMuted ? 0.0f : src[i];
        gRingCount++;
    }
    os_unfair_lock_unlock(&gRingLock);
}

static void audioCallback(void *user, AudioQueueRef q, AudioQueueBufferRef buf)
{
    (void)user;
    float *out = buf->mAudioData;
    int n = (int)(buf->mAudioDataBytesCapacity / sizeof(float));
    os_unfair_lock_lock(&gRingLock);
    for (int i = 0; i < n; i++) {
        if (gRingCount > 0) {
            out[i] = gRing[gRingRead];
            gRingRead = (gRingRead + 1) % RING_SIZE;
            gRingCount--;
        } else {
            out[i] = 0.0f;
        }
    }
    os_unfair_lock_unlock(&gRingLock);
    buf->mAudioDataByteSize = (UInt32)(n * sizeof(float));
    OSStatus st = AudioQueueEnqueueBuffer(q, buf, 0, NULL);
    if (st != noErr) NSLog(@"AudioQueueEnqueueBuffer failed: %d", (int)st);
}

static void startAudio(void)
{
    AudioStreamBasicDescription fmt = {
        .mSampleRate = SAMPLE_RATE,
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
        .mBytesPerPacket = sizeof(float),
        .mFramesPerPacket = 1,
        .mBytesPerFrame = sizeof(float),
        .mChannelsPerFrame = 1,
        .mBitsPerChannel = 32,
    };
    OSStatus st = AudioQueueNewOutput(&fmt, audioCallback, NULL, NULL, NULL, 0, &gQueue);
    if (st != noErr) {
        NSLog(@"AudioQueueNewOutput failed (%d), running without sound", (int)st);
        gQueue = NULL;
        return;
    }
    for (int i = 0; i < AQ_BUFFERS; i++) {
        AudioQueueBufferRef b;
        st = AudioQueueAllocateBuffer(gQueue, AQ_FRAMES * sizeof(float), &b);
        if (st != noErr) {
            NSLog(@"AudioQueueAllocateBuffer failed: %d", (int)st);
            continue;
        }
        audioCallback(NULL, gQueue, b);
    }
    st = AudioQueueStart(gQueue, NULL);
    if (st != noErr) NSLog(@"AudioQueueStart failed: %d", (int)st);
}

// ------------------------------------------------------------------ view

@interface JJView : NSView
@end

@implementation JJView {
    unsigned _keys;
    NSTimer *_timer;
    CFTimeInterval _last, _acc;
    unsigned _savedHi;
}

- (instancetype)initWithFrame:(NSRect)frame
{
    if ((self = [super initWithFrame:frame])) {
        _savedHi = (unsigned)[[NSUserDefaults standardUserDefaults] integerForKey:kHiscoreKey];
        jj_set_hiscore(_savedHi);
        jj_set_sample_rate(SAMPLE_RATE);
        jj_init(arc4random());
        _last = CACurrentMediaTime();
        _timer = [NSTimer timerWithTimeInterval:1.0 / 240.0
                                         target:self
                                       selector:@selector(step:)
                                       userInfo:nil
                                        repeats:YES];
        [[NSRunLoop currentRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes];
    }
    return self;
}

- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)isOpaque { return YES; }

- (void)step:(NSTimer *)t
{
    (void)t;
    CFTimeInterval now = CACurrentMediaTime();
    _acc += MIN(now - _last, 0.25);
    _last = now;
    BOOL ticked = NO;
    while (_acc >= 1.0 / JJ_FPS) {
        jj_tick(_keys);
        ringPush(jj_audio(), jj_audio_len());
        _acc -= 1.0 / JJ_FPS;
        ticked = YES;
    }
    if (ticked) {
        unsigned hi = jj_hiscore();
        if (hi > _savedHi) {
            _savedHi = hi;
            [[NSUserDefaults standardUserDefaults] setInteger:hi forKey:kHiscoreKey];
        }
        [self setNeedsDisplay:YES];
    }
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    NSRect b = self.bounds;
    CGContextSetRGBFillColor(ctx, 0, 0, 0, 1);
    CGContextFillRect(ctx, b);

    CFDataRef data = CFDataCreate(NULL, (const UInt8 *)jj_framebuffer(), JJ_W * JJ_H * 4);
    CGDataProviderRef prov = CGDataProviderCreateWithCFData(data);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGImageRef img = CGImageCreate(JJ_W, JJ_H, 8, 32, JJ_W * 4, cs,
                                   kCGImageAlphaNoneSkipLast | kCGBitmapByteOrder32Big,
                                   prov, NULL, false, kCGRenderingIntentDefault);
    CGFloat scale = MIN(b.size.width / JJ_W, b.size.height / JJ_H);
    CGFloat w = JJ_W * scale, h = JJ_H * scale;
    CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
    CGContextDrawImage(ctx, CGRectMake((b.size.width - w) / 2, (b.size.height - h) / 2, w, h), img);
    CGImageRelease(img);
    CGColorSpaceRelease(cs);
    CGDataProviderRelease(prov);
    CFRelease(data);
}

static unsigned keyFor(unsigned short code)
{
    switch (code) {
    case 123: case 6: case 31: return JJ_KEY_LEFT;   // Left, Z, O
    case 124: case 7: case 35: return JJ_KEY_RIGHT;  // Right, X, P
    case 126: case 49: case 12: return JJ_KEY_JUMP;  // Up, Space, Q
    case 36: return JJ_KEY_START;                    // Return
    case 4: case 53: return JJ_KEY_PAUSE;            // H, Escape
    default: return 0;
    }
}

- (void)keyDown:(NSEvent *)e
{
    if (e.modifierFlags & NSEventModifierFlagCommand) { [super keyDown:e]; return; }
    if (e.keyCode == 46) { // M
        if (!e.isARepeat) gMuted = !gMuted;
        return;
    }
    unsigned k = keyFor(e.keyCode);
    if (!k) { [super keyDown:e]; return; }
    _keys |= k;
}

- (void)keyUp:(NSEvent *)e
{
    _keys &= ~keyFor(e.keyCode);
}

- (void)resetKeys { _keys = 0; }

@end

// ------------------------------------------------------------------ app

@interface JJAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(strong) NSWindow *window;
@property(strong) JJView *view;
@end

@implementation JJAppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)n
{
    (void)n;
    NSRect r = NSMakeRect(0, 0, JJ_W * 3, JJ_H * 3);
    self.window = [[NSWindow alloc]
        initWithContentRect:r
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    self.window.title = @"Jumping Jack";
    self.window.contentAspectRatio = NSMakeSize(JJ_W, JJ_H);
    self.window.contentMinSize = NSMakeSize(JJ_W, JJ_H);
    self.window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    self.window.delegate = self;
    self.view = [[JJView alloc] initWithFrame:r];
    self.window.contentView = self.view;
    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:self.view];
    startAudio();
    [NSApp activateIgnoringOtherApps:YES];
}

- (void)windowDidResignKey:(NSNotification *)n
{
    (void)n;
    [self.view resetKeys];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app
{
    (void)app;
    return YES;
}

- (void)applicationWillTerminate:(NSNotification *)n
{
    (void)n;
    if (gQueue) AudioQueueDispose(gQueue, true);
}

@end

static void buildMenu(void)
{
    NSMenu *bar = [NSMenu new];
    NSMenuItem *appItem = [NSMenuItem new];
    [bar addItem:appItem];
    NSMenu *appMenu = [NSMenu new];
    [appMenu addItemWithTitle:@"Toggle Full Screen"
                       action:@selector(toggleFullScreen:)
                keyEquivalent:@"f"];
    [appMenu.itemArray.lastObject setKeyEquivalentModifierMask:NSEventModifierFlagCommand |
                                                               NSEventModifierFlagControl];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit Jumping Jack" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;
    NSApp.mainMenu = bar;
}

int main(int argc, const char *argv[])
{
    (void)argc;
    (void)argv;
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        JJAppDelegate *delegate = [JJAppDelegate new];
        app.delegate = delegate;
        buildMenu();
        [app run];
    }
    return 0;
}
