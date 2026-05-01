#include "libc64/sprintf.h"
#include "assert.h"
#include "fault.h"

NORETURN void __assert(const char* assertion, const char* file, int line) {
    char msg[256];

    osSyncPrintf("Assertion failed: %s, file %s, line %d, thread %ld\n", assertion, file, line, osGetThreadId(NULL));
    sprintf(msg, "ASSERT: %s:%d(%ld)", file, line, osGetThreadId(NULL));
    Fault_AddHungupAndCrashImpl(msg, assertion);
}
