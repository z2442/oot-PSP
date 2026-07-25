# PSP audio diagnostics

The PSP audio backend has an optional live diagnostic stream for PSPLink. It is disabled in normal builds so
`printf` traffic cannot affect release audio timing.

Build it with:

```sh
./psp.sh PSP_AUDIO_DIAGNOSTICS=1
```

or:

```sh
make -j"$(nproc)" psp-port PSP_AUDIO_DIAGNOSTICS=1
```

The separate output is `build/psp-port-audio-diag/ntsc-1.0/EBOOT.PBP`. Diagnostic lines begin with `[audio]`;
urgent events begin with `[audio!]`.

## Execution model

`OOT PSP AudioGen` builds and executes each ABI command list synchronously on Allegrex. The hot `MIXER`,
`ENVMIXER`, and `ADDMIXER` loops use four-wide PSP VFPU kernels. The Media Engine is not booted, no ME command
queue is created, and audio has no cross-processor cache handoff or ME wait phase.

The separate `OOT PSP AudioOut` thread blocks in the PSP audio driver while hardware consumes the current
buffer. The producer and main game threads are both created with VFPU context support, so the kernel preserves
vector state across thread switches.

## What is reported

The low-priority logger samples every 100 ms and prints a snapshot once per second. The useful fields are:

- `prod`: the producer phase (`PREPARE`, `SYNTH`, `SUBMIT`, `TIMER`, `IO_BACKOFF`, or `RING_FULL`).
- `out`: the output phase. `WAIT_HW` is healthy; the output thread is waiting for the audio device.
- `cpu`: run-clock microseconds consumed by the producer or output thread during the reporting window.
- `buf`, `ring`, and `driver`: queued source frames and frames currently owned by the audio driver.
- `underrun` and `err`: starvation and driver-submit failures; both should remain zero.
- `catchup`, `late`, `io`, and `full`: scheduling pressure, timer lateness, asset-I/O backoff, and a full ring.
- `prepare`: command handling, loads, DMA completion, and buffer preparation.
- `synth`: sequence processing plus ABI command-list construction.
- `submit`: synchronous Allegrex/VFPU command execution and the PCM ring copy.
- `abi` and `dma`: average ABI-command and sample-DMA counts per update.

Legacy `me`, `meq`, `wait_me`, fallback, and timeout fields remain zero so older PSPLink parsers can consume the
same line format. They do not represent an active Media Engine path.

## Reading a stutter

- `[audio!] UNDERRUN` with `buf` near zero confirms starvation rather than a mixer artifact.
- High `submit` points to Allegrex/VFPU mixer execution; high `synth` points to sequence or command construction.
- High `prepare` with elevated `dma` points to sample loading or DMA completion.
- Rising `io` means foreground asset reads are triggering intentional producer backoff.
- Rising `late`/`maxlate_all` and `catchup` with low phase costs point to scheduling starvation elsewhere.
- `OUTPUT_ERROR` is a PSP audio-driver failure. `out=WAIT_HW` by itself is expected.

Disable diagnostics for normal timing comparisons by rebuilding without `PSP_AUDIO_DIAGNOSTICS=1`.
