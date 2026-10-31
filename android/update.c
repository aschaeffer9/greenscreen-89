/* Update check: once per launch, ask GitHub for the latest release of the
 * public app and compare it with this build's version. Nothing is sent but the
 * request itself; it can be compiled out with -DT89_NO_UPDATE_CHECK. */
#include <jni.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "update.h"

#ifndef RELEASES_API
#define RELEASES_API "https://api.github.com/repos/aschaeffer9/greenscreen-89/releases/latest"
#endif
#ifndef RELEASES_PAGE
#define RELEASES_PAGE "https://github.com/aschaeffer9/greenscreen-89/releases/latest"
#endif

static JavaVM *uvm;
static char current[16], latest[16];
static volatile int state;          /* 0 idle, 1 checking, 2 done */
static const char *api_url = RELEASES_API;
void update_test_url(const char *url) { api_url = url; }

/* "3.10.1" -> comparable number; any leading 'v' or text is skipped */
long update_version_number(const char *s) {
    long parts[3] = { 0, 0, 0 }; int k = 0;
    while (*s && (*s < '0' || *s > '9')) s++;
    while (*s && k < 3) {
        if (*s >= '0' && *s <= '9') { parts[k] = parts[k] * 10 + (*s - '0'); if (parts[k] > 9999) parts[k] = 9999; }
        else if (*s == '.') k++;
        else break;
        s++;
    }
    return parts[0] * 100000000L + parts[1] * 10000L + parts[2];
}

static int jx(JNIEnv *e) { if ((*e)->ExceptionCheck(e)) { (*e)->ExceptionClear(e); return 1; } return 0; }

static void *check_thread(void *arg) {
    (void)arg;
    JNIEnv *e = NULL;
    if ((*uvm)->AttachCurrentThread(uvm, &e, NULL) != JNI_OK || !e) { state = 2; return NULL; }
    if ((*e)->PushLocalFrame(e, 64) == 0) {
        jclass cU = (*e)->FindClass(e, "java/net/URL");
        jobject url = cU ? (*e)->NewObject(e, cU, (*e)->GetMethodID(e, cU, "<init>", "(Ljava/lang/String;)V"), (*e)->NewStringUTF(e, api_url)) : NULL;
        if (jx(e)) url = NULL;
        jobject con = url ? (*e)->CallObjectMethod(e, url, (*e)->GetMethodID(e, cU, "openConnection", "()Ljava/net/URLConnection;")) : NULL;
        if (jx(e)) con = NULL;
        if (con) {
            jclass cC = (*e)->FindClass(e, "java/net/URLConnection");
            (*e)->CallVoidMethod(e, con, (*e)->GetMethodID(e, cC, "setConnectTimeout", "(I)V"), 8000); jx(e);
            (*e)->CallVoidMethod(e, con, (*e)->GetMethodID(e, cC, "setReadTimeout", "(I)V"), 8000); jx(e);
            jmethodID sp = (*e)->GetMethodID(e, cC, "setRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V");
            (*e)->CallVoidMethod(e, con, sp, (*e)->NewStringUTF(e, "Accept"), (*e)->NewStringUTF(e, "application/vnd.github+json")); jx(e);
            (*e)->CallVoidMethod(e, con, sp, (*e)->NewStringUTF(e, "User-Agent"), (*e)->NewStringUTF(e, "Greenscreen89-update-check")); jx(e);
            jobject in = (*e)->CallObjectMethod(e, con, (*e)->GetMethodID(e, cC, "getInputStream", "()Ljava/io/InputStream;"));
            if (jx(e)) in = NULL;
            if (in) {
                jclass cI = (*e)->FindClass(e, "java/io/InputStream");
                jmethodID rd = (*e)->GetMethodID(e, cI, "read", "([BII)I");
                jbyteArray arr = (*e)->NewByteArray(e, 4096);
                static char body[65536]; int n = 0;
                while (arr && n < (int)sizeof body - 1) {
                    jint g = (*e)->CallIntMethod(e, in, rd, arr, 0, 4096);
                    if (jx(e) || g < 0) break;
                    if (n + g > (int)sizeof body - 1) g = (int)sizeof body - 1 - n;
                    (*e)->GetByteArrayRegion(e, arr, 0, g, (jbyte *)body + n);
                    n += g;
                }
                body[n] = 0;
                (*e)->CallVoidMethod(e, in, (*e)->GetMethodID(e, cI, "close", "()V")); jx(e);
                char *t = strstr(body, "\"tag_name\"");
                if (t && (t = strchr(t + 10, '"'))) {
                    t++;
                    int k = 0;
                    while (t[k] && t[k] != '"' && k < (int)sizeof latest - 1) { latest[k] = t[k]; k++; }
                    latest[k] = 0;
                }
            }
        }
        (*e)->PopLocalFrame(e, NULL);
    }
    (*uvm)->DetachCurrentThread(uvm);
    state = 2;
    return NULL;
}

void update_check_start(JavaVM *vm, const char *this_version) {
#ifdef T89_NO_UPDATE_CHECK
    (void)vm; (void)this_version; return;
#else
    if (state || !vm) return;
    uvm = vm; state = 1;
    snprintf(current, sizeof current, "%s", this_version);
    pthread_t th;
    if (pthread_create(&th, NULL, check_thread, NULL) != 0) { state = 2; return; }
    pthread_detach(th);
#endif
}

/* 1 (and the version) once a newer release than this build is known */
int update_available(char *ver, int max) {
    if (state != 2 || !latest[0]) return 0;
    if (update_version_number(latest) <= update_version_number(current)) return 0;
    if (ver) snprintf(ver, (size_t)max, "%s", latest);
    return 1;
}

void update_open_page(JavaVM *vm, jobject act) {
    JNIEnv *e = NULL;
    if (!vm || (*vm)->GetEnv(vm, (void **)&e, JNI_VERSION_1_6) != JNI_OK) {
        if (!vm || (*vm)->AttachCurrentThread(vm, &e, NULL) != JNI_OK) return;
    }
    if ((*e)->PushLocalFrame(e, 16) != 0) return;
    jclass cUri = (*e)->FindClass(e, "android/net/Uri");
    jobject uri = cUri ? (*e)->CallStaticObjectMethod(e, cUri, (*e)->GetStaticMethodID(e, cUri, "parse", "(Ljava/lang/String;)Landroid/net/Uri;"),
                                                       (*e)->NewStringUTF(e, RELEASES_PAGE)) : NULL;
    if (jx(e)) uri = NULL;
    jclass cI = (*e)->FindClass(e, "android/content/Intent");
    jobject it = (uri && cI) ? (*e)->NewObject(e, cI, (*e)->GetMethodID(e, cI, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V"),
                                               (*e)->NewStringUTF(e, "android.intent.action.VIEW"), uri) : NULL;
    if (jx(e)) it = NULL;
    if (it) {
        (*e)->CallObjectMethod(e, it, (*e)->GetMethodID(e, cI, "addFlags", "(I)Landroid/content/Intent;"), 0x10000000); jx(e);
        jclass cA = (*e)->FindClass(e, "android/app/Activity");
        (*e)->CallVoidMethod(e, act, (*e)->GetMethodID(e, cA, "startActivity", "(Landroid/content/Intent;)V"), it); jx(e);
    }
    (*e)->PopLocalFrame(e, NULL);
}
/* tests: pretend the check finished with this latest release */
void update_test_set_latest(const char *v) { snprintf(latest, sizeof latest, "%s", v); state = 2; }
