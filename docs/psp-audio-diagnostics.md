# PSP audio diagnostics

The PSP audio backend has an optional live diagnostic stream for PSPLink. It is disabled in normal builds so
`printf` traffic cannot affect release audio timing.

Build it with:

```sh
./psp.sh PSP_AUDIO_DIAGNOSTICS=1
```

or, without the wrapper:

```sh
make -j"$(nproc)" psp-port PSP_AUDIO_DIAGNOSTICS=1
```

The separate output is `build/psp-port-audio-diag/ntsc-1.0/EBOOT.PBP`. Launch that build through PSPLink and
watch its normal stdout console. Diagnostic lines begin with `[audio]`; urgent events begin with `[audio!]`.

## What is reported

The logger runs in its own low-priority `OOT PSP AudioDiag` thread. It samples every 100 ms, prints a snapshot
once per second, and never calls `printf` from `OOT PSP AudioOut` or `OOT PSP AudioGen`.

The first snapshot line reports live execution state:

- `prod`: logical `AudioGen` phase. Important values are `WAIT_ME`, `PREPARE`, `SYNTH`, `SUBMIT`, `TIMER`,
  `IO_BACKOFF`, and `RING_FULL`.
- `out`: logical `AudioOut` phase. `WAIT_HW` is normally healthy: `sceAudio*OutputBlocking` is holding the output
  thread until the hardware needs its next buffer. `PRIME` means it is intentionally submitting silence while
  waiting for enough buffered audio.
- `id`, `k`, `w`, and `p`: PSP thread ID, kernel state, raw kernel wait type, and current priority. A logical phase
  plus `k=WAIT` identifies where a thread was blocked rather than merely using CPU.
- `cpu`: run-clock microseconds consumed by that thread during the preceding reporting window.
- `me`: Media Engine state and progress checkpoint.
- `active` and `pending`: whether ME mixing is enabled and whether an ME job is outstanding.
- `wait`, `waiter`, and `age`: whether an Allegrex thread is currently blocked waiting for the ME, that thread's
  ID, and the current wait duration.

ME progress checkpoints during `me=RUN` are:

| Progress | ME work |
| --- | --- |
| `1` | Invalidating command and sample inputs |
| `2` | Executing the audio mixer command list |
| `3` | Copying generated PCM into the output ring |
| `4` | Writing mixer state back for Allegrex |

`me=IDLE` with progress `4` is normal after a completed job. Progress `256` (`0x100`) means ME boot and VME setup
completed but no job checkpoint has replaced it yet.

The second line reports buffer pressure and one-second event counts. `buf=current/target` includes ring data, an
outstanding ME queue, and frames owned by the audio driver. `ring`, `meq`, and `driver` show those pieces
separately. `underrun`, `err`, `fallback`, and `timeout` should remain zero. `catchup`, `late`, `io`, and `full`
explain why generation did not follow its normal 60 Hz cadence. ME wait `avg`, `last`, and `max_all` are in
microseconds; `max_all` is the worst value since boot. `late` counts producer timer deadlines missed by at least
1 ms.

The third line breaks a completed audio update into average microseconds:

- `wait_me`: Allegrex synchronization with the previous ME job.
- `prepare`: command handling, audio loads, DMA completion, and buffer preparation.
- `synth`: sequence processing plus construction of the mixer command list.
- `seq` and `cmd`: the two measured components of `synth`.
- `submit`: cache publication and submission of the new ME job.
- `abi` and `dma`: average mixer command and sample-DMA counts per update.

Two additional lines profile work inside ME progress `2`:

- `me-job` reports completed ME mixer jobs, command count, average and last job cost, and the most expensive
  opcode in the last and worst jobs. `max_all` is the worst profiled job since boot.
- `me-op` ranks the three opcodes that consumed the most ME time during the reporting window. The percentage is
  that opcode's share of all measured command time; `n` is its call count, `avg` is average cost per call, and
  `max_all` is its slowest single call since boot.

ME command costs use raw `tick` units from the ME hardware counter. Compare ticks and percentages within the same
build; they are deliberately not converted to microseconds because the counter rate is ME-clock dependent. For
example:

```text
[audio] me-job 1s n=36 cmd=14802 avg=811234tick last=790115tick/411cmd max_all=1520031tick/489cmd last_hot=ENVMIXER/401882tick max_hot=ENVMIXER/790441tick
[audio] me-op 1s total=27710102tick top=ENVMIXER 61.8% n=1830 avg=9360 max_all=28120 | RESAMPLE 20.4% n=1220 avg=4635 max_all=12004 | ADPCM 9.7% n=610 avg=4407 max_all=9871
```

The live state and urgent event lines also include `op=NAME#index`. While `me=RUN/2`, this is the command the ME
is executing at the instant the diagnostic thread samples it. `op=IDLE` means the most recent command list has
finished. `snapshot=BUSY` only means the ME was publishing a completed profile at that instant; the logger retries
on the next report.

## Reading a stutter

- An `[audio!] UNDERRUN` with `buf` near zero confirms starvation rather than a bad sample or mixer artifact.
- High `wait_me`, live `wait=1`, and `me=RUN/2` point to ME mixer execution as the bottleneck. Progress `1`, `3`,
  or `4` instead isolates cache input, PCM queueing, or state writeback.
- When `me=RUN/2` is slow, use `me-op` total percentage to choose the first function to optimize. Use `avg` to
  distinguish an intrinsically expensive command from one that dominates only because it is called often, and
  compare `last_hot`/`max_hot` against slow-job or underrun periods.
- High `synth`, with low `wait_me`, points to Allegrex-side sequence or command construction. Compare `seq` and
  `cmd`.
- High `prepare`, a waiting producer kernel state, and elevated `dma` point to sample loading or DMA completion.
- Rising `io` means foreground asset reads are causing the producer's intentional I/O backoff.
- Rising `late`/`maxlate_all` and `catchup` with low phase times point to scheduling starvation elsewhere in the
  game rather than audio work itself.
- Any `cpu` mixes, `ME_FALLBACK`, `ME_TIMEOUT`, `active=0`, or `me=FAULT` means work left the normal ME path.
- `OUTPUT_ERROR` is a PSP audio driver submission failure. `out=WAIT_HW` by itself is expected and is not a
  bottleneck.

Disable the feature for normal timing comparisons by rebuilding without `PSP_AUDIO_DIAGNOSTICS=1`.
