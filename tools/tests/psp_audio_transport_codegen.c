/* Compile-only PSP check: inspect undefined symbols and disassembly to
 * verify atomics do not introduce libatomic/kernel calls on the ME path. */
#include "../../src/port/psp/oot_psp_audio_transport.h"
int AudioTransportCodegenPush(OotPspAudioTransport* queue, const OotPspAudioMessage* messages, uint32_t count) {
    return OotPspAudioTransport_Push(queue, messages, count);
}
int AudioTransportCodegenPop(OotPspAudioTransport* queue, OotPspAudioMessage* message) {
    return OotPspAudioTransport_Pop(queue, message);
}
