/* iOS implementation of the Android APIs the shell in android/main.c uses.
 *
 * This is the same idea as test/sim.c (which fakes the same surface on the
 * desktop): main.c keeps running unmodified, rendering into a locked pixel
 * buffer, reading AInputEvents and writing to an AAudio stream.
 *
 * Rendering: three rotating buffers, handed to UIKit without a copy. UIKit
 * calls ios_buffer_done() when CoreGraphics is finished with the frame.
 * Audio: AudioQueue pulls from a ring buffer; AAudioStream_write() blocks
 * until there is room, which is what paces the game loop. */

#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/input.h>
#include <android/looper.h>
#include <android/configuration.h>
#include <android/window.h>
#include <aaudio/AAudio.h>

#include <AudioToolbox/AudioToolbox.h>
#include <pthread.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ios_glue.h"

#define LOG_TAG "Greenscreen89"

/* forward: defined further down, used by the window and input sections */
static ANativeWindow *g_win;
static AInputQueue *g_queue;

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    (void)prio;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ios_log("[%s] %s", tag ? tag : LOG_TAG, buf);
    return 0;
}

/* ------------------------------------------------------------------ */
/* window: three rotating 32-bit buffers                              */
/* ------------------------------------------------------------------ */
#define NBUF 3

struct ANativeWindow {
    int w, h;
    unsigned char *px[NBUF];
    int busy[NBUF];
    int cur;
    int locked;
    pthread_mutex_t mu;
};

static ANativeWindow *win_new(int w, int h) {
    ANativeWindow *win = calloc(1, sizeof *win);
    if (!win) return NULL;
    win->w = w;
    win->h = h;
    pthread_mutex_init(&win->mu, NULL);
    for (int i = 0; i < NBUF; i++) win->px[i] = calloc((size_t)w * h, 4);
    return win;
}

__attribute__((unused)) static void win_free(ANativeWindow *win) {
    if (!win) return;
    for (int i = 0; i < NBUF; i++) free(win->px[i]);
    pthread_mutex_destroy(&win->mu);
    free(win);
}

int32_t ANativeWindow_getWidth(ANativeWindow *window) { return window ? window->w : 0; }
int32_t ANativeWindow_getHeight(ANativeWindow *window) { return window ? window->h : 0; }

int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *window, int32_t width, int32_t height, int32_t format) {
    (void)window; (void)width; (void)height; (void)format;
    return 0;
}

int32_t ANativeWindow_lock(ANativeWindow *window, ANativeWindow_Buffer *out, ARect *dirty) {
    if (!window) return -1;
    pthread_mutex_lock(&window->mu);
    if (window->locked) { pthread_mutex_unlock(&window->mu); return -1; }
    int idx = -1;
    for (int i = 0; i < NBUF; i++) {
        int j = (window->cur + i) % NBUF;
        if (!window->busy[j]) { idx = j; break; }
    }
    if (idx < 0) idx = window->cur;          /* all in flight: reuse the oldest */
    window->cur = idx;
    window->locked = 1;
    out->width = window->w;
    out->height = window->h;
    out->stride = window->w;
    out->format = WINDOW_FORMAT_RGBX_8888;
    out->bits = window->px[idx];
    if (dirty) {
        if (dirty->left < 0) dirty->left = 0;
        if (dirty->top < 0) dirty->top = 0;
        if (dirty->right > window->w) dirty->right = window->w;
        if (dirty->bottom > window->h) dirty->bottom = window->h;
    }
    pthread_mutex_unlock(&window->mu);
    return 0;
}

int32_t ANativeWindow_unlockAndPost(ANativeWindow *window) {
    if (!window) return -1;
    {   /* one line of geometry so the layout can be checked from CI logs */
        static int logged;
        if (!logged) { logged = 1; ios_log("first frame %dx%d stride %d", window->w, window->h, window->w); }
    }
    pthread_mutex_lock(&window->mu);
    if (!window->locked) { pthread_mutex_unlock(&window->mu); return -1; }
    window->locked = 0;
    int idx = window->cur;
    window->busy[idx] = 1;
    void *px = window->px[idx];
    int w = window->w, h = window->h;
    pthread_mutex_unlock(&window->mu);
    ios_present(px, w, h, px);
    return 0;
}

/* UIKit is done with a frame buffer */
void ios_buffer_done(void *buf) {
    ANativeWindow *win = g_win;
    if (!win || !buf) return;
    pthread_mutex_lock(&win->mu);
    for (int i = 0; i < NBUF; i++)
        if (win->px[i] == buf) win->busy[i] = 0;
    pthread_mutex_unlock(&win->mu);
}

/* ------------------------------------------------------------------ */
/* input queue                                                        */
/* ------------------------------------------------------------------ */
#define QCAP 256
#define MAXP_EV 10

struct AInputEvent {
    int type, action, source, device;
    int n;
    int ids[MAXP_EV];
    float xs[MAXP_EV], ys[MAXP_EV];
    int key, repeat;
    int64_t t;
};

struct AInputQueue {
    pthread_mutex_t mu;
    struct AInputEvent q[QCAP];
    int head, tail;
    int attached;
    int ident;              /* the id the shell passed to attachLooper() */
};

static AInputQueue *queue_new(void) {
    AInputQueue *q = calloc(1, sizeof *q);
    if (!q) return NULL;
    pthread_mutex_init(&q->mu, NULL);
    q->ident = 1;               /* overwritten by the id attachLooper() gets */
    return q;
}

static void queue_push(AInputQueue *q, const struct AInputEvent *ev) {
    if (!q) return;
    pthread_mutex_lock(&q->mu);
    if (q->tail - q->head < QCAP) q->q[q->tail % QCAP] = *ev;
    q->tail++;
    pthread_mutex_unlock(&q->mu);
}

static int queue_pending(AInputQueue *q) {
    if (!q) return 0;
    pthread_mutex_lock(&q->mu);
    int has = q->tail != q->head;
    pthread_mutex_unlock(&q->mu);
    return has;
}

void AInputQueue_attachLooper(AInputQueue *queue, void *looper, int ident, void *callback, void *data) {
    (void)looper; (void)callback; (void)data;
    if (!queue) return;
    queue->ident = ident;
    queue->attached = 1;
}

void AInputQueue_detachLooper(AInputQueue *queue) {
    if (queue) queue->attached = 0;
}

int32_t AInputQueue_getEvent(AInputQueue *queue, AInputEvent **outEvent) {
    if (!queue) return -1;
    pthread_mutex_lock(&queue->mu);
    if (queue->head == queue->tail) { pthread_mutex_unlock(&queue->mu); return -1; }
    *outEvent = &queue->q[queue->head % QCAP];
    queue->head++;
    pthread_mutex_unlock(&queue->mu);
    return 0;
}

int32_t AInputQueue_preDispatchEvent(AInputQueue *queue, AInputEvent *event) { (void)queue; (void)event; return 0; }
void AInputQueue_finishEvent(AInputQueue *queue, AInputEvent *event, int handled) { (void)queue; (void)event; (void)handled; }

int32_t AInputEvent_getType(const AInputEvent *e) { return e->type; }
int32_t AInputEvent_getSource(const AInputEvent *e) { return e->source; }
int32_t AInputEvent_getDeviceId(const AInputEvent *e) { return e->device; }

int32_t AMotionEvent_getAction(const AInputEvent *e) { return e->action; }
int64_t AMotionEvent_getEventTime(const AInputEvent *e) { return e->t; }
size_t AMotionEvent_getPointerCount(const AInputEvent *e) { return (size_t)e->n; }
int32_t AMotionEvent_getPointerId(const AInputEvent *e, size_t i) { return (i < (size_t)e->n) ? e->ids[i] : 0; }
float AMotionEvent_getX(const AInputEvent *e, size_t i) { return (i < (size_t)e->n) ? e->xs[i] : 0.f; }
float AMotionEvent_getY(const AInputEvent *e, size_t i) { return (i < (size_t)e->n) ? e->ys[i] : 0.f; }

int32_t AKeyEvent_getAction(const AInputEvent *e) { return e->action; }
int32_t AKeyEvent_getKeyCode(const AInputEvent *e) { return e->key; }
int32_t AKeyEvent_getRepeatCount(const AInputEvent *e) { return e->repeat; }

/* ---- touch bookkeeping: UIKit touch -> Android pointer events ---- */
static struct { int id; float x, y; } touches[MAXP_EV];
static int ntouch;
static int touch_next_id;

static int touch_slot(int id) {
    for (int i = 0; i < ntouch; i++) if (touches[i].id == id) return i;
    return -1;
}

static void motion_push(int action, int index) {
    struct AInputEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = AINPUT_EVENT_TYPE_MOTION;
    ev.source = AINPUT_SOURCE_TOUCHSCREEN;
    ev.action = (action & AMOTION_EVENT_ACTION_MASK) | (index << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    ev.n = ntouch;
    ev.t = now_ns();
    for (int i = 0; i < ntouch; i++) {
        ev.ids[i] = touches[i].id;
        ev.xs[i] = touches[i].x;
        ev.ys[i] = touches[i].y;
    }
    queue_push(g_queue, &ev);
}

void ios_touch_begin(int id, float x, float y) {
    if (ntouch >= MAXP_EV) return;
    int slot = touch_slot(id);
    if (slot >= 0) { touches[slot].x = x; touches[slot].y = y; motion_push(AMOTION_EVENT_ACTION_MOVE, 0); return; }
    touches[ntouch].id = id;
    touches[ntouch].x = x;
    touches[ntouch].y = y;
    ntouch++;
    motion_push(ntouch == 1 ? AMOTION_EVENT_ACTION_DOWN : AMOTION_EVENT_ACTION_POINTER_DOWN, ntouch - 1);
}

void ios_touch_move(int id, float x, float y) {
    int slot = touch_slot(id);
    if (slot < 0) return;
    touches[slot].x = x;
    touches[slot].y = y;
    motion_push(AMOTION_EVENT_ACTION_MOVE, 0);
}

void ios_touch_end(int id, float x, float y) {
    int slot = touch_slot(id);
    if (slot < 0) return;
    touches[slot].x = x;
    touches[slot].y = y;
    int last = (ntouch == 1);
    motion_push(last ? AMOTION_EVENT_ACTION_UP : AMOTION_EVENT_ACTION_POINTER_UP, slot);
    for (int i = slot; i < ntouch - 1; i++) touches[i] = touches[i + 1];
    ntouch--;
}

void ios_touch_cancel(void) {
    if (ntouch == 0) return;
    motion_push(AMOTION_EVENT_ACTION_CANCEL, 0);
    ntouch = 0;
}

/* ------------------------------------------------------------------ */
/* looper                                                             */
/* ------------------------------------------------------------------ */
#define MAX_FDS 8

struct ALooper {
    int n;
    struct { int fd; ALooper_callbackFunc cb; void *data; } fd[MAX_FDS];
    pthread_mutex_t mu;
};

static ALooper g_looper = { .n = 0, .mu = PTHREAD_MUTEX_INITIALIZER };

ALooper *ALooper_prepare(int opts) { (void)opts; return &g_looper; }
ALooper *ALooper_forThread(void) { return &g_looper; }

int ALooper_addFd(ALooper *looper, int fd, int ident, int events, ALooper_callbackFunc callback, void *data) {
    (void)ident; (void)events;
    if (!looper || fd < 0) return -1;
    pthread_mutex_lock(&looper->mu);
    if (looper->n < MAX_FDS) {
        looper->fd[looper->n].fd = fd;
        looper->fd[looper->n].cb = callback;
        looper->fd[looper->n].data = data;
        looper->n++;
    }
    pthread_mutex_unlock(&looper->mu);
    return 1;
}

int ALooper_pollOnce(int timeoutMillis, int *outFd, int *outEvents, void **outData) {
    if (outFd) *outFd = -1;
    if (outEvents) *outEvents = 0;
    if (outData) *outData = NULL;
    if (queue_pending(g_queue)) {
        if (outEvents) *outEvents = ALOOPER_EVENT_INPUT;
        return g_queue->ident;      /* must match the shell's LOOPER_ID_INPUT */
    }
    struct pollfd pfd[MAX_FDS];
    ALooper_callbackFunc cb[MAX_FDS];
    void *data[MAX_FDS];
    int n = 0;
    pthread_mutex_lock(&g_looper.mu);
    for (int i = 0; i < g_looper.n && n < MAX_FDS; i++) {
        pfd[n].fd = g_looper.fd[i].fd;
        pfd[n].events = POLLIN;
        pfd[n].revents = 0;
        cb[n] = g_looper.fd[i].cb;
        data[n] = g_looper.fd[i].data;
        n++;
    }
    pthread_mutex_unlock(&g_looper.mu);
    if (n == 0) {
        if (timeoutMillis > 0) {
            struct timespec ts = { timeoutMillis / 1000, (long)(timeoutMillis % 1000) * 1000000L };
            nanosleep(&ts, NULL);
        }
        return ALOOPER_POLL_TIMEOUT;
    }
    int r = poll(pfd, (nfds_t)n, timeoutMillis);
    if (r < 0) return ALOOPER_POLL_ERROR;
    if (r == 0) return ALOOPER_POLL_TIMEOUT;
    for (int i = 0; i < n; i++) {
        if (pfd[i].revents) {
            int events = 0;
            if (pfd[i].revents & POLLIN) events |= ALOOPER_EVENT_INPUT;
            if (pfd[i].revents & (POLLERR | POLLNVAL)) events |= ALOOPER_EVENT_ERROR;
            if (pfd[i].revents & POLLHUP) events |= ALOOPER_EVENT_HANGUP;
            if (cb[i]) cb[i](pfd[i].fd, events, data[i]);
            if (outFd) *outFd = pfd[i].fd;
            if (outEvents) *outEvents = events;
            if (outData) *outData = data[i];
            return ALOOPER_POLL_CALLBACK;
        }
    }
    return ALOOPER_POLL_TIMEOUT;
}

/* ------------------------------------------------------------------ */
/* configuration                                                      */
/* ------------------------------------------------------------------ */
struct AConfiguration { int density; };

AConfiguration *AConfiguration_new(void) { return calloc(1, sizeof(AConfiguration)); }
void AConfiguration_delete(AConfiguration *config) { free(config); }
void AConfiguration_fromAssetManager(AConfiguration *out, AAssetManager *m) {
    (void)m;
    if (out) out->density = (int)(ios_dpi() + 0.5);
}
int32_t AConfiguration_getDensity(AConfiguration *config) { return config ? config->density : 0; }

/* ------------------------------------------------------------------ */
/* audio: AAudio over AudioQueue                                      */
/* ------------------------------------------------------------------ */
/* one 4096-frame write from the shell must fit without dropping audio; a
 * bigger ring would only add latency between the picture and the sound */
#define RING_FRAMES 4096
#define AQ_BUFFERS 3
#define AQ_FRAMES 1024

struct AAudioStreamBuilder {
    int32_t sample_rate, channels, format;
    aaudio_direction_t direction;
};

struct AAudioStream {
    int32_t sample_rate, channels;
    aaudio_stream_state_t state;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int16_t *ring;                  /* interleaved stereo */
    int cap, head, count;
    int64_t frames_read;
    AudioQueueRef queue;
    AudioQueueBufferRef buf[AQ_BUFFERS];
    int started;
};

static void aq_fill(void *user, AudioQueueRef q, AudioQueueBufferRef b) {
    AAudioStream *s = user;
    int16_t *out = (int16_t *)b->mAudioData;
    int frames = (int)(b->mAudioDataBytesCapacity / 4);
    int n = 0;
    pthread_mutex_lock(&s->mu);
    while (n < frames && s->count > 0) {
        out[2 * n] = s->ring[2 * s->head];
        out[2 * n + 1] = s->ring[2 * s->head + 1];
        s->head = (s->head + 1) % s->cap;
        s->count--;
        n++;
    }
    if (n < frames) memset(out + 2 * n, 0, (size_t)(frames - n) * 4);
    /* the device position advances even through silence, like AAudio: the
     * shell uses getFramesRead() to notice a stalled stream */
    s->frames_read += frames;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);
    b->mAudioDataByteSize = (UInt32)(frames * 4);
    AudioQueueEnqueueBuffer(q, b, 0, NULL);
}

static void aq_start(AAudioStream *s) {
    if (s->started) {
        AudioQueueStart(s->queue, NULL);
        return;
    }
    AudioStreamBasicDescription fmt;
    memset(&fmt, 0, sizeof fmt);
    fmt.mSampleRate = s->sample_rate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    fmt.mBytesPerPacket = 4;
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = 4;
    fmt.mChannelsPerFrame = 2;
    fmt.mBitsPerChannel = 16;
    if (AudioQueueNewOutput(&fmt, aq_fill, s, NULL, NULL, 0, &s->queue) != noErr) {
        s->queue = NULL;
        return;
    }
    for (int i = 0; i < AQ_BUFFERS; i++) {
        if (AudioQueueAllocateBuffer(s->queue, AQ_FRAMES * 4, &s->buf[i]) != noErr) continue;
        memset(s->buf[i]->mAudioData, 0, AQ_FRAMES * 4);
        s->buf[i]->mAudioDataByteSize = AQ_FRAMES * 4;
        AudioQueueEnqueueBuffer(s->queue, s->buf[i], 0, NULL);
    }
    s->started = 1;
    AudioQueueStart(s->queue, NULL);
}

static void aq_stop(AAudioStream *s, int flush) {
    if (!s->queue) return;
    AudioQueueStop(s->queue, true);
    if (flush) { pthread_mutex_lock(&s->mu); s->head = 0; s->count = 0; pthread_mutex_unlock(&s->mu); }
}

aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder **builder) {
    *builder = calloc(1, sizeof(AAudioStreamBuilder));
    return *builder ? AAUDIO_OK : -1;
}

void AAudioStreamBuilder_setDirection(AAudioStreamBuilder *b, aaudio_direction_t d) { if (b) b->direction = d; }
void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder *b, int32_t r) { if (b) b->sample_rate = r; }
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder *b, int32_t c) { if (b) b->channels = c; }
void AAudioStreamBuilder_setFormat(AAudioStreamBuilder *b, aaudio_format_t f) { if (b) b->format = f; }
void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder *b, aaudio_performance_mode_t m) { (void)b; (void)m; }
void AAudioStreamBuilder_setSharingMode(AAudioStreamBuilder *b, aaudio_sharing_mode_t m) { (void)b; (void)m; }
void AAudioStreamBuilder_setUsage(AAudioStreamBuilder *b, aaudio_usage_t u) { (void)b; (void)u; }
void AAudioStreamBuilder_setContentType(AAudioStreamBuilder *b, aaudio_content_type_t c) { (void)b; (void)c; }

aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder *b, AAudioStream **stream) {
    if (!b || !stream) return -1;
    AAudioStream *s = calloc(1, sizeof *s);
    if (!s) return -1;
    s->sample_rate = b->sample_rate ? b->sample_rate : 48000;
    s->channels = 2;
    s->cap = RING_FRAMES;
    s->ring = calloc((size_t)s->cap * 2, sizeof(int16_t));
    s->state = AAUDIO_STREAM_STATE_OPEN;
    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv, NULL);
    *stream = s;
    return AAUDIO_OK;
}

aaudio_result_t AAudioStreamBuilder_delete(AAudioStreamBuilder *b) { free(b); return AAUDIO_OK; }

int32_t AAudioStream_getSampleRate(AAudioStream *s) { return s ? s->sample_rate : 0; }
int32_t AAudioStream_getChannelCount(AAudioStream *s) { return s ? s->channels : 0; }
aaudio_format_t AAudioStream_getFormat(AAudioStream *s) { (void)s; return AAUDIO_FORMAT_PCM_I16; }
int32_t AAudioStream_getFramesPerBurst(AAudioStream *s) { (void)s; return AQ_FRAMES; }
aaudio_result_t AAudioStream_setBufferSizeInFrames(AAudioStream *s, int32_t n) { (void)s; return n; }
aaudio_stream_state_t AAudioStream_getState(AAudioStream *s) { return s ? s->state : AAUDIO_STREAM_STATE_CLOSED; }
int64_t AAudioStream_getFramesRead(AAudioStream *s) { return s ? s->frames_read : 0; }

aaudio_result_t AAudioStream_requestStart(AAudioStream *s) {
    if (!s) return AAUDIO_ERROR_INVALID_STATE;
    if (s->state == AAUDIO_STREAM_STATE_DISCONNECTED || s->state == AAUDIO_STREAM_STATE_CLOSED) return AAUDIO_ERROR_DISCONNECTED;
    s->state = AAUDIO_STREAM_STATE_STARTED;
    aq_start(s);
    return AAUDIO_OK;
}

aaudio_result_t AAudioStream_requestPause(AAudioStream *s) {
    if (!s) return AAUDIO_ERROR_INVALID_STATE;
    if (s->state != AAUDIO_STREAM_STATE_CLOSED && s->state != AAUDIO_STREAM_STATE_DISCONNECTED) {
        aq_stop(s, 0);
        s->state = AAUDIO_STREAM_STATE_PAUSED;
    }
    return AAUDIO_OK;
}

aaudio_result_t AAudioStream_requestStop(AAudioStream *s) {
    if (!s) return AAUDIO_ERROR_INVALID_STATE;
    aq_stop(s, 1);
    s->state = AAUDIO_STREAM_STATE_STOPPED;
    return AAUDIO_OK;
}

aaudio_result_t AAudioStream_close(AAudioStream *s) {
    if (!s) return AAUDIO_OK;
    aq_stop(s, 1);
    if (s->queue) { AudioQueueDispose(s->queue, true); s->queue = NULL; }
    s->state = AAUDIO_STREAM_STATE_CLOSED;
    pthread_mutex_destroy(&s->mu);
    pthread_cond_destroy(&s->cv);
    free(s->ring);
    free(s);
    return AAUDIO_OK;
}

/* Blocks until the frames are queued or the timeout expires (like AAudio),
 * which is what paces the emulation loop. Returns the frames written. */
aaudio_result_t AAudioStream_write(AAudioStream *s, const void *buffer, int32_t numFrames, int64_t timeoutNanoseconds) {
    if (!s || !buffer) return AAUDIO_ERROR_INVALID_STATE;
    const int16_t *src = buffer;
    int64_t deadline = now_ns() + timeoutNanoseconds;
    int done = 0;
    pthread_mutex_lock(&s->mu);
    while (done < numFrames) {
        if (s->state != AAUDIO_STREAM_STATE_STARTED) break;
        int room = s->cap - s->count;
        if (room <= 0) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            int64_t left = deadline - now_ns();
            if (left <= 0) break;
            ts.tv_nsec += (long)(left % 1000000000LL);
            ts.tv_sec += (time_t)(left / 1000000000LL);
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&s->cv, &s->mu, &ts);
            continue;
        }
        int k = numFrames - done;
        if (k > room) k = room;
        for (int i = 0; i < k; i++) {
            int slot = (s->head + s->count) % s->cap;
            s->ring[2 * slot] = src[2 * (done + i)];
            s->ring[2 * slot + 1] = src[2 * (done + i) + 1];
            s->count++;
        }
        done += k;
    }
    pthread_mutex_unlock(&s->mu);
    return done;
}

/* ------------------------------------------------------------------ */
/* activity                                                            */
/* ------------------------------------------------------------------ */
static ANativeActivity g_activity;
static ANativeActivityCallbacks g_callbacks;
static const char *g_data_path;
static int g_started, g_destroyed, g_finished;

/* the one JNI call the shell makes outside #ifdef __ANDROID__ is pointer
 * capture for trackpad mode; there is no pointer to capture here */
static jint jni_push(JNIEnv *e, jint c) { (void)e; (void)c; return JNI_OK; }
static jobject jni_pop(JNIEnv *e, jobject r) { (void)e; return r; }
static jclass jni_find(JNIEnv *e, const char *n) { (void)e; (void)n; return NULL; }
static jmethodID jni_mid(JNIEnv *e, jclass c, const char *n, const char *s) { (void)e; (void)c; (void)n; (void)s; return NULL; }
static jobject jni_call_obj(JNIEnv *e, jobject o, jmethodID m, ...) { (void)e; (void)o; (void)m; return NULL; }
static jobject jni_call_static_obj(JNIEnv *e, jclass c, jmethodID m, ...) { (void)e; (void)c; (void)m; return NULL; }
static void jni_call_void(JNIEnv *e, jobject o, jmethodID m, ...) { (void)e; (void)o; (void)m; }
static jboolean jni_call_bool(JNIEnv *e, jobject o, jmethodID m, ...) { (void)e; (void)o; (void)m; return JNI_FALSE; }
static jfloat jni_call_float(JNIEnv *e, jobject o, jmethodID m, ...) { (void)e; (void)o; (void)m; return 0.f; }
static jboolean jni_exc(JNIEnv *e) { (void)e; return JNI_TRUE; }
static void jni_exc_clear(JNIEnv *e) { (void)e; }
static void jni_delete(JNIEnv *e, jobject r) { (void)e; (void)r; }
static jstring jni_new_string(JNIEnv *e, const char *s) { (void)e; (void)s; return NULL; }
static const char *jni_get_chars(JNIEnv *e, jstring s, jboolean *c) { (void)e; (void)s; (void)c; return NULL; }
static void jni_release_chars(JNIEnv *e, jstring s, const char *c) { (void)e; (void)s; (void)c; }

static const struct JNINativeInterface g_jni_table = {
    jni_push, jni_pop, jni_find, jni_mid, jni_mid,
    jni_call_obj, jni_call_static_obj, jni_call_void, jni_call_bool, jni_call_float,
    jni_exc, jni_exc_clear, jni_delete, jni_new_string, jni_get_chars, jni_release_chars,
};
static JNIEnv g_jni_env = &g_jni_table;

void ios_android_create(void) {
    if (g_started) return;
    g_started = 1;
    memset(&g_activity, 0, sizeof g_activity);
    memset(&g_callbacks, 0, sizeof g_callbacks);
    g_data_path = ios_data_path();
    g_activity.callbacks = &g_callbacks;
    g_activity.env = &g_jni_env;
    g_activity.internalDataPath = g_data_path;
    g_activity.externalDataPath = g_data_path;
    g_activity.sdkVersion = 34;
    g_activity.instance = NULL;
    g_queue = queue_new();
    ANativeActivity_onCreate(&g_activity, NULL, 0);
    if (g_callbacks.onStart) g_callbacks.onStart(&g_activity);
    if (g_callbacks.onResume) g_callbacks.onResume(&g_activity);
}

void ios_android_size(int w, int h) {
    if (!g_started || g_destroyed || w <= 0 || h <= 0) return;
    ios_log("window request %dx%d dpi %.0f", w, h, ios_dpi());
    if (g_win && g_win->w == w && g_win->h == h) return;
    int old_w = g_win ? g_win->w : 0, old_h = g_win ? g_win->h : 0;
    if (g_win) {
        if (g_callbacks.onNativeWindowDestroyed) g_callbacks.onNativeWindowDestroyed(&g_activity, g_win);
        g_win = NULL;                       /* the shim leaks the old one on purpose: frames may be in flight */
    }
    ANativeWindow *win = win_new(w, h);
    if (!win) return;
    g_win = win;
    if (g_callbacks.onNativeWindowCreated) g_callbacks.onNativeWindowCreated(&g_activity, win);
    if (g_callbacks.onNativeWindowResized && old_w && old_h) g_callbacks.onNativeWindowResized(&g_activity, win);
    if (g_callbacks.onInputQueueCreated) g_callbacks.onInputQueueCreated(&g_activity, g_queue);
    ARect r = { 0, 0, w, h };
    if (g_callbacks.onContentRectChanged) g_callbacks.onContentRectChanged(&g_activity, &r);
    if (g_callbacks.onWindowFocusChanged) g_callbacks.onWindowFocusChanged(&g_activity, 1);
}

void ios_android_pause(void) {
    if (!g_started || g_destroyed) return;
    if (g_callbacks.onWindowFocusChanged) g_callbacks.onWindowFocusChanged(&g_activity, 0);
    if (g_callbacks.onPause) g_callbacks.onPause(&g_activity);
}

void ios_android_resume(void) {
    if (!g_started || g_destroyed) return;
    if (g_callbacks.onResume) g_callbacks.onResume(&g_activity);
    if (g_win) {
        ARect r = { 0, 0, g_win->w, g_win->h };
        if (g_callbacks.onContentRectChanged) g_callbacks.onContentRectChanged(&g_activity, &r);
        if (g_callbacks.onWindowFocusChanged) g_callbacks.onWindowFocusChanged(&g_activity, 1);
    }
}

void ios_android_destroy(void) {
    if (!g_started || g_destroyed) return;
    g_destroyed = 1;
    if (g_callbacks.onWindowFocusChanged) g_callbacks.onWindowFocusChanged(&g_activity, 0);
    if (g_callbacks.onPause) g_callbacks.onPause(&g_activity);
    if (g_callbacks.onStop) g_callbacks.onStop(&g_activity);
    if (g_callbacks.onNativeWindowDestroyed && g_win) g_callbacks.onNativeWindowDestroyed(&g_activity, g_win);
    if (g_callbacks.onInputQueueDestroyed) g_callbacks.onInputQueueDestroyed(&g_activity, g_queue);
    if (g_callbacks.onDestroy) g_callbacks.onDestroy(&g_activity);
}

void ANativeActivity_finish(ANativeActivity *activity) {
    (void)activity;
    if (g_finished) return;
    g_finished = 1;
    ios_finish();
}

void ANativeActivity_setWindowFlags(ANativeActivity *activity, uint32_t addFlags, uint32_t removeFlags) {
    (void)activity; (void)addFlags; (void)removeFlags;
}
