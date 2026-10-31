/* The contract between the C shim and the UIKit host.
 *
 * shim.c implements the Android API surface the app was written against
 * (window, input, looper, AAudio, activity callbacks); main.m provides the
 * handful of UIKit-side services it cannot provide itself. */
#ifndef IOS_GLUE_H
#define IOS_GLUE_H

#include <stddef.h>

/* ---- implemented by main.m, called from shim.c ---- */
/* present a finished frame: px is w*h*4 bytes in R,G,B,X memory order.
 * Ownership of buf (an opaque shim buffer slot) passes to the host until it
 * calls ios_buffer_done(). May be called from the game thread. */
void ios_present(const void *px, int w, int h, void *buf);
/* main.m must call this exactly once per ios_present(), when it is done with
 * the pixels (shim.c gives the buffer back to the renderer) */
void ios_buffer_done(void *buf);
void ios_finish(void);
void ios_open_url(const char *url);
int ios_clipboard(char *out, int max);
/* a writable directory that survives launches (the app's Documents folder) */
const char *ios_data_path(void);
double ios_dpi(void);
void ios_log(const char *fmt, ...);

/* ---- implemented by shim.c, called from main.m ---- */
void ios_android_create(void);
void ios_android_size(int w, int h);
void ios_android_pause(void);
void ios_android_resume(void);
void ios_android_destroy(void);
void ios_touch_begin(int id, float x, float y);
void ios_touch_move(int id, float x, float y);
void ios_touch_end(int id, float x, float y);
void ios_touch_cancel(void);

#endif
