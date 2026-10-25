#ifndef RASTERIZER_H
#define RASTERIZER_H

#define RECT PS1_RECT
#include "../draw.h"
#undef RECT

typedef struct {
    void (*reset)(void);
    void (*texpage_mode)(ParamDrawTexpageMode* mode);
    void (*texture_window)(
        unsigned int mx, unsigned int my, unsigned int ox, unsigned int oy);
    void (*area_start)(int x, int y);
    void (*area_end)(int x, int y);
    void (*offset)(int x, int y);
    int (*horizontal_grid)(unsigned int source, unsigned int target);
    void (*mask)(int set, int preserve);
    void (*clear_image)(PS1_RECT* rect, u_char r, u_char g, u_char b);
    void (*load_image)(PS1_RECT* rect, u_long* data);
    void (*store_image)(PS1_RECT* rect, u_long* data);
    void (*move_image)(PS1_RECT* rect, unsigned int x, unsigned int y);
    void (*reset_buffer)(void);
    void (*flush_buffer)(void);
    int (*push_prim)(u_long* words, int count);
    int (*exeque_sync)(void);
} DrawBackend;

extern const DrawBackend software_rasterizer;
void SoftwareRasterizer_Reset(void);
// Reading VRAM or the gutter planes draws anything still deferred first (see
// SoftwareRasterizer_SetDefer); the Raw getters do not, for callers that
// SoftwareRasterizer_SyncRect what they read.
const u16* SoftwareRasterizer_GetVram(void);
const u16* SoftwareRasterizer_GetVramRaw(void);

// Deferred drawing (sdl3_software_defer.inc). While it is on, primitives are
// queued instead of drawn, and a queued frame that a later opaque rectangle
// covers before anything reads it is never drawn. Everything that could see
// the difference (image ops, texture reads, gutter row clears,
// SoftwareRasterizer_SyncRect, GetVram) draws what it needs first, so VRAM and
// the gutter planes read through them are exactly what immediate drawing
// leaves. Turning it off draws what is queued.
void SoftwareRasterizer_SetDefer(int on);
int SoftwareRasterizer_GetDefer(void);
void SoftwareRasterizer_SyncAll(void);
// the VRAM rectangle, and with gutter the gutter plane rows y .. y + h - 1
void SoftwareRasterizer_SyncRect(int x, int y, int w, int h, int gutter);
void SoftwareRasterizer_GetDeferStats(unsigned long* queued,
                                      unsigned long* dropped,
                                      unsigned long* run,
                                      unsigned long* sync_layers,
                                      unsigned long* other_layers, int reset);

// Side gutters, see Psyz_VideoSetGutter. They apply to drawing areas spanning
// columns [x0, x0 + w), any band of rows (h, the display height, is kept for
// callers and no longer narrows it); px = 0 turns them off. A plane holds VRAM_H rows of PSYZ_GUTTER_MAX pixels, side 0 is left.
void SoftwareRasterizer_SetGutter(int px, int x0, int w, int h);
int SoftwareRasterizer_GetGutter(void);
const u16* SoftwareRasterizer_GetGutterPlane(int side);
const u16* SoftwareRasterizer_GetGutterPlaneRaw(int side);
void SoftwareRasterizer_ClearGutterRows(int y0, int y1);

#endif
