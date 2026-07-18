#ifndef OOT_PSP_PERFORMANCE_H
#define OOT_PSP_PERFORMANCE_H

#include <stdint.h>

#if defined(TARGET_PSP) && defined(OOTDEBUG)
uint64_t OotPspPerformance_Now(void);
void OotPspPerformance_BeginFrame(int sceneId, int roomId);
void OotPspPerformance_RecordActorUpdate(uint64_t usec);
void OotPspPerformance_RecordActorDraw(uint64_t usec);
void OotPspPerformance_RecordRenderer(uint64_t usec);
void OotPspPerformance_RecordPacing(uint64_t usec);
void OotPspPerformance_RecordGfx(uint64_t displayListUsec, uint64_t submitUsec, uint32_t commandCount,
                                 uint32_t inputTriangleCount, uint32_t outputTriangleCount, uint32_t flushCount,
                                 uint32_t drawCallCount, uint32_t opaqueDrawCallCount,
                                 uint32_t opaqueTriangleCount, uint32_t translucentDrawCallCount,
                                 uint32_t translucentTriangleCount, uint32_t maxBatchTriangles,
                                 uint32_t textureUploadCount);
void OotPspPerformance_EndFrame(uint64_t gameUsec, uint64_t audioUsec, uint64_t totalUsec);
void OotPspPerformance_Flush(void);
#endif

#endif
