#ifndef OOT_PSP_HOME_MENU_H
#define OOT_PSP_HOME_MENU_H

#include <stdbool.h>

struct Color_RGB8;

typedef enum OotPspHomeMenuResult {
    OOT_PSP_HOME_MENU_RESULT_NONE,
    OOT_PSP_HOME_MENU_RESULT_EXIT_GAME,
} OotPspHomeMenuResult;

void OotPspHomeMenu_Init(void);
void OotPspHomeMenu_PollHomeButton(void);
bool OotPspHomeMenu_IsOpen(void);
OotPspHomeMenuResult OotPspHomeMenu_RunFrame(const struct Color_RGB8* tunicColor);

#endif
