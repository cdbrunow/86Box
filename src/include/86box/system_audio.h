/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          System fan / power supply sound emulation.
 */
#ifndef EMU_SYSTEM_AUDIO_H
#define EMU_SYSTEM_AUDIO_H

#ifdef __cplusplus
extern "C" {
#endif

#define SYSTEM_AUDIO_PROFILE_MAX 32

/* (C) Selected profile's internal_name; "none" (the default) means off. */
extern char system_fan_audio_profile[64];

/* Read assets/sounds/system/system_audio_profiles.cfg. Once, at start-up. */
extern void        system_audio_load_profiles(void);
extern int         system_audio_get_profile_count(void);
extern const char *system_audio_get_profile_name(int id);
extern const char *system_audio_get_profile_internal_name(int id);

/* Cold power-on (start-up and hard reset): apply the selected profile, then
   play the spin-up followed by the running loop. */
extern void system_audio_reset(void);

/* Guest soft power-off: start the spin-down. Returns how many milliseconds
   it needs to finish playing (0 if there is nothing to play). */
extern int system_audio_power_off(void);

/* Milliseconds of spin-down still to play (0 if none is playing). The Qt
   frontend waits this long before quitting after a power-off. */
extern int system_audio_spin_down_ms_left(void);

/* Emulator exit: stop at once, without playing anything. */
extern void system_audio_close(void);

#ifdef __cplusplus
}
#endif

#endif /* EMU_SYSTEM_AUDIO_H */
