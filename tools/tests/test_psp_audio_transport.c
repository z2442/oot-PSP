#include "../../src/port/psp/oot_psp_audio_transport.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>

static OotPspAudioTransport queue;
static OotPspAudioMessage batch[OOT_PSP_AUDIO_TRANSPORT_CAPACITY];
#define STRESS_COUNT 1000000u

static OotPspAudioMessage MakeMessage(uint32_t ticket) {
    OotPspAudioMessage message = { OOT_PSP_AUDIO_MESSAGE_COMMAND, 7, ticket, {0} };
    uint32_t i;
    for (i = 0; i < 5; i++) {
        message.words[i] = ticket ^ (0x12345678u + i);
    }
    return message;
}

static void CheckMessage(const OotPspAudioMessage* message, uint32_t ticket) {
    OotPspAudioMessage expected = MakeMessage(ticket);
    assert(memcmp(message, &expected, sizeof(expected)) == 0);
}

static void* Produce(void* unused) {
    uint32_t i;
    (void)unused;
    for (i = 0; i < STRESS_COUNT; i++) {
        OotPspAudioMessage message = MakeMessage(i);
        while (!OotPspAudioTransport_Push(&queue, &message, 1)) {
            sched_yield();
        }
    }
    return NULL;
}

int main(void) {
    OotPspAudioMessage message;
    pthread_t producer;
    uint32_t i;
    OotPspAudioTransport_Init(&queue);
    assert(!OotPspAudioTransport_Pop(&queue, &message));
    assert(OotPspAudioTransport_Push(&queue, NULL, 0));
    assert(!OotPspAudioTransport_Push(&queue, NULL, 1));
    assert(!OotPspAudioTransport_Push(&queue, batch, OOT_PSP_AUDIO_TRANSPORT_CAPACITY + 1));
    for (i = 0; i < OOT_PSP_AUDIO_TRANSPORT_CAPACITY; i++) {
        batch[i] = MakeMessage(i);
    }
    assert(OotPspAudioTransport_Push(&queue, batch, OOT_PSP_AUDIO_TRANSPORT_CAPACITY));
    assert(!OotPspAudioTransport_Push(&queue, batch, 1));
    assert(!OotPspAudioTransport_Pop(&queue, NULL));
    for (i = 0; i < OOT_PSP_AUDIO_TRANSPORT_CAPACITY; i++) {
        assert(OotPspAudioTransport_Pop(&queue, &message));
        CheckMessage(&message, i);
    }
    assert(!OotPspAudioTransport_Pop(&queue, &message));

    /* Batch failure does not publish a prefix. */
    assert(OotPspAudioTransport_Push(&queue, batch, OOT_PSP_AUDIO_TRANSPORT_CAPACITY - 1));
    i = queue.published;
    assert(!OotPspAudioTransport_Push(&queue, batch, 2));
    assert(queue.published == i);

    /* Both integer wrap and ring wrap, with exact FIFO payload checks. */
    OotPspAudioTransport_Init(&queue);
    queue.published = queue.consumed = UINT32_MAX - 7u;
    assert(OotPspAudioTransport_Push(&queue, batch, 32));
    for (i = 0; i < 32; i++) {
        assert(OotPspAudioTransport_Pop(&queue, &message));
        CheckMessage(&message, i);
    }
    assert(!OotPspAudioTransport_Pop(&queue, &message));

    /* Corrupt occupancy fails closed rather than overwriting unread slots. */
    queue.published = 1000;
    queue.consumed = 0;
    assert(!OotPspAudioTransport_Push(&queue, batch, 1));
    assert(!OotPspAudioTransport_Pop(&queue, &message));

    OotPspAudioTransport_Init(&queue);
    assert(pthread_create(&producer, NULL, Produce, NULL) == 0);
    for (i = 0; i < STRESS_COUNT; i++) {
        while (!OotPspAudioTransport_Pop(&queue, &message)) {
            sched_yield();
        }
        CheckMessage(&message, i);
    }
    assert(pthread_join(producer, NULL) == 0);
    assert(!OotPspAudioTransport_Pop(&queue, &message));
    puts("audio transport: bounds, backpressure, wrap, payloads and 1000000 concurrent messages passed");
    return 0;
}
