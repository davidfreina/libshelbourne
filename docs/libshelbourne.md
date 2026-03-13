# libshelbourne API Reference

C library for the Shelbourne audio platform (SoundTouch 20 hardware).

## Overview

libshelbourne provides a single-header API for:

- **Audio output** — 48 kHz 32-bit stereo via McASP/EDMA DMA
- **AUX input** — Software loopback from 3.5mm jack to speakers
- **Volume** — Software gain with logarithmic curve
- **Display** — 128x100 8-bit grayscale OLED framebuffer
- **Keypad** — 10 buttons (volume, power, AUX, presets 1-6)
- **LED** — White / yellow / off status indicator
- **Standby** — Low-power mode with amplifier shutdown

All functions are single-threaded. The API is defined in `shelbourne.h`.

## Stock Process Management

The stock Bose software holds the audio device open. These functions
manage the stock processes so the library can access the hardware.

For deployment, use the shepherdd override (`/mnt/nv/shepherd/`) so
stock processes never start. During development, use these functions.

### `int shelbourne_stock_processes_active(void)`

Check if stock audio processes are running. Returns 1 if any are
found, 0 if none.

### `void shelbourne_stop_stock_processes(void)`

Freeze the process supervisor (shepherdd) and kill all stock audio
processes. Safe to call if they are not running.

### `void shelbourne_resume_stock_processes(void)`

Resume the process supervisor, which restarts the stock software.
Call after `shelbourne_shutdown()` to restore normal device operation
without rebooting.

## Lifecycle

### `shelbourne_t *shelbourne_init(void)`

Initialize all hardware. Takes ~4 seconds (amplifier stabilization).

Stock Bose processes must not be running — either use the shepherdd
override or call `shelbourne_stop_stock_processes()` first.

1. Fixes keypad scan timer (driver workaround)
2. Loads codec firmware via sysfs (falls back to direct register writes)
3. Powers on amplifier via GPIO
4. Starts McASP/EDMA for audio output
5. Opens display and keypad (non-fatal if missing)

Returns `NULL` on fatal error (codec or audio device failure).

### `void shelbourne_shutdown(shelbourne_t *hw)`

Mutes amplifier, clears display, turns off LED, closes all file
descriptors, frees resources. Does not resume stock processes.

## Audio Output

Audio format is fixed by hardware: 48 kHz, 32-bit signed, stereo
interleaved (L, R, L, R, ...). Full 32-bit range.

### `int shelbourne_audio_write(shelbourne_t *hw, const int32_t *samples, int frames)`

Buffer audio samples for DMA output. Returns immediately after copying
into an internal ring buffer. Any frame count is accepted.

Returns 0 on success, -1 if buffer overflows.

### `int shelbourne_audio_wait(shelbourne_t *hw)`

Flush buffered audio to DMA, blocking to pace at 48 kHz. Writes all
complete 512-frame chunks, sleeping between writes. Returns when the
buffer has fewer than 512 frames remaining.

Each chunk takes ~10.7ms. Safe to poll the keypad between `audio_write`
and `audio_wait`.

### `int shelbourne_audio_drain(shelbourne_t *hw)`

Pad remaining audio to a 512-frame boundary and flush. Call at end of
track to avoid truncating the last few milliseconds.

### `void shelbourne_audio_reset(shelbourne_t *hw)`

Reset buffer and pacing state. Call when switching sources to avoid a
burst of catch-up writes.

### Typical playback loop

```c
int gain = shelbourne_volume_to_gain(50);

while (have_audio) {
    int32_t samples[512 * 2];
    // ... decode/generate audio, apply gain ...
    for (int i = 0; i < 512; i++) {
        samples[i*2]   = (int32_t)(((int64_t)raw[i*2]   * gain) >> 16);
        samples[i*2+1] = (int32_t)(((int64_t)raw[i*2+1] * gain) >> 16);
    }
    shelbourne_audio_write(hw, samples, 512);
    shelbourne_keypad_poll(hw, events, 8);  // non-blocking
    shelbourne_audio_wait(hw);              // blocks ~10ms
}
shelbourne_audio_drain(hw);
```

## AUX Input

AUX audio does not flow automatically to the speakers. The caller must
pump audio from the codec's ADC to the DAC in a software loop.

### `int shelbourne_aux_enable(shelbourne_t *hw)`

Start receiving from the 3.5mm input. While AUX is active,
`audio_write`/`wait`/`drain` are unavailable.

### `int shelbourne_aux_pump(shelbourne_t *hw, int gain)`

Read one DMA buffer (~10.7ms) from AUX input, apply volume gain, write
to speakers. Blocks until input data is available.

### `int shelbourne_aux_disable(shelbourne_t *hw)`

Stop AUX input and restore normal playback mode.

### Typical AUX loop

```c
int gain = shelbourne_volume_to_gain(50);
shelbourne_aux_enable(hw);
while (aux_active) {
    shelbourne_aux_pump(hw, gain);
    shelbourne_keypad_poll(hw, events, 8);
}
shelbourne_aux_disable(hw);
```

## Volume

### `int shelbourne_volume_to_gain(int volume)`

Convert volume 0-100 to a fixed-point gain multiplier 0-65536.

- Volume 0 = mute (gain 0)
- Volume 100 = unity (gain 65536)
- Logarithmic curve (~67 dB dynamic range)

Apply to samples: `out = (int32_t)(((int64_t)in * gain) >> 16)`

Pure function — no hardware access.

### `void shelbourne_mute(shelbourne_t *hw, int mute)`

Hardware mute via GPIO. Instant silence regardless of audio writes.
Use for mode switches; use software gain for normal volume control.

## Standby

### `void shelbourne_standby(shelbourne_t *hw)`

Enter low-power mode: mute, power off amplifier, blank display, LED off.
WiFi stays on. Audio buffer state is preserved. Keypad remains active.

For deeper sleep, call `shelbourne_wifi_set(hw, 0)` after standby.

### `void shelbourne_resume(shelbourne_t *hw)`

Resume from standby: power on amplifier, unmute, reset audio pacing.
Returns immediately — the amplifier may produce distorted output for
the first few seconds while it stabilizes. Caller should redraw display
after resume.

If WiFi was powered off, call `shelbourne_wifi_set(hw, 1)` before or
after resume. The WiFi connection takes ~10 seconds to re-establish.

## Display

128x100 pixel 8-bit grayscale OLED, driven over SPI. All operations
work on an in-memory framebuffer; nothing is sent to hardware until
`display_flush()`.

**Warning**: `display_flush()` writes 12,800 bytes over SPI (~5ms). Do
NOT call it between `audio_write` and `audio_wait` — it will cause DMA
underruns.

### `void shelbourne_display_clear(shelbourne_t *hw)`

Clear framebuffer to black.

### `void shelbourne_display_pixel(shelbourne_t *hw, int x, int y, uint8_t val)`

Set a single pixel (0=black, 0xFF=white). Out-of-bounds is ignored.

### `uint8_t *shelbourne_display_buffer(shelbourne_t *hw)`

Direct access to the framebuffer (128x100 bytes, row-major).
`buf[y * 128 + x]` is the pixel at (x, y). Returns NULL if display
is unavailable.

### `void shelbourne_display_flush(shelbourne_t *hw)`

Write framebuffer to the OLED. ~5ms.

## Keypad

10 physical buttons. The driver delivers separate press and release
events for each button. This lets you detect short presses, long
presses, and held-key repeats.

### Button constants

| Constant | Button |
|----------|--------|
| `SHELBOURNE_KEY_VOLUME_UP` | Volume + |
| `SHELBOURNE_KEY_VOLUME_DOWN` | Volume - |
| `SHELBOURNE_KEY_POWER` | Power |
| `SHELBOURNE_KEY_AUX` | AUX |
| `SHELBOURNE_KEY_PRESET_1` through `_6` | Presets 1-6 |

### `int shelbourne_keypad_poll(shelbourne_t *hw, shelbourne_key_event_t *events, int max_events)`

Non-blocking poll. Returns number of events (0 if none pending).
Each event has `.key` (which button) and `.pressed` (1=press, 0=release).

### `int shelbourne_keypad_fd(shelbourne_t *hw)`

Get the keypad file descriptor for use with `select()`/`poll()`. When
`select()` signals readability, call `shelbourne_keypad_poll()` to get
decoded events. Do not `read()` from the fd directly.

### Long press detection

The hardware does not generate repeat or long-press events. Detect
them by recording the press timestamp and checking elapsed time:

```c
struct timespec press_time;
int power_held = 0;

/* In your event handler: */
if (ev.key == SHELBOURNE_KEY_POWER) {
    if (ev.pressed) {
        power_held = 1;
        clock_gettime(CLOCK_MONOTONIC, &press_time);
    } else {
        if (power_held && elapsed_ms(&press_time) < 2000)
            short_press_action();
        power_held = 0;
    }
}

/* In your main loop (polled every ~10-50ms): */
if (power_held && elapsed_ms(&press_time) >= 2000) {
    long_press_action();
    power_held = 0;  /* don't re-trigger */
}
```

## WiFi

### `void shelbourne_wifi_set(shelbourne_t *hw, int on)`

Control the DM870 WiFi/Bluetooth module power. `on=1` powers on
(default after init), `on=0` powers off. After powering back on, the
WiFi connection takes ~10 seconds to re-establish.

## LED

### `void shelbourne_led_set(shelbourne_t *hw, shelbourne_led_state_t state)`

Set the status LED: `SHELBOURNE_LED_OFF`, `SHELBOURNE_LED_WHITE`, or
`SHELBOURNE_LED_YELLOW`.

## Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `SHELBOURNE_SAMPLE_RATE` | 48000 | Audio sample rate (Hz) |
| `SHELBOURNE_CHANNELS` | 2 | Stereo |
| `SHELBOURNE_BYTES_PER_SAMPLE` | 4 | 32-bit samples |
| `SHELBOURNE_FRAME_SIZE` | 8 | Bytes per stereo frame |
| `SHELBOURNE_FRAMES_PER_WRITE` | 512 | DMA transfer size |
| `SHELBOURNE_WRITE_SIZE` | 4096 | Bytes per DMA write |
| `SHELBOURNE_FB_WIDTH` | 128 | Display width (pixels) |
| `SHELBOURNE_FB_HEIGHT` | 100 | Display height (pixels) |
| `SHELBOURNE_FB_SIZE` | 12800 | Framebuffer size (bytes) |
