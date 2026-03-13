/*
 * shelbourne-test.c — Automated hardware test suite
 *
 * Runs on the device, tests all hardware subsystems, and prints
 * PASS/FAIL results. Requires no user interaction.
 *
 * Build: make
 * Run:   ./shelbourne-test
 */

#include "shelbourne.h"

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int tests_run, tests_passed, tests_failed;

#define TEST(name) \
    do { \
        tests_run++; \
        printf("  %-36s ", name); \
        fflush(stdout); \
    } while (0)

#define PASS() \
    do { \
        tests_passed++; \
        printf("PASS\n"); \
    } while (0)

#define FAIL(reason) \
    do { \
        tests_failed++; \
        printf("FAIL (%s)\n", reason); \
    } while (0)

static long elapsed_ms(struct timespec *start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000L +
           (now.tv_nsec - start->tv_nsec) / 1000000L;
}

/* ── Volume math tests (no hardware needed) ────────────────────── */

static void test_volume_math(void)
{
    printf("\nVolume curve:\n");

    TEST("volume 0 = mute");
    if (shelbourne_volume_to_gain(0) == 0) PASS(); else FAIL("expected 0");

    TEST("volume 100 = unity");
    if (shelbourne_volume_to_gain(100) == 65536) PASS(); else FAIL("expected 65536");

    TEST("volume -1 = mute");
    if (shelbourne_volume_to_gain(-1) == 0) PASS(); else FAIL("expected 0");

    TEST("volume 101 = unity");
    if (shelbourne_volume_to_gain(101) == 65536) PASS(); else FAIL("expected 65536");

    TEST("volume 50 < unity");
    int g50 = shelbourne_volume_to_gain(50);
    if (g50 > 0 && g50 < 65536) PASS(); else FAIL("out of range");

    TEST("monotonically increasing");
    int ok = 1;
    int prev = 0;
    for (int v = 1; v <= 100; v++) {
        int g = shelbourne_volume_to_gain(v);
        if (g <= prev) { ok = 0; break; }
        prev = g;
    }
    if (ok) PASS(); else FAIL("not monotonic");
}

/* ── Hardware tests ────────────────────────────────────────────── */

static void test_display(shelbourne_t *hw)
{
    printf("\nDisplay:\n");

    TEST("framebuffer available");
    uint8_t *fb = shelbourne_display_buffer(hw);
    if (fb) PASS(); else { FAIL("NULL buffer"); return; }

    TEST("clear sets all black");
    shelbourne_display_clear(hw);
    int all_zero = 1;
    for (int i = 0; i < SHELBOURNE_FB_SIZE; i++)
        if (fb[i] != 0) { all_zero = 0; break; }
    if (all_zero) PASS(); else FAIL("non-zero pixel found");

    TEST("pixel write/read");
    shelbourne_display_pixel(hw, 64, 50, 0xAB);
    if (fb[50 * SHELBOURNE_FB_WIDTH + 64] == 0xAB) PASS(); else FAIL("pixel mismatch");

    TEST("pixel out-of-bounds ignored");
    shelbourne_display_pixel(hw, -1, -1, 0xFF);
    shelbourne_display_pixel(hw, 200, 200, 0xFF);
    PASS(); /* no crash = pass */

    TEST("flush and readback from driver");
    /* Write a known pattern, flush to hardware, read back from /dev/fb0 */
    shelbourne_display_clear(hw);
    for (int x = 0; x < SHELBOURNE_FB_WIDTH; x++)
        fb[x] = (uint8_t)(x * 2);  /* gradient on top row */
    fb[50 * SHELBOURNE_FB_WIDTH + 64] = 0xCD;  /* known pixel */
    shelbourne_display_flush(hw);
    {
        int fbfd = open("/dev/fb0", O_RDONLY);
        if (fbfd < 0) {
            FAIL("cannot open /dev/fb0 for readback");
        } else {
            uint8_t readback[SHELBOURNE_FB_SIZE];
            ssize_t n = read(fbfd, readback, SHELBOURNE_FB_SIZE);
            close(fbfd);
            if (n != SHELBOURNE_FB_SIZE) {
                FAIL("short read from /dev/fb0");
            } else if (memcmp(fb, readback, SHELBOURNE_FB_SIZE) != 0) {
                FAIL("readback mismatch");
            } else {
                PASS();
            }
        }
    }
    usleep(500000);

    TEST("clear and flush");
    shelbourne_display_clear(hw);
    shelbourne_display_flush(hw);
    PASS();
}

static void test_keypad(shelbourne_t *hw)
{
    printf("\nKeypad:\n");

    TEST("keypad fd valid");
    int fd = shelbourne_keypad_fd(hw);
    if (fd >= 0) PASS(); else FAIL("fd < 0");

    TEST("poll returns >= 0");
    shelbourne_key_event_t events[8];
    int n = shelbourne_keypad_poll(hw, events, 8);
    if (n >= 0) PASS(); else FAIL("returned < 0");
}

static void test_led(shelbourne_t *hw)
{
    printf("\nLED:\n");

    TEST("set white");
    shelbourne_led_set(hw, SHELBOURNE_LED_WHITE);
    usleep(300000);
    PASS();

    TEST("set yellow");
    shelbourne_led_set(hw, SHELBOURNE_LED_YELLOW);
    usleep(300000);
    PASS();

    TEST("set off");
    shelbourne_led_set(hw, SHELBOURNE_LED_OFF);
    PASS();
}

static void test_audio(shelbourne_t *hw)
{
    printf("\nAudio:\n");

    TEST("audio_reset");
    shelbourne_audio_reset(hw);
    PASS();

    TEST("write 512 frames");
    int32_t samples[512 * 2];
    int gain = shelbourne_volume_to_gain(15);
    for (int i = 0; i < 512; i++) {
        double t = (double)i / SHELBOURNE_SAMPLE_RATE;
        int32_t s = (int32_t)(sin(2.0 * M_PI * 440.0 * t) * 2147483647.0);
        s = (int32_t)(((int64_t)s * gain) >> 16);
        samples[i * 2] = s;
        samples[i * 2 + 1] = s;
    }
    int ret = shelbourne_audio_write(hw, samples, 512);
    if (ret == 0) PASS(); else FAIL("write returned error");

    TEST("audio_wait flushes");
    ret = shelbourne_audio_wait(hw);
    if (ret == 0) PASS(); else FAIL("wait returned error");

    TEST("play 0.5s tone (440 Hz, vol 15)");
    {
        int ok = 1;
        double phase = 0;
        double step = 2.0 * M_PI * 440.0 / SHELBOURNE_SAMPLE_RATE;
        int frames_left = SHELBOURNE_SAMPLE_RATE / 2;
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        while (frames_left > 0) {
            int chunk = frames_left > 512 ? 512 : frames_left;
            for (int i = 0; i < chunk; i++) {
                int32_t s = (int32_t)(sin(phase) * 2147483647.0);
                s = (int32_t)(((int64_t)s * gain) >> 16);
                samples[i * 2] = s;
                samples[i * 2 + 1] = s;
                phase += step;
            }
            if (shelbourne_audio_write(hw, samples, chunk) < 0 ||
                shelbourne_audio_wait(hw) < 0) {
                ok = 0;
                break;
            }
            frames_left -= chunk;
        }
        if (ok) PASS(); else FAIL("write/wait error during playback");
    }

    TEST("DMA pacing (~500ms for 0.5s)");
    {
        double phase = 0;
        double step = 2.0 * M_PI * 440.0 / SHELBOURNE_SAMPLE_RATE;
        int frames_left = SHELBOURNE_SAMPLE_RATE / 2;
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        while (frames_left > 0) {
            int chunk = frames_left > 512 ? 512 : frames_left;
            for (int i = 0; i < chunk; i++) {
                int32_t s = (int32_t)(sin(phase) * 2147483647.0);
                s = (int32_t)(((int64_t)s * gain) >> 16);
                samples[i * 2] = s;
                samples[i * 2 + 1] = s;
                phase += step;
            }
            shelbourne_audio_write(hw, samples, chunk);
            shelbourne_audio_wait(hw);
            frames_left -= chunk;
        }
        long ms = elapsed_ms(&t0);
        char reason[64];
        if (ms >= 400 && ms <= 700) {
            PASS();
        } else {
            snprintf(reason, sizeof(reason), "took %ldms, expected 400-700", ms);
            FAIL(reason);
        }
    }

    TEST("audio_drain");
    ret = shelbourne_audio_drain(hw);
    if (ret == 0) PASS(); else FAIL("drain returned error");
}

static void test_aux(shelbourne_t *hw)
{
    printf("\nAUX:\n");

    TEST("aux_enable");
    int ret = shelbourne_aux_enable(hw);
    if (ret == 0) PASS(); else { FAIL("enable failed"); return; }

    TEST("aux_pump (5 cycles)");
    {
        int ok = 1;
        int gain = shelbourne_volume_to_gain(30);
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < 5; i++) {
            if (shelbourne_aux_pump(hw, gain) < 0) { ok = 0; break; }
        }
        if (ok) PASS(); else FAIL("pump error");
    }

    TEST("aux_pump DMA timing (~53ms for 5)");
    {
        int ok = 1;
        int gain = shelbourne_volume_to_gain(30);
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < 5; i++) {
            if (shelbourne_aux_pump(hw, gain) < 0) { ok = 0; break; }
        }
        long ms = elapsed_ms(&t0);
        char reason[64];
        if (!ok) {
            FAIL("pump error");
        } else if (ms >= 30 && ms <= 200) {
            PASS();
        } else {
            snprintf(reason, sizeof(reason), "took %ldms, expected 30-200", ms);
            FAIL(reason);
        }
    }

    TEST("aux_disable");
    ret = shelbourne_aux_disable(hw);
    if (ret == 0) PASS(); else FAIL("disable failed");
}

static void test_mute(shelbourne_t *hw)
{
    printf("\nMute:\n");

    TEST("mute on");
    shelbourne_mute(hw, 1);
    usleep(100000);
    PASS();

    TEST("mute off");
    shelbourne_mute(hw, 0);
    PASS();
}

static void test_standby(shelbourne_t *hw)
{
    printf("\nStandby / Resume:\n");

    TEST("enter standby");
    shelbourne_standby(hw);
    PASS();

    usleep(500000);

    TEST("resume from standby");
    shelbourne_resume(hw);
    PASS();

    /* Verify audio still works after resume */
    TEST("audio after resume");
    {
        shelbourne_audio_reset(hw);
        int32_t samples[512 * 2];
        int gain = shelbourne_volume_to_gain(15);
        for (int i = 0; i < 512; i++) {
            double t = (double)i / SHELBOURNE_SAMPLE_RATE;
            int32_t s = (int32_t)(sin(2.0 * M_PI * 880.0 * t) * 2147483647.0);
            s = (int32_t)(((int64_t)s * gain) >> 16);
            samples[i * 2] = s;
            samples[i * 2 + 1] = s;
        }
        int ok = (shelbourne_audio_write(hw, samples, 512) == 0 &&
                  shelbourne_audio_wait(hw) == 0 &&
                  shelbourne_audio_drain(hw) == 0);
        if (ok) PASS(); else FAIL("audio failed after resume");
    }
}

/* ── Hardware test pass (init, test all subsystems, shutdown) ──── */

static int run_hardware_tests(const char *label)
{
    printf("\n%s\n", label);
    printf("Initializing hardware...\n");
    shelbourne_t *hw = shelbourne_init();
    if (!hw) {
        printf("  %-36s FAIL (init returned NULL)\n", "hardware init");
        return -1;
    }
    printf("  %-36s PASS\n", "hardware init");
    tests_run++;
    tests_passed++;

    test_display(hw);
    test_keypad(hw);
    test_led(hw);
    test_audio(hw);
    test_aux(hw);
    test_mute(hw);
    test_standby(hw);

    shelbourne_shutdown(hw);
    return 0;
}

/* ── Stock process management tests ──────────────────────────── */

static void test_process_management(void)
{
    printf("\nStock process management:\n");

    TEST("stock processes initially active");
    if (shelbourne_stock_processes_active())
        PASS();
    else {
        FAIL("not running");
        return;
    }

    TEST("stop stock processes");
    shelbourne_stop_stock_processes();
    /* Give processes time to die */
    usleep(500000);
    if (!shelbourne_stock_processes_active())
        PASS();
    else
        FAIL("still running after stop");

    /* ── First hardware test pass ── */
    if (run_hardware_tests("Hardware tests (pass 1):") < 0) {
        printf("ABORTED: hardware init failed on pass 1\n");
        return;
    }

    TEST("resume stock processes");
    shelbourne_resume_stock_processes();
    PASS();

    TEST("stock processes restart");
    {
        int came_back = 0;
        /* Poll for up to 30 seconds — shepherdd needs to restart children */
        for (int i = 0; i < 300; i++) {
            usleep(100000);
            if (shelbourne_stock_processes_active()) {
                came_back = 1;
                break;
            }
        }
        if (came_back)
            PASS();
        else
            FAIL("not running after 30s");
    }

    TEST("stop stock processes again");
    shelbourne_stop_stock_processes();
    usleep(500000);
    if (!shelbourne_stock_processes_active())
        PASS();
    else
        FAIL("still running after second stop");

    /* ── Second hardware test pass ── */
    if (run_hardware_tests("Hardware tests (pass 2):") < 0)
        printf("ABORTED: hardware init failed on pass 2\n");
}

/* ── Main ──────────────────────────────────────────────────────── */

int main(void)
{
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    printf("Shelbourne Hardware Test Suite\n");
    printf("==============================\n");

    /* Volume math doesn't need hardware */
    test_volume_math();

    if (shelbourne_stock_processes_active()) {
        /* Full lifecycle: stop → test → resume → verify → stop → test */
        test_process_management();
    } else {
        printf("\nStock processes not running (shepherd override?).\n");
        printf("Skipping process management tests.\n");

        /* Freeze shepherdd and kill any player that may hold the audio device */
        shelbourne_stop_stock_processes();
        system("killall -9 player 2>/dev/null");
        sleep(1);

        if (run_hardware_tests("Hardware tests:") < 0) {
            printf("\n==============================\n");
            printf("ABORTED: hardware init failed\n");
            return 1;
        }
    }

    printf("\n==============================\n");
    printf("Results: %d passed, %d failed, %d total\n",
           tests_passed, tests_failed, tests_run);

    if (tests_failed == 0)
        printf("ALL TESTS PASSED\n");
    else
        printf("SOME TESTS FAILED\n");

    return tests_failed > 0 ? 1 : 0;
}
