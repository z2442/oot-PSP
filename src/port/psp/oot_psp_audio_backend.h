#ifndef OOT_PSP_AUDIO_BACKEND_H
#define OOT_PSP_AUDIO_BACKEND_H

#include "ultra64.h"

#ifndef OOT_PSP_AUDIO_DIAGNOSTICS
#define OOT_PSP_AUDIO_DIAGNOSTICS 0
#endif

typedef enum OotPspAudioProducerState {
    OOT_PSP_AUDIO_PRODUCER_STATE_STOPPED,
    OOT_PSP_AUDIO_PRODUCER_STATE_STARTING,
    OOT_PSP_AUDIO_PRODUCER_STATE_PRIMING,
    OOT_PSP_AUDIO_PRODUCER_STATE_TIMER_WAIT,
    OOT_PSP_AUDIO_PRODUCER_STATE_UPDATE,
    OOT_PSP_AUDIO_PRODUCER_STATE_CATCHUP,
    OOT_PSP_AUDIO_PRODUCER_STATE_IO_BACKOFF,
    OOT_PSP_AUDIO_PRODUCER_STATE_RING_FULL,
    OOT_PSP_AUDIO_PRODUCER_STATE_WAIT_ME,
    OOT_PSP_AUDIO_PRODUCER_STATE_PREPARE,
    OOT_PSP_AUDIO_PRODUCER_STATE_SEQUENCE,
    OOT_PSP_AUDIO_PRODUCER_STATE_SUBMIT,
} OotPspAudioProducerState;

#if OOT_PSP_WAKE_DIAGNOSTICS
typedef struct OotPspAudioWakeStatus {
    u32 meActive;
    u32 meState;
    u32 meProgress;
    u32 interruptReady;
    u32 interruptCount;
    u32 fallbackCount;
    u32 fallbackState;
    u32 fallbackProgress;
    u32 fallbackElapsedUsec;
    s32 fallbackCmdCount;
    u32 hardwareSrc;
    u32 bootCount;
    s32 hardwareResult;
    u32 hardware[6];
    s32 fallbackHardwareResult;
    u32 fallbackHardware[6];
    u32 meConfig;
    u32 meStatus;
    u32 meStackPointer;
    s32 cpuSynthInputs[5], meSynthInputs[5];
    u32 fallbackWord0, fallbackWord1, fallbackWordsValid;
} OotPspAudioWakeStatus;
void OotPspAudioBackend_GetWakeStatus(OotPspAudioWakeStatus* status);
/* ME-only diagnostic checkpoints; do not change the scheduling policy. */
void OotPspAudioBackend_RecordMeProgress(u32 progress);
void OotPspAudioBackend_RecordMeCommand(u32 index, u32 word0, u32 word1);
void OotPspAudioBackend_RecordMeSynthInputs(s32 ticks, s32 notes, s32 reverbs, s32 frames, s32 downsampleRate);
#endif

#if defined(OOTDEBUG)
typedef struct OotPspAudioProfileCounters {
    u32 updates;
    u32 waitUsec;
    u32 prepareUsec;
    u32 synthUsec;
    u32 submitUsec;
    u32 sequenceUsec;
    u32 commandBuildUsec;
    u32 abiCommands;
    u32 sampleDmas;
    u32 meSubmits;
    u32 cpuMixes;
    u32 meFailures;
    u32 meActive;
    u32 meState;
    u32 meProgress;
} OotPspAudioProfileCounters;
#endif

s32 OotPspAudioBackend_Init(void);
s32 OotPspAudioBackend_BootMe(void);
/* ME entry point only: recreate private command-building state after reset. */
void OotPspAudioSynth_InitMe(void);
/* Main thread only. Resume must also follow a failed prepare. */
s32 OotPspAudioBackend_PrepareSuspend(void);
const char* OotPspAudioBackend_GetSuspendError(void);
s32 OotPspAudioBackend_Resume(s32 didSleep);
s32 OotPspAudioBackend_Queue(const void* buf, u32 size);
s32 OotPspAudioBackend_SetFrequency(u32 frequency);
u32 OotPspAudioBackend_GetLength(void);
s32 OotPspAudioBackend_NeedsRefillUrgently(void);
s32 OotPspAudioBackend_NeedsRefillDuringIo(void);
#if defined(OOTDEBUG)
void OotPspAudioBackend_GetThreadRunClocks(u64* producerClocks, u64* outputClocks);
void OotPspAudioBackend_GetProfileCounters(OotPspAudioProfileCounters* counters);
#endif
#if defined(OOTDEBUG) || OOT_PSP_AUDIO_DIAGNOSTICS
void OotPspAudioBackend_RecordUpdateProfile(u32 waitUsec, u32 prepareUsec, u32 synthUsec, u32 submitUsec,
                                            u32 abiCommands, u32 sampleDmas);
void OotPspAudioBackend_RecordSynthesisProfile(u32 sequenceUsec, u32 commandBuildUsec);
#endif
#if OOT_PSP_AUDIO_DIAGNOSTICS
void OotPspAudioBackend_SetDiagnosticProducerState(OotPspAudioProducerState state);
#else
#define OotPspAudioBackend_SetDiagnosticProducerState(state) ((void)0)
#endif
void OotPspAudioBackend_SubmitCommands(const Acmd* cmdList, s32 cmdCount);
void OotPspAudioBackend_SubmitCommandsAndQueue(const Acmd* cmdList, s32 cmdCount, const void* buf, u32 size);
void OotPspAudioBackend_SubmitSynthesis(Acmd* cmdList, s16* aiBuffer, s32 aiFrames);
void OotPspAudioBackend_WaitForCommands(void);
void OotPspAudioBackend_ExecuteCommands(const Acmd* cmdList, s32 cmdCount);

void OotPspAudio_Init(void);
void OotPspAudio_Update(void);

#endif
