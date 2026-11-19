#ifndef GFX_PSP_SUBMIT_H
#define GFX_PSP_SUBMIT_H

#include <stddef.h>

#define GFX_PSP_GE_BATCH_DRAWS 12U
#define GFX_PSP_GE_BATCH_TRIANGLES 220U
#define GFX_PSP_VERTEX_ARENA_BYTES (2U * 1024U * 1024U)
#define GFX_PSP_COMMAND_LIST_BYTES (256U * 1024U)

/* Call only after starting a fresh direct list, with the previous list fully
 * synchronized. Resetting also releases that list's immutable vertex uploads. */
void gfx_psp_submit_reset(void);
void* gfx_psp_submit_alloc_vertices(size_t bytes);
/* Write back every arena upload not yet visible to the GE. Required before
 * anything that can advance the GE stall address outside this file (SDK draw,
 * memory or finish calls, intraFont). Draw thresholds and finish do it here. */
void gfx_psp_submit_publish_vertices(void);
/* Reserve up to maxBytes for a batch written in place, then commit its final
 * size before drawing it. Commit before anything else allocates, so the
 * unused tail returns to the arena. A list reset keeps an open reservation. */
void* gfx_psp_submit_reserve_vertices(size_t maxBytes);
void gfx_psp_submit_commit_vertices(size_t bytes);
int gfx_psp_submit_finish(void);
void* gfx_psp_submit_get_memory(int bytes);
void gfx_psp_submit_draw_array(int primitive, int format, int count, const void* indices, const void* vertices);

#endif
