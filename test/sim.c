/* Desktop simulator for the Android shell: fake NativeActivity / window /
 * input / AAudio implementations so main.c's real code (threads, layout,
 * gestures, rendering, persistence) can be exercised headlessly. */
#define _GNU_SOURCE
#include <sys/types.h>
#include <stdarg.h>
#include <sys/stat.h>
#include "../android/main.c"
#include "../android/touchui.c"
#include "../android/link.c"
#include "../android/net.c"
#include <sys/stat.h>

/* ---------------- fakes ---------------- */
struct ANativeWindow { int w, h; uint32_t *px; uint32_t *front; int posts; int copyback; int locked; int f565; };
static pthread_mutex_t fmu = PTHREAD_MUTEX_INITIALIZER;
struct AInputQueue { int dummy; };
struct AInputEvent { int type, action, n, ids[4]; float xs[4], ys[4]; int key, repeat; int64_t t; int source, device; };
struct ALooper { int d; };
struct AConfiguration { int d; };

static pthread_mutex_t qmu = PTHREAD_MUTEX_INITIALIZER;
static struct AInputEvent evq[256]; static int evh, evt_;
static int finished;
static int sim_density = 450;
static int64_t audio_frames;
static FILE *audio_out;

int __android_log_print(int p, const char *tag, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); fprintf(stderr, "[%s] ", tag); vfprintf(stderr, fmt, ap); fputc('\n', stderr); va_end(ap); return 0;
}
int32_t ANativeWindow_getWidth(ANativeWindow *w) { return w->w; }
int32_t ANativeWindow_getHeight(ANativeWindow *w) { return w->h; }
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *w, int32_t a, int32_t b, int32_t f) { return 0; }
int32_t ANativeWindow_lock(ANativeWindow *w, ANativeWindow_Buffer *b, ARect *d) {
    if (w->locked) { fprintf(stderr, "double lock!\n"); abort(); }
    w->locked = 1;
    b->width = w->w; b->height = w->h; b->stride = w->w; b->format = w->f565 ? 4 : 2; b->bits = w->px;
    if (!w->copyback && d) { d->left = 0; d->top = 0; d->right = w->w; d->bottom = w->h; }
    return 0;
}
int32_t ANativeWindow_unlockAndPost(ANativeWindow *w) { w->locked = 0; pthread_mutex_lock(&fmu);
    if (w->f565) { const uint16_t *s16 = (const uint16_t *)w->px; for (int i = 0; i < w->w * w->h; i++) { uint16_t v = s16[i]; w->front[i] = ((v >> 11) << 3) | (((v >> 5) & 63) << 10) | ((v & 31) << 19); } }
    else memcpy(w->front, w->px, (size_t)w->w * w->h * 4);
    pthread_mutex_unlock(&fmu); __atomic_add_fetch(&w->posts, 1, __ATOMIC_SEQ_CST); return 0; }
ALooper *ALooper_prepare(int o) { static ALooper l; return &l; }
static int attached;
int ALooper_pollOnce(int t, int *fd, int *ev, void **data) {
    pthread_mutex_lock(&qmu); int has = evh != evt_; pthread_mutex_unlock(&qmu);
    return (attached && has) ? 1 : -3;   /* like a readable input fd */
}
void AInputQueue_attachLooper(AInputQueue *q, ALooper *l, int id, ALooper_callbackFunc cb, void *d) { attached = 1; }
void AInputQueue_detachLooper(AInputQueue *q) { attached = 0; }
int32_t AInputQueue_getEvent(AInputQueue *q, AInputEvent **e) {
    pthread_mutex_lock(&qmu);
    if (evh == evt_) { pthread_mutex_unlock(&qmu); return -1; }
    *e = &evq[evh % 256]; evh++;
    pthread_mutex_unlock(&qmu);
    return 0;
}
int32_t AInputQueue_preDispatchEvent(AInputQueue *q, AInputEvent *e) { return 0; }
void AInputQueue_finishEvent(AInputQueue *q, AInputEvent *e, int h) {}
int32_t AInputEvent_getType(const AInputEvent *e) { return e->type; }
int32_t AInputEvent_getSource(const AInputEvent *e) { return e->source ? e->source : AINPUT_SOURCE_TOUCHSCREEN; }
int32_t AInputEvent_getDeviceId(const AInputEvent *e) { return e->device; }
ALooper *ALooper_forThread(void) { return NULL; }
int ALooper_addFd(ALooper *l, int fd, int ident, int events, ALooper_callbackFunc cb, void *data) { return 1; }
int32_t AMotionEvent_getAction(const AInputEvent *e) { return e->action; }
int64_t AMotionEvent_getEventTime(const AInputEvent *e) { return e->t; }
size_t AMotionEvent_getPointerCount(const AInputEvent *e) { return (size_t)e->n; }
int32_t AMotionEvent_getPointerId(const AInputEvent *e, size_t i) { return e->ids[i]; }
float AMotionEvent_getX(const AInputEvent *e, size_t i) { return e->xs[i]; }
float AMotionEvent_getY(const AInputEvent *e, size_t i) { return e->ys[i]; }
int32_t AKeyEvent_getAction(const AInputEvent *e) { return e->action; }
int32_t AKeyEvent_getKeyCode(const AInputEvent *e) { return e->key; }
int32_t AKeyEvent_getRepeatCount(const AInputEvent *e) { return e->repeat; }
AConfiguration *AConfiguration_new(void) { return calloc(1, sizeof(AConfiguration)); }
void AConfiguration_delete(AConfiguration *c) { free(c); }
void AConfiguration_fromAssetManager(AConfiguration *c, AAssetManager *m) {}
int32_t AConfiguration_getDensity(AConfiguration *c) { return sim_density; }
void ANativeActivity_finish(ANativeActivity *a) { finished = 1; }
void ANativeActivity_setWindowFlags(ANativeActivity *a, uint32_t add, uint32_t rem) {}

/* AAudio fake. By default a simple sink; with SIM_AUDIO_STRICT it behaves like
 * the real thing: pause/stop take effect a little later (requestStart while
 * PAUSING fails with INVALID_STATE), a non-started stream doesn't drain so
 * writes time out, and a stream can be DISCONNECTED (headphones, Bluetooth). */
struct AAudioStreamStruct { int state; int64_t state_at; int next_state; int64_t played, written, t_start; };
struct AAudioStreamBuilderStruct { int d; };
static int64_t sim_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec; }
static int strict_audio(void) { return getenv("SIM_AUDIO_STRICT") != NULL; }
static int64_t audible_frames;            /* frames the fake device actually played */
static AAudioStream *sim_stream;          /* the open stream (tests can disconnect it) */
static void st_settle(AAudioStream *s) {
    if (s->next_state && sim_ns() >= s->state_at) { s->state = s->next_state; s->next_state = 0; }
}
aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder **b) { *b = calloc(1, sizeof **b); return 0; }
void AAudioStreamBuilder_setDirection(AAudioStreamBuilder *b, aaudio_direction_t d) {}
void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder *b, int32_t r) {}
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder *b, int32_t c) {}
void AAudioStreamBuilder_setFormat(AAudioStreamBuilder *b, aaudio_format_t f) {}
void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder *b, aaudio_performance_mode_t m) {}
void AAudioStreamBuilder_setSharingMode(AAudioStreamBuilder *b, aaudio_sharing_mode_t m) {}
void AAudioStreamBuilder_setUsage(AAudioStreamBuilder *b, aaudio_usage_t u) {}
void AAudioStreamBuilder_setContentType(AAudioStreamBuilder *b, aaudio_content_type_t c) {}
aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder *b, AAudioStream **s) {
    *s = calloc(1, sizeof **s); (*s)->state = AAUDIO_STREAM_STATE_OPEN; sim_stream = *s; return 0; }
aaudio_result_t AAudioStreamBuilder_delete(AAudioStreamBuilder *b) { free(b); return 0; }
int32_t AAudioStream_getSampleRate(AAudioStream *s) { return 48000; }
int32_t AAudioStream_getChannelCount(AAudioStream *s) { return 2; }
aaudio_format_t AAudioStream_getFormat(AAudioStream *s) { return AAUDIO_FORMAT_PCM_I16; }
int32_t AAudioStream_getFramesPerBurst(AAudioStream *s) { return 192; }
aaudio_result_t AAudioStream_setBufferSizeInFrames(AAudioStream *s, int32_t n) { return n; }
aaudio_stream_state_t AAudioStream_getState(AAudioStream *s) { st_settle(s); return s->state; }
int64_t AAudioStream_getFramesRead(AAudioStream *s) { return s->played; }
aaudio_result_t AAudioStream_requestStart(AAudioStream *s) {
    st_settle(s);
    if (s->state == AAUDIO_STREAM_STATE_DISCONNECTED) return AAUDIO_ERROR_DISCONNECTED;
    if (strict_audio() && (s->state == AAUDIO_STREAM_STATE_PAUSING || s->state == AAUDIO_STREAM_STATE_STOPPING)) return AAUDIO_ERROR_INVALID_STATE;
    s->state = AAUDIO_STREAM_STATE_STARTED; s->next_state = 0; s->t_start = 0; return 0;
}
static aaudio_result_t req_halt(AAudioStream *s, int mid, int end) {
    st_settle(s);
    if (s->state == AAUDIO_STREAM_STATE_DISCONNECTED) return AAUDIO_ERROR_DISCONNECTED;
    if (strict_audio()) { s->state = mid; s->next_state = end; s->state_at = sim_ns() + 40000000LL; }
    else s->state = end;
    return 0;
}
aaudio_result_t AAudioStream_requestPause(AAudioStream *s) { return req_halt(s, AAUDIO_STREAM_STATE_PAUSING, AAUDIO_STREAM_STATE_PAUSED); }
aaudio_result_t AAudioStream_requestStop(AAudioStream *s) { return req_halt(s, AAUDIO_STREAM_STATE_STOPPING, AAUDIO_STREAM_STATE_STOPPED); }
aaudio_result_t AAudioStream_close(AAudioStream *s) { if (s == sim_stream) sim_stream = NULL; free(s); return 0; }
static void sim_audio_disconnect(void) { if (sim_stream) sim_stream->state = AAUDIO_STREAM_STATE_DISCONNECTED; }
aaudio_result_t AAudioStream_write(AAudioStream *s, const void *buf, int32_t n, int64_t to) {
    st_settle(s);
    if (strict_audio()) {
        if (s->state == AAUDIO_STREAM_STATE_DISCONNECTED) return AAUDIO_ERROR_DISCONNECTED;
        int64_t tn = sim_ns();
        if (s->state == AAUDIO_STREAM_STATE_STARTED) {
            if (!s->t_start) s->t_start = tn - s->played * 1000000000LL / 48000;
            int64_t can = (tn - s->t_start) * 48000 / 1000000000LL;     /* device position */
            if (can > s->written) { s->t_start = tn - s->written * 1000000000LL / 48000; can = s->written; }   /* underrun */
            s->played = can;
        }
        /* blocking write, like AAudio: wait for room until everything is in or the timeout passes */
        int done = 0; int64_t deadline = tn + to;
        for (;;) {
            st_settle(s);
            if (s->state == AAUDIO_STREAM_STATE_DISCONNECTED) return done ? done : AAUDIO_ERROR_DISCONNECTED;
            int64_t now2 = sim_ns();
            if (s->state == AAUDIO_STREAM_STATE_STARTED) {
                if (!s->t_start) s->t_start = now2 - s->played * 1000000000LL / 48000;
                int64_t can = (now2 - s->t_start) * 48000 / 1000000000LL;
                if (can > s->written) { s->t_start = now2 - s->written * 1000000000LL / 48000; can = s->written; }
                s->played = can;
            }
            int64_t room = 2048 - (s->written - s->played);
            if (room > 0) {
                int k = room < n - done ? (int)room : n - done;
                s->written += k; done += k;
                if (audio_out) fwrite((const char *)buf + (size_t)(done - k) * 4, 4, (size_t)k, audio_out);
                audio_frames += k; audible_frames += k;
                if (done == n) return n;
            }
            if (now2 >= deadline) return done;
            struct timespec ts = { 0, 1000000 }; nanosleep(&ts, NULL);
        }
    }
    if (audio_out) fwrite(buf, 4, (size_t)n, audio_out);
    audio_frames += n;
    if (getenv("SIM_AUDIO_REALTIME")) {
        /* a buffer drained at 48 kHz; write blocks only while it's full */
        static int64_t written, t_start;
        int64_t tn = sim_ns();
        if (!t_start) t_start = tn;
        written += n;
        int64_t played = (tn - t_start) * 48000 / 1000000000LL;
        if (played > written) { t_start = tn - written * 1000000000LL / 48000; played = written; }   /* underrun */
        int64_t over = (written - played) - 2048;          /* ~43 ms of buffer */
        if (over > 0) { struct timespec ts = { 0, (long)(over * 1000000000LL / 48000) }; nanosleep(&ts, NULL); }
    } else {
        /* the device consumes at 48 kHz, 8x faster than real time (fast tests) */
        struct timespec ts = { 0, (long)((int64_t)n * 1000000000LL / 48000 / 8) };
        if (!getenv("SIM_NO_AUDIO_SLEEP")) nanosleep(&ts, NULL);
    }
    return n;
}

/* ---------------- driver ---------------- */
static ANativeActivity act;
static ANativeActivityCallbacks cbs;
static ANativeWindow win;
static AInputQueue queue;
static const char *outdir;

static void push(struct AInputEvent e) {
    pthread_mutex_lock(&qmu);
    e.t = now_ns();
    evq[evt_ % 256] = e; evt_++;
    pthread_mutex_unlock(&qmu);
}
static void wait_frames(int n) {
    int target = __atomic_load_n(&win.posts, __ATOMIC_SEQ_CST) + n;
    while (__atomic_load_n(&win.posts, __ATOMIC_SEQ_CST) < target) { struct timespec ts = {0, 200000}; nanosleep(&ts, NULL); }
}
static void touch(int act_, int id, float x, float y) {
    struct AInputEvent e = { AINPUT_EVENT_TYPE_MOTION, act_, 1, {id}, {x}, {y} };
    push(e);
}
static void tap(float x, float y) { touch(AMOTION_EVENT_ACTION_DOWN, 0, x, y); wait_frames(3); touch(AMOTION_EVENT_ACTION_UP, 0, x, y); wait_frames(4); }
static void drag(float x0, float y0, float x1, float y1, int steps, int hold_frames) {
    touch(AMOTION_EVENT_ACTION_DOWN, 0, x0, y0); wait_frames(1);
    for (int i = 1; i <= steps; i++) { touch(AMOTION_EVENT_ACTION_MOVE, 0, x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps); wait_frames(1); }
    wait_frames(hold_frames);
    touch(AMOTION_EVENT_ACTION_UP, 0, x1, y1); wait_frames(3);
}
static void msleep(int ms) { struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }
static void soft_drag(float x0, float y0, float x1, float y1, int hold_ms) {
    touch(AMOTION_EVENT_ACTION_DOWN, 0, x0, y0); msleep(30);
    for (int i = 1; i <= 6; i++) { touch(AMOTION_EVENT_ACTION_MOVE, 0, x0 + (x1 - x0) * i / 6, y0 + (y1 - y0) * i / 6); msleep(40); }
    msleep(hold_ms);
    touch(AMOTION_EVENT_ACTION_UP, 0, x1, y1); wait_frames(3);
}
static void flick(float x0, float y0, float dy) {
    touch(AMOTION_EVENT_ACTION_DOWN, 0, x0, y0); msleep(16);
    for (int i = 1; i <= 4; i++) { touch(AMOTION_EVENT_ACTION_MOVE, 0, x0, y0 + dy * i / 4); msleep(16); }
    touch(AMOTION_EVENT_ACTION_UP, 0, x0, y0 + dy); wait_frames(4);
}
static void tap_gb(int gx, int gy) { tap(L.screen.x + (gx + 0.5f) * L.screen.w / GB_W, L.screen.y + (gy + 0.5f) * L.screen.h / GB_H); wait_frames(20); }
static void st(const char *what) { fprintf(stderr, "%-28s state=%02x gametype=%02x music=%02x alv=%d blv=%d high=%d\n", what, gb_peek(0xFFE1), gb_peek(0xFFC0), gb_peek(0xFFC1), gb_peek(0xFFC2), gb_peek(0xFFC3), gb_peek(0xFFC4)); }
static void key(int code) {
    struct AInputEvent e = { AINPUT_EVENT_TYPE_KEY, AKEY_EVENT_ACTION_DOWN }; e.key = code; push(e); wait_frames(3);
    e.action = AKEY_EVENT_ACTION_UP; push(e); wait_frames(3);
}
static void shot(const char *name) {
    wait_frames(2);
    char p[512]; snprintf(p, sizeof p, "%s/%s.ppm", outdir, name);
    FILE *f = fopen(p, "wb");
    fprintf(f, "P6 %d %d 255\n", win.w, win.h);
    pthread_mutex_lock(&fmu);
    for (int i = 0; i < win.w * win.h; i++) { uint32_t c = win.front[i]; fputc(c & 255, f); fputc((c >> 8) & 255, f); fputc((c >> 16) & 255, f); }
    pthread_mutex_unlock(&fmu);
    fclose(f);
    fprintf(stderr, "shot %s  state=%02x paused=%02x\n", name, gb_peek(0xFFE1), gb_peek(0xFFAB));
}
static void create(int w, int h) {
    memset(&act, 0, sizeof act); memset(&cbs, 0, sizeof cbs);
    act.callbacks = &cbs; act.internalDataPath = outdir;
    ANativeActivity_onCreate(&act, NULL, 0);
    cbs.onStart(&act); cbs.onResume(&act);
    free(win.px);
    win.w = w; win.h = h; win.px = calloc((size_t)w * h, 4); free(win.front); win.front = calloc((size_t)w * h, 4); win.posts = 0;
    cbs.onNativeWindowCreated(&act, &win);
    cbs.onInputQueueCreated(&act, &queue);
    ARect r = {0, 0, w, h - 48 * 3}; /* nav bar */
    cbs.onContentRectChanged(&act, &r);
    cbs.onWindowFocusChanged(&act, 1);
}
static void destroy(void) {
    cbs.onPause(&act); cbs.onWindowFocusChanged(&act, 0);
    cbs.onInputQueueDestroyed(&act, &queue);
    cbs.onNativeWindowDestroyed(&act, &win);
    cbs.onStop(&act); cbs.onDestroy(&act);
}

static int run_2p(char role, int listen, int send);
static int run_cpu(int level, int rounds);
static int run_norom(void);
static int run_ctl(void);
static int run_audio(void);
static int run_online_ui(void);
static int run_kbd(void);
/* the tester's own ROM: T89_ROM=/path/to/rom.gb */
static const char *rom_src(void) { return getenv("T89_ROM") ? getenv("T89_ROM") : "rom/Tetris (World) (Rev A).gb"; }
static void install_rom(void) {
    char p[512]; snprintf(p, sizeof p, "%s/rom.gb", outdir);
    uint8_t r[ROM_SIZE];
    if (rom_check_file(rom_src(), r) == ROM_OK) rom_save(p, r);
}
int main(int argc, char **argv) {
    outdir = argv[1];
    if (argc > 2 && !strcmp(argv[2], "norom")) return run_norom();
    if (argc > 2 && !strcmp(argv[2], "ctl")) { install_rom(); return run_ctl(); }
    if (argc > 2 && !strcmp(argv[2], "audio")) { install_rom(); return run_audio(); }
    if (argc > 2 && !strcmp(argv[2], "onlineui")) { install_rom(); return run_online_ui(); }
    if (argc > 2 && !strcmp(argv[2], "kbd")) { install_rom(); return run_kbd(); }
    install_rom();
    if (argc > 6) return run_2p(argv[4][0], atoi(argv[5]), atoi(argv[6]));
    if (argc > 2 && !strcmp(argv[2], "cpu")) return run_cpu(argc > 3 ? atoi(argv[3]) : 2, argc > 4 ? atoi(argv[4]) : 2);
    char ap[512]; snprintf(ap, sizeof ap, "%s/audio.raw", outdir);
    audio_out = fopen(ap, "wb");
    char sp[512]; snprintf(sp, sizeof sp, "%s/tetris_state.bin", outdir); remove(sp);
    win.copyback = argc > 2 ? atoi(argv[2]) : 1;
    win.f565 = argc > 3 ? atoi(argv[3]) : 0;

    create(1080, 2340);
    wait_frames(320); shot("a01_copyright");
    float cx = L.screen.x + L.screen.w * 0.75f, cy = L.screen.y + L.screen.h * 0.5f;
    shot("a00_layout");
    { /* volume wheel: roll down 6 notches, then mute and unmute */
      float wx = L.wheel.x + L.wheel.w / 2, wy = L.wheel.y + L.wheel.h / 2;
      touch(AMOTION_EVENT_ACTION_DOWN, 0, wx, wy); wait_frames(2);
      for (int i = 1; i <= 12; i++) { touch(AMOTION_EVENT_ACTION_MOVE, 0, wx, wy + dpi * 0.035f * i); wait_frames(1); }
      shot("v1_rolling");
      touch(AMOTION_EVENT_ACTION_UP, 0, wx, wy + dpi * 0.42f); wait_frames(3);
      fprintf(stderr, "volume after rolling down: %d (muted %d)\n", vol_level, vol_muted);
      long long a0 = audio_frames; (void)a0;
      tap(L.spk.x + L.spk.w / 2, L.spk.y + L.spk.h / 2); wait_frames(3);
      fprintf(stderr, "after mute tap: muted %d\n", vol_muted);
      shot("v2_muted");
      tap(L.spk.x + L.spk.w / 2, L.spk.y + L.spk.h / 2); wait_frames(3);
      fprintf(stderr, "after unmute tap: muted %d vol %d\n", vol_muted, vol_level);
      /* roll back up to full */
      touch(AMOTION_EVENT_ACTION_DOWN, 0, wx, wy); wait_frames(2);
      for (int i = 1; i <= 20; i++) { touch(AMOTION_EVENT_ACTION_MOVE, 0, wx, wy - dpi * 0.035f * i); wait_frames(1); }
      touch(AMOTION_EVENT_ACTION_UP, 0, wx, wy - dpi * 0.7f); wait_frames(3);
      fprintf(stderr, "volume after rolling up: %d\n", vol_level); }
    tap_gb(80, 40); wait_frames(20); st("tap copyright");
    tap_gb(80, 60); wait_frames(10); st("tap title");
    tap_gb(110, 43); st("tap B-TYPE (game)");
    tap_gb(110, 43); st("tap B-TYPE again (confirm)");
    tap_gb(110, 115); st("tap OFF (music)");
    tap_gb(30, 97); st("tap A-TYPE music");
    tap_gb(30, 43); st("tap A-TYPE game (back up)");
    key(AKEYCODE_BACK); st("back key on game type");
    tap_gb(110, 43); tap_gb(110, 43); st("B-TYPE + confirm");
    tap_gb(110, 115); tap_gb(110, 115); wait_frames(30); st("OFF + confirm -> B level");
    shot("a03_blevel");
    tap_gb(12 + 16 * 3 + 6, 50); st("tap level 3");
    tap_gb(100 + 16 * 2 + 6, 68); st("tap high 5");
    key(AKEYCODE_BACK); st("back = B (to level)");
    tap_gb(12 + 16 * 4 + 6, 68); st("tap level 9");
    tap_gb(100 + 16 * 1 + 6, 50); st("tap high 1");
    shot("a04_bhigh");
    tap_gb(100 + 16 * 1 + 6, 50); wait_frames(60); st("tap high 1 again -> start");
    shot("a06_ingame_start");
    { /* a swipe that starts on START must not pause */
      float bx = L.bstart.x + L.bstart.w / 2, by = L.bstart.y + L.bstart.h / 2;
      drag(bx, by, bx - dpi * 0.5f, by, 5, 1); wait_frames(5);
      fprintf(stderr, "swipe from START pill: paused=%d (want 0)\n", gb_peek(0xFFAB));
      fprintf(stderr, "gap screen->buttons: %.2f in\n", (L.bstart.y - (L.bezel.y + L.bezel.h)) / dpi); }
    { int y0 = gb_peek(0xC201);
      soft_drag(cx, cy - dpi * 0.3f, cx, cy + dpi * 0.3f, 300);
      /* now release with a sharp downward jerk, like people do: must NOT hard drop */
      touch(AMOTION_EVENT_ACTION_DOWN, 0, cx, cy - dpi * 0.2f); msleep(30);
      for (int i = 1; i <= 8; i++) { touch(AMOTION_EVENT_ACTION_MOVE, 0, cx, cy - dpi * 0.2f + dpi * 0.03f * i); msleep(25); }
      for (int i = 1; i <= 3; i++) { touch(AMOTION_EVENT_ACTION_MOVE, 0, cx, cy + dpi * 0.04f + dpi * 0.2f * i); msleep(10); }
      touch(AMOTION_EVENT_ACTION_UP, 0, cx, cy + dpi * 0.64f); wait_frames(3);
      fprintf(stderr, "[hard drops after soft-drag tests: %d]\n", hard_drops);
      fprintf(stderr, "soft drag + jerky release: y %d -> %d, locks=%d, hard_drop_hold=%d\n", y0, gb_peek(0xC201), lock_count, hard_drop_hold); }
    { int lc = lock_count;
      flick(cx, cy - dpi * 0.3f, dpi * 0.6f); wait_frames(3);
      fprintf(stderr, "[hard drops after clean flick: %d]\n", hard_drops);
      fprintf(stderr, "clean flick: locks %d -> %d\n", lc, lock_count); }
    { int lc = lock_count;
      flick(cx, cy - dpi * 0.3f, dpi * 0.3f); wait_frames(3);
      fprintf(stderr, "[hard drops after short flick: %d]\n", hard_drops);
      fprintf(stderr, "short flick (0.3in): locks %d -> %d (should not drop)\n", lc, lock_count); }
    wait_frames(30); shot("a09_dropped");
    /* pause/resume cycle: resume should pause the game */
    cbs.onPause(&act);
    { struct stat st; fprintf(stderr, "state saved: %s\n", stat(sp, &st) == 0 ? "yes" : "NO"); }
    cbs.onResume(&act);
    wait_frames(10); shot("a10_after_resume_paused");
    tap(L.bstart.x + L.bstart.w / 2, L.bstart.y + L.bstart.h / 2);  /* unpause */
    wait_frames(10); shot("a11_unpaused");
    /* back once -> pause + message; back twice -> finish */
    key(AKEYCODE_BACK);
    wait_frames(5); shot("a12_back_once");
    key(AKEYCODE_BACK);
    wait_frames(5);
    fprintf(stderr, "finish requested: %s\n", finished ? "yes" : "NO");
    int s_before = gb_peek(0xFFE1);
    uint8_t board_before[0x240]; for (int i = 0; i < 0x240; i++) board_before[i] = gb_peek((uint16_t)(0xC800 + i));
    destroy();
    /* relaunch: must resume the same game */
    create(1080, 2340);
    wait_frames(10);
    int same = 1; for (int i = 0; i < 0x240; i++) if (gb_peek((uint16_t)(0xC800 + i)) != board_before[i]) same = 0;
    fprintf(stderr, "relaunch: gamestate %02x (was %02x), board identical: %s\n", gb_peek(0xFFE1), s_before, same ? "yes" : "NO");
    shot("a13_relaunched");
    /* hold START ~1.5 s -> the game's soft reset */
    { float bx = L.bstart.x + L.bstart.w / 2, by = L.bstart.y + L.bstart.h / 2;
      touch(AMOTION_EVENT_ACTION_DOWN, 0, bx, by); msleep(700); shot("a14_start_holding"); msleep(800);
      touch(AMOTION_EVENT_ACTION_UP, 0, bx, by); wait_frames(60);
      fprintf(stderr, "after hold-START reset: state %02x (copyright = 24/25)\n", gb_peek(0xFFE1)); }
    /* a quick START tap still works as Start */
    wait_frames(200); tap(L.bstart.x + L.bstart.w / 2, L.bstart.y + L.bstart.h / 2); wait_frames(40);
    fprintf(stderr, "START tap on copyright -> state %02x (title = 07)\n", gb_peek(0xFFE1));
    /* window recreate cycle (e.g. screen off/on) */
    cbs.onPause(&act); cbs.onNativeWindowDestroyed(&act, &win);
    cbs.onNativeWindowCreated(&act, &win); cbs.onResume(&act);
    wait_frames(10);
    destroy();
    /* landscape window */
    create(2340, 1080);
    wait_frames(10); shot("b01_landscape");
    destroy();
    create(1440, 3120);
    wait_frames(10); shot("c01_qhd");
    destroy();
    fclose(audio_out);
    fprintf(stderr, "audio frames written: %lld\n", (long long)audio_frames);
    return 0;
}

/* ---------------- two-phone same-Wi-Fi test (two processes) ---------------- */
static int gs_(void) { return gb_peek(0xFFE1); }
static int wait_for(int (*pred)(void), int max_frames) {
    for (int i = 0; i < max_frames; i++) { if (pred()) return 1; wait_frames(1); }
    return pred();
}
static int is_linked(void) { return linked; }
static int at_music(void) { return gs_() == 0x2b || gs_() == 0x2a; }
static int at_heights(void) { return gs_() == 0x17; }
static int in_2p_game(void) { return gs_() == 0x1a; }
static int at_title(void) { return gs_() == 0x07; }
static int lost_msg(void) { return lui == LUI_MSG; }
static void tapw(float x, float y) { tap(x, y); }

static int run_2p(char role, int listen, int send) {
    net_test_ports(listen, send);
    char ap[512]; snprintf(ap, sizeof ap, "%s/audio_%c.raw", outdir, role);
    audio_out = fopen(ap, "wb");
    char sp[512]; snprintf(sp, sizeof sp, "%s/tetris_state.bin", outdir); remove(sp);
    win.copyback = 1;
    create(1080, 2340);
    wait_frames(330);
    tap_gb(80, 40); wait_frames(40);
    fprintf(stderr, "[%c] title: %02x\n", role, gs_());
    tap_gb(120, 115); wait_frames(10);                 /* 2PLAYER without a link -> menu */
    fprintf(stderr, "[%c] overlay state %d\n", role, lui);
    shot(role == 'A' ? "p2_A_menu" : "p2_B_menu");
    overlay_layout();
    tapw(O.b2.x + O.b2.w / 2, O.b2.y + O.b2.h / 2);    /* SAME WI-FI */
    wait_frames(5);
    shot(role == 'A' ? "p2_A_search" : "p2_B_search");
    int ok = wait_for(is_linked, 600);
    fprintf(stderr, "[%c] linked: %d (%s)\n", role, ok, net_status_text());
    if (!ok) return 2;
    wait_frames(30);
    shot(role == 'A' ? "p2_A_linked" : "p2_B_linked");
    if (role == 'A') { wait_frames(60); tap_gb(120, 115); }   /* A becomes Mario (master) */
    ok = wait_for(at_music, 900);
    fprintf(stderr, "[%c] music screen: %d (state %02x, role %02x)\n", role, ok, gs_(), gb_peek(0xFFCB));
    if (role == 'A') { wait_frames(30); tap_gb(110, 115); wait_frames(20); tap_gb(110, 115); }  /* OFF, confirm */
    ok = wait_for(at_heights, 900);
    fprintf(stderr, "[%c] heights screen: %d (state %02x) music=%02x\n", role, ok, gs_(), gb_peek(0xFFC1));
    wait_frames(30);
    if (role == 'B') { tap_gb(84 + 16 * 2 + 6, 104); }              /* Luigi height 2 */
    else { tap_gb(84 + 16 * 1 + 6, 48); wait_frames(90); shot("p2_A_heights"); tap_gb(84 + 16 * 1 + 6, 48); }  /* Mario height 1, start */
    ok = wait_for(in_2p_game, 1200);
    fprintf(stderr, "[%c] in game: %d heights %d/%d\n", role, ok, gb_peek(0xFFAC), gb_peek(0xFFAD));
    float cx = L.screen.x + L.screen.w * 0.5f, cy = L.screen.y + L.screen.h * 0.4f;
    unsigned r = role == 'A' ? 1 : 7;
    for (int k = 0; k < 120 && gs_() == 0x1a; k++) {
        r = r * 1103515245u + 12345u;
        int act = (r >> 16) % 6;
        float dx = ((int)((r >> 8) % 7) - 3) * dpi * 0.16f;
        if (act == 0) tap(cx + L.screen.w * 0.2f, cy);
        else if (act <= 2) drag(cx, cy, cx + dx, cy, 6, 1);
        else flick(cx, cy - dpi * 0.3f, dpi * 0.6f);
        wait_frames(8);
        if (k == 40) shot(role == 'A' ? "p2_A_play" : "p2_B_play");
    }
    fprintf(stderr, "[%c] after play: state %02x lines %02x hard drops %d\n", role, gs_(), gb_peek(0xFF9E), hard_drops);
    shot(role == 'A' ? "p2_A_after" : "p2_B_after");
    if (role == 'A') {
        wait_frames(200);
        key(AKEYCODE_BACK); wait_frames(5); shot("p2_A_back_prompt"); key(AKEYCODE_BACK);
        wait_frames(10);
        fprintf(stderr, "[A] unlinked: linked=%d overlay=%d msg='%s'\n", linked, lui, lui_msg);
        wait_frames(200);
        fprintf(stderr, "[A] back at: %02x (2P flag %d)\n", gs_(), tetris_is_2player());
    } else {
        ok = wait_for(lost_msg, 1200);
        fprintf(stderr, "[B] noticed: %d overlay msg='%s' linked=%d\n", ok, lui_msg, linked);
        shot("p2_B_lost");
        wait_frames(200);
        fprintf(stderr, "[B] back at: %02x (2P flag %d)\n", gs_(), tetris_is_2player());
    }
    destroy();
    return 0;
}

/* ---------------- VS CPU (built-in opponent) ---------------- */
static int cpu_b_state(void) { return b_gb_peek(0xFFE1); }
static int both_in_game(void) { return gs_() == 0x1a && cpu_b_state() == 0x1a; }
static int round_over(void) { return gs_() != 0x1a; }
static int at_heights_or_music(void) { return gs_() == 0x17 || at_music(); }
static int run_cpu(int level, int rounds) {
    char sp[512]; snprintf(sp, sizeof sp, "%s/tetris_state.bin", outdir); remove(sp);
    char ap[512]; snprintf(ap, sizeof ap, "%s/audio_cpu.raw", outdir);
    audio_out = fopen(ap, "wb");
    win.copyback = 1;
    create(1080, 2340);
    wait_frames(330);
    tap_gb(80, 40); wait_frames(40);
    fprintf(stderr, "title: %02x\n", gs_());
    { struct timespec a, b; uint64_t c0 = gb_cycles(); clock_gettime(CLOCK_MONOTONIC, &a); wait_frames(300); clock_gettime(CLOCK_MONOTONIC, &b);
      double secs = (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) / 1e9;
      fprintf(stderr, "unlinked pace: %.1f fps\n", (double)(gb_cycles() - c0) / 70224.0 / secs); }
    fprintf(stderr, "single-player readout: speed %.0f%%, load %.2f ms of CPU per frame\n", perf_speed, perf_ms);
    tap_gb(120, 115); wait_frames(10);
    fprintf(stderr, "overlay %d (menu=%d)\n", lui, LUI_MENU);
    shot("cpu_menu");
    overlay_layout();
    tap(O.b1.x + O.b1.w / 2, O.b1.y + O.b1.h / 2); wait_frames(5);
    fprintf(stderr, "overlay %d (cpu=%d)\n", lui, LUI_CPU);
    shot("cpu_levels");
    key(AKEYCODE_BACK); wait_frames(3);
    fprintf(stderr, "back from levels -> overlay %d (menu=%d)\n", lui, LUI_MENU);
    tap(O.b1.x + O.b1.w / 2, O.b1.y + O.b1.h / 2); wait_frames(5);
    Rc lb = level == 0 ? O.b2 : level == 1 ? O.b3 : O.b4;
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    tap(lb.x + lb.w / 2, lb.y + lb.h / 2); wait_frames(5);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    fprintf(stderr, "linked=%d vs_cpu=%d level=%d, bot GB state %02x (title=07)\n", linked, vs_cpu, vs_cpu_level, cpu_b_state());
    shot("cpu_linked");
    { struct timespec a, b; uint64_t c0 = gb_cycles(); clock_gettime(CLOCK_MONOTONIC, &a); wait_frames(300); clock_gettime(CLOCK_MONOTONIC, &b);
      double secs = (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) / 1e9;
      fprintf(stderr, "vs cpu pace: %.1f fps; bot boot took %.0f ms\n", (double)(gb_cycles() - c0) / 70224.0 / secs,
              (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6); }
    tap_gb(120, 115);
    int ok = wait_for(at_music, 900);
    fprintf(stderr, "music screen: %d (state %02x, bot %02x, role %02x)\n", ok, gs_(), cpu_b_state(), gb_peek(0xFFCB));
    wait_frames(30); tap_gb(110, 115); wait_frames(20); tap_gb(110, 115);
    ok = wait_for(at_heights, 900);
    fprintf(stderr, "heights: %d (state %02x bot %02x)\n", ok, gs_(), cpu_b_state());
    wait_frames(60); tap_gb(84 + 16 * 0 + 6, 48); wait_frames(60); tap_gb(84 + 16 * 0 + 6, 48);
    float cx = L.screen.x + L.screen.w * 0.5f, cy = L.screen.y + L.screen.h * 0.4f;
    unsigned r = 3;
    for (int rd = 0; rd < rounds; rd++) {
        ok = wait_for(both_in_game, 1500);
        fprintf(stderr, "round %d: both in game %d (%02x/%02x)\n", rd + 1, ok, gs_(), cpu_b_state());
        if (!ok) return 2;
        int frames0 = (int)gb_cycles() / 70224;
        struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
        int k = 0;
        while (gs_() == 0x1a && k < 900) {
            r = r * 1103515245u + 12345u;
            int act = (r >> 16) % 6;
            float dx = ((int)((r >> 8) % 7) - 3) * dpi * 0.16f;
            if (getenv("SIM_IDLE")) { wait_frames(10); k++; continue; }
            if (act == 0) tap(cx + L.screen.w * 0.2f, cy);
            else if (act <= 2) drag(cx, cy, cx + dx, cy, 6, 1);
            else flick(cx, cy - dpi * 0.3f, dpi * 0.6f);
            wait_frames(10);
            if (k == 30 && rd == 0) shot("cpu_play");
            k++;
        }
        clock_gettime(CLOCK_MONOTONIC, &b);
        int frames = (int)gb_cycles() / 70224 - frames0;
        double secs = (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) / 1e9;
        fprintf(stderr, "  round lasted %.1fs wall, %d player frames (%.1f fps); lines left you=%02x bot=%02x; states %02x/%02x\n",
                secs, frames, frames / secs, gb_peek(0xFF9E), b_gb_peek(0xFF9E), gs_(), cpu_b_state());
        fprintf(stderr, "  readout: speed %.0f%%, load %.2f ms of CPU per frame\n", perf_speed, perf_ms);
        shot(rd == 0 ? "cpu_round1_end" : "cpu_roundN_end");
        /* continue: press START at the results until back at heights */
        for (int j = 0; j < 40 && !(gs_() == 0x17 || gs_() == 0x1a); j++) { wait_frames(60); tap(L.bstart.x + L.bstart.w / 2, L.bstart.y + L.bstart.h / 2); }
        fprintf(stderr, "  wins you/bot: %d/%d, now %02x/%02x\n", gb_peek(0xFFD7), b_gb_peek(0xFFD7), gs_(), cpu_b_state());
        if (gs_() == 0x17) { wait_frames(90); tap_gb(84 + 6, 48); }
    }
#ifdef SIM_PROFILE
    fprintf(stderr, "bot decisions: %d, avg %.3f ms, max %.2f ms; histogram (<=0.25,0.5,1,2,4,8,16,more ms):", prof_bot_n, prof_bot_sum / 1e6 / (prof_bot_n ? prof_bot_n : 1), prof_bot_max / 1e6);
    for (int i = 0; i < 8; i++) fprintf(stderr, " %lld", (long long)prof_hist[i]); fprintf(stderr, "\n");
#endif
    key(AKEYCODE_BACK); wait_frames(5); key(AKEYCODE_BACK); wait_frames(10);
    fprintf(stderr, "unlinked: linked=%d vs_cpu=%d msg='%s'\n", linked, vs_cpu, lui_msg);
    wait_frames(240);
    fprintf(stderr, "back at %02x (2P flag %d)\n", gs_(), tetris_is_2player());
    shot("cpu_after_unlink");
    destroy();
    return 0;
}

/* ---------------- first run without a ROM ---------------- */
static int run_norom(void) {
    char p[512]; snprintf(p, sizeof p, "%s/rom.gb", outdir); remove(p);
    char sp[512]; snprintf(sp, sizeof sp, "%s/tetris_state.bin", outdir); remove(sp);
    win.copyback = 1;
    create(1080, 2340);
    wait_frames(20);
    fprintf(stderr, "no rom: rom_ready=%d overlay=%d (rom=%d)\n", rom_ready, lui, LUI_ROM);
    shot("r1_need_rom");
    /* taps on the game area / START do nothing harmful */
    tap(L.bstart.x + L.bstart.w / 2, L.bstart.y + L.bstart.h / 2); tap_gb(80, 60);
    overlay_layout();
    /* FIND IN DOWNLOADS on an empty folder */
    char d1[512]; snprintf(d1, sizeof d1, "%s/dl_empty", outdir); mkdir(d1, 0755);
    ROM_DIRS[0] = d1; ROM_DIRS[1] = d1; ROM_DIRS[2] = d1;
    tap(O.b3.x + O.b3.w / 2, O.b3.y + O.b3.h / 2); wait_frames(5);
    fprintf(stderr, "scan empty: rom_ready=%d msg='%s'\n", rom_ready, rom_msg);
    shot("r2_not_found");
    /* a different file that claims to be Tetris (wrong version) */
    char d2[512]; snprintf(d2, sizeof d2, "%s/dl_wrong", outdir); mkdir(d2, 0755);
    { uint8_t r[ROM_SIZE]; rom_check_file(rom_src(), r); r[0x4000] ^= 0x55;
      char f[600]; snprintf(f, sizeof f, "%s/tetris.gb", d2); FILE *o = fopen(f, "wb"); fwrite(r, 1, ROM_SIZE, o); fclose(o); }
    ROM_DIRS[0] = d2;
    tap(O.b3.x + O.b3.w / 2, O.b3.y + O.b3.h / 2); wait_frames(5);
    fprintf(stderr, "scan wrong version: rom_ready=%d msg='%s'\n", rom_ready, rom_msg);
    /* the real one, zipped, in a subfolder */
    char d3[512]; snprintf(d3, sizeof d3, "%s/dl_ok", outdir); mkdir(d3, 0755);
    char d3s[600]; snprintf(d3s, sizeof d3s, "%s/roms", d3); mkdir(d3s, 0755);
    { char cmd[1400]; snprintf(cmd, sizeof cmd, "cd '%s' && cp '%s' t.gb && python3 -c \"import zipfile; z=zipfile.ZipFile('Tetris.zip','w',zipfile.ZIP_DEFLATED); z.write('t.gb','Tetris (World) (Rev A).gb'); z.close()\" && rm t.gb", d3s, rom_src());
      if (system(cmd) != 0) fprintf(stderr, "zip failed\n"); }
    ROM_DIRS[1] = d3;
    tap(O.b3.x + O.b3.w / 2, O.b3.y + O.b3.h / 2); wait_frames(5);
    fprintf(stderr, "scan zip: animation playing=%d rom_ready=%d saved copy=%s\n", insert_anim_t0 != 0, rom_ready, access(p, 0) == 0 ? "yes" : "no");
    msleep(500); shot("r3a_insert"); msleep(800); shot("r3b_click"); msleep(600); shot("r3c_reading");
    wait_frames(120);
    fprintf(stderr, "after the animation: rom_ready=%d overlay=%d\n", rom_ready, lui);
    wait_frames(330);
    fprintf(stderr, "game booted: state %02x (copyright 24/25 or title 07)\n", gb_peek(0xFFE1));
    shot("r3_booted");
    destroy();
    /* relaunch: the private copy is used, no prompt */
    create(1080, 2340); wait_frames(10);
    fprintf(stderr, "relaunch: rom_ready=%d overlay=%d\n", rom_ready, lui);
    destroy();
    /* another window imported it while this one waited */
    remove(p);
    create(1080, 2340); wait_frames(10);
    fprintf(stderr, "waiting: rom_ready=%d\n", rom_ready);
    { uint8_t r[ROM_SIZE]; rom_check_file(rom_src(), r); rom_save(p, r); }
    wait_frames(60);
    fprintf(stderr, "picked up import from elsewhere: animation=%d rom_ready=%d\n", insert_anim_t0 != 0, rom_ready);
    tap(L.screen.x + L.screen.w / 2, L.screen.y + L.screen.h / 2); wait_frames(5);
    fprintf(stderr, "tap skips the animation: rom_ready=%d\n", rom_ready);
    destroy();
    return 0;
}

/* ---------------- CONTROLS panel: sensitivity sliders ---------------- */
static int piece_x(void) { return gb_peek(0xC202); }
static int cols_for_drag(float inches) {
    float cx = L.screen.x + L.screen.w * 0.3f, cy = L.screen.y + L.screen.h * 0.3f;
    int x0 = piece_x();
    drag(cx, cy, cx + dpi * inches, cy, 10, 2); wait_frames(6);
    int d = (piece_x() - x0) / 8;
    drag(cx + dpi * inches, cy, cx, cy, 10, 2); wait_frames(6);   /* back again */
    return d;
}
static void ctl_tap_rc(Rc r) { tap(r.x + r.w / 2, r.y + r.h / 2); wait_frames(3); }
static void set_slider(Rc row, int *v, int target) {
    Rc mi, bar, pl; slider_parts(row, &mi, &bar, &pl);
    for (int k = 0; k < 12 && *v != target; k++) ctl_tap_rc(*v > target ? mi : pl);
}
static int run_ctl(void) {
    char sp[512]; snprintf(sp, sizeof sp, "%s/tetris_state.bin", outdir); remove(sp);
    char setp[600]; snprintf(setp, sizeof setp, "%s/tetris_state.bin.settings", outdir); remove(setp);
    win.copyback = 1;
    create(1080, 2340);
    wait_frames(330);
    for (int i = 0; i < 5 && gb_peek(0xFFE1) != 0x00; i++) { key(AKEYCODE_ENTER); wait_frames(40); }
    fprintf(stderr, "in game: state %02x\n", gb_peek(0xFFE1));
    int res[3], levels[3] = { 1, 5, 10 };
    for (int i = 0; i < 3; i++) {
        tap(L.bctl.x + L.bctl.w / 2, L.bctl.y + L.bctl.h / 2); wait_frames(5);
        if (i == 0) { fprintf(stderr, "panel open: %d (ctl=%d), game paused: %d\n", lui, LUI_CTL, gb_peek(0xFFAB)); shot("ctl_panel"); }
        overlay_layout();
        Rc rows[7], ra, done; ctl_rows(rows); split2(rows[6], &ra, &done);
        set_slider(rows[1], &sens_touch, levels[i]);
        ctl_tap_rc(done);
        if (gb_peek(0xFFAB)) { key(AKEYCODE_ENTER); wait_frames(20); }   /* unpause */
        res[i] = cols_for_drag(0.34f);
        fprintf(stderr, "swipe sensitivity %d: a 0.34 in drag moved %d columns\n", sens_touch, res[i]);
    }
    /* hard drop flick: off / default / max */
    int fl[3] = { 0, 5, 10 }; float dist[3] = { 0.6f, 0.6f, 0.25f };
    for (int i = 0; i < 3; i++) {
        tap(L.bctl.x + L.bctl.w / 2, L.bctl.y + L.bctl.h / 2); wait_frames(5);
        overlay_layout();
        Rc rows[7], ra, done; ctl_rows(rows); split2(rows[6], &ra, &done);
        set_slider(rows[3], &sens_flick, fl[i]);
        if (i == 0) shot("ctl_flick_off");
        ctl_tap_rc(done);
        if (gb_peek(0xFFAB)) { key(AKEYCODE_ENTER); wait_frames(20); }
        int hd = hard_drops;
        flick(L.screen.x + L.screen.w * 0.5f, L.screen.y + L.screen.h * 0.3f, dpi * dist[i]); wait_frames(5);
        fprintf(stderr, "flick setting %d, %.2f in flick: hard drop %s\n", sens_flick, dist[i], hard_drops > hd ? "YES" : "no");
    }
    destroy();
    create(1080, 2340); wait_frames(10);
    fprintf(stderr, "after relaunch: swipe %d flick %d (want 10 10)\n", sens_touch, sens_flick);
    destroy();
    return 0;
}

/* ---------------- audio robustness: app switching, quick in/out, device changes ---------------- */
static int64_t audible_over(int frames) {
    int64_t a0 = audible_frames; int64_t t0 = sim_ns();
    wait_frames(frames);
    double secs = (double)(sim_ns() - t0) / 1e9;
    return (int64_t)((double)(audible_frames - a0) / secs);    /* frames played per second */
}
static int run_audio(void) {
    char sp[512]; snprintf(sp, sizeof sp, "%s/tetris_state.bin", outdir); remove(sp);
    win.copyback = 1;
    create(1080, 2340);
    wait_frames(120);
    fprintf(stderr, "start: %lld frames/s audible (want ~47000-48000)\n", (long long)audible_over(120));
    /* 1: leave and come right back, ten times (requestStart lands while the stream is still pausing) */
    for (int i = 0; i < 10; i++) {
        cbs.onPause(&act); msleep(5 + 3 * i); cbs.onResume(&act); msleep(10);
    }
    { int64_t t0 = sim_ns(); int64_t a0 = audible_frames;
      while (audible_frames == a0 && sim_ns() - t0 < 3000000000LL) msleep(1);
      fprintf(stderr, "after 10 quick in/out: sound back after %.0f ms\n", (double)(sim_ns() - t0) / 1e6); }
    fprintf(stderr, "after 10 quick in/out: %lld frames/s audible\n", (long long)audible_over(120));
    /* 2: a slow away-and-back with the window going away (screen off) */
    cbs.onPause(&act); cbs.onNativeWindowDestroyed(&act, &win); msleep(300);
    cbs.onNativeWindowCreated(&act, &win); cbs.onResume(&act);
    fprintf(stderr, "after screen off/on: %lld frames/s audible\n", (long long)audible_over(120));
    /* 3: the audio device goes away (headphones unplugged / Bluetooth) */
    sim_audio_disconnect();
    fprintf(stderr, "after audio device change: %lld frames/s audible\n", (long long)audible_over(120));
    /* 4: in and out of the link menu and the controls panel */
    for (int i = 0; i < 6; i++) {
        tap_gb(120, 115); wait_frames(5); key(AKEYCODE_BACK); wait_frames(5);
        tap(L.bctl.x + L.bctl.w / 2, L.bctl.y + L.bctl.h / 2); wait_frames(5); key(AKEYCODE_BACK); wait_frames(5);
    }
    fprintf(stderr, "after menus in/out: %lld frames/s audible\n", (long long)audible_over(120));
    destroy();
    return 0;
}

/* ---------------- online presence UI (network faked: net.c's state set directly) ---------------- */
static int run_online_ui(void) {
    char sp[512]; snprintf(sp, sizeof sp, "%s/tetris_state.bin", outdir); remove(sp);
    win.copyback = 1;
    create(1080, 2340);
    wait_frames(330);
    tap_gb(80, 40); wait_frames(40);
    /* pretend the presence connection is up with two free players and one busy */
    pres_connected = 1;
    pthread_mutex_lock(&pmu); peer_seen("aaaaaaaaaaaaaaaa", 'A'); peer_seen("bbbbbbbbbbbbbbbb", 'L'); peer_seen("cccccccccccccccc", 'B'); pthread_mutex_unlock(&pmu);
    tap_gb(120, 115); wait_frames(10);
    fprintf(stderr, "link menu open: %d, free players shown: %d\n", lui == LUI_MENU, net_presence_available());
    shot("o1_menu_dot");
    key(AKEYCODE_BACK); wait_frames(5);
    /* busy while playing solo: start a game */
    for (int i = 0; i < 5 && gb_peek(0xFFE1) != 0x00; i++) { key(AKEYCODE_ENTER); wait_frames(40); }
    wait_frames(5);
    fprintf(stderr, "in a solo game: busy=%d (want 1)\n", pres_busy);
    key(AKEYCODE_ENTER); wait_frames(10);   /* pause: still busy */
    fprintf(stderr, "paused solo game: busy=%d (want 1)\n", pres_busy);
    /* an invitation arriving mid-game must not pop up */
    pthread_mutex_lock(&pmu); sess = SS_PROMPT; sess_t = mono_ns(); snprintf(sess_peer, sizeof sess_peer, "aaaaaaaaaaaaaaaa"); pthread_mutex_unlock(&pmu);
    wait_frames(5);
    fprintf(stderr, "invitation while mid-game: overlay=%d (in-game overlays are suppressed? %s)\n", lui, lui == LUI_INVITE ? "NO, it popped up" : "yes");
    pthread_mutex_lock(&pmu); sess = SS_IDLE; pthread_mutex_unlock(&pmu);
    /* back to the title: free */
    { float bx = L.bstart.x + L.bstart.w / 2, by = L.bstart.y + L.bstart.h / 2;
      touch(AMOTION_EVENT_ACTION_DOWN, 0, bx, by); msleep(1500); touch(AMOTION_EVENT_ACTION_UP, 0, bx, by); }
    wait_frames(400); key(AKEYCODE_ENTER); wait_frames(40);
    fprintf(stderr, "back at title (state %02x): busy=%d (want 0)\n", gb_peek(0xFFE1), pres_busy);
    /* an invitation at the title pops up */
    pthread_mutex_lock(&pmu); sess = SS_PROMPT; sess_t = mono_ns(); snprintf(sess_peer, sizeof sess_peer, "aaaaaaaaaaaaaaaa"); pthread_mutex_unlock(&pmu);
    wait_frames(10);
    fprintf(stderr, "invitation at title: overlay=%d (invite=%d)\n", lui, LUI_INVITE);
    shot("o2_invite");
    overlay_layout();
    tap(O.b6.x + O.b6.w / 2, O.b6.y + O.b6.h / 2); wait_frames(5);
    fprintf(stderr, "after DECLINE: overlay=%d session=%d (idle=%d)\n", lui, sess, SS_IDLE);
    pthread_mutex_lock(&pmu); sess = SS_PROMPT; sess_t = mono_ns(); pthread_mutex_unlock(&pmu);
    wait_frames(10);
    tap(O.b4.x + O.b4.w / 2, O.b4.y + O.b4.h / 2); wait_frames(5);
    fprintf(stderr, "after ACCEPT: overlay=%d (search=%d) status='%s' kind=%d\n", lui, LUI_SEARCH, net_status_text(), net_kind());
    shot("o3_linking");
    net_stop(); lui = LUI_NONE; wait_frames(5);
    /* the invitation runs out */
    pthread_mutex_lock(&pmu); sess = SS_PROMPT; sess_t = mono_ns() - 19000000000LL; pthread_mutex_unlock(&pmu);
    wait_frames(5);
    pthread_mutex_lock(&pmu); sess = SS_IDLE; pthread_mutex_unlock(&pmu);   /* (the network thread would do this) */
    wait_frames(5);
    fprintf(stderr, "expired: overlay=%d msg='%s'\n", lui, lui_msg);
    destroy();
    return 0;
}

/* ---------------- 3.4: keyboard defaults + rebinding, HOME, trackpad, update prompt ---------------- */
static void key_tap(int code) {
    struct AInputEvent e = { AINPUT_EVENT_TYPE_KEY, AKEY_EVENT_ACTION_DOWN }; e.key = code; push(e); wait_frames(3);
    e.action = AKEY_EVENT_ACTION_UP; push(e); wait_frames(3);
}
static void pad_ev(int act, float x, float y) {
    struct AInputEvent e = { AINPUT_EVENT_TYPE_MOTION, act, 1, {0}, {x}, {y} };
    e.source = AINPUT_SOURCE_TOUCHPAD; e.device = 7;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); e.t = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    push(e);
}
static int start_solo_game(void) {
    for (int i = 0; i < 6 && gb_peek(0xFFE1) != 0x00; i++) { key_tap(AKEYCODE_ENTER); wait_frames(40); }
    return gb_peek(0xFFE1) == 0x00;
}
static int run_kbd(void) {
    char sp[512]; snprintf(sp, sizeof sp, "%s/tetris_state.bin", outdir); remove(sp);
    char setp[600]; snprintf(setp, sizeof setp, "%s/tetris_state.bin.settings", outdir); remove(setp);
    win.copyback = 1;
    create(1080, 2340);
    wait_frames(330);
    fprintf(stderr, "solo game started with ENTER: %d\n", start_solo_game());
    int spec = gb_peek(0xC203); key_tap(AKEYCODE_SPACE); wait_frames(4);
    fprintf(stderr, "SPACE rotates: %s\n", gb_peek(0xC203) != spec ? "yes" : "NO");
    int x0 = gb_peek(0xC202); key_tap(AKEYCODE_D); wait_frames(4);
    fprintf(stderr, "D moves right: %s\n", gb_peek(0xC202) > x0 ? "yes" : "NO");
    int hd = hard_drops; key_tap(AKEYCODE_ALT_LEFT); wait_frames(8);
    fprintf(stderr, "ALT hard-drops: %s\n", hard_drops > hd ? "yes" : "NO");
    hd = hard_drops; key_tap(AKEYCODE_DPAD_UP); wait_frames(8);
    fprintf(stderr, "UP no longer hard-drops: %s\n", hard_drops == hd ? "yes" : "NO");
    /* rebind ROTATE to Q through CONTROLS > KEYBOARD KEYS */
    key_tap(AKEYCODE_F1); wait_frames(5);
    fprintf(stderr, "F1 opens CONTROLS: %s\n", lui == LUI_CTL ? "yes" : "NO");
    overlay_layout(); { Rc r[7]; ctl_rows(r); tap(r[5].x + r[5].w / 2, r[5].y + r[5].h / 2); wait_frames(5); }
    fprintf(stderr, "KEYS panel open: %s\n", lui == LUI_KEYS ? "yes" : "NO");
    shot("k1_keys");
    { Rc r[K_N + 1]; keys_rows(r); tap(r[K_ROT_A].x + r[K_ROT_A].w / 2, r[K_ROT_A].y + r[K_ROT_A].h / 2); wait_frames(5);
      fprintf(stderr, "waiting for a key: %s\n", key_capture == K_ROT_A ? "yes" : "NO");
      shot("k2_capture");
      key_tap(AKEYCODE_Q);
      char a[16], b[16]; fprintf(stderr, "ROTATE now: %s / %s\n", key_name(kb[K_ROT_A][0], a), key_name(kb[K_ROT_A][1], b));
      /* binding a key that another action has moves it */
      tap(r[K_DROP].x + r[K_DROP].w / 2, r[K_DROP].y + r[K_DROP].h / 2); wait_frames(5); key_tap(AKEYCODE_D);
      fprintf(stderr, "D taken by HARD DROP, MOVE RIGHT keeps: %s\n", key_name(kb[K_RIGHT][0], a));
      tap(r[K_RIGHT].x + r[K_RIGHT].w / 2, r[K_RIGHT].y + r[K_RIGHT].h / 2); wait_frames(5); key_tap(AKEYCODE_D);   /* and back */
      fprintf(stderr, "D back on MOVE RIGHT: %s, HARD DROP: %s / %s\n", key_name(kb[K_RIGHT][0], a), key_name(kb[K_DROP][0], b), key_name(kb[K_DROP][1], a + 8));
      shot("k3_bound"); }
    key_tap(AKEYCODE_ESCAPE); wait_frames(5); key_tap(AKEYCODE_ESCAPE); wait_frames(5);
    if (gb_peek(0xFFAB)) { key_tap(AKEYCODE_ENTER); wait_frames(20); }
    spec = gb_peek(0xC203); key_tap(AKEYCODE_Q); wait_frames(4);
    fprintf(stderr, "Q rotates after rebinding: %s\n", gb_peek(0xC203) != spec ? "yes" : "NO");
    /* trackpad (captured touchpad events) */
    trackpad_mode = 1; wait_frames(3);
    { int xs = gb_peek(0xC202);
      pad_ev(AMOTION_EVENT_ACTION_DOWN, 500, 300); wait_frames(2);
      for (int i = 1; i <= 10; i++) { pad_ev(AMOTION_EVENT_ACTION_MOVE, 500 + i * 12, 300); wait_frames(1); }   /* 12 mm to the right */
      pad_ev(AMOTION_EVENT_ACTION_UP, 620, 300); wait_frames(6);
      fprintf(stderr, "trackpad drag 12 mm right: moved %d columns\n", (gb_peek(0xC202) - xs) / 8); }
    { int sp2 = gb_peek(0xC203); pad_ev(AMOTION_EVENT_ACTION_DOWN, 700, 300); wait_frames(2); pad_ev(AMOTION_EVENT_ACTION_UP, 700, 300); wait_frames(6);
      fprintf(stderr, "trackpad tap rotates: %s\n", gb_peek(0xC203) != sp2 ? "yes" : "NO"); }
    { int hd2 = hard_drops; pad_ev(AMOTION_EVENT_ACTION_DOWN, 500, 200); msleep(16);
      for (int i = 1; i <= 4; i++) { pad_ev(AMOTION_EVENT_ACTION_MOVE, 500, 200 + i * 45); msleep(16); }
      pad_ev(AMOTION_EVENT_ACTION_UP, 500, 380); wait_frames(8);
      fprintf(stderr, "trackpad flick down 18 mm: hard drop %s\n", hard_drops > hd2 ? "yes" : "NO"); }
    trackpad_mode = 0;
    /* HOME mid-game asks first; YES lands on the title */
    tap(L.bhome.x + L.bhome.w / 2, L.bhome.y + L.bhome.h / 2); wait_frames(5);
    fprintf(stderr, "HOME mid-game asks first: %s\n", lui == LUI_HOME ? "yes" : "NO");
    shot("k4_home_confirm");
    overlay_layout(); tap(O.b4.x + O.b4.w / 2, O.b4.y + O.b4.h / 2);
    { int w = 0; while (gb_peek(0xFFE1) != 0x07 && w < 900) { wait_frames(10); w += 10; }
      fprintf(stderr, "HOME -> title after %d frames (state %02x)\n", w, gb_peek(0xFFE1)); }
    /* HOME from a menu goes straight there */
    key_tap(AKEYCODE_ENTER); wait_frames(30);
    fprintf(stderr, "in the game-type menu: state %02x\n", gb_peek(0xFFE1));
    key_tap(AKEYCODE_MOVE_HOME);
    { int w = 0; while (gb_peek(0xFFE1) != 0x07 && w < 900) { wait_frames(10); w += 10; }
      fprintf(stderr, "Home key from a menu -> title after %d frames, no question asked: %s\n", w, lui == LUI_NONE ? "yes" : "NO"); }
    /* update prompt */
    update_test_set_latest("3.5");
    wait_frames(10);
    fprintf(stderr, "update prompt shown for 3.5: %s\n", lui == LUI_UPDATE ? "yes" : "NO");
    shot("k5_update");
    overlay_layout(); tap(O.b6.x + O.b6.w / 2, O.b6.y + O.b6.h / 2); wait_frames(5);
    fprintf(stderr, "LATER closes it and remembers %ld\n", update_skip);
    destroy();
    create(1080, 2340); wait_frames(20);
    update_test_set_latest("3.5"); wait_frames(10);
    char a[16], b[16];
    fprintf(stderr, "after relaunch: ROTATE %s / %s, prompt for skipped 3.5 again: %s\n", key_name(kb[K_ROT_A][0], a), key_name(kb[K_ROT_A][1], b), lui == LUI_UPDATE ? "YES (bad)" : "no");
    destroy();
    return 0;
}
