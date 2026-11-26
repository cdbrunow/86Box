# CD-ROM sound: audit of the existing code (Phase 4.1)

Read-only survey of `master` at `141ae59`, to decide where CD-ROM drive
sounds can hook in. Line numbers will drift.

**Short version:** the plan assumed seeks go through `cdrom_seek()` /
`seek_diff`. They don't, consistently. But every data read and every
audio-play start *does* go through one of a few functions in `cdrom.c`.
So the sound module can track the head position itself and work out seeks
on its own, and every hook can live in `cdrom.c` except one (the guest's
"stop the disc" command).

## 1. How each interface seeks

| Interface | File | How it moves the head | Sets `seek_diff`? |
|---|---|---|---|
| ATAPI and SCSI | `scsi/scsi_cdrom.c` | `scsi_cdrom_calculate_seek_diff()` (~L1800) before reads/plays (about 15 call sites); `cdrom_seek()` for SEEK (~L5555) and REZERO | yes |
| Mitsumi | `cdrom_mitsumi.c` | computes `seek_diff` itself (~L638); `cdrom_seek()` (~L550, ~L580) | yes |
| Panasonic/MKE | `cdrom_mke.c` | `cdrom_seek()` (~L837, ~L1218, ~L1240, ~L1444, ~L1832) | no (uses whatever is left in `seek_diff` for timing) |
| Hitachi | `cdrom_hitachi.c` | sets `seek_pos` directly (~L124); `cdrom_seek()` (~L218) | no |
| Philips | `cdrom_philips.c` | sets `seek_pos` directly (~L231); `cdrom_seek()` (~L470, ~L669, ~L702) | no |
| Sony | `cdrom_sony.c` | sets `seek_pos` directly (~L213, ~L455) | no |
| CM153 | `cdrom_cm153.c` | sets `seek_pos` directly (~L61) | no |
| Red Book audio (all) | `cdrom.c` `cdrom_audio_play()` (~L2026) | sets `seek_diff` and `seek_pos` | yes |

`cdrom_seek()` itself (~L1811) only stops audio and records the target. It
doesn't compute `seek_diff`. So hooking `cdrom_seek()` or reading
`seek_diff` would miss Sony, CM153 and some Hitachi/Philips paths, and
would get stale distances on MKE.

**What every interface does share:**
- **Data reads:** each driver calls `cdrom_readsector_raw()` (`cdrom.c`
  ~L3094) for every sector it reads: one call site in each of
  `scsi_cdrom.c`, Mitsumi, Hitachi, Philips, Sony and CM153, and five in
  MKE.
- **Audio playback:** starts in `cdrom_audio_play()`,
  `cdrom_audio_play_pioneer()`, `cdrom_audio_play_toshiba()` or
  `cdrom_audio_scan()`, all in `cdrom.c`. It advances in
  `cdrom_audio_callback()` (~L1867).

**Proposal:** the sound module keeps its own "head at LBA x" per drive.
Every read or play start reports its LBA. The distance from the last one
is the seek, and sequential reads (distance 1) are silent. This is
interface-independent, needs no changes to any driver, and also catches
SEEK commands (`cdrom_seek()` reports its target).

## 2. Tray and media

| Event | Where | Who |
|---|---|---|
| Media removed (tray opens) | `cdrom_exit()` (~L4269) → `cdrom_unload()` | every path: host-menu eject (`cdrom_eject()` from `qt_mediamenu.cpp` ~L958), host image swap (`cdromMount()` ~L856 calls `cdrom_exit()` first), guest eject (START STOP UNIT `LoEj` ~L5370, other SCSI vendor commands ~L2137/2866/3098/3351/3857, Mitsumi ~L1420, MKE ~L1133/1395, Sony ~L396), automation |
| Media inserted (tray closes) | `cdrom_load()` (~L3938) | host mount (`cdromMount()` ~L866), guest close-tray `cdrom_reload()` (~L4339), automation |
| Image loaded at start-up / hard reset | `cdrom_hard_reset()` → `cdrom_load()` (~L4208) | must **not** play a tray sound |

So the tray-open sound can hook `cdrom_exit()` and the tray-close sound
`cdrom_load()`, with start-up loads skipped. A host image swap
(`cdromMount()`) becomes open followed by close, which is what a real
swap sounds like.

## 3. Spin state and idle timeout

**There is none.** `cd_status` (`cdrom.h` ~L30) says whether audio is
playing, paused or stopped, or what kind of media is inserted. Nothing
models the motor or an idle timeout.

`GPCMD_START_STOP_UNIT` (`scsi_cdrom.c` ~L5353):

| `cdb[4] & 3` | Does now | Sound |
|---|---|---|
| 0 stop | `scsi_cdrom_stop()` → `cdrom_stop()`: stops audio only | `spin_stop` (needs a hook **here**, the one hook outside `cdrom.c`) |
| 1 start | re-reads the TOC | `spin_start` if stopped |
| 2 eject | stop, then `cdrom_eject()` | covered by the `cdrom_exit()` hook |
| 3 load | `cdrom_reload()` | covered by the `cdrom_load()` hook |

`cdrom_stop()` can't be the spin-down hook: `cdrom_seek()` calls it on
every seek, just to stop audio. Proprietary interfaces have no equivalent
of "stop the disc" in their emulation, so for them spin-down comes only
from the idle timer.

## 4. Red Book audio and drive state

Playback state is `cd_status == CD_STATUS_PLAYING`. The CD audio thread
pulls sectors through `cdrom_audio_callback()`, which advances `seek_pos`
sequentially. While it plays, the idle timer must not run, so the drive
keeps spinning. A paused track (`CD_STATUS_PAUSED`) counts as idle: real
drives spin down after a while on pause too.

## 5. Existing tests

`tests/cdrom/` has GoogleTest suites for Hitachi, Mitsumi, MKE, Philips,
Sony, image TOC/MCN and CD burning (about 150 tests), plus a Mitsumi
benchmark. They build with `-D BUILD_TESTING=ON` (needs `libgtest-dev`)
and run with `ctest --test-dir build -R <suite> --output-on-failure`.

They compile `cdrom.c` with `-DIS_TEST` and without the sound system. So
the new hooks in `cdrom.c` must compile there: either behind
`#ifndef IS_TEST`, or by adding `cdrom_audio.c` stubs to the tests'
sources. I'll get a baseline run before changing anything.

## Revised behaviour (draft for your review)

| Event | Sound |
|---|---|
| Read or audio play while stopped (and a disc is in) | `spin_start`, then loop `spin` |
| Seek (LBA jump between accesses) | distance < threshold → `seek_step`; longer → `seek_out` (toward the rim, LBA up) or `seek_in` (toward the hub). Threshold is a profile field |
| No access and no audio playing for N seconds | `spin_stop`, then silence. N measured in **emulated** time |
| Guest START STOP UNIT stop | `spin_stop` |
| Media removed (any path) | `open_tray`; stops the spin with no spin-down sound (the tray mechanism stops the disc) |
| Media inserted (not at start-up) | `close_tray`; the spin starts on the next access, as real drives do |
| No disc | no spin or seek sounds; tray sounds still play |
| Pause / fast-forward | same as the other ambient sounds: silent while paused; samples play at normal speed |

Per drive, up to 8 drives. Config: `cdrom_NN_audio_profile` on the
Floppy & CD-ROM page, default none.

## Questions for you

1. **Idle spin-down time (N):** the plan suggests about 30 s. Real drives
   of the era often used 2–5 minutes (Windows could also set it). I
   suggest **60 s** as the default, as a field in the profile file so it
   can differ per drive.
2. **Seek threshold** for "step" vs "long seek": I suggest **2,000 LBAs**
   (about 27 s of audio, a few tracks) as the default profile value.
3. **Head tracking instead of `seek_diff`** (section 1): OK to go with this?
