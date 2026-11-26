/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          CD-ROM drive sound emulation.
 *
 *          One source on the shared ambient stream (ambient.c) mixes every
 *          drive that has a profile. Per drive:
 *
 *            first access with a disc in -> spin_start, then the spin loop
 *            head jumps between accesses -> seek_step (short), or seek_out /
 *                                           seek_in (long, by direction)
 *            idle for idle_timeout s     -> spin_stop (emulated time; Red
 *                                           Book audio playing counts as
 *                                           busy)
 *            guest STOP UNIT             -> spin_stop
 *            media removed / inserted    -> open_tray / close_tray; removing
 *                                           the disc also stops the spin
 *
 *          The head position is tracked here, from the LBAs cdrom.c reports,
 *          rather than taken from cdrom_t's seek_diff: not every interface
 *          sets seek_diff (see docs/ambient/CDROM_AUDIT.md).
 *
 *          Everything the render function reads is guarded by the ambient
 *          lock (ambient_lock()). The idle bookkeeping (accessed,
 *          idle_seconds, the timer) belongs to the emulation thread.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <86box/86box.h>
#include <86box/plat_unused.h>
#include <86box/ini.h>
#include <86box/mem.h>
#include <86box/rom.h>
#include <86box/timer.h>
#include <86box/cdrom.h>
#include <86box/sound.h>
#include <86box/sound_util.h>
#include <86box/ambient.h>
#include <86box/cdrom_audio.h>

#define DRIVES 8 /* must match CDROM_NUM, checked below */
typedef char drives_matches_cdrom_num[(DRIVES == CDROM_NUM) ? 1 : -1];

/* One-shot sounds that can overlap per drive (seeks and tray) */
#define VOICES 4

/* Don't start a new seek sound while another started less than this many
   frames ago: a burst of short seeks would otherwise become a buzz */
#define SEEK_SPACING (48000 * 40 / 1000)

/* The idle timer ticks once a second of emulated time */
#define IDLE_TICK_US 1000000.0

enum {
    S_SPIN_START,
    S_SPIN,
    S_SPIN_STOP,
    S_SEEK_STEP,
    S_SEEK_OUT,
    S_SEEK_IN,
    S_OPEN_TRAY,
    S_CLOSE_TRAY,
    S_COUNT
};

/* The profile file's key for each sound: <key>_file, <key>_volume */
static const char *sample_keys[S_COUNT] = {
    "spin_start", "spin", "spin_stop", "seek_step", "seek_out", "seek_in", "open_tray", "close_tray"
};

typedef struct {
    char     name[128];
    char     internal_name[64];
    char     file[S_COUNT][512];
    float    volume[S_COUNT];
    uint32_t seek_threshold; /* sectors */
    int      idle_timeout;   /* seconds */
} profile_t;

/* A decoded sample: interleaved stereo, `frames` frames long */
typedef struct {
    int16_t *buffer;
    int      frames;
    float    volume;
} sample_t;

typedef enum {
    SPIN_STOPPED = 0,
    SPIN_STARTING, /* playing spin_start */
    SPIN_RUNNING,  /* repeating spin */
    SPIN_STOPPING  /* playing spin_stop */
} spin_t;

typedef struct {
    const sample_t *sample; /* NULL = free */
    int             pos;    /* negative: frames of silence before it starts */
    int             is_seek;
} voice_t;

typedef struct {
    int id;

    /* Guarded by the ambient lock */
    int      profile; /* 0 = no sound */
    spin_t   spin;
    int      spin_pos;
    voice_t  voices[VOICES];
    int64_t  head; /* last LBA accessed; -1 = unknown */

    /* Emulation thread only */
    int        accessed;     /* any access since the last idle tick */
    int        idle_seconds; /* consecutive idle ticks */
    int        timer_running;
    pc_timer_t idle_timer;
} drive_t;

char cdrom_audio_profile[DRIVES][64] = { "none", "none", "none", "none", "none", "none", "none", "none" };

static profile_t profiles[CDROM_AUDIO_PROFILE_MAX];
static int       profile_count = 0;

/* Decoded samples, per profile, loaded only for profiles in use */
static sample_t samples[CDROM_AUDIO_PROFILE_MAX][S_COUNT];

static drive_t drives[DRIVES];
static int     registered = 0;

#ifdef ENABLE_CDROM_AUDIO_LOG
int cdrom_audio_do_log = ENABLE_CDROM_AUDIO_LOG;

static void
cdrom_audio_log(const char *fmt, ...)
{
    va_list ap;

    if (cdrom_audio_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define cdrom_audio_log(fmt, ...)
#endif

/* ------------------------------------------------------------------------ */
/* Profiles                                                                 */
/* ------------------------------------------------------------------------ */

void
cdrom_audio_load_profiles(void)
{
    const char *expected     = "cdrom_audio_profiles.cfg";
    char        cfg_fn[1024] = { 0 };

    profile_count = 0;

    if (!asset_getfile("assets/sounds/cdrom/cdrom_audio_profiles.cfg", cfg_fn, sizeof(cfg_fn)))
        return;

    /* Same checks as hdd_audio_load_profiles(): a path from the asset
       search paths, with no "..", ending in the expected name. */
    size_t len = strlen(cfg_fn);
    if ((strstr(cfg_fn, "..") != NULL) || (len < strlen(expected)) || strcmp(cfg_fn + len - strlen(expected), expected) != 0)
        return;

    ini_t ini = ini_read_ex(cfg_fn, 1); /* lgtm[cpp/path-injection] */
    if (ini == NULL)
        return;

    for (int i = 0; i < CDROM_AUDIO_PROFILE_MAX; i++) {
        char section[32];
        snprintf(section, sizeof(section), "Profile \"%d\"", i);

        ini_section_t cat = ini_find_section(ini, section);
        if (cat == NULL)
            continue;

        profile_t *p = &profiles[profile_count];
        memset(p, 0, sizeof(*p));
        strncpy(p->name, ini_section_get_string(cat, "name", "Unknown"), sizeof(p->name) - 1);
        strncpy(p->internal_name, ini_section_get_string(cat, "internal_name", "unknown"), sizeof(p->internal_name) - 1);
        for (int s = 0; s < S_COUNT; s++) {
            char key[64];
            snprintf(key, sizeof(key), "%s_file", sample_keys[s]);
            strncpy(p->file[s], ini_section_get_string(cat, key, ""), sizeof(p->file[s]) - 1);
            snprintf(key, sizeof(key), "%s_volume", sample_keys[s]);
            p->volume[s] = (float) ini_section_get_double(cat, key, 1.0);
        }
        p->seek_threshold = (uint32_t) ini_section_get_int(cat, "seek_threshold", 2000);
        p->idle_timeout   = ini_section_get_int(cat, "idle_timeout", 60);
        if (p->idle_timeout < 1)
            p->idle_timeout = 1;

        cdrom_audio_log("CD-ROM audio: profile %d: %s (%s)\n", profile_count, p->name, p->internal_name);
        profile_count++;
    }

    ini_close(ini);
}

int
cdrom_audio_get_profile_count(void)
{
    return profile_count;
}

const char *
cdrom_audio_get_profile_name(int id)
{
    return ((id >= 0) && (id < profile_count)) ? profiles[id].name : NULL;
}

const char *
cdrom_audio_get_profile_internal_name(int id)
{
    return ((id >= 0) && (id < profile_count)) ? profiles[id].internal_name : NULL;
}

/* Index of the profile with this internal name; 0 (none) if not found */
static int
find_profile(const char *internal_name)
{
    for (int i = 1; i < profile_count; i++) {
        if (!strcmp(profiles[i].internal_name, internal_name))
            return i;
    }
    return 0;
}

static void
load_samples(int id)
{
    for (int s = 0; s < S_COUNT; s++) {
        sample_t *smp = &samples[id][s];

        if (smp->buffer != NULL)
            continue; /* already loaded for another drive */

        smp->volume = profiles[id].volume[s];
        smp->frames = 0;
        if (profiles[id].file[s][0] != '\0')
            smp->buffer = sound_load_wav(profiles[id].file[s], &smp->frames);
        if (smp->buffer == NULL) {
            smp->frames = 0;
            cdrom_audio_log("CD-ROM audio: no %s sound for %s\n", sample_keys[s], profiles[id].name);
        }
    }
}

static void
free_all_samples(void)
{
    for (int id = 0; id < CDROM_AUDIO_PROFILE_MAX; id++) {
        for (int s = 0; s < S_COUNT; s++) {
            free(samples[id][s].buffer);
            samples[id][s].buffer = NULL;
            samples[id][s].frames = 0;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Rendering (ambient thread, ambient lock held)                            */
/* ------------------------------------------------------------------------ */

/* Add up to `frames` frames of `s` from *pos into `buf`. Advances *pos and
   returns the number of frames used (fewer when the sample ends). A negative
   *pos is a delay: that many frames pass in silence first. */
static int
mix(const sample_t *s, int *pos, float *buf, int frames)
{
    /* full scale -> 0.25, like the drive sounds; then the user's CD-ROM level */
    const float scale = s->volume * ((float) cdrom_sound_gain / 100.0f) / 131072.0f;
    int         wait  = 0;

    if (*pos < 0) {
        wait = (-*pos < frames) ? -*pos : frames;
        *pos += wait;
        buf += wait * 2;
        frames -= wait;
    }

    int n = s->frames - *pos;

    if (n > frames)
        n = frames;
    if (n <= 0)
        return wait;

    const int16_t *src = &s->buffer[*pos * 2];
    for (int i = 0; i < n * 2; i++)
        buf[i] += (float) src[i] * scale;

    *pos += n;
    return wait + n;
}

static void
render_spin(drive_t *d, float *buf, int frames)
{
    const sample_t *smp = samples[d->profile];
    int             done = 0;

    /* Fill the whole block, moving through the states as samples end, so
       the hand-off from spin-up to loop leaves no gap */
    while ((done < frames) && (d->spin != SPIN_STOPPED)) {
        float *out  = &buf[done * 2];
        int    left = frames - done;

        switch (d->spin) {
            case SPIN_STARTING:
                done += mix(&smp[S_SPIN_START], &d->spin_pos, out, left);
                if (d->spin_pos >= smp[S_SPIN_START].frames) {
                    d->spin     = SPIN_RUNNING;
                    d->spin_pos = 0;
                }
                break;

            case SPIN_RUNNING:
                if (smp[S_SPIN].frames == 0)
                    return; /* no loop sample */
                done += mix(&smp[S_SPIN], &d->spin_pos, out, left);
                if (d->spin_pos >= smp[S_SPIN].frames)
                    d->spin_pos = 0;
                break;

            case SPIN_STOPPING:
                done += mix(&smp[S_SPIN_STOP], &d->spin_pos, out, left);
                if (d->spin_pos >= smp[S_SPIN_STOP].frames) {
                    d->spin     = SPIN_STOPPED;
                    d->spin_pos = 0;
                }
                break;

            default:
                break;
        }
    }
}

static int
cdrom_audio_render(float *buf, int frames, int paused, UNUSED(void *priv))
{
    /* Paused: silent, and nothing advances, so sounds resume where they were.
       The exception is a spin-down: a power-off stops emulation first, and
       the discs then spin down (see cdrom_audio_power_off()). */
    if (paused) {
        int audible = 0;

        for (int i = 0; i < DRIVES; i++) {
            if ((drives[i].profile != 0) && (drives[i].spin == SPIN_STOPPING)) {
                render_spin(&drives[i], buf, frames);
                audible = 1;
            }
        }
        return audible;
    }

    for (int i = 0; i < DRIVES; i++) {
        drive_t *d = &drives[i];

        if (d->profile == 0)
            continue;

        render_spin(d, buf, frames);

        for (int v = 0; v < VOICES; v++) {
            voice_t *voice = &d->voices[v];

            if (voice->sample == NULL)
                continue;
            mix(voice->sample, &voice->pos, buf, frames);
            if (voice->pos >= voice->sample->frames)
                voice->sample = NULL;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------------ */
/* Hooks                                                                    */
/* ------------------------------------------------------------------------ */

/* Start a one-shot sound on a free voice, after `delay` frames (ambient lock
   held). Dropped if the sample is missing or every voice is busy. */
static void
play_after(drive_t *d, int s, int delay)
{
    const sample_t *smp = &samples[d->profile][s];

    if (smp->frames == 0)
        return;

    for (int v = 0; v < VOICES; v++) {
        if (d->voices[v].sample == NULL) {
            d->voices[v].sample  = smp;
            d->voices[v].pos     = -delay;
            d->voices[v].is_seek = (s == S_SEEK_STEP) || (s == S_SEEK_OUT) || (s == S_SEEK_IN);
            return;
        }
    }
}

static void
play(drive_t *d, int s)
{
    play_after(d, s, 0);
}

/* Is a seek sound that started less than SEEK_SPACING ago still playing? */
static int
seek_busy(const drive_t *d)
{
    for (int v = 0; v < VOICES; v++) {
        if ((d->voices[v].sample != NULL) && d->voices[v].is_seek && (d->voices[v].pos < SEEK_SPACING))
            return 1;
    }
    return 0;
}

static drive_t *
drive_of(const cdrom_t *dev)
{
    if ((dev == NULL) || (dev->id >= DRIVES))
        return NULL;
    return &drives[dev->id];
}

static int
has_disc(const cdrom_t *dev)
{
    return (dev->image_path[0] != '\0') && (dev->cd_status != CD_STATUS_EMPTY);
}

/* Once a second of emulated time while the disc spins (emulation thread) */
static void
idle_tick(void *priv)
{
    drive_t       *d       = (drive_t *) priv;
    const cdrom_t *dev     = &cdrom[d->id];
    const int      playing = (dev->cd_status == CD_STATUS_PLAYING);
    int            spinning;

    ambient_lock();
    spinning = (d->profile != 0) && ((d->spin == SPIN_STARTING) || (d->spin == SPIN_RUNNING));
    ambient_unlock();

    if (!spinning) {
        d->timer_running = 0; /* stopped some other way: tray, STOP UNIT */
        return;
    }

    if (d->accessed || playing)
        d->idle_seconds = 0;
    else
        d->idle_seconds++;
    d->accessed = 0;

    if (d->idle_seconds >= profiles[d->profile].idle_timeout) {
        ambient_lock();
        d->spin     = SPIN_STOPPING;
        d->spin_pos = 0;
        ambient_unlock();
        d->timer_running = 0;
        cdrom_audio_log("CD-ROM audio: drive %d idle, spinning down\n", d->id);
        return;
    }

    timer_on_auto(&d->idle_timer, IDLE_TICK_US);
}

void
cdrom_audio_access(cdrom_t *dev, uint32_t lba)
{
    drive_t *d = drive_of(dev);
    int      spin_up;

    if ((d == NULL) || (d->profile == 0) || !has_disc(dev))
        return;

    d->accessed = 1;

    ambient_lock();
    spin_up = (d->spin == SPIN_STOPPED) || (d->spin == SPIN_STOPPING);
    if (spin_up) {
        d->spin     = SPIN_STARTING;
        d->spin_pos = 0;
    }

    /* A jump of more than one sector is a seek. Sequential reads are silent. */
    if ((d->head >= 0) && ((int64_t) lba != d->head) && ((int64_t) lba != d->head + 1) && !seek_busy(d)) {
        const int64_t dist = ((int64_t) lba > d->head) ? ((int64_t) lba - d->head) : (d->head - (int64_t) lba);

        if (dist < (int64_t) profiles[d->profile].seek_threshold)
            play(d, S_SEEK_STEP);
        else
            play(d, ((int64_t) lba > d->head) ? S_SEEK_OUT : S_SEEK_IN);
    }
    d->head = lba;
    ambient_unlock();

    if (!d->timer_running) {
        d->idle_seconds  = 0;
        d->timer_running = 1;
        timer_on_auto(&d->idle_timer, IDLE_TICK_US);
    }
}

void
cdrom_audio_stop_unit(cdrom_t *dev)
{
    drive_t *d = drive_of(dev);

    if (d == NULL)
        return;

    ambient_lock();
    if ((d->profile != 0) && ((d->spin == SPIN_STARTING) || (d->spin == SPIN_RUNNING))) {
        d->spin     = SPIN_STOPPING;
        d->spin_pos = 0;
    }
    ambient_unlock();
}

void
cdrom_audio_tray_open(cdrom_t *dev)
{
    drive_t *d = drive_of(dev);

    if (d == NULL)
        return;

    ambient_lock();
    if (d->profile != 0) {
        play(d, S_OPEN_TRAY);
        /* The tray stops the disc: no spin-down sound, just silence */
        d->spin     = SPIN_STOPPED;
        d->spin_pos = 0;
        d->head     = -1;
    }
    ambient_unlock();
}

void
cdrom_audio_tray_close(cdrom_t *dev)
{
    drive_t *d = drive_of(dev);

    if (d == NULL)
        return;

    ambient_lock();
    if (d->profile != 0) {
        /* A host image swap removes and inserts at the same instant: let the
           tray finish opening before it closes */
        const sample_t *open = &samples[d->profile][S_OPEN_TRAY];
        int             wait = 0;

        for (int v = 0; v < VOICES; v++) {
            if ((d->voices[v].sample == open) && (open->frames - d->voices[v].pos > wait))
                wait = open->frames - d->voices[v].pos;
        }
        play_after(d, S_CLOSE_TRAY, wait);
    }
    ambient_unlock();
}

/* ------------------------------------------------------------------------ */
/* Lifecycle (emulation or main thread)                                     */
/* ------------------------------------------------------------------------ */

static void
unload(void)
{
    /* After this returns the render function won't run again (see
       ambient.h), so the samples can be freed */
    if (registered) {
        ambient_unregister_source(cdrom_audio_render, NULL);
        registered = 0;
    }

    ambient_lock();
    for (int i = 0; i < DRIVES; i++) {
        drives[i].profile = 0;
        drives[i].spin    = SPIN_STOPPED;
        memset(drives[i].voices, 0, sizeof(drives[i].voices));
    }
    ambient_unlock();

    free_all_samples();
}

void
cdrom_audio_reset(void)
{
    int any = 0;

    unload();

    for (int i = 0; i < DRIVES; i++) {
        drive_t *d = &drives[i];
        int      id = (cdrom[i].bus_type != CDROM_BUS_DISABLED) ? find_profile(cdrom_audio_profile[i]) : 0;

        if (id > 0)
            load_samples(id);

        ambient_lock();
        d->id       = i;
        d->profile  = id;
        d->spin     = SPIN_STOPPED;
        d->spin_pos = 0;
        d->head     = -1;
        ambient_unlock();

        d->accessed      = 0;
        d->idle_seconds  = 0;
        d->timer_running = 0;
        /* The hard reset cleared every timer (timer_close()), so add it again */
        timer_add(&d->idle_timer, idle_tick, d, 0);

        any |= (id > 0);
    }

    /* No drive with a profile: no source, so no thread and no CPU cost */
    if (any)
        registered = (ambient_register_source(cdrom_audio_render, NULL) == 0);
}

void
cdrom_audio_close(void)
{
    unload();
}

void
cdrom_audio_power_off(void)
{
    ambient_lock();
    for (int i = 0; i < DRIVES; i++) {
        drive_t *d = &drives[i];

        if ((d->profile != 0) && ((d->spin == SPIN_STARTING) || (d->spin == SPIN_RUNNING))) {
            d->spin     = SPIN_STOPPING;
            d->spin_pos = 0;
        }
    }
    ambient_unlock();
}

int
cdrom_audio_spin_down_ms_left(void)
{
    int frames = 0;

    ambient_lock();
    for (int i = 0; i < DRIVES; i++) {
        const drive_t *d = &drives[i];

        if ((d->profile != 0) && (d->spin == SPIN_STOPPING)) {
            const int left = samples[d->profile][S_SPIN_STOP].frames - d->spin_pos;
            if (left > frames)
                frames = left;
        }
    }
    ambient_unlock();

    return (int) (((int64_t) frames * 1000) / 48000);
}
