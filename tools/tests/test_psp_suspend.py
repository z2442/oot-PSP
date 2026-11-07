#!/usr/bin/env python3
"""Run the production suspend/restore functions with fault-injecting PSP stubs."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PORT = ROOT / 'src/port/psp'


def function(source, name):
    match = re.search(r'^(?:static )?[^\n]*\b' + name + r'\([^;]*?\)\s*\{', source, re.M)
    start = match.start()
    return source[start:source.index('\n}', source.index('{', start)) + 2] + '\n'


def run(directory, name, source, defines=()):
    path = directory / name
    path.with_suffix('.c').write_text(source)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    *defines, str(path.with_suffix('.c')), '-o', str(path)], check=True)
    subprocess.run([str(path)], check=True, timeout=15)


asset_source = (PORT / 'oot_psp_asset_loader.c').read_text()
allocation = re.search(r'typedef struct OotPspCacheAllocation \{.*?\} OotPspCacheAllocation;',
                       asset_source, re.S).group()
asset_stubs = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef int s32;
typedef int SceUID;
typedef long long SceOff;
#define OOT_PSP_CACHE_ALLOCATION_COUNT 16
#define OOT_PSP_PACKED_CACHE_BLOCK_SIZE 65536
#define OOT_PSP_SUSPEND_SNAPSHOT_PATH "snapshot"
#define PSP_O_RDONLY 1
#define PSP_O_WRONLY 2
#define PSP_O_CREAT 4
#define PSP_O_TRUNC 8
#define PSP_SEEK_SET 0
#define PSP_SEEK_END 2
#define PSP_SMEM_Addr 2
''' + allocation + r'''
static OotPspCacheAllocation sOotPspCacheAllocations[16];
static int sOotPspAssetsSuspended, sOotPspVolatileMemoryLocked;
static size_t sOotPspSuspendSnapshotSize, sOotPspVolatileMemorySize;
static size_t sOotPspVolatileCacheAllocationCount;
static u8* sOotPspPackedCacheData;
static int sOotPspPackedAssetUnavailable, sOotPspPackedAssetPositionKnown;
static u8 buffers[4][131072], original[4][131072], snapshot[1048576], wrong[131072];
static size_t fileSize, filePos;
static int loaderLocked, openFd, cacheCleared, opens, nextId, liveIds[16];
static int failOpen, failWrite, failRead, failClose, failFree, failUnlock, failLock, failAlloc, wrongAddress;
static int wrongId, freeCalls, allocCalls;
static void OotPsp_LockAssetLoader(void) { assert(!loaderLocked); loaderLocked=1; }
static void OotPsp_UnlockAssetLoader(void) { assert(loaderLocked); loaderLocked=0; }
static void OotPsp_ClosePackedAssetFile(void) { assert(loaderLocked); }
static void OotPsp_ClearPackedAssetCache(void) { cacheCleared++; }
static const char* OotPsp_ResolveRootPath(const char* p,char* b,size_t s) { (void)b;(void)s;return p; }
static int sceIoOpen(const char* p,int mode,int perm) {
    (void)p;(void)perm; assert(loaderLocked && !openFd); opens++;
    if (failOpen) return -1;
    if (mode & PSP_O_TRUNC) fileSize=0;
    filePos=0;openFd=mode;return 7;
}
static int sceIoClose(int fd) { assert(fd==7 && openFd);openFd=0;return failClose ? -1 : 0; }
static int sceIoRemove(const char* p) { (void)p;assert(!openFd);fileSize=0;return 0; }
static int sceIoWrite(int fd,const void* p,size_t size) {
    assert(fd==7 && openFd && loaderLocked);
    if (failWrite) return 0;
    // Force short writes and reads across every production chunk boundary.
    if (size>113) size=113;
    assert(filePos+size<=sizeof(snapshot));memcpy(snapshot+filePos,p,size);
    filePos+=size;fileSize=filePos;return size;
}
static int sceIoRead(int fd,void* p,size_t size) {
    assert(fd==7 && openFd && loaderLocked);
    if (failRead) return -1;
    if (size>127) size=127;
    if (size>fileSize-filePos) size=fileSize-filePos;
    memcpy(p,snapshot+filePos,size);filePos+=size;return size;
}
static SceOff sceIoLseek(int fd,SceOff offset,int mode) {
    assert(fd==7 && offset==0);filePos=mode==PSP_SEEK_END ? fileSize : 0;return filePos;
}
static int sceKernelFreePartitionMemory(int id) {
    freeCalls++;
    if (failFree && freeCalls==failFree) return -1;
    if (id==wrongId && wrongId) { wrongId=0;return 0; }
    for (int i=0;i<16;i++) if (liveIds[i]==id && id>0) {liveIds[i]=0;return 0;}
    assert(0);return -1;
}
static int sceKernelVolatileMemUnlock(int mode) {
    assert(mode==0 && loaderLocked);
    if (failUnlock) return -1;
    for (int i=0;i<16;i++) {
        OotPspCacheAllocation* a=&sOotPspCacheAllocations[i];
        if (a->volatileMemory) { assert(!liveIds[i]);memset(a->address,0xcd,a->size); }
    }
    return 0;
}
static int OotPsp_LockVolatileMemory(void) {
    if (failLock) return 0;
    sOotPspVolatileMemoryLocked=1;return 1;
}
static int sceKernelAllocPartitionMemory(int partition,const char* name,int mode,size_t size,void* address) {
    (void)name;assert(partition==5 && mode==PSP_SMEM_Addr && loaderLocked);
    assert(sOotPspVolatileMemoryLocked);allocCalls++;
    if (failAlloc && allocCalls==failAlloc) return -1;
    if (wrongAddress) { wrongId=++nextId;return wrongId; }
    for (int i=0;i<16;i++) {
        OotPspCacheAllocation* a=&sOotPspCacheAllocations[i];
        if (a->address==address) { assert(a->size==size && !liveIds[i]);return liveIds[i]=++nextId; }
    }
    assert(0);return -1;
}
static void* sceKernelGetBlockHeadAddr(int id) {
    if (id==wrongId && wrongId) return wrong;
    for (int i=0;i<16;i++) if (liveIds[i]==id) return sOotPspCacheAllocations[i].address;
    assert(0);return NULL;
}
static void sceKernelDcacheWritebackInvalidateAll(void) { assert(loaderLocked); }
'''
asset_tests = r'''
static void reset(int type) {
    assert(!loaderLocked && !openFd);
    memset(sOotPspCacheAllocations,0,sizeof(sOotPspCacheAllocations));
    memset(liveIds,0,sizeof(liveIds));
    failOpen=failWrite=failRead=failClose=failFree=failUnlock=failLock=failAlloc=wrongAddress=0;
    cacheCleared=opens=allocCalls=freeCalls=wrongId=0;nextId=20;
    sOotPspVolatileCacheAllocationCount=0;sOotPspVolatileMemorySize=4*1024*1024;
    sOotPspVolatileMemoryLocked=type!=0;sOotPspPackedCacheData=buffers[2];
    for (int i=0;i<4;i++) {
        OotPspCacheAllocation* a=&sOotPspCacheAllocations[i];
        a->address=buffers[i];a->size=i==0 ? 90017 : 1024;
        a->volatileMemory=type && (i!=1) && (type!=2 || i==2);
        if (a->volatileMemory) {liveIds[i]=++nextId;a->blockId=liveIds[i];sOotPspVolatileCacheAllocationCount++;}
        else a->blockId=-1;
        for(size_t j=0;j<a->size;j++) buffers[i][j]=(u8)(j*37+i*71);
    }
    // Model runtime relocation/patching: restored bytes must retain mutations.
    buffers[0][77]^=0x57;memcpy(original,buffers,sizeof(buffers));
}
static void verify(void) {
    assert(!loaderLocked && !sOotPspAssetsSuspended && !openFd);
    assert(cacheCleared==sOotPspCacheAllocations[2].volatileMemory);
    for (int i=0;i<4;i++) {
        OotPspCacheAllocation* a=&sOotPspCacheAllocations[i];
        assert(a->address==buffers[i]);
        if (a->address!=sOotPspPackedCacheData || !a->volatileMemory)
            assert(!memcmp(a->address,original[i],a->size));
        if (a->volatileMemory) assert(a->blockId==liveIds[i] && a->blockId>0);
    }
}
int main(void) {
    // Repeated cycles preserve exact pointers and modified bytes, and use no new heap buffers.
    for (int cycle=0;cycle<20;cycle++) {
        reset(1);assert(OotPsp_AssetPrepareSuspend());
        assert(loaderLocked && !sOotPspVolatileMemoryLocked);
        assert(fileSize==90017+1024 && buffers[0][77]==0xcd);
        assert(OotPsp_AssetResume());verify();assert(!fileSize);
    }
    // No volatile data / packed-cache-only require no writable snapshot file.
    for (int type=0;type<=2;type+=2) {
        reset(type);failOpen=1;assert(OotPsp_AssetPrepareSuspend());
        assert(OotPsp_AssetResume());verify();assert(!opens);
    }
    // Preparation errors must not free memory or leave readers blocked.
    for (int error=0;error<3;error++) {
        reset(1);failOpen=error==0;failWrite=error==1;failClose=error==2;
        assert(!OotPsp_AssetPrepareSuspend());assert(!loaderLocked && !freeCalls);
        assert(!memcmp(buffers,original,sizeof(buffers)));assert(OotPsp_AssetResume());
    }
    // Partial release and unlock failure can be rolled back without sleeping.
    for (int error=0;error<2;error++) {
        reset(1);failFree=error==0 ? 2 : 0;failUnlock=error==1;
        assert(!OotPsp_AssetPrepareSuspend());assert(loaderLocked);
        failFree=failUnlock=0;assert(OotPsp_AssetResume());verify();
    }
    // Every restore failure retains the reader lock and supports retry.
    for (int error=0;error<7;error++) {
        reset(1);assert(OotPsp_AssetPrepareSuspend());
        size_t savedSize=fileSize;u8 savedByte=snapshot[100];
        failLock=error==0;failAlloc=error==1 ? 2 : 0;wrongAddress=error==2;
        failOpen=error==3;failRead=error==4;
        if (error==5) fileSize--;
        if (error==6) snapshot[100]^=0x80;
        assert(!OotPsp_AssetResume());assert(loaderLocked && sOotPspAssetsSuspended);
        assert(!openFd);
        failLock=failAlloc=wrongAddress=failOpen=failRead=0;
        fileSize=savedSize;snapshot[100]=savedByte;
        assert(OotPsp_AssetResume());verify();
    }
    puts("Suspend assets: exact addresses/bytes, warm normal cache, volatile cache discard, partial I/O and 12 fault paths passed");
}
'''
asset_functions = ''.join(function(asset_source, name) for name in (
    'OotPsp_SuspendChecksum', 'OotPsp_TransferSuspendBytes',
    'OotPsp_AssetPrepareSuspend', 'OotPsp_AssetResume'))

coordinator = (PORT / 'oot_psp_suspend.c').read_text()
coordinator = re.sub(r'^#include .*\n', '', coordinator, flags=re.M)
coordinator_stubs = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PSP_POWER_CB_RESUMING 1
#define PSP_POWER_CB_POWER_SWITCH 2
#define PSP_POWER_CB_SUSPENDING 4
#define PSP_POWER_CB_RESUME_COMPLETE 8
#define OOT_PSP_WAKE_DIAGNOSTICS 1
#define PSP_CTRL_HOME 1
#define PSP_CTRL_CROSS 2
typedef struct {unsigned Buttons;} SceCtrlData;
void OotPspSuspend_NotifyPowerEvent(int flags);
static int locked, audioParked, assetsParked, rendererParked, sleeps, restores, retries, audioResumes;
static int failAudioPrepare, failAssetsPrepare, failRestore, failAudioResume, failUnlock;
static int failGraphicsPrepare, failGraphicsResume;
static int traceResumes, wakeForAudio;
static unsigned clockNow, completionAt, completionDelay=676000;
static int firmwareComplete, completionPending, resumeMode, wakeWaitScreens;
static void OotPspWakeTrace_Poll(void) {}
static void OotPspWakeTrace_Checkpoint(const char* stage) {
    assert(locked);
    if(!strcmp(stage,"before_sleep")) assert(audioParked && assetsParked);
    else if(!strcmp(stage,"firmware_complete")) assert(audioParked && assetsParked && firmwareComplete);
    else if(!strcmp(stage,"assets_restored")) assert(audioParked && !assetsParked);
    else {assert(!strcmp(stage,"me_restarted"));assert(!audioParked && !assetsParked);}
}
static void OotPspWakeTrace_NotifyPowerEvent(int flags) {(void)flags;}
static void OotPspWakeTrace_Resume(void) {
    assert(locked && !audioParked && !assetsParked);traceResumes++;
}
static int eventCount;
static char events[128];
static void event(char code) {assert(eventCount<(int)sizeof(events)-1);events[eventCount++]=code;events[eventCount]=0;}
static int scePowerLock(int n) {assert(!n && !locked);locked=1;event('L');return 0;}
static int scePowerUnlock(int n) {
    assert(!n && locked && audioParked && assetsParked && rendererParked);event('U');
    if (failUnlock) return -1;
    locked=0;sleeps++;wakeForAudio=1;
    firmwareComplete=0;completionPending=1;
    // Callback while suspending must not queue another suspend.
    OotPspSuspend_NotifyPowerEvent(PSP_POWER_CB_SUSPENDING);
    return 0;
}
static void scePowerSetClockFrequency(int a,int b,int c) {
    assert(a==333 && b==333 && c==166 && locked && audioParked && firmwareComplete);
}
static unsigned sceKernelGetSystemTimeLow(void) {return clockNow;}
static void completeWake(void) {
    assert(audioParked && assetsParked && rendererParked);
    firmwareComplete=1;completionPending=0;event('C');
    OotPspSuspend_NotifyPowerEvent(PSP_POWER_CB_RESUME_COMPLETE);
}
static void sceKernelDelayThread(unsigned delay) {
    clockNow+=delay;
    if(completionPending==1 && !locked) {
        if(resumeMode==1) {
            OotPspSuspend_NotifyPowerEvent(PSP_POWER_CB_RESUMING);completeWake();
        } else if(resumeMode==2) completeWake();
        else {
            OotPspSuspend_NotifyPowerEvent(PSP_POWER_CB_RESUMING);
            completionAt=clockNow+completionDelay;completionPending=2;
        }
    } else if(completionPending==2 && resumeMode!=3 && (int)(clockNow-completionAt)>=0) {
        completeWake();
    }
}
static void sceKernelExitGame(void) {abort();}
static void sceCtrlReadBufferPositive(SceCtrlData* p,int n) {
    assert(n==1);p->Buttons=(retries++%2) ? PSP_CTRL_CROSS : 0;
    if(resumeMode==3 && p->Buttons==PSP_CTRL_CROSS && !firmwareComplete) completeWake();
}
static int OotPspRenderer_PrepareSuspend(void) {rendererParked=1;event('V');return !failGraphicsPrepare;}
static int OotPspRenderer_Resume(void) {
    assert(locked && audioParked && assetsParked && firmwareComplete);event('G');
    if(failGraphicsResume) {failGraphicsResume--;return 0;}return 1;
}
static void OotPspRenderer_RenderSuspendMessage(const char* a,const char* b) {
    (void)b;if(!strcmp(a,"PSP wake did not finish")) wakeWaitScreens++;
}
static int OotPspAudioBackend_PrepareSuspend(void) {assert(locked && rendererParked);audioParked=1;event('A');return !failAudioPrepare;}
static const char* OotPspAudioBackend_GetSuspendError(void) {return "Audio workers did not pause";}
static int OotPsp_AssetPrepareSuspend(void) {assert(locked && audioParked);assetsParked=1;event('S');return !failAssetsPrepare;}
static int OotPsp_AssetResume(void) {
    assert(locked && (audioParked || failGraphicsPrepare) && !completionPending);event('R');
    if (failRestore) {failRestore--;return 0;}
    assetsParked=0;restores++;return 1;
}
static int OotPspAudioBackend_Resume(int didSleep) {
    assert(didSleep==wakeForAudio);
    assert(locked && !assetsParked && !completionPending);event('B');
    if (failAudioResume) {failAudioResume--;return 0;}
    audioParked=0;audioResumes++;wakeForAudio=0;return 1;
}
'''
coordinator_tests = r'''
int main(void) {
    assert(OotPspSuspend_Init());
    // No power request: no extra work or per-frame SDK polling.
    eventCount=0;events[0]=0;OotPspSuspend_Poll();assert(!eventCount);
    for(int i=0;i<3;i++) {
        OotPspSuspend_NotifyPowerEvent(PSP_POWER_CB_POWER_SWITCH);
        OotPspSuspend_Poll();assert(locked && !audioParked && !assetsParked);
        int before=sleeps;OotPspSuspend_Poll();assert(sleeps==before);
    }
    assert(sleeps==3 && restores==3 && audioResumes==3);
    assert(!strcmp(events,"VAVSULCGRBVAVSULCGRBVAVSULCGRB"));
    failRestore=2;failAudioResume=1;
    OotPspSuspend_NotifyPowerEvent(PSP_POWER_CB_SUSPENDING);OotPspSuspend_Poll();
    assert(sleeps==4 && retries==6 && !audioParked && !assetsParked);
    // Preparation/unlock failures roll back under the lock without sleeping.
    for(int i=0;i<3;i++) {
        failAudioPrepare=i==0;failAssetsPrepare=i==1;failUnlock=i==2;
        OotPspSuspend_NotifyPowerEvent(PSP_POWER_CB_POWER_SWITCH);OotPspSuspend_Poll();
        assert(sleeps==4 && locked && !audioParked && !assetsParked);
    }
    failAudioPrepare=failAssetsPrepare=failUnlock=0;failGraphicsPrepare=1;
    OotPspSuspend_NotifyPowerEvent(PSP_POWER_CB_POWER_SWITCH);OotPspSuspend_Poll();
    assert(sleeps==4 && locked && !audioParked && !assetsParked);
    failGraphicsPrepare=0;failGraphicsResume=1;
    OotPspSuspend_NotifyPowerEvent(PSP_POWER_CB_POWER_SWITCH);OotPspSuspend_Poll();
    assert(sleeps==5 && locked && !audioParked && !assetsParked && !failGraphicsResume);
    assert(traceResumes==sleeps);
    // Completion may be combined with RESUMING or arrive without it. Stale
    // completion flags from an earlier cycle must not satisfy a new wake.
    for(resumeMode=0;resumeMode<3;resumeMode++) {
        OotPspSuspend_NotifyPowerEvent(PSP_POWER_CB_RESUME_COMPLETE);
        OotPspSuspend_NotifyPowerEvent(PSP_POWER_CB_POWER_SWITCH);OotPspSuspend_Poll();
        assert(firmwareComplete && !audioParked && !assetsParked);
    }
    // A completion delayed beyond the timeout keeps every reader parked
    // through the retry screen; it cannot silently continue recovery.
    resumeMode=3;
    OotPspSuspend_NotifyPowerEvent(PSP_POWER_CB_POWER_SWITCH);OotPspSuspend_Poll();
    assert(wakeWaitScreens==1 && firmwareComplete && !audioParked);
    // Compare deadlines safely when the 32-bit microsecond counter wraps.
    resumeMode=0;clockNow=~0U-500000U;
    OotPspSuspend_NotifyPowerEvent(PSP_POWER_CB_POWER_SWITCH);OotPspSuspend_Poll();
    assert(firmwareComplete && clockNow<1000000U);
    assert(traceResumes==sleeps);
    puts("Suspend coordinator: delayed/combined/complete-only wake, missing callback retry, timer wrap and ordered recovery passed");
}
'''

audio_source = (PORT / 'oot_psp_audio_backend.c').read_text()
audio_functions = ''.join(function(audio_source, name) for name in (
    'OotPspAudioBackend_BootMe', 'OotPspAudioBackend_InitMe',
    'OotPspAudioBackend_HoldMeReset', 'OotPspAudioBackend_ResetMeAfterSuspend',
    'OotPspAudioBackend_ClearPendingMeCommand',
    'OotPspAudioBackend_ParkForSuspend', 'OotPspAudioBackend_GetSuspendError', 'OotPspAudioBackend_PrepareSuspend',
    'OotPspAudioBackend_VerifyMeAfterSuspend',
    'OotPspAudioBackend_Resume', 'OotPspAudioBackend_MeJobIsRunning',
    'OotPspAudioBackend_WaitForCommandsLocked', 'OotPspAudioBackend_SubmitSynthesis'))
# The host has a different ISA. Keep the compiler ordering barrier in the
# production helper; only remove the Allegrex-specific instruction mnemonic.
audio_functions = audio_functions.replace('__asm__ volatile("sync" ::: "memory")',
                                          '__asm__ volatile("" ::: "memory")')
audio_stubs = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
#include <string.h>
#include <ucontext.h>
typedef uint32_t u32;
typedef int32_t s32;
typedef int SceUID;
typedef unsigned int SceUInt;
typedef int16_t s16;
typedef struct {int words[2];} Acmd;
enum {OOT_PSP_AUDIO_ME_STATE_BOOTING, OOT_PSP_AUDIO_ME_STATE_IDLE,
      OOT_PSP_AUDIO_ME_STATE_RUN, OOT_PSP_AUDIO_ME_STATE_SYNTH_RUN,
      OOT_PSP_AUDIO_ME_STATE_QUEUE_BUFFER, OOT_PSP_AUDIO_ME_STATE_STOP,
      OOT_PSP_AUDIO_ME_STATE_HALTED, OOT_PSP_AUDIO_ME_STATE_FAULT};
#define OOT_PSP_AUDIO_MEDIA_ENGINE 1
#define OOT_PSP_AUDIO_ME_TIMEOUT_US 250000
#define OOT_PSP_AUDIO_ME_WAKE_TIMEOUT_US 5000000
#define OOT_PSP_AUDIO_ME_MAX_JOB_US 2000000
#define OOT_PSP_AUDIO_ME_POLL_USEC 100
#define OOT_PSP_AUDIO_ME_COMPLETION_POLL_USEC 1000
#define OOT_PSP_AUDIO_ME_COMPLETION_WAIT_USEC 10000
#define SCE_KERNEL_ERROR_WAIT_TIMEOUT (-2)
#define AUDIO_DIAG_INCREMENT(value) ((void)0)
#define OOT_PSP_AUDIO_CHANNELS 2
#define OOT_PSP_AUDIO_OUTPUT_CHUNK_FRAMES 768
#define PSP_AUDIO_FORMAT_STEREO 0
#define PSP_AUDIO_VOLUME_MAX 32768
#define PSP_MECODEC_INT 3
#define OOT_PSP_AUDIO_ME_PROGRESS_ENTERED 0x10
#define OOT_PSP_AUDIO_ME_PROGRESS_BOOT_RELEASED 0x20
#define OOT_PSP_AUDIO_ME_PROGRESS_SYNTH_INIT 0x30
#define OOT_PSP_AUDIO_ME_PROGRESS_MIXER_INIT 0x40
#define OOT_PSP_AUDIO_ME_PROGRESS_READY 0x100
#define OOT_PSP_AUDIO_ME_OPCODE_PROFILE NULL
#define UNUSED __attribute__((unused))
#define meLibSync() meSync()
#define ERROR_ON_ME_IMG (-6)
#define ERROR_ON_KINIT (-5)
static volatile s32 sAudioSuspendRequested, sAudioProducerParked, sAudioOutputParked;
static volatile s32 sAudioProducerThreadRunning, sAudioOutputThreadRunning;
static s32 sAudioMeBootStarted, sAudioMeBootResult, sAudioMeInitialized;
static s32 sAudioMeRestartAfterSuspend, sAudioMeCompletionInterruptEnabled;
static s32 sAudioMeLibraryInitialized, sAudioMeResumeStarted;
static u32 sAudioMeWakeGeneration, firmwareWakeGeneration, wakeHookAt;
static int wakeHookPending, requireWakeHook;
static Acmd sAudioMeWakeProbe;
static s32 sAudioMeState, sAudioMeCompletionInterruptReady;
static volatile u32 sAudioMeProgress;
static s32 sAudioMeCompletionInterruptRegistered;
static s32 sAudioSuspendOutputReleased, sAudioSuspendReleasePending, sAudioHardwareSrc, sAudioOutputChannel;
static const char* sAudioSuspendError;
static char sAudioMeSetupError[80];
static s32 sAudioPlaybackPrimed, sAudioOutputFrames, sAudioPendingOutputFrames;
static u32 sAudioSourceChunkFrames=384, sAudioSourceFrequency=22050;
static u32 sAudioMeCmdList, sAudioMeCmdCount, sAudioMeQueueFrames, sAudioMeQueueSrc;
static u32 sAudioMeQueueWritePos, sAudioMeQueueResultWritePos, sAudioWritePos;
static u32 now, HW_SYS_RESET_ENABLE;
static int bootPending, failEntry, failReady, failReset, holds, witnessTable, tableSelections;
static int cooperativeMe, onMe, entrySyncYielded, readySyncYielded, workerHalted;
static ucontext_t cpuContext, meContext;
static unsigned char meHostStack[65536];
static void meLibOnProcess(void);
void OotPspAudioBackend_RunMe(void);
static int ackWorkers, ackMe, busyRelease, failReserve, initialized;
static int resets, reserves, released, waits, mixerInits, meFlushed, synthInits, mixerShutdowns;
static int meCacheDiscarded;
static int bridgeLoads, liveLock, liveCompletion, semaCreates, semaDeletes;
static int failReleaseHandler, lockDepth, cpuBuilds, synthPublishes, pcmPublishes;
static int drainRequired, drains, failDrain;
static int failEnable, interruptEnables, interruptRegistrations;
static int probeFailure, dropInterrupt, completionSignal, completionWaits, interruptDisables;
static int restoreDeliveryOnRegister, registeredHandler;
static int testingCommandWait, waitPolls, fallbackCalls, timeoutInterrupts;
static int failCommandWait, failCommandWaitAfter;
static u32 commandCompletesAt;
static u32 commandProgressInterval;
static s32 sAudioMeCommandPending, sAudioMePendingSynthesis, sAudioMePendingCmdCount;
static u32 sAudioMePendingStartTime, sAudioMePendingQueueFrames, sAudioMePendingQueueWritePos;
static const Acmd* sAudioMePendingCmdList;
static const s16* sAudioMePendingQueueSrc;
static struct {s32 maxAudioCmds;} gAudioCtx={64};
static SceUID sAudioMeCompletionSema=4;
static SceUID sAudioMeLockSema=3;
static s32 OotPspAudioBackend_BootMe(void);
static int OotPspAudioBackend_HoldMeReset(void);
static int OotPspAudioBackend_ResetMeAfterSuspend(void);
static s32 OotPspAudioBackend_EnsureMeLockSema(void);
static volatile s32* autoUnpark;
static jmp_buf halted;
static void runMe(void) {
    onMe=1;assert(swapcontext(&cpuContext,&meContext)==0);onMe=0;
}
static void yieldMe(void) {assert(swapcontext(&meContext,&cpuContext)==0);}
static void meSync(void) {
    // Delay the real worker before its entry guard and between READY and
    // IDLE. CPU startup must never change BOOTING or accept partial READY.
    if(cooperativeMe && onMe && !entrySyncYielded &&
       sAudioMeProgress==OOT_PSP_AUDIO_ME_PROGRESS_ENTERED) {
        entrySyncYielded=1;yieldMe();
        assert(sAudioMeState==OOT_PSP_AUDIO_ME_STATE_BOOTING);
    }
    if(cooperativeMe && onMe && !readySyncYielded &&
       sAudioMeProgress==OOT_PSP_AUDIO_ME_PROGRESS_READY) {
        readySyncYielded=1;yieldMe();
    }
}
static u32 sceKernelGetSystemTimeLow(void) { return now; }
static void advanceCommand(u32 until) {
    now=until;
    if(commandProgressInterval) sAudioMeProgress=0x10000+(now/commandProgressInterval)*16;
    if(now>=commandCompletesAt) sAudioMeState=OOT_PSP_AUDIO_ME_STATE_IDLE;
}
static u32 OotPspMeSuspend_WakeGeneration(void) {return firmwareWakeGeneration;}
static void finishWakeHook(void) {
    assert(sAudioSuspendRequested && sAudioProducerParked && sAudioOutputParked);
    HW_SYS_RESET_ENABLE=0x14;firmwareWakeGeneration++;wakeHookPending=0;
}
static void sceKernelDelayThread(u32 usec) {
    now+=usec;
    if(wakeHookPending && (s32)(now-wakeHookAt)>=0) finishWakeHook();
    if(testingCommandWait) {
        waitPolls++;
        advanceCommand(now);
        return;
    }
    if(autoUnpark) {assert(*autoUnpark);sAudioSuspendRequested=0;return;}
    if (!sAudioSuspendRequested) sAudioProducerParked=sAudioOutputParked=0;
    else if(ackWorkers) {
        if(sAudioProducerThreadRunning) sAudioProducerParked=1;
        if(sAudioOutputThreadRunning) sAudioOutputParked=1;
    }
    if(ackMe && sAudioMeState==OOT_PSP_AUDIO_ME_STATE_STOP) sAudioMeState=OOT_PSP_AUDIO_ME_STATE_HALTED;
    if(cooperativeMe && bootPending) {runMe();return;}
    if(bootPending && !failEntry && sAudioMeState==OOT_PSP_AUDIO_ME_STATE_BOOTING) {
        if(failReady) sAudioMeProgress=OOT_PSP_AUDIO_ME_PROGRESS_MIXER_INIT;
        else {
            sAudioMeProgress=OOT_PSP_AUDIO_ME_PROGRESS_READY;
            sAudioMeState=OOT_PSP_AUDIO_ME_STATE_IDLE;bootPending=0;
        }
    }
    if(sAudioMeState==OOT_PSP_AUDIO_ME_STATE_RUN) {
        assert(sAudioMeCmdCount==1 && !sAudioMeQueueSrc && !sAudioMeQueueFrames);
        assert(sAudioMeCmdList==(u32)(uintptr_t)&sAudioMeWakeProbe);
        if(probeFailure==1) return;
        sAudioMeState=probeFailure==2 ? OOT_PSP_AUDIO_ME_STATE_FAULT : OOT_PSP_AUDIO_ME_STATE_IDLE;
        sAudioMeProgress=probeFailure==3 ? 0 : 6;
        if(sAudioMeCompletionInterruptEnabled && !dropInterrupt) completionSignal=1;
    }
}
static void OotPspAudioBackend_WaitForCommands(void) {waits++;}
static int OotPspAudioBackend_IsOutputInitialized(void) {return initialized;}
static int release(void) {
    assert(sAudioSuspendRequested && (!sAudioProducerThreadRunning || sAudioProducerParked));
    assert(!sAudioOutputThreadRunning || sAudioOutputParked);
    // Firmware cannot release the last SRC buffer until it has drained.
    if(sAudioHardwareSrc && drainRequired) return -1;
    if(busyRelease) {busyRelease--;return -1;}
    released++;return 0;
}
static int sceAudioSRCChRelease(void) {assert(sAudioHardwareSrc);return release();}
static int sceAudioSRCOutputBlocking(int volume,void* buffer) {
    assert(volume==PSP_AUDIO_VOLUME_MAX && buffer==NULL && sAudioHardwareSrc);
    assert((!sAudioProducerThreadRunning || sAudioProducerParked) &&
           (!sAudioOutputThreadRunning || sAudioOutputParked));
    if(failDrain) return -1;
    drains++;drainRequired=0;return 0;
}
static int sceAudioChRelease(int c) {assert(!sAudioHardwareSrc && c==5);return release();}
static int reserve(void) {
    reserves++;assert(sAudioSuspendRequested);
    if(failReserve) {failReserve--;return -1;}
    return 0;
}
static int sceAudioSRCChReserve(u32 frames,u32 freq,int channels) {
    assert(frames==384 && freq==22050 && channels==2);return reserve();
}
static int sceAudioChReserve(int c,u32 frames,int format) {
    assert(c==5 && frames==768 && format==0);return reserve();
}
static void meLibReset(void) {
    assert(sAudioMeState==OOT_PSP_AUDIO_ME_STATE_BOOTING && (HW_SYS_RESET_ENABLE&0x14)==0x14);
    assert(!sAudioMeCmdList && !sAudioMeCmdCount && !sAudioMeQueueSrc && !sAudioMeQueueFrames);
    assert(!sAudioMeQueueWritePos && !sAudioMeQueueResultWritePos);
    assert(!sAudioMeCommandPending && !sAudioMePendingCmdList && !sAudioMePendingQueueFrames);
    HW_SYS_RESET_ENABLE=0;resets++;bootPending=1;
    if(cooperativeMe) {
        assert(getcontext(&meContext)==0);
        meContext.uc_stack.ss_sp=meHostStack;meContext.uc_stack.ss_size=sizeof(meHostStack);
        meContext.uc_link=&cpuContext;entrySyncYielded=readySyncYielded=workerHalted=0;
        makecontext(&meContext,meLibOnProcess,0);
    }
}
static int OotPspMeSuspend_Init(void) {bridgeLoads++;HW_SYS_RESET_ENABLE=0x14;meLibReset();return 0;}
static int meCoreGetTableIdFromWitnessWord(void) {return witnessTable;}
static void meCoreSelectSystemTable(int table) {assert(table==witnessTable && table>=2);tableSelections++;}
static int kcall(int (*f)(void),int segment) {
    assert(segment==0);
    if(f==OotPspAudioBackend_HoldMeReset) {
        if(requireWakeHook) assert(firmwareWakeGeneration!=sAudioMeWakeGeneration);
        holds++;if(failReset) {failReset--;return -1;}bootPending=0;
    }
    return f();
}
static void OotPspAudioBackend_DrainMeCompletion(void) {completionSignal=0;}
static void OotPspAudioBackend_WritebackRange(const void* p,u32 bytes) {
    assert(p==&sAudioMeWakeProbe && bytes==sizeof(sAudioMeWakeProbe));
}
static int sceKernelWaitSema(SceUID id,int count,SceUInt* timeout) {
    assert(id==4 && count==1 && timeout);completionWaits++;
    if(testingCommandWait) {
        // A broken semaphore may fail without blocking at all. Lost IRQ
        // delivery still lets the ME finish its command in shared memory.
        if(failCommandWait || (failCommandWaitAfter && completionWaits>failCommandWaitAfter)) return -1;
        if(dropInterrupt || now+*timeout<commandCompletesAt) {
            advanceCommand(now+*timeout);*timeout=0;
            return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
        }
        advanceCommand(commandCompletesAt);return 0;
    }
    assert(*timeout==2000);
    if(completionSignal) {completionSignal=0;return 0;}
    now+=*timeout;*timeout=0;return -1;
}
static int sceKernelDisableSubIntr(int intr,int sub) {
    assert(intr==PSP_MECODEC_INT && sub==0);interruptDisables++;return 0;
}
static int sceKernelEnableSubIntr(int intr,int sub) {
    assert(intr==PSP_MECODEC_INT && sub==0);interruptEnables++;
    if(failEnable) {failEnable--;return -1;}return 0;
}
static int OotPspAudioBackend_IsValidUid(int id) {return id>0;}
static SceUID sceKernelCreateSema(const char* name,int attr,int initial,int max,void* opts) {
    assert(!attr && max==1 && opts==NULL);semaCreates++;
    if(!strcmp(name,"OOT PSP AudioME")) {
        assert(initial==1 && !liveLock);liveLock=1;return 3;
    }
    assert(!initial && !liveCompletion);liveCompletion=1;return 4;
}

static int sceKernelSignalSema(int id,int value) {assert(id==4 && value==1);return 0;}
static int sceKernelRegisterSubIntrHandler(int intr,int sub,void (*handler)(int,void*),void* arg) {
    assert(intr==PSP_MECODEC_INT && sub==0 && handler && arg==NULL && !registeredHandler);
    registeredHandler=1;interruptRegistrations++;
    if(restoreDeliveryOnRegister) dropInterrupt=0;
    return 0;
}
static int sceKernelReleaseSubIntrHandler(int intr,int sub) {
    assert(intr==PSP_MECODEC_INT && sub==0 && registeredHandler);
    if(failReleaseHandler) {failReleaseHandler--;return -1;}
    registeredHandler=0;return 0;
}
static s32 OotPspAudioBackend_EnsureMeCompletionInterrupt(void);
// Stubs for the actual ME entry point's cold initialization and shutdown.
static int entryTest;
static void meLibDelayPipeline(void) {
    if(cooperativeMe && onMe) {yieldMe();return;}
    if(entryTest==1) sAudioMeState=OOT_PSP_AUDIO_ME_STATE_STOP;
    else if(entryTest==2) {assert(sAudioMeState==OOT_PSP_AUDIO_ME_STATE_HALTED);longjmp(halted,2);}
    else assert(0);
}
// Model the leaf assembly cache reset; the linked-code test verifies its
// opcodes and that it cannot touch a live C stack or call a writeback helper.
static void meLibOnProcess(void) {
    meCacheDiscarded=1;sAudioMeProgress=OOT_PSP_AUDIO_ME_PROGRESS_BOOT_RELEASED;
    OotPspAudioBackend_RunMe();
}
static void OotPspMixer_InitVme(void) {assert(meCacheDiscarded && synthInits);mixerInits++;}
static void OotPspAudioSynth_InitMe(void) {assert(meCacheDiscarded);synthInits++;}
static void meLibDcacheWritebackInvalidateAll(void) {
    assert(mixerShutdowns); // Full writeback is allowed only at shutdown.
    meFlushed=1;
}
static void meLibHalt(void) {
    assert(meFlushed && sAudioMeState==OOT_PSP_AUDIO_ME_STATE_HALTED);
    if(cooperativeMe && onMe) {workerHalted=1;yieldMe();assert(0);}
    longjmp(halted,1);
}
static void OotPspAudioSynth_MeInvalidateState(void) {assert(0);}
static void AudioSynth_BuildCommandListMe(Acmd* c,s32* n,s16* p,u32 f) {(void)c;(void)n;(void)p;(void)f;assert(0);}
static void OotPspAudioSynth_MeWritebackState(void) {assert(0);}
static void OotPspAudioBackend_MeWritebackRange(const void* p,u32 s) {(void)p;(void)s;assert(0);}
static void OotPspAudioBackend_MeInvalidateInputs(const Acmd* c,s32 n,const void* p,u32 s) {(void)c;(void)n;(void)p;(void)s;assert(0);}
static void OotPspMixer_ExecuteCommandListMe(const Acmd* c,s32 n,const void* p,volatile u32* progress) {
    (void)c;(void)n;(void)p;(void)progress;assert(0);
}
static void OotPspAudioBackend_MeQueueBuffer(int i) {(void)i;assert(0);}
static void OotPspAudioBackend_MeWritebackOutputs(const Acmd* c,s32 n,const void* p,u32 s) {(void)c;(void)n;(void)p;(void)s;assert(0);}
static void meLibSendExternalSoftInterrupt(void) {assert(0);}
static void OotPspMixer_ShutdownVme(void) {mixerShutdowns++;}
static void meLibEmitSoftwareInterrupt(void) {timeoutInterrupts++;}
static void OotPspAudioBackend_FallbackFromMe(const Acmd* c,s32 n,const s16* p,u32 frames,u32 position) {
    (void)c;(void)n;(void)p;(void)frames;(void)position;fallbackCalls++;sAudioMeInitialized=0;
}
static void OotPspAudioBackend_InvalidateRange(const void* p,u32 bytes) {(void)p;(void)bytes;}
static void OotPspAudioSynth_InvalidateMeState(void) {}
static void OotPspAudioBackend_InvalidateMeWritesFromCommands(const Acmd* c,s32 n,const void* p,u32 bytes) {
    (void)c;(void)p;assert(n==2 && bytes==32);
}
static void OotPspAudioBackend_InvalidateMeWrites(const Acmd* c,s32 n,const void* p,u32 bytes) {
    (void)c;(void)n;(void)p;assert(!bytes);
}
static void OotPspAudioBackend_PublishPendingMeQueue(void) {pcmPublishes+=sAudioMePendingQueueFrames!=0;}
static void OotPspAudioBackend_ResetMeWriteRanges(void) {}
static void OotPspAudioBackend_LockMe(void) {assert(liveLock && !lockDepth);lockDepth=1;}
static void OotPspAudioBackend_UnlockMe(void) {assert(lockDepth==1);lockDepth=0;}
static u32 OotPspAudioBackend_FreeFrames(void) {return 1024;}
static void OotPspAudioBackend_RunSynthesisCpu(Acmd* commands,s16* output,s32 frames,u32 writePos) {
    (void)commands;(void)output;(void)frames;(void)writePos;cpuBuilds++;assert(0);
}
static int AudioSynth_CanBuildCommandsOnMe(void) {return 1;}
static void OotPspAudioSynth_WritebackMeState(void) {assert(lockDepth==1);synthPublishes++;}
'''
audio_tests = r'''
static void reset(int src) {
    now=0;sAudioSuspendRequested=sAudioProducerParked=sAudioOutputParked=0;
    sAudioProducerThreadRunning=sAudioOutputThreadRunning=1;
    sAudioMeBootStarted=sAudioMeInitialized=1;sAudioMeBootResult=0;
    sAudioMeRestartAfterSuspend=0;sAudioMeLibraryInitialized=1;sAudioMeResumeStarted=0;
    bootPending=failEntry=failReady=failReset=holds=HW_SYS_RESET_ENABLE=tableSelections=0;witnessTable=2;
    cooperativeMe=onMe=entrySyncYielded=readySyncYielded=workerHalted=0;
    meCacheDiscarded=0;
    sAudioMeWakeGeneration=firmwareWakeGeneration=wakeHookAt=0;
    wakeHookPending=requireWakeHook=0;
    sAudioMeCompletionInterruptEnabled=sAudioMeCompletionInterruptReady=1;
    sAudioMeCompletionInterruptRegistered=1;
    sAudioSuspendOutputReleased=sAudioSuspendReleasePending=0;
    sAudioHardwareSrc=src;sAudioOutputChannel=5;sAudioMeState=OOT_PSP_AUDIO_ME_STATE_IDLE;
    ackWorkers=ackMe=initialized=1;busyRelease=3;failReserve=0;
    resets=reserves=released=waits=0;
    bridgeLoads=semaCreates=semaDeletes=0;liveLock=liveCompletion=1;
    sAudioMeLockSema=3;sAudioMeCompletionSema=4;
    failReleaseHandler=lockDepth=cpuBuilds=synthPublishes=pcmPublishes=0;
    drainRequired=1;drains=failDrain=0;
    failEnable=interruptEnables=interruptRegistrations=0;
    probeFailure=dropInterrupt=completionSignal=completionWaits=interruptDisables=0;
    restoreDeliveryOnRegister=testingCommandWait=waitPolls=fallbackCalls=timeoutInterrupts=0;
    failCommandWait=failCommandWaitAfter=0;
    commandProgressInterval=0;sAudioMeProgress=0;
    registeredHandler=1;sAudioMeCommandPending=sAudioMePendingSynthesis=0;
    sAudioMePendingQueueFrames=sAudioMePendingQueueWritePos=sAudioMePendingStartTime=0;
}
int main(void) {
    // Every coordinated boot initializes fresh DSP state on the original
    // stack. An early firmware entry remains inert until the explicit reset;
    // no register-context restore or interrupt initialization is involved.
    for(volatile int cycle=0;cycle<3;cycle++) {
        sAudioMeState=OOT_PSP_AUDIO_ME_STATE_BOOTING;entryTest=1;
        meFlushed=mixerInits=mixerShutdowns=synthInits=0;
        if(!setjmp(halted)) meLibOnProcess();
        assert(meFlushed && mixerInits==1 && synthInits==1 && mixerShutdowns==1);
        sAudioMeState=OOT_PSP_AUDIO_ME_STATE_HALTED;entryTest=2;
        if(!setjmp(halted)) meLibOnProcess();
        assert(mixerInits==1 && synthInits==1 && mixerShutdowns==1);
    }
    entryTest=0;
    reset(1);sAudioMeLibraryInitialized=0;sAudioMeBootStarted=0;
    assert(OotPspAudioBackend_BootMe()==0 && bridgeLoads==1 && resets==1);
    assert(OotPspAudioBackend_BootMe()==0 && bridgeLoads==1 && resets==1);
    // Run the actual C worker and actual CPU initializer as two coroutines.
    // Delay entry until InitMe is waiting, then preempt at ENTERED and at
    // READY before IDLE. Only the worker may finish this startup handshake.
    for(int cycle=0;cycle<4;cycle++) {
        reset(1);cooperativeMe=1;sAudioMeBootStarted=sAudioMeInitialized=0;
        HW_SYS_RESET_ENABLE=0x14;mixerInits=synthInits=mixerShutdowns=meFlushed=0;
        assert(OotPspAudioBackend_InitMe()==0);
        assert(entrySyncYielded && readySyncYielded && sAudioMeInitialized &&
               sAudioMeState==OOT_PSP_AUDIO_ME_STATE_IDLE);
        assert(mixerInits==1 && synthInits==1 && !mixerShutdowns && !cpuBuilds);
        sAudioMeState=OOT_PSP_AUDIO_ME_STATE_STOP;runMe();
        assert(workerHalted && mixerShutdowns==1 && meFlushed);
        cooperativeMe=0;
    }
    for(int route=0;route<2;route++) {
        reset(route);
        for(int cycle=0;cycle<20;cycle++) {
            assert(OotPspAudioBackend_PrepareSuspend());
            assert(sAudioProducerParked && sAudioOutputParked && sAudioSuspendRequested);
            assert(sAudioMeState==OOT_PSP_AUDIO_ME_STATE_HALTED && released==cycle+1);
            assert(liveLock && liveCompletion && registeredHandler && !sAudioMeInitialized);
            // Firmware ME completion can arrive after the ordinary power
            // completion callback. Do not boot until its reset write is done.
            wakeHookAt=now+676000;wakeHookPending=requireWakeHook=1;
            assert(OotPspAudioBackend_Resume(1));
            assert(!wakeHookPending && now>=wakeHookAt);
            requireWakeHook=0;
            assert(!sAudioSuspendRequested && !sAudioProducerParked && !sAudioOutputParked);
            assert(resets==cycle+1 && holds==cycle+1 && tableSelections==cycle+1 && reserves==cycle+1 && !bridgeLoads);
            assert(liveLock && liveCompletion && registeredHandler && sAudioMeInitialized);
            assert(!semaCreates && !semaDeletes);
            Acmd commands[2]={0};s16 pcm[16]={0};
            OotPspAudioBackend_SubmitSynthesis(commands,pcm,8);
            assert(!lockDepth && sAudioMeCommandPending && sAudioMePendingSynthesis);
            assert(sAudioMeState==OOT_PSP_AUDIO_ME_STATE_SYNTH_RUN && !cpuBuilds);
            sAudioMeCmdCount=2;testingCommandWait=1;commandCompletesAt=now+4500;
            OotPspAudioBackend_WaitForCommandsLocked();testingCommandWait=0;
            assert(!sAudioMeCommandPending && sAudioMeInitialized && pcmPublishes==cycle+1);
            assert(!fallbackCalls && !cpuBuilds && synthPublishes==cycle+1);
            assert(OotPspAudioBackend_Resume(0) && resets==cycle+1);
        }
    }
    reset(1);assert(OotPspAudioBackend_PrepareSuspend());failReserve=2;
    assert(!OotPspAudioBackend_Resume(0));assert(!OotPspAudioBackend_Resume(0));
    assert(sAudioProducerParked && sAudioOutputParked && !resets);
    assert(OotPspAudioBackend_Resume(0) && resets==1 && reserves==3);
    reset(1);ackWorkers=0;assert(!OotPspAudioBackend_PrepareSuspend());
    assert(!strcmp(OotPspAudioBackend_GetSuspendError(),"Audio workers did not pause"));
    assert(!released && !resets);assert(OotPspAudioBackend_Resume(0));
    reset(1);ackMe=0;assert(!OotPspAudioBackend_PrepareSuspend());
    assert(!strcmp(OotPspAudioBackend_GetSuspendError(),"Media Engine did not shut down"));
    assert(!OotPspAudioBackend_Resume(0) && !resets && liveLock && liveCompletion);
    ackMe=1;sceKernelDelayThread(100);assert(OotPspAudioBackend_Resume(0));
    reset(1);busyRelease=1000;assert(!OotPspAudioBackend_PrepareSuspend());
    assert(!OotPspAudioBackend_Resume(0) && sAudioSuspendRequested);
    busyRelease=0;assert(OotPspAudioBackend_Resume(0) && released==1 && reserves==1);
    reset(1);failDrain=1;assert(!OotPspAudioBackend_PrepareSuspend());
    assert(!strcmp(OotPspAudioBackend_GetSuspendError(),"Audio output did not drain"));
    assert(!OotPspAudioBackend_Resume(0) && sAudioSuspendRequested);
    failDrain=0;drainRequired=0;busyRelease=0;assert(OotPspAudioBackend_Resume(0));
    assert(OotPspAudioBackend_EnsureMeLockSema()==0 && !semaCreates);
    // Hardware and DSP state restart. The Allegrex completion interrupt
    // may need rearming/rebuilding, without reallocating live semaphores.
    reset(1);assert(OotPspAudioBackend_PrepareSuspend());dropInterrupt=1;
    assert(OotPspAudioBackend_Resume(0) && sAudioMeInitialized);
    assert(!sAudioMeCompletionInterruptReady && registeredHandler);
    assert(completionWaits==2 && interruptRegistrations==1);
    dropInterrupt=0;
    assert(OotPspAudioBackend_PrepareSuspend());assert(OotPspAudioBackend_Resume(0));
    assert(sAudioMeCompletionInterruptReady && registeredHandler);
    reset(1);assert(OotPspAudioBackend_PrepareSuspend());failEnable=1;
    assert(OotPspAudioBackend_Resume(0) && sAudioMeCompletionInterruptReady);
    assert(registeredHandler && resets==1);
    reset(1);assert(OotPspAudioBackend_PrepareSuspend());dropInterrupt=1;
    restoreDeliveryOnRegister=1;
    assert(OotPspAudioBackend_Resume(0) && sAudioMeCompletionInterruptReady);
    assert(interruptRegistrations==1 && registeredHandler && resets==1);
    // A failed probe or READY check keeps both workers parked. Retry holds
    // the ME in reset before clearing transport and rebooting, so the old
    // writer cannot race the new command. Neither path builds audio on CPU.
    for(int failure=1;failure<=3;failure++) {
        reset(1);assert(OotPspAudioBackend_PrepareSuspend());probeFailure=failure;
        assert(!OotPspAudioBackend_Resume(0) && !sAudioMeInitialized);
        assert(!strcmp(OotPspAudioBackend_GetSuspendError(),"Media Engine command did not finish"));
        assert(sAudioSuspendRequested && sAudioProducerParked && sAudioOutputParked && resets==1);
        probeFailure=0;assert(OotPspAudioBackend_Resume(0));
        assert(resets==2 && holds==2 && sAudioMeInitialized && !cpuBuilds && !bridgeLoads);
    }
    for(int failure=0;failure<4;failure++) {
        reset(1);assert(OotPspAudioBackend_PrepareSuspend());
        failReady=failure==0;failReset=failure==1;witnessTable=failure==2 ? 1 : 2;failEntry=failure==3;
        assert(!OotPspAudioBackend_Resume(0) && !sAudioMeInitialized);
        assert(sAudioSuspendRequested && sAudioProducerParked && sAudioOutputParked && !cpuBuilds);
        if(failure==0) assert(!strcmp(sAudioSuspendError,"ME mixer stalled p=40 s=0"));
        if(failure==2) assert(!strcmp(sAudioSuspendError,"Media Engine boot rejected"));
        if(failure==3) assert(!strcmp(sAudioSuspendError,"ME setup stalled p=0 s=0"));
        failEntry=failReady=0;witnessTable=2;assert(OotPspAudioBackend_Resume(0));
        assert(sAudioMeInitialized && !cpuBuilds && !bridgeLoads);
    }
    // Roll back a cancelled suspend by the same boot path; no firmware wake
    // callback is required for the worker to become usable again.
    reset(1);initialized=0;sAudioProducerThreadRunning=sAudioOutputThreadRunning=0;
    assert(OotPspAudioBackend_PrepareSuspend());assert(OotPspAudioBackend_Resume(0));
    assert(resets==1 && !reserves);
    // A missing ME firmware callback leaves hardware and both workers
    // untouched for retry; an acknowledgement from the previous cycle is
    // insufficient. Counter wrap and timer wrap are valid completions.
    reset(1);firmwareWakeGeneration=7;
    assert(OotPspAudioBackend_PrepareSuspend());requireWakeHook=1;
    assert(!OotPspAudioBackend_Resume(1));
    assert(!strcmp(sAudioSuspendError,"Media Engine wake not finished"));
    assert(!holds && !reserves && sAudioProducerParked && sAudioOutputParked);
    finishWakeHook();assert(OotPspAudioBackend_Resume(1) && holds==1 && resets==1);
    reset(1);firmwareWakeGeneration=UINT32_MAX;now=UINT32_MAX-500000;
    assert(OotPspAudioBackend_PrepareSuspend());
    wakeHookAt=now+676000;wakeHookPending=requireWakeHook=1;
    assert(OotPspAudioBackend_Resume(1));
    assert(!firmwareWakeGeneration && !wakeHookPending && holds==1 && resets==1);
    // A 4.5 ms job must not require dozens of scheduler wakeups if firmware
    // cannot deliver completion interrupts. IRQ completion still wakes once.
    for(int irq=0;irq<=1;irq++) {
        reset(1);testingCommandWait=1;commandCompletesAt=4500;
        sAudioMeCompletionInterruptReady=irq;sAudioMeCommandPending=1;
        sAudioMeState=OOT_PSP_AUDIO_ME_STATE_RUN;
        OotPspAudioBackend_WaitForCommandsLocked();
        assert(!sAudioMeCommandPending && sAudioMeInitialized && !fallbackCalls);
        assert(now>=4500 && now<=5500 && waitPolls<=6);
        assert(irq ? completionWaits==1 && !waitPolls : completionWaits==0);
    }
    // Delivery can disappear AFTER a successful resume probe. The first
    // real job must not sleep for 250 ms or fall back to CPU mixing, and a
    // semaphore error must not busy-loop until the command timeout.
    for(int brokenSema=0;brokenSema<=1;brokenSema++) {
        reset(1);assert(OotPspAudioBackend_PrepareSuspend());
        assert(OotPspAudioBackend_Resume(0) && sAudioMeCompletionInterruptReady);
        now=0;completionWaits=0;testingCommandWait=1;commandCompletesAt=4500;
        dropInterrupt=!brokenSema;failCommandWait=brokenSema;
        sAudioMeCommandPending=1;sAudioMeState=OOT_PSP_AUDIO_ME_STATE_SYNTH_RUN;
        OotPspAudioBackend_WaitForCommandsLocked();
        assert(!sAudioMeCommandPending && sAudioMeInitialized && !fallbackCalls && !timeoutInterrupts);
        assert(now>=4500 && now<=10000 && completionWaits==1 && waitPolls<=5);
        assert(!sAudioMeCompletionInterruptReady && !sAudioMeCompletionInterruptEnabled);
        // Subsequent commands yield without touching the failed semaphore.
        now=0;waitPolls=0;sAudioMeCommandPending=1;sAudioMeState=OOT_PSP_AUDIO_ME_STATE_RUN;
        OotPspAudioBackend_WaitForCommandsLocked();
        assert(now==5000 && waitPolls==5 && completionWaits==1 && !fallbackCalls);
    }
    // A slow healthy job can exceed the watchdog interval while retaining
    // immediate interrupt completion and without enabling polling forever.
    reset(1);testingCommandWait=1;commandCompletesAt=24500;
    sAudioMeCommandPending=1;sAudioMeState=OOT_PSP_AUDIO_ME_STATE_RUN;
    OotPspAudioBackend_WaitForCommandsLocked();
    assert(now==24500 && completionWaits==3 && !waitPolls && !fallbackCalls);
    assert(sAudioMeCompletionInterruptReady && sAudioMeCompletionInterruptEnabled);
    reset(1);testingCommandWait=1;commandCompletesAt=UINT32_MAX;
    sAudioMeCompletionInterruptReady=0;sAudioMeCommandPending=1;
    sAudioMeState=OOT_PSP_AUDIO_ME_STATE_RUN;
    OotPspAudioBackend_WaitForCommandsLocked();
    assert(fallbackCalls==1 && timeoutInterrupts==1 && !sAudioMeCommandPending);
    assert(now==260000 && waitPolls<=260);
    // A genuinely hung ME still reaches its bounded recovery/fallback path.
    // Also exercise an immediate semaphore error during the recovery wait.
    for(int brokenRecovery=0;brokenRecovery<=1;brokenRecovery++) {
        reset(1);testingCommandWait=1;commandCompletesAt=UINT32_MAX;
        failCommandWaitAfter=brokenRecovery ? 25 : 0;
        sAudioMeCommandPending=1;sAudioMeState=OOT_PSP_AUDIO_ME_STATE_RUN;
        OotPspAudioBackend_WaitForCommandsLocked();
        assert(fallbackCalls==1 && timeoutInterrupts==1 && !sAudioMeCommandPending);
        assert(now==260000 && completionWaits==26 && waitPolls<=10);
    }
    // A real post-wake mixer can exceed total age 250 ms while continuing
    // through commands. Keep its ownership and ME offload until completion.
    for(int irq=0;irq<=1;irq++) {
        reset(1);testingCommandWait=1;commandCompletesAt=400000;commandProgressInterval=100000;
        sAudioMeCompletionInterruptReady=irq;sAudioMeCommandPending=1;
        sAudioMeState=OOT_PSP_AUDIO_ME_STATE_SYNTH_RUN;
        OotPspAudioBackend_WaitForCommandsLocked();
        assert(now==400000 && !sAudioMeCommandPending && sAudioMeInitialized);
        assert(!fallbackCalls && !timeoutInterrupts);
    }
    // Late entry into the wait does not prove a stall while the asynchronous
    // caller was descheduled. Observe the ME before abandoning offload.
    reset(1);testingCommandWait=1;commandCompletesAt=400000;now=300000;
    sAudioMeCompletionInterruptReady=0;sAudioMeCommandPending=1;
    sAudioMeState=OOT_PSP_AUDIO_ME_STATE_SYNTH_RUN;
    OotPspAudioBackend_WaitForCommandsLocked();
    assert(now==400000 && sAudioMeInitialized && !fallbackCalls && !timeoutInterrupts);
    // The absolute cap still bounds a continually progressing bad job.
    reset(1);testingCommandWait=1;commandCompletesAt=UINT32_MAX;commandProgressInterval=100000;
    sAudioMeCompletionInterruptReady=0;sAudioMeCommandPending=1;
    sAudioMeState=OOT_PSP_AUDIO_ME_STATE_SYNTH_RUN;
    OotPspAudioBackend_WaitForCommandsLocked();
    assert(now==2010000 && fallbackCalls==1 && timeoutInterrupts==1);
    // The production worker helper leaves no stale acknowledgement behind.
    reset(1);sAudioSuspendRequested=0;
    assert(!OotPspAudioBackend_ParkForSuspend(&sAudioProducerParked,true));
    assert(!sAudioProducerParked);
    sAudioSuspendRequested=1;autoUnpark=&sAudioProducerParked;
    assert(OotPspAudioBackend_ParkForSuspend(&sAudioProducerParked,true));
    assert(!sAudioProducerParked && waits==1);
    sAudioSuspendRequested=1;autoUnpark=&sAudioOutputParked;
    assert(OotPspAudioBackend_ParkForSuspend(&sAudioOutputParked,false));
    assert(!sAudioOutputParked && waits==1);
    puts("Suspend audio: early-entry guard, 40 cold boots, READY/probe/reset retries, retained IPC, ME synthesis submissions and watchdogs passed");
}
'''

cache_stubs = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef int s32;
typedef unsigned u32;
typedef unsigned char u8;
typedef int SceUID;
#define PSP_SMEM_Low 0
#define OOT_PSP_CACHE_ALLOCATION_COUNT 16
#define OOT_PSP_PACKED_CACHE_TARGET_SIZE (4*1024*1024U)
#define OOT_PSP_PACKED_CACHE_MIN_SIZE (1024*1024U)
#define OOT_PSP_PACKED_CACHE_ALLOC_STEP (1024*1024U)
#define OOT_PSP_PACKED_CACHE_BLOCK_SIZE 65536
''' + allocation + r'''
static OotPspCacheAllocation sOotPspCacheAllocations[16];
static size_t sOotPspVolatileCacheAllocationCount, sOotPspVolatileCacheBytes, sOotPspVolatileMemorySize;
static int sOotPspVolatileMemoryLocked, sOotPspVolatileMemoryFailureReported;
static int sOotPspPackedCacheInitTried;
static size_t sOotPspPackedCacheBlockCount;
static u8* sOotPspPackedCacheData;
static u8 normal[4*1024*1024], volatileRam[4*1024*1024];
static size_t normalLimit, volatileLimit;
static int model, normalCalls, normalFrees, volatileCalls, volatileFrees, lockCalls, liveBlock;
static int OotPspDve_GetModel(void) {return model;}
static void* cacheMalloc(size_t bytes) {normalCalls++;return bytes<=normalLimit ? normal : NULL;}
static void cacheFree(void* p) {assert(p==normal);normalFrees++;}
static int sceKernelVolatileMemLock(int type,void** address,int* bytes) {
    assert(type==0 && model==0);lockCalls++;*address=volatileRam;*bytes=sizeof(volatileRam);return 0;
}
static int sceKernelVolatileMemUnlock(int type) {assert(type==0 && !liveBlock);return 0;}
static int sceKernelAllocPartitionMemory(int part,const char* name,int type,size_t bytes,void* address) {
    (void)name;assert(part==5 && type==PSP_SMEM_Low && address==NULL && model==0);
    assert(!liveBlock && sOotPspVolatileMemoryLocked);volatileCalls++;
    if(bytes>volatileLimit) return -1;
    liveBlock=7;return 7;
}
static void* sceKernelGetBlockHeadAddr(int id) {assert(id==liveBlock && id==7);return volatileRam;}
static int sceKernelFreePartitionMemory(int id) {assert(id==liveBlock && id==7);liveBlock=0;volatileFrees++;return 0;}
static void OotPsp_ClearPackedAssetCache(void) {}
#define malloc cacheMalloc
#define free cacheFree
'''
cache_functions = ''.join(function(asset_source, name) for name in (
    'OotPsp_UnlockUnusedVolatileMemory', 'OotPsp_LockVolatileMemory', 'OotPsp_FindFreeCacheAllocation',
    'OotPsp_AllocVolatileCacheMemory', 'OotPsp_AllocCacheMemory', 'OotPsp_FreeCacheMemory',
    'OotPsp_InitPackedAssetCache'))
cache_tests = r'''
static void reset(int hardware) {
    assert(!liveBlock && !sOotPspVolatileCacheAllocationCount);
    memset(sOotPspCacheAllocations,0,sizeof(sOotPspCacheAllocations));
    model=hardware;normalLimit=volatileLimit=sizeof(normal);
    normalCalls=normalFrees=volatileCalls=volatileFrees=lockCalls=0;
    sOotPspPackedCacheInitTried=0;sOotPspPackedCacheData=NULL;sOotPspPackedCacheBlockCount=0;
}
int main(void) {
    int models[]={-1,1,2,3,4,10};
    for(unsigned i=0;i<sizeof(models)/sizeof(models[0]);i++) {
        reset(models[i]);assert(OotPsp_AllocCacheMemory(1024)==normal);
        OotPsp_FreeCacheMemory(normal);assert(normalFrees==1 && !volatileCalls);
        normalLimit=0;assert(!OotPsp_AllocCacheMemory(1024));assert(!lockCalls && !volatileCalls);
        reset(models[i]);OotPsp_InitPackedAssetCache();
        assert(sOotPspPackedCacheData==normal && sOotPspPackedCacheBlockCount==64 && !lockCalls);
        OotPsp_FreeCacheMemory(normal);assert(normalFrees==1);
        reset(models[i]);normalLimit=2*1024*1024;OotPsp_InitPackedAssetCache();
        assert(normalCalls==3 && sOotPspPackedCacheBlockCount==32 && !lockCalls);
        OotPsp_FreeCacheMemory(normal);
        reset(models[i]);normalLimit=0;OotPsp_InitPackedAssetCache();
        assert(!sOotPspPackedCacheData && !volatileCalls && !lockCalls);
    }
    // The Phat keeps normal-heap preference for pins and volatile fallback.
    reset(0);assert(OotPsp_AllocCacheMemory(1024)==normal);
    OotPsp_FreeCacheMemory(normal);assert(!lockCalls);
    normalLimit=0;assert(OotPsp_AllocCacheMemory(1024)==volatileRam);
    assert(lockCalls==1 && sOotPspVolatileCacheBytes==1024);
    OotPsp_FreeCacheMemory(volatileRam);assert(volatileFrees==1 && !sOotPspVolatileMemoryLocked);
    reset(0);OotPsp_InitPackedAssetCache();
    assert(sOotPspPackedCacheData==volatileRam && !normalCalls);
    OotPsp_FreeCacheMemory(volatileRam);
    reset(0);volatileLimit=2*1024*1024;OotPsp_InitPackedAssetCache();
    assert(sOotPspPackedCacheData==volatileRam && sOotPspPackedCacheBlockCount==32 && volatileCalls==3);
    OotPsp_FreeCacheMemory(volatileRam);
    puts("Cache memory: later/unknown models never use volatile RAM, heap-pressure fallback, Phat placement/free passed");
}
'''

hook_source = (PORT / 'oot_psp_me_suspend.cpp').read_text()
hook_source = re.sub(r'^#include .*\n', '', hook_source, flags=re.M)
hook_stubs = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
#define UNCACHED_USER_MASK 0
#define meLibSync() ((void)0)
static u32 SC_HW_RESET=0x14;
static int resetWrites, bridgeLoads;
struct ResetRegister {
    u32 value;
    ResetRegister& operator=(u32 next) {
        // The firmware hooks must never release reset. Only the explicit
        // application boot may start a worker after hook completion.
        assert(next==SC_HW_RESET);value=next;resetWrites++;return *this;
    }
};
static ResetRegister HW_SYS_RESET_ENABLE;
static int meLibDefaultInit(void) {bridgeLoads++;return 2;}
'''
hook_tests = r'''
int main(void) {
    assert(OotPspMeSuspend_Init()==2 && bridgeLoads==1);
    for(u32 cycle=0;cycle<40;cycle++) {
        assert(OotPspMeSuspend_WakeGeneration()==cycle);
        meLibOnSleep();
        assert(OotPspMeSuspend_WakeGeneration()==cycle);
        assert(HW_SYS_RESET_ENABLE.value==0x14);
        meLibOnWake();
        assert(OotPspMeSuspend_WakeGeneration()==cycle+1);
        assert(HW_SYS_RESET_ENABLE.value==0x14);
    }
    assert(resetWrites==80 && bridgeLoads==1);
    puts("ME firmware hooks: 40 generations, reset held on sleep/wake, bridge loaded once passed");
}
'''

with tempfile.TemporaryDirectory(prefix='oot-suspend-') as temp:
    directory = Path(temp)
    run(directory, 'assets', asset_stubs + asset_functions + asset_tests)
    run(directory, 'coordinator', coordinator_stubs + coordinator + coordinator_tests)
    run(directory, 'audio', audio_stubs + function(audio_source, 'OotPspAudioBackend_RunMe') +
        function(audio_source, 'OotPspAudioBackend_EnsureMeLockSema') +
        function(audio_source, 'OotPspAudioBackend_MeCompletionInterrupt') +
        function(audio_source, 'OotPspAudioBackend_EnsureMeCompletionInterrupt') +
        function(audio_source, 'OotPspAudioBackend_RebuildMeCompletionInterrupt') +
        audio_functions + audio_tests)
    run(directory, 'cache_memory', cache_stubs + cache_functions + cache_tests)
    hook_path = directory / 'me_firmware_hook.cpp'
    hook_binary = directory / 'me_firmware_hook'
    hook_path.write_text(hook_stubs + hook_source + hook_tests)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++11', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(hook_path), '-o', str(hook_binary)], check=True)
    subprocess.run([str(hook_binary)], check=True, timeout=15)
