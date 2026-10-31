/* Minimal android/native_activity.h for the iOS port. */
#ifndef IOS_ANDROID_NATIVE_ACTIVITY_H
#define IOS_ANDROID_NATIVE_ACTIVITY_H
#include <stdint.h>
#include <stddef.h>
#include "jni.h"
#include "asset_manager.h"
#include "input.h"
#include "native_window.h"
#include "rect.h"

typedef struct ANativeActivity ANativeActivity;

typedef struct ANativeActivityCallbacks {
    void (*onStart)(ANativeActivity *activity);
    void (*onResume)(ANativeActivity *activity);
    void *(*onSaveInstanceState)(ANativeActivity *activity, size_t *outSize);
    void (*onPause)(ANativeActivity *activity);
    void (*onStop)(ANativeActivity *activity);
    void (*onDestroy)(ANativeActivity *activity);
    void (*onWindowFocusChanged)(ANativeActivity *activity, int hasFocus);
    void (*onNativeWindowCreated)(ANativeActivity *activity, ANativeWindow *window);
    void (*onNativeWindowResized)(ANativeActivity *activity, ANativeWindow *window);
    void (*onNativeWindowRedrawNeeded)(ANativeActivity *activity, ANativeWindow *window);
    void (*onNativeWindowDestroyed)(ANativeActivity *activity, ANativeWindow *window);
    void (*onInputQueueCreated)(ANativeActivity *activity, AInputQueue *queue);
    void (*onInputQueueDestroyed)(ANativeActivity *activity, AInputQueue *queue);
    void (*onContentRectChanged)(ANativeActivity *activity, const ARect *rect);
    void (*onConfigurationChanged)(ANativeActivity *activity);
    void (*onLowMemory)(ANativeActivity *activity);
} ANativeActivityCallbacks;

struct ANativeActivity {
    ANativeActivityCallbacks *callbacks;
    JavaVM *vm;
    JNIEnv *env;
    jobject clazz;
    const char *internalDataPath;
    const char *externalDataPath;
    int32_t sdkVersion;
    void *instance;
    AAssetManager *assetManager;
    const char *obbPath;
};

void ANativeActivity_onCreate(ANativeActivity *activity, void *savedState, size_t savedStateSize);
void ANativeActivity_finish(ANativeActivity *activity);
void ANativeActivity_setWindowFlags(ANativeActivity *activity, uint32_t addFlags, uint32_t removeFlags);

#endif
