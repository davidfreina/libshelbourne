# libshelbourne

An interface to the hardware of Bose SoundTouch smart speakers, so you
can easily create your own software for them.  Tested on SoundTouch 20;
should also work on SoundTouch 10, SoundTouch 30, and SoundTouch
Portable.

## Library Capabilities

  * Play audio (48 kHz 32-bit stereo PCM; your app is responsible for
    decoding MP3, receiving internet radio, etc.)
  * AUX input loopback (3.5mm jack to speakers)
  * Update the 128x100 8-bit grayscale OLED display
  * Control the WiFi-symbol status LED (white, yellow, off)
  * Read the keypad (volume, power, AUX, presets 1-6)
  * Suspend and resume the amplifier to save energy when "off"
  * Includes demo program and a test suite

## The platform

The SoundTouch devices are ARM-powered Linux computers.  While it
would be possible to replace the operating system completely, the
approach I'm taking is to keep it in place.  That way, we can rely on
Bose's custom Linux drivers to interact with the specialized hardware.

You can install your own software (or the demo included with this
library) without modifying the stock firmware irreversibly.  Holding
the power button and the volume-down button for 10 seconds factory
resets the device.

## Getting started

See [docs/getting_started.md](docs/getting_started.md) for how to
build the library, demo, and test suite and run them on the device.
For SSH access to the device, see
[docs/remote_access.md](docs/remote_access.md).

## Auto-boot

See [docs/autoboot.md](docs/autoboot.md) for how to run your app on
startup, replacing the stock Bose software.

## API Documentation

See [docs/libshelbourne.md](docs/libshelbourne.md).
