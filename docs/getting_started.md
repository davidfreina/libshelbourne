# Getting Started

Build the library, demo, and test suite, prepare a device, and run them.

## Prerequisites

- Docker (for cross-compilation)
- A Bose SoundTouch 20 with SSH access (see [remote_access.md](remote_access.md))
- The device on the same network as your build machine

## 1. Clone and Build

```
git clone <repo-url> libshelbourne
cd libshelbourne
make
```

This builds a Docker image with the ARM cross-compiler (first run takes
~20 seconds, subsequent builds use the cached image) and produces:

- `build/demo` — Interactive hardware demo
- `build/shelbourne-test` — Automated test suite

Both are statically linked ARM binaries that run directly on the device.

## 2. Prepare the Device

If you haven't already enabled SSH, see [remote_access.md](remote_access.md).

Verify SSH access:

```
ssh root@<device-ip> "uname -a"
```

You should see something like `Linux Bose-SoundTouch 3.14.43+ ...`.

## 3. Deploy and Run

Deploy both binaries to the device:

```
make deploy DEVICE=<device-ip>
```

Or manually:

```
scp build/demo build/shelbourne-test root@<device-ip>:/tmp/
```

**Important**: The test suite and demo call
`shelbourne_stop_stock_processes()` before `shelbourne_init()`. This
freezes the stock process supervisor and kills stock audio processes,
which hold the audio device open. The stock processes will restart
after a reboot, or you can call `shelbourne_resume_stock_processes()`
to restart them without rebooting.

### Run the Test Suite

```
ssh root@<device-ip> /tmp/shelbourne-test
```

Expected output:

```
Shelbourne Hardware Test Suite
==============================

Volume curve:
  volume 0 = mute                      PASS
  volume 100 = unity                   PASS
  ...

Initializing hardware...
  hardware init                        PASS

Display:
  framebuffer available                PASS
  ...

Audio:
  play 0.5s tone (440 Hz, vol 15)      PASS
  ...

==============================
Results: 31 passed, 0 failed, 31 total
ALL TESTS PASSED
```

You should hear a brief quiet tone during the audio tests. The display
will flash a test pattern. The LED will cycle white → yellow → off.

### Run the Interactive Demo

```
ssh root@<device-ip> /tmp/demo
```

The display shows a splash screen with a grayscale gradient. Use the
physical buttons:

| Button | Action |
|--------|--------|
| Presets 1-6 | Play sine wave (C4, D4, E4, F4, G4, A4) |
| Same preset again | Stop (back to idle) |
| AUX | Switch to AUX input loopback |
| AUX again | Back to idle |
| Volume +/- | Adjust volume |
| Power | Suspend / resume |
| Ctrl+C (SSH) | Quit |

The LED indicates state: white = playing tone, yellow = AUX, off = idle.

## 4. Using the Library in Your Own Code

Create a C file that includes the library:

```c
#include "shelbourne.h"
#include <math.h>

int main(void) {
    shelbourne_stop_stock_processes();
    shelbourne_t *hw = shelbourne_init();
    if (!hw) return 1;

    // Play a 440 Hz tone for 1 second
    int gain = shelbourne_volume_to_gain(30);
    int32_t samples[512 * 2];
    for (int f = 0; f < 48000; f += 512) {
        for (int i = 0; i < 512; i++) {
            double t = (double)(f + i) / 48000.0;
            int32_t s = (int32_t)(sin(2.0 * 3.14159 * 440.0 * t) * 2147483647.0);
            s = (int32_t)(((int64_t)s * gain) >> 16);
            samples[i * 2] = s;
            samples[i * 2 + 1] = s;
        }
        shelbourne_audio_write(hw, samples, 512);
        shelbourne_audio_wait(hw);
    }
    shelbourne_audio_drain(hw);

    shelbourne_shutdown(hw);
    return 0;
}
```

Compile with the Docker toolchain:

```
docker run --rm -v "$PWD:/work" --platform linux/amd64 shelbourne-build \
  arm-linux-gnueabihf-gcc -static -O2 -Wall \
    -o /work/myapp /work/myapp.c /work/shelbourne.c -lm
```

Copy to the device and run:

```
scp myapp root@<device-ip>:/tmp/
ssh root@<device-ip> /tmp/myapp
```

## 5. After Testing

Reboot the device to restore the stock Bose software:

```
ssh root@<device-ip> reboot
```

Or power-cycle the device. The stock processes restart automatically
on boot.

## Troubleshooting

**"Hardware init failed"**: Another process is holding the audio device.
Reboot and try again, or manually kill competing processes:
```
kill -STOP $(pidof shepherdd)
killall -9 APServer BoseApp
```

**No sound**: Check that the amplifier power GPIO is active
(`cat /sys/class/gpio/gpio55/value` should be `1`) and mute is off
(`cat /sys/class/gpio/gpio49/value` should be `1`).

**Display not available**: Verify `/dev/fb0` exists. Some firmware
versions may not include the framebuffer driver.

**Codec init timeout**: The codec firmware may fail to load on the first
try. The library retries up to 3 times. If it consistently fails, the
fallback path writes registers directly.
