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
        layer.contentsScale = vc.view.contentScaleFactor;
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
    _touchIds = [NSMutableDictionary dictionary];
    _nextTouchId = 1;
    ios_android_create();
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    CGFloat s = self.view.contentScaleFactor;
    int w = (int)lrint(self.view.bounds.size.width * s);
    int h = (int)lrint(self.view.bounds.size.height * s);
    ios_android_size(w, h);
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    [self becomeFirstResponder];
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
