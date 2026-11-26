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
 */
#ifndef EMU_AMBIENT_H
#define EMU_AMBIENT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Most sources that can be registered at once. */
#define AMBIENT_MAX_SOURCES 16

/*
 * A source's render function. It must ADD (not overwrite) `frames` stereo
 * frames of interleaved float audio (L, R, L, R, ...) into `buf`.
 *
 * Level: to match the floppy and hard disk sounds, a full-scale 16-bit
 * sample should come out at 0.25 (sample / 131072.0f). The mixer clamps the
 * sum to -1.0 ... +1.0.
 *
 * It runs on the ambient sound thread, with the registry lock held. It must
 * not call ambient_register_source() or ambient_unregister_source().
 */
typedef void (*ambient_render_t)(float *buf, int frames, void *priv);

/*
 * Add a source. Starts the ambient sound thread if this is the first one.
 * Returns 0 on success, -1 if the table is full or the source is already
 * registered. Call from the emulation or main thread.
 */
extern int ambient_register_source(ambient_render_t render, void *priv);

/*
 * Remove a source. When this returns, `render` is not running and will
 * never be called again for `priv`, so `priv` may be freed. Stops the
 * thread if this was the last source. Call from the emulation or main thread.
 */
extern void ambient_unregister_source(ambient_render_t render, void *priv);

/* Remove every source and stop the thread. Called once from pc_close(). */
extern void ambient_close(void);

/* Used by the ambient sound thread in sound.c: zero `buf`, mix every
   source into it, clamp. */
extern void ambient_render(float *buf, int frames);


#ifdef __cplusplus
}
#endif

#endif /* EMU_AMBIENT_H */
