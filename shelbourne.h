/*
 * shelbourne.h — Audio hardware abstraction library
 *
 * Provides a clean C interface to the Shelbourne embedded audio platform:
 *   - Audio output (McASP/EDMA DMA driver)
 *   - AUX input (codec ADC → software loopback → speakers)
 *   - Codec (TLV320AIC3256 via sysfs)
 *   - Amplifier (GPIO power and mute)
 *   - OLED display (128x100 grayscale framebuffer)
 *   - Keypad (10 buttons via keypad driver)
 *   - Status LED (white/yellow/off)
 *
 * Typical usage:
 *
 *   shelbourne_t *hw = shelbourne_init();
 *   if (!hw) exit(1);
 *
 *   // Play audio (32-bit stereo interleaved @ 48kHz)
 *   int gain = shelbourne_volume_to_gain(50);
 *   while (have_audio) {
 *       // ... decode, resample, apply gain to fill samples[] ...
 *       shelbourne_audio_write(hw, samples, num_frames);  // non-blocking buffer
 *       shelbourne_keypad_poll(hw, events, 8);             // check buttons
 *       shelbourne_audio_wait(hw);                         // block until DMA ready
 *   }
 *   shelbourne_audio_drain(hw);
 *
 *   // AUX loopback (audio jack -> codec ADC -> software -> speakers)
 *   int gain = shelbourne_volume_to_gain(50);
 *   shelbourne_aux_enable(hw);
 *   while (aux_active) {
 *       shelbourne_aux_pump(hw, gain);       // read input, write output (~10ms)
 *       shelbourne_keypad_poll(hw, events, 8);
 *   }
 *   shelbourne_aux_disable(hw);
 *
 *   // Display (pixel-level API)
 *   shelbourne_display_clear(hw);
 *   uint8_t *fb = shelbourne_display_buffer(hw);
 *   fb[y * SHELBOURNE_FB_WIDTH + x] = 0xFF;  // set pixel
 *   shelbourne_display_flush(hw);  // slow (~5ms SPI), avoid during audio
 *
 *   // Standby / resume
 *   shelbourne_standby(hw);  // mute, power down amp, blank display
 *   // ... user presses power button ...
 *   shelbourne_resume(hw);   // power up amp, unmute (~3s)
 *
 *   shelbourne_shutdown(hw);
 *
 * Thread safety: None. All functions must be called from the same thread.
 *
 * Performance notes:
 *   - shelbourne_audio_write() copies into an internal buffer and returns
 *     immediately. shelbourne_audio_wait() blocks (via usleep) until all
 *     buffered 512-frame chunks have been written to DMA. This split lets
 *     the caller poll keypad or do other work between buffering and waiting.
 *   - shelbourne_display_flush() writes 12,800 bytes over SPI (~5ms).
 *     Do NOT call it between audio_write and audio_wait — it will cause
 *     DMA underruns. Call it between tracks, during pauses, or in standby.
 *   - shelbourne_keypad_poll() is non-blocking (O_NONBLOCK read). Safe to
 *     call anywhere.
 *   - shelbourne_led_set() opens/writes/closes a file each call (~100us).
 *     Safe in the audio loop but avoid calling it every frame.
 *   - shelbourne_aux_pump() blocks for ~10ms per call (one DMA buffer).
 *     Safe to interleave with keypad_poll().
 */

#ifndef SHELBOURNE_H
#define SHELBOURNE_H

#include <stdint.h>

/* ── Constants ────────────────────────────────────────────────────── */

/* Audio format (fixed by hardware) */
#define SHELBOURNE_SAMPLE_RATE      48000
#define SHELBOURNE_CHANNELS         2
#define SHELBOURNE_BYTES_PER_SAMPLE 4
#define SHELBOURNE_FRAME_SIZE       (SHELBOURNE_CHANNELS * SHELBOURNE_BYTES_PER_SAMPLE) /* 8 */
#define SHELBOURNE_FRAMES_PER_WRITE 512     /* hardware DMA transfer size */
#define SHELBOURNE_WRITE_SIZE       (SHELBOURNE_FRAMES_PER_WRITE * SHELBOURNE_FRAME_SIZE) /* 4096 */

/* Display */
#define SHELBOURNE_FB_WIDTH   128
#define SHELBOURNE_FB_HEIGHT  100
#define SHELBOURNE_FB_SIZE    (SHELBOURNE_FB_WIDTH * SHELBOURNE_FB_HEIGHT)

/* ── Opaque handle ────────────────────────────────────────────────── */

typedef struct shelbourne shelbourne_t;

/* ── Lifecycle ────────────────────────────────────────────────────── */

/*
 * Initialize all hardware. This:
 *   1. Stops competing audio processes
 *   2. Fixes the keypad scan timer (driver workaround)
 *   3. Loads codec firmware via sysfs (falls back to direct register writes)
 *   4. Powers on amplifier via GPIO (includes 3s stabilization delay)
 *   5. Clears txRunning flag in audio driver via /dev/kmem
 *   6. Opens audio device and starts McASP/EDMA
 *   7. Unmutes amplifier
 *   8. Opens OLED display and keypad (non-fatal if missing)
 *
 * Returns NULL on fatal error (audio device or codec failure).
 * Display/keypad/LED failures are non-fatal — the corresponding
 * functions become no-ops.
 *
 * Takes ~4 seconds (dominated by amp stabilization delay).
 */
shelbourne_t *shelbourne_init(void);

/*
 * Shut down hardware and free resources.
 * Mutes, clears display, turns off LED, closes all file descriptors.
 * Does NOT kill or resume any processes.
 */
void shelbourne_shutdown(shelbourne_t *hw);

/* ── Audio output ─────────────────────────────────────────────────── */

/*
 * Buffer audio samples for DMA output (non-blocking).
 *
 * samples: interleaved 32-bit signed stereo (L, R, L, R, ...)
 *          at 48000 Hz. Full 32-bit range.
 * frames:  number of stereo frames (each frame = 2 samples = 8 bytes).
 *          Any count is accepted — the library buffers internally.
 *
 * Returns immediately after copying into internal buffer.
 * Call shelbourne_audio_wait() to pace output and write to DMA.
 *
 * Returns 0 on success, -1 on error (buffer overflow).
 */
int shelbourne_audio_write(shelbourne_t *hw, const int32_t *samples, int frames);

/*
 * Flush buffered audio to DMA, blocking to pace at 48kHz.
 *
 * Writes all complete 512-frame chunks from the internal buffer to the
 * DMA device, sleeping between writes to maintain 48kHz playback rate.
 * One chunk = 10.667ms. Returns when the buffer has fewer than 512
 * frames remaining (i.e., the partial tail is kept for the next write).
 *
 * Typical loop:
 *   shelbourne_audio_write(hw, samples, n);  // buffer
 *   // ... poll keypad, check state ...
 *   shelbourne_audio_wait(hw);               // pace and flush to DMA
 *
 * Returns 0 on success, -1 on write error.
 */
int shelbourne_audio_wait(shelbourne_t *hw);

/*
 * Flush remaining buffered audio (pads with silence to 512-frame boundary).
 * Call at end of track to avoid truncating the last few milliseconds.
 * Returns 0 on success, -1 on write error.
 */
int shelbourne_audio_drain(shelbourne_t *hw);

/*
 * Reset audio buffer state and DMA pacing.
 * Call when switching tracks or streams — resets the write-pacing clock
 * so the next write starts a fresh timing cycle instead of trying to
 * "catch up" to the old one.
 */
void shelbourne_audio_reset(shelbourne_t *hw);

/* ── AUX input ────────────────────────────────────────────────────── */

/*
 * Enable AUX input mode.
 *
 * Starts receiving audio from the 3.5mm AUX input via the codec's ADC.
 * Audio does NOT flow automatically to the speakers — the caller must
 * call shelbourne_aux_pump() in a loop to transfer audio from the
 * input to the output.
 *
 * While AUX is active, shelbourne_audio_write/wait/drain are unavailable.
 *
 * Returns 0 on success, -1 on error.
 */
int shelbourne_aux_enable(shelbourne_t *hw);

/*
 * Pump one cycle of AUX audio (input -> output).
 *
 * Reads one DMA buffer (512 frames, ~10.7ms) from the AUX input and
 * writes it to the speaker output. Blocks until input data is available.
 *
 * gain: volume multiplier from shelbourne_volume_to_gain() (0-65536).
 *       65536 = unity gain (full volume), 0 = silence.
 *
 * Typical AUX loop:
 *   int gain = shelbourne_volume_to_gain(50);
 *   shelbourne_aux_enable(hw);
 *   while (aux_active) {
 *       shelbourne_aux_pump(hw, gain);
 *       shelbourne_keypad_poll(hw, events, 8);
 *   }
 *   shelbourne_aux_disable(hw);
 *
 * Returns 0 on success, -1 on read/write error.
 */
int shelbourne_aux_pump(shelbourne_t *hw, int gain);

/*
 * Disable AUX input and restore normal playback mode.
 * Stops RX, restarts TX for normal audio output.
 * Returns 0 on success, -1 on error.
 */
int shelbourne_aux_disable(shelbourne_t *hw);

/* ── Volume ───────────────────────────────────────────────────────── */

/*
 * Convert a volume level (0-100) to a fixed-point gain multiplier (0-65536).
 *
 * Uses a 50 dB logarithmic curve:
 *   volume 100 = 0 dB (unity gain, multiplier 65536)
 *   volume  50 = -25 dB (multiplier ~3686)
 *   volume   0 = mute (multiplier 0)
 *
 * Apply to 32-bit samples before passing to shelbourne_audio_write():
 *   out_sample = (int32_t)(((int64_t)in_sample * gain) >> 16);
 *
 * Pure function — no hardware access.
 */
int shelbourne_volume_to_gain(int volume);

/*
 * Hardware mute via GPIO (active low).
 * mute=1 silences output regardless of audio writes or AUX loopback.
 * mute=0 enables output.
 *
 * Use for instant silence (e.g., during mode switches).
 * For normal volume control, use software gain via shelbourne_volume_to_gain().
 */
void shelbourne_mute(shelbourne_t *hw, int mute);

/* ── Standby / Resume ─────────────────────────────────────────────── */

/*
 * Enter standby (low power state).
 *   - Mutes amplifier
 *   - Powers off amplifier and secondary power supply
 *   - Blanks display
 *   - Turns off LED
 *
 * Audio buffer state is preserved — resume from where you left off.
 * Keypad remains active (needed to detect the power button for wake).
 */
void shelbourne_standby(shelbourne_t *hw);

/*
 * Resume from standby.
 *   - Powers on amplifier and secondary power supply
 *   - Waits for stabilization (~3s)
 *   - Unmutes amplifier
 *   - Resets audio pacing (avoids catch-up burst)
 *
 * After resume, the caller can immediately start writing audio or
 * enable AUX. Display and LED are available but not automatically
 * restored — the caller should redraw.
 */
void shelbourne_resume(shelbourne_t *hw);

/* ── Display ──────────────────────────────────────────────────────── */

/*
 * The display is a 128x100 8-bit grayscale OLED driven over SPI.
 * All functions operate on an in-memory framebuffer (128x100 bytes,
 * row-major, 0x00=black, 0xFF=white). Nothing is sent to hardware
 * until shelbourne_display_flush().
 */

/* Clear the framebuffer to black. */
void shelbourne_display_clear(shelbourne_t *hw);

/*
 * Set a single pixel.
 * x: 0 to SHELBOURNE_FB_WIDTH-1, y: 0 to SHELBOURNE_FB_HEIGHT-1.
 * val: grayscale intensity (0=black, 0xFF=white).
 * Out-of-bounds coordinates are silently ignored.
 */
void shelbourne_display_pixel(shelbourne_t *hw, int x, int y, uint8_t val);

/*
 * Direct access to the framebuffer (128x100 uint8_t, row-major).
 * Pixel at (x, y) is buf[y * SHELBOURNE_FB_WIDTH + x].
 * Returns NULL if display is not available.
 */
uint8_t *shelbourne_display_buffer(shelbourne_t *hw);

/*
 * Flush the framebuffer to the OLED.
 * Writes 12,800 bytes over SPI — takes ~5ms.
 * WARNING: Do not call between shelbourne_audio_write() and
 * shelbourne_audio_wait() — the delay will cause DMA underruns.
 */
void shelbourne_display_flush(shelbourne_t *hw);

/* ── Keypad ───────────────────────────────────────────────────────── */

typedef enum {
    SHELBOURNE_KEY_VOLUME_UP,
    SHELBOURNE_KEY_VOLUME_DOWN,
    SHELBOURNE_KEY_POWER,
    SHELBOURNE_KEY_AUX,
    SHELBOURNE_KEY_PRESET_1,
    SHELBOURNE_KEY_PRESET_2,
    SHELBOURNE_KEY_PRESET_3,
    SHELBOURNE_KEY_PRESET_4,
    SHELBOURNE_KEY_PRESET_5,
    SHELBOURNE_KEY_PRESET_6,
    SHELBOURNE_KEY_UNKNOWN,
} shelbourne_key_t;

typedef struct {
    shelbourne_key_t key;
    int pressed;    /* 1 = press, 0 = release */
} shelbourne_key_event_t;

/*
 * Poll for keypad events (non-blocking).
 *
 * Reads all pending events from the keypad driver and decodes them
 * into named key events. Returns the number of events written to the
 * array (0 if none pending).
 *
 * Cost: one non-blocking read() syscall (~5us). Safe to call anywhere.
 *
 * events:     output array for decoded events
 * max_events: capacity of the events array
 *
 * Returns number of events (0 to max_events), or -1 if keypad unavailable.
 */
int shelbourne_keypad_poll(shelbourne_t *hw, shelbourne_key_event_t *events, int max_events);

/*
 * Get the keypad file descriptor for use with select()/poll().
 *
 * The fd is opened O_NONBLOCK, but the caller can use it in
 * select()/poll() to sleep until a button is pressed — avoiding
 * CPU-wasting spin loops (e.g., during standby waiting for power
 * button). When select() signals readability, call
 * shelbourne_keypad_poll() to retrieve the decoded events.
 *
 * The fd can also be included in a select() set alongside network
 * sockets, timers, etc. for a single-threaded event loop.
 *
 * Returns the fd (>= 0), or -1 if keypad is unavailable.
 * The fd is owned by the shelbourne_t — do not close it.
 */
int shelbourne_keypad_fd(shelbourne_t *hw);

/* ── LED ──────────────────────────────────────────────────────────── */

/*
 * The status LED supports white and yellow (and off).
 */

typedef enum {
    SHELBOURNE_LED_OFF,
    SHELBOURNE_LED_WHITE,
    SHELBOURNE_LED_YELLOW,
} shelbourne_led_state_t;

/*
 * Set the status LED state.
 * Each call opens/writes/closes the LED device file (~100us).
 */
void shelbourne_led_set(shelbourne_t *hw, shelbourne_led_state_t state);

#endif /* SHELBOURNE_H */
