# PSP silhouette shadows

Link's dynamic silhouette (`En_Sda`), Ganon's silhouette (`Boss_Ganon2`) and
Ganondorf's silhouette/cape (`Boss_Ganon`) build their masks using the PSP GU.
The PSP paths no longer clear, fill or shade a 64×64 image on the CPU. Original
pixel routines remain available to the N64 build.

The CPU retains body-part interpolation, projection into mask coordinates,
floor placement and final shadow opacity. Instead of writing pixels, it records
compact stamp commands inside the original frame allocation. A PSP display-list
extension attaches those commands to the following ordinary shadow texture load.
Unmarked textures continue through the normal image importer.

The GU stamps the original body, shield and cape profiles from an immutable
atlas into a dedicated 64×64 RGBA render target in EDRAM. Transparent atlas texels
are alpha-tested; passing texels replace framebuffer alpha using stencil, giving
opaque mask pixels without darkening overlapping shapes. Texture-edge clipping
matches Link's separate X/Y bounds and the bosses' linear-index row wrapping.
Link's debug border and optional row fade are preserved with GU passes.

The GE copies each completed mask into its own texture-cache allocation. This
also works when TV mode keeps textures in main RAM: rendering stays in EDRAM,
and only the GE transfer writes the cached texture. No CPU readback or texture
conversion is required. Cache keys distinguish GPU masks from ordinary I8
images, reserve the full RGBA size, and retain separate allocations when a
texture has already been used in the current frame. Draw buffer, scissor,
lighting, depth, shader and sampler state are restored after mask generation.

Memory costs are 16 KiB of persistent EDRAM for the scratch target (plus any
padding needed for its 8 KiB framebuffer alignment), an 8 KiB
immutable stamp atlas in RAM, and 16 KiB per cached mask. Vertex commands and
GE transfers add overhead, so any speed improvement requires measurement on
hardware. Static shadows baked into environment textures and ordinary
foot/blob-shadow placement are unchanged.

## Validation

```sh
make -j4 psp-port
python3 tools/tests/test_psp_gpu_shadows.py
python3 tools/tests/test_psp_shadow_dispatch.py
python3 tools/tests/test_psp_texture_stride.py
python3 tools/tests/test_psp_rectangle_scissor.py
```

The mask test rasterizes the actual GU commands on the host and compares 4,129
masks against the original CPU stamp loops. It covers body/shield/cape profiles,
overlap, edge wrapping, borders, row fades, stencil alpha, texture copies and GU
state restoration. Separate tests cover display-list dispatch, GPU import routing,
cache allocation boundaries and retained textures. PSP compilation checks the real
API and ABI. These tests do not emulate GE scheduling or rasterization precision.

On PSP, check Link's dynamic shadow scenes, the Ganondorf fight with intact/torn
cape sections, Ganon's final battle, near-plane clipping, ordinary geometry drawn
after a shadow, and both LCD and TV output. Compare performance using the same
scene and camera before and after this change.
