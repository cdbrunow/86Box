/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          System fan / power supply sound emulation.
 *
 *          The fan is a source on the shared ambient stream (ambient.c).
 *          It follows the machine's power, not what the CPU is doing:
 *
 *            cold power-on / hard reset -> spin-up, then the running loop
 *            guest soft power-off       -> spin-down, then silence
 *            pause                      -> silent; resumes mid-loop
 *            warm reset, fast-forward   -> no change
 *            emulator exit              -> stops at once, plays nothing
 *
 *          Everything the render function reads is guarded by the ambient
 *          lock (ambient_lock()), which the render function runs under.
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
#include <86box/sound_util.h>
#include <86box/ambient.h>
#include <86box/system_audio.h>

/* All samples are 48 kHz (see sound_load_wav()) */
#define SAMPLE_RATE 48000

/* Extra time to let the audio backend's queued buffers play out */
#define POWER_OFF_MARGIN_MS 250

typedef struct {
    char  filename[512];
    float volume;
} system_audio_sample_config_t;

typedef struct {
    int                          id;
    char                         name[128];
    char                         internal_name[64];
    system_audio_sample_config_t start;
    system_audio_sample_config_t loop;
    system_audio_sample_config_t stop;
} system_audio_profile_t;

/* A decoded sample: interleaved stereo, `frames` frames long */
typedef struct {
    int16_t *buffer;
    int      frames;
    float    volume;
} system_audio_sample_t;

typedef enum {
    FAN_STOPPED = 0,
    FAN_STARTING, /* playing `start` */
    FAN_RUNNING,  /* repeating `loop` */
    FAN_STOPPING  /* playing `stop` */
} fan_state_t;

char system_fan_audio_profile[64] = "none";

static system_audio_profile_t profiles[SYSTEM_AUDIO_PROFILE_MAX];
static int                    profile_count = 0;

/* The fan. `loaded_profile` and the samples change only while the source is
   unregistered; `state` and `pos` change under the ambient lock. */
static struct {
    int                   loaded_profile; /* 0 = none */
    int                   registered;
    system_audio_sample_t start;
    system_audio_sample_t loop;
    system_audio_sample_t stop;
    fan_state_t           state;
    int                   pos; /* frame position in the current sample */
} fan;

#ifdef ENABLE_SYSTEM_AUDIO_LOG
int system_audio_do_log = ENABLE_SYSTEM_AUDIO_LOG;

static void
system_audio_log(const char *fmt, ...)
{
    va_list ap;

    if (system_audio_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define system_audio_log(fmt, ...)
#endif

/* ------------------------------------------------------------------------ */
/* Profiles                                                                 */
/* ------------------------------------------------------------------------ */

static void
read_sample_config(ini_section_t cat, const char *prefix, system_audio_sample_config_t *cfg)
{
    char key[64];

    snprintf(key, sizeof(key), "%s_file", prefix);
    strncpy(cfg->filename, ini_section_get_string(cat, key, ""), sizeof(cfg->filename) - 1);

    snprintf(key, sizeof(key), "%s_volume", prefix);
    cfg->volume = (float) ini_section_get_double(cat, key, 1.0);
}

void
system_audio_load_profiles(void)
{
    const char *expected     = "system_audio_profiles.cfg";
    char        cfg_fn[1024] = { 0 };

    profile_count = 0;

    if (!asset_getfile("assets/sounds/system/system_audio_profiles.cfg", cfg_fn, sizeof(cfg_fn))) {
        system_audio_log("System audio: no system_audio_profiles.cfg\n");
        return;
    }

    /* Same checks as hdd_audio_load_profiles(): a path from the asset
       search paths, with no "..", ending in the expected name. */
    size_t len = strlen(cfg_fn);
    if ((strstr(cfg_fn, "..") != NULL) || (len < strlen(expected)) || strcmp(cfg_fn + len - strlen(expected), expected) != 0)
        return;

    ini_t ini = ini_read_ex(cfg_fn, 1); /* lgtm[cpp/path-injection] */
    if (ini == NULL)
        return;

    for (int i = 0; i < SYSTEM_AUDIO_PROFILE_MAX; i++) {
        char section[32];
        snprintf(section, sizeof(section), "Profile \"%d\"", i);

        ini_section_t cat = ini_find_section(ini, section);
        if (cat == NULL)
            continue;

        system_audio_profile_t *p = &profiles[profile_count];
        memset(p, 0, sizeof(*p));
        p->id = ini_section_get_int(cat, "id", i);
        strncpy(p->name, ini_section_get_string(cat, "name", "Unknown"), sizeof(p->name) - 1);
        strncpy(p->internal_name, ini_section_get_string(cat, "internal_name", "unknown"),
                sizeof(p->internal_name) - 1);
        read_sample_config(cat, "start", &p->start);
        read_sample_config(cat, "loop", &p->loop);
        read_sample_config(cat, "stop", &p->stop);

        system_audio_log("System audio: profile %d: %s (%s)\n", profile_count, p->name, p->internal_name);
        profile_count++;
    }

    ini_close(ini);
}

int
system_audio_get_profile_count(void)
{
    return profile_count;
}

const char *
system_audio_get_profile_name(int id)
{
    return ((id >= 0) && (id < profile_count)) ? profiles[id].name : NULL;
}

const char *
system_audio_get_profile_internal_name(int id)
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

/* ------------------------------------------------------------------------ */
/* Samples                                                                  */
/* ------------------------------------------------------------------------ */

static void
load_sample(const system_audio_sample_config_t *cfg, system_audio_sample_t *s)
{
    s->buffer = NULL;
    s->frames = 0;
    s->volume = cfg->volume;

    if (cfg->filename[0] == '\0')
        return;

    s->buffer = sound_load_wav(cfg->filename, &s->frames);
    if (s->buffer == NULL) {
        s->frames = 0;
        system_audio_log("System audio: failed to load %s\n", cfg->filename);
    }
}

static void
free_sample(system_audio_sample_t *s)
{
    free(s->buffer);
    s->buffer = NULL;
    s->frames = 0;
}

/* ------------------------------------------------------------------------ */
/* Rendering (ambient thread, ambient lock held)                            */
/* ------------------------------------------------------------------------ */

/*
 * Add up to `frames` frames of `s`, starting at fan.pos, into `buf`.
 * Advances fan.pos and returns how many frames it added (fewer than asked
 * when the sample ends).
 */
static int
mix_sample(const system_audio_sample_t *s, float *buf, int frames)
{
    const float scale = s->volume / 131072.0f; /* full scale -> 0.25, like the drive sounds */
    int         n     = s->frames - fan.pos;

    if (n > frames)
        n = frames;

    const int16_t *src = &s->buffer[fan.pos * 2];
    for (int i = 0; i < n * 2; i++)
        buf[i] += (float) src[i] * scale;

    fan.pos += n;
    return n;
}

static int
fan_render(float *buf, int frames, int paused, UNUSED(void *priv))
{
    /* While paused, only the power-off spin-down plays (see ambient.h) */
    if (paused && (fan.state != FAN_STOPPING))
        return 0;

    /* Fill the whole block, moving through the states as samples end, so
       the hand-off from spin-up to loop leaves no gap */
    int done = 0;
    while ((done < frames) && (fan.state != FAN_STOPPED)) {
        float *out  = &buf[done * 2];
        int    left = frames - done;

        switch (fan.state) {
            case FAN_STARTING:
                if (fan.pos < fan.start.frames)
                    done += mix_sample(&fan.start, out, left);
                if (fan.pos >= fan.start.frames) {
                    fan.state = FAN_RUNNING;
                    fan.pos   = 0;
                }
                break;

            case FAN_RUNNING:
                if (fan.loop.frames == 0)
                    return 0; /* no loop sample: nothing more to play */
                done += mix_sample(&fan.loop, out, left);
                if (fan.pos >= fan.loop.frames)
                    fan.pos = 0;
                break;

            case FAN_STOPPING:
                if (fan.pos < fan.stop.frames)
                    done += mix_sample(&fan.stop, out, left);
                if (fan.pos >= fan.stop.frames) {
                    fan.state = FAN_STOPPED;
                    fan.pos   = 0;
                    system_audio_log("System audio: spin-down finished\n");
                }
                break;

            default:
                break;
        }
    }

    /* Ask to be heard while paused only for blocks of the spin-down */
    return paused && (done > 0);
}

/* ------------------------------------------------------------------------ */
/* Lifecycle (emulation or main thread)                                     */
/* ------------------------------------------------------------------------ */

static void
unload(void)
{
    /* After this returns, fan_render() won't run again (see ambient.h), so
       the samples can be freed */
    if (fan.registered) {
        ambient_unregister_source(fan_render, NULL);
        fan.registered = 0;
    }

    free_sample(&fan.start);
    free_sample(&fan.loop);
    free_sample(&fan.stop);
    fan.loaded_profile = 0;
    fan.state          = FAN_STOPPED;
    fan.pos            = 0;
}

void
system_audio_reset(void)
{
    const int id = find_profile(system_fan_audio_profile);

    /* Load the selected profile's samples, if it changed (or was turned off) */
    if (id != fan.loaded_profile) {
        unload();

        if (id > 0) {
            load_sample(&profiles[id].start, &fan.start);
            load_sample(&profiles[id].loop, &fan.loop);
            load_sample(&profiles[id].stop, &fan.stop);
            fan.loaded_profile = id;
            system_audio_log("System audio: using %s\n", profiles[id].name);
        }
    }

    if (fan.loaded_profile == 0)
        return; /* off: no source, so no thread and no CPU cost */

    /* Cold power-on: spin up from the start, whatever the fan was doing */
    ambient_lock();
    fan.state = FAN_STARTING;
    fan.pos   = 0;
    ambient_unlock();

    if (!fan.registered)
        fan.registered = (ambient_register_source(fan_render, NULL) == 0);
}

int
system_audio_power_off(void)
{
    int ms = 0;

    if (!fan.registered)
        return 0;

    ambient_lock();
    if ((fan.state == FAN_STARTING) || (fan.state == FAN_RUNNING)) {
        fan.state = FAN_STOPPING;
        fan.pos   = 0;
        if (fan.stop.frames > 0)
            ms = (int) (((int64_t) fan.stop.frames * 1000) / SAMPLE_RATE) + POWER_OFF_MARGIN_MS;
    }
    ambient_unlock();

    system_audio_log("System audio: power off, spin-down takes %d ms\n", ms);
    return ms;
}

void
system_audio_close(void)
{
    unload();
}
