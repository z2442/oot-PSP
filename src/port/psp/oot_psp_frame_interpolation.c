#include "oot_psp_frame_interpolation.h"

#include <stdint.h>
#include <string.h>

#define MATRIX_CAPACITY 2048
#define HASH_SIZE 1024
#define NO_INDEX (-1)

typedef struct MatrixRecord {
    const void* address;
    const void* identity;
    const void* site;
    float value[4][4];
    int addressNext;
    int groupNext;
    int previous;
} MatrixRecord;

typedef struct MatrixGroup {
    const void* identity;
    const void* site;
    int head;
    int tail;
    int count;
    int next;
} MatrixGroup;

typedef struct FrameHistory {
    MatrixRecord matrices[MATRIX_CAPACITY];
    MatrixGroup groups[MATRIX_CAPACITY];
    int addresses[HASH_SIZE];
    int buckets[HASH_SIZE];
    int count;
    int groupCount;
    int valid;
    const void* state;
    int scene;
    int room;
    int camera;
} FrameHistory;

static FrameHistory sHistory[2];
static unsigned int sCurrent;
static int sRecording;
static int sWorldScope;
static const void* sIdentity;
static float sFraction = 1.0f;

static unsigned int Hash(const void* a, const void* b) {
    uintptr_t bits = ((uintptr_t)a >> 4) ^ ((uintptr_t)b >> 2);
    return (unsigned int)(bits ^ (bits >> 10)) & (HASH_SIZE - 1);
}

static const void* MatrixAddress(const void* address) {
#if defined(TARGET_PSP)
    return (const void*)((uintptr_t)address & ~(uintptr_t)0x40000000U);
#else
    return address;
#endif
}

void OotPspFrameInterpolation_Reset(void) {
    sHistory[0].valid = sHistory[1].valid = 0;
    sRecording = sWorldScope = 0;
    sIdentity = NULL;
    sFraction = 1.0f;
}

void OotPspFrameInterpolation_Begin(const void* state, int scene, int room, int camera, int enabled) {
    FrameHistory* current;

    sCurrent ^= 1;
    current = &sHistory[sCurrent];
    current->count = current->groupCount = current->valid = 0;
    current->state = state;
    current->scene = scene;
    current->room = room;
    current->camera = camera;
    memset(current->addresses, 0xff, sizeof(current->addresses));
    memset(current->buckets, 0xff, sizeof(current->buckets));
    sRecording = enabled;
    sWorldScope = 0;
    sIdentity = state;
    sFraction = 1.0f;
}

void OotPspFrameInterpolation_SetContext(int scene, int room, int camera) {
    sHistory[sCurrent].scene = scene;
    sHistory[sCurrent].room = room;
    sHistory[sCurrent].camera = camera;
}

int OotPspFrameInterpolation_HasSample(void) {
    return sHistory[sCurrent].valid;
}

void OotPspFrameInterpolation_SetWorldScope(int enabled) {
    sWorldScope = enabled;
    sIdentity = sHistory[sCurrent].state;
}

void OotPspFrameInterpolation_SetIdentity(const void* identity) {
    sIdentity = identity != NULL ? identity : sHistory[sCurrent].state;
}

void OotPspFrameInterpolation_ForgetIdentity(const void* identity) {
    unsigned int pool;
    int i;

    /* An actor allocation can be reused for a different actor in the same tick. */
    for (pool = 0; pool < 2; pool++) {
        for (i = 0; i < sHistory[pool].count; i++) {
            if (sHistory[pool].matrices[i].identity == identity) {
                sHistory[pool].matrices[i].identity = NULL;
                sHistory[pool].matrices[i].previous = NO_INDEX;
            }
        }
        for (i = 0; i < sHistory[pool].groupCount; i++) {
            if (sHistory[pool].groups[i].identity == identity) {
                sHistory[pool].groups[i].identity = NULL;
            }
        }
    }
}

void OotPspFrameInterpolation_Record(const void* address, const float value[4][4], const void* site) {
    FrameHistory* current = &sHistory[sCurrent];
    MatrixRecord* record;
    unsigned int bucket;
    int index;

    if (!sRecording || !sWorldScope || address == NULL) {
        return;
    }
    address = MatrixAddress(address);
    bucket = Hash(address, NULL);
    for (index = current->addresses[bucket]; index != NO_INDEX; index = current->matrices[index].addressNext) {
        if (current->matrices[index].address == address) {
            break;
        }
    }
    if (index == NO_INDEX) {
        if (current->count == MATRIX_CAPACITY) {
            /* Overflow invalidates the whole sample, including its history. */
            sRecording = 0;
            return;
        }
        index = current->count++;
        current->matrices[index].addressNext = current->addresses[bucket];
        current->addresses[bucket] = index;
    }
    record = &current->matrices[index];
    record->address = address;
    record->identity = sIdentity;
    record->site = site;
    record->groupNext = record->previous = NO_INDEX;
    memcpy(record->value, value, sizeof(record->value));
}

static int FindGroup(FrameHistory* history, const void* identity, const void* site) {
    int group;
    for (group = history->buckets[Hash(identity, site)]; group != NO_INDEX; group = history->groups[group].next) {
        if (history->groups[group].identity == identity && history->groups[group].site == site) {
            return group;
        }
    }
    return NO_INDEX;
}

void OotPspFrameInterpolation_End(void) {
    FrameHistory* current = &sHistory[sCurrent];
    FrameHistory* previous = &sHistory[sCurrent ^ 1];
    int i;
    int compatible = previous->valid && current->state == previous->state && current->scene == previous->scene &&
                     current->room == previous->room && current->camera == previous->camera;

    current->valid = sRecording && current->count != 0;
    sRecording = sWorldScope = 0;
    if (!current->valid) {
        return;
    }
    for (i = 0; i < current->count; i++) {
        MatrixRecord* matrix = &current->matrices[i];
        int index = FindGroup(current, matrix->identity, matrix->site);
        MatrixGroup* group;
        if (index == NO_INDEX) {
            unsigned int bucket = Hash(matrix->identity, matrix->site);
            index = current->groupCount++;
            group = &current->groups[index];
            group->identity = matrix->identity;
            group->site = matrix->site;
            group->head = group->tail = NO_INDEX;
            group->count = 0;
            group->next = current->buckets[bucket];
            current->buckets[bucket] = index;
        }
        group = &current->groups[index];
        if (group->tail != NO_INDEX) {
            current->matrices[group->tail].groupNext = i;
        } else {
            group->head = i;
        }
        group->tail = i;
        group->count++;
    }
    if (!compatible) {
        return;
    }
    for (i = 0; i < current->groupCount; i++) {
        MatrixGroup* group = &current->groups[i];
        int oldGroup = FindGroup(previous, group->identity, group->site);
        if (group->identity != NULL && oldGroup != NO_INDEX && group->count == previous->groups[oldGroup].count) {
            int a = group->head;
            int b = previous->groups[oldGroup].head;
            while (a != NO_INDEX) {
                current->matrices[a].previous = b;
                a = current->matrices[a].groupNext;
                b = previous->matrices[b].groupNext;
            }
        }
    }
}

void OotPspFrameInterpolation_SetFraction(float fraction) {
    sFraction = fraction >= 0.0f && fraction <= 1.0f ? fraction : 1.0f;
}

static int CanBlend(const float a[4][4], const float b[4][4]) {
    float distance = 0.0f;
    int row, column;
    for (row = 0; row < 4; row++) {
        for (column = 0; column < 4; column++) {
            if (!__builtin_isfinite(a[row][column]) || !__builtin_isfinite(b[row][column])) {
                return 0;
            }
        }
    }
    for (column = 0; column < 3; column++) {
        float delta = a[3][column] - b[3][column];
        distance += delta * delta;
    }
    if (distance > 1000.0f * 1000.0f) {
        return 0;
    }
    /* Snap teleports, flips and abrupt scale changes instead of collapsing a
     * skeleton through a singular transform halfway through the interval. */
    for (row = 0; row < 3; row++) {
        float dot = 0.0f, aa = 0.0f, bb = 0.0f;
        for (column = 0; column < 3; column++) {
            dot += a[row][column] * b[row][column];
            aa += a[row][column] * a[row][column];
            bb += b[row][column] * b[row][column];
        }
        if (dot <= 0.0f || aa > bb * 16.0f || bb > aa * 16.0f) {
            return 0;
        }
    }
    return 1;
}

void OotPspFrameInterpolation_Resolve(const void* address, float value[4][4]) {
    FrameHistory* current = &sHistory[sCurrent];
    int index;
    int row, column;
    const float (*previous)[4];

    if (sFraction >= 1.0f || !current->valid) {
        return;
    }
    address = MatrixAddress(address);
    for (index = current->addresses[Hash(address, NULL)]; index != NO_INDEX;
         index = current->matrices[index].addressNext) {
        if (current->matrices[index].address == address) {
            break;
        }
    }
    if (index == NO_INDEX || current->matrices[index].previous == NO_INDEX) {
        return;
    }
    previous = sHistory[sCurrent ^ 1].matrices[current->matrices[index].previous].value;
    if (!CanBlend(previous, value)) {
        return;
    }
    for (row = 0; row < 4; row++) {
        for (column = 0; column < 4; column++) {
            value[row][column] = previous[row][column] + (value[row][column] - previous[row][column]) * sFraction;
        }
    }
}
