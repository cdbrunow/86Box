# How HDD sounds work in 86Box (`src/disk/hdd_audio.c`)

This is the pattern the fan and CD-ROM sounds will copy. Line numbers are
from `master` at `141ae59` (Nov 2026), so they will drift.

## The big picture

Two threads touch HDD audio:

- **The emulation thread** runs the guest. When the guest moves the drive's
  heads, it calls `hdd_audio_seek()`, which only *notes* that a seek
  happened. It never makes any sound itself.
- **The HDD sound thread** (`sound_hdd_thread`, `src/sound/sound.c` ~L1410)
  wakes about 50 times a second. Each time it asks `hdd_audio_callback()` to
  fill a 20 ms buffer (`SOUNDBUFLEN` = 960 stereo frames at 48 kHz) and hands
  it to the audio backend on its own stream, `I_HDD`.

So the emulator says *what* happened, and the sound thread decides *what it
sounds like* right now.

## Data structures (top of the file)

| Struct | One per | Holds |
|---|---|---|
| `hdd_audio_profile_config_t` (in the header) | profile in `hdd_audio_profiles.cfg` | name, `internal_name`, RPM, and a filename and volume for each of the four sounds |
| `hdd_audio_samples_t` | profile | the decoded WAV data (`int16_t`, interleaved stereo L,R,L,R…) and its length in **frames**. Loaded only when a drive actually uses the profile |
| `hdd_audio_drive_state_t` | configured drive (max 8) | which profile it uses, its spindle state, two play positions, and 8 seek "voices" |
| `hdd_seek_voice_t` | seek being played | `active`, `position` (in frames), `volume` |

A profile's number in the code is its **position in the file**, not its
`id =` field. `hdd[i].audio_profile` stores that position. The config file
saves the `internal_name` instead (`config.c` ~L1905, ~L5012), so
reordering profiles doesn't break saved VMs. Profile 0 must be "None".

## The spindle state machine

```
STOPPED --spinup--> STARTING --start.wav finished--> RUNNING (loop.wav repeats)
   ^                                                    |
   +------- stop.wav finished <------ STOPPING <--spindown
```

- `hdd_audio_spinup_drive()` / `hdd_audio_spindown_drive()` (emulation side)
  only set the state and rewind `spindle_transition_pos` to 0.
- The mix helpers (sound-thread side) play from that position. When a
  one-shot sample runs out, **the sound thread itself** moves the state on
  (STARTING→RUNNING, STOPPING→STOPPED).
- If a profile has no start or stop file, that state is skipped right away.
- `spindle_pos` is the position in the loop and wraps to 0 at the end, so the
  loop sample must end where it begins or you hear a click every lap.

## How a seek turns into a sound

1. `hdd_seek_get_time()` in `hdd.c` (~L248) works out the new cylinder. If it
   changed, it calls `hdd_audio_seek(hdd, new_cylinder)`. The ST-506 XT
   controller (`hdc_st506_xt.c` ~L536, ~L932) calls it directly, because it
   doesn't use that timing helper.
2. `hdd_audio_seek()` finds the drive's state, takes the mutex and:
   - **drops** the seek if another seek started less than `cyl_switch_usec`
     ago (converted to frames), so a burst of track-to-track steps doesn't
     turn into a buzz;
   - otherwise claims a free voice (`active = 1`, `position = 0`). If all 8
     are busy, the seek is silently dropped.
3. On its next pass, the sound thread mixes every active voice and frees it
   when its sample ends.

Every seek plays the same sample at the same volume. Seek distance only
decides *whether* a seek is heard (0 cylinders = no sound), not what it
sounds like.

## Mixing

`hdd_audio_callback()` clears the buffer, then for each drive adds (`+=`)
the spindle sound for its current state, plus its seek voices **only while
the spindle is RUNNING**. Adding is how several drives and voices share one
buffer.

There are two copies of every mix helper:
- **float** (the default, `sound_type = float`): `sample / 131072.0f * volume`.
  131072 is 4 × 32768, so a full-scale sample lands at 0.25. That leaves room
  for several sounds to add up without clipping.
- **int16**: `sample * volume`, clamped to ±32767 *after each addition*.

## Threading and the mutex

`hdd_audio_mutex` protects **only the seek voices**: `hdd_audio_seek()`
takes it to claim a voice, and the sound thread takes it to mix them.
Spinup and spindown also take it to change the state. The spindle mixing and
the state changes the sound thread makes itself don't take it, because only
the sound thread advances those positions.

On pause, `sound_hdd_thread` outputs nothing (`dopause`). When it resumes,
the playback positions are where they stopped, so the loop picks up
seamlessly. The thread paces itself by **wall-clock** time, so sounds play at
normal speed whatever the emulation speed.

## Lifecycle

| When | What runs |
|---|---|
| Start-up (`86box.c` ~L1754) | `hdd_audio_load_profiles()` reads the `.cfg`; `hdd_audio_init()` builds drive states, loads samples, calls spinup on every drive, and starts the sound thread |
| Hard reset (`86box.c` ~L2045) | `hdd_audio_reset()` frees the samples, rebuilds the drive states (so changed settings apply), and spins every drive up again |
| Settings UI (`qt_settingsharddisks.cpp` ~L492) | lists profiles whose `rpm` is 0 or matches the drive's speed preset |

## Things I noticed (no changes made; for your judgement)

1. **Nothing calls `hdd_audio_close()`, `hdd_audio_spindown()` or
   `sound_hdd_thread_end()`.** The thread isn't stopped on exit (the process
   just ends), and the spin-down sample never plays. Phase 3's "stop cleanly
   on close" and soft-off spin-down need a shutdown hook that doesn't exist
   for HDDs either.
2. **The float and int16 paths are 12 dB apart.** Float divides by 4 and
   int16 doesn't, so HDD sounds are 4× louder with *Float32 sound* turned off.
   The floppy code has the same float scaling. Worth checking by ear in both
   modes.
3. **`hdd_audio_reset()` releases the mutex before rebuilding the drive
   list**, while the sound thread may be reading it. In practice this is a
   brief window during a hard reset. The new code shouldn't copy it: hold the
   lock while the shared state changes.
4. **The WAV loader is strict** (`sound_load_wav`, `src/sound/sound_util.c`).
   It needs the plain 44-byte header (`fmt ` chunk of exactly 16 bytes,
   followed directly by `data`) and doesn't check the sample rate. IBMulator's
   `system/power_down.wav` has an 18-byte `fmt ` chunk and would be rejected,
   so the prep script must always write a clean header. If the output rate is
   set to 44.1 kHz, every 48 kHz sample also plays about 8% slow and flat.
