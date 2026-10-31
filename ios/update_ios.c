/* The update checker is Android/JNI-shaped; on iOS it is off for now.
 * (The app only shows its update panel when update_available() is true.) */

#include <stdlib.h>
#include <string.h>

#include "update.h"

void update_check_start(JavaVM *vm, const char *this_version) {
    (void)vm; (void)this_version;
}

int update_available(char *ver, int max) {
    (void)ver; (void)max;
    return 0;
}

void update_open_page(JavaVM *vm, jobject activity) {
    (void)vm; (void)activity;
}

long update_version_number(const char *s) {
    if (!s) return 0;
    while (*s && (*s < '0' || *s > '9')) s++;
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

void update_test_url(const char *url) { (void)url; }
void update_test_set_latest(const char *v) { (void)v; }
