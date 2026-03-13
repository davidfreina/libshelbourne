/*
 * demo.c — Interactive Shelbourne library demonstration
 *
 * Demonstrates all hardware features with visual feedback:
 *   Presets 1-6:  Play sine waves at different pitches (C4-A4)
 *   Volume +/-:   Adjust volume with on-screen bar
 *   AUX:          Switch to AUX input loopback
 *   Power:        Suspend / resume
 *
 * Build: make
 * Run:   ./demo
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

/* ── 5x7 bitmap font (column-major, bit 0 = top) ──────────────── */

static const uint8_t font5x7[][5] = {
    /* 32 ' ' */ {0x00,0x00,0x00,0x00,0x00},
    /* 33 '!' */ {0x00,0x00,0x5F,0x00,0x00},
    /* 34 '"' */ {0x00,0x07,0x00,0x07,0x00},
    /* 35 '#' */ {0x14,0x7F,0x14,0x7F,0x14},
    /* 36 '$' */ {0x24,0x2A,0x7F,0x2A,0x12},
    /* 37 '%' */ {0x23,0x13,0x08,0x64,0x62},
    /* 38 '&' */ {0x36,0x49,0x55,0x22,0x50},
    /* 39 ''' */ {0x00,0x05,0x03,0x00,0x00},
    /* 40 '(' */ {0x00,0x1C,0x22,0x41,0x00},
    /* 41 ')' */ {0x00,0x41,0x22,0x1C,0x00},
    /* 42 '*' */ {0x14,0x08,0x3E,0x08,0x14},
    /* 43 '+' */ {0x08,0x08,0x3E,0x08,0x08},
    /* 44 ',' */ {0x00,0x50,0x30,0x00,0x00},
    /* 45 '-' */ {0x08,0x08,0x08,0x08,0x08},
    /* 46 '.' */ {0x00,0x60,0x60,0x00,0x00},
    /* 47 '/' */ {0x20,0x10,0x08,0x04,0x02},
    /* 48 '0' */ {0x3E,0x51,0x49,0x45,0x3E},
    /* 49 '1' */ {0x00,0x42,0x7F,0x40,0x00},
    /* 50 '2' */ {0x42,0x61,0x51,0x49,0x46},
    /* 51 '3' */ {0x21,0x41,0x45,0x4B,0x31},
    /* 52 '4' */ {0x18,0x14,0x12,0x7F,0x10},
    /* 53 '5' */ {0x27,0x45,0x45,0x45,0x39},
    /* 54 '6' */ {0x3C,0x4A,0x49,0x49,0x30},
    /* 55 '7' */ {0x01,0x71,0x09,0x05,0x03},
    /* 56 '8' */ {0x36,0x49,0x49,0x49,0x36},
    /* 57 '9' */ {0x06,0x49,0x49,0x29,0x1E},
    /* 58 ':' */ {0x00,0x36,0x36,0x00,0x00},
    /* 59 ';' */ {0x00,0x56,0x36,0x00,0x00},
    /* 60 '<' */ {0x08,0x14,0x22,0x41,0x00},
    /* 61 '=' */ {0x14,0x14,0x14,0x14,0x14},
    /* 62 '>' */ {0x00,0x41,0x22,0x14,0x08},
    /* 63 '?' */ {0x02,0x01,0x51,0x09,0x06},
    /* 64 '@' */ {0x32,0x49,0x79,0x41,0x3E},
    /* 65 'A' */ {0x7E,0x11,0x11,0x11,0x7E},
    /* 66 'B' */ {0x7F,0x49,0x49,0x49,0x36},
    /* 67 'C' */ {0x3E,0x41,0x41,0x41,0x22},
    /* 68 'D' */ {0x7F,0x41,0x41,0x22,0x1C},
    /* 69 'E' */ {0x7F,0x49,0x49,0x49,0x41},
    /* 70 'F' */ {0x7F,0x09,0x09,0x09,0x01},
    /* 71 'G' */ {0x3E,0x41,0x49,0x49,0x7A},
    /* 72 'H' */ {0x7F,0x08,0x08,0x08,0x7F},
    /* 73 'I' */ {0x00,0x41,0x7F,0x41,0x00},
    /* 74 'J' */ {0x20,0x40,0x41,0x3F,0x01},
    /* 75 'K' */ {0x7F,0x08,0x14,0x22,0x41},
    /* 76 'L' */ {0x7F,0x40,0x40,0x40,0x40},
    /* 77 'M' */ {0x7F,0x02,0x0C,0x02,0x7F},
    /* 78 'N' */ {0x7F,0x04,0x08,0x10,0x7F},
    /* 79 'O' */ {0x3E,0x41,0x41,0x41,0x3E},
    /* 80 'P' */ {0x7F,0x09,0x09,0x09,0x06},
    /* 81 'Q' */ {0x3E,0x41,0x51,0x21,0x5E},
    /* 82 'R' */ {0x7F,0x09,0x19,0x29,0x46},
    /* 83 'S' */ {0x46,0x49,0x49,0x49,0x31},
    /* 84 'T' */ {0x01,0x01,0x7F,0x01,0x01},
    /* 85 'U' */ {0x3F,0x40,0x40,0x40,0x3F},
    /* 86 'V' */ {0x1F,0x20,0x40,0x20,0x1F},
    /* 87 'W' */ {0x3F,0x40,0x38,0x40,0x3F},
    /* 88 'X' */ {0x63,0x14,0x08,0x14,0x63},
    /* 89 'Y' */ {0x07,0x08,0x70,0x08,0x07},
    /* 90 'Z' */ {0x61,0x51,0x49,0x45,0x43},
};

#define FONT_W 5
#define FONT_H 7
#define CHAR_SPACE 1
#define W SHELBOURNE_FB_WIDTH
#define H SHELBOURNE_FB_HEIGHT

static void draw_char(uint8_t *fb, int x, int y, char c, uint8_t val)
{
    if (c >= 'a' && c <= 'z') c -= 32;
    int idx = c - 32;
    if (idx < 0 || idx >= (int)(sizeof(font5x7) / sizeof(font5x7[0]))) return;
    const uint8_t *glyph = font5x7[idx];
    for (int col = 0; col < FONT_W; col++)
        for (int row = 0; row < FONT_H; row++)
            if (glyph[col] & (1 << row)) {
                int px = x + col, py = y + row;
                if (px >= 0 && px < W && py >= 0 && py < H)
                    fb[py * W + px] = val;
            }
}

static void draw_text(uint8_t *fb, int x, int y, const char *s, uint8_t val)
{
    for (; *s; s++, x += FONT_W + CHAR_SPACE)
        draw_char(fb, x, y, *s, val);
}

static void draw_char_2x(uint8_t *fb, int x, int y, char c, uint8_t val)
{
    if (c >= 'a' && c <= 'z') c -= 32;
    int idx = c - 32;
    if (idx < 0 || idx >= (int)(sizeof(font5x7) / sizeof(font5x7[0]))) return;
    const uint8_t *glyph = font5x7[idx];
    for (int col = 0; col < FONT_W; col++)
        for (int row = 0; row < FONT_H; row++)
            if (glyph[col] & (1 << row))
                for (int dy = 0; dy < 2; dy++)
                    for (int dx = 0; dx < 2; dx++) {
                        int px = x + col * 2 + dx, py = y + row * 2 + dy;
                        if (px >= 0 && px < W && py >= 0 && py < H)
                            fb[py * W + px] = val;
                    }
}

static void draw_text_2x(uint8_t *fb, int x, int y, const char *s, uint8_t val)
{
    for (; *s; s++, x += FONT_W * 2 + 2)
        draw_char_2x(fb, x, y, *s, val);
}

static int text_width(const char *s)
{
    int n = strlen(s);
    return n > 0 ? n * (FONT_W + CHAR_SPACE) - CHAR_SPACE : 0;
}

static int text_width_2x(const char *s)
{
    int n = strlen(s);
    return n > 0 ? n * (FONT_W * 2 + 2) - 2 : 0;
}

static void center_text(uint8_t *fb, int y, const char *s, uint8_t val)
{
    draw_text(fb, (W - text_width(s)) / 2, y, s, val);
}

static void center_text_2x(uint8_t *fb, int y, const char *s, uint8_t val)
{
    draw_text_2x(fb, (W - text_width_2x(s)) / 2, y, s, val);
}

/* ── Display helpers ───────────────────────────────────────────── */

static void draw_hline(uint8_t *fb, int x0, int x1, int y, uint8_t val)
{
    if (y < 0 || y >= H) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= W) x1 = W - 1;
    for (int x = x0; x <= x1; x++)
        fb[y * W + x] = val;
}

static void draw_volume_bar(uint8_t *fb, int volume)
{
    int bar_y = H - 12;
    int bar_h = 8;
    int bar_x = 10;
    int bar_w = W - 20;

    /* Outline */
    draw_hline(fb, bar_x, bar_x + bar_w, bar_y, 0x60);
    draw_hline(fb, bar_x, bar_x + bar_w, bar_y + bar_h, 0x60);
    for (int y = bar_y; y <= bar_y + bar_h; y++) {
        if (bar_x >= 0 && bar_x < W) fb[y * W + bar_x] = 0x60;
        int rx = bar_x + bar_w;
        if (rx >= 0 && rx < W) fb[y * W + rx] = 0x60;
    }

    /* Fill */
    int fill_w = (bar_w - 2) * volume / 100;
    for (int y = bar_y + 1; y < bar_y + bar_h; y++)
        for (int x = bar_x + 1; x <= bar_x + 1 + fill_w; x++)
            if (x < W) fb[y * W + x] = 0xC0;

    /* Label */
    char label[8];
    snprintf(label, sizeof(label), "%d", volume);
    int lx = bar_x + bar_w + 3;
    if (lx + text_width(label) <= W)
        draw_text(fb, lx, bar_y + 1, label, 0xA0);
}

/* ── Screens ───────────────────────────────────────────────────── */

static void draw_splash(uint8_t *fb)
{
    /* Horizontal gradient background */
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            fb[y * W + x] = (uint8_t)((x * 40) / W);

    center_text_2x(fb, 4, "SHELBOURNE", 0xFF);

    /* Grayscale ramp bar */
    int ramp_y = 26;
    for (int x = 10; x < W - 10; x++) {
        uint8_t val = (uint8_t)((x - 10) * 255 / (W - 21));
        for (int y = ramp_y; y < ramp_y + 12; y++)
            fb[y * W + x] = val;
    }

    center_text(fb, 42, "128 X 100  8-BIT", 0xFF);
    center_text(fb, 52, "GRAYSCALE OLED", 0xFF);

    /* Corner markers showing exact dimensions */
    for (int i = 0; i < 5; i++) {
        fb[i] = 0xFF;                              /* top-left */
        fb[i * W] = 0xFF;
        fb[i + W - 5] = 0xFF;                      /* top-right */
        fb[i * W + W - 1] = 0xFF;
        fb[(H - 1) * W + i] = 0xFF;                /* bottom-left */
        fb[(H - 1 - i) * W] = 0xFF;
        fb[(H - 1) * W + W - 5 + i] = 0xFF;        /* bottom-right */
        fb[(H - 1 - i) * W + W - 1] = 0xFF;
    }

    center_text(fb, 68, "VOL+/-  PRESETS 1-6", 0xC0);
    center_text(fb, 78, "AUX  POWER=STANDBY", 0xC0);
    center_text(fb, 90, "PRESS ANY BUTTON", 0xFF);
}

static const struct {
    const char *name;
    double freq;
} notes[6] = {
    {"C4",  261.63},
    {"D4",  293.66},
    {"E4",  329.63},
    {"F4",  349.23},
    {"G4",  392.00},
    {"A4",  440.00},
};

static void draw_tone_screen(uint8_t *fb, int preset, int volume)
{
    char hdr[16];
    snprintf(hdr, sizeof(hdr), "PRESET %d", preset + 1);
    center_text_2x(fb, 2, hdr, 0xFF);

    center_text_2x(fb, 24, notes[preset].name, 0xFF);

    char freq[16];
    snprintf(freq, sizeof(freq), "%.0f HZ", notes[preset].freq);
    center_text(fb, 42, freq, 0xC0);

    /* Sine wave visualization */
    int wave_y = 58;
    for (int x = 4; x < W - 4; x++) {
        double t = (double)(x - 4) / (W - 8) * 4.0 * M_PI;
        int y = wave_y + (int)(8.0 * sin(t));
        if (y >= 0 && y < H) fb[y * W + x] = 0xFF;
        if (y + 1 >= 0 && y + 1 < H) fb[(y + 1) * W + x] = 0x80;
    }

    draw_volume_bar(fb, volume);
}

static void draw_aux_screen(uint8_t *fb, int volume)
{
    center_text_2x(fb, 6, "AUX INPUT", 0xFF);

    /* Simple speaker icon */
    int cx = W / 2, cy = 42;
    for (int dy = -6; dy <= 6; dy++)
        for (int dx = -4; dx <= 4; dx++)
            if (cx + dx >= 0 && cx + dx < W && cy + dy >= 0 && cy + dy < H)
                fb[(cy + dy) * W + (cx + dx)] = 0xC0;
    /* Sound waves */
    for (int r = 10; r <= 22; r += 6)
        for (int a = -45; a <= 45; a++) {
            double rad = a * M_PI / 180.0;
            int wx = cx + (int)(r * cos(rad));
            int wy = cy + (int)(r * sin(rad));
            if (wx >= 0 && wx < W && wy >= 0 && wy < H)
                fb[wy * W + wx] = (uint8_t)(0xFF - r * 4);
        }

    center_text(fb, 64, "LISTENING...", 0xA0);

    draw_volume_bar(fb, volume);
}

/* ── State ─────────────────────────────────────────────────────── */

enum demo_state { STATE_IDLE, STATE_TONE, STATE_AUX, STATE_STANDBY };

static shelbourne_t *hw;
static enum demo_state state = STATE_IDLE;
static enum demo_state saved_state = STATE_IDLE;  /* state before standby */
static int current_preset = -1;
static int saved_preset = -1;                     /* preset before standby */
static int volume = 20;
static int gain;
static double phase = 0;
static int display_dirty = 1;

static void update_display(void)
{
    uint8_t *fb = shelbourne_display_buffer(hw);
    if (!fb) return;
    shelbourne_display_clear(hw);

    switch (state) {
    case STATE_IDLE:
        draw_splash(fb);
        break;
    case STATE_TONE:
        draw_tone_screen(fb, current_preset, volume);
        break;
    case STATE_AUX:
        draw_aux_screen(fb, volume);
        break;
    case STATE_STANDBY:
        center_text_2x(fb, 40, "OFF", 0x40);
        break;
    }

    shelbourne_display_flush(hw);
    display_dirty = 0;
}

static void enter_tone(int preset)
{
    if (state == STATE_AUX)
        shelbourne_aux_disable(hw);
    shelbourne_audio_reset(hw);
    current_preset = preset;
    state = STATE_TONE;
    phase = 0;
    shelbourne_led_set(hw, SHELBOURNE_LED_WHITE);
    display_dirty = 1;
    printf("Preset %d: %s (%.0f Hz)\n", preset + 1,
           notes[preset].name, notes[preset].freq);
}

static void enter_aux(void)
{
    shelbourne_audio_reset(hw);
    if (shelbourne_aux_enable(hw) < 0) {
        printf("AUX enable failed\n");
        return;
    }
    state = STATE_AUX;
    current_preset = -1;
    shelbourne_led_set(hw, SHELBOURNE_LED_YELLOW);
    display_dirty = 1;
    printf("AUX mode\n");
}

static void enter_idle(void)
{
    if (state == STATE_AUX)
        shelbourne_aux_disable(hw);
    state = STATE_IDLE;
    current_preset = -1;
    shelbourne_led_set(hw, SHELBOURNE_LED_OFF);
    shelbourne_audio_reset(hw);
    display_dirty = 1;
    printf("Idle\n");
}

static void enter_standby(void)
{
    saved_state = state;
    saved_preset = current_preset;
    if (state == STATE_AUX)
        shelbourne_aux_disable(hw);
    state = STATE_STANDBY;
    shelbourne_standby(hw);
    display_dirty = 1;
    printf("Standby\n");
}

static void leave_standby(void)
{
    shelbourne_resume(hw);
    state = STATE_IDLE;
    display_dirty = 1;
    printf("Resumed\n");
}

static void restore_from_standby(void)
{
    leave_standby();
    if (saved_state == STATE_TONE && saved_preset >= 0)
        enter_tone(saved_preset);
    else if (saved_state == STATE_AUX)
        enter_aux();
    else {
        shelbourne_led_set(hw, SHELBOURNE_LED_OFF);
        shelbourne_audio_reset(hw);
    }
}

static void handle_key(shelbourne_key_t key)
{
    /* Any button in standby: resume first */
    if (state == STATE_STANDBY) {
        if (key == SHELBOURNE_KEY_POWER) {
            restore_from_standby();
            return;
        }
        /* Other buttons: resume to idle, then fall through to handle */
        leave_standby();
    }

    switch (key) {
    case SHELBOURNE_KEY_POWER:
        enter_standby();
        break;

    case SHELBOURNE_KEY_PRESET_1:
    case SHELBOURNE_KEY_PRESET_2:
    case SHELBOURNE_KEY_PRESET_3:
    case SHELBOURNE_KEY_PRESET_4:
    case SHELBOURNE_KEY_PRESET_5:
    case SHELBOURNE_KEY_PRESET_6: {
        int p = key - SHELBOURNE_KEY_PRESET_1;
        if (state == STATE_TONE && current_preset == p)
            enter_idle();
        else
            enter_tone(p);
        break;
    }

    case SHELBOURNE_KEY_AUX:
        if (state == STATE_AUX)
            enter_idle();
        else
            enter_aux();
        break;

    case SHELBOURNE_KEY_VOLUME_UP:
        if (volume < 100) {
            volume += 5;
            if (volume > 100) volume = 100;
            gain = shelbourne_volume_to_gain(volume);
            display_dirty = 1;
            printf("Volume: %d\n", volume);
        }
        break;

    case SHELBOURNE_KEY_VOLUME_DOWN:
        if (volume > 5) {
            volume -= 5;
            if (volume < 5) volume = 5;
            gain = shelbourne_volume_to_gain(volume);
            display_dirty = 1;
            printf("Volume: %d\n", volume);
        }
        break;

    default:
        break;
    }
}

static void poll_keys(void)
{
    shelbourne_key_event_t events[8];
    int n = shelbourne_keypad_poll(hw, events, 8);
    for (int i = 0; i < n; i++)
        if (events[i].pressed)
            handle_key(events[i].key);
}

/* ── Main ──────────────────────────────────────────────────────── */

int main(void)
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    printf("Shelbourne Interactive Demo\n");
    printf("===========================\n\n");

    shelbourne_stop_stock_processes();

    hw = shelbourne_init();
    if (!hw) {
        fprintf(stderr, "Hardware init failed\n");
        return 1;
    }

    gain = shelbourne_volume_to_gain(volume);
    update_display();
    printf("Ready. Press buttons to interact.\n\n");

    int32_t samples[512 * 2];

    while (running) {
        poll_keys();

        if (state == STATE_TONE) {
            double freq = notes[current_preset].freq;
            double step = 2.0 * M_PI * freq / SHELBOURNE_SAMPLE_RATE;

            for (int i = 0; i < 512; i++) {
                int32_t s = (int32_t)(sin(phase) * 2147483647.0);
                s = (int32_t)(((int64_t)s * gain) >> 16);
                samples[i * 2] = s;
                samples[i * 2 + 1] = s;
                phase += step;
            }
            if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;

            shelbourne_audio_write(hw, samples, 512);
            if (display_dirty) update_display();
            shelbourne_audio_wait(hw);

        } else if (state == STATE_AUX) {
            shelbourne_aux_pump(hw, gain);
            if (display_dirty) update_display();

        } else if (state == STATE_STANDBY) {
            /* Sleep efficiently, waiting for keypad input */
            int kfd = shelbourne_keypad_fd(hw);
            if (kfd >= 0) {
                fd_set fds;
                FD_ZERO(&fds);
                FD_SET(kfd, &fds);
                struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};
                select(kfd + 1, &fds, NULL, NULL, &tv);
            } else {
                usleep(100000);
            }
            if (display_dirty) update_display();

        } else {
            /* IDLE — low CPU usage */
            usleep(50000);
            if (display_dirty) update_display();
        }
    }

    printf("\nShutting down...\n");
    if (state == STATE_AUX)
        shelbourne_aux_disable(hw);
    if (state == STATE_STANDBY)
        shelbourne_resume(hw);
    shelbourne_shutdown(hw);
    printf("Done.\n");
    return 0;
}
