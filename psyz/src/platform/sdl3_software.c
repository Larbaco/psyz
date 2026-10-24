#include <psyz.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "../internal.h"
#include "rasterizer.h"

typedef struct {
    int x, y;
} Point;

typedef struct {
    int x, y;
    int u, v;
    int r, g, b;
} SwVertex;

#define SEMITRANSP 0x02
#define TEXTURED 0x04
#define EXTRA_VERTEX 0x08
#define GOURAUD 0x10
#define TRIANGLE 0x20

static u16 vram[VRAM_W * VRAM_H];
static bool dither_enabled;

static int SoftwareDither(void) {
    return dither_enabled && Psyz_VideoGetDitheringMode() != PSYZ_DITHER_OFF;
}
static Point draw_offset;
static Point draw_area_start;
static Point draw_area_end = {VRAM_W - 1, VRAM_H - 1};
static u16 cur_tpage;
static u8 twin_and_x = 0xff;
static u8 twin_and_y = 0xff;
static u8 twin_or_x;
static u8 twin_or_y;
static bool set_mask_bit;
static bool preserve_masked;
static unsigned grid_source = 1;
static unsigned grid_target = 1;

// Side gutters (Psyz_VideoSetGutter): columns past the left and right edges
// of a display-sized drawing area, kept outside VRAM. Row y of a plane is the
// gutter of VRAM row y; column 0 is the leftmost gutter pixel.
static u16 gutter_planes[2][VRAM_H * PSYZ_GUTTER_MAX];
static int gutter_px;
static int gutter_x0;
static int gutter_w;
static bool gutter_active; // the current drawing area takes the gutters

// An area spanning the display's columns takes the gutters, whatever its
// height: a layer clipped to a band of rows (a banner closing from the top
// and the bottom) still reaches the side gutters in those rows.
static void UpdateGutterActive(void) {
    gutter_active = gutter_px > 0 && draw_area_start.x == gutter_x0 &&
                    draw_area_end.x == gutter_x0 + gutter_w - 1;
}

// leftmost and rightmost column a primitive may touch
static int ClipX0(void) {
    return gutter_active ? draw_area_start.x - gutter_px
                         : (draw_area_start.x > 0 ? draw_area_start.x : 0);
}

static int ClipX1(void) {
    return gutter_active
               ? draw_area_end.x + gutter_px
               : (draw_area_end.x < VRAM_W - 1 ? draw_area_end.x : VRAM_W - 1);
}

// the pixel (x, y) is written to, or NULL when it is clipped
static u16* PixelPtr(int x, int y) {
    if (y < draw_area_start.y || y > draw_area_end.y || y < 0 || y >= VRAM_H) {
        return NULL;
    }
    if (x >= draw_area_start.x && x <= draw_area_end.x) {
        return x >= 0 && x < VRAM_W ? &vram[y * VRAM_W + x] : NULL;
    }
    if (!gutter_active) {
        return NULL;
    }
    if (x < draw_area_start.x && x >= draw_area_start.x - gutter_px) {
        return &gutter_planes[0][y * PSYZ_GUTTER_MAX +
                                 (x - (draw_area_start.x - gutter_px))];
    }
    if (x > draw_area_end.x && x <= draw_area_end.x + gutter_px) {
        return &gutter_planes[1][y * PSYZ_GUTTER_MAX +
                                 (x - draw_area_end.x - 1)];
    }
    return NULL;
}

static void FillGutterRows(int y0, int y1, u16 color) {
    y0 = CLAMP(y0, 0, VRAM_H);
    y1 = CLAMP(y1, 0, VRAM_H);
    for (int side = 0; side < 2; side++) {
        for (int y = y0; y < y1; y++) {
            u16* row = &gutter_planes[side][y * PSYZ_GUTTER_MAX];
            for (int x = 0; x < PSYZ_GUTTER_MAX; x++) {
                row[x] = color;
            }
        }
    }
}

static int Sign11(int value) {
    value &= 0x7ff;
    return (value ^ 0x400) - 0x400;
}

static int Min3(int a, int b, int c) {
    int m = a < b ? a : b;
    return m < c ? m : c;
}

static int Max3(int a, int b, int c) {
    int m = a > b ? a : b;
    return m > c ? m : c;
}

static int Clamp5(int value) { return CLAMP(value, 0, 31); }

static u16 PackColor(int r, int g, int b, bool mask) {
    return (u16)(Clamp5(r) | (Clamp5(g) << 5) | (Clamp5(b) << 10) |
                 (mask ? 0x8000 : 0));
}

// writes src to the pixel at out: mask test, semi-transparency, mask bit
static inline void PlotPixel(u16* out, u16 src, bool semi, int abr) {
    if (preserve_masked && (*out & 0x8000)) {
        return;
    }
    if (semi) {
        u16 dst = *out;
        int sr = src & 31;
        int sg = (src >> 5) & 31;
        int sb = (src >> 10) & 31;
        int dr = dst & 31;
        int dg = (dst >> 5) & 31;
        int db = (dst >> 10) & 31;
        switch (abr & 3) {
        case 0:
            sr = (sr + dr) >> 1;
            sg = (sg + dg) >> 1;
            sb = (sb + db) >> 1;
            break;
        case 1:
            sr += dr;
            sg += dg;
            sb += db;
            break;
        case 2:
            sr = dr - sr;
            sg = dg - sg;
            sb = db - sb;
            break;
        case 3:
            sr = dr + sr / 4;
            sg = dg + sg / 4;
            sb = db + sb / 4;
            break;
        }
        src = PackColor(sr, sg, sb, (src & 0x8000) != 0);
    }
    *out =
        (u16)((src & 0x7fff) | ((src | (set_mask_bit ? 0x8000 : 0)) & 0x8000));
}

static void WritePixel(int x, int y, u16 src, bool semi, int abr) {
    u16* out = PixelPtr(x, y);
    if (out) {
        PlotPixel(out, src, semi, abr);
    }
}

// The columns of row y that PixelPtr accepts (within ClipX0..ClipX1), as up
// to three runs in increasing x: left gutter, VRAM, right gutter. Column x of
// a run is run->row[x - run->x0]. The caller has checked y the way PixelPtr
// does.
typedef struct {
    int x0, x1;
    u16* row;
} RowRun;

static int RowRuns(int y, RowRun* runs) {
    int n = 0;
    int vx0 = draw_area_start.x > 0 ? draw_area_start.x : 0;
    int vx1 = draw_area_end.x < VRAM_W - 1 ? draw_area_end.x : VRAM_W - 1;
    if (gutter_active) {
        int gx0 = draw_area_start.x - gutter_px;
        if (gx0 <= draw_area_start.x - 1) {
            runs[n++] = (RowRun){
                gx0, draw_area_start.x - 1,
                &gutter_planes[0][y * PSYZ_GUTTER_MAX]};
        }
    }
    if (vx0 <= vx1) {
        runs[n++] = (RowRun){vx0, vx1, &vram[y * VRAM_W + vx0]};
    }
    if (gutter_active) {
        int gx1 = draw_area_end.x + gutter_px;
        if (draw_area_end.x + 1 <= gx1) {
            runs[n++] = (RowRun){
                draw_area_end.x + 1, gx1,
                &gutter_planes[1][y * PSYZ_GUTTER_MAX]};
        }
    }
    return n;
}

// A texture page and CLUT decoded once per primitive; FetchTexel is the
// texel at (u, v) through the texture window.
typedef struct {
    int tx, ty0, depth, clut_x;
    const u16* clut_row;
} TexPage;

static inline TexPage TexPageOf(u16 tpage, u16 clut) {
    TexPage t;
    t.tx = (tpage & 0xf) * 64;
    t.ty0 = ((tpage >> 4) & 1) * 256;
    t.depth = (tpage >> 7) & 3;
    t.clut_x = (clut & 0x3f) * 16;
    t.clut_row = &vram[((clut >> 6) & 511) * VRAM_W];
    return t;
}

static inline u16 FetchTexel(const TexPage* t, int u, int v) {
    u = ((u & twin_and_x) | twin_or_x) & 0xff;
    v = ((v & twin_and_y) | twin_or_y) & 0xff;
    const u16* row = &vram[(t->ty0 + v) * VRAM_W];
    if (t->depth == 0) {
        u16 packed = row[(t->tx + u / 4) & 1023];
        int index = (packed >> ((u & 3) * 4)) & 15;
        return t->clut_row[(t->clut_x + index) & 1023];
    }
    if (t->depth == 1) {
        u16 packed = row[(t->tx + u / 2) & 1023];
        int index = (packed >> ((u & 1) * 8)) & 255;
        return t->clut_row[(t->clut_x + index) & 1023];
    }
    return row[(t->tx + u) & 1023];
}

static const int dither[4][4] = {
    {-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};

static u16 ShadeTexel(
    u16 texel, int r, int g, int b, bool raw, int x, int y, bool do_dither) {
    int tr = texel & 31;
    int tg = (texel >> 5) & 31;
    int tb = (texel >> 10) & 31;
    if (raw) {
        return texel;
    }
    if (do_dither) {
        int d = dither[y & 3][x & 3];
        tr = Clamp5((CLAMP((tr * r) >> 4, 0, 255) + d) >> 3);
        tg = Clamp5((CLAMP((tg * g) >> 4, 0, 255) + d) >> 3);
        tb = Clamp5((CLAMP((tb * b) >> 4, 0, 255) + d) >> 3);
    } else {
        tr = CLAMP((tr * r) >> 7, 0, 31);
        tg = CLAMP((tg * g) >> 7, 0, 31);
        tb = CLAMP((tb * b) >> 7, 0, 31);
    }
    return PackColor(tr, tg, tb, (texel & 0x8000) != 0);
}

static int64_t Edge(const SwVertex* a, const SwVertex* b, int px2, int py2) {
    return (int64_t)(px2 - a->x * 2) * (b->y - a->y) -
           (int64_t)(py2 - a->y * 2) * (b->x - a->x);
}

static bool IsTopLeft(const SwVertex* a, const SwVertex* b) {
    int dy = b->y - a->y;
    int dx = b->x - a->x;
    return dy < 0 || (dy == 0 && dx > 0);
}

// An attribute interpolated along a row of a triangle: q is
// (wa * a + wb * b + wc * c) / denom, with the remainder r kept in [0, denom)
// so that one step to the next column is an add, not a division. Valid while
// the numerator is >= 0, which it is inside the triangle (the weights and the
// attributes are >= 0), so q equals the truncating division it replaces.
typedef struct {
    int64_t q, r, dq, dr;
} RowLerp;

// the step to the next column, once per triangle
static inline void RowLerpInit(RowLerp* l, int64_t dn, int denom) {
    l->dq = dn / denom;
    l->dr = dn % denom;
}

// the value at the first covered column of a row
static inline void RowLerpStart(RowLerp* l, int64_t n, int denom) {
    l->q = n / denom;
    l->r = n % denom;
}

static inline void RowLerpStep(RowLerp* l, int denom) {
    l->q += l->dq;
    l->r += l->dr;
    if (l->r >= denom) {
        l->r -= denom;
        l->q++;
    } else if (l->r < 0) {
        l->r += denom;
        l->q--;
    }
}

static void RasterTriangle(
    SwVertex a, SwVertex b, SwVertex c, bool textured, bool raw_texture,
    bool semi, bool gouraud, u16 tpage, u16 clut, bool centered) {
    int64_t area =
        (int64_t)(b.x - a.x) * (c.y - a.y) - (int64_t)(b.y - a.y) * (c.x - a.x);
    if (!area) {
        return;
    }
    if (area < 0) {
        SwVertex swap = b;
        b = c;
        c = swap;
        area = -area;
    }
    int min_x = CLAMP(Min3(a.x, b.x, c.x), ClipX0(), ClipX1());
    int max_x = CLAMP(Max3(a.x, b.x, c.x), ClipX0(), ClipX1());
    int min_y = CLAMP(Min3(a.y, b.y, c.y), draw_area_start.y, draw_area_end.y);
    int max_y = CLAMP(Max3(a.y, b.y, c.y), draw_area_start.y, draw_area_end.y);
    min_y = CLAMP(min_y, 0, VRAM_H - 1);
    max_y = CLAMP(max_y, 0, VRAM_H - 1);
    // Only rows that pass PixelPtr's y test can be written: the clamps above
    // still leave row 511 when the area starts at y >= 512 (or row 0 when it
    // ends above 0), and an inverted area keeps rows. Those rows write nothing
    // through PixelPtr, so dropping them here keeps the pixels the same.
    if (min_y < draw_area_start.y) {
        min_y = draw_area_start.y;
    }
    if (max_y > draw_area_end.y) {
        max_y = draw_area_end.y;
    }
    int abr = (tpage >> 5) & 3;
    // Per row, the edge functions step by a constant per column, and the
    // covered columns are one run (each edge test passes on a half-line of
    // x), so the row ends at the first miss after a hit. Exact integer steps:
    // the same pixels and values as evaluating Edge at every column.
    bool dith = SoftwareDither() != 0;
    TexPage tex = TexPageOf(tpage, clut);
    // ShadeTexel returns the texel as it is: raw, or flat neutral undithered
    bool keep_texel = raw_texture || (!gouraud && !dith && a.r == 128 &&
                                      a.g == 128 && a.b == 128);
    bool top_left_a = IsTopLeft(&b, &c);
    bool top_left_b = IsTopLeft(&c, &a);
    bool top_left_c = IsTopLeft(&a, &b);
    int64_t step_a = 2 * (int64_t)(c.y - b.y);
    int64_t step_b = 2 * (int64_t)(a.y - c.y);
    int64_t step_c = 2 * (int64_t)(b.y - a.y);
    int denom = (int)(area * 2);
    // a denominator that the int conversion wrapped keeps the division
    bool lerp = denom > 0;
    RowLerp lr = {0}, lg = {0}, lb = {0}, lu = {0}, lv = {0};
    // the weights step by minus the edge steps
#define DW(p) (-step_a * a.p - step_b * b.p - step_c * c.p)
    if (lerp && gouraud) {
        RowLerpInit(&lr, DW(r), denom);
        RowLerpInit(&lg, DW(g), denom);
        RowLerpInit(&lb, DW(b), denom);
    }
    if (lerp && textured) {
        RowLerpInit(&lu, DW(u), denom);
        RowLerpInit(&lv, DW(v), denom);
    }
#undef DW
    for (int y = min_y; y <= max_y; y++) {
        int px2 = min_x * 2 + centered;
        int py2 = y * 2 + centered;
        int64_t ea = Edge(&b, &c, px2, py2);
        int64_t eb = Edge(&c, &a, px2, py2);
        int64_t ec = Edge(&a, &b, px2, py2);
        bool hit = false;
        // rows here pass PixelPtr's y test (min_y..max_y lie in the area and
        // in VRAM, see above), and RowRuns are PixelPtr's columns
        RowRun runs[3];
        int nruns = RowRuns(y, runs);
        int run = 0;
        for (int x = min_x; x <= max_x;
             x++, ea += step_a, eb += step_b, ec += step_c) {
            if (ea > 0 || (ea == 0 && !top_left_a) || eb > 0 ||
                (eb == 0 && !top_left_b) || ec > 0 ||
                (ec == 0 && !top_left_c)) {
                if (hit) {
                    break;
                }
                continue;
            }
            int64_t wa = -ea;
            int64_t wb = -eb;
            int64_t wc = -ec;
#define WN(p) (wa * a.p + wb * b.p + wc * c.p)
            if (lerp && !hit) {
                if (gouraud) {
                    RowLerpStart(&lr, WN(r), denom);
                    RowLerpStart(&lg, WN(g), denom);
                    RowLerpStart(&lb, WN(b), denom);
                }
                if (textured) {
                    RowLerpStart(&lu, WN(u), denom);
                    RowLerpStart(&lv, WN(v), denom);
                }
            } else if (lerp) {
                if (gouraud) {
                    RowLerpStep(&lr, denom);
                    RowLerpStep(&lg, denom);
                    RowLerpStep(&lb, denom);
                }
                if (textured) {
                    RowLerpStep(&lu, denom);
                    RowLerpStep(&lv, denom);
                }
            }
            hit = true;
            int r = !gouraud ? a.r
                    : lerp   ? (int)lr.q
                             : (int)(WN(r) / denom);
            int g = !gouraud ? a.g
                    : lerp   ? (int)lg.q
                             : (int)(WN(g) / denom);
            int blue = !gouraud ? a.b
                       : lerp   ? (int)lb.q
                                : (int)(WN(b) / denom);
            u16 color;
            bool blend = semi;
            if (textured) {
                int u = lerp ? (int)lu.q : (int)(WN(u) / denom);
                int v = lerp ? (int)lv.q : (int)(WN(v) / denom);
#undef WN
                u16 texel = FetchTexel(&tex, u, v);
                if (texel == 0) {
                    continue;
                }
                blend = semi && (texel & 0x8000);
                color = keep_texel ? texel
                                   : ShadeTexel(texel, r, g, blue, raw_texture,
                                                x, y, dith && !raw_texture);
            } else {
                int d = dith && gouraud ? dither[y & 3][x & 3] : 0;
                color = PackColor(
                    (r + d) >> 3, (g + d) >> 3, (blue + d) >> 3, false);
            }
            while (run < nruns && x > runs[run].x1) {
                run++;
            }
            if (run < nruns && x >= runs[run].x0) {
                PlotPixel(&runs[run].row[x - runs[run].x0], color, blend, abr);
            }
        }
    }
}

static SwVertex VertexXY(u32 word, u32 color) {
    SwVertex v = {0};
    v.x = Sign11(word) + draw_offset.x;
    v.y = Sign11(word >> 16) + draw_offset.y;
    if (grid_source != grid_target) {
        v.x = draw_offset.x +
              (v.x - draw_offset.x) * (int)grid_target / (int)grid_source;
    }
    v.r = color & 0xff;
    v.g = (color >> 8) & 0xff;
    v.b = (color >> 16) & 0xff;
    return v;
}

static void DrawLine(SwVertex a, SwVertex b, bool semi, u16 tpage) {
    int dx = abs(b.x - a.x);
    int dy = abs(b.y - a.y);
    if (!dx && !dy) {
        WritePixel(a.x, a.y, PackColor(a.r >> 3, a.g >> 3, a.b >> 3, false),
                   semi, (tpage >> 5) & 3);
        return;
    }
    int ox, oy, ex, ey;
    if (dx >= dy) {
        ox = 0;
        oy = 1;
        ex = b.x >= a.x ? 1 : -1;
        ey = 0;
    } else {
        ox = 1;
        oy = 0;
        ex = 0;
        ey = b.y >= a.y ? 1 : -1;
    }
    SwVertex q0 = a;
    SwVertex q1 = b;
    SwVertex q2 = b;
    SwVertex q3 = a;
    q1.x += ex;
    q1.y += ey;
    q2.x += ex + ox;
    q2.y += ey + oy;
    q3.x += ox;
    q3.y += oy;
    RasterTriangle(q0, q1, q2, false, false, semi, true, tpage, 0, true);
    RasterTriangle(q0, q2, q3, false, false, semi, true, tpage, 0, true);
}

// Rectangles (GP0 0x60-0x7f). The same pixels in the same order (rows top to
// bottom, columns left to right) as testing each pixel of the rectangle
// against ClipX0/ClipX1 and PixelPtr, but clipped once per row (RowRuns), with
// the texture row, the CLUT row and ShadeTexel's modulation (a table per
// channel) looked up once, and an opaque untextured rectangle as a plain fill.
static void RasterSprite(
    const SwVertex* origin, int w, int source_w, int h, int u, int v,
    bool textured, bool raw_texture, bool semi, u16 clut) {
    int abr = (cur_tpage >> 5) & 3;
    if (w <= 0 || h <= 0) {
        return;
    }
    int ox = origin->x;
    int ox1 = ox + w - 1;
    bool scaled = source_w != w;
    u16 flat =
        PackColor(origin->r >> 3, origin->g >> 3, origin->b >> 3, false);
    u16 flat_out = (u16)((flat & 0x7fff) |
                         ((flat | (set_mask_bit ? 0x8000 : 0)) & 0x8000));
    bool flat_fill = !textured && !semi && !preserve_masked;
    u16 mod_r[32], mod_g[32], mod_b[32];
    int tx = (cur_tpage & 0xf) * 64;
    int ty0 = ((cur_tpage >> 4) & 1) * 256;
    int depth = (cur_tpage >> 7) & 3;
    const u16* clut_row = &vram[((clut >> 6) & 511) * VRAM_W];
    int clut_x = (clut & 0x3f) * 16;
    // ShadeTexel leaves a texel as it is at the neutral color (128, 128, 128)
    bool keep_texel = raw_texture || (origin->r == 128 && origin->g == 128 &&
                                      origin->b == 128);
    // the common case: no texture window, no scaling, opaque, no mask test,
    // texel kept; PlotPixel then only sets the mask bit
    bool plain = keep_texel && !scaled && !semi && !preserve_masked &&
                 twin_and_x == 0xff && twin_or_x == 0;
    u16 set_bit = set_mask_bit ? 0x8000 : 0;
    if (textured && !raw_texture) {
        for (int c = 0; c < 32; c++) {
            mod_r[c] = (u16)CLAMP((c * origin->r) >> 7, 0, 31);
            mod_g[c] = (u16)(CLAMP((c * origin->g) >> 7, 0, 31) << 5);
            mod_b[c] = (u16)(CLAMP((c * origin->b) >> 7, 0, 31) << 10);
        }
    }
    for (int py = 0; py < h; py++) {
        int y = origin->y + py;
        if (y < draw_area_start.y || y > draw_area_end.y || y < 0 ||
            y >= VRAM_H) {
            continue;
        }
        RowRun runs[3];
        int nruns = RowRuns(y, runs);
        const u16* trow = NULL;
        if (textured) {
            int tv = (((v + py) & twin_and_y) | twin_or_y) & 0xff;
            trow = &vram[(ty0 + tv) * VRAM_W];
        }
        for (int r = 0; r < nruns; r++) {
            int xa = runs[r].x0 > ox ? runs[r].x0 : ox;
            int xb = runs[r].x1 < ox1 ? runs[r].x1 : ox1;
            if (xa > xb) {
                continue;
            }
            u16* out = runs[r].row + (xa - runs[r].x0);
            if (!textured) {
                if (flat_fill) {
                    for (int x = xa; x <= xb; x++) {
                        *out++ = flat_out;
                    }
                } else {
                    for (int x = xa; x <= xb; x++) {
                        PlotPixel(out++, flat, semi, abr);
                    }
                }
                continue;
            }
            if (plain) {
                int su = (u + (xa - ox)) & 0xff;
                if (depth == 0) {
                    for (int x = xa; x <= xb; x++, out++, su = (su + 1) & 0xff) {
                        u16 packed = trow[(tx + su / 4) & 1023];
                        int index = (packed >> ((su & 3) * 4)) & 15;
                        u16 texel = clut_row[(clut_x + index) & 1023];
                        // transparent texels keep the pixel (no branch)
                        *out = texel ? (u16)(texel | set_bit) : *out;
                    }
                } else if (depth == 1) {
                    for (int x = xa; x <= xb; x++, out++, su = (su + 1) & 0xff) {
                        u16 packed = trow[(tx + su / 2) & 1023];
                        int index = (packed >> ((su & 1) * 8)) & 255;
                        u16 texel = clut_row[(clut_x + index) & 1023];
                        // transparent texels keep the pixel (no branch)
                        *out = texel ? (u16)(texel | set_bit) : *out;
                    }
                } else {
                    for (int x = xa; x <= xb; x++, out++, su = (su + 1) & 0xff) {
                        u16 texel = trow[(tx + su) & 1023];
                        // transparent texels keep the pixel (no branch)
                        *out = texel ? (u16)(texel | set_bit) : *out;
                    }
                }
                continue;
            }
            for (int x = xa; x <= xb; x++, out++) {
                int px = x - ox;
                int su = (((u + (scaled ? px * source_w / w : px)) &
                           twin_and_x) |
                          twin_or_x) &
                         0xff;
                u16 texel;
                if (depth == 0) {
                    u16 packed = trow[(tx + su / 4) & 1023];
                    int index = (packed >> ((su & 3) * 4)) & 15;
                    texel = clut_row[(clut_x + index) & 1023];
                } else if (depth == 1) {
                    u16 packed = trow[(tx + su / 2) & 1023];
                    int index = (packed >> ((su & 1) * 8)) & 255;
                    texel = clut_row[(clut_x + index) & 1023];
                } else {
                    texel = trow[(tx + su) & 1023];
                }
                if (texel == 0) {
                    continue;
                }
                bool blend = semi && (texel & 0x8000);
                if (!keep_texel) {
                    texel = (u16)(mod_r[texel & 31] | mod_g[(texel >> 5) & 31] |
                                  mod_b[(texel >> 10) & 31] | (texel & 0x8000));
                }
                PlotPixel(out, texel, blend, abr);
            }
        }
    }
}

static void Software_Reset(void) {
    cur_tpage = 0;
    draw_offset = (Point){0, 0};
    draw_area_start = (Point){0, 0};
    draw_area_end = (Point){VRAM_W - 1, VRAM_H - 1};
    set_mask_bit = preserve_masked = false;
    UpdateGutterActive();
}

static void Software_SetTexpageMode(ParamDrawTexpageMode* p) {
    u16 mode = *(u16*)p;
    cur_tpage = mode & 0x1ff;
    dither_enabled = (mode & 0x200) != 0;
}

static void Software_SetTextureWindow(unsigned int mask_x, unsigned int mask_y,
                                      unsigned int off_x, unsigned int off_y) {
    mask_x &= 31;
    mask_y &= 31;
    twin_and_x = (u8) ~(mask_x * 8);
    twin_and_y = (u8) ~(mask_y * 8);
    twin_or_x = (u8)((off_x & mask_x) * 8);
    twin_or_y = (u8)((off_y & mask_y) * 8);
}

static void Software_SetAreaStart(int x, int y) {
    draw_area_start = (Point){x, y};
    UpdateGutterActive();
}

static void Software_SetAreaEnd(int x, int y) {
    draw_area_end = (Point){x, y};
    UpdateGutterActive();
}

static void Software_SetOffset(int x, int y) {
    draw_offset = (Point){Sign11(x), Sign11(y)};
}

static int Software_SetHorizontalGrid(
    unsigned int source_width, unsigned int target_width) {
    if (!source_width || !target_width) {
        return -1;
    }
    grid_source = source_width;
    grid_target = target_width;
    return 0;
}

static void Software_SetMask(int bit0, int bit1) {
    set_mask_bit = bit0 != 0;
    preserve_masked = bit1 != 0;
}

static void Software_ClearImage(PS1_RECT* rect, u_char r, u_char g, u_char b) {
    u16 color = PackColor(r >> 3, g >> 3, b >> 3, set_mask_bit);
    int x0 = CLAMP(rect->x, 0, VRAM_W);
    int y0 = CLAMP(rect->y, 0, VRAM_H);
    int x1 = CLAMP(rect->x + rect->w, 0, VRAM_W);
    int y1 = CLAMP(rect->y + rect->h, 0, VRAM_H);
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            u16* p = &vram[y * VRAM_W + x];
            if (!preserve_masked || !(*p & 0x8000)) {
                *p = color;
            }
        }
    }
    // a clear of the whole gutter-sized display also clears its gutters
    if (gutter_px > 0 && rect->x == gutter_x0 && rect->w == gutter_w) {
        FillGutterRows(rect->y, rect->y + rect->h, color);
    }
}

static void Software_LoadImage(PS1_RECT* rect, u_long* data) {
    if (rect->w <= 0 || rect->h <= 0) {
        return;
    }
    const u16* src = (const u16*)data;
    if (rect->x >= 0 && rect->y >= 0 && rect->x + rect->w <= VRAM_W &&
        rect->y + rect->h <= VRAM_H) {
        for (int y = 0; y < rect->h; y++) {
            memcpy(&vram[(rect->y + y) * VRAM_W + rect->x], &src[y * rect->w],
                   (size_t)rect->w * sizeof(u16));
        }
        return;
    }
    for (int y = 0; y < rect->h; y++) {
        for (int x = 0; x < rect->w; x++) {
            int vx = rect->x + x;
            int vy = rect->y + y;
            if ((unsigned)vx < VRAM_W && (unsigned)vy < VRAM_H) {
                vram[vy * VRAM_W + vx] = src[y * rect->w + x];
            }
        }
    }
}

static void Software_StoreImage(PS1_RECT* rect, u_long* data) {
    if (rect->w <= 0 || rect->h <= 0) {
        return;
    }
    u16* dst = (u16*)data;
    if (rect->x >= 0 && rect->y >= 0 && rect->x + rect->w <= VRAM_W &&
        rect->y + rect->h <= VRAM_H) {
        for (int y = 0; y < rect->h; y++) {
            memcpy(&dst[y * rect->w], &vram[(rect->y + y) * VRAM_W + rect->x],
                   (size_t)rect->w * sizeof(u16));
        }
        return;
    }
    for (int y = 0; y < rect->h; y++) {
        for (int x = 0; x < rect->w; x++) {
            int vx = rect->x + x;
            int vy = rect->y + y;
            dst[y * rect->w + x] =
                ((unsigned)vx < VRAM_W && (unsigned)vy < VRAM_H)
                    ? vram[vy * VRAM_W + vx]
                    : 0;
        }
    }
}

static void Software_MoveImage(PS1_RECT* rect, unsigned int x, unsigned int y) {
    int src_x = CLAMP(rect->x, 0, VRAM_W - 1);
    int src_y = CLAMP(rect->y, 0, VRAM_H - 1);
    int dst_x = CLAMP((int)x, 0, VRAM_W - 1);
    int dst_y = CLAMP((int)y, 0, VRAM_H - 1);
    int max_w =
        VRAM_W - src_x < VRAM_W - dst_x ? VRAM_W - src_x : VRAM_W - dst_x;
    int max_h =
        VRAM_H - src_y < VRAM_H - dst_y ? VRAM_H - src_y : VRAM_H - dst_y;
    int w = CLAMP(rect->w, 0, max_w);
    int h = CLAMP(rect->h, 0, max_h);
    if (!w || !h) {
        return;
    }
    for (int row = 0; row < h; row++) {
        memmove(&vram[(dst_y + row) * VRAM_W + dst_x],
                &vram[(src_y + row) * VRAM_W + src_x], (size_t)w * sizeof(u16));
    }
}

static int Software_PushPrim(u_long* words, int max_len) {
    if (max_len <= 0) {
        return 0;
    }
    int code = (int)(words[0] >> 24);
    bool textured = (code & TEXTURED) != 0;
    bool gouraud = (code & GOURAUD) != 0;
    bool semi = (code & SEMITRANSP) != 0;
    bool raw_texture = textured && (code & 1);
    bool polygon = !(code & 0x40);
    bool line = (code & 0x40) && !(code & 0x20);
    int at = 1;
    if (polygon && (code & TRIANGLE)) {
        int count = (code & EXTRA_VERTEX) ? 4 : 3;
        int required = 1 + count * (1 + textured) + (count - 1) * gouraud;
        if (max_len < required) {
            return max_len;
        }
        SwVertex v[4] = {0};
        u16 clut = 0;
        u16 tpage = cur_tpage;
        u32 color = words[0];
        for (int i = 0; i < count && at < max_len; i++) {
            v[i] = VertexXY(words[at++], color);
            if (textured && at < max_len) {
                u32 uv = words[at++];
                v[i].u = uv & 0xff;
                v[i].v = (uv >> 8) & 0xff;
                if (i == 0) {
                    clut = uv >> 16;
                } else if (i == 1) {
                    tpage = uv >> 16;
                }
            }
            if (gouraud && i + 1 < count && at < max_len) {
                color = words[at++];
            }
        }
        RasterTriangle(v[0], v[1], v[2], textured, raw_texture, semi, gouraud,
                       tpage, clut, false);
        if (count == 4) {
            RasterTriangle(v[1], v[3], v[2], textured, raw_texture, semi,
                           gouraud, tpage, clut, false);
        }
        return at;
    }
    if (line) {
        int points = ((code >> 2) & 3) + 1;
        bool padding = points != 1;
        if (points == 1) {
            points = 2;
        }
        int required = 1 + points + (points - 1) * gouraud + padding;
        if (max_len < required) {
            return max_len;
        }
        SwVertex prev = {0};
        u32 color = words[0];
        for (int i = 0; i < points && at < max_len; i++) {
            SwVertex cur = VertexXY(words[at++], color);
            if (i) {
                DrawLine(prev, cur, semi, cur_tpage);
            }
            prev = cur;
            if (gouraud && i + 1 < points && at < max_len) {
                color = words[at++];
            }
        }
        if (padding && at < max_len) {
            at++;
        }
        return at;
    }
    if ((code & 0x60) == 0x60) {
        int required = 2 + textured + ((code & 0x18) == 0);
        if (max_len < required) {
            return max_len;
        }
        SwVertex origin = VertexXY(words[at++], words[0]);
        u16 clut = 0;
        int u = 0, v = 0;
        if (textured && at < max_len) {
            u32 uv = words[at++];
            u = uv & 0xff;
            v = (uv >> 8) & 0xff;
            clut = uv >> 16;
        }
        int w, h;
        switch (code & ~3) {
        case 0x60:
        case 0x64:
            w = (short)(words[at] & 0xffff);
            h = (short)(words[at++] >> 16);
            break;
        case 0x68:
        case 0x6c:
            w = h = 1;
            break;
        case 0x70:
        case 0x74:
            w = h = 8;
            break;
        default:
            w = h = 16;
            break;
        }
        // a background clear covering the drawing area covers the gutters too
        if (gutter_active && !textured && origin.x == draw_area_start.x &&
            w == draw_area_end.x - draw_area_start.x + 1) {
            origin.x -= gutter_px;
            w += gutter_px * 2;
        }
        int source_w = w;
        if (grid_source != grid_target) {
            w = w * (int)grid_target / (int)grid_source;
        }
        RasterSprite(
            &origin, w, source_w, h, u, v, textured, raw_texture, semi, clut);
        return at;
    }
    WARNF("unsupported GP0 primitive %02x", code);
    return 1;
}

void SoftwareRasterizer_Reset(void) {
    memset(vram, 0, sizeof(vram));
    Software_Reset();
    memset(gutter_planes, 0, sizeof(gutter_planes));
    gutter_px = gutter_x0 = gutter_w = 0;
    gutter_active = false;
    grid_source = grid_target = 1;
    twin_and_x = twin_and_y = 0xff;
    twin_or_x = twin_or_y = 0;
    dither_enabled = false;
}

const u16* SoftwareRasterizer_GetVram(void) { return vram; }

void SoftwareRasterizer_SetGutter(int px, int x0, int w, int h) {
    px = CLAMP(px, 0, PSYZ_GUTTER_MAX);
    if (px != gutter_px) {
        FillGutterRows(0, VRAM_H, 0);
    }
    gutter_px = px;
    gutter_x0 = x0;
    gutter_w = w;
    (void)h; // the display height; any band of rows takes the gutters
    UpdateGutterActive();
}

int SoftwareRasterizer_GetGutter(void) { return gutter_px; }

const u16* SoftwareRasterizer_GetGutterPlane(int side) {
    return gutter_planes[side ? 1 : 0];
}

void SoftwareRasterizer_ClearGutterRows(int y0, int y1) {
    FillGutterRows(y0, y1, 0);
}

static void Software_FlushBuffer(void) {}
static void Software_ResetBuffer(void) {}
static int Software_ExequeSync(void) { return 0; }

const DrawBackend software_rasterizer = {
    .reset = Software_Reset,
    .texpage_mode = Software_SetTexpageMode,
    .texture_window = Software_SetTextureWindow,
    .area_start = Software_SetAreaStart,
    .area_end = Software_SetAreaEnd,
    .offset = Software_SetOffset,
    .horizontal_grid = Software_SetHorizontalGrid,
    .mask = Software_SetMask,
    .clear_image = Software_ClearImage,
    .load_image = Software_LoadImage,
    .store_image = Software_StoreImage,
    .move_image = Software_MoveImage,
    .reset_buffer = Software_ResetBuffer,
    .flush_buffer = Software_FlushBuffer,
    .push_prim = Software_PushPrim,
    .exeque_sync = Software_ExequeSync,
};
