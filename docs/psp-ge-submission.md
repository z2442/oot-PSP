# PSP GE submission and vertex uploads

The GU backend publishes accumulated rendering commands after **12 draws** or
**220 triangles**, whichever threshold is reached first. A single large draw
publishes immediately. The SDK's `sceGuFinish` publishes any remaining tail
before synchronization, including frames with fewer than twelve draws. Sprites
count as two triangles per quad. Object scopes defer publication as in the SDK;
call-list contexts continue through the ordinary SDK entry points.

`gfx_psp_submit.c` uses the command encoding and display-list cursor ABI from
[PSPSDK](https://github.com/pspdev/pspsdk/tree/master/src/gu). Backend draw and
inline-allocation calls use these helpers; SDK and intraFont functions retain
their original behavior. The SDK ABI is checked at compile time on PSP.
[PSPSDK's license](PSPSDK-LICENSE.txt) is also included in installable archives.

## Cached arena

Base and fog passes upload into a **2 MiB cached vertex arena**, inspired by
[Sonic-R-PSP's cached frame arena](https://github.com/ghurone/Sonic-R-PSP/blob/main/src/psp/render_gu.c).
Each upload gets its own 64-byte-aligned allocation and is written back before
its draw command becomes visible to the GE. This also makes early publication
by a font or SDK call safe. Secondary texture and fog passes retain separate
copies, so changing their UVs or alpha cannot modify a queued base pass.

Arena exhaustion, texture-cache eviction and command-list rollover finish and
synchronize the previous list before recycling storage. Every list restart
restores the current draw buffer, including the spare used by triple buffering.
The command list is now **256 KiB**, since it no longer contains 3D/fog vertex
uploads. The combined command/vertex allocation grows by 256 KiB compared with
the former 2 MiB list. UI and shadow inline data remain uncached.

## Triangle output

Shade-only materials use a separate output loop. It selects textured/untextured
output outside the vertex loop, copies the shade color directly, emits the
appropriate fog stream separately, and updates batch counters once per polygon.
White RGB multipliers and fully opaque alpha multipliers are removed when
preparing material state, so these exact identities do not force character
surfaces through the general output loop.
Multipass materials, color tints, environment/primitive multiplication, LOD color
and independent alpha sources continue through the general emitter. Clipping,
winding, lighting, triangle order and animation timing are unchanged.

## Validation and measurement

- `tools/tests/test_psp_ge_submit.py` checks command words, both publication
  thresholds, short-frame tails, indexed/native draws, object/call-list behavior,
  cache-line alignment, immutable multipass uploads and fenced rollover.
- `tools/tests/test_psp_triangle_output.py` compares 100,000 randomized output
  cases byte-for-byte with the general emitter, including clipped polygons,
  texture coordinates, surface alpha and both fog vertex formats. Another
  10,000 material cases compare output before and after removing identity
  multipliers, including non-white tints that must retain their multiplication.
- The existing PSP rendering and frame-pacing tests cover the surrounding paths.

For a synthetic workload of 120 small uploaded draws, the old allocation/draw
path needed 240 GE-update calls plus finish. The new path needs 120 cache
writebacks, 10 batched GE updates and finish. That is a call-count model, not an
on-device performance measurement; external SDK/font activity can publish more
often, and large batches reach the triangle threshold sooner.

The requested roughly 40% reduction and 0.9 ms Castle Courtyard gain have not
been measured in this checkout. Compare the same scene, camera, video output
and FPS setting on hardware. `make psp-port PSP_OOTDEBUG=1` enables the existing
`oot-psp-performance.csv` renderer/display-list/submit timing report. Check fog,
shadows, multipass effects, HUD, pause/HOME and LCD/TV transitions as well as FPS.
