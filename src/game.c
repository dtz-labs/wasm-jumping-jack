/*
 * Jumping Jack - game core. See game.h.
 *
 * Rules (after the 1983 original):
 *  - Jack starts on the ground below 8 horizontal lines.
 *  - Gaps travel along the lines: right-moving gaps wrap to the line below,
 *    left-moving gaps wrap to the line above.
 *  - Jump through a gap to climb one floor; every successful jump opens
 *    one more gap (up to 8). Jumping into solid line = bumped head + stun.
 *  - A gap passing under Jack drops him one floor (stunned). Falling all
 *    the way to the ground costs a life.
 *  - Hazards (one more per level) roam the floors and stun Jack on contact.
 *  - Reach the top floor to complete the level.
 */
#include "game.h"

typedef unsigned char u8;
typedef unsigned int u32;

/* ---------------------------------------------------------------- palette */

enum {
    BLACK, BLUE, RED, MAGENTA, GREEN, CYAN, YELLOW, WHITE,
    BBLACK, BBLUE, BRED, BMAGENTA, BGREEN, BCYAN, BYELLOW, BWHITE
};

/* 0xAABBGGRR so that little-endian memory reads R,G,B,A. */
static const u32 PAL[16] = {
    0xFF000000, 0xFFD70000, 0xFF0000D7, 0xFFD700D7,
    0xFF00D700, 0xFFD7D700, 0xFF00D7D7, 0xFFD7D7D7,
    0xFF000000, 0xFFFF0000, 0xFF0000FF, 0xFFFF00FF,
    0xFF00FF00, 0xFFFFFF00, 0xFF00FFFF, 0xFFFFFFFF,
};

/* ---------------------------------------------------------------- layout */

#define SW 256
#define SH 192
#define BX ((JJ_W - SW) / 2)
#define BY ((JJ_H - SH) / 2)

#define NFLOORS 8     /* lines 1..8; floor 0 is the ground */
#define FLOOR_H 19    /* vertical distance between floors */
#define LINE_T 3      /* line thickness */
#define GAPW 24
#define MAXGAPS 8
#define MAXHZ 20
#define GAP_SPEED 1
#define JACK_SPEED 2
#define JUMP_SPEED 2
#define FALL_SPEED 3
#define HIT_L 4       /* Jack hitbox inside his 16 px sprite */
#define HIT_R 12

#define STUN_BUMP 40
#define STUN_FALL 45
#define STUN_HAZARD 60
#define START_LIVES 6

static int floor_y(int k) { return 180 - FLOOR_H * k; }
static int line_up(int l) { return l >= NFLOORS ? 1 : l + 1; }
static int line_down(int l) { return l <= 1 ? NFLOORS : l - 1; }

/* ---------------------------------------------------------------- state */

enum { ST_TITLE, ST_PLAY, ST_DEAD, ST_LEVELDONE, ST_GAMEOVER };
enum { J_STAND, J_JUMP, J_FALL, J_BUMP, J_STUN };

typedef struct { int line, x, dir; } Gap;
typedef struct { int line, a, b; } Seg;
typedef struct { int floor, x, type, speed, sub; } Hazard;

typedef struct {
    int state, timer, paused;
    int level, lives;
    u32 score, hiscore, bonus;
    Gap gaps[MAXGAPS];
    int ngaps;
    Hazard hz[MAXHZ];
    int nhz;
    int jx, jfloor, jy, jst, jtimer, facing, walking, walkt;
    u32 keys, prev_keys, rng, frame;
    int tgap[3]; /* title screen demo gaps */
} Game;

static Game G;
static u32 fb[JJ_W * JJ_H];

/* ---------------------------------------------------------------- rng */

static u32 rnd(void)
{
    u32 x = G.rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    G.rng = x;
    return x;
}

static int rndn(int n) { return (int)(rnd() % (u32)n); }

/* ---------------------------------------------------------------- sound */

typedef struct { short f0, f1, len; } Note;
#define SNDQ 48
#define ABUF 4096
#define AMP 0.12f

static Note sq[SNDQ];
static int sq_n, sq_pos, sq_t;
static float abuf[ABUF];
static int alen, srate = 44100;
static u32 sacc;
static float phase;

static void snd_clear(void) { sq_n = sq_pos = sq_t = 0; }
static int snd_busy(void) { return sq_pos < sq_n; }

static void snd_add(int f0, int f1, int len)
{
    if (sq_n < SNDQ) {
        sq[sq_n].f0 = (short)f0;
        sq[sq_n].f1 = (short)f1;
        sq[sq_n].len = (short)len;
        sq_n++;
    }
}

enum { S_JUMP, S_LAND, S_BUMP, S_FALL, S_HIT, S_DIE, S_LEVEL, S_OVER, S_STEP, S_START };

static void snd_play(int fx)
{
    static const short tune_level[] = { 523, 659, 784, 1047, 784, 1047, 1319 };
    static const short tune_over[] = { 392, 370, 330, 311, 262, 196 };
    static const short tune_start[] = { 262, 330, 392, 523 };
    int i;

    if (fx == S_STEP) {
        if (!snd_busy()) { snd_clear(); snd_add(90, 90, 1); }
        return;
    }
    snd_clear();
    switch (fx) {
    case S_JUMP: snd_add(220, 1100, 9); break;
    case S_LAND: snd_add(1400, 1400, 1); break;
    case S_BUMP: snd_add(160, 60, 12); break;
    case S_FALL: snd_add(1000, 150, 10); break;
    case S_HIT:
        for (i = 0; i < 6; i++) snd_add(i & 1 ? 300 : 200, i & 1 ? 300 : 200, 3);
        break;
    case S_DIE: snd_add(700, 40, 60); break;
    case S_LEVEL:
        for (i = 0; i < 7; i++) { snd_add(tune_level[i], tune_level[i], 6); snd_add(0, 0, 1); }
        break;
    case S_OVER:
        for (i = 0; i < 6; i++) { snd_add(tune_over[i], tune_over[i], 12); snd_add(0, 0, 2); }
        break;
    case S_START:
        for (i = 0; i < 4; i++) { snd_add(tune_start[i], tune_start[i], 5); snd_add(0, 0, 1); }
        break;
    }
}

static void audio_frame(void)
{
    float f = 0.0f, inc;
    int i, n;

    sacc += (u32)srate;
    n = (int)(sacc / JJ_FPS);
    sacc %= JJ_FPS;
    if (n > ABUF) n = ABUF;

    if (snd_busy()) {
        const Note *nt = &sq[sq_pos];
        f = (float)nt->f0 + (float)(nt->f1 - nt->f0) * (float)sq_t / (float)nt->len;
        if (++sq_t >= nt->len) { sq_pos++; sq_t = 0; }
        if (!snd_busy()) snd_clear();
    }
    inc = f / (float)srate;
    for (i = 0; i < n; i++) {
        if (f > 0.0f) {
            phase += inc;
            if (phase >= 1.0f) phase -= 1.0f;
            abuf[i] = phase < 0.5f ? AMP : -AMP;
        } else {
            abuf[i] = 0.0f;
        }
    }
    alen = n;
}

/* ---------------------------------------------------------------- drawing */

static void clear_screen(int border, int paper)
{
    u32 b = PAL[border], p = PAL[paper];
    int x, y;
    for (y = 0; y < JJ_H; y++)
        for (x = 0; x < JJ_W; x++)
            fb[y * JJ_W + x] = (x >= BX && x < BX + SW && y >= BY && y < BY + SH) ? p : b;
}

static void pset(int x, int y, int c)
{
    if ((unsigned)x < SW && (unsigned)y < SH)
        fb[(y + BY) * JJ_W + x + BX] = PAL[c];
}

static void fill(int x, int y, int w, int h, int c)
{
    int i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            pset(x + i, y + j, c);
}

/* Sprites are arrays of strings, '#' = ink. Width w <= 16. */
static void draw_sprite(const char *const *rows, int w, int h, int x, int y,
                        int c, int flip, int wrap)
{
    int r, col;
    for (r = 0; r < h; r++) {
        for (col = 0; col < w; col++) {
            char ch = rows[r][flip ? w - 1 - col : col];
            int px = x + col;
            if (ch != '#') continue;
            if (wrap) px = ((px % SW) + SW) % SW;
            pset(px, y + r, c);
        }
    }
}

/* ---------------------------------------------------------------- font */

typedef struct { char ch; const char *rows[8]; } Glyph;

#define E "........"
static const Glyph GLYPHS[] = {
    { 'A', { E, "..####..", ".#....#.", ".#....#.", ".######.", ".#....#.", ".#....#.", E } },
    { 'B', { E, ".#####..", ".#....#.", ".#####..", ".#....#.", ".#....#.", ".#####..", E } },
    { 'C', { E, "..####..", ".#....#.", ".#......", ".#......", ".#....#.", "..####..", E } },
    { 'D', { E, ".####...", ".#...#..", ".#....#.", ".#....#.", ".#...#..", ".####...", E } },
    { 'E', { E, ".######.", ".#......", ".#####..", ".#......", ".#......", ".######.", E } },
    { 'F', { E, ".######.", ".#......", ".#####..", ".#......", ".#......", ".#......", E } },
    { 'G', { E, "..####..", ".#....#.", ".#......", ".#..###.", ".#....#.", "..####..", E } },
    { 'H', { E, ".#....#.", ".#....#.", ".######.", ".#....#.", ".#....#.", ".#....#.", E } },
    { 'I', { E, "..#####.", "....#...", "....#...", "....#...", "....#...", "..#####.", E } },
    { 'J', { E, "......#.", "......#.", "......#.", ".#....#.", ".#....#.", "..####..", E } },
    { 'K', { E, ".#...#..", ".#..#...", ".###....", ".#..#...", ".#...#..", ".#....#.", E } },
    { 'L', { E, ".#......", ".#......", ".#......", ".#......", ".#......", ".######.", E } },
    { 'M', { E, ".#....#.", ".##..##.", ".#.##.#.", ".#....#.", ".#....#.", ".#....#.", E } },
    { 'N', { E, ".#....#.", ".##...#.", ".#.#..#.", ".#..#.#.", ".#...##.", ".#....#.", E } },
    { 'O', { E, "..####..", ".#....#.", ".#....#.", ".#....#.", ".#....#.", "..####..", E } },
    { 'P', { E, ".#####..", ".#....#.", ".#....#.", ".#####..", ".#......", ".#......", E } },
    { 'Q', { E, "..####..", ".#....#.", ".#....#.", ".#.#..#.", ".#..#.#.", "..####..", E } },
    { 'R', { E, ".#####..", ".#....#.", ".#....#.", ".#####..", ".#...#..", ".#....#.", E } },
    { 'S', { E, "..####..", ".#......", "..####..", "......#.", ".#....#.", "..####..", E } },
    { 'T', { E, "#######.", "...#....", "...#....", "...#....", "...#....", "...#....", E } },
    { 'U', { E, ".#....#.", ".#....#.", ".#....#.", ".#....#.", ".#....#.", "..####..", E } },
    { 'V', { E, ".#....#.", ".#....#.", ".#....#.", ".#....#.", "..#..#..", "...##...", E } },
    { 'W', { E, ".#....#.", ".#....#.", ".#....#.", ".#....#.", ".#.##.#.", "..#..#..", E } },
    { 'X', { E, ".#....#.", "..#..#..", "...##...", "...##...", "..#..#..", ".#....#.", E } },
    { 'Y', { E, "#.....#.", ".#...#..", "..#.#...", "...#....", "...#....", "...#....", E } },
    { 'Z', { E, ".######.", ".....#..", "....#...", "...#....", "..#.....", ".######.", E } },
    { '0', { E, "..####..", ".#...##.", ".#..#.#.", ".#.#..#.", ".##...#.", "..####..", E } },
    { '1', { E, "...##...", "..#.#...", "....#...", "....#...", "....#...", "..#####.", E } },
    { '2', { E, "..####..", ".#....#.", "......#.", "..####..", ".#......", ".######.", E } },
    { '3', { E, "..####..", ".#....#.", "....##..", "......#.", ".#....#.", "..####..", E } },
    { '4', { E, "....#...", "...##...", "..#.#...", ".#..#...", ".######.", "....#...", E } },
    { '5', { E, ".######.", ".#......", ".#####..", "......#.", ".#....#.", "..####..", E } },
    { '6', { E, "..####..", ".#......", ".#####..", ".#....#.", ".#....#.", "..####..", E } },
    { '7', { E, ".######.", "......#.", ".....#..", "....#...", "...#....", "...#....", E } },
    { '8', { E, "..####..", ".#....#.", "..####..", ".#....#.", ".#....#.", "..####..", E } },
    { '9', { E, "..####..", ".#....#.", ".#....#.", "..#####.", "......#.", "..####..", E } },
    { '!', { E, "...#....", "...#....", "...#....", "...#....", E, "...#....", E } },
    { '.', { E, E, E, E, E, "...##...", "...##...", E } },
    { ',', { E, E, E, E, E, "...#....", "...#....", "..#....." } },
    { '\'', { E, "...#....", "...#....", "..#.....", E, E, E, E } },
    { '"', { E, "..#.#...", "..#.#...", E, E, E, E, E } },
    { '-', { E, E, E, "..####..", E, E, E, E } },
    { ':', { E, E, "...#....", E, E, "...#....", E, E } },
    { '?', { E, "..####..", ".#....#.", ".....#..", "....#...", E, "....#...", E } },
    { '(', { E, "....#...", "...#....", "...#....", "...#....", "...#....", "....#...", E } },
    { ')', { E, "..#.....", "...#....", "...#....", "...#....", "...#....", "..#.....", E } },
    { '/', { E, "......#.", ".....#..", "....#...", "...#....", "..#.....", E, E } },
    { '<', { E, "....#...", "...#....", "..#.....", "...#....", "....#...", E, E } },
    { '>', { E, "..#.....", "...#....", "....#...", "...#....", "..#.....", E, E } },
};
#undef E

static u8 font[96][8];

static void font_init(void)
{
    unsigned i;
    int r, c;
    for (i = 0; i < sizeof(GLYPHS) / sizeof(GLYPHS[0]); i++) {
        const Glyph *g = &GLYPHS[i];
        for (r = 0; r < 8; r++) {
            u8 bits = 0;
            for (c = 0; c < 8; c++)
                if (g->rows[r][c] == '#') bits |= (u8)(0x80 >> c);
            font[g->ch - 32][r] = bits;
        }
    }
}

static void draw_char(int x, int y, char ch, int c, int scale)
{
    int r, col, i, j;
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
    if (ch < 32 || ch > 127) return;
    for (r = 0; r < 8; r++)
        for (col = 0; col < 8; col++)
            if (font[ch - 32][r] & (0x80 >> col))
                for (j = 0; j < scale; j++)
                    for (i = 0; i < scale; i++)
                        pset(x + col * scale + i, y + r * scale + j, c);
}

static int str_len(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void draw_text(int x, int y, const char *s, int c)
{
    for (; *s; s++, x += 8) draw_char(x, y, *s, c, 1);
}

static void draw_text_c(int y, const char *s, int c, int scale)
{
    int x = (SW - str_len(s) * 8 * scale) / 2;
    for (; *s; s++, x += 8 * scale) draw_char(x, y, *s, c, scale);
}

/* Writes v as zero-padded decimal into out, returns pointer past it. */
static char *fmt_num(char *out, u32 v, int digits)
{
    int i;
    for (i = digits - 1; i >= 0; i--) { out[i] = (char)('0' + v % 10); v /= 10; }
    out[digits] = 0;
    return out + digits;
}

static char *str_put(char *out, const char *s)
{
    while (*s) *out++ = *s++;
    *out = 0;
    return out;
}

/* ---------------------------------------------------------------- sprites */

static const char *const SPR_STAND[16] = {
    "................", "......####......", "......####......", "......##.#......",
    "......####......", ".......##.......", ".....######.....", "....########....",
    "...##.####.##...", "...#..####..#...", "......####......", "......#..#......",
    "......#..#......", "......#..#......", "......#..#......", ".....##..##.....",
};
static const char *const SPR_WALK1[16] = {
    "................", "......####......", "......####......", "......##.#......",
    "......####......", ".......##.......", ".....######.....", "....########....",
    "...##.####.##...", "...#..####..#...", "......####......", ".....#....#.....",
    "....#......#....", "....#......#....", "...#........#...", "..##........##..",
};
static const char *const SPR_WALK2[16] = {
    "................", "......####......", "......####......", "......##.#......",
    "......####......", ".......##.......", "......####......", ".....######.....",
    ".....#.##.##....", ".....#.##..#....", "......####......", ".......##.......",
    ".......##.......", "......#.#.......", "......#..#......", ".....##..##.....",
};
static const char *const SPR_JUMP[16] = {
    "...#........#...", "...#..####..#...", "....#.####.#....", "....#.##.#.#....",
    ".....######.....", ".......##.......", "......####......", "......####......",
    "......####......", "......####......", "......####......", ".....#....#.....",
    "....#......#....", "....#......#....", ".....#....#.....", "....##....##....",
};
static const char *const SPR_STUN[16] = {
    "................", "................", "................", "................",
    "................", "................", "......####......", "......####......",
    "......####......", ".......##.......", "....########....", "...#..####..#...",
    "......####......", "......#######...", "......#.....#...", ".....##.....##..",
};
static const char *const SPR_LIFE[8] = {
    "..##....", "..##....", ".####...", "#.##.#..",
    "..##....", ".#..#...", ".#..#...", "##..##..",
};

/* Hazards all face left (they move left). */
static const char *const HZ_SNAKE[16] = {
    "................", "................", "................", "................",
    "................", "................", "................", "..###...........",
    ".#####..........", ".#.###..........", ".#####..........", "#..####.....##..",
    "....####...####.", ".....########.##", "......######...#", "................",
};
static const char *const HZ_GHOST[16] = {
    "................", ".....######.....", "....########....", "...##########...",
    "...#..###..###..", "...#..###..###..", "...##########...", "...##########...",
    "...###....####..", "...####..#####..", "...##########...", "...##########...",
    "...##########...", "...##########...", "...##.###.###...", "...#...#...#....",
};
static const char *const HZ_PLANE[16] = {
    "................", "................", "................", "................",
    "..............##", ".....##......###", "....###.....####", ".##############.",
    "################", ".##############.", "....###.........", ".....##.........",
    "................", "................", "................", "................",
};
static const char *const HZ_BUS[16] = {
    "................", "................", "................", "................",
    "................", "...#########....", "..##.##.##.##...", ".###.##.##.##...",
    ".##############.", "################", "################", "################",
    "..##......##....", ".####....####...", ".####....####...", "..##......##....",
};
static const char *const HZ_SPIDER[16] = {
    "................", "................", "................", "................",
    "................", "................", "................", ".....######.....",
    "...##########...", "..###.####.###..", "..############..", ".#.#.#.##.#.#.#.",
    "#.#.#......#.#.#", "#..#........#..#", "#..#........#..#", "................",
};

static const char *const *const HAZARDS[] = { HZ_SNAKE, HZ_GHOST, HZ_PLANE, HZ_BUS, HZ_SPIDER };
static const int HZ_COLOR[] = { BGREEN, BMAGENTA, BCYAN, BRED, BYELLOW };
#define NHZTYPES 5

static const int LINE_COLORS[] = { BRED, BYELLOW, BGREEN, BCYAN, BMAGENTA, BWHITE };

/* ---------------------------------------------------------------- limericks */

static const char *const LIMERICKS[][5] = {
    { "A JUMPER CALLED JACK", "LIKED HOLES IN THE TRACK,", "HE LEAPT THROUGH THE GAP,",
      "GAVE HIS HEAD NOT A TAP,", "AND LANDED UP HIGH ON HIS BACK." },
    { "THE LINES MOVED AROUND,", "BUT JACK'S AIM WAS SOUND,", "HE SPRANG TO THE TOP",
      "WITHOUT ONE SINGLE STOP,", "AND NEVER ONCE HIT THE GROUND." },
    { "A SNAKE ON THE FLOOR", "GAVE JACK SUCH A ROAR", "THAT HE SAT FOR A WHILE",
      "WITH A DAZED LITTLE SMILE,", "THEN LEAPT TO THE TOP ONCE MORE." },
    { "WHEN THE GAPS START TO RACE,", "KEEP A SMILE ON YOUR FACE,", "FOR A JUMP IN THE DARK",
      "CAN BE QUITE A LARK,", "IF YOU LAND IN THE PROPER PLACE." },
    { "THERE ONCE WAS A BUS", "THAT MADE QUITE A FUSS,", "IT RAN OVER JACK'S TOE,",
      "SO HE SHOUTED \"OH NO!\"", "AND LEAPT UP HIGH WITHOUT US." },
    { "THE HIGHER HE WENT,", "THE MORE HE WAS BENT", "ON REACHING THE SKY",
      "WITH A HOP AND A CRY,", "NOW ALL OF HIS ENERGY'S SPENT." },
    { "A GHOST AND AN AXE", "TRIED TO FOLLOW JACK'S TRACKS,", "BUT HE FLEW THROUGH THE HOLE",
      "LIKE A HIGH-JUMPING MOLE,", "SO NOW HE CAN SIT AND RELAX." },
    { "HE'S REACHED THE TOP FLOOR", "AND HE'S ASKING FOR MORE,", "SO GRAB HOLD OF YOUR KEYS",
      "AND JUMP HIGH AS YOU PLEASE,", "MORE PERIL IS WAITING IN STORE." },
};
#define NLIMERICKS 8

/* ---------------------------------------------------------------- gaps */

static int gap_segs(const Gap *g, Seg out[2])
{
    int n = 0, s = g->x, e = g->x + GAPW;
    int ms = s < 0 ? 0 : s, me = e > SW ? SW : e;
    if (me > ms) { out[n].line = g->line; out[n].a = ms; out[n].b = me; n++; }
    if (g->dir > 0 && e > SW) { out[n].line = line_down(g->line); out[n].a = 0; out[n].b = e - SW; n++; }
    if (g->dir < 0 && s < 0) { out[n].line = line_up(g->line); out[n].a = SW + s; out[n].b = SW; n++; }
    return n;
}

/* 1 if the span [a,b) on the given line lies entirely inside one gap. */
static int in_gap(int line, int a, int b)
{
    int i, j, n;
    Seg s[2];
    if (a < 0 || b > SW) return 0;
    for (i = 0; i < G.ngaps; i++) {
        n = gap_segs(&G.gaps[i], s);
        for (j = 0; j < n; j++)
            if (s[j].line == line && s[j].a <= a && b <= s[j].b) return 1;
    }
    return 0;
}

static void move_gaps(void)
{
    int i;
    for (i = 0; i < G.ngaps; i++) {
        Gap *g = &G.gaps[i];
        g->x += g->dir * GAP_SPEED;
        if (g->dir > 0 && g->x >= SW) { g->x -= SW; g->line = line_down(g->line); }
        if (g->dir < 0 && g->x + GAPW <= 0) { g->x += SW; g->line = line_up(g->line); }
    }
}

static void add_gap(void)
{
    Gap *g;
    if (G.ngaps >= MAXGAPS) return;
    g = &G.gaps[G.ngaps];
    g->dir = (G.ngaps & 1) ? -1 : 1;
    g->line = 1 + rndn(NFLOORS);
    g->x = rndn(SW - GAPW);
    /* Don't open a gap right under Jack's feet. */
    if (g->line == G.jfloor && g->x < G.jx + 16 && g->x + GAPW > G.jx)
        g->x = (G.jx + SW / 2) % (SW - GAPW);
    G.ngaps++;
}

/* ---------------------------------------------------------------- jack */

static void jack_span(int *a, int *b)
{
    *a = G.jx + HIT_L;
    *b = G.jx + HIT_R;
    if (*a >= SW) { *a -= SW; *b -= SW; }
}

static int jack_over_gap(int line)
{
    int a, b;
    jack_span(&a, &b);
    return in_gap(line, a, b);
}

static int hazard_hits_jack(void)
{
    int a, b, i, k;
    jack_span(&a, &b);
    for (i = 0; i < G.nhz; i++) {
        const Hazard *h = &G.hz[i];
        if (h->floor != G.jfloor) continue;
        for (k = -1; k <= 1; k++) {
            int ha = h->x + 3 + k * SW, hb = h->x + 13 + k * SW;
            if (ha < b && a < hb) return 1;
        }
    }
    return 0;
}

static void spawn_hazards(void)
{
    int i;
    G.nhz = G.level - 1;
    if (G.nhz > MAXHZ) G.nhz = MAXHZ;
    for (i = 0; i < G.nhz; i++) {
        Hazard *h = &G.hz[i];
        h->floor = 1 + rndn(NFLOORS - 1);
        h->x = rndn(SW);
        h->type = i % NHZTYPES;
        h->speed = 3 + rndn(4); /* quarter pixels per frame */
        h->sub = 0;
    }
}

static void move_hazards(void)
{
    int i;
    for (i = 0; i < G.nhz; i++) {
        Hazard *h = &G.hz[i];
        h->sub += h->speed;
        while (h->sub >= 4) { h->sub -= 4; h->x--; }
        if (h->x <= -16) { h->x = SW; h->floor = (h->floor + 1) % NFLOORS; }
    }
}

static void start_level(void)
{
    G.ngaps = 0;
    G.jx = SW / 2 - 8;
    G.jfloor = 0;
    G.jy = 0;
    G.jst = J_STAND;
    G.jtimer = 0;
    G.facing = 1;
    add_gap();
    add_gap();
    spawn_hazards();
    G.state = ST_PLAY;
    G.paused = 0;
}

static void new_game(void)
{
    G.level = 1;
    G.lives = START_LIVES;
    G.score = 0;
    start_level();
    snd_play(S_START);
}

static void add_score(u32 pts)
{
    G.score += pts;
    if (G.score > G.hiscore) G.hiscore = G.score;
}

static void level_complete(void)
{
    G.bonus = 100u * (u32)G.level;
    add_score(G.bonus);
    G.state = ST_LEVELDONE;
    G.timer = 0;
    snd_play(S_LEVEL);
}

static void update_jack(u32 pressed)
{
    int dx;
    switch (G.jst) {
    case J_STAND:
        dx = 0;
        if (G.keys & JJ_KEY_LEFT) dx -= JACK_SPEED;
        if (G.keys & JJ_KEY_RIGHT) dx += JACK_SPEED;
        G.walking = dx != 0;
        if (dx) {
            G.jx = (G.jx + dx + SW) % SW;
            G.facing = dx < 0 ? -1 : 1;
            if ((++G.walkt & 7) == 0) snd_play(S_STEP);
        }
        if (pressed & JJ_KEY_JUMP) {
            if (jack_over_gap(G.jfloor + 1)) {
                G.jst = J_JUMP;
                snd_play(S_JUMP);
            } else {
                G.jst = J_BUMP;
                G.jtimer = 8;
                snd_play(S_BUMP);
            }
        }
        break;
    case J_JUMP:
        G.jy -= JUMP_SPEED;
        if (G.jy <= -FLOOR_H) {
            G.jfloor++;
            G.jy = 0;
            G.jst = J_STAND;
            add_score(5u * (u32)G.level);
            add_gap();
            snd_play(S_LAND);
            if (G.jfloor >= NFLOORS) { level_complete(); return; }
        }
        break;
    case J_BUMP:
        G.jy = G.jtimer > 4 ? -2 : 0;
        if (--G.jtimer <= 0) { G.jy = 0; G.jst = J_STUN; G.jtimer = STUN_BUMP; }
        break;
    case J_FALL:
        G.jy += FALL_SPEED;
        if (G.jy >= FLOOR_H) {
            G.jfloor--;
            G.jy = 0;
            if (G.jfloor == 0) {
                G.lives--;
                G.state = ST_DEAD;
                G.timer = 0;
                G.jst = J_STUN;
                snd_play(S_DIE);
                return;
            }
            G.jst = J_STUN;
            G.jtimer = STUN_FALL;
            snd_play(S_LAND);
        }
        break;
    case J_STUN:
        if (--G.jtimer <= 0) G.jst = J_STAND;
        break;
    }

    if ((G.jst == J_STAND || G.jst == J_STUN) && G.jfloor > 0 && jack_over_gap(G.jfloor)) {
        G.jst = J_FALL;
        G.jy = 0;
        snd_play(S_FALL);
    } else if (G.jst == J_STAND && hazard_hits_jack()) {
        G.jst = J_STUN;
        G.jtimer = STUN_HAZARD;
        snd_play(S_HIT);
    }
}

/* ---------------------------------------------------------------- render */

static void render_status(void)
{
    char buf[40], *p = buf;
    p = str_put(p, "SCORE ");
    p = fmt_num(p, G.score, 6);
    p = str_put(p, "  HI ");
    p = fmt_num(p, G.hiscore, 6);
    p = str_put(p, "  L");
    fmt_num(p, (u32)G.level, 2);
    draw_text(0, 0, buf, BWHITE);
}

static void render_lines(int color_shift)
{
    int k, i, j, n;
    Seg s[2];
    for (k = 1; k <= NFLOORS; k++)
        fill(0, floor_y(k), SW, LINE_T, LINE_COLORS[(k + color_shift) % 6]);
    fill(0, floor_y(0), SW, 4, GREEN);
    for (i = 0; i < G.ngaps; i++) {
        n = gap_segs(&G.gaps[i], s);
        for (j = 0; j < n; j++)
            fill(s[j].a, floor_y(s[j].line), s[j].b - s[j].a, LINE_T, BLACK);
    }
}

static void render_stars(int x, int y)
{
    static const int dx[8] = { 6, 4, 0, -4, -6, -4, 0, 4 };
    static const int dy[8] = { 0, -2, -3, -2, 0, 2, 3, 2 };
    int k;
    for (k = 0; k < 3; k++) {
        int i = (int)((G.frame / 3 + (u32)k * 3) & 7);
        int sx = x + dx[i], sy = y + dy[i];
        pset(sx, sy, BYELLOW);
        pset(sx - 1, sy, BYELLOW);
        pset(sx + 1, sy, BYELLOW);
        pset(sx, sy - 1, BYELLOW);
        pset(sx, sy + 1, BYELLOW);
    }
}

static void render_jack(void)
{
    const char *const *spr = SPR_STAND;
    int y = floor_y(G.jfloor) - 16 + G.jy;
    int flip = G.facing < 0;

    switch (G.jst) {
    case J_STAND:
        if (G.walking) spr = (G.walkt & 8) ? SPR_WALK1 : SPR_WALK2;
        break;
    case J_JUMP:
    case J_FALL:
        spr = SPR_JUMP;
        break;
    case J_BUMP:
    case J_STUN:
        spr = SPR_STUN;
        break;
    }
    if (G.state == ST_DEAD && (G.timer & 4)) return;
    draw_sprite(spr, 16, 16, G.jx, y, BWHITE, flip, 1);
    if (spr == SPR_STUN && G.state != ST_DEAD)
        render_stars((G.jx + 8) % SW, y + 3);
}

static void render_world(void)
{
    int i;
    clear_screen(G.state == ST_DEAD ? ((G.timer >> 2) & 1 ? RED : BLACK) : BLACK, BLACK);
    render_status();
    render_lines(G.level - 1);
    for (i = 0; i < G.nhz; i++) {
        const Hazard *h = &G.hz[i];
        draw_sprite(HAZARDS[h->type], 16, 16, h->x, floor_y(h->floor) - 16,
                    HZ_COLOR[h->type], 0, 0);
    }
    render_jack();
    for (i = 0; i < G.lives; i++)
        draw_sprite(SPR_LIFE, 8, 8, i * 10, 184, BCYAN, 0, 0);
}

static void render_box(const char *msg, int ink)
{
    int w = str_len(msg) * 8 + 16;
    int x = (SW - w) / 2;
    fill(x, 84, w, 24, BLACK);
    fill(x, 84, w, 1, ink);
    fill(x, 107, w, 1, ink);
    fill(x, 84, 1, 24, ink);
    fill(x + w - 1, 84, 1, 24, ink);
    draw_text_c(92, msg, ink, 1);
}

static void render_title(void)
{
    char buf[32];
    int i, x, jx;

    clear_screen(BLUE, BLACK);
    draw_text_c(12, "JUMPING JACK", BYELLOW, 2);
    draw_text_c(34, "A TRIBUTE TO THE 1983 CLASSIC", CYAN, 1);

    /* Demo line with travelling gaps. */
    fill(0, 52, SW, LINE_T, BRED);
    for (i = 0; i < 3; i++)
        for (x = 0; x < GAPW; x++)
            fill((G.tgap[i] + x) % SW, 52, 1, LINE_T, BLACK);

    draw_text_c(68, "LEFT   Z  OR  <", BWHITE, 1);
    draw_text_c(80, "RIGHT  X  OR  >", BWHITE, 1);
    draw_text_c(92, "JUMP   SPACE OR UP", BWHITE, 1);
    draw_text_c(104, "PAUSE  H  OR  ESC", BWHITE, 1);
    draw_text_c(122, "JUMP THROUGH THE GAPS TO", GREEN, 1);
    draw_text_c(132, "REACH THE TOP. MIND THE", GREEN, 1);
    draw_text_c(142, "HOLES UNDER YOUR FEET!", GREEN, 1);

    fmt_num(str_put(buf, "HI-SCORE "), G.hiscore, 6);
    draw_text_c(152, buf, BCYAN, 1);
    if ((G.frame / 20) & 1) draw_text_c(162, "PRESS SPACE OR TAP TO START", BYELLOW, 1);

    fill(0, 188, SW, 4, GREEN);
    jx = (int)(G.frame % (2 * (SW + 16)));
    if (jx < SW + 16)
        draw_sprite((G.frame & 8) ? SPR_WALK1 : SPR_WALK2, 16, 16, jx - 16, 172, BWHITE, 0, 0);
    else
        draw_sprite((G.frame & 8) ? SPR_WALK1 : SPR_WALK2, 16, 16, 2 * (SW + 16) - jx - 16, 172,
                    BWHITE, 1, 0);
}

static void render_level_done(void)
{
    char buf[40], *p;
    const char *const *lim = LIMERICKS[(G.level - 1) % NLIMERICKS];
    int i;

    clear_screen(MAGENTA, BLACK);
    p = str_put(buf, "LEVEL ");
    p = fmt_num(p, (u32)G.level, 2);
    str_put(p, " COMPLETE!");
    draw_text_c(16, buf, BYELLOW, 1);
    p = str_put(buf, "BONUS ");
    fmt_num(p, G.bonus, 5);
    draw_text_c(32, buf, BWHITE, 1);

    for (i = 0; i < 5; i++)
        draw_text_c(64 + i * 14, lim[i], i == 0 ? BCYAN : CYAN, 1);

    p = str_put(buf, "NEXT LEVEL: ");
    p = fmt_num(p, (u32)(G.level < MAXHZ ? G.level : MAXHZ), 2);
    str_put(p, " HAZARDS");
    draw_text_c(152, buf, BGREEN, 1);
    if (G.timer > 50 && ((G.frame / 20) & 1)) draw_text_c(172, "PRESS SPACE OR TAP", BYELLOW, 1);
}

static void render_game_over(void)
{
    char buf[40], *p;
    clear_screen(RED, BLACK);
    draw_text_c(60, "GAME OVER", BRED, 2);
    p = str_put(buf, "SCORE ");
    fmt_num(p, G.score, 6);
    draw_text_c(96, buf, BWHITE, 1);
    p = str_put(buf, "LEVEL ");
    fmt_num(p, (u32)G.level, 2);
    draw_text_c(110, buf, BWHITE, 1);
    if (G.score > 0 && G.score >= G.hiscore) draw_text_c(130, "NEW HIGH SCORE!", BYELLOW, 1);
    if (G.timer > 60 && ((G.frame / 20) & 1)) draw_text_c(160, "PRESS SPACE OR TAP", BYELLOW, 1);
}

/* ---------------------------------------------------------------- api */

void jj_init(unsigned int seed)
{
    u32 hi = G.hiscore;
    font_init();
    G = (Game){ 0 };
    G.hiscore = hi;
    G.rng = seed ? seed : 0x2545F491u;
    G.state = ST_TITLE;
    G.level = 1;
    G.tgap[0] = 0;
    G.tgap[1] = 90;
    G.tgap[2] = 180;
    snd_clear();
    render_title();
}

void jj_set_sample_rate(int rate)
{
    if (rate >= 8000 && rate <= 192000) srate = rate;
}

void jj_tick(unsigned int keys)
{
    u32 pressed = keys & ~G.prev_keys;
    u32 go = pressed & (JJ_KEY_JUMP | JJ_KEY_START);
    int i;

    G.keys = keys;
    G.frame++;

    switch (G.state) {
    case ST_TITLE:
        for (i = 0; i < 3; i++) G.tgap[i] = (G.tgap[i] + 1) % SW;
        if (go) new_game();
        break;
    case ST_PLAY:
        if (pressed & JJ_KEY_PAUSE) G.paused = !G.paused;
        if (G.paused) break;
        move_gaps();
        move_hazards();
        update_jack(pressed);
        break;
    case ST_DEAD:
        if (++G.timer >= 100) {
            if (G.lives <= 0) {
                G.state = ST_GAMEOVER;
                G.timer = 0;
                snd_play(S_OVER);
            } else {
                G.state = ST_PLAY;
                G.jst = J_STAND;
            }
        }
        break;
    case ST_LEVELDONE:
        G.timer++;
        if ((G.timer > 50 && go) || G.timer > 600) {
            G.level++;
            start_level();
            snd_play(S_START);
        }
        break;
    case ST_GAMEOVER:
        G.timer++;
        if ((G.timer > 60 && go) || G.timer > 750) {
            G.state = ST_TITLE;
            G.frame = 0;
        }
        break;
    }

    switch (G.state) {
    case ST_TITLE: render_title(); break;
    case ST_LEVELDONE: render_level_done(); break;
    case ST_GAMEOVER: render_game_over(); break;
    default:
        render_world();
        if (G.paused) render_box("PAUSED", BYELLOW);
        else if (G.state == ST_DEAD) render_box("OUCH! YOU LOST A LIFE", BRED);
        break;
    }

    audio_frame();
    G.prev_keys = keys;
}

const unsigned int *jj_framebuffer(void) { return fb; }
const float *jj_audio(void) { return abuf; }
int jj_audio_len(void) { return alen; }
unsigned int jj_hiscore(void) { return G.hiscore; }
void jj_set_hiscore(unsigned int score)
{
    if (score > G.hiscore) G.hiscore = score;
}
