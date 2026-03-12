/*
 * demo.c — Shelbourne library demonstration
 *
 * Shows basic usage of all library features: audio output, display,
 * keypad, LED, and AUX input.
 *
 * Build: make
 * Run:   ./demo
 */

#include "shelbourne.h"

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile int running = 1;
static void on_signal(int s) { (void)s; running = 0; }

/* Draw a simple text string using a minimal 3x5 digit font */
static void draw_char(shelbourne_t *hw, int x, int y, char c, uint8_t val)
{
    /* Tiny 3x5 font for digits and a few letters */
    static const uint8_t digits[][3] = {
        {0x1F,0x11,0x1F}, /* 0 */ {0x00,0x1F,0x00}, /* 1 */
        {0x1D,0x15,0x17}, /* 2 */ {0x15,0x15,0x1F}, /* 3 */
        {0x07,0x04,0x1F}, /* 4 */ {0x17,0x15,0x1D}, /* 5 */
        {0x1F,0x15,0x1D}, /* 6 */ {0x01,0x01,0x1F}, /* 7 */
        {0x1F,0x15,0x1F}, /* 8 */ {0x17,0x15,0x1F}, /* 9 */
    };

    if (c >= '0' && c <= '9') {
        const uint8_t *g = digits[c - '0'];
        for (int col = 0; col < 3; col++)
            for (int row = 0; row < 5; row++)
                if (g[col] & (1 << row))
                    shelbourne_display_pixel(hw, x + col, y + row, val);
    }
}

int main(void)
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    printf("Shelbourne Library Demo\n");
    printf("=======================\n\n");

    /* Initialize hardware */
    printf("Initializing hardware...\n");
    shelbourne_t *hw = shelbourne_init();
    if (!hw) {
        fprintf(stderr, "Hardware init failed\n");
        return 1;
    }
    printf("Hardware ready.\n\n");

    /* Display: gradient pattern */
    printf("Display: gradient pattern\n");
    shelbourne_display_clear(hw);
    uint8_t *fb = shelbourne_display_buffer(hw);
    if (fb) {
        for (int y = 0; y < SHELBOURNE_FB_HEIGHT; y++)
            for (int x = 0; x < SHELBOURNE_FB_WIDTH; x++)
                fb[y * SHELBOURNE_FB_WIDTH + x] = (x * 255) / SHELBOURNE_FB_WIDTH;
    }
    shelbourne_display_flush(hw);
    sleep(2);

    /* LED: cycle through states */
    printf("LED: white\n");
    shelbourne_led_set(hw, SHELBOURNE_LED_WHITE);
    sleep(1);
    printf("LED: yellow\n");
    shelbourne_led_set(hw, SHELBOURNE_LED_YELLOW);
    sleep(1);
    printf("LED: off\n");
    shelbourne_led_set(hw, SHELBOURNE_LED_OFF);

    /* Audio: play a 440 Hz sine wave for 2 seconds */
    printf("Audio: 440 Hz tone for 2 seconds at volume 30\n");
    int gain = shelbourne_volume_to_gain(30);
    int total_frames = SHELBOURNE_SAMPLE_RATE * 2;
    int32_t samples[512 * 2];

    for (int frame = 0; frame < total_frames && running; frame += 512) {
        int chunk = 512;
        if (frame + chunk > total_frames) chunk = total_frames - frame;

        for (int i = 0; i < chunk; i++) {
            double t = (double)(frame + i) / SHELBOURNE_SAMPLE_RATE;
            int32_t s = (int32_t)(sin(2.0 * M_PI * 440.0 * t) * 2147483647.0);
            s = (int32_t)(((int64_t)s * gain) >> 16);
            samples[i * 2] = s;
            samples[i * 2 + 1] = s;
        }

        shelbourne_audio_write(hw, samples, chunk);
        shelbourne_audio_wait(hw);
    }
    shelbourne_audio_drain(hw);
    printf("Audio done.\n\n");

    /* Keypad: show events for 5 seconds */
    printf("Keypad: press buttons (5 seconds)...\n");
    shelbourne_display_clear(hw);
    if (fb) {
        /* Write "PRESS" in pixels manually */
        for (int x = 30; x < 98; x++)
            shelbourne_display_pixel(hw, x, 45, 0xFF);
        for (int x = 30; x < 98; x++)
            shelbourne_display_pixel(hw, x, 55, 0xFF);
    }
    shelbourne_display_flush(hw);

    static const char *key_names[] = {
        "VOL+", "VOL-", "POWER", "AUX",
        "P1", "P2", "P3", "P4", "P5", "P6", "?"
    };

    for (int t = 0; t < 100 && running; t++) {
        shelbourne_key_event_t events[8];
        int n = shelbourne_keypad_poll(hw, events, 8);
        for (int i = 0; i < n; i++) {
            printf("  %s %s\n", key_names[events[i].key],
                   events[i].pressed ? "pressed" : "released");
        }
        usleep(50000);
    }

    /* Cleanup */
    printf("\nDemo complete. Shutting down.\n");
    shelbourne_display_clear(hw);
    shelbourne_display_flush(hw);
    shelbourne_shutdown(hw);

    return 0;
}
