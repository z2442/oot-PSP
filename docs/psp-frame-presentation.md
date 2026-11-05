# PSP frame presentation

HOME > Video Settings > Frame Rate selects **20 FPS** (default) or **60 FPS**.
Changes take effect on resuming the game. Select **Save video.ini** to retain
this setting. The equivalent configuration beside EBOOT.PBP is:

```ini
[video]
fps = 60
vsync = on
```

Only `20` and `60` are supported. Missing or invalid values select 20. Reset
Defaults restores 20 FPS and VSync on. Frame Rate controls gameplay presentation;
states with their own native update interval retain that interval.

## Timing and animation

The design follows the separation of simulation and presentation used by
[sf64-psp](https://github.com/TheMrIron2/sf64-psp/blob/master/src/psp/frame_interpolation.c),
with an OoT-specific history implementation. The scheduler preserves each task's
original VI update rate. A normal three-VI game tick remains 50 ms; selecting
60 FPS replays its completed display list three times, with render-only matrix
fractions of 1/3, 2/3 and 1. It never reruns game updates, actor draw callbacks,
pose sampling, collision, timers, input or animation event handling for those
extra presentations. The last presentation uses the exact simulation matrices,
including the depth buffer read by the following tick.

History owns float copies of transforms instead of retaining the previous
graphics arena. Matching uses actor/effect identity, matrix creation site and
ordinal within that site. Changed group counts, new/reused allocations, scene,
room or camera mode changes, large translation jumps, flips and abrupt scale
changes use the current transform. History resets around HOME and display mode
changes. Pause, transitions and states without captured world matrices use their
native task cadence. HUD matrices are outside the capture scope.

This smooths matrix-driven motion and skeletal poses. Texture frame changes,
vertex deformation, colors and HUD animation still advance at their original
simulation cadence. Matrix blending is linear, so extreme rotations snap rather
than blend through a collapsed pose. Intermediate presentations introduce up to
two VIs of visual delay relative to the current simulation sample. 60 FPS is a
target: expensive scenes can run slower on hardware. Missed deadlines rebase
without running catch-up simulation ticks.

## Buffer ownership

The game thread publishes each tick's audio requests before translating its
completed display list into GE commands. AudioGen continues at its independent
60 Hz cadence, preparing sequence/control state on Allegrex and submitting
command construction and mixing to the ME.

After `sceGuFinish`, the `OOT PSP Video` thread owns GE completion and
presentation, including vblank waits and 480i field conversion. Submission
returns immediately so the game thread can run the following tick's logic and
build its N64 display list while the previous tick finishes on the GE. Only one
GU frame is outstanding. Before translating the following display list, the
game thread joins the worker, then releases retained mesh pins and reuses the
command list, cached vertex arena and textures. Interpolated presentations use
the same handoff and fence between passes.

Depth-buffer reads join the worker before consuming the previous frame's
result. Display-mode changes, render/texture cache invalidation and game-state
destruction also drain outstanding video work. HOME callback frames drain
before invalidating GU state. If worker initialization fails, presentation
uses the synchronous path.

LCD and native 240p use three EDRAM color buffers plus a depth buffer. With
VSync enabled, a completed color buffer is queued for the next vblank while
the GE can render into the spare. Neither the scanned buffer nor the queued
buffer is reused for drawing. Turning VSync off first retires a pending swap,
then presents immediately. Render storage is reused only after the video
worker's completion fence.

720x480 TV modes retain double buffering: three 32-bit color buffers plus depth
would exceed the Slim's 4 MiB of EDRAM. The 480i field conversion remains intact.
Mode changes rebuild the framebuffer layout; Slim textures remain in RAM and
the PSP-1000 LCD texture allocation keeps a stable EDRAM address.

## Validation

`tools/tests/test_psp_frame_interpolation.py` checks history matching, graphics
arena reuse, discontinuities, scope exclusion, context changes and overflow.
`tools/tests/test_psp_frame_pacing.py` checks presentation times, one completion
per task, native update intervals, overruns and 32-bit timer wraparound.
`tools/tests/test_psp_vsync.py` checks configuration, HOME controls, scanout
ownership, queued/immediate swaps and EDRAM layouts with host SDK stubs.
`tools/tests/test_psp_video_thread.py` runs the real worker and presentation
handoff with host threads, checking logic progress during GE/vblank waits,
ownership fences, VSync capture, repeated submissions and initialization
failure cleanup.

These checks and a PSPSDK build do not replace on-device play testing. Check
movement, aiming, combat, cutscenes, room transitions, pause/HOME and LCD/TV mode
changes on a PSP; measure achieved frame rate in demanding scenes.
