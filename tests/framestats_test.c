#include "../include/framestats.h"
#include <stdio.h>

#define PASS(name)         printf("PASS: %s\n", (name))
#define FAIL(name)         do { printf("FAIL: %s\n", (name)); return 1; } while (0)
#define ASSERT(name, cond) do { if (!(cond)) FAIL(name); } while (0)

/* Feed `n` frames of constant dt and return the last FPS result. */
static int feed_constant(FrameStats *fs, double dt, int n) {
    int fps = 0;
    for (int i = 0; i < n; i++)
        fps = frame_stats_update(fs, dt);
    return fps;
}

static int test_constant_60fps(void) {
    FrameStats fs;
    frame_stats_init(&fs);

    /* 60 Hz → dt ≈ 0.01667 s; EMA converges to steady state immediately
       for constant input, so even 1 frame gives the right answer, but we
       feed several to confirm stability. */
    double dt60 = 1.0 / 60.0;
    int fps = feed_constant(&fs, dt60, 30);

    /* rounded result must be exactly 60 for constant 60 Hz input */
    ASSERT("constant 60 Hz gives 60 FPS", fps == 60);

    PASS("constant_60fps");
    return 0;
}

static int test_constant_30fps(void) {
    FrameStats fs;
    frame_stats_init(&fs);

    double dt30 = 1.0 / 30.0;
    int fps = feed_constant(&fs, dt30, 30);

    ASSERT("constant 30 Hz gives 30 FPS", fps == 30);

    PASS("constant_30fps");
    return 0;
}

static int test_result_positive(void) {
    FrameStats fs;
    frame_stats_init(&fs);

    /* any positive dt must yield a positive FPS */
    int fps = frame_stats_update(&fs, 0.016);
    ASSERT("positive dt gives positive FPS", fps > 0);

    PASS("result_positive");
    return 0;
}

static int test_smoothing(void) {
    FrameStats fs;
    frame_stats_init(&fs);

    /* Alternate between a fast frame (10 ms) and a slow frame (30 ms).
       The true average period is 20 ms → 50 FPS.  After many pairs the
       EMA should settle near 50 FPS.  We allow a generous band [35,70]
       so the test is robust to the chosen alpha without being too loose
       to catch a broken implementation that just echoes the last frame. */
    for (int i = 0; i < 60; i++) {
        frame_stats_update(&fs, 0.010);
        frame_stats_update(&fs, 0.030);
    }
    int fps = frame_stats_update(&fs, 0.010); /* one extra to read */

    ASSERT("jitter smoothed into sane band [35,70]", fps >= 35 && fps <= 70);

    /* confirm it doesn't just echo the last raw frame (which would be 100) */
    ASSERT("smoothed result is not the raw last-frame rate", fps != 100);

    PASS("smoothing");
    return 0;
}

static int test_center_offset_normal(void) {
    /* (400 - 200) / 2 == 100 */
    float off = center_offset(400.0f, 200.0f);
    float diff = off - 100.0f;
    if (diff < 0.0f) diff = -diff;
    ASSERT("center_offset(400, 200) == 100", diff < 0.001f);

    /* content == avail → offset == 0 */
    off = center_offset(100.0f, 100.0f);
    ASSERT("center_offset(100, 100) == 0", off == 0.0f);

    PASS("center_offset_normal");
    return 0;
}

static int test_center_offset_clamp(void) {
    /* content > avail → clamped to 0 */
    float off = center_offset(100.0f, 200.0f);
    ASSERT("center_offset clamps to 0 when content > avail", off == 0.0f);

    off = center_offset(0.0f, 50.0f);
    ASSERT("center_offset(0, 50) == 0", off == 0.0f);

    PASS("center_offset_clamp");
    return 0;
}

static int test_reinit_resets_state(void) {
    FrameStats fs;
    frame_stats_init(&fs);

    /* prime with a fast rate */
    feed_constant(&fs, 1.0 / 120.0, 20);

    /* re-init and immediately feed a slow rate; should not be influenced
       by the prior run */
    frame_stats_init(&fs);
    int fps = frame_stats_update(&fs, 1.0 / 30.0);

    ASSERT("re-init discards prior state; first frame reads 30 FPS", fps == 30);

    PASS("reinit_resets_state");
    return 0;
}

int main(void) {
    int failures = 0;
    failures += test_constant_60fps();
    failures += test_constant_30fps();
    failures += test_result_positive();
    failures += test_smoothing();
    failures += test_center_offset_normal();
    failures += test_center_offset_clamp();
    failures += test_reinit_resets_state();

    if (failures == 0) {
        printf("All framestats tests passed.\n");
        return 0;
    }
    printf("%d framestats test(s) failed.\n", failures);
    return 1;
}
