#ifndef OOT_PSP_ME_CONTEXT_H
#define OOT_PSP_ME_CONTEXT_H

#include <stdint.h>

static inline int OotPspMe_IsWorker(void) {
    uintptr_t stack;

    /* me-core's default ME stack is below physical 4 MiB; Allegrex user
     * stacks live in main RAM. This works with either cached address alias
     * and does not let ME cache eviction publish an execution flag to CPU. */
    __asm__ volatile("move %0, $sp" : "=r"(stack));
    return (stack & 0x1FFFFFFFU) < 0x00400000U;
}

#endif
