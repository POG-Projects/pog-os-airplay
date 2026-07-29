# 🔊 POG OS AirPlay

[![CI & Release](https://github.com/POG-Projects/pog-os-airplay/actions/workflows/ci-release.yml/badge.svg)](https://github.com/POG-Projects/pog-os-airplay/actions/workflows/ci-release.yml)
[![Latest release](https://img.shields.io/github/v/release/POG-Projects/pog-os-airplay)](https://github.com/POG-Projects/pog-os-airplay/releases/latest)

POG OS AirPlay turns an ESP32 with PSRAM into a customizable **AirPlay 2**
speaker. It combines an AirPlay receiver, a software DSP pipeline, speaker
protection, lighting effects, optional Bluetooth and Ethernet, Home Assistant
integration, and a responsive web interface.

This project is a fork of
[`rbouteiller/airplay-esp32`](https://github.com/rbouteiller/airplay-esp32),
which provides the core AirPlay 2 receiver. POG OS AirPlay currently targets
ESP-IDF 5.5.

## ✨ Features

### Audio

- **AirPlay 2** receiver with multi-room playback and PTP synchronization
- Optional legacy RAOP/AirPlay 1 compatibility
- Optional **Bluetooth A2DP sink** on classic ESP32 builds
- I2S output, with configurable S/PDIF and USB-audio backends
- Software volume ceiling
- **3-band tone equalizer** (bass, mids, and treble, ±12 dB)
- **4th-order Butterworth high-pass filter** (24 dB/oct, up to 400 Hz)
- Feed-forward anti-clipping limiter (~3 ms attack / ~150 ms release)
- Stereo, Mono (L+R), Left, and Right output modes
- Configurable amplifier standby/mute GPIO
- Playback controls through configurable physical GPIO buttons

### Interface and integrations

- Responsive dark web interface with desktop and mobile layouts
- Guided first-run setup and password-protected settings
- Automatic and manual **OTA firmware updates**
- Automatic rollback when a newly installed firmware cannot boot successfully
- Live logs and Wi-Fi throughput test
- Home Assistant integration through MQTT auto-discovery
- W5500 Ethernet support with automatic Wi-Fi fallback
- Optional OLED or ST7789 display for metadata and playback progress

### Lighting

- WS2812/WS2812B/SK6812 addressable LED strips using the RMT driver
- 12 music-reactive and ambient effects
- Adjustable brightness, color, animation speed, GPIO, and LED count
- Optional MAX7219 8×8 audio-reactive LED matrix

## 📦 Published Firmware

Every successful release publishes a manifest, SHA-256 checksums, an OTA image,
and a complete serial-flash image for each supported release target.

| Hardware | Release profile | OTA image | Full serial image |
|---|---|---|---|
| Generic ESP32-S3 DevKitC-1, N16R8 | `esp32s3` | `firmware-esp32s3.bin` | `merged-esp32s3.bin` |
| Seeed XIAO ESP32-S3, 8 MB flash | `xiao-s3` | `firmware-xiao-s3.bin` | `merged-xiao-s3.bin` |
| SqueezeAMP, ESP32, Bluetooth, 8 MB | `squeezeamp` | `firmware-squeezeamp-bt.bin` | `merged-squeezeamp-bt.bin` |
| Esparagus Audio Brick, ESP32, Bluetooth, 8 MB | `esparagus-audio-brick` | `firmware-esparagus-audio-brick-bt.bin` | `merged-esparagus-audio-brick-bt.bin` |

Download the latest files from the
[GitHub Releases page](https://github.com/POG-Projects/pog-os-airplay/releases/latest).

- `merged-*.bin` contains the bootloader, partition table, application, OTA
  metadata, and SPIFFS filesystem. Use it for a first installation over USB.
- `firmware-*.bin` contains only the application partition. Use it for OTA and
  only with the matching board profile.
- `manifest.json` describes every published board and its matching files.
- `SHA256SUMS` contains checksums for all release files.

Other development environments are available in `platformio.ini`, but they are
not necessarily published as release assets. The 4 MB SqueezeAMP build is
currently excluded from releases because the complete web filesystem does not
fit its SPIFFS partition.

## 🧰 Hardware Requirements

AirPlay 2 requires **PSRAM**. Supported choices include an ESP32-S3 with PSRAM
or a classic ESP32/WROVER-class board with PSRAM. ESP32-C3/C6 boards and
ESP32-S3 modules without PSRAM are not suitable.

### I2S DAC or amplifier

Common options include PCM5102A and MAX98357A modules.

| Signal | `esp32s3` DevKit | `xiao-s3` |
|---|---|---|
| BCLK | GPIO 11 | GPIO 1 (D0) |
| LRCLK / WS | GPIO 13 | GPIO 2 (D1) |
| DIN / DO | GPIO 12 | GPIO 4 (D3) |
| Amplifier SD/enable | Configurable | GPIO 5 (D4), configured at runtime |
| Addressable LED data | Configurable | GPIO 8 (D9) |

### Power supply

- Power the amplifier and LED strip from a dedicated external **5 V supply**.
  USB power may collapse under load and cause low volume or distortion.
- Add a decoupling capacitor of approximately 470 µF close to the amplifier's
  Vin pin.
- Use a common ground between the power supply, amplifier, and ESP.
- A 5 V LED strip may require a 74AHCT125 or similar level shifter for the ESP's
  3.3 V data signal, especially with long cables or strips.

## 🚀 First Installation

### Option 1: Release image

Download the matching `merged-*.bin` file and flash it at address `0x0`. For
example, for a XIAO ESP32-S3:

```bash
python -m esptool --chip esp32s3 write-flash 0x0 merged-xiao-s3.bin
```

Use `--chip esp32` for classic ESP32 boards.

### Option 2: Build with PlatformIO

```bash
git clone --recursive https://github.com/POG-Projects/pog-os-airplay.git
cd pog-os-airplay

# If the repository was cloned without --recursive:
git submodule update --init --recursive

# Build one target
pio run -e xiao-s3

# Flash the application, bootloader, and partitions
pio run -e xiao-s3 -t upload

# Flash the SPIFFS web filesystem separately
pio run -e xiao-s3 -t uploadfs
```

Do not combine `-t upload` and `-t uploadfs` in one command. Run them
separately to avoid the PlatformIO/ESP-IDF filesystem flashing interaction.

Common PlatformIO environments:

| Environment | Purpose |
|---|---|
| `esp32s3` | Generic 16 MB ESP32-S3 |
| `xiao-s3` | Seeed XIAO ESP32-S3 |
| `squeezeamp` / `squeezeamp-bt` | SqueezeAMP without/with Bluetooth |
| `esparagus-audio-brick` / `esparagus-audio-brick-bt` | Esparagus Audio Brick |
| `esparagus-audio-brick-s3` | ESP32-S3 Esparagus Audio Brick |
| `esparagus-louder` / `esparagus-louder-bt` | Louder Esparagus |
| `esparagus-louder-s3` | ESP32-S3 Louder Esparagus |
| `esp32wrover-dev` | Generic ESP32-WROVER development target |

### Build with ESP-IDF

ESP-IDF 5.5 or newer is recommended.

```bash
# Generic ESP32-S3
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3" \
  set-target esp32s3 build

# Flash application and SPIFFS, then open the serial monitor
idf.py -p /dev/ttyUSB0 flash monitor
```

For a XIAO ESP32-S3, include `sdkconfig.defaults.xiao` in
`SDKCONFIG_DEFAULTS`.

## 📶 First-Run Setup

After the first flash:

1. Join the **`ESP32-AirPlay-Setup`** Wi-Fi network.
2. Open `http://192.168.4.1`.
3. Choose the AirPlay device name and administrator password.
4. Select the home Wi-Fi network.
5. Wait for the device to reboot, then open its new IP address.

The web interface contains four main areas:

- **Playback** — now playing, volume, and equalizer
- **Effects** — addressable strip and LED matrix
- **Settings** — Wi-Fi, device, buttons, protection, amplifier, and MQTT
- **System** — device information, OTA, logs, speed test, and restart

## ⬆️ OTA Updates

### Automatic update

Open **System → Update** and select **Check for updates**. The interface:

1. reads the latest release metadata from GitHub;
2. compares it with the installed firmware version;
3. verifies that the release contains the exact asset for the device profile;
4. asks the ESP to download that fixed release asset over HTTPS;
5. validates and writes it to the inactive OTA partition;
6. reboots and automatically reloads the page.

The update endpoint is protected by the same administrator session used for
other sensitive settings. The firmware URL is selected by the device and cannot
be replaced with an arbitrary remote URL.

### Manual update

Expand **Manual installation**, select the matching `firmware-*.bin`, and
install it. The firmware rejects empty, oversized, malformed, or corrupt
images.

### Rollback and SPIFFS limitation

The firmware uses two application slots. A new image remains pending until
initialization completes; if it crashes before confirmation, the bootloader
rolls back to the previous slot.

OTA updates only the application partition. They do **not** replace SPIFFS. A
device that still contains the previously truncated web page must receive
`uploadfs` once over USB, or be reflashed with a current `merged-*.bin`. Normal
firmware updates can then be performed over OTA.

## 🗂️ Repository Layout

```text
main/                    application, audio, networking, AirPlay, and UI APIs
components/              boards, DACs, displays, resampler, and submodules
data/www/                web interface, live logs, EQ, and speed-test pages
data/bg/                 optional ST7789 background image
.github/workflows/       formatting, linting, builds, and release publication
sdkconfig.defaults.*     per-target ESP-IDF configuration
platformio.ini           PlatformIO environments
version.txt              firmware and release version
```

TAS57xx HybridFlow firmware can optionally be placed in
`data/hf/tas57xx_fw.bin`; create that directory when needed.

The default 8 MB partition layout contains NVS, OTA metadata, two 3 MiB
application slots, and a 1.875 MiB SPIFFS partition.

## 🔧 Optional Configuration

Hardware and feature options are available through:

```bash
idf.py menuconfig
```

Relevant menus cover board selection, I2S/S/PDIF/USB output, Bluetooth,
displays, Ethernet, AirPlay compatibility, GPIO assignments, and audio timing.
PlatformIO builds can use a local `user_platformio.ini` for project-specific
overrides.

## 📜 Credits & License

This project is based on
[`rbouteiller/airplay-esp32`](https://github.com/rbouteiller/airplay-esp32) by
Rémi Bouteiller. It also includes or depends on third-party projects such as
u8g2 and audio-resampler.

The project is distributed under the original **Non-Commercial License**. See
[`LICENSE`](LICENSE) and [`NOTICE`](NOTICE). Use, copying, modification, and
distribution are permitted for non-commercial purposes only. Contact the
upstream author for commercial use.

This personal DIY project is provided “as is,” without warranty. AirPlay is a
trademark of Apple Inc.; this project is neither affiliated with nor endorsed
by Apple.
