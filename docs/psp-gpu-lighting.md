# PSP GPU lighting

The PSP renderer uses GE fixed-function ambient and diffuse lighting. The old
CPU per-vertex light accumulation has been removed; there is no software
lighting fallback or runtime switch.

For each N64 light set, the renderer selects up to four nonzero directional
lights by RGB sum, keeping source order for equal strengths. OoT already
converts point lights into actor-relative directions and attenuated colors in
`Lights_BindPoint`, so those lights use the same GE directional-light path.
The original ambient color comes from the slot after the full N64 light count.
Each remaining light contributes one quarter of its RGB to ambient, clamped
to 255. This approximates the average diffuse contribution over a sphere and
retains some brightness when more than four lights affect an actor.

Normals follow the modelview matrix at vertex-load time, alongside positions.
This preserves flexible-limb meshes that combine vertices loaded under different
matrices. The VFPU transforms normals; the GE normalizes them and calculates
ambient/diffuse vertex colors. Clipping interpolates normals, surface alpha and
fog alpha independently. A saved light set stays attached to each loaded vertex,
and changing batch lighting flushes queued geometry before updating GE state.
Snapshots and ranking are reused until a light command changes the light set.

Vertex-transform setup loads aligned model/projection matrices using eight
quad loads instead of 32 scalar loads. Memory rows are read through transposed
VFPU register views so dot products retain their original operand order. A
word-aligned scalar fallback handles deferred native vertices and other
unaligned matrix pointers. XYZ integer coordinates convert together with one
triple instruction instead of three scalar conversions. Matrix bytes read,
transform math and vertex layout are unchanged; these are instruction-count
reductions, not measured FPS gains.

The GU backend also retains the applied lighting registers across material
batches. Equal light sets emit no repeated setup commands; fog and UI passes
toggle lighting without discarding those registers. Changes update only the
affected ambient value, light direction, color or enable bit. Frame boundaries
and video mode changes invalidate this cache for callback/SDK rendering.

Vertex RGB supplies the GE ambient/diffuse material tint, and vertex alpha
continues to supply surface opacity and two-texture pass weights. Fog overlays,
HUD rectangles and unlit native meshes explicitly disable GE lighting.
The CPU still interprets display lists, prepares lights and material colors,
transforms/clips geometry, and generates reflection texture coordinates.

## Approximation limits

- Lights beyond the strongest four lose their directional response. Their
  ambient approximation can brighten shadowed faces and cannot reproduce their
  highlights.
- GE normal normalization and color rounding differ from N64 lighting. Uneven
  scaling or sheared matrices can change the lighting response further.
- Material tint is applied during GE lighting instead of after the N64 shade
  clamp, so saturated surfaces can differ.
- A triangle whose vertices were loaded with different light sets uses the
  majority set, or the first vertex's set when all three differ. Ordinary limb
  matrix changes retain the same set and preserve each vertex's own normal.
- Clipped vertices are lit from interpolated normals rather than interpolated
  prelit colors.

The GE vertex stream grows from 24 to 36 bytes per vertex, and the interpreter's
loaded-vertex structure grows from 48 to 64 bytes. Removing CPU light accumulation
therefore trades some CPU work for GPU work and additional vertex bandwidth;
speed improvement must be measured on hardware.

CPU-generated silhouette masks have a separate [GU shadow path](psp-gpu-shadows.md).

## Validation

Build with `make -j4 psp-port`. Relevant host regressions:

```sh
python3 tools/tests/test_psp_gpu_lighting.py
python3 tools/tests/test_psp_vertex_pipeline.py
python3 tools/tests/test_psp_native_mesh.py
python3 tools/tests/test_psp_texture_coords.py
python3 tools/tests/test_psp_zero_fog.py
```

The lighting test exercises the actual selection, snapshot, batch and GU setup
functions with sanitizers, including repeated material/fog transitions and
10,000 changing light sets. The assembly test interprets the actual transform and
clipping instructions, including limb normals, vertex stride, alpha and fog.
It also compares 256 aligned/unaligned transform configurations byte-for-byte
with the previous scalar matrix loader/conversions, using general projections,
signed coordinate extremes and both lighting/fog modes.
Neither replaces a PSP visual/performance check. Check Link's animated joints,
torch/fairy lighting with more than four nearby lights, reflective equipment,
translucent effects, near-plane clipping, fog, and HUD/menu transitions.
