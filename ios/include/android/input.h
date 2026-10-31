/* Minimal android/input.h for the iOS port: the shim produces these events
 * from UIKit touches and (later) hardware keyboards. */
#ifndef IOS_ANDROID_INPUT_H
#define IOS_ANDROID_INPUT_H
#include <stdint.h>
#include <stddef.h>

typedef struct AInputQueue AInputQueue;
typedef struct AInputEvent AInputEvent;

enum {
    AINPUT_EVENT_TYPE_KEY = 1,
    AINPUT_EVENT_TYPE_MOTION = 2,
};

enum {
    AINPUT_SOURCE_CLASS_POINTER = 0x00000004,
    AINPUT_SOURCE_TOUCHSCREEN = 0x00001002,
    AINPUT_SOURCE_MOUSE = 0x00002002,
    AINPUT_SOURCE_TOUCHPAD = 0x00100008,
    AINPUT_SOURCE_MOUSE_RELATIVE = 0x00020004,
    AINPUT_SOURCE_GAMEPAD = 0x00000401,
    AINPUT_SOURCE_KEYBOARD = 0x00000101,
};

enum {
    AMOTION_EVENT_ACTION_MASK = 0xff,
    AMOTION_EVENT_ACTION_POINTER_INDEX_MASK = 0xff00,
    AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT = 8,
    AMOTION_EVENT_ACTION_DOWN = 0,
    AMOTION_EVENT_ACTION_UP = 1,
    AMOTION_EVENT_ACTION_MOVE = 2,
    AMOTION_EVENT_ACTION_CANCEL = 3,
    AMOTION_EVENT_ACTION_OUTSIDE = 4,
    AMOTION_EVENT_ACTION_POINTER_DOWN = 5,
    AMOTION_EVENT_ACTION_POINTER_UP = 6,
};

enum {
    AKEY_EVENT_ACTION_DOWN = 0,
    AKEY_EVENT_ACTION_UP = 1,
    AKEY_EVENT_ACTION_MULTIPLE = 2,
};

/* key codes (values match AOSP; only the set the shell can show is listed) */
enum {
    AKEYCODE_UNKNOWN = 0,
    AKEYCODE_HOME = 3,
    AKEYCODE_BACK = 4,
    AKEYCODE_0 = 7, AKEYCODE_1 = 8, AKEYCODE_2 = 9, AKEYCODE_3 = 10, AKEYCODE_4 = 11,
    AKEYCODE_5 = 12, AKEYCODE_6 = 13, AKEYCODE_7 = 14, AKEYCODE_8 = 15, AKEYCODE_9 = 16,
    AKEYCODE_DPAD_UP = 19,
    AKEYCODE_DPAD_DOWN = 20,
    AKEYCODE_DPAD_LEFT = 21,
    AKEYCODE_DPAD_RIGHT = 22,
    AKEYCODE_VOLUME_UP = 24,
    AKEYCODE_VOLUME_DOWN = 25,
    AKEYCODE_POWER = 26,
    AKEYCODE_A = 29, AKEYCODE_B = 30, AKEYCODE_C = 31, AKEYCODE_D = 32, AKEYCODE_E = 33,
    AKEYCODE_F = 34, AKEYCODE_G = 35, AKEYCODE_H = 36, AKEYCODE_I = 37, AKEYCODE_J = 38,
    AKEYCODE_K = 39, AKEYCODE_L = 40, AKEYCODE_M = 41, AKEYCODE_N = 42, AKEYCODE_O = 43,
    AKEYCODE_P = 44, AKEYCODE_Q = 45, AKEYCODE_R = 46, AKEYCODE_S = 47, AKEYCODE_T = 48,
    AKEYCODE_U = 49, AKEYCODE_V = 50, AKEYCODE_W = 51, AKEYCODE_X = 52, AKEYCODE_Y = 53,
    AKEYCODE_Z = 54,
    AKEYCODE_COMMA = 55,
    AKEYCODE_PERIOD = 56,
    AKEYCODE_ALT_LEFT = 57,
    AKEYCODE_ALT_RIGHT = 58,
    AKEYCODE_SHIFT_LEFT = 59,
    AKEYCODE_SHIFT_RIGHT = 60,
    AKEYCODE_TAB = 61,
    AKEYCODE_SPACE = 62,
    AKEYCODE_ENTER = 66,
    AKEYCODE_DEL = 67,
    AKEYCODE_MINUS = 69,
    AKEYCODE_EQUALS = 70,
    AKEYCODE_LEFT_BRACKET = 71,
    AKEYCODE_RIGHT_BRACKET = 72,
    AKEYCODE_BACKSLASH = 73,
    AKEYCODE_SEMICOLON = 74,
    AKEYCODE_APOSTROPHE = 75,
    AKEYCODE_SLASH = 76,
    AKEYCODE_GRAVE = 68,
    AKEYCODE_PAGE_UP = 92,
    AKEYCODE_PAGE_DOWN = 93,
    AKEYCODE_BUTTON_A = 96,
    AKEYCODE_BUTTON_B = 97,
    AKEYCODE_BUTTON_C = 98,
    AKEYCODE_BUTTON_X = 99,
    AKEYCODE_BUTTON_Y = 100,
    AKEYCODE_BUTTON_Z = 101,
    AKEYCODE_BUTTON_L1 = 102,
    AKEYCODE_BUTTON_R1 = 103,
    AKEYCODE_BUTTON_L2 = 104,
    AKEYCODE_BUTTON_R2 = 105,
    AKEYCODE_BUTTON_THUMBL = 106,
    AKEYCODE_BUTTON_THUMBR = 107,
    AKEYCODE_BUTTON_START = 108,
    AKEYCODE_BUTTON_SELECT = 109,
    AKEYCODE_BUTTON_MODE = 110,
    AKEYCODE_ESCAPE = 111,
    AKEYCODE_FORWARD_DEL = 112,
    AKEYCODE_CTRL_LEFT = 113,
    AKEYCODE_CTRL_RIGHT = 114,
    AKEYCODE_CAPS_LOCK = 115,
    AKEYCODE_INSERT = 124,
    AKEYCODE_MOVE_HOME = 122,
    AKEYCODE_MOVE_END = 123,
    AKEYCODE_F1 = 131, AKEYCODE_F2 = 132, AKEYCODE_F3 = 133, AKEYCODE_F4 = 134,
    AKEYCODE_F5 = 135, AKEYCODE_F6 = 136, AKEYCODE_F7 = 137, AKEYCODE_F8 = 138,
    AKEYCODE_F9 = 139, AKEYCODE_F10 = 140, AKEYCODE_F11 = 141, AKEYCODE_F12 = 142,
    AKEYCODE_NUMPAD_0 = 144, AKEYCODE_NUMPAD_1 = 145, AKEYCODE_NUMPAD_2 = 146,
    AKEYCODE_NUMPAD_3 = 147, AKEYCODE_NUMPAD_4 = 148, AKEYCODE_NUMPAD_5 = 149,
    AKEYCODE_NUMPAD_6 = 150, AKEYCODE_NUMPAD_7 = 151, AKEYCODE_NUMPAD_8 = 152,
    AKEYCODE_NUMPAD_9 = 153,
    AKEYCODE_NUMPAD_ENTER = 160,
    AKEYCODE_VOLUME_MUTE = 164,
    AKEYCODE_APP_SWITCH = 187,
};

int32_t AInputEvent_getType(const AInputEvent *event);
int32_t AInputEvent_getSource(const AInputEvent *event);
int32_t AInputEvent_getDeviceId(const AInputEvent *event);

int32_t AMotionEvent_getAction(const AInputEvent *motion_event);
int64_t AMotionEvent_getEventTime(const AInputEvent *motion_event);
size_t AMotionEvent_getPointerCount(const AInputEvent *motion_event);
int32_t AMotionEvent_getPointerId(const AInputEvent *motion_event, size_t pointer_index);
float AMotionEvent_getX(const AInputEvent *motion_event, size_t pointer_index);
float AMotionEvent_getY(const AInputEvent *motion_event, size_t pointer_index);

int32_t AKeyEvent_getAction(const AInputEvent *key_event);
int32_t AKeyEvent_getKeyCode(const AInputEvent *key_event);
int32_t AKeyEvent_getRepeatCount(const AInputEvent *key_event);

void AInputQueue_attachLooper(AInputQueue *queue, void *looper, int ident, void *callback, void *data);
void AInputQueue_detachLooper(AInputQueue *queue);
int32_t AInputQueue_getEvent(AInputQueue *queue, AInputEvent **outEvent);
int32_t AInputQueue_preDispatchEvent(AInputQueue *queue, AInputEvent *event);
void AInputQueue_finishEvent(AInputQueue *queue, AInputEvent *event, int handled);

#endif
