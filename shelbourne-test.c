/*
 * shelbourne-test.c — CLI tool for testing Shelbourne hardware
 *
 * Usage: shelbourne-test <command> [args]
 *
 * Commands:
 *   init                        Initialize hardware and show status
 *   tone [freq] [secs] [vol]    Play sine wave (default: 440 Hz, 2s, vol 30)
 *   aux [vol]                   AUX loopback (default vol 50, Ctrl+C to stop)
 *   display <pattern>           Test pattern (gradient|grid|circle|white|black)
 *   keypad                      Print key events (Ctrl+C to stop)
 *   led <off|white|yellow>      Set LED state
 *   standby                     Enter standby, press power to resume
 *   mute                        Toggle hardware mute
 */

#include "shelbourne.h"

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

static volatile int running = 1;
static void on_signal(int s) { (void)s; running = 0; }

static const char *key_names[] = {
    "VOLUME_UP", "VOLUME_DOWN", "POWER", "AUX",
    "PRESET_1", "PRESET_2", "PRESET_3", "PRESET_4",
    "PRESET_5", "PRESET_6", "UNKNOWN"
};

/* ── Commands ─────────────────────────────────────────────────────── */

static int cmd_init(shelbourne_t *hw)
{
    printf("Hardware initialized successfully.\n");
    printf("  Audio:   fd open\n");
    printf("  Display: %s\n",
           shelbourne_display_buffer(hw) ? "available" : "not available");
    printf("  Keypad:  %s (fd=%d)\n",
           shelbourne_keypad_fd(hw) >= 0 ? "available" : "not available",
           shelbourne_keypad_fd(hw));
    return 0;
}

static int cmd_tone(shelbourne_t *hw, int argc, char **argv)
{
    double freq = 440.0;
    double secs = 2.0;
    int vol = 30;

    if (argc > 0) freq = atof(argv[0]);
    if (argc > 1) secs = atof(argv[1]);
    if (argc > 2) vol = atoi(argv[2]);

    if (freq < 20 || freq > 20000) { fprintf(stderr, "Frequency out of range\n"); return 1; }
    if (vol < 0 || vol > 100) { fprintf(stderr, "Volume out of range\n"); return 1; }

    printf("Playing %.0f Hz for %.1fs at volume %d\n", freq, secs, vol);

    int gain = shelbourne_volume_to_gain(vol);
    int total_frames = (int)(SHELBOURNE_SAMPLE_RATE * secs);
    int32_t samples[512 * 2];

    for (int frame = 0; frame < total_frames && running; frame += 512) {
        int chunk = 512;
        if (frame + chunk > total_frames) chunk = total_frames - frame;

        for (int i = 0; i < chunk; i++) {
            double t = (double)(frame + i) / SHELBOURNE_SAMPLE_RATE;
            int32_t s = (int32_t)(sin(2.0 * M_PI * freq * t) * 2147483647.0);
            s = (int32_t)(((int64_t)s * gain) >> 16);
            samples[i * 2] = s;
            samples[i * 2 + 1] = s;
        }

        shelbourne_audio_write(hw, samples, chunk);
        shelbourne_audio_wait(hw);
    }
    shelbourne_audio_drain(hw);
    printf("Done.\n");
    return 0;
}

static int cmd_aux(shelbourne_t *hw, int argc, char **argv)
{
    int vol = 50;
    if (argc > 0) vol = atoi(argv[0]);
    if (vol < 0 || vol > 100) { fprintf(stderr, "Volume out of range\n"); return 1; }

    int gain = shelbourne_volume_to_gain(vol);
    printf("AUX loopback at volume %d (gain=%d). Ctrl+C to stop.\n", vol, gain);

    if (shelbourne_aux_enable(hw) < 0) {
        fprintf(stderr, "Failed to enable AUX\n");
        return 1;
    }

    int loops = 0;
    while (running) {
        if (shelbourne_aux_pump(hw, gain) < 0) {
            fprintf(stderr, "AUX pump error\n");
            break;
        }

        /* Poll keypad for volume changes */
        shelbourne_key_event_t events[8];
        int n = shelbourne_keypad_poll(hw, events, 8);
        for (int i = 0; i < n; i++) {
            if (!events[i].pressed) continue;
            if (events[i].key == SHELBOURNE_KEY_VOLUME_UP && vol < 100) {
                vol += 5;
                gain = shelbourne_volume_to_gain(vol);
                printf("Volume: %d\n", vol);
            } else if (events[i].key == SHELBOURNE_KEY_VOLUME_DOWN && vol > 0) {
                vol -= 5;
                gain = shelbourne_volume_to_gain(vol);
                printf("Volume: %d\n", vol);
            }
        }

        if (++loops % 1000 == 0)
            printf("  %d cycles\n", loops);
    }

    shelbourne_aux_disable(hw);
    printf("AUX stopped after %d cycles.\n", loops);
    return 0;
}

static int cmd_display(shelbourne_t *hw, int argc, char **argv)
{
    const char *pattern = "gradient";
    if (argc > 0) pattern = argv[0];

    uint8_t *fb = shelbourne_display_buffer(hw);
    if (!fb) { fprintf(stderr, "Display not available\n"); return 1; }

    shelbourne_display_clear(hw);

    if (strcmp(pattern, "gradient") == 0) {
        printf("Display: horizontal gradient\n");
        for (int y = 0; y < SHELBOURNE_FB_HEIGHT; y++)
            for (int x = 0; x < SHELBOURNE_FB_WIDTH; x++)
                fb[y * SHELBOURNE_FB_WIDTH + x] = (x * 255) / SHELBOURNE_FB_WIDTH;

    } else if (strcmp(pattern, "grid") == 0) {
        printf("Display: 8x8 grid\n");
        for (int y = 0; y < SHELBOURNE_FB_HEIGHT; y++)
            for (int x = 0; x < SHELBOURNE_FB_WIDTH; x++)
                if (x % 8 == 0 || y % 8 == 0)
                    fb[y * SHELBOURNE_FB_WIDTH + x] = 0x80;

    } else if (strcmp(pattern, "circle") == 0) {
        printf("Display: centered circle\n");
        int cx = SHELBOURNE_FB_WIDTH / 2, cy = SHELBOURNE_FB_HEIGHT / 2;
        int r = (SHELBOURNE_FB_HEIGHT - 4) / 2;
        for (int y = 0; y < SHELBOURNE_FB_HEIGHT; y++)
            for (int x = 0; x < SHELBOURNE_FB_WIDTH; x++) {
                int dx = x - cx, dy = y - cy;
                double d = sqrt(dx * dx + dy * dy);
                if (d < r - 1) fb[y * SHELBOURNE_FB_WIDTH + x] = 0xFF;
                else if (d < r + 1) fb[y * SHELBOURNE_FB_WIDTH + x] = 0x80;
            }

    } else if (strcmp(pattern, "white") == 0) {
        printf("Display: all white\n");
        memset(fb, 0xFF, SHELBOURNE_FB_SIZE);

    } else if (strcmp(pattern, "black") == 0) {
        printf("Display: all black\n");
        /* Already cleared */

    } else {
        fprintf(stderr, "Unknown pattern: %s\n", pattern);
        fprintf(stderr, "Available: gradient, grid, circle, white, black\n");
        return 1;
    }

    shelbourne_display_flush(hw);
    printf("Displayed. Press Ctrl+C to clear and exit.\n");

    while (running) usleep(100000);

    shelbourne_display_clear(hw);
    shelbourne_display_flush(hw);
    return 0;
}

static int cmd_keypad(shelbourne_t *hw)
{
    printf("Keypad monitor. Press buttons (Ctrl+C to stop).\n");

    int kfd = shelbourne_keypad_fd(hw);
    if (kfd < 0) { fprintf(stderr, "Keypad not available\n"); return 1; }

    while (running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(kfd, &fds);
        struct timeval tv = {.tv_sec = 1};

        int ret = select(kfd + 1, &fds, NULL, NULL, &tv);
        if (ret > 0) {
            shelbourne_key_event_t events[16];
            int n = shelbourne_keypad_poll(hw, events, 16);
            for (int i = 0; i < n; i++)
                printf("  %-12s %s\n", key_names[events[i].key],
                       events[i].pressed ? "PRESS" : "RELEASE");
        }
    }

    return 0;
}

static int cmd_led(shelbourne_t *hw, int argc, char **argv)
{
    if (argc < 1) { fprintf(stderr, "Usage: led <off|white|yellow>\n"); return 1; }

    if (strcmp(argv[0], "off") == 0) {
        shelbourne_led_set(hw, SHELBOURNE_LED_OFF);
        printf("LED: off\n");
    } else if (strcmp(argv[0], "white") == 0) {
        shelbourne_led_set(hw, SHELBOURNE_LED_WHITE);
        printf("LED: white\n");
    } else if (strcmp(argv[0], "yellow") == 0) {
        shelbourne_led_set(hw, SHELBOURNE_LED_YELLOW);
        printf("LED: yellow\n");
    } else {
        fprintf(stderr, "Unknown LED state: %s\n", argv[0]);
        return 1;
    }
    return 0;
}

static int cmd_standby(shelbourne_t *hw)
{
    printf("Entering standby. Press POWER to resume.\n");
    shelbourne_standby(hw);

    int kfd = shelbourne_keypad_fd(hw);
    if (kfd < 0) {
        fprintf(stderr, "Keypad not available, sleeping 10s instead\n");
        sleep(10);
    } else {
        while (running) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(kfd, &fds);
            struct timeval tv = {.tv_sec = 1};

            if (select(kfd + 1, &fds, NULL, NULL, &tv) > 0) {
                shelbourne_key_event_t events[8];
                int n = shelbourne_keypad_poll(hw, events, 8);
                for (int i = 0; i < n; i++)
                    if (events[i].key == SHELBOURNE_KEY_POWER && events[i].pressed)
                        goto wake;
            }
        }
    }

wake:
    printf("Resuming...\n");
    shelbourne_resume(hw);
    printf("Resumed. Playing confirmation tone.\n");

    int gain = shelbourne_volume_to_gain(30);
    int32_t samples[512 * 2];
    for (int frame = 0; frame < SHELBOURNE_SAMPLE_RATE; frame += 512) {
        int chunk = 512;
        for (int i = 0; i < chunk; i++) {
            double t = (double)(frame + i) / SHELBOURNE_SAMPLE_RATE;
            int32_t s = (int32_t)(sin(2.0 * M_PI * 880.0 * t) * 2147483647.0);
            s = (int32_t)(((int64_t)s * gain) >> 16);
            samples[i * 2] = s;
            samples[i * 2 + 1] = s;
        }
        shelbourne_audio_write(hw, samples, chunk);
        shelbourne_audio_wait(hw);
    }
    shelbourne_audio_drain(hw);
    printf("Done.\n");
    return 0;
}

static int cmd_mute(shelbourne_t *hw)
{
    static int muted = 0;
    muted = !muted;
    shelbourne_mute(hw, muted);
    printf("Mute: %s\n", muted ? "ON" : "OFF");
    return 0;
}

/* ── Main ─────────────────────────────────────────────────────────── */

static void usage(void)
{
    fprintf(stderr,
        "Usage: shelbourne-test <command> [args]\n"
        "\n"
        "Commands:\n"
        "  init                        Initialize and show status\n"
        "  tone [freq] [secs] [vol]    Play sine wave (440 Hz, 2s, vol 30)\n"
        "  aux [vol]                   AUX loopback (vol 50, Ctrl+C to stop)\n"
        "  display <pattern>           gradient|grid|circle|white|black\n"
        "  keypad                      Print key events (Ctrl+C to stop)\n"
        "  led <off|white|yellow>      Set LED state\n"
        "  standby                     Standby, press power to resume\n"
        "  mute                        Toggle hardware mute\n"
    );
}

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(); return 1; }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    const char *cmd = argv[1];

    shelbourne_t *hw = shelbourne_init();
    if (!hw) {
        fprintf(stderr, "Hardware init failed\n");
        return 1;
    }

    int ret = 0;
    if (strcmp(cmd, "init") == 0)          ret = cmd_init(hw);
    else if (strcmp(cmd, "tone") == 0)     ret = cmd_tone(hw, argc - 2, argv + 2);
    else if (strcmp(cmd, "aux") == 0)      ret = cmd_aux(hw, argc - 2, argv + 2);
    else if (strcmp(cmd, "display") == 0)  ret = cmd_display(hw, argc - 2, argv + 2);
    else if (strcmp(cmd, "keypad") == 0)   ret = cmd_keypad(hw);
    else if (strcmp(cmd, "led") == 0)      ret = cmd_led(hw, argc - 2, argv + 2);
    else if (strcmp(cmd, "standby") == 0)  ret = cmd_standby(hw);
    else if (strcmp(cmd, "mute") == 0)     ret = cmd_mute(hw);
    else { fprintf(stderr, "Unknown command: %s\n", cmd); usage(); ret = 1; }

    shelbourne_shutdown(hw);
    return ret;
}
