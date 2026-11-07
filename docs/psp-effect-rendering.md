# PSP material and overlay rendering

Epona's eye, hair and other clamped textures use both MIRROR and CLAMP.
The RDP clamps coordinates before applying the mirror mask. When the clamp
edge lies within the first mask period, PSP uploads the original image
without a reflected copy. This keeps the eye at its original 32x16 extent
and prevents an extra eye on each side of her head. Wrapped mirror textures
and clamped tiles extending beyond a mask period retain mirror emulation.

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

Gameplay minimaps explicitly select the right HUD anchor before drawing the
map, entrance/chest markers and compass arrows. Rectangles and projected
arrows use the same uniform HUD scale in 4:3 and widescreen on LCD and TV.
Compass arrows use their original z=0 rather than the obsolete z=-50
workaround, keeping them inside the HUD projection throughout their rotation.

Close-up cutscenes and first-person/weapon-aiming views temporarily use a
one-unit perspective near plane on PSP. The nominal ten-unit plane clips
foreground weapons that OoT's NoN microcode normally permits, including the
Master Sword placement and slingshot string. Both CPU clipping and GU receive
the same updated projection. The view's requested near value is restored
after matrix generation, so ordinary gameplay retains its normal depth
precision. Side, far and behind-camera clipping remain enabled.

The foreground plane is selected by scripted cutscenes and aiming camera
modes, rather than the broader player-blocking state used by doors and
ocarina interaction. When it is active (including scripted warps), fog still
uses the original near plane's depth curve. A display-list extension carries
an affine depth correction derived from the two Q16 projection matrices to
both world streams. The vertex transform applies that correction to each
room/actor's raw fog factors using floats, avoiding signed-16-bit overflow.
Zero-multiplier fog fades retain their original opacity. Ordinary world
frames, the overlay stream and renderer frame reset restore identity fog
correction, so the close-up setting cannot leak into later drawing.

Link's first-person model selection also depends on projected head depth.
On PSP it now evaluates the head in view space and applies the requested
near/far plane's Q16 depth coefficients. The shorter foreground plane
therefore cannot delay the switch to the existing first-person limb callback.
Looking hides the body; aiming retains the weapon/arm meshes while hiding
the head, hair and hat. The original transition threshold remains in effect
as the camera approaches and leaves Link.

Host regression checks:

- `python3 tools/tests/test_psp_arrow_glow.py`
- `python3 tools/tests/test_psp_equip_projection.py`
- `python3 tools/tests/test_psp_get_item_materials.py`
- `python3 tools/tests/test_psp_frustum.py`
- `python3 tools/tests/test_psp_decal_depth.py`
- `python3 tools/tests/test_psp_close_camera.py`
- `python3 tools/tests/test_psp_first_person.py`
- `python3 tools/tests/test_psp_rectangle_scissor.py`

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
Close-camera checks exercise blocked-player and scripted/aiming transitions,
normal-projection restoration and both world fog streams with the actual Q16
perspective matrix. Fog sweeps cover multiple far distances, matrix scales,
room factors and constant-opacity fades; the generated float factors also
run through the actual VFPU transform assembly interpreter. Minimap checks include
rectangles, compass depth and marker alignment in LCD/TV aspect modes.
First-person model checks sweep camera/head positions, original depth ranges
and matrix scales against the nominal projection, reproducing the head
visibility regression and checking the actual looking/aiming limb callback
for both ages.
Appearance and blending still require verification on PSP hardware.
