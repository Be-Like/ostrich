#ifndef FRAMESTATS_H
#define FRAMESTATS_H

typedef struct {
    double accum;  /* exponentially smoothed frame time, seconds */
    int    seeded; /* 0 until the first update seeds accum */
} FrameStats;

void  frame_stats_init(FrameStats *fs);
int   frame_stats_update(FrameStats *fs, double dt_seconds);
float center_offset(float avail, float content);

#endif /* FRAMESTATS_H */
