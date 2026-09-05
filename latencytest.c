/*
 * latencytest — characterise the SoundTouch DMA output path.
 *
 * Writes silence (inaudible) with wall-clock pacing DISABLED, so pacing comes
 * only from write() blocking on DMA back-pressure. That reveals:
 *   1. Driver buffer depth: how many chunks are absorbed instantly at startup
 *      before writes begin to block. This is the constant latency between
 *      handing a frame to the driver and it being audible.
 *   2. True hardware rate: the steady-state frames/second once back-pressure
 *      dominates, i.e. the actual DAC crystal rate vs the nominal 48000.
 */
#include "shelbourne.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int main(void) {
    shelbourne_stop_stock_processes();
    shelbourne_t *hw = shelbourne_init();
    if (!hw) { fprintf(stderr, "latencytest: init FAILED\n"); return 1; }

    shelbourne_audio_set_pacing(0);   /* pure DMA back-pressure pacing */
    shelbourne_audio_reset(hw);

    static int32_t silence[SHELBOURNE_FRAMES_PER_WRITE * 2];
    memset(silence, 0, sizeof(silence));

    /* Phase A: startup fill — how many chunks are absorbed before blocking? */
    printf("\n=== Phase A: startup fill (per-chunk write time, us) ===\n");
    int absorbed = 0;
    int64_t t_start = now_us();
    for (int i = 0; i < 40; i++) {
        int64_t t0 = now_us();
        shelbourne_audio_write(hw, silence, SHELBOURNE_FRAMES_PER_WRITE);
        shelbourne_audio_wait(hw);
        int64_t dt = now_us() - t0;
        printf("  chunk %2d: %6lld us%s\n", i, (long long)dt,
               dt < 3000 ? "   <- absorbed (buffered)" : "");
        if (dt < 3000) absorbed++;
    }
    int64_t fill_us = now_us() - t_start;
    printf("  absorbed-immediately chunks: %d  (~%d frames, ~%.1f ms of buffer)\n",
           absorbed, absorbed * SHELBOURNE_FRAMES_PER_WRITE,
           absorbed * SHELBOURNE_FRAMES_PER_WRITE * 1000.0 / SHELBOURNE_SAMPLE_RATE);
    printf("  (40 chunks took %lld us total)\n", (long long)fill_us);

    /* Phase B: steady state — measure true hardware rate over ~10s */
    printf("\n=== Phase B: steady-state rate (back-pressure paced) ===\n");
    const int N = 900;                       /* 900 * 512 / 48000 ~= 9.6 s */
    int64_t t1 = now_us();
    for (int i = 0; i < N; i++) {
        shelbourne_audio_write(hw, silence, SHELBOURNE_FRAMES_PER_WRITE);
        shelbourne_audio_wait(hw);
    }
    int64_t elapsed = now_us() - t1;
    double frames = (double)N * SHELBOURNE_FRAMES_PER_WRITE;
    double rate = frames * 1000000.0 / (double)elapsed;
    printf("  %d chunks (%.0f frames) in %lld us\n", N, frames, (long long)elapsed);
    printf("  measured hardware rate: %.2f Hz (nominal %d)\n", rate, SHELBOURNE_SAMPLE_RATE);
    printf("  deviation from nominal: %+.1f ppm\n",
           (rate - SHELBOURNE_SAMPLE_RATE) / SHELBOURNE_SAMPLE_RATE * 1e6);

    shelbourne_audio_drain(hw);
    shelbourne_shutdown(hw);
    return 0;
}
