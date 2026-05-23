#include "framestats.h"

#define EMA_ALPHA 0.1

void frame_stats_init(FrameStats *fs) {
    fs->accum  = 0.0;
    fs->seeded = 0;
}

int frame_stats_update(FrameStats *fs, double dt_seconds) {
    if (!fs->seeded) {
        fs->accum  = dt_seconds;
        fs->seeded = 1;
    } else {
        fs->accum = EMA_ALPHA * dt_seconds + (1.0 - EMA_ALPHA) * fs->accum;
    }
    if (fs->accum <= 0.0) return 0;
    return (int)(1.0 / fs->accum + 0.5);
}

float center_offset(float avail, float content) {
    float offset = (avail - content) * 0.5f;
    return offset > 0.0f ? offset : 0.0f;
}
