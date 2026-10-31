/* Minimal jni.h for the iOS port.
 *
 * There is no JVM: the Android-only code (ROM intents, MQTT-over-SSLSocket,
 * the update checker) is compiled out with #ifdef __ANDROID__, and the one
 * place the shell still touches JNI outside those guards (pointer capture for
 * trackpad mode) gets a table of no-ops. */
#ifndef IOS_JNI_STUB_H
#define IOS_JNI_STUB_H
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

typedef int32_t jint;
typedef uint8_t jboolean;
typedef int8_t jbyte;
typedef uint16_t jchar;
typedef int16_t jshort;
typedef int64_t jlong;
typedef float jfloat;
typedef double jdouble;
typedef jint jsize;

typedef void *jobject;
typedef jobject jclass;
typedef jobject jstring;
typedef jobject jthrowable;
typedef jobject jarray;
typedef jobject jweak;
typedef struct _jmethodID *jmethodID;
typedef struct _jfieldID *jfieldID;

typedef union jvalue {
    jboolean z;
    jbyte b;
    jchar c;
    jshort s;
    jint i;
    jlong j;
    jfloat f;
    jdouble d;
    jobject l;
} jvalue;

#define JNI_FALSE 0
#define JNI_TRUE 1
#define JNI_OK 0
#define JNI_ERR (-1)
#define JNI_VERSION_1_6 0x00010006

struct JNINativeInterface;
typedef const struct JNINativeInterface *JNIEnv;

struct JNIInvokeInterface;
typedef const struct JNIInvokeInterface *JavaVM;

struct JNINativeInterface {
    jint (*PushLocalFrame)(JNIEnv *env, jint capacity);
    jobject (*PopLocalFrame)(JNIEnv *env, jobject result);
    jclass (*FindClass)(JNIEnv *env, const char *name);
    jmethodID (*GetMethodID)(JNIEnv *env, jclass clazz, const char *name, const char *sig);
    jmethodID (*GetStaticMethodID)(JNIEnv *env, jclass clazz, const char *name, const char *sig);
    jobject (*CallObjectMethod)(JNIEnv *env, jobject obj, jmethodID methodID, ...);
    jobject (*CallStaticObjectMethod)(JNIEnv *env, jclass clazz, jmethodID methodID, ...);
    void (*CallVoidMethod)(JNIEnv *env, jobject obj, jmethodID methodID, ...);
    jboolean (*CallBooleanMethod)(JNIEnv *env, jobject obj, jmethodID methodID, ...);
    jfloat (*CallFloatMethod)(JNIEnv *env, jobject obj, jmethodID methodID, ...);
    jboolean (*ExceptionCheck)(JNIEnv *env);
    void (*ExceptionClear)(JNIEnv *env);
    void (*DeleteLocalRef)(JNIEnv *env, jobject ref);
    jstring (*NewStringUTF)(JNIEnv *env, const char *utf);
    const char *(*GetStringUTFChars)(JNIEnv *env, jstring str, jboolean *isCopy);
    void (*ReleaseStringUTFChars)(JNIEnv *env, jstring str, const char *chars);
};

struct JNIInvokeInterface {
    jint (*DestroyJavaVM)(JavaVM *vm);
    jint (*AttachCurrentThread)(JavaVM *vm, void **penv, void *args);
    jint (*DetachCurrentThread)(JavaVM *vm);
    jint (*GetEnv)(JavaVM *vm, void **penv, jint version);
};

#endif
