# The ambient sound stream (`I_AMBIENT`)

Phase 2 adds one audio output stream that every "ambient" sound shares: the
system fan in Phase 3, the CD-ROM drive in Phase 4, and anything later.
Each sound is a **source** that registers a render function. One thread
mixes them all.

Why one shared stream: every stream needs its own entry in four audio
backends, a thread and a few tables. Adding the stream once means the fan and
the CD-ROM only have to write their sound, not plumbing.

## The pieces

| File | What it does |
|---|---|
| `include/86box/sound.h` | `I_AMBIENT` stream number, `givealbuffer_ambient()` |
| `sound/ambient.c` + `include/86box/ambient.h` | the source registry and the mixer |
| `sound/sound.c` | `sound_ambient_thread()`, the 50-times-a-second render loop |
| `sound/openal.c`, `xaudio2.c`, `sndio.c`, `audio4.c` | give the stream the same rate and buffer size as `I_HDD` |
| `automation.c` | names the stream `"ambient"` for the automation recorder |
| `86box.c` | `ambient_close()` in `pc_close()` |

`I_AMBIENT` sits **before `I_MIDI`**, not just before `I_MAX` as the plan
said. Every backend creates `I_MIDI + 1` sources when MIDI is on and
`I_MIDI` when it's off, so it treats MIDI as the last, optional stream.
Anything placed after MIDI would never get an output.

## How a block of sound is made

```
sound_ambient_thread (sound.c)         every 20 ms
  └─ ambient_render(buf, frames, paused)   ambient.c
       ├─ zero buf
       ├─ lock ambient_mutex
       │    └─ for each source: source->render(buf, frames, paused, priv)
       │         (adds into buf; returns 1 if it must be heard while paused)
       ├─ unlock
       └─ clamp to -1.0 ... +1.0
  └─ paused and nobody asked to be heard? → output nothing, sleep 50 ms
  └─ convert to int16 if Float32 sound is off
  └─ givealbuffer_ambient(...)         to the backend
```

Sources always work in float. The thread converts to int16 when needed, so
no source needs two copies of its mixing code (`hdd_audio.c` has a float and
an int16 copy of everything). To sit at the same level as drive sounds, a
source scales a full-scale 16-bit sample to 0.25 (`sample / 131072.0f`),
just as the floppy and HDD code do.

## Threading rules

There are only two threads involved:

- the **emulation thread** (or the main thread at start-up and exit), which
  registers and unregisters sources and changes their state;
- the **ambient sound thread**, which calls the render functions.

One mutex, `ambient_mutex`, protects the registry, and the sound thread holds
it for the whole time it calls the render functions. That gives two
guarantees the later phases rely on:

1. **When `ambient_unregister_source()` returns, the source is not running
   and never will be again**, so its state can be freed straight away.
   Taking the lock waits out a render in progress.
2. **A render function runs with the lock held**, so a source can share the
   same mutex for its own state through `ambient_lock()` /
   `ambient_unlock()`. The fan (Phase 3) does exactly that, which avoids the
   kind of race that crashed `hdd_audio_reset()`.

The one rule for sources: **never call `ambient_register_source()` or
`ambient_unregister_source()` from a render function or while holding
`ambient_lock()`.** The mutex isn't recursive, so it would deadlock.

## Start, stop, pause

- **No sources, no thread.** The first `ambient_register_source()` starts
  the thread, and the last `ambient_unregister_source()` stops it. With
  every ambient sound off, the feature costs nothing at all.
- The thread is started and stopped **outside** the lock. Stopping waits for
  the thread to exit, and the thread takes the lock on every pass, so
  stopping while holding it would deadlock.
- **Pause:** like the HDD thread, it outputs nothing while `dopause` is set.
  Sources keep their positions, so a fan loop resumes mid-loop. The one
  exception: a source can return nonzero from its render function to be
  heard anyway. *(Added in Phase 3: powering off stops emulation, which sets
  `dopause`, and the fan's spin-down has to play after that.)*
- **Timing** is wall-clock (`plat_get_ticks()`), so fast-forward doesn't
  speed sounds up. Phase 4's idle timer will use emulated time instead.
- **Exit:** `pc_close()` calls `ambient_close()`, which drops every source and
  stops the thread before the audio backend is closed.

## Volume control

FDD and HDD sounds have no separate volume slider. The Sound Gain dialog
only sets the master `sound_gain`. Following the plan, the ambient stream
doesn't get one either. A per-stream mixer would be its own feature.

## How it was tested

- **ThreadSanitizer harness** (outside the repo): the real `ambient.c`, with a
  render thread standing in for `sound_ambient_thread`. The main thread
  registered three sources and then unregistered and **freed** them, 3,000
  times over. TSan reported nothing, and the thread started and stopped 3,000
  times. Duplicate and over-full registrations are refused, and 16 sources at
  0.5 clamp to 1.0.
- **Test tone** (`-DAMBIENT_TEST`, see below): captured output measures
  440 Hz at −26 dBFS, as written.
- **Full build** with and without `AMBIENT_TEST`.
- **Not tested:** hearing it inside a running VM. That needs a ROM set,
  which this session doesn't have.

## Listening test (for you)

```sh
cmake -S . -B build-test --preset regular -D QT=ON -D USE_QT6=ON \
      -D CMAKE_C_FLAGS=-DAMBIENT_TEST      # plus -D SDL2=ON if needed
cmake --build build-test -j"$(nproc)"
```

Start any VM:
- You should hear a quiet, steady 440 Hz tone (an A), starting when the VM
  starts.
- It should stop while the VM is paused and pick up again on resume.
- Floppy and HDD sounds should play exactly as before.

The last commit on this branch adds the test tone. **Drop it before
merging.** Without `-DAMBIENT_TEST` it isn't compiled at all.
