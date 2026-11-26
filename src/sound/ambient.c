/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Ambient sound mixer: one output stream (I_AMBIENT) shared by
 *          every ambient sound source (system fan, CD-ROM drive, ...).
 *
 *          Each source registers a render function. The ambient sound
 *          thread (sound_ambient_thread() in sound.c) calls ambient_render()
 *          about 50 times a second; that zeroes the buffer, lets every
 *          source add its sound, and clamps the result.
 *
 *          The thread only runs while at least one source is registered,
 *          so with every ambient sound turned off it costs nothing.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <86box/86box.h>
#include <86box/sound.h>
#include <86box/thread.h>
#include <86box/ambient.h>

typedef struct {
    ambient_render_t render;
    void            *priv;
} ambient_source_t;

/* The registry. `ambient_mutex` guards both fields. */
static ambient_source_t sources[AMBIENT_MAX_SOURCES];
static int              source_count  = 0;
static mutex_t         *ambient_mutex = NULL;

int
ambient_register_source(ambient_render_t render, void *priv)
{
    if (render == NULL)
        return -1;

    if (ambient_mutex == NULL)
        ambient_mutex = thread_create_mutex();

    thread_wait_mutex(ambient_mutex);

    for (int i = 0; i < source_count; i++) {
        if ((sources[i].render == render) && (sources[i].priv == priv)) {
            thread_release_mutex(ambient_mutex);
            return -1;
        }
    }

    if (source_count >= AMBIENT_MAX_SOURCES) {
        thread_release_mutex(ambient_mutex);
        return -1;
    }

    sources[source_count].render = render;
    sources[source_count].priv   = priv;
    source_count++;

    thread_release_mutex(ambient_mutex);

    /* Does nothing if the thread is already running. Started outside the
       lock: the new thread's first job is to take it. */
    sound_ambient_thread_init();

    return 0;
}

void
ambient_unregister_source(ambient_render_t render, void *priv)
{
    int now_empty;

    if (ambient_mutex == NULL)
        return;

    /* Taking the lock waits for a render pass in progress to finish, so once
       we hold it the source isn't running, and after we drop it the thread
       can't see it any more. */
    thread_wait_mutex(ambient_mutex);

    for (int i = 0; i < source_count; i++) {
        if ((sources[i].render == render) && (sources[i].priv == priv)) {
            /* Close the gap so the list stays packed */
            memmove(&sources[i], &sources[i + 1], (source_count - i - 1) * sizeof(ambient_source_t));
            source_count--;
            break;
        }
    }
    now_empty = (source_count == 0);

    thread_release_mutex(ambient_mutex);

    /* Stopping waits for the thread to exit, and the thread takes the lock
       on every pass, so this must happen after releasing it. */
    if (now_empty)
        sound_ambient_thread_end();
}

void
ambient_close(void)
{
    if (ambient_mutex == NULL)
        return;

    thread_wait_mutex(ambient_mutex);
    source_count = 0;
    thread_release_mutex(ambient_mutex);

    sound_ambient_thread_end();
}

void
ambient_render(float *buf, int frames)
{
    memset(buf, 0, frames * 2 * sizeof(float));

    if (ambient_mutex == NULL)
        return;

    thread_wait_mutex(ambient_mutex);
    for (int i = 0; i < source_count; i++)
        sources[i].render(buf, frames, sources[i].priv);
    thread_release_mutex(ambient_mutex);

    /* Several loud sources can add up past full scale */
    for (int i = 0; i < frames * 2; i++) {
        if (buf[i] > 1.0f)
            buf[i] = 1.0f;
        else if (buf[i] < -1.0f)
            buf[i] = -1.0f;
    }
}

#ifdef AMBIENT_TEST
#    include <math.h>
#    include <86box/plat_unused.h>

static double test_phase = 0.0;

static void
ambient_test_render(float *buf, int frames, UNUSED(void *priv))
{
    const double step = 2.0 * M_PI * 440.0 / (double) sound_sample_rate;

    for (int i = 0; i < frames; i++) {
        /* 0.05 is about -26 dBFS: clearly audible, but quiet */
        float s = (float) (0.05 * sin(test_phase));
        buf[i * 2] += s;
        buf[i * 2 + 1] += s;
        test_phase += step;
        if (test_phase >= 2.0 * M_PI)
            test_phase -= 2.0 * M_PI;
    }
}

void
ambient_test_init(void)
{
    ambient_register_source(ambient_test_render, NULL);
}
#endif
