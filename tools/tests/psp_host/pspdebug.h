#include <stdio.h>
#define pspDebugScreenPrintf printf
static inline void pspDebugScreenSetXY(int x, int y) { (void)x; (void)y; }
