# Ideas and follow-ups (not built)

- **Power switch in the VM window** (Emma). Turn the machine off and on
  without closing the window. The fan code already has both halves:
  `system_audio_power_off()` for the spin-down and `system_audio_reset()`
  for the spin-up. What's missing is a "powered off" state for the machine
  (emulation stopped, window open) and a toolbar/menu toggle.
- **Modem sounds** (plan: out of scope). IBMulator has a full Hayes set.
  The modem files are all mono, and several are 44.1 kHz.
- **Guest close-tray on ATAPI** doesn't work in 86Box at all: START STOP UNIT
  is `CHECK_READY`, so it's refused on an empty drive.
- **ZIP/MO/tape sounds**, fan speed following hardware monitoring (plan: out
  of scope).
