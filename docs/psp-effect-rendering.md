# PSP material and overlay rendering

Elemental arrows use two scrolling 32x64 I8 masks. Their tile rectangles
describe scrolling views, sometimes only 4x32 or 4x16, rather than the loaded
image dimensions. Wrapped I8 load-block textures retain their loaded row
stride and image extent during import. Explicit strided load-tile views and
clamped textures retain their existing sizing rules.

The fire, ice and light arrow material uses two RDP combiner cycles. On PSP,
both masks use the existing two-pass GU effect path, with separate scroll
coordinates, primitive/environment colors and the actor's charge/fade alpha.
This approximates the original additive/subtractive mask combiner with two
translucent layers. It costs a second draw for these effects and does not
reintroduce software lighting or per-frame texture composition.

The secondary arrow shimmer fades to zero at the wide outer rim of the mesh.
The fade uses unscrolled mesh T coordinates, so it stays attached to that rim
while both textures scroll. GU interpolates its vertex alpha over the outer
faces. The primary glow and the inner shimmer retain their existing opacity;
unrelated two-texture materials do not use the fade.

Freezard breath uses a subtractive two-texture alpha combiner that the compact
PSP shader cannot represent. Its GU fallback samples the stationary TEXEL1
cloud mask directly for both tint and coverage, multiplied by the particle's
animated primitive alpha. This preserves the soft silhouette and fade of
the mist without showing the enclosing billboard triangle. The scrolling
TEXEL0 noise is omitted in this approximation.

The 3D Odd Mushroom and Odd Potion share a reflective outer material. Its
first color cycle is `(TEXEL0 - SHADE) * PRIM_LOD_FRAC + ENVIRONMENT`; its
second tints the result between primitive and environment colors. The
compact PSP combiner cannot represent that first cycle and used to discard
the texture, leaving a flat purple shell. A single GU tint pass now retains
the reflection map and the purple/yellow color endpoints. This approximates
the subtractive lighting equation without reintroducing CPU lighting. The
primitive alpha of 255 keeps dark reflection texels opaque even in the
models' texture-edge render mode.

Regular bombs use `TEXEL0 + PRIMITIVE` for their body color. GU's additive
texture function now preserves the red primitive-color pulses from the bomb
timer, while texture alpha continues to mask the silhouette. Both texture
tint option bits together encode addition; either bit on its own keeps the
existing primitive/environment or shade tint mode.

Ground paths in Hyrule Field and Hyrule Castle use overlays. CPU frustum
clipping now uses the same horizontal projection as GU: aspect/anchor
adjustments apply only to anchored fullscreen HUD geometry. World geometry
retains the existing GU projection, and native meshes materialize their
vertices in that same clip space. Near-plane rejection (`z < -w`) clips
against `-z - w`, while far-plane rejection (`z > w`) clips against `z - w`.
This prevents crossing ground overlays from reaching GU with vertices
outside the view volume.

Comparing road and terrain meshes finds road samples about 0.54 game units
below the grass after vertex rounding. A fixed GE offset could lose that
comparison; a slope-based screen-depth offset also let distant paths cover
Link's feet. Upward-facing lit floor decals now move one world unit along
their surface normal, with only two GE depth units for rounding. CPU clip
coordinates and submitted positions both receive that displacement before
clipping. Other decals retain their 32-unit offset, and batches flush when
switching offset classes. Color and fog share the same depth; ordinary
geometry uses zero bias and depth testing remains enabled. Decals bypass
the native mesh shortcut so the bounded displacement is applied.

The Temple of Time crossfades normal and blue floor textures using
environment alpha. Its room lists use both SHADE and PRIMITIVE as their
second-cycle multiplier. Both now use the two-texture GU path, preserving
the selected multiplier and the transition's blue color across all sections.

The inventory equip icon is an orthographic quad at z=0. After drawing the
A button, the HUD view still has perspective near/far settings of 10..60.
The PSP HUD orthographic setup temporarily uses -1..1 so the equip animation
passes near-plane clipping, then restores the saved depth settings. This is
the same correction used in the Majora's Mask port.
The targeting reticle also uses z=0. The old PSP z=-50 workaround lay outside
this corrected HUD depth range and has been removed.

Host regression checks:

- `python3 tools/tests/test_psp_arrow_glow.py`
- `python3 tools/tests/test_psp_equip_projection.py`
- `python3 tools/tests/test_psp_get_item_materials.py`
- `python3 tools/tests/test_psp_frustum.py`
- `python3 tools/tests/test_psp_decal_depth.py`

The checks cover mask extents, scrolling coordinates, arrow rim coverage,
Freezard cloud-mask selection, particle opacity and the icon's projected
flight, plus the Odd Mushroom/Potion shaders and additive bomb flashes.
Frustum checks compare CPU clipping to the uploaded GU projection and sweep
road quads across side, bottom and near boundaries, including positions
behind the camera.
Decal tests reproduce fixed-offset failures for slightly sunken road meshes
and check bounded displacement, feet two units above the ground, bias
isolation/reset and matching color/fog depths. HUD checks include reticle
depth, and material checks cover both Temple floor crossfade variants.
Appearance and blending still require verification on PSP hardware.
