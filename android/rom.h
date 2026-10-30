#ifndef ROM_H
#define ROM_H
#include <stdint.h>
#include <stddef.h>
#define ROM_SIZE 32768
#define ROM_MAX_FILE (8 * 1024 * 1024)
enum { ROM_OK = 0, ROM_NOT_FOUND = -1, ROM_WRONG_VERSION = -2, ROM_UNREADABLE = -3 };

void rom_md5(const uint8_t *msg, size_t len, uint8_t out[16]);
int  rom_check(const uint8_t *data, size_t n, uint8_t *out);    /* .gb or .zip bytes -> verified ROM */
int  rom_check_file(const char *path, uint8_t *out);
int  rom_save(const char *path, const uint8_t *rom);
int  rom_scan(const char *const *dirs, int ndirs, uint8_t *out);
#ifdef __ANDROID__
#include <jni.h>
int  rom_intent_has_file(JavaVM *vm, jobject act);
int  rom_from_intent(JavaVM *vm, jobject act, uint8_t *out);
int  rom_have_storage_permission(JavaVM *vm, jobject act);
void rom_request_storage_permission(JavaVM *vm, jobject act);
void rom_open_files_app(JavaVM *vm, jobject act);
void rom_launch_main(JavaVM *vm, jobject act);
#endif
#endif
