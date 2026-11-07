#!/usr/bin/env python3
"""Exercise wake measurements, excluding sleep, using the production logger."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'src/port/psp/oot_psp_wake_trace.c').read_text()
source = re.sub(r'^#include .*\n', '', source, flags=re.M)
header = (ROOT / 'src/port/psp/oot_psp_audio_backend.h').read_text()
audio_status = re.search(r'typedef struct OotPspAudioWakeStatus \{.*?\} OotPspAudioWakeStatus;',
                         header, re.S).group()
stubs = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef int32_t s32;
typedef int SceUID;
typedef struct {u32 low, hi;} SceKernelSysClock;
typedef struct {
    unsigned size; SceKernelSysClock idleClocks;
    u32 comesOutOfIdleCount, threadSwitchCount, vfpuSwitchCount;
} SceKernelSystemStatus;
typedef struct {
    unsigned size; char name[32]; SceKernelSysClock runClocks;
    u32 intrPreemptCount, releaseCount; int currentPriority;
} SceKernelThreadInfo;
#define OOT_PSP_WAKE_DIAGNOSTICS 1
#define SCE_KERNEL_TMID_Thread 1
#define PSP_O_WRONLY 1
#define PSP_O_CREAT 2
#define PSP_O_APPEND 4
#define PSP_O_TRUNC 8
#define PSP_POWER_CB_SUSPENDING 0x10000
#define PSP_POWER_CB_RESUMING 0x20000
#define PSP_POWER_CB_RESUME_COMPLETE 0x40000
''' + audio_status + r'''
static u64 now, idle, runs[4];
static int woke, openFd, opens, closes, writeCalls, failOpen, failWrite, badCounters;
static u32 presentations;
static char output[65536];
static unsigned outputLength;
static SceKernelSysClock clockValue(u64 value) {
    return (SceKernelSysClock){(u32)value, (u32)(value>>32)};
}
static u64 sceKernelGetSystemTimeWide(void) {return now;}
static u32 sceKernelGetSystemTimeLow(void) {return (u32)now;}
static int scePowerGetCpuClockFrequencyInt(void) {return woke ? 266 : 333;}
static int scePowerGetBusClockFrequencyInt(void) {return woke ? 133 : 166;}
static u32 OotPspVideoThread_GetCompletedFrames(void) {return presentations;}
static void OotPspAudioBackend_GetWakeStatus(OotPspAudioWakeStatus* status) {
    memset(status, 0, sizeof(*status));status->meActive=1;status->interruptReady=!woke;
    status->interruptCount=(u32)(runs[1]/100);status->hardwareSrc=1;
    status->meStackPointer=0x88CFFF00;
    const s32 inputs[5]={2,24,1,384,1};
    memcpy(status->cpuSynthInputs,inputs,sizeof(inputs));
    memcpy(status->meSynthInputs,inputs,sizeof(inputs));
    status->hardware[1]=woke ? 0x002001ff : 0x01ff01ff;
    if(woke) {
        status->fallbackCount=1;status->fallbackProgress=0x80000f02U;
        status->fallbackHardware[1]=0x002001ff;
        status->fallbackWord0=0x02000000;status->fallbackWord1=0x2e0;status->fallbackWordsValid=1;
    }
}
static int sceKernelReferSystemStatus(SceKernelSystemStatus* status) {
    assert(status->size==sizeof(*status));
    status->idleClocks=clockValue(badCounters ? UINT64_MAX : idle);
    return 0;
}
static int sceKernelGetThreadmanIdList(int type,SceUID* ids,int capacity,int* count) {
    assert(type==1 && capacity==128);*count=4;
    for(int i=0;i<4;i++) ids[i]=i+1;
    return 0;
}
static int sceKernelReferThreadStatus(SceUID id,SceKernelThreadInfo* info) {
    assert(id>=1 && id<=4 && info->size==sizeof(*info));
    // An inaccessible firmware thread must be reported as missing, not idle.
    if(id==4) return -1;
    const char* names[]={"OOT main", "OOT PSP Audio", "OOT PSP Video"};
    strcpy(info->name,names[id-1]);info->runClocks=clockValue(runs[id-1]);
    info->currentPriority=32-id;return 0;
}
static const char* OotPsp_ResolveRootPath(const char* path,char* buffer,unsigned size) {
    (void)buffer;(void)size;assert(!strcmp(path,"oot-psp-wake.log"));return path;
}
static int sceIoOpen(const char* path,int flags,int mode) {
    assert(!strcmp(path,"oot-psp-wake.log") && mode==0777 && !openFd);
    opens++;if(failOpen) return -1;
    if(flags&PSP_O_TRUNC) outputLength=0;
    else assert(flags&PSP_O_APPEND);
    openFd=1;return 7;
}
static int sceIoWrite(int fd,const char* data,int size) {
    assert(fd==7 && openFd);writeCalls++;
    if(failWrite) return 0;
    // Production must retain the entire report on a short write.
    if(size>23) size=23;
    assert(outputLength+size<sizeof(output));
    memcpy(output+outputLength,data,size);outputLength+=size;output[outputLength]=0;
    return size;
}
static int sceIoClose(int fd) {assert(fd==7 && openFd);openFd=0;closes++;return 0;}
static void advance(u32 usec) {
    now+=usec;idle+=(u64)usec*(woke ? 45 : 65)/100;
    runs[0]+=(u64)usec*(woke ? 40 : 20)/100;
    runs[1]+=(u64)usec*5/100;runs[2]+=(u64)usec*10/100;
    presentations+=(u64)usec*(woke ? 16 : 20)/1000000;
}
'''
tests = r'''
static int occurrences(const char* needle) {
    int count=0;const char* p=output;
    while((p=strstr(p,needle))) {count++;p+=strlen(needle);}return count;
}
int main(void) {
    // No per-frame enumeration or file I/O: only one snapshot per 3 seconds.
    OotPspWakeTrace_Poll();
    for(int i=0;i<10;i++) {advance(1000000);OotPspWakeTrace_Poll();}
    assert(sHaveBaseline && !opens);
    // Persist a closed file before firmware suspend, even if wake crashes
    // before the first three-second sample can be collected.
    OotPspWakeTrace_Checkpoint("before_sleep");
    assert(opens==1 && closes==1 && !openFd && !sPostSamples);
    assert(strstr(output,"OOT PSP wake trace v8"));
    assert(strstr(output,"recovery cycle=1 stage=before_sleep"));
    OotPspWakeTrace_NotifyPowerEvent(PSP_POWER_CB_SUSPENDING);
    now+=3000000000ULL; // Sleep exceeds the signed timer comparison window.
    OotPspWakeTrace_NotifyPowerEvent(PSP_POWER_CB_RESUMING);
    woke=1;OotPspWakeTrace_Resume();
    OotPspWakeTrace_NotifyPowerEvent(PSP_POWER_CB_RESUME_COMPLETE);
    for(int i=0;i<3;i++) {advance(3000000);OotPspWakeTrace_Poll();}
    assert(opens==4 && closes==4 && !openFd && !sPostSamples);
    assert(occurrences("phase=before_sleep")==1 && occurrences("phase=wake_")==3);
    assert(strstr(output,"phase=before_sleep wall_us=3000000 cpu_busy_tenths_pct=350 valid=1"));
    assert(strstr(output,"phase=wake_0_3s wall_us=3000000 cpu_busy_tenths_pct=550 valid=1"));
    assert(strstr(output,"cpu_mhz=333->333 bus_mhz=166->166"));
    assert(strstr(output,"cpu_mhz=266->266 bus_mhz=133->133"));
    assert(strstr(output,"threads=3/4"));
    assert(strstr(output,"video presentations=60 fps_tenths=200"));
    assert(strstr(output,"video presentations=48 fps_tenths=160"));
    assert(strstr(output,"name=\"OOT main\" cpu_us=1200000 cpu_tenths_pct=400"));
    assert(occurrences("wall_us=3000000")==4);
    assert(strstr(output,"hardware_start result=0 cpu_div=0x00000000 me_div=0x01FF01FF"));
    assert(strstr(output,"hardware_end result=0 cpu_div=0x00000000 me_div=0x002001FF"));
    assert(strstr(output,"fallback_mixer command_index=15 opcode=2"));
    assert(strstr(output,"word0=0x02000000 word1=0x000002E0 valid=1"));
    assert(strstr(output,"power_flags=0x00030000->0x00070000"));
    assert(strstr(output,"me_stack sp=0x88CFFF00->0x88CFFF00"));
    assert(strstr(output,"cpu=2/24/1/384/1 me=2/24/1/384/1"));
    // Do not retain the previous cycle's cold baseline for a quick re-sleep.
    OotPspWakeTrace_Resume();advance(3000000);OotPspWakeTrace_Poll();
    assert(strstr(output,"No baseline:"));
    // Counter failures must be explicit, without unsigned utilization spikes.
    badCounters=1;advance(3000000);OotPspWakeTrace_Poll();
    assert(strstr(output,"cpu_busy_tenths_pct=-1 valid=0"));
    // File errors cannot stall gameplay or leave a descriptor open.
    failOpen=1;advance(3000000);OotPspWakeTrace_Poll();assert(!openFd);
    failOpen=0;failWrite=1;OotPspWakeTrace_Resume();
    int previous=writeCalls;advance(3000000);OotPspWakeTrace_Poll();
    assert(!openFd && writeCalls>previous && writeCalls-previous<32);
    puts("Wake trace: per-thread CPU/FPS, before/after windows, long sleep, missing threads, invalid counters and I/O failures passed");
}
'''
with tempfile.TemporaryDirectory(prefix='oot-wake-trace-') as temp:
    path = Path(temp) / 'test.c'
    path.write_text(stubs + source + tests)
    binary = Path(temp) / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=15)
