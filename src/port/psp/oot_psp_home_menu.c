#include "oot_psp_home_menu.h"

#include <pspctrl.h>
#include <pspimpose_driver.h>
#include <pspkernel.h>
#include <stddef.h>
#include <stdio.h>

#include "oot_psp_controls.h"
#include "oot_psp_renderer.h"

#define OOT_PSP_HOME_MENU_ITEM_EXIT_GAME 0
#define OOT_PSP_HOME_MENU_ITEM_CONTROLLER_MAPPING 1
#define OOT_PSP_HOME_MENU_ITEM_COUNT 2

#define OOT_PSP_HOME_MENU_SCREEN_MAIN 0
#define OOT_PSP_HOME_MENU_SCREEN_CONTROLLER_MAPPING 1

#define OOT_PSP_HOME_MENU_HOME_INPUT_DELAY_US 250000

static volatile int sOpenRequested;
static bool sActive;
static bool sHomeCloseArmed;
static bool sOpeningHomePressSeen;
static int sSelectedIndex;
static int sScreen;
static int sControlSelectedIndex;
static int sStatusTimer;
static u32 sLastButtons;
static u32 sLastPolledButtons;
static u32 sHomeInputDelayStartUsec;
static bool sHomeInputDelayActive;
static char sStatusMessage[64];

static u32 OotPspHomeMenu_ReadButtons(void) {
    SceCtrlData pad;

    sceCtrlPeekBufferPositive(&pad, 1);
    return pad.Buttons;
}

static void OotPspHomeMenu_BeginHomeInputDelay(void) {
    sHomeInputDelayStartUsec = sceKernelGetSystemTimeLow();
    sHomeInputDelayActive = true;
}

static bool OotPspHomeMenu_IsHomeInputDelayed(void) {
    if (sHomeInputDelayActive &&
        ((sceKernelGetSystemTimeLow() - sHomeInputDelayStartUsec) >= OOT_PSP_HOME_MENU_HOME_INPUT_DELAY_US)) {
        sHomeInputDelayActive = false;
    }

    return sHomeInputDelayActive;
}

static void OotPspHomeMenu_Open(void) {
    sActive = true;
    sHomeCloseArmed = false;
    OotPspHomeMenu_BeginHomeInputDelay();
    sSelectedIndex = OOT_PSP_HOME_MENU_ITEM_EXIT_GAME;
    sScreen = OOT_PSP_HOME_MENU_SCREEN_MAIN;
    sControlSelectedIndex = 0;
    sStatusTimer = 0;
    sStatusMessage[0] = '\0';
    sLastButtons = OotPspHomeMenu_ReadButtons();
    if ((sLastPolledButtons | sLastButtons) & PSP_CTRL_HOME) {
        sOpeningHomePressSeen = true;
    }

    OotPspRenderer_SetHomeMenuBackgroundActive(true);
    OotPspRenderer_RequestHomeMenuBackground();
}

static void OotPspHomeMenu_Close(void) {
    sActive = false;
    sHomeCloseArmed = false;
    sOpeningHomePressSeen = false;
    sScreen = OOT_PSP_HOME_MENU_SCREEN_MAIN;
    sStatusTimer = 0;
    OotPspRenderer_SetHomeMenuBackgroundActive(false);
}

static int OotPspHomeMenu_ControlDeadzoneRow(void) {
    return OotPspControls_GetBindingCount();
}

static int OotPspHomeMenu_ControlSaveRow(void) {
    return OotPspControls_GetBindingCount() + 1;
}

static int OotPspHomeMenu_ControlResetRow(void) {
    return OotPspControls_GetBindingCount() + 2;
}

static int OotPspHomeMenu_ControlBackRow(void) {
    return OotPspControls_GetBindingCount() + 3;
}

static int OotPspHomeMenu_ControlRowCount(void) {
    return OotPspControls_GetBindingCount() + 4;
}

static void OotPspHomeMenu_SetStatus(const char* message) {
    snprintf(sStatusMessage, sizeof(sStatusMessage), "%s", message);
    sStatusTimer = 120;
}

static void OotPspHomeMenu_MoveControlSelection(int direction) {
    int rowCount = OotPspHomeMenu_ControlRowCount();

    sControlSelectedIndex += direction;
    if (sControlSelectedIndex < 0) {
        sControlSelectedIndex = rowCount - 1;
    } else if (sControlSelectedIndex >= rowCount) {
        sControlSelectedIndex = 0;
    }
}

static void OotPspHomeMenu_AdjustControlRow(int direction) {
    int bindingCount = OotPspControls_GetBindingCount();

    if (sControlSelectedIndex < bindingCount) {
        OotPspControls_CycleBinding(sControlSelectedIndex, direction);
    } else if (sControlSelectedIndex == OotPspHomeMenu_ControlDeadzoneRow()) {
        OotPspControls_AdjustDeadzone(direction * 2);
    }
}

static void OotPspHomeMenu_ActivateControlRow(void) {
    if (sControlSelectedIndex < OotPspControls_GetBindingCount()) {
        OotPspControls_CycleBinding(sControlSelectedIndex, 1);
    } else if (sControlSelectedIndex == OotPspHomeMenu_ControlDeadzoneRow()) {
        OotPspControls_AdjustDeadzone(2);
    } else if (sControlSelectedIndex == OotPspHomeMenu_ControlSaveRow()) {
        if (OotPspControls_Save() == 0) {
            OotPspHomeMenu_SetStatus("Saved controls.ini");
        } else {
            OotPspHomeMenu_SetStatus("Save failed");
        }
    } else if (sControlSelectedIndex == OotPspHomeMenu_ControlResetRow()) {
        OotPspControls_ResetDefaults();
        if (OotPspControls_Save() == 0) {
            OotPspHomeMenu_SetStatus("Defaults saved");
        } else {
            OotPspHomeMenu_SetStatus("Defaults restored");
        }
    } else if (sControlSelectedIndex == OotPspHomeMenu_ControlBackRow()) {
        sScreen = OOT_PSP_HOME_MENU_SCREEN_MAIN;
    }
}

void OotPspHomeMenu_Init(void) {
    sOpenRequested = false;
    sActive = false;
    sHomeCloseArmed = false;
    sOpeningHomePressSeen = false;
    sSelectedIndex = OOT_PSP_HOME_MENU_ITEM_EXIT_GAME;
    sScreen = OOT_PSP_HOME_MENU_SCREEN_MAIN;
    sControlSelectedIndex = 0;
    sStatusTimer = 0;
    sLastButtons = 0;
    sLastPolledButtons = OotPspHomeMenu_ReadButtons();
    sHomeInputDelayStartUsec = 0;
    sHomeInputDelayActive = false;
    sStatusMessage[0] = '\0';

    sceImposeSetHomePopup(0);
}

void OotPspHomeMenu_RequestOpen(void) {
    sOpenRequested = true;
}

void OotPspHomeMenu_PollHomeButton(void) {
    u32 buttons = OotPspHomeMenu_ReadButtons();
    u32 pressed = buttons & ~sLastPolledButtons;

    sLastPolledButtons = buttons;

    if (OotPspHomeMenu_IsHomeInputDelayed()) {
        return;
    }

    if ((buttons & PSP_CTRL_HOME) && !sHomeCloseArmed) {
        sOpeningHomePressSeen = true;
    }

    if ((pressed & PSP_CTRL_HOME) && !sActive) {
        OotPspHomeMenu_RequestOpen();
    }
}

bool OotPspHomeMenu_IsOpen(void) {
    return sActive || sOpenRequested;
}

OotPspHomeMenuResult OotPspHomeMenu_RunFrame(void) {
    u32 buttons;
    u32 pressed;

    if (sOpenRequested) {
        sOpenRequested = false;

        /*
         * The HOME press can be reported both by controller polling and by
         * the PSP exit callback.  Treat requests as idempotent so the second
         * notification cannot immediately close a menu opened by the first.
         */
        if (!sActive && !OotPspHomeMenu_IsHomeInputDelayed()) {
            OotPspHomeMenu_Open();
        }
    }

    if (!sActive) {
        return OOT_PSP_HOME_MENU_RESULT_NONE;
    }

    buttons = OotPspHomeMenu_ReadButtons();
    pressed = buttons & ~sLastButtons;
    sLastButtons = buttons;
    sLastPolledButtons = buttons;

    if (sStatusTimer > 0) {
        sStatusTimer--;
    }

    if (!OotPspHomeMenu_IsHomeInputDelayed()) {
        /* Do not let the HOME press that opened the menu close it as well. */
        if (!sHomeCloseArmed) {
            if (buttons & PSP_CTRL_HOME) {
                sOpeningHomePressSeen = true;
            } else if (sOpeningHomePressSeen) {
                sHomeCloseArmed = true;
            }
        } else if (pressed & PSP_CTRL_HOME) {
            OotPspHomeMenu_BeginHomeInputDelay();
            OotPspHomeMenu_Close();
            return OOT_PSP_HOME_MENU_RESULT_NONE;
        }
    }

    if (sScreen == OOT_PSP_HOME_MENU_SCREEN_CONTROLLER_MAPPING) {
        if (pressed & PSP_CTRL_CIRCLE) {
            sScreen = OOT_PSP_HOME_MENU_SCREEN_MAIN;
        } else if (pressed & PSP_CTRL_UP) {
            OotPspHomeMenu_MoveControlSelection(-1);
        } else if (pressed & PSP_CTRL_DOWN) {
            OotPspHomeMenu_MoveControlSelection(1);
        } else if (pressed & PSP_CTRL_LEFT) {
            OotPspHomeMenu_AdjustControlRow(-1);
        } else if (pressed & PSP_CTRL_RIGHT) {
            OotPspHomeMenu_AdjustControlRow(1);
        } else if (pressed & (PSP_CTRL_CROSS | PSP_CTRL_START)) {
            OotPspHomeMenu_ActivateControlRow();
        }
    } else {
        if (pressed & PSP_CTRL_CIRCLE) {
            OotPspHomeMenu_Close();
            return OOT_PSP_HOME_MENU_RESULT_NONE;
        }

        if (pressed & (PSP_CTRL_UP | PSP_CTRL_DOWN)) {
            sSelectedIndex ^= 1;
        }

        if (pressed & (PSP_CTRL_CROSS | PSP_CTRL_START)) {
            if (sSelectedIndex == OOT_PSP_HOME_MENU_ITEM_EXIT_GAME) {
                OotPspHomeMenu_Close();
                return OOT_PSP_HOME_MENU_RESULT_EXIT_GAME;
            }

            sScreen = OOT_PSP_HOME_MENU_SCREEN_CONTROLLER_MAPPING;
        }
    }

    OotPspRenderer_RenderHomeMenu(sSelectedIndex, sScreen, sControlSelectedIndex,
                                  (sStatusTimer > 0) ? sStatusMessage : NULL);
    return OOT_PSP_HOME_MENU_RESULT_NONE;
}
