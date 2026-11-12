/* XBA-29: the frame-time log's numbers. */
#include "frame_times.h"
#include <math.h>
#include <stdio.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
#define MS 1000000ull

int main(void)
{
    FrameTimes f = { 0 };
    uint32_t n;
    double mean, sd, worst;
    uint64_t t = 1000 * MS;

    /* 60 fps exactly for 10 s: mean 16.67, sd 0. */
    for (int i = 0; i <= 600; i++, t += 16666667ull)
        frame_times_add(&f, t);
    CHECK(frame_times_take(&f, t, 10000 * MS, &n, &mean, &sd, &worst));
    CHECK(n == 600 && fabs(mean - 16.667) < 0.01 && sd < 0.01 && fabs(worst - 16.667) < 0.01);
    CHECK(!frame_times_take(&f, t, 10000 * MS, &n, &mean, &sd, &worst));   /* a new period started */

    /* 30 fps with every other frame 3 ms late: sd 1.5 ms, worst 34.8. */
    t = f.last_ns;
    for (int i = 0; i < 300; i++) {
        t += (i & 1) ? 31833333ull : 34833333ull;
        frame_times_add(&f, t);
    }
    CHECK(frame_times_take(&f, t, 9000 * MS, &n, &mean, &sd, &worst));
    CHECK(fabs(mean - 33.333) < 0.01 && fabs(sd - 1.5) < 0.01 && fabs(worst - 34.833) < 0.01);

    /* A 2 s pause is not a frame time. */
    frame_times_add(&f, t + 2000 * MS);
    frame_times_add(&f, t + 2000 * MS + 16 * MS);
    CHECK(frame_times_take(&f, t + 12000 * MS, 10000 * MS, &n, &mean, &sd, &worst));
    CHECK(n == 1 && fabs(mean - 16.0) < 0.01 && worst < 17);

    if (failures)
        return 1;
    puts("frame_times: ok");
    return 0;
}
