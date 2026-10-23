#include "guesser_view.h"

#include <furi.h>
#include <gui/elements.h>

/* Keep in sync with tools/preview/gscreens.py, which redraws every screen
 * on the PC with the real Flipper fonts for the README pictures. */

#define GS_TICK_MS    40 /* 25 fps */
#define GS_INTRO_LEN  56 /* ticks the intro plays before the menu */
#define GS_BUSY_DELAY 12 /* ticks of work before the thinking screen shows */
#define GS_WRAP_LINES 4
#define GS_WRAP_WIDTH 124

typedef enum {
    GsModeIntro,
    GsModeMenu,
    GsModeThinking,
    GsModeQuestion,
    GsModeGuess,
    GsModeWin,
    GsModeLose,
    GsModeStats,
    GsModeHelp,
    GsModeDialog,
} GsMode;

typedef enum {
    GsMoodNormal,
    GsMoodHappy,
    GsMoodSad,
    GsMoodWide,
} GsMood;

struct GuesserView {
    View* view;
    FuriTimer* timer;
    GuesserViewCallback callback;
    void* context;
};

typedef struct {
    uint8_t mode;
    uint32_t tick;
    bool busy; /* the app is working: keep the screen, ignore keys */
    uint8_t busy_t; /* ticks spent busy */
    bool fired; /* intro: IntroDone already sent */
    uint8_t sel; /* menu item, answer or help page */
    int16_t anim; /* animated selection position (pixels) */
    int16_t anim_w; /* animated selection width */
    uint16_t asked;
    uint16_t total;
    uint16_t num[5];
    uint8_t guess_number;
    char text[TQ_MAX_QTEXT + 1];
    char line1[40];
    char line2[40];
    char left[16];
    char right[16];
} GsModel;

/* ---------------------------------------------------------------- helpers */

static const int8_t gs_sine[32] = {0,   12,  24,  36,  45,  53,  59,  63,  64,  63,  59,
                                   53,  45,  36,  24,  12,  0,   -12, -24, -36, -45, -53,
                                   -59, -63, -64, -63, -59, -53, -45, -36, -24, -12};

static inline int32_t gs_sin(uint32_t i) {
    return gs_sine[i & 31u];
}

static inline int32_t gs_cos(uint32_t i) {
    return gs_sine[(i + 8u) & 31u];
}

static inline int32_t gs_abs(int32_t v) {
    return v < 0 ? -v : v;
}

/* Moves *pos one easing step towards target; always arrives. */
static void gs_ease(int16_t* pos, int16_t target) {
    int16_t d = (int16_t)(target - *pos);
    if(d == 0) return;
    int16_t step = (int16_t)(d / 2);
    if(step == 0) step = (d > 0) ? 1 : -1;
    *pos = (int16_t)(*pos + step);
}

static void gs_vline(Canvas* c, int32_t x, int32_t y, int32_t h) {
    if(h > 0) canvas_draw_line(c, x, y, x, y + h - 1);
}

static void gs_sparkle(Canvas* c, int32_t x, int32_t y, int32_t s) {
    canvas_draw_line(c, x - s, y, x + s, y);
    canvas_draw_line(c, x, y - s, x, y + s);
}

/**
 * The background: a slowly drifting, twinkling star field in three depths,
 * drawn only inside the box (x0, y0)-(x1, y1) so it never touches text.
 */
static void gs_stars(Canvas* c, uint32_t t, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    static const uint8_t div[3] = {16, 8, 4}; /* far stars drift slowest */
    for(uint32_t i = 0; i < 18; i++) {
        uint32_t layer = i % 3u;
        int32_t x = (int32_t)((i * 53u + 17u + 1024u - (t / div[layer]) % 128u) % 128u);
        int32_t y = (int32_t)((i * 29u + 7u) % 64u);
        if(x < x0 || x > x1 || y < y0 || y > y1) continue;
        uint32_t tw = (t + i * 7u) % 60u;
        if(tw < 3u) continue; /* twinkle off */
        if(layer == 2 && tw > 52u && x - 1 >= x0 && x + 1 <= x1 && y - 1 >= y0 && y + 1 <= y1) {
            gs_sparkle(c, x, y, 1); /* a near star flares up */
        } else {
            canvas_draw_dot(c, x, y);
        }
    }
}

/**
 * A glint sweeping over a box now and then: a slanted 2 px band drawn in
 * XOR, so it brightens dark areas and darkens light ones.
 */
static void
    gs_shine(Canvas* c, uint32_t t, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t period) {
    int32_t pos = (int32_t)(t % period) * 4 - h;
    if(pos > w + h) return;
    canvas_set_color(c, ColorXOR);
    for(int32_t row = 0; row < h; row++) {
        int32_t sx = x + pos + (h - row) / 2;
        for(int32_t k = 0; k < 2; k++) {
            if(sx + k >= x && sx + k < x + w) canvas_draw_dot(c, sx + k, y + row);
        }
    }
    canvas_set_color(c, ColorBlack);
}

/* A small filled triangle pointing left (dir < 0) or right (dir > 0). */
static void gs_arrow(Canvas* c, int32_t x, int32_t y, int32_t dir) {
    for(int32_t i = 0; i < 3; i++) {
        int32_t col = (dir < 0) ? x + i : x + 2 - i;
        gs_vline(c, col, y - i, 2 * i + 1);
    }
}

/** A rounded button; filled = inverted. arrow: -1 left, 1 right, 0 none. */
static void gs_pill(
    Canvas* c,
    int32_t x,
    int32_t y,
    int32_t w,
    const char* label,
    bool filled,
    int32_t arrow) {
    if(filled) {
        canvas_draw_rbox(c, x, y, w, 12, 3);
        canvas_set_color(c, ColorWhite);
    } else {
        canvas_draw_rframe(c, x, y, w, 12, 3);
    }
    int32_t tx = x + w / 2;
    if(arrow < 0) {
        gs_arrow(c, x + 4, y + 6, -1);
        tx += 3;
    } else if(arrow > 0) {
        gs_arrow(c, x + w - 7, y + 6, 1);
        tx -= 3;
    }
    canvas_draw_str_aligned(c, tx, y + 10, AlignCenter, AlignBottom, label);
    canvas_set_color(c, ColorBlack);
}

/**
 * The mascot: a crystal ball with eyes on a little stand. lx/ly shift the
 * eyes (where it looks), mood changes them, tick drives the blinking.
 */
static void gs_orb(
    Canvas* c,
    int32_t cx,
    int32_t cy,
    int32_t r,
    int32_t lx,
    int32_t ly,
    GsMood mood,
    uint32_t tick) {
    if(r < 2) return;
    /* stand */
    if(r >= 5) {
        int32_t bw = r + r / 2;
        canvas_draw_box(c, cx - bw / 2, cy + r - 2, bw, 3);
        canvas_draw_box(c, cx - bw / 2 - 2, cy + r + 1, bw + 4, 2);
    }
    /* glass */
    canvas_set_color(c, ColorWhite);
    canvas_draw_disc(c, cx, cy, r - 1);
    canvas_set_color(c, ColorBlack);
    canvas_draw_circle(c, cx, cy, r);
    if(r >= 8) {
        int32_t hx = cx - r / 2, hy = cy - r / 2;
        canvas_draw_line(c, hx - 1, hy + 2, hx + 2, hy - 1);
        canvas_draw_dot(c, hx + 4, hy - 2);
    }
    /* eyes */
    int32_t ex = (r * 35) / 100;
    int32_t ey = cy - r / 6 + ly;
    int32_t er = r / 6;
    if(er < 1) er = 1;
    bool blink = (mood == GsMoodNormal) && ((tick + 7u) % 70u) < 3u;
    for(int32_t side = -1; side <= 1; side += 2) {
        int32_t x = cx + side * ex + lx;
        switch(mood) {
        case GsMoodHappy:
            canvas_draw_line(c, x - er - 1, ey + 1, x, ey - er);
            canvas_draw_line(c, x, ey - er, x + er + 1, ey + 1);
            break;
        case GsMoodSad: {
            /* closed, drooping eyes */
            canvas_draw_line(c, x - er - 1, ey, x, ey + er);
            canvas_draw_line(c, x, ey + er, x + er + 1, ey);
            break;
        }
        case GsMoodWide:
            canvas_draw_circle(c, x, ey, er + 1);
            canvas_draw_dot(c, x, ey);
            break;
        default:
            if(blink) {
                canvas_draw_line(c, x - er, ey, x + er, ey);
            } else if(r < 6) {
                canvas_draw_dot(c, x, ey);
            } else if(r < 10) {
                canvas_draw_box(c, x - 1, ey - 1, 2, 2); /* a disc of 1 looks like + */
            } else {
                canvas_draw_disc(c, x, ey, er);
            }
            break;
        }
    }
    /* mouth */
    if(r >= 8) {
        int32_t my = cy + r / 3 + ly / 2;
        if(mood == GsMoodHappy) {
            canvas_draw_line(c, cx - 3 + lx, my, cx - 1 + lx, my + 2);
            canvas_draw_line(c, cx - 1 + lx, my + 2, cx + 1 + lx, my + 2);
            canvas_draw_line(c, cx + 1 + lx, my + 2, cx + 3 + lx, my);
        } else if(mood == GsMoodSad) {
            canvas_draw_line(c, cx - 3 + lx, my + 2, cx - 1 + lx, my);
            canvas_draw_line(c, cx - 1 + lx, my, cx + 1 + lx, my);
            canvas_draw_line(c, cx + 1 + lx, my, cx + 3 + lx, my + 2);
        } else if(mood == GsMoodWide) {
            canvas_draw_circle(c, cx + lx, my + 1, 1);
        }
    }
}

/* Draw callbacks run only on the GUI thread, so the wrap buffers are static. */
static char gs_lines[GS_WRAP_LINES][TQ_MAX_QTEXT + 1];
static char gs_trial[TQ_MAX_QTEXT + 1];

/** Greedy word wrap; returns lines used or max_lines + 1 if it does not fit. */
static uint8_t gs_wrap(Canvas* c, const char* text, uint8_t max_lines, int32_t width) {
    uint8_t n = 0;
    size_t len = 0;
    gs_lines[0][0] = '\0';
    const char* p = text;
    while(*p) {
        while(*p == ' ')
            p++;
        if(!*p) break;
        const char* end = p;
        while(*end && *end != ' ')
            end++;
        size_t wlen = (size_t)(end - p);
        size_t tlen = len;
        memcpy(gs_trial, gs_lines[n], len);
        if(tlen) gs_trial[tlen++] = ' ';
        if(tlen >= TQ_MAX_QTEXT)
            wlen = 0;
        else if(tlen + wlen > TQ_MAX_QTEXT)
            wlen = TQ_MAX_QTEXT - tlen;
        memcpy(gs_trial + tlen, p, wlen);
        gs_trial[tlen + wlen] = '\0';
        if(len && (int32_t)canvas_string_width(c, gs_trial) > width) {
            if(++n >= max_lines) return (uint8_t)(max_lines + 1);
            memcpy(gs_lines[n], p, wlen);
            gs_lines[n][wlen] = '\0';
            len = wlen;
        } else {
            memcpy(gs_lines[n], gs_trial, tlen + wlen + 1);
            len = tlen + wlen;
        }
        p = end;
    }
    return (uint8_t)(n + 1);
}

/** Wrapped, centred text in the band [top, top + height); bold if it fits. */
static void gs_text_block(Canvas* c, const char* text, int32_t dx, int32_t top, int32_t height) {
    int32_t line_h = 10;
    canvas_set_font(c, FontPrimary);
    uint8_t n = gs_wrap(c, text, 3, GS_WRAP_WIDTH);
    if(n > 3) {
        canvas_set_font(c, FontSecondary);
        line_h = 9;
        n = gs_wrap(c, text, GS_WRAP_LINES, GS_WRAP_WIDTH);
        if(n > GS_WRAP_LINES) n = GS_WRAP_LINES;
    }
    int32_t y = top + (height - (int32_t)n * line_h) / 2;
    for(uint8_t i = 0; i < n; i++) {
        canvas_draw_str_aligned(c, 64 + dx, y + i * line_h, AlignCenter, AlignTop, gs_lines[i]);
    }
}

/* Small header used by the question, stats and help screens. */
static void gs_header(Canvas* c, const char* title, uint32_t tick) {
    gs_orb(c, 5, 5, 4, 0, 0, GsMoodNormal, tick);
    canvas_set_font(c, FontSecondary);
    canvas_draw_str(c, 13, 8, title);
}

/* ------------------------------------------------------------- answer bar */

static const char* const gs_answer_labels[TqAnswerCount] = {"No", "No?", "?", "Yes?", "Yes"};
static const char* const gs_answer_names[TqAnswerCount] =
    {"No", "Probably not", "Don't know", "Probably", "Yes"};

typedef struct {
    int16_t x[TqAnswerCount];
    int16_t w[TqAnswerCount];
} GsBar;

/* Five equal buttons across the full width: 5 x 24 px + 4 gaps of 2 px. */
static void gs_bar_layout(Canvas* c, GsBar* bar) {
    canvas_set_font(c, FontSecondary);
    for(uint8_t i = 0; i < TqAnswerCount; i++) {
        bar->x[i] = (int16_t)(i * 26);
        bar->w[i] = 24;
    }
}

/* Selection targets need font metrics, which only a canvas knows: the bar
 * layout is cached the first time it is drawn, for the animation timer. */
static GsBar gs_bar_cache;
static bool gs_bar_cached;

static void gs_draw_bar(Canvas* c, const GsModel* m, int32_t y) {
    GsBar bar;
    gs_bar_layout(c, &bar);
    gs_bar_cache = bar;
    gs_bar_cached = true;
    for(uint8_t i = 0; i < TqAnswerCount; i++) {
        canvas_draw_rframe(c, bar.x[i], y, bar.w[i], 12, 3);
    }
    /* the highlight glides between answers */
    canvas_draw_rbox(c, m->anim, y, m->anim_w ? m->anim_w : bar.w[m->sel], 12, 3);
    for(uint8_t i = 0; i < TqAnswerCount; i++) {
        bool inside = bar.x[i] + bar.w[i] / 2 >= m->anim &&
                      bar.x[i] + bar.w[i] / 2 < m->anim + m->anim_w;
        canvas_set_color(c, inside ? ColorWhite : ColorBlack);
        canvas_draw_str_aligned(
            c, bar.x[i] + bar.w[i] / 2, y + 10, AlignCenter, AlignBottom, gs_answer_labels[i]);
    }
    canvas_set_color(c, ColorBlack);
}

/* ---------------------------------------------------------------- screens */

static void gs_draw_intro(Canvas* c, const GsModel* m) {
    uint32_t t = m->tick;
    /* the ball pops in with a small overshoot */
    int32_t r = 14;
    if(t < 10) r = (int32_t)t * 17 / 10;
    else if(t < 14) r = 17 - ((int32_t)t - 10);
    int32_t look = (t > 20 && t < 40) ? gs_sin(t) * 2 / 64 : 0;
    if(t > 4) gs_stars(c, t, 0, 0, 127, 36);
    gs_orb(c, 64, 22, r, look, 0, (t > 44) ? GsMoodHappy : GsMoodNormal, t);
    /* sparkles circling the ball */
    if(t > 6) {
        for(uint32_t k = 0; k < 4; k++) {
            uint32_t a = t + k * 8u;
            int32_t sx = 64 + gs_cos(a) * 26 / 64;
            int32_t sy = 22 + gs_sin(a) * 13 / 64;
            gs_sparkle(c, sx, sy, ((t + k) % 6u < 3u) ? 2 : 1);
        }
    }
    /* title letters drop in one by one */
    static const char title[] = "GUESSER";
    canvas_set_font(c, FontPrimary);
    int32_t tw = (int32_t)canvas_string_width(c, title) + 6 * 2;
    int32_t x = 64 - tw / 2;
    char ch[2] = {0, 0};
    for(uint32_t i = 0; i < sizeof(title) - 1; i++) {
        ch[0] = title[i];
        int32_t w = (int32_t)canvas_string_width(c, ch);
        int32_t start = 14 + (int32_t)i * 3;
        int32_t dy = (int32_t)t - start;
        if(dy >= 0) {
            int32_t y = 51 - (dy < 5 ? (5 - dy) * 3 : 0);
            canvas_draw_str(c, x, y, ch);
        }
        x += w + 2;
    }
    if(t > 34) gs_shine(c, t - 34u, 64 - tw / 2 - 2, 40, tw + 4, 12, 200);
    /* the subtitle types itself */
    canvas_set_font(c, FontSecondary);
    if(t > 36) {
        static const char sub[] = "Think of anything";
        size_t n = (t - 36u) * 2u;
        if(n > sizeof(sub) - 1) n = sizeof(sub) - 1;
        char buf[sizeof(sub)];
        memcpy(buf, sub, n);
        buf[n] = '\0';
        canvas_draw_str(c, 64 - (int32_t)canvas_string_width(c, sub) / 2, 63, buf);
    }
}

static const char* const gs_menu_labels[GuesserMenuCount] = {"Play", "How to play", "Stats", "Settings"};

static void gs_draw_menu(Canvas* c, const GsModel* m) {
    uint32_t t = m->tick;
    /* left: the mascot watching the menu */
    int32_t ly = (int32_t)m->sel - 1;
    gs_stars(c, t, 0, 0, 43, 54);
    /* the ball floats gently */
    int32_t fy = gs_sin(t / 2u) * 2 / 64;
    gs_orb(c, 21, 27 + fy, 14, 2, ly, GsMoodNormal, t);
    uint32_t phase = (t / 7u) % 6u;
    if(phase == 0) gs_sparkle(c, 6, 10, 2);
    if(phase == 2) gs_sparkle(c, 37, 8, 2);
    if(phase == 4) gs_sparkle(c, 39, 40, 1);
    canvas_set_font(c, FontSecondary);
    canvas_draw_str_aligned(c, 21, 63, AlignCenter, AlignBottom, "v1.1");
    /* right: title and items */
    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 50, 10, "GUESSER");
    gs_shine(c, t + 120u, 48, 1, 54, 10, 160);
    /* the underline draws itself out from the left */
    int32_t ul = t < 9u ? 46 + (int32_t)t * 9 : 127;
    canvas_draw_line(c, 46, 12, ul, 12);
    canvas_draw_rbox(c, 46, m->anim, 82, 12, 3);
    for(uint8_t i = 0; i < GuesserMenuCount; i++) {
        int32_t y = 14 + i * 12;
        /* the items glide in one after another */
        int32_t in = (int32_t)t - (int32_t)i * 2;
        int32_t dx = in < 6 ? (6 - (in < 0 ? 0 : in)) * 12 : 0;
        bool on = (m->anim + 6 >= y && m->anim + 6 < y + 12);
        canvas_set_color(c, on ? ColorWhite : ColorBlack);
        canvas_draw_str(c, 52 + dx, y + 9, gs_menu_labels[i]);
        if(on) gs_arrow(c, 120, y + 5, 1);
        canvas_set_color(c, ColorBlack);
    }
    gs_shine(c, t, 46, m->anim, 82, 12, 110);
}

static void gs_draw_thinking(Canvas* c, const GsModel* m) {
    uint32_t t = m->tick;
    static const int8_t look[4][2] = {{-3, -2}, {3, -2}, {3, 1}, {-3, 1}};
    const int8_t* l = look[(t / 6u) % 4u];
    gs_stars(c, t, 0, 0, 127, 47);
    /* three dots orbit the ball; the glass hides them as they pass behind */
    for(uint32_t k = 0; k < 3; k++) {
        uint32_t a = t * 2u + k * 11u;
        int32_t s = gs_sin(a);
        int32_t x = 64 + gs_cos(a) * 25 / 64;
        int32_t y = 22 + s * 8 / 64;
        canvas_draw_disc(c, x, y, s > 0 ? 2 : 1);
    }
    gs_orb(c, 64, 26, 15, l[0], l[1], GsMoodNormal, t);
    canvas_set_font(c, FontPrimary);
    static const char* const dots[4] = {"Thinking", "Thinking.", "Thinking..", "Thinking..."};
    int32_t w = (int32_t)canvas_string_width(c, "Thinking...");
    canvas_draw_str(c, 64 - w / 2, 59, dots[(t / 5u) % 4u]);
}

static void gs_draw_question(Canvas* c, GsModel* m) {
    if(!m->anim_w && gs_bar_cached) {
        m->anim = gs_bar_cache.x[m->sel];
        m->anim_w = gs_bar_cache.w[m->sel];
    }
    char title[24];
    snprintf(title, sizeof(title), "Question %u", m->asked + 1u);
    gs_header(c, title, m->tick);
    /* progress */
    canvas_draw_rframe(c, 72, 2, 54, 7, 2);
    uint32_t total = m->total ? m->total : 1u;
    uint32_t fill = 50u * (m->asked < total ? m->asked : total) / total;
    if(fill) canvas_draw_box(c, 74, 4, (int32_t)fill, 3);
    if(fill > 2) gs_shine(c, m->tick, 74, 4, (int32_t)fill, 3, 90);
    /* the question slides in from the right, and out to the left once it
     * is answered -- the next one follows straight away */
    int32_t dx = (m->tick < 6u) ? (int32_t)(6u - m->tick) * 22 : 0;
    if(m->busy) dx = -(int32_t)(m->busy_t + 1u) * 22;
    if(dx > -128) gs_text_block(c, m->text, dx, 12, 31);
    /* what the highlighted button means, in full (blinks once chosen) */
    canvas_set_font(c, FontSecondary);
    if(!m->busy || (m->busy_t / 2u) % 2u == 0u)
        canvas_draw_str_aligned(c, 64, 50, AlignCenter, AlignBottom, gs_answer_names[m->sel]);
    gs_draw_bar(c, m, 52);
}

static void gs_draw_guess(Canvas* c, const GsModel* m) {
    uint32_t t = m->tick;
    if(t < 14) {
        gs_stars(c, t, 0, 10, 127, 63);
        /* the ball lights up: rings flowing outwards */
        for(uint32_t k = 0; k < 3; k++) {
            int32_t rr = 17 + (int32_t)((t * 2u + k * 8u) % 24u);
            if(rr < 40) canvas_draw_circle(c, 64, 30, rr);
        }
        gs_orb(c, 64, 30, 14, 0, 0, GsMoodWide, t);
        canvas_set_font(c, FontSecondary);
        canvas_draw_str_aligned(c, 64, 8, AlignCenter, AlignBottom, "Hmm...");
        return;
    }
    uint32_t s = t - 14u;
    canvas_set_font(c, FontSecondary);
    canvas_draw_str_aligned(
        c, 64, 9, AlignCenter, AlignBottom, m->guess_number ? "Then maybe..." : "I think it's...");
    /* the name types itself into a box */
    char shout[TQ_MAX_NAME + 1];
    size_t len = strlen(m->text);
    for(size_t i = 0; i <= len; i++) {
        char ch = m->text[i];
        shout[i] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;
    }
    canvas_set_font(c, FontPrimary);
    if((int32_t)canvas_string_width(c, shout) > 116) canvas_set_font(c, FontSecondary);
    size_t n = s < len ? s + 1u : len;
    char vis[TQ_MAX_NAME + 1];
    memcpy(vis, shout, n);
    vis[n] = '\0';
    canvas_draw_rframe(c, 2, 13, 124, 22, 4);
    if(s >= len && ((s - len) / 3u) % 2u == 0u && s < len + 9u) {
        canvas_draw_rframe(c, 0, 11, 128, 26, 5); /* flash when complete */
    }
    int32_t full = (int32_t)canvas_string_width(c, shout);
    canvas_draw_str(c, 64 - full / 2, 28, vis);
    /* mascot between the buttons */
    gs_orb(c, 64, 49, 8, 0, 0, s >= len ? GsMoodHappy : GsMoodWide, t);
    canvas_set_font(c, FontSecondary);
    gs_pill(c, 0, 52, 36, "No", false, -1);
    gs_pill(c, 90, 52, 38, "Yes!", true, 1);
}

static void gs_confetti(Canvas* c, uint32_t t) {
    for(uint32_t i = 0; i < 18; i++) {
        int32_t x = (int32_t)((i * 37u + 11u) % 128u) + gs_sin(t + i * 5u) * 3 / 64;
        uint32_t speed = 1u + (i * 7u) % 3u;
        int32_t y = (int32_t)((t * speed + i * 13u) % 72u) - 6;
        switch(i % 3u) {
        case 0:
            canvas_draw_box(c, x, y, 2, 2);
            break;
        case 1:
            canvas_draw_line(c, x, y, x + 1, y + 2);
            break;
        default:
            canvas_draw_dot(c, x, y);
            canvas_draw_dot(c, x + 1, y + 1);
            break;
        }
    }
}

static void gs_draw_win(Canvas* c, const GsModel* m) {
    uint32_t t = m->tick;
    gs_confetti(c, t);
    int32_t bounce = gs_abs(gs_sin(t * 2u)) * 4 / 64;
    gs_orb(c, 26, 27 - bounce, 14, 0, 0, GsMoodHappy, t);
    canvas_set_color(c, ColorWhite);
    canvas_draw_box(c, 48, 8, 80, 40);
    canvas_set_color(c, ColorBlack);
    canvas_set_font(c, FontPrimary);
    canvas_draw_str_aligned(c, 88, 19, AlignCenter, AlignBottom, "Got it!");
    canvas_set_font(c, FontSecondary);
    char line[32];
    snprintf(line, sizeof(line), "in %u question%s", m->asked, m->asked == 1 ? "" : "s");
    canvas_draw_str_aligned(c, 88, 31, AlignCenter, AlignBottom, line);
    if(m->guess_number) {
        snprintf(line, sizeof(line), "on guess %u", m->guess_number + 1u);
    } else {
        snprintf(line, sizeof(line), "Win rate %u%%", m->num[0]);
    }
    canvas_draw_str_aligned(c, 88, 41, AlignCenter, AlignBottom, line);
    gs_pill(c, 70, 51, 36, "OK", true, 0);
    gs_shine(c, t, 70, 51, 36, 12, 70);
}

static void gs_draw_lose(Canvas* c, const GsModel* m) {
    uint32_t t = m->tick;
    gs_stars(c, t, 0, 0, 46, 63);
    gs_orb(c, 26, 27, 14, 0, 1, GsMoodSad, t);
    /* a tear rolls down */
    int32_t ty = 28 + (int32_t)(t % 18u) / 2;
    if(ty < 36) {
        canvas_draw_dot(c, 26 - 7, ty);
        canvas_draw_box(c, 26 - 8, ty + 1, 3, 2);
    }
    canvas_set_font(c, FontPrimary);
    canvas_draw_str_aligned(c, 88, 17, AlignCenter, AlignBottom, "You win!");
    canvas_set_font(c, FontSecondary);
    canvas_draw_str_aligned(c, 88, 29, AlignCenter, AlignBottom, "I couldn't guess");
    canvas_draw_str_aligned(c, 88, 38, AlignCenter, AlignBottom, "it. Teach me?");
    gs_pill(c, 48, 51, 36, "Menu", false, -1);
    gs_pill(c, 88, 51, 40, "Teach", true, 1);
}

static void gs_stat_box(Canvas* c, int32_t x, int32_t y, const char* label, const char* value) {
    canvas_draw_rframe(c, x, y, 61, 21, 3);
    canvas_set_font(c, FontSecondary);
    canvas_draw_str(c, x + 4, y + 8, label);
    canvas_set_font(c, FontPrimary);
    canvas_draw_str_aligned(c, x + 57, y + 19, AlignRight, AlignBottom, value);
}

static void gs_draw_stats(Canvas* c, const GsModel* m) {
    uint32_t t = m->tick;
    gs_header(c, "Stats", t);
    gs_stars(c, t, 40, 0, 127, 9);
    char v[16];
    /* the boxes pop in one by one */
    if(t >= 1u) {
        snprintf(v, sizeof(v), "%u", m->num[0]);
        gs_stat_box(c, 2, 11, "Games", v);
    }
    if(t >= 3u) {
        snprintf(v, sizeof(v), "%u", m->num[1]);
        gs_stat_box(c, 65, 11, "Won by me", v);
    }
    if(t >= 5u) {
        snprintf(
            v, sizeof(v), "%u%%", m->num[0] ? (unsigned)(100u * m->num[1] / m->num[0]) : 0u);
        gs_stat_box(c, 2, 33, "Win rate", v);
    }
    if(t >= 7u) {
        if(m->num[2])
            snprintf(v, sizeof(v), "%u", m->num[2]);
        else
            snprintf(v, sizeof(v), "-");
        gs_stat_box(c, 65, 33, "Fastest", v);
    }
    canvas_set_font(c, FontSecondary);
    char line[40];
    snprintf(line, sizeof(line), "Avg %u questions  -  %u taught", m->num[3], m->num[4]);
    canvas_draw_str_aligned(c, 64, 62, AlignCenter, AlignBottom, line);
}

static void gs_draw_help(Canvas* c, const GsModel* m) {
    uint32_t t = m->tick;
    switch(m->sel) {
    case 0: {
        gs_stars(c, t, 0, 0, 38, 52);
        gs_orb(c, 24, 30, 13, 2, -2, GsMoodNormal, t);
        /* thought bubble with a question mark */
        canvas_draw_circle(c, 40, 13, 1);
        canvas_draw_circle(c, 44, 8, 2);
        canvas_draw_rframe(c, 48, 1, 14, 14, 5);
        canvas_set_font(c, FontPrimary);
        canvas_draw_str_aligned(c, 55, 12, AlignCenter, AlignBottom, "?");
        canvas_draw_str(c, 66, 24, "Think of");
        canvas_draw_str(c, 66, 35, "anything!");
        canvas_set_font(c, FontSecondary);
        canvas_draw_str(c, 66, 45, "animal, food,");
        canvas_draw_str(c, 66, 53, "person, thing...");
        break;
    }
    case 1: {
        canvas_set_font(c, FontPrimary);
        canvas_draw_str_aligned(c, 64, 10, AlignCenter, AlignBottom, "Answer honestly");
        GsModel demo = *m;
        demo.sel = (uint8_t)((t / 12u) % TqAnswerCount);
        GsBar bar;
        gs_bar_layout(c, &bar);
        demo.anim = bar.x[demo.sel];
        demo.anim_w = bar.w[demo.sel];
        gs_draw_bar(c, &demo, 14);
        canvas_set_font(c, FontSecondary);
        canvas_draw_str_aligned(c, 64, 35, AlignCenter, AlignBottom, gs_answer_names[demo.sel]);
        canvas_draw_str_aligned(c, 64, 45, AlignCenter, AlignBottom, "Up = Yes     Down = No");
        canvas_draw_str_aligned(c, 64, 54, AlignCenter, AlignBottom, "Back = undo, hold = quit");
        break;
    }
    default: {
        gs_stars(c, t, 0, 0, 44, 52);
        gs_orb(c, 24, 28, 13, 0, 0, GsMoodHappy, t);
        if((t / 6u) % 2u) gs_sparkle(c, 8, 9, 2);
        else gs_sparkle(c, 41, 12, 2);
        canvas_set_font(c, FontPrimary);
        canvas_draw_str(c, 48, 22, "I guess it!");
        canvas_set_font(c, FontSecondary);
        canvas_draw_str(c, 48, 33, "Wrong guess?");
        canvas_draw_str(c, 48, 42, "I ask a bit more.");
        canvas_draw_str(c, 48, 51, "Then teach me!");
        break;
    }
    }
    /* page dots */
    for(uint8_t i = 0; i < 3; i++) {
        int32_t x = 56 + i * 8;
        if(i == m->sel)
            canvas_draw_disc(c, x, 60, 2);
        else
            canvas_draw_circle(c, x, 60, 2);
    }
    if(m->sel > 0) gs_arrow(c, 1, 60, -1);
    if(m->sel < 2) gs_arrow(c, 124, 60, 1);
}

static void gs_draw_dialog(Canvas* c, const GsModel* m) {
    gs_stars(c, m->tick, 0, 0, 127, 18);
    gs_orb(c, 64, 9, 7, 0, 0, GsMoodNormal, m->tick);
    canvas_set_font(c, FontPrimary);
    canvas_draw_str_aligned(c, 64, 30, AlignCenter, AlignBottom, m->text);
    canvas_set_font(c, FontSecondary);
    canvas_draw_str_aligned(c, 64, 40, AlignCenter, AlignBottom, m->line1);
    canvas_draw_str_aligned(c, 64, 49, AlignCenter, AlignBottom, m->line2);
    if(m->right[0]) {
        int32_t lw = (int32_t)canvas_string_width(c, m->left) + 16;
        int32_t rw = (int32_t)canvas_string_width(c, m->right) + 16;
        gs_pill(c, 0, 52, lw, m->left, false, -1);
        gs_pill(c, 128 - rw, 52, rw, m->right, true, 1);
    } else {
        gs_pill(c, 44, 52, 40, m->left[0] ? m->left : "OK", true, 0);
    }
}

static void gs_draw(Canvas* canvas, void* model) {
    GsModel* m = model;
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    switch(m->mode) {
    case GsModeIntro:
        gs_draw_intro(canvas, m);
        break;
    case GsModeMenu:
        gs_draw_menu(canvas, m);
        break;
    case GsModeThinking:
        gs_draw_thinking(canvas, m);
        break;
    case GsModeQuestion:
        gs_draw_question(canvas, m);
        break;
    case GsModeGuess:
        gs_draw_guess(canvas, m);
        break;
    case GsModeWin:
        gs_draw_win(canvas, m);
        break;
    case GsModeLose:
        gs_draw_lose(canvas, m);
        break;
    case GsModeStats:
        gs_draw_stats(canvas, m);
        break;
    case GsModeHelp:
        gs_draw_help(canvas, m);
        break;
    default:
        gs_draw_dialog(canvas, m);
        break;
    }
}

/* ------------------------------------------------------------- animation */

static void gs_update_targets(GsModel* m) {
    if(m->mode == GsModeMenu) {
        gs_ease(&m->anim, (int16_t)(13 + m->sel * 12));
    } else if(m->mode == GsModeQuestion && gs_bar_cached) {
        gs_ease(&m->anim, gs_bar_cache.x[m->sel]);
        gs_ease(&m->anim_w, gs_bar_cache.w[m->sel]);
    }
}

static void gs_timer(void* context) {
    GuesserView* view = context;
    bool fire = false;
    with_view_model(
        view->view,
        GsModel * m,
        {
            m->tick++;
            gs_update_targets(m);
            if(m->busy && m->mode != GsModeThinking && ++m->busy_t >= GS_BUSY_DELAY) {
                /* the turn takes a while: show that we are on it */
                m->mode = GsModeThinking;
                m->tick = 0;
            }
            if(m->mode == GsModeIntro && !m->fired && m->tick >= GS_INTRO_LEN) {
                m->fired = true;
                fire = true;
            }
        },
        true);
    if(fire && view->callback) view->callback(GuesserViewEventIntroDone, view->context);
}

static void gs_enter(void* context) {
    GuesserView* view = context;
    furi_timer_start(view->timer, furi_ms_to_ticks(GS_TICK_MS));
}

static void gs_exit(void* context) {
    GuesserView* view = context;
    furi_timer_stop(view->timer);
}

/* ------------------------------------------------------------------ input */

static bool gs_input(InputEvent* event, void* context) {
    GuesserView* view = context;
    uint32_t send = 0;
    bool consumed = true;
    bool is_short = event->type == InputTypeShort || event->type == InputTypeRepeat;

    with_view_model(
        view->view,
        GsModel * m,
        {
            uint8_t sel_before = m->sel;
            /* busy: the worker owns the engine, ignore keys */
            switch(m->busy ? GsModeThinking : m->mode) {
            case GsModeIntro:
                if(event->type == InputTypeShort && !m->fired && m->tick > 8) {
                    m->fired = true;
                    send = GuesserViewEventIntroDone;
                }
                break;
            case GsModeMenu:
                if(!is_short) break;
                if(event->key == InputKeyUp)
                    m->sel = (uint8_t)((m->sel + GuesserMenuCount - 1) % GuesserMenuCount);
                else if(event->key == InputKeyDown)
                    m->sel = (uint8_t)((m->sel + 1) % GuesserMenuCount);
                else if(event->key == InputKeyOk && event->type == InputTypeShort)
                    send = GuesserViewEventMenu + m->sel;
                else if(event->key == InputKeyBack && event->type == InputTypeShort)
                    send = GuesserViewEventBack;
                break;
            case GsModeQuestion:
                if(event->type == InputTypeLong && event->key == InputKeyBack) {
                    send = GuesserViewEventQuit;
                    break;
                }
                if(!is_short) break;
                if(event->key == InputKeyLeft && m->sel > 0)
                    m->sel--;
                else if(event->key == InputKeyRight && m->sel + 1 < TqAnswerCount)
                    m->sel++;
                else if(event->type == InputTypeShort && event->key == InputKeyOk)
                    send = GuesserViewEventAnswer + m->sel;
                else if(event->type == InputTypeShort && event->key == InputKeyUp)
                    send = GuesserViewEventAnswer + TqAnswerYes;
                else if(event->type == InputTypeShort && event->key == InputKeyDown)
                    send = GuesserViewEventAnswer + TqAnswerNo;
                else if(event->type == InputTypeShort && event->key == InputKeyBack)
                    send = GuesserViewEventUndo;
                break;
            case GsModeGuess:
                if(event->type != InputTypeShort || m->tick < 14) break;
                if(event->key == InputKeyRight || event->key == InputKeyOk)
                    send = GuesserViewEventGuessYes;
                else if(event->key == InputKeyLeft)
                    send = GuesserViewEventGuessNo;
                break;
            case GsModeWin:
                if(event->type == InputTypeShort &&
                   (event->key == InputKeyOk || event->key == InputKeyBack))
                    send = GuesserViewEventResultOk;
                break;
            case GsModeLose:
                if(event->type != InputTypeShort) break;
                if(event->key == InputKeyRight || event->key == InputKeyOk)
                    send = GuesserViewEventResultTeach;
                else if(event->key == InputKeyLeft || event->key == InputKeyBack)
                    send = GuesserViewEventResultOk;
                break;
            case GsModeHelp:
                if(!is_short) break;
                if(event->key == InputKeyLeft && m->sel > 0)
                    m->sel--;
                else if(event->key == InputKeyRight && m->sel < 2)
                    m->sel++;
                else if(event->type == InputTypeShort && event->key == InputKeyOk && m->sel < 2)
                    m->sel++;
                else if(event->type == InputTypeShort &&
                        (event->key == InputKeyBack || event->key == InputKeyOk))
                    send = GuesserViewEventBack;
                break;
            case GsModeStats:
                if(event->type == InputTypeShort &&
                   (event->key == InputKeyBack || event->key == InputKeyOk))
                    send = GuesserViewEventBack;
                break;
            case GsModeDialog:
                if(event->type != InputTypeShort) break;
                if(!m->right[0]) {
                    if(event->key == InputKeyOk || event->key == InputKeyBack)
                        send = GuesserViewEventDialogLeft;
                } else if(event->key == InputKeyRight || event->key == InputKeyOk) {
                    send = GuesserViewEventDialogRight;
                } else if(event->key == InputKeyLeft || event->key == InputKeyBack) {
                    send = GuesserViewEventDialogLeft;
                }
                break;
            default: /* thinking: the worker owns the engine, ignore keys */
                break;
            }
            if(!send && m->sel != sel_before) send = GuesserViewEventClick;
        },
        true);

    if(send && view->callback) view->callback(send, view->context);
    return consumed;
}

/* ------------------------------------------------------------- public api */

GuesserView* guesser_view_alloc(void) {
    GuesserView* view = malloc(sizeof(GuesserView));
    memset(view, 0, sizeof(*view));
    view->view = view_alloc();
    view_allocate_model(view->view, ViewModelTypeLocking, sizeof(GsModel));
    view_set_context(view->view, view);
    view_set_draw_callback(view->view, gs_draw);
    view_set_input_callback(view->view, gs_input);
    view_set_enter_callback(view->view, gs_enter);
    view_set_exit_callback(view->view, gs_exit);
    view->timer = furi_timer_alloc(gs_timer, FuriTimerTypePeriodic, view);
    return view;
}

void guesser_view_free(GuesserView* view) {
    furi_assert(view);
    furi_timer_stop(view->timer);
    furi_timer_free(view->timer);
    view_free(view->view);
    free(view);
}

View* guesser_view_get_view(GuesserView* view) {
    return view->view;
}

void guesser_view_set_callback(GuesserView* view, GuesserViewCallback callback, void* context) {
    view->callback = callback;
    view->context = context;
}

/* Resets the shared model for a new screen. */
#define GS_SHOW(view, mode_, body)                    \
    with_view_model(                                  \
        (view)->view,                                 \
        GsModel * m,                                  \
        {                                             \
            memset(m, 0, sizeof(*m));                 \
            m->mode = (mode_);                        \
            body                                      \
        },                                            \
        true)

static void gs_copy(char* dst, size_t size, const char* src) {
    if(!src) src = "";
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}

void guesser_view_show_intro(GuesserView* view) {
    GS_SHOW(view, GsModeIntro, {});
}

void guesser_view_set_busy(GuesserView* view) {
    with_view_model(
        view->view,
        GsModel * m,
        {
            m->busy = true;
            m->busy_t = 0;
        },
        true);
}

void guesser_view_show_menu(GuesserView* view, uint8_t selected) {
    GS_SHOW(view, GsModeMenu, {
        m->sel = selected < GuesserMenuCount ? selected : 0;
        m->anim = (int16_t)(13 + m->sel * 12);
    });
}

void guesser_view_show_thinking(GuesserView* view) {
    GS_SHOW(view, GsModeThinking, {});
}

void guesser_view_show_question(
    GuesserView* view,
    const char* text,
    uint16_t asked,
    uint16_t total,
    Tq_Answer default_answer) {
    GS_SHOW(view, GsModeQuestion, {
        gs_copy(m->text, sizeof(m->text), text);
        m->asked = asked;
        m->total = total;
        m->sel = (uint8_t)default_answer;
        if(gs_bar_cached) {
            m->anim = gs_bar_cache.x[m->sel];
            m->anim_w = gs_bar_cache.w[m->sel];
        }
    });
}

void guesser_view_show_guess(GuesserView* view, const char* name, uint8_t guess_number) {
    GS_SHOW(view, GsModeGuess, {
        gs_copy(m->text, TQ_MAX_NAME + 1, name);
        m->guess_number = guess_number;
    });
}

void guesser_view_show_win(GuesserView* view, uint16_t questions, uint8_t guess_number, uint8_t win_rate) {
    GS_SHOW(view, GsModeWin, {
        m->asked = questions;
        m->guess_number = guess_number;
        m->num[0] = win_rate;
    });
}

void guesser_view_show_lose(GuesserView* view) {
    GS_SHOW(view, GsModeLose, {});
}

void guesser_view_show_stats(
    GuesserView* view,
    uint16_t games,
    uint16_t wins,
    uint16_t best,
    uint16_t average,
    uint16_t learned) {
    GS_SHOW(view, GsModeStats, {
        m->num[0] = games;
        m->num[1] = wins;
        m->num[2] = best;
        m->num[3] = average;
        m->num[4] = learned;
    });
}

void guesser_view_show_help(GuesserView* view) {
    GS_SHOW(view, GsModeHelp, {});
}

void guesser_view_show_dialog(
    GuesserView* view,
    const char* title,
    const char* line1,
    const char* line2,
    const char* left,
    const char* right) {
    GS_SHOW(view, GsModeDialog, {
        gs_copy(m->text, sizeof(m->text), title);
        gs_copy(m->line1, sizeof(m->line1), line1);
        gs_copy(m->line2, sizeof(m->line2), line2);
        gs_copy(m->left, sizeof(m->left), left);
        gs_copy(m->right, sizeof(m->right), right);
    });
}
