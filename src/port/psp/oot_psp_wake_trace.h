#ifndef OOT_PSP_WAKE_TRACE_H
#define OOT_PSP_WAKE_TRACE_H

#ifndef OOT_PSP_WAKE_DIAGNOSTICS
#define OOT_PSP_WAKE_DIAGNOSTICS 0
#endif

#if OOT_PSP_WAKE_DIAGNOSTICS
/* Main-loop boundary only. No extra worker or continuous file logging. */
void OotPspWakeTrace_Poll(void);
void OotPspWakeTrace_Resume(void);
/* Main thread only: close each checkpoint before sleep or the next stage. */
void OotPspWakeTrace_Checkpoint(const char* stage);
/* Callback-safe: retain power-event timestamps in RAM. */
void OotPspWakeTrace_NotifyPowerEvent(int flags);
#endif

#endif
