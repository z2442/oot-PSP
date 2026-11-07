#ifndef OOT_PSP_SUSPEND_H
#define OOT_PSP_SUSPEND_H

/* Acquire the process power lock before starting any workers or caches. */
int OotPspSuspend_Init(void);
/* Callback-safe notification; all blocking work runs at a main-loop boundary. */
void OotPspSuspend_NotifyPowerEvent(int flags);
void OotPspSuspend_Poll(void);

#endif
