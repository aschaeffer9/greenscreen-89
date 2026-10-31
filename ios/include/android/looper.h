/* Minimal ALooper for the iOS port: poll(2) over registered fds plus the
 * input queue. */
#ifndef IOS_ANDROID_LOOPER_H
#define IOS_ANDROID_LOOPER_H
#include <stdint.h>

typedef struct ALooper ALooper;
typedef int (*ALooper_callbackFunc)(int fd, int events, void *data);

enum {
    ALOOPER_PREPARE_ALLOW_NON_CALLBACKS = 1 << 0,
};

enum {
    ALOOPER_POLL_WAKE = -1,
    ALOOPER_POLL_CALLBACK = -2,
    ALOOPER_POLL_TIMEOUT = -3,
    ALOOPER_POLL_ERROR = -4,
};

enum {
    ALOOPER_EVENT_INPUT = 1 << 0,
    ALOOPER_EVENT_OUTPUT = 1 << 1,
    ALOOPER_EVENT_ERROR = 1 << 2,
    ALOOPER_EVENT_HANGUP = 1 << 3,
    ALOOPER_EVENT_INVALID = 1 << 4,
};

/* main.c defines LOOPER_ID_INPUT itself (the NDK does not), so the shim uses
 * whatever id the shell passes to AInputQueue_attachLooper() instead of
 * hard-coding one here. */

ALooper *ALooper_prepare(int opts);
int ALooper_pollOnce(int timeoutMillis, int *outFd, int *outEvents, void **outData);
int ALooper_addFd(ALooper *looper, int fd, int ident, int events, ALooper_callbackFunc callback, void *data);
ALooper *ALooper_forThread(void);

#endif
