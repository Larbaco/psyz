#ifndef PSYZ_VIDEO_H
#define PSYZ_VIDEO_H

/**
 * @file video.h
 * @brief Window, display timing and frame presentation endpoints.
 */

#include <psyz/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Psyz_VideoSetResolution: reset draw area to its default behavior */
#define PSYZ_DEFAULT_RESOLUTION ((PsyzSize){0, 0})

/** Maximum accepted internal resolution multiplier */
#define PSYZ_INTERNAL_RES_MAX 8

/** Psyz_VideoSetInternalResolution: 1x in PS1 mode, display scale if native */
#define PSYZ_INTERNAL_RES_AUTO 0

#ifndef __PSP__
/**
 * Targets without this capability must:
 *   - Implement Psyz_VideoSetInternalResolution as no-op
 *   - Always return 1 for Psyz_VideoGetInternalResolution
 */
#define PSYZ_HAS_INTERNAL_RESOLUTION_SCALE
#endif

typedef enum {
    /**
     * Auto-detect use of driver VSync (default)
     * - Tests monitor refresh rate on initialization (adds ~30ms startup time)
     * - Uses driver VSync if monitor is detected between 57Hz-63Hz
     *   (±5% tolerance). This covers both 59.94Hz (NTSC) and 60Hz
     * - Uses manual limiter otherwise (144Hz, 165Hz, etc.)
     * - Balances performance and accuracy automatically
     */
    PSYZ_VSYNC_AUTO,

    /**
     * Always use driver VSync
     * Pros: Zero CPU overhead and no frame pacing on matching refresh rates
     * Cons: Game will run faster on high refresh monitors
     * Cons: Framerate always matches monitor exactly.
     *   On 60Hz monitor: runs at 60fps instead of NTSC 59.94fps
     *   causing ~1 second drift every ~16.7 minutes of gameplay
     * Real NTSC PSX hardware runs at 59.94Hz (60/1.001)
     *   PAL PSX hardware runs at exactly 50Hz
     * Best when combined with driver control panel frame rate limit
     * IMPORTANT: PAL games (50fps) will run at 60fps on 60Hz monitors without
     *   driver-level limiting, which is incorrect
     */
    PSYZ_VSYNC_ON,

    /**
     * Always use internal manual frame limiter
     * Pros: Precise 59.94fps (NTSC) / 50fps (PAL) matching real hardware
     * Pros: Safe for VRR displays
     * Pros: Consistent timing across all monitor refresh rates
     * Cons: ~6% CPU usage on one core (1ms busy-wait per frame for precision)
     * Cons: May have minor frame pacing variance on non-VRR displays
     */
    PSYZ_VSYNC_OFF,

    /**
     * Disable both driver VSync and the internal frame limiter.
     * Most games will be unplayable with this option, but it's useful for:
     *   - Adapting game on variable framerate using a deltaTime.
     *   - Reproduce gameplay with simulatd input on CI at high speed.
     *   - Measure frame time end-to-end and if game is CPU or GPU bound.
     */
    PSYZ_VSYNC_LIMITLESS,
} PsyzVsyncMode;

typedef enum {
    PSYZ_DITHER_AUTO, /**< let the game decide whether to dither (default) */
    PSYZ_DITHER_OFF,  /**< force dithering always off */
} PsyzDitherMode;

typedef enum {
    PSYZ_ASPECT_DISPLAY, /**< aspect from PS1 H/V display ranges (default) */
    PSYZ_ASPECT_SQUARE,  /**< 1:1 from framebuffer (pixel-perfect) */
} PsyzAspectMode;

typedef struct {
    double last_frame_time_us;       /**< duration of last frame */
    double last_draw_time_us;        /**< render time excluding vsync wait */
    double target_frame_time_us;     /**< target frame time */
    unsigned long long total_frames; /**< total frames rendered */
    int using_driver_vsync;          /**< 1 for VSync, 0 for limiter */
} PsyzVideoStats;

typedef enum {
    PSYZ_ANCHOR_CENTER = 0,   /**< extra space split evenly (default) */
    PSYZ_ANCHOR_LEFT = 1,     /**< left edge, centered vertically */
    PSYZ_ANCHOR_TOP = 2,      /**< top edge, centered horizontally */
    PSYZ_ANCHOR_TOP_LEFT = 3, /**< top-left corner */
} PsyzAnchor;

/**
 * @brief Set the title of the game window
 *
 * Can also be set with psyz_title(<target> <str>) on CMakeLists
 *
 * @param str Window title string, truncated past 255 characters
 */
void Psyz_SetTitle(const char* str);

/**
 * @brief Get the current VSync mode
 *
 * @return current VSync mode
 */
PsyzVsyncMode Psyz_VideoGetVsyncMode(void);

/**
 * @brief Set VSync mode (default: AUTO)
 *
 * @param mode VSync mode to set
 * @return 0 on success, -1 if invalid mode
 */
int Psyz_VideoSetVsyncMode(PsyzVsyncMode mode);

/**
 * @brief Get the current dithering mode
 *
 * @return current dithering mode
 */
PsyzDitherMode Psyz_VideoGetDitheringMode(void);

/**
 * @brief Set dithering mode (default: AUTO)
 *
 * @param mode Dithering mode to set
 * @return 0 on success, -1 if invalid mode
 */
int Psyz_VideoSetDitheringMode(PsyzDitherMode mode);

/**
 * @brief Get the current aspect mode
 *
 * @return current aspect mode
 */
PsyzAspectMode Psyz_VideoGetAspectMode(void);

/**
 * @brief Select how the presented aspect ratio is determined
 *
 * DISPLAY: use the game horizontal/vertical display (display sync) ranges,
 *   so different game resolutions present at the intended physical proportions.
 *   This is the accurate and default behaviour, originally intended to control
 *   the beam range of a CRT display. This is not pixel-perfect.
 * SQUARE: present framebuffer pixels 1:1, ignoring sync ranges. This presents
 *   the game pixel-perfect to the display at the cost of intent-accuracy.
 *
 * @param mode Aspect mode to set
 * @return 0 on success, -1 if invalid mode
 */
int Psyz_VideoSetAspectMode(PsyzAspectMode mode);

/**
 * @brief Physical display size in pixels
 *
 * - PC: either the window size, or the value set in Psyz_VideoSimulateDisplay
 * - PSP: always 480x272
 *
 * @return display size in pixels
 */
PsyzSize Psyz_VideoGetDisplaySize(void);

/**
 * @brief Mock custom display size to emulate a specific console
 *
 * PC shows the simulated display scaled inside the window. Useful when testing
 * a game on different resolutions without targeting a specific console.
 * PSYZ_VIDEO_DISPLAY env var can also be used (e.g. 480x272).
 * Set size to 0x0 to reset to the default behavior.
 *
 * @param size display size in pixels
 * @return 0 on success, -1 if invalid or not supported
 */
int Psyz_VideoSimulateDisplay(PsyzSize size);

/**
 * @brief Set a custom output resolution
 *
 * Psyz_VideoSetResolution(Psyz_VideoGetDisplaySize()) will effectively match
 * the game's resolution with the native display. Setting anything higher is
 * undefined behavior. Setting a resolution smaller than the display will give
 * black bars around the output image. Setting a size of 0x0 will revert to its
 * default behavior.
 *
 * @return 0 on success, -1 on invalid size or when the target cannot do it
 */
int Psyz_VideoSetResolution(PsyzSize size);

/**
 * @brief Choose where the PS1 framebuffer sits inside the canvas
 *
 * @return 0 on success, -1 if invalid
 */
int Psyz_VideoSetAnchor(PsyzAnchor anchor);

/** @brief Get the current anchor */
PsyzAnchor Psyz_VideoGetAnchor(void);

/**
 * @brief Visible canvas in game coordinates, the framebuffer top-left is 0,0
 *
 * This is the function you want to use to adjust your game based on the
 * display resolution of the targeted window or console. The returned value has
 * the coordinates of what parts of the game will be rendered in relation of
 * the chosen anchor. Resolution and aspect ratio are also evaluated.

 * Examples:
 * - Game resolution 320x240
 *   Display         480x272
 *   Resolution set  480x272
 *   Anchor          Center
 *   Aspect ratio    Display
 *   Draw area is:   -80,-16,400,256
 * - Game resolution 320x240
 *   Display         480x272
 *   Resolution set  400x240
 *   Anchor          TopLeft         <--- top-left anchor allows you to avoid
 *   Aspect ratio    Display              working on negative coordinates, so
 *   Draw area is:   0,0,400,240          sx,sy will start from 0.
 * - Game resolution 256x240
 *   Display         480x272
 *   Resolution set  480x240
 *   Anchor          Center
 *   Aspect ratio    Display         <--- at 256x240, draw area is 384x240
 *   Draw area is:   -64,0,320,240        due to the aspect ratio correction.
 * - Game resolution 256x240
 *   Display         480x272
 *   Resolution set  480x240
 *   Anchor          Center
 *   Aspect ratio    Square          <--- Square match display resolution, but
 *   Draw area is:   -112,0,368,240       aspect ratio will look different.
 */
PsyzRect Psyz_VideoGetDrawArea(void);

/**
 * @brief Set the internal rendering resolution multiplier (default: AUTO)
 *
 * Internally multiplies horizontal and vertical resolution, giving 3D
 * geometry more sub-pixel precision (smoother edges, less shimmer).
 * Safe to call at runtime; the change applies on the next presented frame
 * without needing to restart the game.
 *
 * A target may define PSYZ_HAS_INTERNAL_RESOLUTION_SCALE if the internal
 * resolution multiplier is supported. This is useful to hide or remove any
 * scaling functionality from the front-end / UI.
 *
 * @param multiplier PSYZ_INTERNAL_RES_AUTO, or 1 to PSYZ_INTERNAL_RES_MAX
 * @return 0 on success, -1 if multiplier > PSYZ_INTERNAL_RES_MAX
 */
int Psyz_VideoSetInternalResolution(unsigned multiplier);

/**
 * @brief Get the current internal resolution multiplier
 *
 * @return current multiplier (>= 1)
 */
unsigned Psyz_VideoGetInternalResolution(void);

/** Maximum accepted side gutter, in framebuffer pixels per side */
#define PSYZ_GUTTER_MAX 64

/**
 * @brief Draw past the left and right edges of the display (widescreen)
 *
 * With a non-zero gutter, a drawing area exactly as wide as the displayed
 * framebuffer (any band of its rows) is extended by `px` columns on each
 * side. The extra
 * columns do not live in VRAM, which the game keeps using as usual: they are
 * kept in side planes, and the presented picture becomes the framebuffer plus
 * both gutters, with the aspect ratio widened to match. An untextured
 * rectangle covering the whole drawing area (a background clear) is extended
 * into the gutters too. Gutters start black on every buffer flip.
 *
 * Targets without this capability return -1 for any non-zero value.
 *
 * @param px gutter width per side, 0 (default, off) to PSYZ_GUTTER_MAX
 * @return 0 on success, -1 if px is out of range or unsupported
 */
int Psyz_VideoSetGutter(unsigned px);

/**
 * @brief Get the current side gutter width
 *
 * @return gutter width per side in pixels, 0 when off
 */
unsigned Psyz_VideoGetGutter(void);

/**
 * @brief Copy the side gutters of the display into VRAM, beside a MoveImage copy
 *
 * The gutters do not live in VRAM, so MoveImage of the displayed framebuffer
 * leaves them behind. After `MoveImage(&{displayX, src_y, displayW, h}, dx, dy)`
 * this copies the gutter columns of rows [src_y, src_y + h) to VRAM rows
 * [dy, dy + h): the left gutter to columns [dx - px, dx) and the right gutter
 * to [dx + displayW, dx + displayW + px), where px is the gutter width and
 * displayW the width the gutter was set for. Together with the copy they are
 * one contiguous (displayW + 2 * px) wide image a textured primitive can
 * sample. Queued drawing that could change the rows is drawn first. Nothing
 * happens (0 is returned) with the gutter off, so a game can call it
 * unconditionally.
 *
 * @param src_y first VRAM row of the displayed framebuffer to take
 * @param h number of rows
 * @param dx destination column of the MoveImage copy
 * @param dy destination row of the MoveImage copy
 * @return the gutter width copied per side, 0 when there is none, -1 if the
 *         target cannot do it or the strips would leave VRAM
 */
int Psyz_VideoCopyGutters(int src_y, int h, int dx, int dy);

/**
 * @brief Synchronize with vertical blank
 *
 * Synchronize with the refresh rate mode set in Psyz_VideoSetVsyncMode.
 * The interface is very similar to libetc VSync.
 *
 * @param mode Synchronization mode:
 *             - 0: Present and wait for the next vertical blank
 *             - 1: Return immediately (non-blocking)
 *             - n > 1: Present and wait until n vertical blanks have
 *               passed since the previous blocking call
 *             - Negative: Return immediately (non-blocking)
 * @return Simulates the SDK VSync return values as closely as possible.
 */
int Psyz_VideoVSync(int mode);

/**
 * @brief Get frame timing statistics
 *
 * @param stats Output structure to fill
 * @return 0 on success, -1 if stats is NULL or platform not initialized
 */
int Psyz_VideoStats(PsyzVideoStats* stats);

/**
 * @brief Get frame output as a byte array
 *
 * This function is very slow.
 *
 * @param w Output frame width
 * @param h Output frame height
 * @return NULL on failure, or RGB888 buffer to be destroyed with free(ptr)
 */
unsigned char* Psyz_VideoAllocCapturedFrame(int* w, int* h);

/**
 * @brief Dump the whole video memory plane as RGB888
 *
 * @param w Output VRAM width
 * @param h Output VRAM height
 * @return NULL on failure, or RGB888 buffer to be destroyed with free(ptr)
 */
unsigned char* Psyz_VideoAllocVramDump(int* w, int* h);

#ifdef __cplusplus
}
#endif

#endif
