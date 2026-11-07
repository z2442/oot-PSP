# PSP sleep and resume

Flick the power switch normally. The game finishes its current frame, parks
audio, and displays “Preparing to sleep” before allowing the PSP to suspend.
The request can be delayed until startup or a scene transition finishes.

PSP-2000 and later models keep their asset caches in normal RAM. They never
allocate volatile RAM or write a volatile-memory backup, regardless of TV
output or whether the DVE plugin is available. If a normal cache allocation
fails, the cache shrinks or reads assets without caching.

On the PSP-1000 (Phat), live allocations in the 4 MB volatile region are temporarily saved to
`data/segments/oot_psp_suspend.bin` beside the game's asset pack. Leave up to
4 MB free on the Memory Stick. Saving can add a short delay before sleep.
No second copy of these allocations is reserved in the Phat's normal heap.
The Phat's volatile packed read cache is discarded rather than saved. A cache
in normal RAM keeps its contents and lookup metadata across sleep.

Resume recreates every volatile allocation at its original address and checks
the restored bytes before allowing audio or gameplay to continue. Actual live
bytes are backed up because cached audio fonts can contain relocated pointers
and other changes absent from the original asset pack. The temporary backup
is removed after successful recovery. A failed write cancels sleep. A failed
restore leaves the game paused with Cross to retry or Home to quit; reinsert
the same Memory Stick before retrying.

Audio workers acknowledge suspension at update/output boundaries. The Media
Engine finishes pending commands, shuts down its mixer, writes its private
cache back to RAM, and acknowledges HALTED. Its main-CPU semaphores and
completion handler remain allocated; wake reuses and rearms them.

ME suspend follows the reset-based approach from mcidclan's
[sleep-wake sample](https://github.com/mcidclan/psp-media-engine-custom-core/blob/main/samples/sleep-wake/main.cpp),
with one owner for startup. Both firmware hooks assert reset. The wake hook
publishes an uncached generation counter after its final reset write and
leaves the processor stopped. The game waits for that counter to advance
before reserving the audio channel and performing the one full ME boot.
This prevents a late firmware hook from resetting a new worker during setup
or playback. No register context or old interrupt stack is restored. The
firmware event handler and kernel bridge remain installed across wakes.

Recovery waits for the firmware's `RESUME_COMPLETE` notification before
changing clocks or restoring graphics, assets, or audio. `RESUMING` only
marks the start of wake. A hardware trace showed these callbacks 676 ms
apart, with the old audio path timing out during that interval. If completion
is absent for 5 seconds, the game stays paused with retry/quit controls;
retry continues waiting rather than assuming firmware hardware setup is done.

The firmware power callback and ME event hook have separate acknowledgements.
Recovery waits up to 5 seconds for the current ME wake generation; a missing
hook keeps workers parked for retry. An old generation cannot satisfy a later
sleep, and counter wrap is accepted. Cancelled sleep skips this hook wait.

An uncoordinated ME entry waits without touching DSP state. After assets and
audio channels are restored, the coordinator holds the ME in reset, clears
pending command transport, reselects the firmware table, reinstalls the boot
stub and boots the worker through the normal READY handshake. The entry
assembly clears inherited data-cache tags without writing dirty contents
back. It zeros TagLo/TagHi, uses the index invalidation operations for both
ways, and derives the index span from CP0 Config. This runs before the C
worker creates a frame, preserves me-core's original stack pointer, and makes
no C calls. Live game data was published during shutdown; stale cache contents
left across firmware reset must not be written back during cold startup.

The C worker then initializes synthesis-local flags, reverb command buffers
and mixer caches. Cache, synthesis and mixer startup stages have separate
failure messages in regular builds. A hardware report of `p=20 s=0` isolated
the old cold-start writeback call; it has been removed. Shutdown retains full
writeback before HALTED.
The worker initializes directly on entry and publishes READY followed by
IDLE. The CPU waits for both and never changes BOOTING to release the worker.
This follows the examples' startup direction and avoids a second release
handshake. Completion interrupt delivery is armed only after startup finishes.
Setup failures include the sampled progress (`p`, hexadecimal) and state
(`s`, decimal) in the regular retry screen.

The mailbox and wake-generation storage reserve whole 64-byte cache lines.
Alignment of the start alone does not protect the last line: the previous
linked image placed newlib heap globals in the wake counter's trailing line.
Cached writes to those neighbors could overwrite the uncached counter.
The worker retains me-core's original entry and firmware-local eDRAM stack.
Synthesis identifies the processor from the physical stack range, without a
cached execution flag that could become visible to the other processor.

After the boot READY handshake, a harmless ME command must
complete before the audio workers resume. Its interrupt must actually reach
the completion semaphore before later commands trust that semaphore. If
delivery fails, recovery rebuilds the owned main-CPU handler and tests again.
If firmware still cannot deliver completion interrupts, command waits yield
for 1 ms between shared-state checks. Command construction and mixing remain
on the ME. Failed verification keeps audio workers paused, and retries do not
clear a still-running probe until the processor is held in reset.

Normal playback rechecks shared command state every 10 ms during semaphore
waits. Failed semaphore waits yield rather than spinning on the main CPU.
The playback watchdog checks mixer progress every 16 commands: a 250 ms stall
or a 2-second total job limit enters timeout recovery. Wake traces retain the
last fallback's state, progress, elapsed time, and command count, plus the
number of successful hardware boots.

Clocks and audio-channel setup complete before the ME boot and command test;
there is no clock change after releasing the workers. Audio channels are
drained/released and reserved again; the SRC path explicitly drains its final
buffer before release. Preparation failures identify the graphics, audio
worker, ME shutdown/restart, audio drain, or volatile-memory step.

The renderer saves a 2 KB GE context after the final display list completes.
Resume restores that context alongside GU's retained software state, keeping
the existing SDK callbacks, framebuffer rotation, and warm texture cache.
It restores the active display route without applying pending HOME-menu edits
or calling `sceGuInit()` again. Render bindings are invalidated, interpolation
is reset, and presentation starts a fresh timeline. Graphics save failures
cancel sleep; restore failures leave the game paused for retry.

Validation:

- `python3 tools/tests/test_psp_suspend.py` executes the production asset,
  coordinator, audio handshake, and ME entry functions against PSP stubs.
  It covers repeated cycles, partial I/O, damaged/truncated backups, exact
  address allocation failures, failed preparation, DMA drain timeouts, and
  restore retries, warm normal-RAM caches, 40 cold ME boots,
  retained kernel-resource counts, ME synthesis submissions, early-entry
  guards, delayed entry and preemption at ENTERED/READY using the actual worker
  and initializer as coroutines, late firmware hook acknowledgement, stale
  and wrapped generations, held-reset hook behavior, startup cache order,
  READY/reset/firmware-table retries, stalled/faulted
  command verification, lost interrupt delivery, immediate handler rebuild,
  interrupt recovery on later wakes, delivery loss after a successful wake,
  immediate semaphore errors, slow healthy commands, command-wait wakeup budgets, timeout
  behavior, and hardware setup order. It also verifies that later/unknown models cannot allocate
  volatile RAM, including under heap pressure, and retains Phat fallbacks.
  Coordinator cases include a 676 ms delayed completion, combined and
  complete-only callbacks, stale completion flags, missing-callback retry,
  and a completion deadline across the 32-bit timer wrap.
- `python3 tools/tests/test_psp_frame_pacing.py` covers a wake across the
  signed timer comparison window.
- `python3 tools/tests/test_psp_graphics_resume.py` checks 50 simulated cycles
  preserve GE/GU state, SDK resource counts, textures, active mode, and buffer
  addresses. It also checks graphics save failures and restore retries.
- `python3 tools/tests/test_psp_me_cache_layout.py` checks the linked regular
  image, including section boundaries, for cache-line sharing between ME
  storage and unrelated globals. It reproduced the wake counter/newlib heap
  overlap in the previous executable and must pass for the new build. It
  also disassembles the ME entry and checks that it clears both cache tag
  registers, invalidates both ways, and has no writeback, stack access or C
  calls during that reset.
- `make -j4 psp-port` builds the PSP executable.

Real hardware validation is still required: test several sleep/wake cycles
during gameplay with music and sound effects, and from the HOME menu. Check
that music, characters, textures, controls, and frame pacing recover each time.

To diagnose sustained CPU/FPS changes, build with
`make -j4 psp-port PSP_WAKE_DIAGNOSTICS=1`. The release settings remain active;
this uses a separate `build/psp-port-wake-diag/ntsc-1.0/EBOOT.PBP`. Stand still
in one scene for at least 10 seconds, sleep, then stay in the same scene for
at least 10 seconds after waking. The game writes `oot-psp-wake.log` next to
the EBOOT. Copy that file before restarting the game; a new run replaces it
after its first wake.

The log compares a completed 3-second interval before sleep with three
separate 3-second intervals after recovery. It records system CPU usage,
available thread CPU times/priorities/preemptions, completed presentations,
CPU/bus clocks, and ME/interrupt/fallback state. It explicitly marks missing
threads and invalid counter deltas; suspended time is excluded. Snapshots
stay in RAM during normal play. Recovery checkpoints are written and closed
before sleep, after firmware completion, after asset restoration and after ME
restart, so a crash before the first post-wake sample can leave a stage marker.
The three post-wake measurement reports are appended separately. Ordinary builds omit this instrumentation. Host coverage is in
`python3 tools/tests/test_psp_wake_trace.py`.

Version 4 adds read-only hardware snapshots at each interval boundary and
at the actual fallback: CPU/ME/bus divider registers, bus enable, reset, and
Tachyon configuration. It also records the ME's CP0 configuration/status and
timestamps the RESUMING and RESUME_COMPLETE power callbacks. Mixer failures
include the exact command index, opcode, and command words, with a validity
flag if the ME advanced while they were sampled. Diagnostic synthesis
checkpoints distinguish ticks, note setup, and sample processing. These
measurements distinguish late firmware restoration from bad command inputs
without changing the configured clocks or moving audio work to Allegrex.

Version 6 records the worker stack pointer and both processors' synthesis
inputs (ticks, notes, reverbs, frame count, and first reverb downsample rate).
These fields identify mismatched published state and show the worker stack
before and after sleep. Version 8 uses the sample's simple-reset handler and
boots a fresh worker after wake completion. It removes the v7 saved-context
path following an immediate wake-crash report. The subsequent
wake-generation fence and cache ownership fixes are followed by a tag-only
cold-cache reset after the reported writeback stall. Hardware validation is
still required; passing host tests does not establish that the original
slowdown is fixed.
