/* Greenscreen '89 for iOS: the UIKit host.
 *
 * It creates the Android-shaped activity the shell expects (via shim.c),
 * presents the frames the game thread renders, forwards touches as
 * AInputEvents and owns the audio session. */

#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <AVFoundation/AVFoundation.h>

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <math.h>

#include "ios_glue.h"

/* ------------------------------------------------------------------ */
/* glue the shim calls                                                 */
/* ------------------------------------------------------------------ */
static void px_release(void *info, const void *data, size_t size) {
    (void)data; (void)size;
    ios_buffer_done(info);
}

@interface GameViewController : UIViewController
@end

static __weak GameViewController *g_vc;

/* UIView.contentScaleFactor is 1.0 until the view is attached to a screen, and
 * the first layout pass happens before that — so ask the screen instead. */
static CGFloat screen_scale(UIView *v) {
    UIScreen *sc = v.window.screen;
    if (!sc) sc = [UIScreen mainScreen];
    CGFloat s = sc.scale;
    return s > 0 ? s : 1.0;
}

/* The canvas must be device pixels. UIScreen.nativeBounds is always in pixels,
 * so use it whenever the view's own scale looks like points (1.0). */
static void canvas_pixels(UIView *v, int *outW, int *outH) {
    UIScreen *sc = v.window.screen ?: [UIScreen mainScreen];
    CGSize b = v.bounds.size;
    CGFloat s = screen_scale(v);
    CGSize native = sc.nativeBounds.size;
    if (b.width > b.height && native.width < native.height) {
        CGFloat t = native.width;
        native.width = native.height;
        native.height = t;
    }
    int w = (int)lrint(b.width * s);
    int h = (int)lrint(b.height * s);
    if ((CGFloat)w < native.width * 0.5 || (CGFloat)h < native.height * 0.5) {
        w = (int)lrint(native.width);
        h = (int)lrint(native.height);
    }
    *outW = w;
    *outH = h;
}

void ios_present(const void *px, int w, int h, void *buf) {
    if (!px || w <= 0 || h <= 0) { ios_buffer_done(buf); return; }
    size_t len = (size_t)w * (size_t)h * 4;
    CGDataProviderRef provider = CGDataProviderCreateWithData(buf, px, len, px_release);
    if (!provider) { ios_buffer_done(buf); return; }
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGImageRef img = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4, cs,
                                   kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault,
                                   provider, NULL, false, kCGRenderingIntentDefault);
    CGColorSpaceRelease(cs);
    CGDataProviderRelease(provider);
    if (!img) { ios_buffer_done(buf); return; }
    dispatch_async(dispatch_get_main_queue(), ^{
        GameViewController *vc = g_vc;
        if (!vc) { CGImageRelease(img); return; }
        CALayer *layer = vc.view.layer;
        layer.contentsScale = screen_scale(vc.view);
        layer.contentsGravity = kCAGravityResize;
        layer.contents = (__bridge id)img;
        CGImageRelease(img);
    });
}

void ios_finish(void) {
    dispatch_async(dispatch_get_main_queue(), ^{ ios_android_pause(); });
}

void ios_open_url(const char *url) {
    if (!url) return;
    NSString *s = [NSString stringWithUTF8String:url];
    NSURL *u = s ? [NSURL URLWithString:s] : nil;
    if (!u) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        [[UIApplication sharedApplication] openURL:u options:@{} completionHandler:nil];
    });
}

int ios_clipboard(char *out, int max) {
    if (!out || max <= 0) return -1;
    NSString *s = [UIPasteboard generalPasteboard].string;
    if (!s) return -1;
    const char *c = s.UTF8String;
    if (!c) return -1;
    snprintf(out, (size_t)max, "%s", c);
    return 0;
}

const char *ios_data_path(void) {
    static char path[1024];
    if (path[0]) return path;
    @autoreleasepool {
        NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
        if (!docs) return NULL;
        const char *c = docs.fileSystemRepresentation;
        if (!c) return NULL;
        snprintf(path, sizeof path, "%s", c);
    }
    return path;
}

double ios_dpi(void) {
    return 163.0 * (double)[UIScreen mainScreen].scale;
}

void ios_log(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    NSLog(@"[gs89] %s", buf);
}

/* ------------------------------------------------------------------ */
/* rendering view                                                      */
/* ------------------------------------------------------------------ */
@interface GameView : UIView
@end

@implementation GameView
+ (Class)layerClass { return [CALayer class]; }
- (BOOL)isOpaque { return YES; }
- (BOOL)canBecomeFirstResponder { return YES; }
@end

/* ------------------------------------------------------------------ */
/* game screen                                                         */
/* ------------------------------------------------------------------ */
@implementation GameViewController {
    NSMutableDictionary *_touchIds;
    int _nextTouchId;
}

- (void)loadView {
    GameView *v = [[GameView alloc] initWithFrame:[UIScreen mainScreen].bounds];
    v.multipleTouchEnabled = YES;
    v.backgroundColor = [UIColor blackColor];
    v.layer.backgroundColor = [UIColor blackColor].CGColor;
    self.view = v;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    g_vc = self;
    self.view.contentScaleFactor = screen_scale(self.view);
    _touchIds = [NSMutableDictionary dictionary];
    _nextTouchId = 1;
    ios_android_create();
}

/* the shell works in pixels: points x screen scale */
- (void)applyPixelSize {
    CGFloat s = screen_scale(self.view);
    if (self.view.contentScaleFactor != s) self.view.contentScaleFactor = s;
    CGSize b = self.view.bounds.size;
    int w = 0, h = 0;
    canvas_pixels(self.view, &w, &h);
    ios_log("view %.0fx%.0f pt, scale %.2f -> %dx%d px, window %s",
            b.width, b.height, s, w, h, self.view.window ? "attached" : "nil");
    ios_android_size(w, h);
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    [self applyPixelSize];
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    [self becomeFirstResponder];
    [self applyPixelSize];       /* the window exists now: correct any 1x first pass */
    ios_android_resume();
}

- (void)viewWillDisappear:(BOOL)animated {
    [super viewWillDisappear:animated];
    ios_android_pause();
}

- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures { return UIRectEdgeAll; }

- (BOOL)shouldAutorotate { return YES; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskAllButUpsideDown; }

/* ---- touches -> Android pointer events ---- */
- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    CGFloat s = self.view.contentScaleFactor;
    for (UITouch *t in touches) {
        NSValue *key = [NSValue valueWithNonretainedObject:t];
        if (_touchIds[key]) continue;
        int tid = _nextTouchId++;
        _touchIds[key] = @(tid);
        CGPoint p = [t locationInView:self.view];
        ios_touch_begin(tid, (float)(p.x * s), (float)(p.y * s));
    }
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    CGFloat s = self.view.contentScaleFactor;
    for (UITouch *t in touches) {
        NSValue *key = [NSValue valueWithNonretainedObject:t];
        NSNumber *n = _touchIds[key];
        if (!n) {
            int tid = _nextTouchId++;
            _touchIds[key] = @(tid);
            CGPoint p = [t locationInView:self.view];
            ios_touch_begin(tid, (float)(p.x * s), (float)(p.y * s));
            continue;
        }
        CGPoint p = [t locationInView:self.view];
        ios_touch_move(n.intValue, (float)(p.x * s), (float)(p.y * s));
    }
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    CGFloat s = self.view.contentScaleFactor;
    for (UITouch *t in touches) {
        NSValue *key = [NSValue valueWithNonretainedObject:t];
        NSNumber *n = _touchIds[key];
        if (!n) continue;
        CGPoint p = [t locationInView:self.view];
        ios_touch_end(n.intValue, (float)(p.x * s), (float)(p.y * s));
        [_touchIds removeObjectForKey:key];
    }
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    ios_touch_cancel();
    [_touchIds removeAllObjects];
}

@end

/* ------------------------------------------------------------------ */
/* app                                                                 */
/* ------------------------------------------------------------------ */
@interface AppDelegate : UIResponder <UIApplicationDelegate>
@property (strong, nonatomic) UIWindow *window;
@end

@implementation AppDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    AVAudioSession *session = [AVAudioSession sharedInstance];
    [session setCategory:AVAudioSessionCategoryAmbient error:nil];
    [session setActive:YES error:nil];
    [UIApplication sharedApplication].idleTimerDisabled = YES;

    self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
    self.window.backgroundColor = [UIColor blackColor];
    self.window.rootViewController = [[GameViewController alloc] init];
    [self.window makeKeyAndVisible];
    return YES;
}

- (void)applicationDidEnterBackground:(UIApplication *)application {
    ios_android_pause();
}

- (void)applicationWillEnterForeground:(UIApplication *)application {
    ios_android_resume();
}

- (void)applicationWillTerminate:(UIApplication *)application {
    ios_android_destroy();
}

@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
    }
}
