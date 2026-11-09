// Frame Finch core. C99, no dependencies (stb_image_write.h, public domain, writes the JPEGs and deflates the PNGs).
//
// How it works, in the order things happen:
//
// 1. Changes (ff_frame). Each frame is turned to grey and compared with the one before, in a grid of
//    8x8 pixel cells. A cell whose average brightness changed by more than CELL_DIFF has changed.
//    Changed cells close to each other are joined into boxes (a line of text, a list row, a whole
//    screen). Each big box is also tested for a scroll: if the old picture slid up or down by some
//    number of pixels matches the new one much better than the old picture where it was, the box scrolled.
//
// 2. Blinkers and the clock (find_ticker, in ff_finish). Small boxes that change on a steady beat in
//    the same place are not activity: a cursor blinking twice a second, or a clock showing seconds.
//    They are left out. A clock is useful too: the frames where its second ticks over happened just
//    after a whole second of phone time, so they pin the video to the phone's clock to the millisecond.
//
// 3. Bursts (ff_finish). Frames with real change are grouped into bursts, split wherever the screen
//    was still for BURST_GAP. A burst is one thing happening: a screen opening, a list scrolling.
//    For each burst we keep the frame before it, its first frame, its biggest change and its last.
//
// 4. Pictures (ff_picture). The front end decodes the video again at a larger size and hands over
//    those key frames. App screens use few colours, so each is reduced to 64 colours and written as a
//    PNG, which keeps text sharp and the file small. A frame with too many shades for that (a photo,
//    a video, a gradient) is written as a JPEG instead.
//
// 5. The report (ff_write_html). One HTML file: a plain-text timeline at the top that an agent can
//    read as it is, a timeline drawing, the key frames with their change boxes drawn over them, and
//    every frame's boxes as JSON at the end.

#include "ff.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

#define CELL 8              // analysis pixels per grid cell
#define CELL_DIFF 3.0       // mean |brightness change| that marks a cell changed
#define BURST_GAP 300.0     // ms of stillness that ends a burst
#define MAX_KEYS 80         // key frames in the report, at most
#define JPEG_Q 85           // key frames with many shades (photos, video, gradients)
#define PAL 64              // otherwise a PNG with this many colours: sharp text, small file

// A box of changed cells in one frame, in analysis pixels (x1, y1 exclusive).
typedef struct
{
    int x0, y0, x1, y1;
    int cells;              // changed cells inside it
    int dy;                 // scroll in video pixels (down is +), 0 if it did not scroll
    int ticker;             // 1 the seconds clock, 2 another steady blinker, 0 real change
} rect;

typedef struct
{
    double t;               // video ms
    int nr;                 // its change boxes
    rect *r;
    int cells;              // changed cells, all boxes
    unsigned char *img;     // the key frame's picture (PNG or JPEG bytes)
    int nimg, png;
    int burst;              // burst index or -1
    int key;                // embedded in the report
    int shown;              // already written once (that copy carries the id)
} frame;

typedef struct
{
    double t;
    int wall;               // t is still epoch ms (converted in ff_finish)
    char kind;              // 'd' down, 'm' move, 'u' up
    int x, y;
} touch;

typedef struct
{
    double t;
    int wall;
    char *text;
} mark;

// A run of frames with real change.
typedef struct
{
    int first, last;        // its first and last changed frames
    int peak;               // the frame with the most change
    int before;             // the still frame just before it
    double t0, t1;          // first and last change, video ms
    int x0, y0, x1, y1;     // everything that changed, analysis px
    int dy, maxcells, scrolls;
} burst;

struct ff
{
    int w, h, sw, sh;       // analysis size, video size
    int cols, rows;         // the cell grid
    unsigned char *prev, *cur;  // brightness of the last frame and this one
    float *celld;           // per cell: how much it changed
    unsigned char *mask, *seen;
    int *queue;
    frame *fr;
    int nfr, capfr;
    touch *tc;
    int ntc, captc;
    mark *mk;
    int nmk, capmk;
    burst *bu;
    int nbu;
    double dur;
    long long creation_s;   // what the file says (see ff_clock_hint)
    int creation_is_end, utc_off, have_hint;
    int have_wall;          // 0 unknown, 1 worked out, 2 given (ff_set_wall)
    double wall0;           // epoch ms at video 0
    char how[256];          // how wall0 was found, for the report
    int tick_x0, tick_y0, tick_x1, tick_y1, ticks;  // the seconds clock, if found
    double tick_spread, tick_phase;
};

static void *grow(void *p, int *cap, int need, size_t size)
{
    if (need <= *cap)
        return p;
    int c = *cap ? *cap * 2 : 64;
    while (c < need)
        c *= 2;
    *cap = c;
    return realloc(p, (size_t)c * size);
}

ff *ff_new(int w, int h, int src_w, int src_h)
{
    ff *f = calloc(1, sizeof *f);
    f->w = w;
    f->h = h;
    f->sw = src_w;
    f->sh = src_h;
    f->cols = (w + CELL - 1) / CELL;
    f->rows = (h + CELL - 1) / CELL;
    f->prev = malloc((size_t)w * h);
    f->cur = malloc((size_t)w * h);
    f->celld = malloc(sizeof(float) * f->cols * f->rows);
    f->mask = malloc((size_t)f->cols * f->rows);
    f->seen = malloc((size_t)f->cols * f->rows);
    f->queue = malloc(sizeof(int) * f->cols * f->rows);
    return f;
}

void ff_free(ff *f)
{
    if (!f)
        return;
    for (int i = 0; i < f->nfr; i++)
    {
        free(f->fr[i].r);
        free(f->fr[i].img);
    }
    for (int i = 0; i < f->nmk; i++)
        free(f->mk[i].text);
    free(f->fr);
    free(f->tc);
    free(f->mk);
    free(f->bu);
    free(f->prev);
    free(f->cur);
    free(f->celld);
    free(f->mask);
    free(f->seen);
    free(f->queue);
    free(f);
}

// A growing byte buffer, for pictures made in memory.
typedef struct
{
    unsigned char *p;
    int n, cap;
} buf;

static void buf_put(void *ctx, void *data, int size)
{
    buf *b = ctx;
    if (b->n + size > b->cap)
    {
        b->cap = (b->n + size) * 2;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, data, size);
    b->n += size;
}

// ---- 1. Changes ----

// Mean |difference| over the box between this frame and the last one moved down by d pixels
// (every 2nd pixel, which is plenty). d = 0 compares the box with itself where it was.
static double shift_score(const ff *f, const rect *r, int d)
{
    long sum = 0, n = 0;
    for (int y = r->y0; y < r->y1; y += 2)
    {
        int py = y - d;
        if (py < r->y0 || py >= r->y1)
            continue;
        const unsigned char *a = f->cur + (size_t)y * f->w, *b = f->prev + (size_t)py * f->w;
        for (int x = r->x0; x < r->x1; x += 2)
        {
            int v = a[x] - b[x];
            sum += v < 0 ? -v : v;
            n++;
        }
    }
    return n ? (double)sum / n : 1e9;
}

// A box whose content moved up or down as a whole: tries every shift and keeps the best one
// if it matches far better than no shift at all. Returns the shift in analysis px, 0 if it is not a scroll.
static int find_scroll(const ff *f, const rect *r)
{
    int hgt = r->y1 - r->y0;
    if (hgt < 48 || r->x1 - r->x0 < f->w / 3)
        return 0;
    double still = shift_score(f, r, 0), best = still;
    int bd = 0, maxd = (int)(hgt * 0.7);
    for (int d = -maxd; d <= maxd; d++)
    {
        if (!d)
            continue;
        double s = shift_score(f, r, d);
        if (s < best)
        {
            best = s;
            bd = d;
        }
    }
    return (bd && best < still * 0.35 && best < 6.0) ? bd : 0;
}

void ff_frame(ff *f, double t_ms, const unsigned char *rgb)
{
    f->fr = grow(f->fr, &f->capfr, f->nfr + 1, sizeof(frame));
    frame *fr = &f->fr[f->nfr];
    memset(fr, 0, sizeof *fr);
    fr->t = t_ms;
    fr->burst = -1;
    int w = f->w, h = f->h;

    // brightness, weighted the way eyes see it
    for (int i = 0; i < w * h; i++)
        f->cur[i] = (unsigned char)((rgb[i * 3] * 77 + rgb[i * 3 + 1] * 150 + rgb[i * 3 + 2] * 29) >> 8);

    if (f->nfr)
    {
        // which cells changed since the last frame
        int nc = f->cols * f->rows;
        for (int c = 0; c < nc; c++)
        {
            int cx = c % f->cols, cy = c / f->cols, x0 = cx * CELL, y0 = cy * CELL;
            int x1 = x0 + CELL < w ? x0 + CELL : w, y1 = y0 + CELL < h ? y0 + CELL : h;
            long s = 0;
            for (int y = y0; y < y1; y++)
            {
                for (int x = x0; x < x1; x++)
                {
                    int v = f->cur[y * w + x] - f->prev[y * w + x];
                    s += v < 0 ? -v : v;
                }
            }
            f->celld[c] = (float)s / ((x1 - x0) * (y1 - y0));
            f->mask[c] = f->celld[c] > CELL_DIFF;
        }

        // flood fill: changed cells within two cells of each other form one box (a line of text, a list row)
        memset(f->seen, 0, (size_t)nc);
        for (int c = 0; c < nc; c++)
        {
            if (!f->mask[c] || f->seen[c])
                continue;
            int qh = 0, qt = 0;
            f->queue[qt++] = c;
            f->seen[c] = 1;
            rect r = { w, h, 0, 0, 0, 0, 0 };
            while (qh < qt)
            {
                int q = f->queue[qh++], qx = q % f->cols, qy = q / f->cols;
                r.cells++;
                if (qx * CELL < r.x0)
                    r.x0 = qx * CELL;
                if (qy * CELL < r.y0)
                    r.y0 = qy * CELL;
                if ((qx + 1) * CELL > r.x1)
                    r.x1 = (qx + 1) * CELL > w ? w : (qx + 1) * CELL;
                if ((qy + 1) * CELL > r.y1)
                    r.y1 = (qy + 1) * CELL > h ? h : (qy + 1) * CELL;
                for (int dy = -2; dy <= 2; dy++)
                {
                    for (int dx = -2; dx <= 2; dx++)
                    {
                        int nx = qx + dx, ny = qy + dy;
                        if (nx < 0 || ny < 0 || nx >= f->cols || ny >= f->rows)
                            continue;
                        int n = ny * f->cols + nx;
                        if (f->mask[n] && !f->seen[n])
                        {
                            f->seen[n] = 1;
                            f->queue[qt++] = n;
                        }
                    }
                }
            }
            int d = find_scroll(f, &r);
            r.dy = (int)lround((double)d * f->sh / h);
            fr->r = realloc(fr->r, sizeof(rect) * (fr->nr + 1));
            fr->r[fr->nr++] = r;
            fr->cells += r.cells;
        }
    }

    // this frame is the next one's "before"
    unsigned char *t = f->prev;
    f->prev = f->cur;
    f->cur = t;
    f->nfr++;
}

void ff_touch(ff *f, double t, int wall, char kind, int x, int y)
{
    f->tc = grow(f->tc, &f->captc, f->ntc + 1, sizeof(touch));
    f->tc[f->ntc++] = (touch){ t, wall, kind, x, y };
}

void ff_mark(ff *f, double t, int wall, const char *text)
{
    f->mk = grow(f->mk, &f->capmk, f->nmk + 1, sizeof(mark));
    f->mk[f->nmk++] = (mark){ t, wall, strdup(text) };
}

void ff_clock_hint(ff *f, long long creation_s, int creation_is_end, int utc_offset_s)
{
    f->creation_s = creation_s;
    f->creation_is_end = creation_is_end;
    f->utc_off = utc_offset_s;
    f->have_hint = 1;
}

void ff_set_wall(ff *f, double wall0_ms, const char *how)
{
    f->have_wall = 2;
    f->wall0 = wall0_ms;
    snprintf(f->how, sizeof f->how, "%s", how);
}

static int area(const rect *r)
{
    return (r->x1 - r->x0) * (r->y1 - r->y0);
}

// Whether at least half of box a lies inside the given box.
static int overlaps(const rect *a, int x0, int y0, int x1, int y1)
{
    int ix = (a->x1 < x1 ? a->x1 : x1) - (a->x0 > x0 ? a->x0 : x0);
    int iy = (a->y1 < y1 ? a->y1 : y1) - (a->y0 > y0 ? a->y0 : y0);
    return ix > 0 && iy > 0 && ix * iy * 2 >= area(a);
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

// ---- 2. Blinkers and the clock ----

// Every frame where a small box changes in the same place as c: frame and box indexes into fi, ri.
static int hits_of(ff *f, const rect *c, int small, int *fi, int *ri)
{
    int n = 0;
    for (int j = 0; j < f->nfr; j++)
    {
        for (int m = 0; m < f->fr[j].nr; m++)
        {
            rect *o = &f->fr[j].r[m];
            if (o->ticker || area(o) > small || !overlaps(o, c->x0, c->y0, c->x1, c->y1))
                continue;
            fi[n] = j;
            ri[n] = m;
            n++;
            break;
        }
    }
    return n;
}

// Finds small boxes that change on a steady beat and marks them as tickers, so they don't count as
// activity. The one that beats once a second is taken as a seconds clock, and the moments it ticks
// over set the phone time of video 0 (wall0).
static void find_ticker(ff *f)
{
    int small = f->w * f->h / 60, cap = f->nfr + 1;
    int *fi = malloc(sizeof(int) * cap), *ri = malloc(sizeof(int) * cap);
    double *iv = malloc(sizeof(double) * cap), *ph = malloc(sizeof(double) * cap);
    int best_clock = 0;
    for (int i = 0; i < f->nfr; i++)
    {
        for (int k = 0; k < f->fr[i].nr; k++)
        {
            rect c = f->fr[i].r[k];
            if (c.ticker || area(&c) > small)
                continue;
            int n = hits_of(f, &c, small, fi, ri);
            if (n < 4)
                continue;

            // the beat: the commonest interval (the median of the shortest two thirds, so missed beats
            // don't stretch it); most intervals must be a whole number of beats
            for (int j = 1; j < n; j++)
                iv[j - 1] = f->fr[fi[j]].t - f->fr[fi[j - 1]].t;
            qsort(iv, n - 1, sizeof(double), cmp_double);
            double beat = iv[(n - 1) * 2 / 3 / 2];
            int good = 0;
            for (int j = 0; j < n - 1; j++)
            {
                double r = iv[j] / beat;
                if (fabs(r - floor(r + 0.5)) < 0.12 && r > 0.5)
                    good++;
            }
            if (beat < 250 || beat > 1150 || good * 10 < (n - 1) * 7)
                continue;

            int clock = beat > 880 && beat < 1120;
            if (clock && n <= best_clock)
                clock = 0;
            for (int j = 0; j < n; j++)
                f->fr[fi[j]].r[ri[j]].ticker = 2;
            if (!clock)
                continue;
            best_clock = n;
            f->tick_x0 = c.x0;
            f->tick_y0 = c.y0;
            f->tick_x1 = c.x1;
            f->tick_y1 = c.y1;

            // The phase: where in each second the ticks land. Take the tick times modulo a second, find
            // the densest 60 ms of them (strays fall outside), and use the earliest quarter of that, since
            // the frame showing the new second is captured a little after the second starts.
            int np = 0;
            for (int j = 0; j < n; j++)
            {
                if (fi[j] > 0)
                    ph[np++] = fmod(f->fr[fi[j]].t, 1000.0);
            }
            int bi = 0, bc = 0;
            for (int a = 0; a < np; a++)
            {
                int cnt = 0;
                for (int b = 0; b < np; b++)
                {
                    double d = fmod(ph[b] - ph[a] + 2000.0, 1000.0);
                    if (d < 60)
                        cnt++;
                }
                if (cnt > bc)
                {
                    bc = cnt;
                    bi = a;
                }
            }
            double *win = malloc(sizeof(double) * (bc ? bc : 1));
            int nw = 0;
            for (int b = 0; b < np; b++)
            {
                double d = fmod(ph[b] - ph[bi] + 2000.0, 1000.0);
                if (d < 60)
                    win[nw++] = d;
            }
            qsort(win, nw, sizeof(double), cmp_double);
            f->ticks = bc;
            f->tick_spread = nw ? win[nw - 1] - win[0] : 0;
            f->tick_phase = fmod(ph[bi] + (nw ? win[nw / 4] : 0), 1000.0);
            free(win);
        }
    }

    // the clock's own boxes are type 1, other blinkers stay type 2
    if (best_clock)
    {
        for (int i = 0; i < f->nfr; i++)
        {
            for (int k = 0; k < f->fr[i].nr; k++)
            {
                rect *o = &f->fr[i].r[k];
                if (o->ticker && overlaps(o, f->tick_x0, f->tick_y0, f->tick_x1, f->tick_y1))
                    o->ticker = 1;
            }
        }
    }
    free(fi);
    free(ri);
    free(iv);
    free(ph);
    if (!best_clock || f->have_wall == 2)
        return;

    // The clock gives the milliseconds of wall0; the file's creation time gives the second.
    double off = fmod(1000.0 - f->tick_phase, 1000.0);
    if (!f->have_hint)
    {
        f->wall0 = off;
        return;
    }
    double anchor = f->creation_is_end ? (double)f->creation_s * 1000 - f->dur : (double)f->creation_s * 1000;
    double base = floor(anchor / 1000.0) * 1000.0 + off, w0 = base;
    for (int k = -2; k <= 2; k++)
    {
        double c = base + k * 1000.0;
        // Samsung stamps the file in the second the recording ended; others in the second it started
        int fits = f->creation_is_end ? floor((c + f->dur) / 1000.0) == (double)f->creation_s
                                      : floor(c / 1000.0) == (double)f->creation_s;
        if (fits)
        {
            w0 = c;
            break;
        }
    }
    f->wall0 = w0;
    f->have_wall = 1;
    snprintf(f->how, sizeof f->how,
             "the milliseconds from %d ticks of the on-screen seconds clock (they spread over %.0f ms; taken from the earliest quarter), the second from the file's creation time (%s of recording)",
             f->ticks, f->tick_spread, f->creation_is_end ? "end" : "start");
}

// Changed cells in a frame that are real activity (not a clock or cursor).
static int active_cells(const frame *fr)
{
    int c = 0;
    for (int k = 0; k < fr->nr; k++)
    {
        if (!fr->r[k].ticker)
            c += fr->r[k].cells;
    }
    return c;
}

// ---- 3. Bursts ----

void ff_finish(ff *f, double duration_ms)
{
    f->dur = duration_ms;
    if (f->nfr && duration_ms < f->fr[f->nfr - 1].t)
        f->dur = f->fr[f->nfr - 1].t;
    find_ticker(f);
    if (!f->have_wall && f->have_hint)
    {
        f->wall0 = f->creation_is_end ? (double)f->creation_s * 1000 + 500 - f->dur : (double)f->creation_s * 1000 + 500;
        f->have_wall = 1;
        snprintf(f->how, sizeof f->how, "from the file's creation time only (±0.5 s); no on-screen seconds clock was found");
    }

    // touches and log lines given in phone time move onto the video's timeline
    for (int i = 0; i < f->ntc; i++)
    {
        if (f->tc[i].wall)
        {
            f->tc[i].t -= f->wall0;
            f->tc[i].wall = 0;
        }
    }
    for (int i = 0; i < f->nmk; i++)
    {
        if (f->mk[i].wall)
        {
            f->mk[i].t -= f->wall0;
            f->mk[i].wall = 0;
        }
    }

    // bursts: runs of frames with real change, split by BURST_GAP of stillness
    burst *cur = NULL;
    for (int i = 1; i < f->nfr; i++)
    {
        frame *fr = &f->fr[i];
        if (active_cells(fr) < 2)
            continue;
        if (!cur || fr->t - cur->t1 > BURST_GAP)
        {
            f->bu = realloc(f->bu, sizeof(burst) * (f->nbu + 1));
            cur = &f->bu[f->nbu++];
            *cur = (burst){ i, i, i, i - 1, fr->t, fr->t, f->w, f->h, 0, 0, 0, 0, 0 };
        }
        cur->last = i;
        cur->t1 = fr->t;
        int ac = active_cells(fr);
        if (ac > cur->maxcells)
        {
            cur->maxcells = ac;
            cur->peak = i;
        }
        for (int k = 0; k < fr->nr; k++)
        {
            rect *r = &fr->r[k];
            if (r->ticker)
                continue;
            if (r->x0 < cur->x0)
                cur->x0 = r->x0;
            if (r->y0 < cur->y0)
                cur->y0 = r->y0;
            if (r->x1 > cur->x1)
                cur->x1 = r->x1;
            if (r->y1 > cur->y1)
                cur->y1 = r->y1;
            if (r->dy)
            {
                cur->dy += r->dy;
                cur->scrolls++;
            }
        }
    }
    for (int b = 0; b < f->nbu; b++)
    {
        for (int i = f->bu[b].first; i <= f->bu[b].last; i++)
            f->fr[i].burst = b;
    }

    // key frames: the first and last of the video, then for each burst the still frame before it,
    // its first change and where it settled, then (while there is room) each burst's biggest change
    int keys = 0;
    #define KEY(i) \
        do \
        { \
            int _i = (i); \
            if (_i >= 0 && _i < f->nfr && !f->fr[_i].key && keys < MAX_KEYS) \
            { \
                f->fr[_i].key = 1; \
                keys++; \
            } \
        } while (0)
    KEY(0);
    KEY(f->nfr - 1);
    for (int b = 0; b < f->nbu; b++)
    {
        KEY(f->bu[b].before);
        KEY(f->bu[b].first);
        KEY(f->bu[b].last);
    }
    for (int b = 0; b < f->nbu; b++)
        KEY(f->bu[b].peak);
    #undef KEY
}

// ---- 4. Pictures ----

int ff_is_key(const ff *f, int i)
{
    return i >= 0 && i < f->nfr && f->fr[i].key;
}

// For the colour reduction: every colour used, at 5 bits a channel (r | g << 5 | b << 10), and how many
// pixels use it. A box is a run of that list that will become one palette colour.
typedef struct
{
    int key;
    long n;
} colour;

typedef struct
{
    int lo, hi;
    long n;
} box;

static int sort_ch;     // the channel cmp_colour sorts by (qsort has no context argument)

static int cmp_colour(const void *a, const void *b)
{
    int s = sort_ch * 5, x = ((const colour *)a)->key >> s & 31, y = ((const colour *)b)->key >> s & 31;
    return x - y;
}

static unsigned crc32(unsigned crc, const unsigned char *p, size_t n)
{
    crc = ~crc;
    while (n--)
    {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = crc >> 1 ^ (0xEDB88320u & -(crc & 1));
    }
    return ~crc;
}

// One PNG chunk: length, type, data, CRC.
static void chunk(buf *b, const char *type, const unsigned char *data, unsigned n)
{
    unsigned char len[4] = { n >> 24, n >> 16, n >> 8, n }, crc[4];
    buf_put(b, len, 4);
    buf_put(b, (void *)type, 4);
    if (n)
        buf_put(b, (void *)data, (int)n);
    unsigned c = crc32(crc32(0, (const unsigned char *)type, 4), data, n);
    crc[0] = c >> 24;
    crc[1] = c >> 16;
    crc[2] = c >> 8;
    crc[3] = c;
    buf_put(b, crc, 4);
}

// Reduces the frame to PAL colours by median cut: start with one box holding every colour, keep
// splitting the box that spans the widest range (weighted by how many pixels it holds) at its median,
// and give each box the average colour of its pixels. Then checks how far the result is from the
// frame. Returns 0, with a PNG written into out, when the colours hold up; 1 when the frame has too
// many shades for a palette (it is then made a JPEG).
static int palette_png(const unsigned char *rgb, int w, int h, buf *out)
{
    int n = w * h;
    long *count = calloc(32768, sizeof(long));
    double (*sum)[3] = calloc(32768, sizeof *sum);
    for (int i = 0; i < n; i++)
    {
        const unsigned char *p = rgb + i * 3;
        int k = (p[0] >> 3) | (p[1] >> 3) << 5 | (p[2] >> 3) << 10;
        count[k]++;
        sum[k][0] += p[0];
        sum[k][1] += p[1];
        sum[k][2] += p[2];
    }
    int nc = 0;
    colour *cl = malloc(sizeof(colour) * 32768);
    for (int k = 0; k < 32768; k++)
    {
        if (count[k])
            cl[nc++] = (colour){ k, count[k] };
    }

    box bx[PAL];
    int nb = 1;
    bx[0] = (box){ 0, nc, n };
    while (nb < PAL)
    {
        // the box to split, and along which channel
        int bi = -1, bch = 0;
        double bs = 0;
        for (int b = 0; b < nb; b++)
        {
            if (bx[b].hi - bx[b].lo < 2)
                continue;
            for (int ch = 0; ch < 3; ch++)
            {
                int lo = 31, hi = 0;
                for (int i = bx[b].lo; i < bx[b].hi; i++)
                {
                    int v = cl[i].key >> ch * 5 & 31;
                    if (v < lo)
                        lo = v;
                    if (v > hi)
                        hi = v;
                }
                double s = (double)(hi - lo) * bx[b].n;
                if (s > bs)
                {
                    bs = s;
                    bi = b;
                    bch = ch;
                }
            }
        }
        if (bi < 0)
            break;
        // sort it along that channel and cut where half its pixels are on each side
        sort_ch = bch;
        qsort(cl + bx[bi].lo, bx[bi].hi - bx[bi].lo, sizeof(colour), cmp_colour);
        long half = bx[bi].n / 2, acc = 0;
        int m = bx[bi].lo;
        while (m < bx[bi].hi - 1 && acc + cl[m].n <= half)
            acc += cl[m++].n;
        if (m == bx[bi].lo)
            acc += cl[m++].n;
        bx[nb] = (box){ m, bx[bi].hi, bx[bi].n - acc };
        bx[bi].hi = m;
        bx[bi].n = acc;
        nb++;
    }

    // each box's colour is the average of its pixels; lut maps a 5-bit colour to its palette entry
    unsigned char pal[PAL * 3];
    unsigned char *lut = malloc(32768);
    for (int b = 0; b < nb; b++)
    {
        double s[3] = { 0, 0, 0 };
        long c = 0;
        for (int i = bx[b].lo; i < bx[b].hi; i++)
        {
            int k = cl[i].key;
            s[0] += sum[k][0];
            s[1] += sum[k][1];
            s[2] += sum[k][2];
            c += count[k];
            lut[k] = (unsigned char)b;
        }
        for (int ch = 0; ch < 3; ch++)
            pal[b * 3 + ch] = (unsigned char)(c ? s[ch] / c + 0.5 : 0);
    }

    // the PNG rows (a filter byte, then one palette index a pixel), and how far the palette is from
    // the frame: the mean error, and the share of pixels that are clearly off
    unsigned char *raw = malloc((size_t)(w + 1) * h);
    double err = 0;
    long off = 0;
    for (int y = 0; y < h; y++)
    {
        raw[y * (w + 1)] = 0;
        for (int x = 0; x < w; x++)
        {
            const unsigned char *p = rgb + (y * w + x) * 3;
            int k = (p[0] >> 3) | (p[1] >> 3) << 5 | (p[2] >> 3) << 10, q = lut[k], worst = 0;
            for (int ch = 0; ch < 3; ch++)
            {
                int d = abs(p[ch] - pal[q * 3 + ch]);
                err += d;
                if (d > worst)
                    worst = d;
            }
            if (worst > 28)
                off++;
            raw[y * (w + 1) + 1 + x] = (unsigned char)q;
        }
    }
    err /= 3.0 * n;
    int many = err > 2.5 || off * 200 > n;

    if (!many)
    {
        static const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
        unsigned char ihdr[13] = { w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, 3, 0, 0, 0 };
        buf_put(out, (void *)sig, 8);
        chunk(out, "IHDR", ihdr, 13);
        chunk(out, "PLTE", pal, (unsigned)nb * 3);
        int zn = 0;
        unsigned char *z = stbi_zlib_compress(raw, (w + 1) * h, &zn, 8);
        chunk(out, "IDAT", z, (unsigned)zn);
        STBIW_FREE(z);
        chunk(out, "IEND", NULL, 0);
    }
    free(count);
    free(sum);
    free(cl);
    free(lut);
    free(raw);
    return many;
}

void ff_picture(ff *f, int i, int w, int h, const unsigned char *rgb)
{
    if (!ff_is_key(f, i) || f->fr[i].img)
        return;
    buf b = { 0 };
    if (palette_png(rgb, w, h, &b))
    {
        b.n = 0;
        stbi_write_jpg_to_func(buf_put, &b, w, h, 3, rgb, JPEG_Q);
        f->fr[i].png = 0;
    }
    else
    {
        f->fr[i].png = 1;
    }
    f->fr[i].img = b.p;
    f->fr[i].nimg = b.n;
}

// ---- 5. The report ----

// Video ms as phone time, HH:MM:SS.mmm, or "-" when the clock is unknown.
static void wall_str(const ff *f, double t, char *out, size_t n)
{
    if (f->have_wall == 0)
    {
        snprintf(out, n, "-");
        return;
    }
    double w = f->wall0 + t + (double)f->utc_off * 1000;
    long long ms = (long long)floor(w + 0.5);
    long long s = ms / 1000;
    int milli = (int)(ms % 1000);
    int day = (int)(((s % 86400) + 86400) % 86400);
    snprintf(out, n, "%02d:%02d:%02d.%03d", day / 3600, day / 60 % 60, day % 60, milli);
}

static void put_esc(FILE *o, const char *s)
{
    for (; *s; s++)
    {
        if (*s == '<')
            fputs("&lt;", o);
        else if (*s == '>')
            fputs("&gt;", o);
        else if (*s == '&')
            fputs("&amp;", o);
        else
            fputc(*s, o);
    }
}

static void put_json_str(FILE *o, const char *s)
{
    fputc('"', o);
    for (; *s; s++)
    {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
        {
            fputc('\\', o);
            fputc(c, o);
        }
        else if (c == '<')
            fputs("\\u003c", o);
        else if (c < 0x20)
            fprintf(o, "\\u%04x", c);
        else
            fputc(c, o);
    }
    fputc('"', o);
}

static void put_b64(FILE *o, const unsigned char *p, int n)
{
    static const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < n; i += 3)
    {
        unsigned v = p[i] << 16 | (i + 1 < n ? p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        fputc(t[v >> 18 & 63], o);
        fputc(t[v >> 12 & 63], o);
        fputc(i + 1 < n ? t[v >> 6 & 63] : '=', o);
        fputc(i + 2 < n ? t[v & 63] : '=', o);
    }
}

// analysis pixels to video pixels
static int src_x(const ff *f, int x)
{
    return (int)lround((double)x * f->sw / f->w);
}

static int src_y(const ff *f, int y)
{
    return (int)lround((double)y * f->sh / f->h);
}

static const char *where(const ff *f, int y0, int y1)
{
    double a = (double)y0 / f->h, b = (double)y1 / f->h;
    if (b - a > 0.6)
        return "most of the screen";
    double m = (a + b) / 2;
    return m < 0.2 ? "top" : m < 0.4 ? "upper middle" : m < 0.6 ? "middle" : m < 0.8 ? "lower middle" : "bottom";
}

// One line of the timeline: a burst, a touch or a log line, sorted by time.
typedef struct
{
    double t;
    int kind;               // 0 burst, 1 touch, 2 log line
    int i;
} ev;

static int cmp_ev(const void *a, const void *b)
{
    const ev *x = a, *y = b;
    return x->t < y->t ? -1 : x->t > y->t ? 1 : x->kind - y->kind;
}

// The plain-text report at the top of the page.
static void report(ff *f, FILE *o, const char *name)
{
    char w[32];
    fprintf(o, "FRAME FINCH REPORT\n");
    fprintf(o, "video: %s, %dx%d, %.2f s, %d frames (a frame is only recorded when the screen changes)\n", name, f->sw, f->sh, f->dur / 1000, f->nfr);
    if (f->have_wall)
    {
        wall_str(f, 0, w, sizeof w);
        fprintf(o, "clock: video 0 ms = %s phone time; %s\n", w, f->how);
    }
    else
    {
        fprintf(o, "clock: unknown; times are video ms only\n");
    }
    if (f->ticks)
    {
        fprintf(o, "ticker: a seconds clock at [%d,%d %dx%d] (source px); it and anything else that blinks on a steady beat (a cursor) is left out of the changes below\n",
                src_x(f, f->tick_x0), src_y(f, f->tick_y0), src_x(f, f->tick_x1 - f->tick_x0), src_y(f, f->tick_y1 - f->tick_y0));
    }
    fprintf(o, "precision: a time is when the frame was captured; changes land on the next screen refresh, so read them as +0..17 ms\n");
    fprintf(o, "\nTIMELINE (video ms | phone time)\n");

    int n = f->nbu + f->ntc + f->nmk;
    ev *e = malloc(sizeof(ev) * (n ? n : 1));
    int k = 0;
    for (int i = 0; i < f->nbu; i++)
        e[k++] = (ev){ f->bu[i].t0, 0, i };
    for (int i = 0; i < f->ntc; i++)
    {
        if (f->tc[i].kind != 'm')
            e[k++] = (ev){ f->tc[i].t, 1, i };
    }
    for (int i = 0; i < f->nmk; i++)
    {
        if (f->mk[i].t > -2000 && f->mk[i].t < f->dur + 2000)
            e[k++] = (ev){ f->mk[i].t, 2, i };
    }
    qsort(e, k, sizeof(ev), cmp_ev);

    double prev_end = -1;
    for (int j = 0; j < k; j++)
    {
        wall_str(f, e[j].t, w, sizeof w);
        if (e[j].kind == 0)
        {
            burst *b = &f->bu[e[j].i];
            int cells = f->cols * f->rows;
            fprintf(o, "%9.1f  %s  CHANGE #%d", b->t0, w, e[j].i + 1);
            if (prev_end >= 0)
                fprintf(o, " (still for %.0f ms before)", b->t0 - prev_end);
            fprintf(o, ": %s [%d,%d %dx%d], lasts %.0f ms over %d frames, peak %.1f%% of screen at %.1f",
                    where(f, b->y0, b->y1), src_x(f, b->x0), src_y(f, b->y0), src_x(f, b->x1 - b->x0), src_y(f, b->y1 - b->y0),
                    b->t1 - b->t0, b->last - b->first + 1, 100.0 * b->maxcells / cells, f->fr[b->peak].t);
            if (b->scrolls)
                fprintf(o, ", scrolled %+d px in %d frames", b->dy, b->scrolls);
            // the steps: the first frame, then every frame that changed at least 3% of the screen
            int steps = 0;
            for (int i = b->first; i <= b->last && steps < 10; i++)
            {
                double pc = 100.0 * active_cells(&f->fr[i]) / cells;
                if (pc < 3 && i != b->first)
                    continue;
                fprintf(o, "%s f%d %+.0f ms %.0f%%", steps ? "," : "; steps:", i, f->fr[i].t - b->t0, pc);
                steps++;
            }
            fprintf(o, "; frames f%d (before) f%d f%d (settled)\n", b->before, b->first, b->last);
            prev_end = b->t1;
        }
        else if (e[j].kind == 1)
        {
            // a touch, and the first frame that really changed after it. That frame can sit inside a
            // burst that began before the touch (a fading toast, a playing video), so look at frames,
            // not at burst starts.
            touch *t = &f->tc[e[j].i];
            fprintf(o, "%9.1f  %s  TOUCH %s at (%d,%d)", t->t, w, t->kind == 'd' ? "down" : "up", t->x, t->y);
            for (int i = 1; i < f->nfr; i++)
            {
                const frame *fr = &f->fr[i];
                if (fr->t < t->t - 5 || active_cells(fr) < 2 || fr->burst < 0)
                    continue;
                fprintf(o, " -> first change %+.0f ms (#%d f%d), settled %+.0f ms", fr->t - t->t, fr->burst + 1, i,
                        f->bu[fr->burst].t1 - t->t);
                break;
            }
            // touched while the screen was already changing: when that change stopped (a pause, a stop)
            for (int b = 0; b < f->nbu; b++)
            {
                if (f->bu[b].t0 < t->t - 5 && f->bu[b].t1 >= t->t)
                {
                    fprintf(o, "; during #%d, which stopped %+.0f ms", b + 1, f->bu[b].t1 - t->t);
                    break;
                }
            }
            fputc('\n', o);
        }
        else
        {
            fprintf(o, "%9.1f  %s  LOG ", f->mk[e[j].i].t, w);
            fputs(f->mk[e[j].i].text, o);
            fputc('\n', o);
        }
    }
    free(e);
    fprintf(o, "\nFRAMES: every frame with its changes is in the JSON (script id=\"data\"); key frames are embedded as <img id=\"fN\"> (64-colour PNG, or JPEG when a frame has too many shades).\n");
    fprintf(o, "To look at one: python3 -c \"import re,sys,base64;h=open(sys.argv[1]).read();n=sys.argv[2];m=re.search('id=.f'+n+'.[^>]*?src=.data:image/(png|jpeg);base64,([^\\\"]+)',h);open('f'+n+'.'+m.group(1),'wb').write(base64.b64decode(m.group(2)))\" REPORT.html 12\n");
}

int ff_write_html(ff *f, FILE *o, const char *name)
{
    fprintf(o, "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n<title>Frame Finch · ");
    put_esc(o, name);
    fprintf(o, "</title>\n</head><body>\n<pre id=\"report\">\n");

    // the text report comes first so an agent reading the file sees it at once;
    // it is written to memory first so it can be escaped for HTML
    char *txt = NULL;
    size_t tn = 0;
    FILE *m = open_memstream(&txt, &tn);
    report(f, m, name);
    fclose(m);
    put_esc(o, txt);
    free(txt);
    fprintf(o, "</pre>\n");

    fputs("<style>\n:root{--bg:#fbfaf7;--fg:#22211e;--dim:#77736a;--line:#dcd8cf;--chg:#d9822b;--scr:#2b7bd9;--tick:#b9b4a8;--touch:#c8343d;--mark:#3f8f5a}\n"
          "@media (prefers-color-scheme:dark){:root{--bg:#161614;--fg:#ecebe6;--dim:#9a968c;--line:#3a3833;--chg:#f0a04b;--scr:#5aa2f0;--tick:#5d5a52;--touch:#ef5b63;--mark:#64c184}}\n"
          "body{background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,sans-serif;margin:0;padding:16px;max-width:1200px}\n"
          "#report{font:12px/1.5 ui-monospace,Menlo,monospace;white-space:pre-wrap;border-bottom:1px solid var(--line);padding-bottom:12px}\n"
          "svg{width:100%;height:110px;display:block;margin:12px 0}h2{font-size:15px;margin:22px 0 8px}\n"
          ".row{display:flex;gap:10px;overflow-x:auto;padding-bottom:6px}figure{margin:0;flex:0 0 auto;width:180px}\n"
          ".pic{position:relative;line-height:0}.pic img{width:180px;border:1px solid var(--line)}\n"
          ".box{position:absolute;border:1.5px solid var(--chg)}.box.s{border-color:var(--scr)}.box.t{border-color:var(--tick);border-style:dashed}\n"
          "figcaption{font:11px/1.35 ui-monospace,monospace;color:var(--dim);margin-top:4px}figcaption b{color:var(--fg)}\n"
          "</style>\n", o);

    // the timeline drawing: one bar per frame (height = how much changed), bursts shaded,
    // touches (red) and log lines (green) as ticks underneath
    double D = f->dur > 0 ? f->dur : 1;
    int cells = f->cols * f->rows;
    fprintf(o, "<svg viewBox=\"0 0 1000 110\" preserveAspectRatio=\"none\" role=\"img\" aria-label=\"timeline\">\n");
    for (int b = 0; b < f->nbu; b++)
    {
        fprintf(o, "<a href=\"#b%d\"><rect x=\"%.2f\" y=\"0\" width=\"%.2f\" height=\"80\" fill=\"var(--chg)\" opacity=\".15\"><title>change #%d</title></rect></a>\n",
                b + 1, f->bu[b].t0 / D * 1000, fmax(2, (f->bu[b].t1 - f->bu[b].t0) / D * 1000), b + 1);
    }
    for (int i = 1; i < f->nfr; i++)
    {
        int ac = active_cells(&f->fr[i]), sc = 0;
        for (int k = 0; k < f->fr[i].nr; k++)
        {
            if (f->fr[i].r[k].dy)
                sc = 1;
        }
        double hgt = ac ? 6 + 74.0 * sqrt((double)ac / cells) : 4;
        fprintf(o, "<rect x=\"%.2f\" y=\"%.1f\" width=\"1.2\" height=\"%.1f\" fill=\"var(--%s)\"/>\n", f->fr[i].t / D * 1000, 80 - hgt, hgt, ac ? (sc ? "scr" : "chg") : "tick");
    }
    for (int i = 0; i < f->ntc; i++)
    {
        if (f->tc[i].kind != 'm')
            fprintf(o, "<rect x=\"%.2f\" y=\"84\" width=\"1.5\" height=\"12\" fill=\"var(--touch)\"/>\n", f->tc[i].t / D * 1000);
    }
    for (int i = 0; i < f->nmk; i++)
    {
        if (f->mk[i].t >= 0 && f->mk[i].t <= f->dur)
        {
            fprintf(o, "<rect x=\"%.2f\" y=\"98\" width=\"1.5\" height=\"12\" fill=\"var(--mark)\"><title>", f->mk[i].t / D * 1000);
            put_esc(o, f->mk[i].text);
            fprintf(o, "</title></rect>\n");
        }
    }
    fprintf(o, "</svg>\n");

    // the key frames: a row for the start and end of the video, then a row per burst,
    // each picture with its change boxes drawn over it (orange change, blue scroll, dashed grey ticker)
    char w[32];
    for (int b = -1; b < f->nbu; b++)
    {
        if (b < 0)
            fprintf(o, "<h2>Start and end</h2><div class=\"row\">\n");
        else
            fprintf(o, "<h2 id=\"b%d\">Change #%d at %.1f ms, %.0f ms long</h2><div class=\"row\">\n", b + 1, b + 1, f->bu[b].t0, f->bu[b].t1 - f->bu[b].t0);
        for (int i = 0; i < f->nfr; i++)
        {
            frame *fr = &f->fr[i];
            if (!fr->key)
                continue;
            int in = b < 0 ? (i == 0 || i == f->nfr - 1) : (i == f->bu[b].before || (i >= f->bu[b].first && i <= f->bu[b].last));
            if (!in)
                continue;
            // a frame shown in two rows carries its id only the first time
            if (fr->shown)
                fprintf(o, "<figure><div class=\"pic\"><img alt=\"frame %d\" src=\"data:image/%s;base64,", i, fr->png ? "png" : "jpeg");
            else
                fprintf(o, "<figure><div class=\"pic\"><img id=\"f%d\" alt=\"frame %d\" src=\"data:image/%s;base64,", i, i, fr->png ? "png" : "jpeg");
            fr->shown = 1;
            if (fr->img)
                put_b64(o, fr->img, fr->nimg);
            fprintf(o, "\">");
            for (int k = 0; k < fr->nr; k++)
            {
                rect *r = &fr->r[k];
                fprintf(o, "<div class=\"box%s\" style=\"left:%.2f%%;top:%.2f%%;width:%.2f%%;height:%.2f%%\"></div>", r->ticker ? " t" : r->dy ? " s" : "",
                        100.0 * r->x0 / f->w, 100.0 * r->y0 / f->h, 100.0 * (r->x1 - r->x0) / f->w, 100.0 * (r->y1 - r->y0) / f->h);
            }
            wall_str(f, fr->t, w, sizeof w);
            fprintf(o, "</div><figcaption><b>f%d</b> %.1f ms<br>%s", i, fr->t, w);
            if (b >= 0 && i != f->bu[b].before)
                fprintf(o, "<br>%+.0f ms into #%d", fr->t - f->bu[b].t0, b + 1);
            if (b >= 0 && i == f->bu[b].before)
                fprintf(o, "<br>before");
            fprintf(o, "</figcaption></figure>\n");
        }
        fprintf(o, "</div>\n");
    }

    // everything, for scripts: every frame's boxes, the touches and the log lines
    fprintf(o, "<script type=\"application/json\" id=\"data\">\n{\"video\":");
    put_json_str(o, name);
    fprintf(o, ",\"width\":%d,\"height\":%d,\"duration_ms\":%.1f,\"utc_offset_s\":%d,\"wall0_ms\":", f->sw, f->sh, f->dur, f->utc_off);
    if (f->have_wall)
        fprintf(o, "%.1f", f->wall0);
    else
        fputs("null", o);
    fprintf(o, ",\n\"frames\":[");
    for (int i = 0; i < f->nfr; i++)
    {
        frame *fr = &f->fr[i];
        fprintf(o, "%s\n{\"i\":%d,\"t\":%.1f,\"burst\":%d,\"key\":%d,\"boxes\":[", i ? "," : "", i, fr->t, fr->burst + 1, fr->key);
        for (int k = 0; k < fr->nr; k++)
        {
            rect *r = &fr->r[k];
            fprintf(o, "%s{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"cells\":%d,\"scroll_dy\":%d,\"ticker\":%d}", k ? "," : "", src_x(f, r->x0), src_y(f, r->y0),
                    src_x(f, r->x1 - r->x0), src_y(f, r->y1 - r->y0), r->cells, r->dy, r->ticker);
        }
        fprintf(o, "]}");
    }
    fprintf(o, "],\n\"touches\":[");
    for (int i = 0; i < f->ntc; i++)
        fprintf(o, "%s{\"t\":%.1f,\"kind\":\"%c\",\"x\":%d,\"y\":%d}", i ? "," : "", f->tc[i].t, f->tc[i].kind, f->tc[i].x, f->tc[i].y);
    fprintf(o, "],\n\"log\":[");
    for (int i = 0; i < f->nmk; i++)
    {
        fprintf(o, "%s{\"t\":%.1f,\"text\":", i ? "," : "", f->mk[i].t);
        put_json_str(o, f->mk[i].text);
        fputc('}', o);
    }
    fprintf(o, "]}\n</script>\n</body></html>\n");
    return ferror(o) ? -1 : 0;
}
