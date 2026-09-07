#include "oot_psp_home_menu.h"

#include <pspctrl.h>
#include <pspimpose_driver.h>
#include <pspkernel.h>
#include <psppower.h>
#include <stddef.h>
#include <stdio.h>

#include "color.h"
#include "oot_psp_controls.h"
#include "oot_psp_renderer.h"
#include "oot_psp_video.h"

#define OOT_PSP_HOME_MENU_ITEM_RESUME_GAME 0
#define OOT_PSP_HOME_MENU_ITEM_CONTROLLER_MAPPING 1
#define OOT_PSP_HOME_MENU_ITEM_VIDEO_SETTINGS 2
#define OOT_PSP_HOME_MENU_ITEM_ABOUT 3
#define OOT_PSP_HOME_MENU_ITEM_EXIT_GAME 4
#define OOT_PSP_HOME_MENU_ITEM_COUNT 5

#define OOT_PSP_HOME_MENU_SCREEN_MAIN 0
#define OOT_PSP_HOME_MENU_SCREEN_CONTROLLER_MAPPING 1
#define OOT_PSP_HOME_MENU_SCREEN_VIDEO_SETTINGS 2
#define OOT_PSP_HOME_MENU_SCREEN_ABOUT 3

#define OOT_PSP_HOME_MENU_ABOUT_MAX_SCROLL 16

#define OOT_PSP_VIDEO_MENU_ROW_OUTPUT 0
#define OOT_PSP_VIDEO_MENU_ROW_RESOLUTION 1
#define OOT_PSP_VIDEO_MENU_ROW_ASPECT 2
#define OOT_PSP_VIDEO_MENU_ROW_APPLY 3
#define OOT_PSP_VIDEO_MENU_ROW_SAVE 4
#define OOT_PSP_VIDEO_MENU_ROW_RESET 5
#define OOT_PSP_VIDEO_MENU_ROW_BACK 6
#define OOT_PSP_VIDEO_MENU_ROW_COUNT 7

#define OOT_PSP_HOME_MENU_INPUT_LOCKOUT_US 500000
#define OOT_PSP_HOME_MENU_INPUT_DEBOUNCE_US 150000
#define OOT_PSP_HOME_MENU_FALLBACK_HIGHLIGHT_RED 38
#define OOT_PSP_HOME_MENU_FALLBACK_HIGHLIGHT_GREEN 92
#define OOT_PSP_HOME_MENU_FALLBACK_HIGHLIGHT_BLUE 78
#define OOT_PSP_HOME_MENU_BUTTON_MASK \
    (PSP_CTRL_CIRCLE | PSP_CTRL_CROSS | PSP_CTRL_START | PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT)

static volatile int sOpenRequested;
static bool sActive;
static int sSelectedIndex;
static int sScreen;
static int sControlSelectedIndex;
static int sVideoSelectedIndex;
static int sAboutScrollOffset;
static int sStatusTimer;
static u32 sCurrentButtons;
static u32 sLastButtons;
static u32 sLastPolledButtons;
static u32 sMenuInputLockoutStartUsec;
static u32 sLastMenuInputUsec;
static char sStatusMessage[64];

static u32 OotPspHomeMenu_ReadButtons(void) {
    SceCtrlData pad = { 0 };

    if (sceCtrlPeekBufferPositive(&pad, 1) > 0) {
        sCurrentButtons = pad.Buttons;
    }

    return sCurrentButtons;
}

static void OotPspHomeMenu_Open(void) {
    sActive = true;
    sMenuInputLockoutStartUsec = sceKernelGetSystemTimeLow();
    sSelectedIndex = OOT_PSP_HOME_MENU_ITEM_RESUME_GAME;
    sScreen = OOT_PSP_HOME_MENU_SCREEN_MAIN;
    sControlSelectedIndex = 0;
    sVideoSelectedIndex = 0;
    sAboutScrollOffset = 0;
    sStatusTimer = 0;
    sStatusMessage[0] = '\0';
    sLastButtons = sCurrentButtons;
    sLastMenuInputUsec = sMenuInputLockoutStartUsec;

    OotPspRenderer_SetHomeMenuBackgroundActive(true);
    OotPspRenderer_RequestHomeMenuBackground();
}

static void OotPspHomeMenu_Close(void) {
    sActive = false;
    sScreen = OOT_PSP_HOME_MENU_SCREEN_MAIN;
    sStatusTimer = 0;
    OotPspRenderer_SetHomeMenuBackgroundActive(false);
}

static int OotPspHomeMenu_ControlDeadzoneRow(void) {
    return OotPspControls_GetBindingCount();
}

static int OotPspHomeMenu_ControlN64StickScalingRow(void) {
    return OotPspControls_GetBindingCount() + 1;
}

static int OotPspHomeMenu_ControlSaveRow(void) {
    return OotPspControls_GetBindingCount() + 2;
}

static int OotPspHomeMenu_ControlResetRow(void) {
    return OotPspControls_GetBindingCount() + 3;
}

static int OotPspHomeMenu_ControlBackRow(void) {
    return OotPspControls_GetBindingCount() + 4;
}

static int OotPspHomeMenu_ControlRowCount(void) {
    return OotPspControls_GetBindingCount() + 5;
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
    } else if (sControlSelectedIndex == OotPspHomeMenu_ControlN64StickScalingRow()) {
        OotPspControls_SetN64StickScalingEnabled(!OotPspControls_IsN64StickScalingEnabled());
    }
}

static void OotPspHomeMenu_ActivateControlRow(void) {
    if (sControlSelectedIndex < OotPspControls_GetBindingCount()) {
        OotPspControls_CycleBinding(sControlSelectedIndex, 1);
    } else if (sControlSelectedIndex == OotPspHomeMenu_ControlDeadzoneRow()) {
        OotPspControls_AdjustDeadzone(2);
    } else if (sControlSelectedIndex == OotPspHomeMenu_ControlN64StickScalingRow()) {
        OotPspControls_SetN64StickScalingEnabled(!OotPspControls_IsN64StickScalingEnabled());
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

static void OotPspHomeMenu_MoveVideoSelection(int direction) {
    sVideoSelectedIndex += direction;
    if (sVideoSelectedIndex < 0) {
        sVideoSelectedIndex = OOT_PSP_VIDEO_MENU_ROW_COUNT - 1;
    } else if (sVideoSelectedIndex >= OOT_PSP_VIDEO_MENU_ROW_COUNT) {
        sVideoSelectedIndex = 0;
    }
}

static void OotPspHomeMenu_AdjustVideoRow(int direction) {
    if (sVideoSelectedIndex == OOT_PSP_VIDEO_MENU_ROW_OUTPUT) {
        OotPspVideo_CycleOutput(direction);
    } else if (sVideoSelectedIndex == OOT_PSP_VIDEO_MENU_ROW_RESOLUTION) {
        OotPspVideo_CycleResolution(direction);
    } else if (sVideoSelectedIndex == OOT_PSP_VIDEO_MENU_ROW_ASPECT) {
        OotPspVideo_CycleAspect(direction);
    }
}

static void OotPspHomeMenu_ActivateVideoRow(void) {
    if (sVideoSelectedIndex <= OOT_PSP_VIDEO_MENU_ROW_ASPECT) {
        OotPspHomeMenu_AdjustVideoRow(1);
    } else if (sVideoSelectedIndex == OOT_PSP_VIDEO_MENU_ROW_APPLY) {
        int result;

        if (OotPspVideo_IsTvSelected() && !OotPspVideo_RefreshTvAvailability()) {
            OotPspHomeMenu_SetStatus("No video cable detected");
            return;
        }
        result = OotPspRenderer_ApplyVideoSettings();
        if (result == 0) {
            OotPspHomeMenu_SetStatus("Video mode applied");
        } else {
            char message[64];

            snprintf(message, sizeof(message), "Video mode failed: %d", result);
            OotPspHomeMenu_SetStatus(message);
        }
    } else if (sVideoSelectedIndex == OOT_PSP_VIDEO_MENU_ROW_SAVE) {
        if (OotPspVideo_Save() == 0) {
            OotPspHomeMenu_SetStatus("Saved video.ini");
        } else {
            OotPspHomeMenu_SetStatus("Save failed");
        }
    } else if (sVideoSelectedIndex == OOT_PSP_VIDEO_MENU_ROW_RESET) {
        OotPspVideo_ResetDefaults();
        if (OotPspRenderer_ApplyVideoSettings() == 0) {
            OotPspHomeMenu_SetStatus("Video defaults restored");
        } else {
            OotPspHomeMenu_SetStatus("Defaults unavailable");
        }
    } else if (sVideoSelectedIndex == OOT_PSP_VIDEO_MENU_ROW_BACK) {
        sScreen = OOT_PSP_HOME_MENU_SCREEN_MAIN;
    }
}

void OotPspHomeMenu_Init(void) {
    sOpenRequested = false;
    sActive = false;
    sSelectedIndex = OOT_PSP_HOME_MENU_ITEM_RESUME_GAME;
    sScreen = OOT_PSP_HOME_MENU_SCREEN_MAIN;
    sControlSelectedIndex = 0;
    sVideoSelectedIndex = 0;
    sAboutScrollOffset = 0;
    sStatusTimer = 0;
    sCurrentButtons = 0;
    sLastButtons = 0;
    sLastPolledButtons = OotPspHomeMenu_ReadButtons();
    sMenuInputLockoutStartUsec = 0;
    sLastMenuInputUsec = 0;
    sStatusMessage[0] = '\0';

    sceImposeSetHomePopup(0);
}

static void OotPspHomeMenu_RequestOpen(void) {
    sOpenRequested = true;
}

static void OotPspHomeMenu_MoveAboutScroll(int direction) {
    sAboutScrollOffset += direction;
    if (sAboutScrollOffset < 0) {
        sAboutScrollOffset = 0;
    } else if (sAboutScrollOffset > OOT_PSP_HOME_MENU_ABOUT_MAX_SCROLL) {
        sAboutScrollOffset = OOT_PSP_HOME_MENU_ABOUT_MAX_SCROLL;
    }
}

static void OotPspHomeMenu_Render(const char* statusMessage, const Color_RGB8* tunicColor) {
    u8 highlightRed = OOT_PSP_HOME_MENU_FALLBACK_HIGHLIGHT_RED;
    u8 highlightGreen = OOT_PSP_HOME_MENU_FALLBACK_HIGHLIGHT_GREEN;
    u8 highlightBlue = OOT_PSP_HOME_MENU_FALLBACK_HIGHLIGHT_BLUE;
    int submenuSelectedIndex = sControlSelectedIndex;

    if (tunicColor != NULL) {
        highlightRed = tunicColor->r;
        highlightGreen = tunicColor->g;
        highlightBlue = tunicColor->b;
    }

    if (sScreen == OOT_PSP_HOME_MENU_SCREEN_VIDEO_SETTINGS) {
        submenuSelectedIndex = sVideoSelectedIndex;
    } else if (sScreen == OOT_PSP_HOME_MENU_SCREEN_ABOUT) {
        submenuSelectedIndex = sAboutScrollOffset;
    }

    OotPspRenderer_RenderHomeMenu(sSelectedIndex, sScreen, submenuSelectedIndex,
                                  statusMessage, highlightRed, highlightGreen, highlightBlue,
                                  scePowerGetBatteryLifePercent());
}

void OotPspHomeMenu_PollHomeButton(void) {
    u32 buttons = OotPspHomeMenu_ReadButtons();
    u32 pressed = buttons & ~sLastPolledButtons;

    sLastPolledButtons = buttons;

    if ((pressed & PSP_CTRL_HOME) && !sActive) {
        OotPspHomeMenu_RequestOpen();
    }
}

bool OotPspHomeMenu_IsOpen(void) {
    return sActive || sOpenRequested;
}

OotPspHomeMenuResult OotPspHomeMenu_RunFrame(const Color_RGB8* tunicColor) {
    u32 buttons;
    u32 pressed;
    u32 now;

    if (sOpenRequested) {
        sOpenRequested = false;

        if (!sActive) {
            OotPspHomeMenu_Open();
        }
    }

    if (!sActive) {
        return OOT_PSP_HOME_MENU_RESULT_NONE;
    }

    buttons = sCurrentButtons;
    pressed = buttons & ~sLastButtons;
    sLastButtons = buttons;
    sLastPolledButtons = buttons;

    if (sStatusTimer > 0) {
        sStatusTimer--;
    }

    now = sceKernelGetSystemTimeLow();
    if ((now - sMenuInputLockoutStartUsec) < OOT_PSP_HOME_MENU_INPUT_LOCKOUT_US) {
        OotPspHomeMenu_Render(NULL, tunicColor);
        return OOT_PSP_HOME_MENU_RESULT_NONE;
    }

    pressed &= OOT_PSP_HOME_MENU_BUTTON_MASK;
    if (pressed != 0) {
        if ((now - sLastMenuInputUsec) < OOT_PSP_HOME_MENU_INPUT_DEBOUNCE_US) {
            pressed = 0;
        } else {
            sLastMenuInputUsec = now;
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
    } else if (sScreen == OOT_PSP_HOME_MENU_SCREEN_VIDEO_SETTINGS) {
        if (pressed & PSP_CTRL_CIRCLE) {
            sScreen = OOT_PSP_HOME_MENU_SCREEN_MAIN;
        } else if (pressed & PSP_CTRL_UP) {
            OotPspHomeMenu_MoveVideoSelection(-1);
        } else if (pressed & PSP_CTRL_DOWN) {
            OotPspHomeMenu_MoveVideoSelection(1);
        } else if (pressed & PSP_CTRL_LEFT) {
            OotPspHomeMenu_AdjustVideoRow(-1);
        } else if (pressed & PSP_CTRL_RIGHT) {
            OotPspHomeMenu_AdjustVideoRow(1);
        } else if (pressed & (PSP_CTRL_CROSS | PSP_CTRL_START)) {
            OotPspHomeMenu_ActivateVideoRow();
        }
    } else if (sScreen == OOT_PSP_HOME_MENU_SCREEN_ABOUT) {
        if (pressed & (PSP_CTRL_CIRCLE | PSP_CTRL_CROSS | PSP_CTRL_START)) {
            sScreen = OOT_PSP_HOME_MENU_SCREEN_MAIN;
        } else if (pressed & PSP_CTRL_UP) {
            OotPspHomeMenu_MoveAboutScroll(-1);
        } else if (pressed & PSP_CTRL_DOWN) {
            OotPspHomeMenu_MoveAboutScroll(1);
        }
    } else {
        if (pressed & PSP_CTRL_UP) {
            sSelectedIndex = (sSelectedIndex + OOT_PSP_HOME_MENU_ITEM_COUNT - 1) % OOT_PSP_HOME_MENU_ITEM_COUNT;
        } else if (pressed & PSP_CTRL_DOWN) {
            sSelectedIndex = (sSelectedIndex + 1) % OOT_PSP_HOME_MENU_ITEM_COUNT;
        }

        if (pressed & (PSP_CTRL_CROSS | PSP_CTRL_START)) {
            if (sSelectedIndex == OOT_PSP_HOME_MENU_ITEM_RESUME_GAME) {
                OotPspHomeMenu_Close();
                return OOT_PSP_HOME_MENU_RESULT_NONE;
            } else if (sSelectedIndex == OOT_PSP_HOME_MENU_ITEM_EXIT_GAME) {
                OotPspHomeMenu_Close();
                return OOT_PSP_HOME_MENU_RESULT_EXIT_GAME;
            } else if (sSelectedIndex == OOT_PSP_HOME_MENU_ITEM_VIDEO_SETTINGS) {
                OotPspVideo_RefreshTvAvailability();
                sScreen = OOT_PSP_HOME_MENU_SCREEN_VIDEO_SETTINGS;
                sVideoSelectedIndex = 0;
            } else if (sSelectedIndex == OOT_PSP_HOME_MENU_ITEM_ABOUT) {
                sScreen = OOT_PSP_HOME_MENU_SCREEN_ABOUT;
                sAboutScrollOffset = 0;
            } else {
                sScreen = OOT_PSP_HOME_MENU_SCREEN_CONTROLLER_MAPPING;
            }
        }
    }

    OotPspHomeMenu_Render((sStatusTimer > 0) ? sStatusMessage : NULL, tunicColor);
    return OOT_PSP_HOME_MENU_RESULT_NONE;
}
