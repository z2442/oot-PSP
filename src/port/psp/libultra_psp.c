#include "ultra64.h"

#include "attributes.h"

#include <pspctrl.h>
#include <pspkernel.h>
#include <pspthreadman.h>
#include <psprtc.h>
#include <stdarg.h>
#include <stdio.h>

#include "controller.h"
#include "oot_psp_audio_backend.h"

#define OOT_PSP_MAX_THREADS 16
#define OOT_PSP_MAX_TIMERS  16

typedef struct {
    OSThread* thread;
    SceUID sceThreadId;
    void (*entry)(void*);
    void* arg;
} OotPspThreadSlot;

typedef struct {
    OSTimer* timer;
    SceUID sceThreadId;
    volatile s32 active;
} OotPspTimerSlot;

static OotPspThreadSlot sThreadSlots[OOT_PSP_MAX_THREADS];
static OotPspTimerSlot sTimerSlots[OOT_PSP_MAX_TIMERS];
static OSContPad sControllerPads[MAXCONTROLLERS];
static OSContStatus sControllerStatus[MAXCONTROLLERS];
static OSMesgQueue* sPiCmdQueue;
static OSMesgQueue* sViEventQueue;
static OSMesg sViEventMsg;
static u32 sIntMask = OS_IM_ALL;
static void* sCurrentFramebuffer;
static void* sNextFramebuffer;
static OSViMode* sCurrentViMode;
static OSViContext sCurrentViContext;
static u32 sMesgQueueSemaIndex;

s32 osRomType = 0;
void* osRomBase = NULL;
s32 osTvType = OS_TV_NTSC;
s32 osResetType = 0;
s32 osCicId = 0;
s32 osVersion = 0;
u32 osMemSize = 0x02000000;
s32 osAppNMIBuffer[0x10];
u64 osClockRate = OS_CLOCK_RATE;
__osHwInt __osHwIntTable[1];
OSIntMask __OSGlobalIntMask = OS_IM_ALL;
OSPiHandle* __osPiTable = NULL;
u8 __osContLastCmd = CONT_CMD_READ_BUTTON;
OSPifRam __osContPifRam;
OSPifRam __osPfsPifRam;
u8 __osMaxControllers = 1;

static OotPspThreadSlot* OotPsp_FindThreadSlot(OSThread* thread) {
    s32 i;

    for (i = 0; i < OOT_PSP_MAX_THREADS; i++) {
        if (sThreadSlots[i].thread == thread) {
            return &sThreadSlots[i];
        }
    }

    return NULL;
}

static OotPspThreadSlot* OotPsp_AllocThreadSlot(OSThread* thread) {
    s32 i;

    for (i = 0; i < OOT_PSP_MAX_THREADS; i++) {
        if (sThreadSlots[i].thread == NULL) {
            sThreadSlots[i].thread = thread;
            return &sThreadSlots[i];
        }
    }

    return NULL;
}

static int OotPsp_ThreadTrampoline(SceSize args, void* argp) {
    OSThread* thread = (OSThread*)argp;
    OotPspThreadSlot* slot = OotPsp_FindThreadSlot(thread);

    (void)args;

    if (slot != NULL && slot->entry != NULL) {
        thread->state = OS_STATE_RUNNING;
        slot->entry(slot->arg);
        thread->state = OS_STATE_STOPPED;
    }

    return 0;
}

static OotPspTimerSlot* OotPsp_FindTimerSlot(OSTimer* timer) {
    s32 i;

    for (i = 0; i < OOT_PSP_MAX_TIMERS; i++) {
        if (sTimerSlots[i].timer == timer) {
            return &sTimerSlots[i];
        }
    }

    return NULL;
}

static OotPspTimerSlot* OotPsp_AllocTimerSlot(OSTimer* timer) {
    s32 i;

    for (i = 0; i < OOT_PSP_MAX_TIMERS; i++) {
        if (sTimerSlots[i].timer == NULL) {
            sTimerSlots[i].timer = timer;
            return &sTimerSlots[i];
        }
    }

    return NULL;
}

static SceUID OotPsp_GetMesgQueueSema(OSMesgQueue* mq) {
    SceUID sema;

    if (mq == NULL) {
        return -1;
    }

    sema = (SceUID)(s32)(uintptr_t)mq->mtqueue;
    return (sema > 0) ? sema : -1;
}

static void OotPsp_LockMesgQueue(OSMesgQueue* mq) {
    SceUID sema = OotPsp_GetMesgQueueSema(mq);

    if (sema > 0) {
        sceKernelWaitSema(sema, 1, NULL);
    }
}

static void OotPsp_UnlockMesgQueue(OSMesgQueue* mq) {
    SceUID sema = OotPsp_GetMesgQueueSema(mq);

    if (sema > 0) {
        sceKernelSignalSema(sema, 1);
    }
}

static int OotPsp_TimerThread(SceSize args, void* argp) {
    OotPspTimerSlot* slot = (OotPspTimerSlot*)argp;

    (void)args;

    while (slot != NULL && slot->active) {
        u32 delayUsec = (u32)OS_CYCLES_TO_USEC(slot->timer->value);

        if (delayUsec > 0) {
            sceKernelDelayThread(delayUsec);
        }

        if (!slot->active) {
            break;
        }

        if (slot->timer->mq != NULL) {
            osSendMesg(slot->timer->mq, slot->timer->msg, OS_MESG_NOBLOCK);
        }

        if (slot->timer->interval == 0) {
            break;
        }

        slot->timer->value = slot->timer->interval;
    }

    if (slot != NULL) {
        slot->active = false;
        slot->timer = NULL;
        slot->sceThreadId = -1;
    }

    return 0;
}

static u16 OotPsp_MapButtons(const SceCtrlData* pad) {
    u16 button = 0;

    if (pad->Buttons & PSP_CTRL_CROSS) {
        button |= BTN_A;
    }
    if (pad->Buttons & PSP_CTRL_CIRCLE) {
        button |= BTN_B;
    }
    if (pad->Buttons & PSP_CTRL_TRIANGLE) {
        button |= BTN_CUP;
    }
    if (pad->Buttons & PSP_CTRL_SQUARE) {
        button |= BTN_CLEFT;
    }
    if (pad->Buttons & PSP_CTRL_LTRIGGER) {
        button |= BTN_Z;
    }
    if (pad->Buttons & PSP_CTRL_RTRIGGER) {
        button |= BTN_R;
    }
    if (pad->Buttons & PSP_CTRL_START) {
        button |= BTN_START;
    }
    if (pad->Buttons & PSP_CTRL_SELECT) {
        button |= BTN_L;
    }
    if (pad->Buttons & PSP_CTRL_UP) {
        button |= BTN_CUP;
    }
    if (pad->Buttons & PSP_CTRL_DOWN) {
        button |= BTN_CDOWN;
    }
    if (pad->Buttons & PSP_CTRL_LEFT) {
        button |= BTN_CLEFT;
    }
    if (pad->Buttons & PSP_CTRL_RIGHT) {
        button |= BTN_CRIGHT;
    }

    return button;
}

static s8 OotPsp_MapStick(u8 raw) {
    s32 centered = (s32)raw - 128;

    if (centered < -80) {
        centered = -80;
    }
    if (centered > 80) {
        centered = 80;
    }

    return centered;
}

void osSyncPrintf(const char* fmt, ...) {
    va_list args;

    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

void isPrintfInit(void) {
}

void __osInitialize_common(void) {
    memset(sThreadSlots, 0, sizeof(sThreadSlots));
    memset(sTimerSlots, 0, sizeof(sTimerSlots));
    memset(sControllerPads, 0, sizeof(sControllerPads));
    memset(sControllerStatus, 0, sizeof(sControllerStatus));
    memset(&sCurrentViContext, 0, sizeof(sCurrentViContext));

    osMemSize = 0x02000000;
    osClockRate = OS_CLOCK_RATE;
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
}

void __osInitialize_autodetect(void) {
}

u32 osGetMemSize(void) {
    return osMemSize;
}

void osCreateMesgQueue(OSMesgQueue* mq, OSMesg* msgBuf, s32 count) {
    SceUID sema;
    char name[32];

    mq->mtqueue = NULL;
    mq->fullqueue = NULL;
    mq->validCount = 0;
    mq->first = 0;
    mq->msgCount = count;
    mq->msg = msgBuf;

    snprintf(name, sizeof(name), "oot-mq-%lu", (unsigned long)sMesgQueueSemaIndex++);
    sema = sceKernelCreateSema(name, 0, 1, 1, NULL);
    if (sema > 0) {
        mq->mtqueue = (OSThread*)(uintptr_t)sema;
    }
}

s32 osSendMesg(OSMesgQueue* mq, OSMesg msg, s32 flag) {
    while (true) {
        OotPsp_LockMesgQueue(mq);
        if (!MQ_IS_FULL(mq)) {
            mq->msg[(mq->first + mq->validCount) % mq->msgCount] = msg;
            mq->validCount++;
            OotPsp_UnlockMesgQueue(mq);
            return 0;
        }

        OotPsp_UnlockMesgQueue(mq);
        if (flag == OS_MESG_NOBLOCK) {
            return -1;
        }

        sceKernelDelayThread(1000);
    }
}

s32 osJamMesg(OSMesgQueue* mq, OSMesg msg, s32 flag) {
    while (true) {
        OotPsp_LockMesgQueue(mq);
        if (!MQ_IS_FULL(mq)) {
            mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
            mq->msg[mq->first] = msg;
            mq->validCount++;
            OotPsp_UnlockMesgQueue(mq);
            return 0;
        }

        OotPsp_UnlockMesgQueue(mq);
        if (flag == OS_MESG_NOBLOCK) {
            return -1;
        }

        sceKernelDelayThread(1000);
    }
}

s32 osRecvMesg(OSMesgQueue* mq, OSMesg* msg, s32 flag) {
    while (true) {
        OotPsp_LockMesgQueue(mq);
        if (!MQ_IS_EMPTY(mq)) {
            if (msg != NULL) {
                *msg = mq->msg[mq->first];
            }

            mq->first = (mq->first + 1) % mq->msgCount;
            mq->validCount--;
            OotPsp_UnlockMesgQueue(mq);
            return 0;
        }

        OotPsp_UnlockMesgQueue(mq);
        if (flag == OS_MESG_NOBLOCK) {
            return -1;
        }

        sceKernelDelayThread(1000);
    }
}

void osSetEventMesg(UNUSED OSEvent e, UNUSED OSMesgQueue* mq, UNUSED OSMesg msg) {
}

void osCreateThread(OSThread* thread, OSId id, void (*entry)(void*), void* arg, void* sp, OSPri pri) {
    OotPspThreadSlot* slot = OotPsp_AllocThreadSlot(thread);
    char name[32];

    memset(thread, 0, sizeof(*thread));
    thread->id = id;
    thread->priority = pri;
    thread->state = OS_STATE_STOPPED;
    thread->context.sp = (u32)(uintptr_t)sp;

    if (slot == NULL) {
        return;
    }

    snprintf(name, sizeof(name), "oot-thr-%ld", (long)id);
    slot->entry = entry;
    slot->arg = arg;
    slot->sceThreadId =
        sceKernelCreateThread(name, OotPsp_ThreadTrampoline, 0x20 + (OS_PRIORITY_APPMAX - pri), 0x20000,
                              THREAD_ATTR_USER | THREAD_ATTR_VFPU, NULL);
}

void osStartThread(OSThread* thread) {
    OotPspThreadSlot* slot = OotPsp_FindThreadSlot(thread);

    if (slot == NULL || slot->sceThreadId < 0) {
        return;
    }

    thread->state = OS_STATE_RUNNABLE;
    sceKernelStartThread(slot->sceThreadId, sizeof(thread), thread);
}

void osDestroyThread(OSThread* thread) {
    OotPspThreadSlot* slot = OotPsp_FindThreadSlot(thread);

    if (slot != NULL && slot->sceThreadId >= 0) {
        sceKernelTerminateDeleteThread(slot->sceThreadId);
        memset(slot, 0, sizeof(*slot));
    }

    if (thread != NULL) {
        thread->state = OS_STATE_STOPPED;
    }
}

void osStopThread(OSThread* thread) {
    osDestroyThread(thread);
}

void osYieldThread(void) {
    sceKernelDelayThread(0);
}

void osSetThreadPri(OSThread* thread, OSPri pri) {
    OotPspThreadSlot* slot = OotPsp_FindThreadSlot(thread);

    if (thread != NULL) {
        thread->priority = pri;
    }

    if (slot != NULL && slot->sceThreadId >= 0) {
        sceKernelChangeThreadPriority(slot->sceThreadId, 0x20 + (OS_PRIORITY_APPMAX - pri));
    }
}

OSPri osGetThreadPri(OSThread* thread) {
    if (thread == NULL) {
        return 0;
    }

    return thread->priority;
}

OSId osGetThreadId(OSThread* thread) {
    if (thread == NULL) {
        return sceKernelGetThreadId();
    }

    return thread->id;
}

OSThread* __osGetActiveQueue(void) {
    return NULL;
}

OSThread* __osGetCurrFaultedThread(void) {
    return NULL;
}

OSTime osGetTime(void) {
    u64 ticks = 0;

    sceRtcGetCurrentTick(&ticks);
    return ticks;
}

void osSetTime(UNUSED OSTime time) {
}

u32 osGetCount(void) {
    return sceKernelGetSystemTimeLow();
}

s32 osSetTimer(OSTimer* timer, OSTime countdown, OSTime interval, OSMesgQueue* mq, OSMesg msg) {
    OotPspTimerSlot* slot = OotPsp_FindTimerSlot(timer);
    char name[32];

    if (slot == NULL) {
        slot = OotPsp_AllocTimerSlot(timer);
    }
    if (slot == NULL) {
        return -1;
    }

    timer->value = countdown;
    timer->interval = interval;
    timer->mq = mq;
    timer->msg = msg;

    slot->active = true;
    snprintf(name, sizeof(name), "oot-timer-%02d", (int)(slot - sTimerSlots));
    slot->sceThreadId = sceKernelCreateThread(name, OotPsp_TimerThread, 0x18, 0x1000, THREAD_ATTR_USER, NULL);
    if (slot->sceThreadId < 0) {
        slot->active = false;
        slot->timer = NULL;
        return -1;
    }

    sceKernelStartThread(slot->sceThreadId, sizeof(slot), slot);
    return 0;
}

s32 osStopTimer(OSTimer* timer) {
    OotPspTimerSlot* slot = OotPsp_FindTimerSlot(timer);

    if (slot == NULL) {
        return -1;
    }

    slot->active = false;
    if (slot->sceThreadId >= 0) {
        sceKernelTerminateDeleteThread(slot->sceThreadId);
    }
    slot->timer = NULL;
    slot->sceThreadId = -1;
    return 0;
}

OSIntMask osGetIntMask(void) {
    return sIntMask;
}

OSIntMask osSetIntMask(OSIntMask mask) {
    OSIntMask prev = sIntMask;

    sIntMask = mask;
    return prev;
}

s32 __osDisableInt(void) {
    return 0;
}

void __osRestoreInt(UNUSED s32 enable) {
}

void __osSetHWIntrRoutine(UNUSED OSHWIntr intr, UNUSED s32 (*callback)(void), UNUSED void* sp) {
}

void __osGetHWIntrRoutine(UNUSED OSHWIntr intr, s32 (**callbackOut)(void), void** spOut) {
    if (callbackOut != NULL) {
        *callbackOut = NULL;
    }
    if (spOut != NULL) {
        *spOut = NULL;
    }
}

void __osSetSR(UNUSED u32 value) {
}

u32 __osGetSR(void) {
    return 0;
}

void __osSetFpcCsr(UNUSED u32 value) {
}

u32 __osGetFpcCsr(void) {
    return 0;
}

u32 __osGetCause(void) {
    return 0;
}

void __osSetCompare(UNUSED u32 value) {
}

void __osSetWatchLo(UNUSED u32 value) {
}

u32 __osProbeTLB(UNUSED void* addr) {
    return 0;
}

void osUnmapTLBAll(void) {
}

void osMapTLBRdb(void) {
}

void osWritebackDCache(UNUSED void* vaddr, UNUSED s32 nbytes) {
}

void osInvalDCache(UNUSED void* vaddr, UNUSED s32 nbytes) {
}

void osWritebackDCacheAll(void) {
}

void osInvalICache(UNUSED void* vaddr, UNUSED s32 nbytes) {
}

u32 osVirtualToPhysical(void* vaddr) {
    return (u32)(uintptr_t)vaddr;
}

void osCreatePiManager(OSPri pri, OSMesgQueue* cmdQueue, OSMesg* cmdBuf, s32 cmdMsgCnt) {
    (void)pri;
    sPiCmdQueue = cmdQueue;
    osCreateMesgQueue(cmdQueue, cmdBuf, cmdMsgCnt);
}

OSMesgQueue* osPiGetCmdQueue(void) {
    return sPiCmdQueue;
}

static OSPiHandle sCartHandle;

OSPiHandle* osCartRomInit(void) {
    memset(&sCartHandle, 0, sizeof(sCartHandle));
    sCartHandle.type = DEVICE_TYPE_CART;
    return &sCartHandle;
}

OSPiHandle* osDriveRomInit(void) {
    return &sCartHandle;
}

s32 osEPiStartDma(OSPiHandle* handle, OSIoMesg* mb, s32 direction) {
    (void)handle;
    (void)direction;

    if (mb != NULL && mb->dramAddr != NULL && mb->devAddr != 0 && mb->size != 0) {
        memcpy(mb->dramAddr, (void*)(uintptr_t)mb->devAddr, mb->size);
    }

    if (mb != NULL && mb->hdr.retQueue != NULL) {
        osSendMesg(mb->hdr.retQueue, mb, OS_MESG_NOBLOCK);
    }

    return 0;
}

s32 osPiStartDma(OSIoMesg* mb, s32 priority, s32 direction, uintptr_t devAddr, void* vAddr, size_t nbytes,
                 OSMesgQueue* mq) {
    (void)priority;
    (void)direction;

    if (vAddr != NULL && devAddr != 0 && nbytes != 0) {
        memcpy(vAddr, (const void*)devAddr, nbytes);
    }

    if (mq != NULL) {
        osSendMesg(mq, mb, OS_MESG_NOBLOCK);
    }

    return 0;
}

void osCreateViManager(UNUSED OSPri pri) {
}

void osViSetMode(OSViMode* mode) {
    sCurrentViMode = mode;
    sCurrentViContext.modep = mode;
}

void osViBlack(UNUSED u8 active) {
}

void osViSetSpecialFeatures(UNUSED u32 func) {
}

void osViSwapBuffer(void* frameBufPtr) {
    sCurrentFramebuffer = frameBufPtr;
    sNextFramebuffer = frameBufPtr;
    sCurrentViContext.framep = frameBufPtr;
}

void* osViGetCurrentFramebuffer(void) {
    return sCurrentFramebuffer;
}

void* osViGetNextFramebuffer(void) {
    return sNextFramebuffer;
}

void osViSetEvent(OSMesgQueue* mq, OSMesg msg, u32 retraceCount) {
    sViEventQueue = mq;
    sViEventMsg = msg;
    sCurrentViContext.mq = mq;
    sCurrentViContext.msg = msg;
    sCurrentViContext.retraceCount = retraceCount;
}

void osViSetYScale(f32 scale) {
    sCurrentViContext.y.factor = scale;
}

void osViSetXScale(f32 value) {
    sCurrentViContext.x.factor = value;
}

void osViExtendVStart(UNUSED u32 value) {
}

OSViContext* __osViGetCurrentContext(void) {
    return &sCurrentViContext;
}

s32 osContInit(OSMesgQueue* mq, u8* ctlBitfield, OSContStatus* status) {
    s32 i;

    (void)mq;

    for (i = 0; i < MAXCONTROLLERS; i++) {
        sControllerStatus[i].type = 0;
        sControllerStatus[i].status = 0;
        sControllerStatus[i].errno = CONT_ERR_NO_CONTROLLER;
    }

    sControllerStatus[0].type = CONT_TYPE_NORMAL;
    sControllerStatus[0].status = 0;
    sControllerStatus[0].errno = 0;

    if (ctlBitfield != NULL) {
        *ctlBitfield = 1;
    }
    if (status != NULL) {
        memcpy(status, sControllerStatus, sizeof(sControllerStatus));
    }

    return 0;
}

s32 osContStartQuery(UNUSED OSMesgQueue* mq) {
    return 0;
}

void osContGetQuery(OSContStatus* data) {
    if (data != NULL) {
        memcpy(data, sControllerStatus, sizeof(sControllerStatus));
    }
}

s32 osContSetCh(UNUSED u8 ch) {
    return 0;
}

s32 osContStartReadData(UNUSED OSMesgQueue* mq) {
    SceCtrlData pad;

    memset(sControllerPads, 0, sizeof(sControllerPads));
    sceCtrlPeekBufferPositive(&pad, 1);

    sControllerPads[0].button = OotPsp_MapButtons(&pad);
    sControllerPads[0].stick_x = OotPsp_MapStick(pad.Lx);
    sControllerPads[0].stick_y = -OotPsp_MapStick(pad.Ly);
    sControllerPads[0].errno = 0;

    return 0;
}

void osContGetReadData(OSContPad* contData) {
    if (contData != NULL) {
        memcpy(contData, sControllerPads, sizeof(sControllerPads));
    }
}

s32 osPfsIsPlug(UNUSED OSMesgQueue* mq, u8* pattern) {
    if (pattern != NULL) {
        *pattern = 0;
    }
    return PFS_ERR_NOPACK;
}

s32 osPfsInitPak(UNUSED OSMesgQueue* queue, UNUSED OSPfs* pfs, UNUSED s32 channel) {
    return PFS_ERR_NOPACK;
}

s32 osMotorInit(UNUSED OSMesgQueue* ctrlrqueue, UNUSED OSPfs* pfs, UNUSED s32 channel) {
    return PFS_ERR_DEVICE;
}

s32 __osMotorAccess(UNUSED OSPfs* pfs, UNUSED s32 vibrate) {
    return PFS_ERR_DEVICE;
}

s32 osAfterPreNMI(void) {
    return 0;
}

void osSpTaskLoad(UNUSED OSTask* intp) {
}

void osSpTaskStartGo(UNUSED OSTask* tp) {
    if (sViEventQueue != NULL) {
        osSendMesg(sViEventQueue, sViEventMsg, OS_MESG_NOBLOCK);
    }
}

void osSpTaskYield(void) {
}

u32 osSpTaskYielded(UNUSED OSTask* task) {
    return 0;
}

u32 osDpGetStatus(void) {
    return 0;
}

void osDpSetStatus(UNUSED u32 status) {
}

s32 osAiSetFrequency(u32 frequency) {
    return OotPspAudioBackend_SetFrequency(frequency);
}

u32 osAiGetLength(void) {
    return OotPspAudioBackend_GetLength();
}
