/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          CD-ROM drive sound emulation.
 */
#ifndef EMU_CDROM_AUDIO_H
#define EMU_CDROM_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CDROM_AUDIO_PROFILE_MAX 32

struct cdrom;

/* (C) Each drive's profile internal_name; "none" (the default) means off. */
extern char cdrom_audio_profile[8][64];

/* Read assets/sounds/cdrom/cdrom_audio_profiles.cfg. Once, at start-up. */
extern void        cdrom_audio_load_profiles(void);
extern int         cdrom_audio_get_profile_count(void);
extern const char *cdrom_audio_get_profile_name(int id);
extern const char *cdrom_audio_get_profile_internal_name(int id);

/* Hard reset and start-up: apply the selected profiles. Discs start stopped
   and spin up on the first access. */
extern void cdrom_audio_reset(void);
/* Emulator exit: stop at once, without playing anything. */
extern void cdrom_audio_close(void);

/* Machine power-off: every spinning disc spins down. The spin-downs keep
   playing after emulation stops. */
extern void cdrom_audio_power_off(void);
/* Milliseconds of spin-down still to play (0 if none). */
extern int cdrom_audio_spin_down_ms_left(void);

/*
 * Hooks, called from cdrom.c (and scsi_cdrom.c for STOP UNIT). All of them
 * do nothing for a drive with no profile.
 */

/* The head goes to `lba`: a data read, an audio play start or a seek.
   Spins the disc up if needed and plays a seek sound if the head jumped.
   Emulation thread only. */
extern void cdrom_audio_access(struct cdrom *dev, uint32_t lba);
/* The guest told the drive to stop the disc (START STOP UNIT, Start = 0). */
extern void cdrom_audio_stop_unit(struct cdrom *dev);
/* Media removed / inserted (the tray opened / closed). Any thread. */
extern void cdrom_audio_tray_open(struct cdrom *dev);
extern void cdrom_audio_tray_close(struct cdrom *dev);

#ifdef __cplusplus
}
#endif

#endif /* EMU_CDROM_AUDIO_H */
