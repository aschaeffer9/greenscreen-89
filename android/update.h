#ifndef UPDATE_H
#define UPDATE_H
#include <jni.h>
void update_check_start(JavaVM *vm, const char *this_version);   /* background, once */
int  update_available(char *ver, int max);                       /* newer release known */
void update_open_page(JavaVM *vm, jobject activity);              /* the releases page in the browser */
long update_version_number(const char *s);
void update_test_url(const char *url);
void update_test_set_latest(const char *v);
#endif
