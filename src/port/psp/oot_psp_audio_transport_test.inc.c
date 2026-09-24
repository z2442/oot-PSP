/* Opt-in diagnostic only; included by the backend after its PSP headers.
 * This tests transport, not autonomous sequence production. */
#include "oot_psp_audio_transport.h"

static OotPspAudioTransport sTransportRequests __attribute__((aligned(64), section(".uncached")));
static OotPspAudioTransport sTransportReplies __attribute__((aligned(64), section(".uncached")));
#define TRANSPORT_REQUESTS ((OotPspAudioTransport*)((uintptr_t)&sTransportRequests | 0x40000000u))
#define TRANSPORT_REPLIES ((OotPspAudioTransport*)((uintptr_t)&sTransportReplies | 0x40000000u))
#define TRANSPORT_TEST_COUNT 4096u

static void OotPspAudioTransportTest_Init(void) {
    /* Before ME boot. No cached accesses to these objects, including memset. */
    OotPspAudioTransport_Init(TRANSPORT_REQUESTS);
    OotPspAudioTransport_Init(TRANSPORT_REPLIES);
    meLibSync();
}

static void OotPspAudioTransportTest_MeStep(void) {
    OotPspAudioMessage message;
    uint32_t i;
    uint32_t write = __atomic_load_n(&TRANSPORT_REPLIES->published, __ATOMIC_RELAXED);
    uint32_t read = __atomic_load_n(&TRANSPORT_REPLIES->consumed, __ATOMIC_ACQUIRE);
    /* Reserve reply space before consuming the request. Only this CPU
     * produces replies; the main CPU can only increase the available space. */
    if (write - read >= OOT_PSP_AUDIO_TRANSPORT_CAPACITY) {
        return;
    }
    if (!OotPspAudioTransport_Pop(TRANSPORT_REQUESTS, &message)) {
        return;
    }
    message.kind = OOT_PSP_AUDIO_MESSAGE_LOAD_COMPLETE;
    for (i = 0; i < 5; i++) {
        message.words[i] ^= 0xa55a3cc3u;
    }
    OotPspAudioTransport_Push(TRANSPORT_REPLIES, &message, 1);
}

static void OotPspAudioTransportTest_MainStep(void) {
    static uint32_t sent;
    static uint32_t received;
    static uint32_t lastProgress;
    static int started;
    static int finished;
    OotPspAudioMessage message;
    uint32_t i;
    uint32_t budget;
    uint32_t now;
    if (finished) {
        return;
    }
    now = sceKernelGetSystemTimeLow();
    if (!started) {
        started = 1;
        lastProgress = now;
        printf("[audio-me-transport] begin 4096 RAM round trips; producer NOT migrated yet\n");
    }
    for (budget = 0; budget < 8 && OotPspAudioTransport_Pop(TRANSPORT_REPLIES, &message); budget++) {
        int valid = message.kind == OOT_PSP_AUDIO_MESSAGE_LOAD_COMPLETE &&
                    message.generation == 7 && message.ticket == received;
        for (i = 0; i < 5; i++) {
            valid = valid && message.words[i] == ((received ^ (0x12345678u + i)) ^ 0xa55a3cc3u);
        }
        if (!valid) {
            printf("[audio-me-transport] FAIL payload/order at %lu\n", (unsigned long)received);
            finished = 1;
            return;
        }
        received++;
        lastProgress = now;
    }
    if (received == TRANSPORT_TEST_COUNT) {
        printf("[audio-me-transport] PASS 4096 ordered RAM round trips\n");
        finished = 1;
        return;
    }
    if (now - lastProgress > 10000000u) {
        printf("[audio-me-transport] FAIL timeout sent=%lu received=%lu\n",
               (unsigned long)sent, (unsigned long)received);
        finished = 1;
        return;
    }
    for (budget = 0; budget < 8 && sent < TRANSPORT_TEST_COUNT; budget++) {
        message.kind = OOT_PSP_AUDIO_MESSAGE_LOAD_REQUEST;
        message.generation = 7;
        message.ticket = sent;
        for (i = 0; i < 5; i++) {
            message.words[i] = sent ^ (0x12345678u + i);
        }
        if (!OotPspAudioTransport_Push(TRANSPORT_REQUESTS, &message, 1)) {
            break;
        }
        sent++;
    }
}
