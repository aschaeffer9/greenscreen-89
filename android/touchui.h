#ifndef TOUCHUI_H
#define TOUCHUI_H
#include <stdint.h>
#define MACRO_MAX 32
/* a sequence of button masks; each is pressed for one frame then released for one */
typedef struct { uint8_t steps[MACRO_MAX]; int n, pos, phase; } Macro;
void macro_push(Macro *m, uint8_t mask);
int touchui_menu_state(void);
#define TOUCHUI_OPEN_LINK 1
int touchui_tap(Macro *m, int gx, int gy);
void touchui_set_linked(int on);
int touchui_back_is_b(void);
#endif
