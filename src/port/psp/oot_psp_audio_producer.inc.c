/* Included by the backend: shares its ME FIFO and PCM ring, not a new ME
 * bootstrap or a PSP thread. Allegrex is only the command/IO endpoint. */
#include "oot_psp_audio_producer.h"
#include "oot_psp_audio_transport.h"
#include "audiothread_cmd.h"

typedef struct {
    u32 valid, enabled, priority;
    s8 io[8];
} OotAudioChannelStatus;
typedef struct {
    u32 enabled, tempo;
    s8 io[8];
    OotAudioChannelStatus channels[16];
} OotAudioPlayerStatus;
typedef struct {
    volatile u32 ready;
    u32 serial, ticks, random, reset, spec;
    AudioBufferParameters parameters;
    OotAudioPlayerStatus players[4];
    s32 remaining[4][16][4];
    s32 notes, ramNotes;
} OotAudioStatus;
typedef struct {
    volatile u32 active, stop, inStep, rpcState;
    volatile u32 rpcOp, args[5], result, loadsPending, ticks, errors;
    u32 pad[2];
} OotAudioProducerBus;
static OotAudioProducerBus sProducerBus __attribute__((aligned(64), section(".uncached")));
static OotPspAudioTransport sProducerCommands __attribute__((aligned(64), section(".uncached")));
static OotAudioStatus sProducerStatus[3] __attribute__((aligned(64), section(".uncached")));
#define AUTO_BUS ((volatile OotAudioProducerBus*)((uintptr_t)&sProducerBus | 0x40000000u))
#define AUTO_COMMANDS ((OotPspAudioTransport*)((uintptr_t)&sProducerCommands | 0x40000000u))
#define AUTO_STATUS ((OotAudioStatus*)((uintptr_t)&sProducerStatus | 0x40000000u))

/* These are main-only; the ME never reads or writes their cache lines. */
static AudioContext sProducerGameContext __attribute__((aligned(64)));
static SequenceChannel sProducerGameChannels[4][16] __attribute__((aligned(64)));
static struct {
    s32 servicing, pumping;
    s32 liveReported;
    u32 observedTicks, lastProgressUsec, stallReports;
    u32 serial;
    s32 remaining[4][16][4];
    s32 notes, ramNotes;
    OotPspAudioMessage batch[256];
} sProducerMain __attribute__((aligned(64)));

void AudioThread_ProcessCmd(AudioCmd* cmd);
void AudioThread_ProcessCmds(u32 msg);
void AudioThread_InitMesgQueuesImpl(void);
s32 func_800E6590(s32 player, s32 channel, s32 layer);
s32 func_800E66C0(s32 flags);
static void OotPspAudioProducer_MeStep(void);

s32 OotPspAudioProducer_Active(void) {
    return AUTO_BUS->active != 0;
}

AudioContext* OotPspAudioProducer_GameContext(void) {
    if (OotPspAudioProducer_IsMe() || !AUTO_BUS->active || sProducerMain.servicing) {
        return &gAudioCtx;
    }
    return &sProducerGameContext;
}

/* Only the ME calls this. Ownership moves to main after dirty engine state
 * is published, and returns after main publishes its changes. While waiting
 * the ME continues draining already committed PCM. No kernel calls here. */
u32 OotPspAudioProducer_Request(u32 op, u32 a, u32 b, u32 c, u32 d, u32 e) {
    meLibDcacheWritebackInvalidateAll();
    AUTO_BUS->rpcOp = op;
    AUTO_BUS->args[0] = a;
    AUTO_BUS->args[1] = b;
    AUTO_BUS->args[2] = c;
    AUTO_BUS->args[3] = d;
    AUTO_BUS->args[4] = e;
    meLibSync();
    AUTO_BUS->rpcState = 1;
    meLibSync();
    while (AUTO_BUS->rpcState == 1) {
        OotPspAudioBackend_ServiceOutputMe();
        meLibDelayPipeline();
    }
    /* Only FIFO/ring-local data was touched during the wait. Preserve it
     * while discarding old engine/asset cache lines before resuming. */
    meLibDcacheWritebackInvalidateAll();
    meLibSync();
    AUTO_BUS->rpcState = 0;
    return AUTO_BUS->result;
}

static void OotPspAudioProducer_ServiceRequest(void) {
    u32 a, b, c, d, e, result = 0;
    AudioCmd command;
    OSMesg ignored;
    if (AUTO_BUS->rpcState != 1) {
        return;
    }
    /* No main-side code mutates live engine data during autonomous playback.
     * This is the sole ownership transfer, including loader-private globals. */
    sceKernelDcacheWritebackInvalidateAll();
    sProducerMain.servicing = true;
    a = AUTO_BUS->args[0]; b = AUTO_BUS->args[1]; c = AUTO_BUS->args[2];
    d = AUTO_BUS->args[3]; e = AUTO_BUS->args[4];
    switch (AUTO_BUS->rpcOp) {
        case OOT_AUDIO_RPC_COMMAND:
            command.opArgs = a;
            command.asUInt = b;
            AudioThread_ProcessCmd(&command);
            break;
        case OOT_AUDIO_RPC_MAINTENANCE:
            while (osRecvMesg(&gAudioCtx.curAudioFrameDmaQueue, &ignored, OS_MESG_NOBLOCK) == 0) {}
            gAudioCtx.curAudioFrameDmaCount = 0;
            AudioLoad_ProcessLoads(gAudioCtx.resetStatus);
            AudioLoad_ProcessScriptLoads();
            {
                u32 wasReset = gAudioCtx.resetStatus;
                result = wasReset == 0 || AudioHeap_ResetStep();
                if (wasReset && !gAudioCtx.resetStatus) {
                    sProducerGameContext.specId = gAudioCtx.specId;
                    osSendMesg(sProducerGameContext.audioResetQueueP,
                               (OSMesg)(u32)gAudioCtx.specId, OS_MESG_NOBLOCK);
                }
            }
            if (gAudioCustomUpdateFunction != NULL) {
                gAudioCustomUpdateFunction();
            }
            break;
        case OOT_AUDIO_RPC_SAMPLE_DMA:
            result = (u32)(uintptr_t)AudioLoad_DmaSampleData(a, b, c, (u8*)(uintptr_t)d, e);
            break;
        case OOT_AUDIO_RPC_SLOW_SAMPLE:
            result = AudioLoad_SlowLoadSample(a, b, (s8*)(uintptr_t)c);
            break;
        case OOT_AUDIO_RPC_SLOW_SEQ:
            result = AudioLoad_SlowLoadSeq(a, (u8*)(uintptr_t)b, (s8*)(uintptr_t)c);
            break;
        case OOT_AUDIO_RPC_SCRIPT_LOAD:
            AudioLoad_ScriptLoad(a, b, (s8*)(uintptr_t)c);
            break;
        case OOT_AUDIO_RPC_INIT_SEQ:
            /* Original loader has no defined return on success. */
            AudioLoad_SyncInitSeqPlayer(a, b, c);
            break;
        default:
            AUTO_BUS->errors++;
            break;
    }
    while (MQ_GET_COUNT(&sProducerGameContext.externalLoadQueue) < 16 &&
           osRecvMesg(&gAudioCtx.externalLoadQueue, &ignored, OS_MESG_NOBLOCK) == 0) {
        osSendMesg(&sProducerGameContext.externalLoadQueue, ignored, OS_MESG_NOBLOCK);
    }
    AUTO_BUS->loadsPending = AudioLoad_HasPendingLoads() || MQ_GET_COUNT(&gAudioCtx.externalLoadQueue) != 0;
    sceKernelDcacheWritebackInvalidateAll();
    sProducerMain.servicing = false;
    AUTO_BUS->result = result;
    meLibSync();
    AUTO_BUS->rpcState = 2;
    meLibSync();
}

static void OotPspAudioProducer_PublishStatus(void) {
    OotAudioStatus* status = NULL;
    s32 i, p, c, l;
    for (i = 0; i < 3; i++) {
        if (__atomic_load_n(&AUTO_STATUS[i].ready, __ATOMIC_ACQUIRE) == 0) {
            status = &AUTO_STATUS[i];
            break;
        }
    }
    if (status == NULL) {
        return; /* Never hold up sound because the game has not read status. */
    }
    status->serial = AUTO_BUS->ticks;
    status->ticks = gAudioCtx.totalTaskCount;
    status->random = gAudioCtx.audioRandom;
    status->reset = gAudioCtx.resetStatus;
    status->spec = gAudioCtx.specId;
    status->parameters = gAudioCtx.audioBufferParameters;
    status->notes = func_800E66C0(0);
    status->ramNotes = func_800E66C0(2);
    for (p = 0; p < 4; p++) {
        SequencePlayer* player = &gAudioCtx.seqPlayers[p];
        status->players[p].enabled = player->enabled;
        status->players[p].tempo = player->tempo;
        memcpy(status->players[p].io, player->seqScriptIO, 8);
        for (c = 0; c < 16; c++) {
            SequenceChannel* channel = player->channels[c];
            OotAudioChannelStatus* dst = &status->players[p].channels[c];
            dst->valid = channel != NULL && channel != &gAudioCtx.sequenceChannelNone;
            dst->enabled = dst->valid && channel->enabled;
            dst->priority = dst->valid ? channel->notePriority : 0;
            for (l = 0; l < 8; l++) {
                dst->io[l] = dst->valid ? channel->seqScriptIO[l] : SEQ_IO_VAL_NONE;
            }
            for (l = 0; l < 4; l++) {
                status->remaining[p][c][l] = dst->valid ? func_800E6590(p, c, l) : 0;
            }
            OotPspAudioBackend_ServiceOutputMe();
        }
    }
    __atomic_store_n(&status->ready, 1u, __ATOMIC_RELEASE);
}

void OotPspAudioProducer_Pump(void) {
    s32 i, p, c;
    if (!AUTO_BUS->active || sProducerMain.pumping) {
        return;
    }
    sProducerMain.pumping = true;
    OotPspAudioProducer_ServiceRequest();
    for (i = 0; i < 3; i++) {
        OotAudioStatus* status = &AUTO_STATUS[i];
        if (__atomic_load_n(&status->ready, __ATOMIC_ACQUIRE) == 0) {
            continue;
        }
        /* ME cannot reuse a ready slot until this reader releases it. */
        if ((s32)(status->serial - sProducerMain.serial) > 0) {
            sProducerMain.serial = status->serial;
            sProducerGameContext.totalTaskCount = status->ticks;
            sProducerGameContext.audioRandom = status->random;
            sProducerGameContext.resetStatus = status->reset;
            sProducerGameContext.specId = status->spec;
            sProducerGameContext.audioBufferParameters = status->parameters;
            sProducerMain.notes = status->notes;
            sProducerMain.ramNotes = status->ramNotes;
            memcpy(sProducerMain.remaining, status->remaining, sizeof(status->remaining));
            for (p = 0; p < 4; p++) {
                SequencePlayer* player = &sProducerGameContext.seqPlayers[p];
                player->enabled = status->players[p].enabled;
                player->tempo = status->players[p].tempo;
                memcpy(player->seqScriptIO, status->players[p].io, 8);
                for (c = 0; c < 16; c++) {
                    OotAudioChannelStatus* src = &status->players[p].channels[c];
                    SequenceChannel* dst = &sProducerGameChannels[p][c];
                    dst->enabled = src->enabled;
                    dst->notePriority = src->priority;
                    memcpy(dst->seqScriptIO, src->io, 8);
                    player->channels[c] = src->valid ? dst : &sProducerGameContext.sequenceChannelNone;
                }
            }
        }
        __atomic_store_n(&status->ready, 0u, __ATOMIC_RELEASE);
    }
    sProducerMain.pumping = false;
    {
        u32 now = sceKernelGetSystemTimeLow();
        if (!sProducerMain.lastProgressUsec || AUTO_BUS->ticks != sProducerMain.observedTicks) {
            sProducerMain.observedTicks = AUTO_BUS->ticks;
            sProducerMain.lastProgressUsec = now;
        } else if (now - sProducerMain.lastProgressUsec >= 1000000u && sProducerMain.stallReports < 4) {
            printf("[audio-me] producer stalled ticks=%lu stage=%lu rpc=%lu op=%lu arg=%08lx fifo=%lu errors=%lu\n",
                   (unsigned long)AUTO_BUS->ticks, (unsigned long)sAudioMeProgress,
                   (unsigned long)AUTO_BUS->rpcState, (unsigned long)AUTO_BUS->rpcOp,
                   (unsigned long)AUTO_BUS->args[0], (unsigned long)sAudioMeOutputWrites,
                   (unsigned long)AUTO_BUS->errors);
            sProducerMain.lastProgressUsec = now;
            sProducerMain.stallReports++;
        }
    }
    if (!sProducerMain.liveReported && AUTO_BUS->ticks >= 4) {
        sProducerMain.liveReported = true;
        printf("[audio-me] autonomous live ticks=%lu fifo-writes=%lu underruns=%lu errors=%lu\n",
               (unsigned long)AUTO_BUS->ticks, (unsigned long)sAudioMeOutputWrites,
               (unsigned long)sAudioMeOutputUnderruns, (unsigned long)AUTO_BUS->errors);
    }
}

s32 OotPspAudioProducer_SubmitBatch(void) {
    AudioContext* context = OotPspAudioProducer_GameContext();
    u32 read = context->threadCmdReadPos;
    u32 count = (u8)(context->threadCmdWritePos - read);
    u32 i;
    for (i = 0; i < count; i++) {
        AudioCmd* cmd = &context->threadCmdBuf[(read + i) & 255u];
        OotPspAudioMessage* message = &sProducerMain.batch[i];
        memset(message, 0, sizeof(*message));
        message->kind = OOT_PSP_AUDIO_MESSAGE_COMMAND;
        message->words[0] = cmd->opArgs;
        message->words[1] = cmd->asUInt;
    }
    if (!OotPspAudioTransport_Push(AUTO_COMMANDS, sProducerMain.batch, count)) {
        return -1; /* Caller retains its staged batch for a later submission. */
    }
    context->threadCmdReadPos = context->threadCmdWritePos;
    return 0;
}

s32 OotPspAudioProducer_CountNotes(s32 flags) {
    if (sProducerMain.servicing) {
        return -1;
    }
    return flags >= 2 ? sProducerMain.ramNotes : sProducerMain.notes;
}
s32 OotPspAudioProducer_SampleRemaining(s32 p, s32 c, s32 l) {
    return (u32)p < 4 && (u32)c < 16 && (u32)l < 4 ? sProducerMain.remaining[p][c][l] : 0;
}
void OotPspAudioProducer_WaitTick(void) {
    u32 tick = AUTO_BUS->ticks;
    u32 start = sceKernelGetSystemTimeLow();
    if (sProducerMain.servicing) {
        return;
    }
    while (AUTO_BUS->active && AUTO_BUS->ticks == tick && sceKernelGetSystemTimeLow() - start < 250000u) {
        OotPspAudioProducer_Pump();
        sceKernelDelayThread(100);
    }
}

static void OotPspAudioProducer_Start(void) {
    OSMesg message;
    s32 p, c;
    /* Finish startup commands on main before the permanent handover. */
    while (osRecvMesg(gAudioCtx.threadCmdProcQueueP, &message, OS_MESG_NOBLOCK) == 0) {
        AudioThread_ProcessCmds((u32)message);
    }
    memcpy(&sProducerGameContext, &gAudioCtx, sizeof(gAudioCtx));
    for (p = 0; p < 4; p++) {
        for (c = 0; c < 16; c++) {
            SequenceChannel* channel = gAudioCtx.seqPlayers[p].channels[c];
            if (channel != NULL && channel != &gAudioCtx.sequenceChannelNone) {
                sProducerGameChannels[p][c] = *channel;
                sProducerGameContext.seqPlayers[p].channels[c] = &sProducerGameChannels[p][c];
            } else {
                sProducerGameContext.seqPlayers[p].channels[c] = &sProducerGameContext.sequenceChannelNone;
            }
        }
    }
    OotPspAudioTransport_Init(AUTO_COMMANDS);
    memset((void*)AUTO_BUS, 0, sizeof(*AUTO_BUS));
    memset(AUTO_STATUS, 0, sizeof(sProducerStatus));
    AUTO_BUS->loadsPending = AudioLoad_HasPendingLoads();
    sceKernelDcacheWritebackInvalidateAll();
    /* Both the real engine and main snapshot have their own OS queues.
     * ME never calls those queues; requests are serviced on Allegrex. */
    AUTO_BUS->active = 1;
    AudioThread_InitMesgQueuesImpl();
    osCreateMesgQueue(&sProducerGameContext.externalLoadQueue,
                     sProducerGameContext.externalLoadMsgBuf, 16);
    meLibSync();
    printf("[audio-me] autonomous producer: sequence, synthesis and output on ME; main handles commands/IO\n");
}

static s32 OotPspAudioProducer_Stop(void) {
    u32 start = sceKernelGetSystemTimeLow();
    AUTO_BUS->stop = 1;
    meLibSync();
    while (AUTO_BUS->inStep || AUTO_BUS->rpcState == 1) {
        OotPspAudioProducer_Pump();
        if (sceKernelGetSystemTimeLow() - start > 250000u) {
            printf("[audio-me] producer stop timed out; retaining audio ownership\n");
            return false;
        }
        sceKernelDelayThread(100);
    }
    return true;
}

static void OotPspAudioProducer_MeStep(void) {
#if OOT_PSP_AUDIO_MEDIA_ENGINE && OOT_PSP_AUDIO_ME_DIRECT_OUTPUT
    /* Reset phases which produce no PCM still advance at an audio tick,
     * not at the speed of the polling loop. FIFO writes include silence. */
    static struct { u32 waiting, untilWrite; u8 pad[56]; } emptyTick __attribute__((aligned(64)));
    OotPspAudioMessage message;
    AudioCmd command;
    Acmd* list;
    s16* output;
    s32 count, frames, budget;
    u32 buffered;
    if (!AUTO_BUS->active || AUTO_BUS->stop) {
        return;
    }
    if (emptyTick.waiting && (s32)(sAudioMeLocalOutputWrites - emptyTick.untilWrite) < 0) {
        return;
    }
    emptyTick.waiting = false;
    AUTO_BUS->inStep = 1;
    meLibSync();
    if (AUTO_BUS->stop) {
        AUTO_BUS->inStep = 0;
        return;
    }
    if (AUTO_BUS->ticks == 0) {
        /* One-time cache refresh after the startup handover. */
        meLibDcacheWritebackInvalidateAll();
    }
    buffered = (sAudioWritePos - sAudioMeLocalReadPos) & OOT_PSP_AUDIO_RING_MASK;
    if (buffered >= sAudioSourceChunkFrames * OOT_PSP_AUDIO_TARGET_CHUNKS) {
        AUTO_BUS->inStep = 0;
        return;
    }
    /* Reset is a multi-tick barrier, not just the command that starts it.
     * Keep subsequent IO/music-start commands queued until the heap is ready.
     * Otherwise they run against the old heap and ResetStep fades/discards
     * their notes (file select queues RESET followed immediately by BGM).
     * Match AudioThread_UpdateImpl's resetStatus == 0 command gate. */
    for (budget = 0; budget < 256 && gAudioCtx.resetStatus == 0 &&
                     OotPspAudioTransport_Pop(AUTO_COMMANDS, &message); budget++) {
        if (message.kind != OOT_PSP_AUDIO_MESSAGE_COMMAND) {
            AUTO_BUS->errors++;
            continue;
        }
        command.opArgs = message.words[0];
        command.asUInt = message.words[1];
        if (command.op == AUDIOCMD_OP_GLOBAL_STOP_AUDIOCMDS) {
            break;
        }
        if ((command.op & 0x80) && command.op != AUDIOCMD_OP_GLOBAL_DISABLE_SEQPLAYER &&
            command.op != AUDIOCMD_OP_GLOBAL_SET_CHANNEL_MASK &&
            command.op != AUDIOCMD_OP_GLOBAL_SET_SOUND_OUTPUT_MODE &&
            command.op != AUDIOCMD_OP_GLOBAL_MUTE && command.op != AUDIOCMD_OP_GLOBAL_UNMUTE) {
            OotPspAudioProducer_Request(OOT_AUDIO_RPC_COMMAND, command.opArgs, command.asUInt, 0, 0, 0);
        } else {
            AudioThread_ProcessCmd(&command);
        }
        OotPspAudioBackend_ServiceOutputMe();
        if (gAudioCtx.resetStatus) {
            break;
        }
    }
    gAudioCtx.totalTaskCount += gAudioCtx.audioBufferParameters.specUnk4;
    AUTO_BUS->ticks++;
    AudioLoad_DecreaseSampleDmaTtls();
    if (AUTO_BUS->loadsPending || gAudioCtx.curAudioFrameDmaCount ||
        gAudioCtx.resetStatus || gAudioCustomUpdateFunction != NULL) {
        if (!OotPspAudioProducer_Request(OOT_AUDIO_RPC_MAINTENANCE, 0, 0, 0, 0, 0)) {
            emptyTick.waiting = true;
            emptyTick.untilWrite = sAudioMeLocalOutputWrites +
                gAudioCtx.audioBufferParameters.samplesPerFrameTarget * 2u;
            OotPspAudioProducer_PublishStatus();
            AUTO_BUS->inStep = 0;
            return;
        }
    }
    frames = gAudioCtx.audioBufferParameters.samplesPerFrameTarget;
    if (frames < gAudioCtx.audioBufferParameters.minAiBufferLength) {
        frames = gAudioCtx.audioBufferParameters.minAiBufferLength;
    }
    if (frames > gAudioCtx.audioBufferParameters.maxAiBufferLength) {
        frames = gAudioCtx.audioBufferParameters.maxAiBufferLength;
    }
    if (frames <= 0 || (u32)frames >= OOT_PSP_AUDIO_RING_FRAMES - buffered) {
        AUTO_BUS->errors++;
        AUTO_BUS->inStep = 0;
        return;
    }
    gAudioCtx.rspTaskIndex ^= 1;
    gAudioCtx.curAiBufIndex = (gAudioCtx.curAiBufIndex + 1) % 3;
    list = gAudioCtx.abiCmdBufs[gAudioCtx.rspTaskIndex];
    output = gAudioCtx.aiBuffers[gAudioCtx.curAiBufIndex];
    gAudioCtx.aiBufLengths[gAudioCtx.curAiBufIndex] = frames;
    sAudioMeProgress = 7;
    AudioSynth_ProcessSequenceControl();
    sAudioMeProgress = 2;
    AudioSynth_BuildCommandListMe(list, &count, output, frames);
    if (count <= 0 || count > gAudioCtx.maxAudioCmds) {
        AUTO_BUS->errors++;
        AUTO_BUS->inStep = 0;
        return;
    }
    /* The ME owns all mutable inputs/outputs. Do not invalidate its own
     * freshly built commands or mixer history as the old job path did. */
    sAudioMeProgress = 4;
    OotPspMixer_ExecuteCommandListMe(list, count, OOT_PSP_AUDIO_ME_OPCODE_PROFILE);
    sAudioMeProgress = 5;
    sAudioMeQueueSrc = (u32)(uintptr_t)output;
    sAudioMeQueueFrames = frames;
    sAudioMeQueueWritePos = sAudioWritePos;
    OotPspAudioBackend_MeQueueBuffer(false);
    gAudioCtx.audioRandom = gAudioCtx.audioRandom * 1664525u + 1013904223u;
    gWaveSamples[8] = (s16*)((u8*)AudioThread_Update + (gAudioCtx.audioRandom & 0xfff0));
    sAudioMeProgress = 8;
    OotPspAudioProducer_PublishStatus();
    sAudioMeProgress = 6;
    meLibSync();
    AUTO_BUS->inStep = 0;
#endif
}
