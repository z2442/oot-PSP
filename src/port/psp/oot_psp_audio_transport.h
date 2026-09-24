#ifndef OOT_PSP_AUDIO_TRANSPORT_H
#define OOT_PSP_AUDIO_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

/* A single-producer/single-consumer transport, NOT an audio engine.
 * PSP: allocate aligned storage, then use ONLY its uncached alias on BOTH
 * CPUs. Atomics order publication; they do not make PSP caches coherent.
 * Each direction needs its own queue. Init/reset requires both sides stopped.
 * No pointers into a recyclable command buffer are transported. */
#define OOT_PSP_AUDIO_TRANSPORT_CAPACITY 256u
#define OOT_PSP_AUDIO_TRANSPORT_MASK (OOT_PSP_AUDIO_TRANSPORT_CAPACITY - 1u)

typedef struct OotPspAudioMessage {
    uint32_t kind;
    uint32_t generation;
    uint32_t ticket;
    uint32_t words[5];
} OotPspAudioMessage;

enum {
    OOT_PSP_AUDIO_MESSAGE_COMMAND = 1,
    OOT_PSP_AUDIO_MESSAGE_BATCH_END,
    OOT_PSP_AUDIO_MESSAGE_LOAD_REQUEST,
    OOT_PSP_AUDIO_MESSAGE_LOAD_COMPLETE,
    OOT_PSP_AUDIO_MESSAGE_QUIESCE,
    OOT_PSP_AUDIO_MESSAGE_QUIESCED
};

typedef struct OotPspAudioTransport {
    uint32_t published __attribute__((aligned(64)));
    uint8_t producerPadding[60];
    uint32_t consumed __attribute__((aligned(64)));
    uint8_t consumerPadding[60];
    OotPspAudioMessage slots[OOT_PSP_AUDIO_TRANSPORT_CAPACITY] __attribute__((aligned(64)));
} OotPspAudioTransport;

typedef char OotPspAudioMessageSizeCheck[sizeof(OotPspAudioMessage) == 32 ? 1 : -1];
typedef char OotPspAudioTransportConsumerAlignment[offsetof(OotPspAudioTransport, consumed) == 64 ? 1 : -1];
typedef char OotPspAudioTransportSlotsAlignment[offsetof(OotPspAudioTransport, slots) == 128 ? 1 : -1];

static inline void OotPspAudioTransport_Init(OotPspAudioTransport* queue) {
    /* Slots need not be initialized: the consumer cannot see unpublished data. */
    __atomic_store_n(&queue->published, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&queue->consumed, 0u, __ATOMIC_RELAXED);
}

/* Producer only. Publishes the whole batch, or nothing on backpressure.
 * Inputs must not alias queue storage. A batch may include its BATCH_END
 * marker so partial sound-command transactions cannot become visible. */
static inline int OotPspAudioTransport_Push(OotPspAudioTransport* queue,
                                           const OotPspAudioMessage* messages, uint32_t count) {
    uint32_t write;
    uint32_t read;
    uint32_t used;
    uint32_t i;
    if (count > OOT_PSP_AUDIO_TRANSPORT_CAPACITY || (count != 0 && messages == NULL)) {
        return 0;
    }
    write = __atomic_load_n(&queue->published, __ATOMIC_RELAXED);
    read = __atomic_load_n(&queue->consumed, __ATOMIC_ACQUIRE);
    used = write - read;
    if (used > OOT_PSP_AUDIO_TRANSPORT_CAPACITY || count > OOT_PSP_AUDIO_TRANSPORT_CAPACITY - used) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        queue->slots[(write + i) & OOT_PSP_AUDIO_TRANSPORT_MASK] = messages[i];
    }
    __atomic_store_n(&queue->published, write + count, __ATOMIC_RELEASE);
    return 1;
}

/* Consumer only. Copy before releasing a slot; never return an internal
 * pointer that the producer could overwrite while a command is executing. */
static inline int OotPspAudioTransport_Pop(OotPspAudioTransport* queue, OotPspAudioMessage* message) {
    uint32_t read = __atomic_load_n(&queue->consumed, __ATOMIC_RELAXED);
    uint32_t write = __atomic_load_n(&queue->published, __ATOMIC_ACQUIRE);
    uint32_t available = write - read;
    if (message == NULL || available == 0 || available > OOT_PSP_AUDIO_TRANSPORT_CAPACITY) {
        return 0;
    }
    *message = queue->slots[read & OOT_PSP_AUDIO_TRANSPORT_MASK];
    __atomic_store_n(&queue->consumed, read + 1u, __ATOMIC_RELEASE);
    return 1;
}

#endif
