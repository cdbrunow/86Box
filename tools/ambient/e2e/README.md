# End-to-end tests for the ambient sounds

These drive a real 86Box VM through its automation socket, record the
`ambient` stream to WAV, and find each real sample in the recording by
cross-correlation, so they check timing and order as well as levels. No
guest OS is needed: the CD-ROM tests talk to the drive with raw port I/O.

Needs: an 86Box built with `-D AUTOMATION=ON`, a ROM set, the assets fork,
Python 3 with numpy, and (on a headless machine) `xvfb-run`.

```sh
python3 -I fan_e2e.py        <86Box> <roms> <assets> <workdir>   # system fan
python3 -I cd_atapi_e2e.py   <86Box> <roms> <assets> <workdir>   # ATAPI CD-ROM
python3 -I cd_mitsumi_e2e.py <86Box> <roms> <assets> <workdir>   # Mitsumi CD-ROM
```

Each prints the commands it ran and then the sounds it found, in order, with
a correlation `r` (1.000 = the exact sample). A sound that overlaps a louder
one can score lower or go unmatched, which is why the scripts leave time
between events.

Known emulator behaviour the scripts work around (not sound bugs):

- ATAPI answers the first command after a disc change with UNIT ATTENTION,
  and Mitsumi refuses reads for about a second after a disc change. The
  scripts clear or retry, as a driver would.
- 86Box refuses START STOP UNIT (load) on an empty drive (`CHECK_READY` in
  `scsi_cdrom.c`), so an ATAPI guest can't close the tray after ejecting.
  Mitsumi's CLOSE works and is tested instead.
