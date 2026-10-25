// Deferred rasterization for sdl3_software.c (included by it, same file scope).
//
// While deferring is on (SoftwareRasterizer_SetDefer), a primitive is not
// drawn: its packet and the drawing state it needs are queued in a "layer"
// together with its footprint (the VRAM rectangle and gutter rows it can
// write, and the VRAM blocks it can read). A layer is a run of primitives
// under one drawing area. Nothing is lost: a layer is drawn, in order, as soon
// as anything could tell the difference, and the only layers that are never
// drawn are those a later opaque rectangle covers completely before anything
// looked at them (a field frame that turbo does not present: the next frame
// into the same buffer starts with a full-screen clear).
//
// "Anything could tell the difference" is exact, not heuristic:
//   - an image op (LoadImage, ClearImage, MoveImage, StoreImage), a gutter
//     row clear or a present reading VRAM, whose read or write set meets the
//     footprint of a layer, draws that layer first; the layers it depends on
//     (older layers that overlap it) are drawn with it, in their order;
//   - a layer that reads VRAM blocks (textures, CLUTs) that an op is about to
//     write is drawn first;
//   - an op that overwrites the whole footprint of a layer unconditionally
//     (LoadImage, MoveImage, ClearImage without mask test, an opaque
//     rectangle) removes it instead;
//   - everything else (the draw state, the dither mode) is captured per
//     primitive, so a layer draws the same pixels whenever it runs.
// The footprints are supersets of what the rasterizer can touch (DefFootprint
// parses a packet the way Software_PushPrimNow does). The fuzz in
// tools/soft_raster_bench.c compares every observed rectangle with the plain
// rasterizer.

typedef struct {
    int x0, y0, x1, y1; // inclusive; empty when x1 < x0 or y1 < y0
} IRect;

#define IR_EMPTY ((IRect){0, 0, -1, -1})

static inline bool IR_IsEmpty(IRect r) { return r.x1 < r.x0 || r.y1 < r.y0; }

static inline bool IR_Hit(IRect a, IRect b) {
    return !IR_IsEmpty(a) && !IR_IsEmpty(b) && a.x0 <= b.x1 && b.x0 <= a.x1 &&
           a.y0 <= b.y1 && b.y0 <= a.y1;
}

// a is inside b (an empty a is inside anything)
static inline bool IR_Inside(IRect a, IRect b) {
    return IR_IsEmpty(a) || (!IR_IsEmpty(b) && a.x0 >= b.x0 && a.x1 <= b.x1 &&
                             a.y0 >= b.y0 && a.y1 <= b.y1);
}

static inline IRect IR_Union(IRect a, IRect b) {
    if (IR_IsEmpty(a)) {
        return b;
    }
    if (IR_IsEmpty(b)) {
        return a;
    }
    return (IRect){a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0,
                   a.x1 > b.x1 ? a.x1 : b.x1, a.y1 > b.y1 ? a.y1 : b.y1};
}

// ---- VRAM blocks read by a layer: 16x16 pixel blocks, one uint64_t per block row

#define DEF_BLK_ROWS (VRAM_H / 16)

static inline uint64_t BlockMask(int x0, int x1) {
    int b0 = x0 >> 4;
    int b1 = x1 >> 4;
    return (~0ull >> (63 - b1)) & (~0ull << b0);
}

static void BlocksMark(uint64_t* bm, IRect r) {
    if (IR_IsEmpty(r)) {
        return;
    }
    uint64_t m = BlockMask(r.x0, r.x1);
    for (int by = r.y0 >> 4; by <= r.y1 >> 4; by++) {
        bm[by] |= m;
    }
}

static bool BlocksHit(const uint64_t* bm, IRect r) {
    if (IR_IsEmpty(r)) {
        return false;
    }
    uint64_t m = BlockMask(r.x0, r.x1);
    for (int by = r.y0 >> 4; by <= r.y1 >> 4; by++) {
        if (bm[by] & m) {
            return true;
        }
    }
    return false;
}

// ---- drawing state captured with each queued primitive

typedef struct {
    Point offset, area_start, area_end;
    unsigned grid_source, grid_target;
    int gutter_px, gutter_x0, gutter_w;
    u16 tpage;
    u8 twin_and_x, twin_and_y, twin_or_x, twin_or_y;
    bool set_mask, preserve_masked, dither_enabled, dither_on, gutter_active;
} DefSnap;

static void DefCapture(DefSnap* s) {
    memset(s, 0, sizeof(*s));
    s->offset = draw_offset;
    s->area_start = draw_area_start;
    s->area_end = draw_area_end;
    s->grid_source = grid_source;
    s->grid_target = grid_target;
    s->gutter_px = gutter_px;
    s->gutter_x0 = gutter_x0;
    s->gutter_w = gutter_w;
    s->tpage = cur_tpage;
    s->twin_and_x = twin_and_x;
    s->twin_and_y = twin_and_y;
    s->twin_or_x = twin_or_x;
    s->twin_or_y = twin_or_y;
    s->set_mask = set_mask_bit;
    s->preserve_masked = preserve_masked;
    s->dither_enabled = dither_enabled;
    s->dither_on = SoftwareDither() != 0;
    s->gutter_active = gutter_active;
}

// gutters: false draws the snapshot without touching the gutter planes
static void DefLoad(const DefSnap* s, bool gutters) {
    draw_offset = s->offset;
    draw_area_start = s->area_start;
    draw_area_end = s->area_end;
    grid_source = s->grid_source;
    grid_target = s->grid_target;
    gutter_px = s->gutter_px;
    gutter_x0 = s->gutter_x0;
    gutter_w = s->gutter_w;
    cur_tpage = s->tpage;
    twin_and_x = s->twin_and_x;
    twin_and_y = s->twin_and_y;
    twin_or_x = s->twin_or_x;
    twin_or_y = s->twin_or_y;
    set_mask_bit = s->set_mask;
    preserve_masked = s->preserve_masked;
    dither_enabled = s->dither_enabled;
    gutter_active = s->gutter_active && gutters;
}

// ---- layers

#define DEF_MAX_LAYERS 12
#define DEF_MAX_WORDS (1u << 20)
#define DEF_MAX_SNAPS 60000
#define DEF_MAX_SRCR 32
#define DEF_UNDO_MAX_PX (1 << 16)      // one saved rectangle
#define DEF_UNDO_MAX_TOTAL_PX (1 << 18) // all of a layer's

// The VRAM under a rectangle as it was before an image op overwrote it. A
// layer that reads the rectangle keeps this instead of being drawn first: when
// it is drawn, the rectangle is put back for the duration.
typedef struct {
    IRect r;
    uint16_t* pix;
} DefUndo;

typedef struct {
    bool used;
    bool closed;   // takes no more primitives
    bool g_alive;  // still has gutter plane writes to do
    Point area_start, area_end;
    u_long* buf;
    size_t len, cap;
    DefSnap* snaps;
    int nsnaps, cap_snaps;
    int nprims;
    IRect d;       // VRAM bounding box of everything the layer can write
    int g0, g1;    // gutter plane rows it can write; g1 < g0: none
    uint64_t src[DEF_BLK_ROWS]; // blocks it can read (coarse)
    IRect srcr[DEF_MAX_SRCR];   // the rectangles it reads, exactly,
    int nsrcr;                  // unless src_over: then only the blocks
    bool src_over;
    DefUndo* undo;              // what VRAM held under later writes to its sources
    int nundo, cap_undo;
    long undo_px;
} DefLayer;

static DefLayer def_pool[DEF_MAX_LAYERS];
static DefLayer* def_layers[DEF_MAX_LAYERS]; // oldest first
static int def_n;
static bool def_on;

// counters for the log (SoftwareRasterizer_GetDeferStats)
static int def_cause; // 0: other, 2: a present's SyncRect
static unsigned long def_stat_queued, def_stat_dropped, def_stat_run,
    def_stat_sync_layers, def_stat_other_layers;

static inline bool GRowsHit(int a0, int a1, int b0, int b1) {
    return a0 <= a1 && b0 <= b1 && a0 <= b1 && b0 <= a1;
}

static bool LayerSrcHit(const DefLayer* l, IRect r) {
    if (!BlocksHit(l->src, r)) {
        return false;
    }
    if (l->src_over) {
        return true;
    }
    for (int k = 0; k < l->nsrcr; k++) {
        if (IR_Hit(l->srcr[k], r)) {
            return true;
        }
    }
    return false;
}

static void LayerSrcAdd(DefLayer* l, IRect r) {
    if (IR_IsEmpty(r)) {
        return;
    }
    BlocksMark(l->src, r);
    if (l->src_over) {
        return;
    }
    for (int k = 0; k < l->nsrcr; k++) {
        if (IR_Inside(r, l->srcr[k])) {
            return;
        }
    }
    if (l->nsrcr < DEF_MAX_SRCR) {
        l->srcr[l->nsrcr++] = r;
    } else {
        l->src_over = true;
    }
}

static void DefLayerFree(DefLayer* l) {
    for (int k = 0; k < l->nundo; k++) {
        free(l->undo[k].pix);
        l->undo[k].pix = NULL;
    }
    l->nundo = 0;
    l->undo_px = 0;
    l->used = false;
    l->len = 0;
    l->nsnaps = 0;
    l->nprims = 0;
}

static void DefRemove(int i) {
    DefLayerFree(def_layers[i]);
    for (int j = i + 1; j < def_n; j++) {
        def_layers[j - 1] = def_layers[j];
    }
    def_n--;
}

// older a, newer b: must a be drawn before b is?
static bool DefConflict(const DefLayer* a, const DefLayer* b) {
    if (IR_Hit(a->d, b->d) || LayerSrcHit(b, a->d) || LayerSrcHit(a, b->d)) {
        return true;
    }
    return a->g_alive && b->g_alive && GRowsHit(a->g0, a->g1, b->g0, b->g1);
}



#ifdef DEFER_CHECK
static int DefCheckedPush(u_long* w, int n);
#define DEF_PUSH_NOW(w, n) DefCheckedPush((w), (n))
#else
#define DEF_PUSH_NOW(w, n) Software_PushPrimNow((w), (n))
#endif

static void DefRunLayer(DefLayer* l) {
    DefSnap saved;
    int cur = -1;
    size_t at = 0;

    uint16_t* keep[16] = {0};

    DefCapture(&saved);
    // put the rectangles later image ops overwrote back, newest write first
    for (int k = 0; k < l->nundo && k < 16; k++) {
        const IRect* r = &l->undo[k].r;
        size_t w = (size_t)(r->x1 - r->x0 + 1);
        size_t h = (size_t)(r->y1 - r->y0 + 1);

        keep[k] = malloc(w * h * sizeof(u16));
        for (size_t y = 0; keep[k] && y < h; y++) {
            memcpy(keep[k] + y * w, &vram[(r->y0 + (int)y) * VRAM_W + r->x0],
                   w * sizeof(u16));
        }
    }
    for (int k = l->nundo - 1; k >= 0; k--) {
        const IRect* r = &l->undo[k].r;
        size_t w = (size_t)(r->x1 - r->x0 + 1);
        size_t h = (size_t)(r->y1 - r->y0 + 1);

        for (size_t y = 0; y < h; y++) {
            memcpy(&vram[(r->y0 + (int)y) * VRAM_W + r->x0],
                   l->undo[k].pix + y * w, w * sizeof(u16));
        }
    }
    while (at < l->len) {
        u_long h = l->buf[at++];
        int n = (int)(h & 0xffff);
        int si = (int)(h >> 16);
        if (si != cur) {
            DefLoad(&l->snaps[si], l->g_alive);
            def_dither_override = l->snaps[si].dither_on ? 1 : 0;
            cur = si;
        }
        DEF_PUSH_NOW(&l->buf[at], n);
        at += (size_t)n;
        def_stat_run++;
    }
    def_dither_override = -1;
    DefLoad(&saved, true);
    for (int k = 0; k < l->nundo && k < 16; k++) {
        const IRect* r = &l->undo[k].r;
        size_t w = (size_t)(r->x1 - r->x0 + 1);
        size_t h = (size_t)(r->y1 - r->y0 + 1);

        for (size_t y = 0; keep[k] && y < h; y++) {
            memcpy(&vram[(r->y0 + (int)y) * VRAM_W + r->x0], keep[k] + y * w,
                   w * sizeof(u16));
        }
        free(keep[k]);
    }
}

// Draws the layers in sel and, first, every older layer one of them depends
// on, oldest first, and drops them from the queue. Layers in drop (killed
// by an overwrite) are dropped unless the closure draws them.
static void DefFlushSet(bool* sel, const bool* drop, const bool* suppress) {
    for (int i = def_n - 1; i >= 0; i--) {
        if (!sel[i]) {
            continue;
        }
        for (int j = 0; j < i; j++) {
            if (!sel[j] && DefConflict(def_layers[j], def_layers[i])) {
                sel[j] = true;
            }
        }
    }
    for (int i = 0; i < def_n; i++) {
        if (sel[i]) {
            DefRunLayer(def_layers[i]);
            if (def_cause == 2) {
                def_stat_sync_layers++;
            } else {
                def_stat_other_layers++;
            }
        }
    }
    // remove from the back so indexes stay valid
    for (int i = def_n - 1; i >= 0; i--) {
        if (sel[i]) {
            DefRemove(i);
        } else if (drop && drop[i]) {
            def_stat_dropped += (unsigned long)def_layers[i]->nprims;
            DefRemove(i);
        } else if (suppress && suppress[i]) {
            def_layers[i]->g_alive = false;
            def_layers[i]->closed = true;
        }
    }
}

static void DefSyncAll(void) {
    bool sel[DEF_MAX_LAYERS];

    if (!def_n) {
        return;
    }
    for (int i = 0; i < def_n; i++) {
        sel[i] = true;
    }
    DefFlushSet(sel, NULL, NULL);
}

static void DefDiscardAll(void) {
    while (def_n) {
        DefRemove(def_n - 1);
    }
}

// What an op that runs now does to VRAM and the gutter planes.
typedef struct {
    IRect r[3];      // VRAM it reads
    int nr;
    IRect w;         // VRAM it writes
    bool w_cover;    // ... and every pixel of w is overwritten
    int gw0, gw1;    // gutter plane rows it overwrites completely; gw1 < gw0: none
} DefOpDesc;

static void DefOp(const DefOpDesc* o) {
    bool sel[DEF_MAX_LAYERS] = {0};
    bool drop[DEF_MAX_LAYERS] = {0};
    bool supp[DEF_MAX_LAYERS] = {0};
    bool undo[DEF_MAX_LAYERS] = {0};
    DefLayer* undo_l[DEF_MAX_LAYERS];
    int nundo = 0;
    long w_px = IR_IsEmpty(o->w)
                    ? 0
                    : (long)(o->w.x1 - o->w.x0 + 1) * (o->w.y1 - o->w.y0 + 1);

    for (int i = 0; i < def_n; i++) {
        const DefLayer* l = def_layers[i];
        bool vhit = IR_Hit(o->w, l->d);
        bool shit = LayerSrcHit(l, o->w);
        bool rhit = false;
        for (int k = 0; k < o->nr; k++) {
            rhit |= IR_Hit(o->r[k], l->d);
        }
        bool ghit = l->g_alive && GRowsHit(l->g0, l->g1, o->gw0, o->gw1);
        bool vcov = !vhit || (o->w_cover && IR_Inside(l->d, o->w));
        bool gcov = !ghit || (l->g0 >= o->gw0 && l->g1 <= o->gw1);

        if (rhit || (vhit && !vcov) || (ghit && !gcov)) {
            sel[i] = true;
        } else if (vhit) {
            // the whole VRAM footprint is overwritten; the layer can go if it
            // has no gutter writes that survive
            if (l->g_alive && !ghit) {
                sel[i] = true;
            } else {
                drop[i] = true;
            }
        } else {
            if (ghit) {
                supp[i] = true;
            }
            if (shit) {
                // the op overwrites VRAM the layer samples: keep the old
                // pixels for it rather than drawing it now, unless that is
                // too much to keep
                if (w_px <= DEF_UNDO_MAX_PX &&
                    l->undo_px + w_px <= DEF_UNDO_MAX_TOTAL_PX &&
                    l->nundo < 16) {
                    undo[i] = true;
                } else {
                    sel[i] = true;
                    supp[i] = false;
                }
            }
        }
    }
    // a layer is dropped only if no layer that stays reads what it draws
    for (int i = def_n - 1; i >= 0; i--) {
        if (!drop[i]) {
            continue;
        }
        for (int j = i + 1; j < def_n; j++) {
            if (!drop[j] && LayerSrcHit(def_layers[j], def_layers[i]->d)) {
                drop[i] = false;
                sel[i] = true;
                break;
            }
        }
    }
    for (int i = 0; i < def_n; i++) {
        if (undo[i] && !sel[i]) {
            undo_l[nundo++] = def_layers[i];
        }
    }
    DefFlushSet(sel, drop, supp);
    // the layers that were drawn instead (the closure) need nothing; the
    // others keep the pixels the op is about to overwrite
    for (int k = 0; k < nundo; k++) {
        DefLayer* l = undo_l[k];
        size_t w = (size_t)(o->w.x1 - o->w.x0 + 1);
        size_t h = (size_t)(o->w.y1 - o->w.y0 + 1);
        DefUndo* u;

        if (!l->used) {
            continue; // drawn (or dropped) by the closure
        }
        if (l->nundo == l->cap_undo) {
            int cap = l->cap_undo ? l->cap_undo * 2 : 4;
            DefUndo* nu = realloc(l->undo, (size_t)cap * sizeof(DefUndo));

            if (!nu) {
                continue;
            }
            l->undo = nu;
            l->cap_undo = cap;
        }
        u = &l->undo[l->nundo];
        u->r = o->w;
        u->pix = malloc(w * h * sizeof(u16));
        if (!u->pix) {
            continue;
        }
        for (size_t y = 0; y < h; y++) {
            memcpy(u->pix + y * w, &vram[(o->w.y0 + (int)y) * VRAM_W + o->w.x0],
                   w * sizeof(u16));
        }
        l->nundo++;
        l->undo_px += (long)(w * h);
        l->closed = true;
    }
}

// ---- footprints

enum { FP_DRAWS, FP_NOOP, FP_UNSUPPORTED };

typedef struct {
    int kind;
    int n;           // words consumed
    IRect v;         // VRAM it can write
    int g0, g1;      // gutter rows it can write
    bool cover;      // opaque rectangle: v is overwritten completely
    bool cover_g;    // ... and the gutter rows g0..g1 with it
    IRect src[2];
    int nsrc;
} DefFp;

static IRect AreaVram(void) {
    return (IRect){draw_area_start.x > 0 ? draw_area_start.x : 0,
                   draw_area_start.y > 0 ? draw_area_start.y : 0,
                   draw_area_end.x < VRAM_W - 1 ? draw_area_end.x : VRAM_W - 1,
                   draw_area_end.y < VRAM_H - 1 ? draw_area_end.y
                                                : VRAM_H - 1};
}

// the destination of a primitive whose pixels lie in [minx, maxx] x [miny,
// maxy]
static void FpDest(DefFp* fp, int minx, int miny, int maxx, int maxy) {
    IRect a = AreaVram();
    int ry0 = miny > a.y0 ? miny : a.y0;
    int ry1 = maxy < a.y1 ? maxy : a.y1;

    fp->v = IR_EMPTY;
    fp->g0 = 0;
    fp->g1 = -1;
    if (minx > maxx || ry0 > ry1) {
        return;
    }
    IRect v = {minx > a.x0 ? minx : a.x0, ry0, maxx < a.x1 ? maxx : a.x1, ry1};
    fp->v = IR_IsEmpty(v) ? IR_EMPTY : v;
    if (gutter_active &&
        ((maxx >= draw_area_start.x - gutter_px &&
          minx <= draw_area_start.x - 1) ||
         (maxx >= draw_area_end.x + 1 &&
          minx <= draw_area_end.x + gutter_px))) {
        fp->g0 = ry0;
        fp->g1 = ry1;
    }
}

// The VRAM a textured primitive can sample: the texture page rows and columns
// for texels u0..u1, v0..v1 and the CLUT row.
static void FpTexture(DefFp* fp, u16 tpage, u16 clut, int u0, int u1, int v0,
                      int v1) {
    bool twin = twin_and_x != 0xff || twin_and_y != 0xff || twin_or_x ||
                twin_or_y;
    int tx = (tpage & 0xf) * 64;
    int ty0 = ((tpage >> 4) & 1) * 256;
    int depth = (tpage >> 7) & 3;
    int x0, x1;

    if (twin || u0 < 0 || u1 > 255) {
        u0 = 0;
        u1 = 255;
    }
    if (twin || v0 < 0 || v1 > 255) {
        v0 = 0;
        v1 = 255;
    }
    if (depth == 0) {
        x0 = tx + u0 / 4;
        x1 = tx + u1 / 4;
    } else if (depth == 1) {
        x0 = tx + u0 / 2;
        x1 = tx + u1 / 2;
    } else {
        x0 = tx + u0;
        x1 = tx + u1;
    }
    if (x1 > VRAM_W - 1) {
        x0 = 0;
        x1 = VRAM_W - 1;
    }
    fp->src[fp->nsrc++] = (IRect){x0, ty0 + v0, x1, ty0 + v1};
    if (depth < 2) {
        int cx = (clut & 0x3f) * 16;
        int cx1 = cx + (depth == 0 ? 15 : 255);
        int cy = (clut >> 6) & 511;

        if (cx1 > VRAM_W - 1) {
            cx = 0;
            cx1 = VRAM_W - 1;
        }
        fp->src[fp->nsrc++] = (IRect){cx, cy, cx1, cy};
    }
}

// Mirrors Software_PushPrimNow's parsing: the same words consumed, the same
// vertices, a superset of the pixels written and the VRAM read.
static void DefFootprint(const u_long* words, int max_len, DefFp* fp) {
    int code = (int)(words[0] >> 24);
    bool textured = (code & TEXTURED) != 0;
    bool gouraud = (code & GOURAUD) != 0;
    bool semi = (code & SEMITRANSP) != 0;
    bool polygon = !(code & 0x40);
    bool line = (code & 0x40) && !(code & 0x20);
    int at = 1;

    memset(fp, 0, sizeof(*fp));
    fp->v = IR_EMPTY;
    fp->g1 = -1;
    fp->kind = FP_DRAWS;
    if (polygon && (code & TRIANGLE)) {
        int count = (code & EXTRA_VERTEX) ? 4 : 3;
        int required = 1 + count * (1 + textured) + (count - 1) * gouraud;
        if (max_len < required) {
            fp->kind = FP_NOOP;
            fp->n = max_len;
            return;
        }
        int minx = 1 << 30, maxx = -(1 << 30), miny = 1 << 30,
            maxy = -(1 << 30);
        int u0 = 1 << 30, u1 = -1, v0 = 1 << 30, v1 = -1;
        u16 clut = 0;
        u16 tpage = cur_tpage;
        u32 color = (u32)words[0];
        for (int i = 0; i < count && at < max_len; i++) {
            SwVertex v = VertexXY((u32)words[at++], color);
            minx = v.x < minx ? v.x : minx;
            maxx = v.x > maxx ? v.x : maxx;
            miny = v.y < miny ? v.y : miny;
            maxy = v.y > maxy ? v.y : maxy;
            if (textured && at < max_len) {
                u32 uv = (u32)words[at++];
                int u = uv & 0xff;
                int vv = (uv >> 8) & 0xff;
                u0 = u < u0 ? u : u0;
                u1 = u > u1 ? u : u1;
                v0 = vv < v0 ? vv : v0;
                v1 = vv > v1 ? vv : v1;
                if (i == 0) {
                    clut = uv >> 16;
                } else if (i == 1) {
                    tpage = uv >> 16;
                }
            }
            if (gouraud && i + 1 < count && at < max_len) {
                color = (u32)words[at++];
            }
        }
        fp->n = at;
        FpDest(fp, minx, miny, maxx, maxy);
        if (textured && u1 >= 0) {
            FpTexture(fp, tpage, clut, u0, u1, v0, v1);
        }
        return;
    }
    if (line) {
        int points = ((code >> 2) & 3) + 1;
        bool padding = points != 1;
        if (points == 1) {
            points = 2;
        }
        int required = 1 + points + (points - 1) * gouraud + padding;
        if (max_len < required) {
            fp->kind = FP_NOOP;
            fp->n = max_len;
            return;
        }
        int minx = 1 << 30, maxx = -(1 << 30), miny = 1 << 30,
            maxy = -(1 << 30);
        u32 color = (u32)words[0];
        for (int i = 0; i < points && at < max_len; i++) {
            SwVertex cur = VertexXY((u32)words[at++], color);
            minx = cur.x < minx ? cur.x : minx;
            maxx = cur.x > maxx ? cur.x : maxx;
            miny = cur.y < miny ? cur.y : miny;
            maxy = cur.y > maxy ? cur.y : maxy;
            if (gouraud && i + 1 < points && at < max_len) {
                color = (u32)words[at++];
            }
        }
        if (padding && at < max_len) {
            at++;
        }
        fp->n = at;
        // a line is two thin triangles around its end points
        FpDest(fp, minx - 2, miny - 2, maxx + 2, maxy + 2);
        return;
    }
    if ((code & 0x60) == 0x60) {
        int required = 2 + textured + ((code & 0x18) == 0);
        if (max_len < required) {
            fp->kind = FP_NOOP;
            fp->n = max_len;
            return;
        }
        SwVertex origin = VertexXY((u32)words[at++], (u32)words[0]);
        int u = 0, v = 0;
        u16 clut = 0;
        if (textured && at < max_len) {
            u32 uv = (u32)words[at++];
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
        fp->n = at;
        if (gutter_active && !textured && origin.x == draw_area_start.x &&
            w == draw_area_end.x - draw_area_start.x + 1) {
            origin.x -= gutter_px;
            w += gutter_px * 2;
        }
        int source_w = w;
        if (grid_source != grid_target) {
            w = w * (int)grid_target / (int)grid_source;
        }
        if (w <= 0 || h <= 0) {
            return; // RasterSprite draws nothing
        }
        FpDest(fp, origin.x, origin.y, origin.x + w - 1, origin.y + h - 1);
        if (textured) {
            int span = source_w != w ? source_w : w;
            FpTexture(fp, cur_tpage, clut, u, u + span - 1, v, v + h - 1);
        } else if (!semi && !preserve_masked) {
            fp->cover = !IR_IsEmpty(fp->v);
            // gutter rows are overwritten from the left to the right edge
            // of both gutters
            fp->cover_g = fp->g1 >= fp->g0 &&
                          origin.x <= draw_area_start.x - gutter_px &&
                          origin.x + w - 1 >= draw_area_end.x + gutter_px;
        }
        return;
    }
    fp->kind = FP_UNSUPPORTED;
    fp->n = 1;
}

#ifdef DEFER_CHECK
#include <stdio.h>
static uint32_t def_chk_rng = 2463534242u;
static uint32_t DefChkRnd(void) {
    def_chk_rng ^= def_chk_rng << 13;
    def_chk_rng ^= def_chk_rng >> 17;
    def_chk_rng ^= def_chk_rng << 5;
    return def_chk_rng;
}

static void DefChkFail(const char* what, const u_long* w, int n) {
    fprintf(stderr, "DEFER_CHECK: %s (prim %08lx, %d words)\n", what,
            (unsigned long)w[0], n);
    abort();
}

static int DefCheckedPush(u_long* w, int n) {
    static u16 v0[VRAM_W * VRAM_H], v1[VRAM_W * VRAM_H];
    static u16 g0[2][VRAM_H * PSYZ_GUTTER_MAX], g1[2][VRAM_H * PSYZ_GUTTER_MAX];
    DefFp fp;
    int used;

    DefFootprint(w, n, &fp);
    memcpy(v0, vram, sizeof(v0));
    memcpy(g0, gutter_planes, sizeof(g0));
    used = Software_PushPrimNow(w, n);
    if (fp.kind == FP_DRAWS && used != fp.n) {
        DefChkFail("consumed words differ from the footprint parse", w, n);
    }
    if (fp.kind != FP_DRAWS) {
        return used;
    }
    for (int y = 0; y < VRAM_H; y++) {
        for (int x = 0; x < VRAM_W; x++) {
            if (vram[y * VRAM_W + x] != v0[y * VRAM_W + x] &&
                !(x >= fp.v.x0 && x <= fp.v.x1 && y >= fp.v.y0 &&
                  y <= fp.v.y1)) {
                DefChkFail("wrote VRAM outside the predicted rectangle", w, n);
            }
        }
        for (int s = 0; s < 2; s++) {
            for (int x = 0; x < PSYZ_GUTTER_MAX; x++) {
                if (gutter_planes[s][y * PSYZ_GUTTER_MAX + x] !=
                        g0[s][y * PSYZ_GUTTER_MAX + x] &&
                    !(y >= fp.g0 && y <= fp.g1)) {
                    DefChkFail("wrote gutter rows outside the prediction", w,
                               n);
                }
            }
        }
    }
    memcpy(v1, vram, sizeof(v1));
    memcpy(g1, gutter_planes, sizeof(g1));
    memcpy(vram, v0, sizeof(v0));
    memcpy(gutter_planes, g0, sizeof(g0));
    for (int y = 0; y < VRAM_H; y++) {
        for (int x = 0; x < VRAM_W; x++) {
            bool keep = x >= fp.v.x0 && x <= fp.v.x1 && y >= fp.v.y0 &&
                        y <= fp.v.y1;
            for (int k = 0; k < fp.nsrc; k++) {
                keep |= x >= fp.src[k].x0 && x <= fp.src[k].x1 &&
                        y >= fp.src[k].y0 && y <= fp.src[k].y1;
            }
            if (!keep) {
                vram[y * VRAM_W + x] = (u16)DefChkRnd();
            }
        }
    }
    Software_PushPrimNow(w, n);
    for (int y = fp.v.y0; y <= fp.v.y1; y++) {
        for (int x = fp.v.x0; x <= fp.v.x1; x++) {
            if (vram[y * VRAM_W + x] != v1[y * VRAM_W + x]) {
                DefChkFail("pixels depend on VRAM outside the predicted source",
                           w, n);
            }
        }
    }
    for (int y = fp.g0; y <= fp.g1; y++) {
        for (int s = 0; s < 2; s++) {
            if (memcmp(&gutter_planes[s][y * PSYZ_GUTTER_MAX],
                       &g1[s][y * PSYZ_GUTTER_MAX],
                       PSYZ_GUTTER_MAX * sizeof(u16))) {
                DefChkFail("gutter pixels depend on VRAM outside the source",
                           w, n);
            }
        }
    }
    memcpy(vram, v1, sizeof(v1));
    memcpy(gutter_planes, g1, sizeof(g1));
    return used;
}
#endif

// ---- queueing

static DefLayer* DefNewLayer(void) {
    DefLayer* l = NULL;

    if (def_n == DEF_MAX_LAYERS) {
        // full: draw the oldest and what it depends on
        bool sel[DEF_MAX_LAYERS] = {0};
        sel[0] = true;
        DefFlushSet(sel, NULL, NULL);
    }
    for (int i = 0; i < DEF_MAX_LAYERS; i++) {
        if (!def_pool[i].used) {
            l = &def_pool[i];
            break;
        }
    }
    if (!l) {
        return NULL;
    }
    l->used = true;
    l->closed = false;
    l->g_alive = false;
    l->area_start = draw_area_start;
    l->area_end = draw_area_end;
    l->len = 0;
    l->nsnaps = 0;
    l->nprims = 0;
    l->d = IR_EMPTY;
    l->g0 = 0;
    l->g1 = -1;
    memset(l->src, 0, sizeof(l->src));
    l->nsrcr = 0;
    l->src_over = false;
    def_layers[def_n++] = l;
    return l;
}

static DefLayer* DefOpenLayer(void) {
    DefLayer* l = def_n ? def_layers[def_n - 1] : NULL;

    if (l && !l->closed && l->len < DEF_MAX_WORDS &&
        l->nsnaps < DEF_MAX_SNAPS &&
        l->area_start.x == draw_area_start.x &&
        l->area_start.y == draw_area_start.y &&
        l->area_end.x == draw_area_end.x && l->area_end.y == draw_area_end.y) {
        return l;
    }
    return DefNewLayer();
}

// An opaque rectangle queued now overwrites, when it runs, everything it
// covers: layers inside it need not be drawn at all. A layer that a newer
// layer samples from stays (that one reads its pixels before the rectangle
// runs).
static void DefKillCovered(const DefFp* x) {
    bool drop[DEF_MAX_LAYERS] = {0};
    bool sel[DEF_MAX_LAYERS] = {0};
    bool any = false;

    for (int i = def_n - 1; i >= 0; i--) {
        const DefLayer* l = def_layers[i];
        bool ok = IR_Inside(l->d, x->v) &&
                  (!l->g_alive || (x->cover_g && l->g0 >= x->g0 &&
                                   l->g1 <= x->g1));
        for (int j = i + 1; ok && j < def_n; j++) {
            if (!drop[j] && LayerSrcHit(def_layers[j], l->d)) {
                ok = false;
            }
        }
        drop[i] = ok;
        any |= ok;
    }
    if (any) {
        DefFlushSet(sel, drop, NULL);
    }
}

static bool LayerReserve(DefLayer* l, size_t words) {
    if (l->len + words > l->cap) {
        size_t cap = l->cap ? l->cap : 1024;
        while (cap < l->len + words) {
            cap *= 2;
        }
        u_long* nb = realloc(l->buf, cap * sizeof(u_long));
        if (!nb) {
            return false;
        }
        l->buf = nb;
        l->cap = cap;
    }
    if (l->nsnaps + 1 > l->cap_snaps) {
        int cap = l->cap_snaps ? l->cap_snaps * 2 : 16;
        DefSnap* ns = realloc(l->snaps, (size_t)cap * sizeof(DefSnap));
        if (!ns) {
            return false;
        }
        l->snaps = ns;
        l->cap_snaps = cap;
    }
    return true;
}

static int DefPush(u_long* words, int max_len) {
    DefFp fp;
    DefSnap snap;
    DefLayer* l;

    DefFootprint(words, max_len, &fp);
    if (fp.kind == FP_NOOP) {
        return fp.n;
    }
    if (fp.kind == FP_UNSUPPORTED || fp.n > 0xffff) {
        DefSyncAll();
        return Software_PushPrimNow(words, max_len);
    }
    if (IR_IsEmpty(fp.v) && fp.g1 < fp.g0) {
        return fp.n; // clipped away
    }
    if (fp.cover) {
        DefKillCovered(&fp);
    }
    l = DefOpenLayer();
    if (!l || !LayerReserve(l, (size_t)fp.n + 1)) {
        DefSyncAll();
        return Software_PushPrimNow(words, max_len);
    }
    DefCapture(&snap);
    if (l->nsnaps == 0 || memcmp(&l->snaps[l->nsnaps - 1], &snap, sizeof(snap))) {
        l->snaps[l->nsnaps++] = snap;
    }
    l->buf[l->len++] = (u_long)fp.n | ((u_long)(l->nsnaps - 1) << 16);
    memcpy(&l->buf[l->len], words, (size_t)fp.n * sizeof(u_long));
    l->len += (size_t)fp.n;
    l->nprims++;
    l->d = IR_Union(l->d, fp.v);
    if (fp.g1 >= fp.g0) {
        if (!l->g_alive) {
            l->g_alive = true;
            l->g0 = fp.g0;
            l->g1 = fp.g1;
        } else {
            l->g0 = fp.g0 < l->g0 ? fp.g0 : l->g0;
            l->g1 = fp.g1 > l->g1 ? fp.g1 : l->g1;
        }
    }
    for (int k = 0; k < fp.nsrc; k++) {
        LayerSrcAdd(l, fp.src[k]);
    }
    def_stat_queued++;
    return fp.n;
}

// ---- ops that run at once

static int Software_PushPrim(u_long* words, int max_len) {
    if (max_len <= 0) {
        return 0;
    }
    if (def_on) {
        return DefPush(words, max_len);
    }
    return Software_PushPrimNow(words, max_len);
}

static void Software_ClearImage(PS1_RECT* rect, u_char r, u_char g, u_char b) {
    if (def_n) {
        DefOpDesc o = {.nr = 0, .gw0 = 0, .gw1 = -1};
        int x0 = CLAMP(rect->x, 0, VRAM_W);
        int y0 = CLAMP(rect->y, 0, VRAM_H);
        int x1 = CLAMP(rect->x + rect->w, 0, VRAM_W);
        int y1 = CLAMP(rect->y + rect->h, 0, VRAM_H);

        o.w = (IRect){x0, y0, x1 - 1, y1 - 1};
        o.w_cover = !preserve_masked;
        if (preserve_masked) {
            o.r[o.nr++] = o.w;
        }
        if (gutter_px > 0 && rect->x == gutter_x0 && rect->w == gutter_w) {
            int g0 = CLAMP(rect->y, 0, VRAM_H);
            int g1 = CLAMP(rect->y + rect->h, 0, VRAM_H);

            o.gw0 = g0;
            o.gw1 = g1 - 1;
        }
        DefOp(&o);
    }
    Software_ClearImageNow(rect, r, g, b);
}

static void Software_LoadImage(PS1_RECT* rect, u_long* data) {
    if (def_n && rect->w > 0 && rect->h > 0) {
        DefOpDesc o = {.nr = 0, .gw0 = 0, .gw1 = -1, .w_cover = true};

        o.w = (IRect){rect->x > 0 ? rect->x : 0, rect->y > 0 ? rect->y : 0,
                      rect->x + rect->w - 1 < VRAM_W - 1 ? rect->x + rect->w - 1
                                                         : VRAM_W - 1,
                      rect->y + rect->h - 1 < VRAM_H - 1 ? rect->y + rect->h - 1
                                                         : VRAM_H - 1};
        DefOp(&o);
    }
    Software_LoadImageNow(rect, data);
}

static void Software_StoreImage(PS1_RECT* rect, u_long* data) {
    if (def_n && rect->w > 0 && rect->h > 0) {
        DefOpDesc o = {.nr = 1, .gw0 = 0, .gw1 = -1, .w = IR_EMPTY};

        o.r[0] = (IRect){rect->x > 0 ? rect->x : 0, rect->y > 0 ? rect->y : 0,
                         rect->x + rect->w - 1 < VRAM_W - 1 ? rect->x + rect->w - 1
                                                            : VRAM_W - 1,
                         rect->y + rect->h - 1 < VRAM_H - 1 ? rect->y + rect->h - 1
                                                            : VRAM_H - 1};
        DefOp(&o);
    }
    Software_StoreImageNow(rect, data);
}

static void Software_MoveImage(PS1_RECT* rect, unsigned int x, unsigned int y) {
    if (def_n) {
        DefOpDesc o = {.nr = 1, .gw0 = 0, .gw1 = -1, .w_cover = true};
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

        if (w && h) {
            o.r[0] = (IRect){src_x, src_y, src_x + w - 1, src_y + h - 1};
            o.w = (IRect){dst_x, dst_y, dst_x + w - 1, dst_y + h - 1};
            DefOp(&o);
        }
    }
    Software_MoveImageNow(rect, x, y);
}

// ---- public

void SoftwareRasterizer_SetDefer(int on) {
    if (!on) {
        DefSyncAll();
    }
    def_on = on != 0;
}

int SoftwareRasterizer_GetDefer(void) { return def_on; }

void SoftwareRasterizer_SyncAll(void) { DefSyncAll(); }

// Draws what the rectangle (and, with gutter, the gutter rows of its lines)
// shows. Call before reading VRAM or the gutter planes for it.
void SoftwareRasterizer_SyncRect(int x, int y, int w, int h, int gutter) {
    bool sel[DEF_MAX_LAYERS] = {0};
    IRect r;
    int y0, y1;

    if (!def_n || w <= 0 || h <= 0) {
        return;
    }
    r = (IRect){x > 0 ? x : 0, y > 0 ? y : 0,
                x + w - 1 < VRAM_W - 1 ? x + w - 1 : VRAM_W - 1,
                y + h - 1 < VRAM_H - 1 ? y + h - 1 : VRAM_H - 1};
    y0 = r.y0;
    y1 = r.y1;
    for (int i = 0; i < def_n; i++) {
        const DefLayer* l = def_layers[i];
        sel[i] = IR_Hit(r, l->d) ||
                 (gutter && l->g_alive && GRowsHit(y0, y1, l->g0, l->g1));
    }
    def_cause = 2;
    DefFlushSet(sel, NULL, NULL);
    def_cause = 0;
}

// counters since the last reset: primitives queued, dropped without being
// drawn, drawn later; layers drawn for a present (SyncRect) and for anything
// else
void SoftwareRasterizer_GetDeferStats(unsigned long* queued,
                                      unsigned long* dropped,
                                      unsigned long* run,
                                      unsigned long* sync_layers,
                                      unsigned long* other_layers, int reset) {
    if (queued) {
        *queued = def_stat_queued;
    }
    if (dropped) {
        *dropped = def_stat_dropped;
    }
    if (run) {
        *run = def_stat_run;
    }
    if (sync_layers) {
        *sync_layers = def_stat_sync_layers;
    }
    if (other_layers) {
        *other_layers = def_stat_other_layers;
    }
    if (reset) {
        def_stat_queued = def_stat_dropped = def_stat_run = 0;
        def_stat_sync_layers = def_stat_other_layers = 0;
    }
}
