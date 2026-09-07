/* Host check: cc -Wall -Wextra -Werror tools/tests/test_psp_launcher_path.c
 * -o /tmp/test_psp_launcher_path && /tmp/test_psp_launcher_path */
#include <assert.h>
#include "../../src/port/psp/oot_psp_launcher_path.h"

static void check(const char* argument, const char* cwd, const char* expected) {
    char output[512];
    assert(OotPspLauncher_ResolveExecutable(argument, cwd, output, sizeof(output)));
    assert(strcmp(output, expected) == 0);
}

int main(void) {
    char output[512];
    check("ms0:/PSP/GAME/OOT/EBOOT.PBP", "ef0:/elsewhere", "ms0:/PSP/GAME/OOT/EBOOT.PBP");
    check("ef0:\\PSP\\GAME\\OOT\\EBOOT.PBP", NULL, "ef0:/PSP/GAME/OOT/EBOOT.PBP");
    check("EBOOT.PBP", "ms0:/PSP/GAME/OOT", "ms0:/PSP/GAME/OOT/EBOOT.PBP");
    check(NULL, "ef0:/PSP/GAME/OOT/", "ef0:/PSP/GAME/OOT/EBOOT.PBP");
    check("/PSP/GAME/OOT/EBOOT.PBP", "ef0:/other", "ef0:/PSP/GAME/OOT/EBOOT.PBP");
    check("./OOT//EBOOT.PBP", "ms0:/PSP/GAME/", "ms0:/PSP/GAME/OOT/EBOOT.PBP");
    check("../OOT/EBOOT.PBP", "ms0:/PSP/GAME/other", "ms0:/PSP/GAME/OOT/EBOOT.PBP");
    check("ms0:/one/../EBOOT.PBP", NULL, "ms0:/EBOOT.PBP");
    check("EBOOT.PBP", "ms0:/PSP/GAME/OOT Port", "ms0:/PSP/GAME/OOT Port/EBOOT.PBP");
    assert(!OotPspLauncher_ResolveExecutable("EBOOT.PBP", NULL, output, sizeof(output)));
    assert(!OotPspLauncher_ResolveExecutable("ms0:/../EBOOT.PBP", NULL, output, sizeof(output)));
    assert(!OotPspLauncher_ResolveExecutable("ms0:EBOOT.PBP", NULL, output, sizeof(output)));
    assert(!OotPspLauncher_ResolveExecutable("ms0:/EBOOT.PBP", NULL, output, 8));
    assert(!OotPspLauncher_ResolveExecutable("ms0:/EBOOT.PBP", NULL, output, 0));
    puts("PSP launcher path checks passed");
    return 0;
}
