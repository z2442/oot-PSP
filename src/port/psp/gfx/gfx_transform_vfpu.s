.set noat

.text
.set push
.set noreorder

/*
 * void gfx_transform_vertices_vfpu(
 *     struct LoadedVertex *dest,
 *     const Vtx *source,
 *     uint32_t count,
 *     const struct GfxVfpuTransformMatrices *matrices);
 *
 * Vtx is 16 bytes, with signed object coordinates at offsets 0, 2, and 4.
 * LoadedVertex is 48 bytes; its model and clip vectors start at 0 and 16.
 * Matrices use the renderer's row-vector layout, so each VFPU dot product is
 * made against one matrix column.
 */

#define VTX_SIZE 16
#define LOADED_VERTEX_SIZE 48
#define LOADED_VERTEX_MODEL_POS 0
#define LOADED_VERTEX_CLIP_POS 16

.globl gfx_transform_vertices_vfpu
gfx_transform_vertices_vfpu:
	beq     $a2, $zero, .Ltransform_done
	nop
	lw      $t8, 0($a3)
	lw      $t9, 4($a3)

	/* Model matrix columns. */
	lv.s    S000, 0($t8)
	lv.s    S001, 16($t8)
	lv.s    S002, 32($t8)
	lv.s    S003, 48($t8)
	lv.s    S010, 4($t8)
	lv.s    S011, 20($t8)
	lv.s    S012, 36($t8)
	lv.s    S013, 52($t8)
	lv.s    S020, 8($t8)
	lv.s    S021, 24($t8)
	lv.s    S022, 40($t8)
	lv.s    S023, 56($t8)
	lv.s    S030, 12($t8)
	lv.s    S031, 28($t8)
	lv.s    S032, 44($t8)
	lv.s    S033, 60($t8)

	/* Projection matrix columns. */
	lv.s    S100, 0($t9)
	lv.s    S101, 16($t9)
	lv.s    S102, 32($t9)
	lv.s    S103, 48($t9)
	lv.s    S110, 4($t9)
	lv.s    S111, 20($t9)
	lv.s    S112, 36($t9)
	lv.s    S113, 52($t9)
	lv.s    S120, 8($t9)
	lv.s    S121, 24($t9)
	lv.s    S122, 40($t9)
	lv.s    S123, 56($t9)
	lv.s    S130, 12($t9)
	lv.s    S131, 28($t9)
	lv.s    S132, 44($t9)
	lv.s    S133, 60($t9)

.Ltransform_vertex:
	lh      $t0, 0($a1)
	lh      $t1, 2($a1)
	lh      $t2, 4($a1)
	mtv     $t0, S200
	mtv     $t1, S201
	mtv     $t2, S202
	vi2f.s  S200, S200, 0
	vi2f.s  S201, S201, 0
	vi2f.s  S202, S202, 0
	vone.s  S203

	vdot.q  S300, C000, C200
	vdot.q  S301, C010, C200
	vdot.q  S302, C020, C200
	vdot.q  S303, C030, C200

	vdot.q  S310, C100, C300
	vdot.q  S311, C110, C300
	vdot.q  S312, C120, C300
	vdot.q  S313, C130, C300

	sv.q    C300, LOADED_VERTEX_MODEL_POS($a0)
	sv.q    C310, LOADED_VERTEX_CLIP_POS($a0)

	addiu   $a1, $a1, VTX_SIZE
	addiu   $a0, $a0, LOADED_VERTEX_SIZE
	addiu   $a2, $a2, -1
	bne     $a2, $zero, .Ltransform_vertex
	nop

.Ltransform_done:
	jr      $ra
	nop

.set pop
