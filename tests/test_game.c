/* Unit tests for the game core logic. Includes game.c to reach statics. */
#include <stdio.h>

#include "../src/game.c"

static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static void setup_play(void)
{
    jj_init(1234);
    jj_tick(JJ_KEY_START);
    jj_tick(0);
    G.ngaps = 0;
    G.nhz = 0;
}

static void run(int frames, unsigned keys)
{
    while (frames-- > 0) jj_tick(keys);
}

static void test_start_game(void)
{
    jj_init(1);
    CHECK(G.state == ST_TITLE);
    jj_tick(JJ_KEY_JUMP);
    CHECK(G.state == ST_PLAY);
    CHECK(G.lives == START_LIVES);
    CHECK(G.ngaps == 2);
    CHECK(G.nhz == 0);
}

static void test_gap_wraps_right_to_line_below(void)
{
    Gap g = { 3, SW - 1, 1 };
    G.ngaps = 1;
    G.gaps[0] = g;
    move_gaps();
    CHECK(G.gaps[0].line == 2 && G.gaps[0].x == 0);
    G.gaps[0].line = 1;
    G.gaps[0].x = SW - 1;
    move_gaps();
    CHECK(G.gaps[0].line == NFLOORS);
}

static void test_gap_wraps_left_to_line_above(void)
{
    Gap g = { 8, -GAPW + 1, -1 };
    G.ngaps = 1;
    G.gaps[0] = g;
    move_gaps();
    CHECK(G.gaps[0].line == 1 && G.gaps[0].x == SW - GAPW);
}

static void test_gap_segments_split_across_lines(void)
{
    Seg s[2];
    Gap g = { 5, SW - 10, 1 };
    int n = gap_segs(&g, s);
    CHECK(n == 2);
    CHECK(s[0].line == 5 && s[0].a == SW - 10 && s[0].b == SW);
    CHECK(s[1].line == 4 && s[1].a == 0 && s[1].b == GAPW - 10);
}

static void test_jump_through_gap(void)
{
    setup_play();
    G.jx = 100;
    G.gaps[0].line = 1;
    G.gaps[0].x = 100;
    G.gaps[0].dir = 1;
    G.ngaps = 1;
    jj_tick(JJ_KEY_JUMP);
    CHECK(G.jst == J_JUMP);
    run(FLOOR_H / JUMP_SPEED + 1, 0);
    CHECK(G.jfloor == 1);
    CHECK(G.ngaps == 2);
    CHECK(G.score == 5);
}

static void test_bump_without_gap(void)
{
    setup_play();
    G.jx = 100;
    jj_tick(JJ_KEY_JUMP);
    CHECK(G.jst == J_BUMP);
    run(10, 0);
    CHECK(G.jst == J_STUN);
    CHECK(G.jfloor == 0);
    run(STUN_BUMP, 0);
    CHECK(G.jst == J_STAND);
}

static void test_fall_through_gap_and_lose_life(void)
{
    setup_play();
    G.jfloor = 1;
    G.jx = 100;
    G.gaps[0].line = 1;
    G.gaps[0].x = 98;
    G.gaps[0].dir = 1;
    G.ngaps = 1;
    jj_tick(0);
    CHECK(G.jst == J_FALL);
    run(FLOOR_H / FALL_SPEED + 1, 0);
    CHECK(G.jfloor == 0);
    CHECK(G.state == ST_DEAD);
    CHECK(G.lives == START_LIVES - 1);
    run(100, 0);
    CHECK(G.state == ST_PLAY);
}

static void test_fall_from_higher_floor_stuns(void)
{
    setup_play();
    G.jfloor = 4;
    G.jx = 100;
    G.gaps[0].line = 4;
    G.gaps[0].x = 98;
    G.gaps[0].dir = 1;
    G.ngaps = 1;
    run(FLOOR_H / FALL_SPEED + 2, 0);
    CHECK(G.jfloor == 3);
    CHECK(G.jst == J_STUN);
    CHECK(G.lives == START_LIVES);
}

static void test_hazard_stuns(void)
{
    setup_play();
    G.jx = 100;
    G.nhz = 1;
    G.hz[0].floor = 0;
    G.hz[0].x = 102;
    G.hz[0].type = 0;
    G.hz[0].speed = 0;
    jj_tick(0);
    CHECK(G.jst == J_STUN);
}

static void test_reaching_top_completes_level(void)
{
    setup_play();
    G.jfloor = NFLOORS - 1;
    G.jx = 100;
    G.gaps[0].line = NFLOORS;
    G.gaps[0].x = 100;
    G.gaps[0].dir = 1;
    G.ngaps = 1;
    jj_tick(JJ_KEY_JUMP);
    run(FLOOR_H / JUMP_SPEED + 1, 0);
    CHECK(G.state == ST_LEVELDONE);
    run(60, 0);
    jj_tick(JJ_KEY_JUMP);
    CHECK(G.state == ST_PLAY);
    CHECK(G.level == 2);
    CHECK(G.nhz == 1);
}

static void test_game_over(void)
{
    setup_play();
    G.lives = 1;
    G.jfloor = 1;
    G.jx = 100;
    G.gaps[0].line = 1;
    G.gaps[0].x = 98;
    G.gaps[0].dir = 1;
    G.ngaps = 1;
    run(120, 0);
    CHECK(G.state == ST_GAMEOVER);
}

static void test_audio_length(void)
{
    jj_init(1);
    jj_set_sample_rate(48000);
    jj_tick(0);
    CHECK(jj_audio_len() == 960);
    jj_set_sample_rate(44100);
    jj_tick(0);
    CHECK(jj_audio_len() == 882);
}

int main(void)
{
    test_start_game();
    test_gap_wraps_right_to_line_below();
    test_gap_wraps_left_to_line_above();
    test_gap_segments_split_across_lines();
    test_jump_through_gap();
    test_bump_without_gap();
    test_fall_through_gap_and_lose_life();
    test_fall_from_higher_floor_stuns();
    test_hazard_stuns();
    test_reaching_top_completes_level();
    test_game_over();
    test_audio_length();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
