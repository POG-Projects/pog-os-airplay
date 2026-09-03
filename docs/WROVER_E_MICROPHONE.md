# WROVER-E and microphone bring-up

## Current support

`pio run -e wrover-e` builds POG AirPlay for an ESP32-WROVER-E with **4 MB
flash and PSRAM**. Flash size is not inferred from the module family: WROVER-E
also exists with other flash capacities. This profile explicitly uses 4 MB,
DIO at 40 MHz, the existing two-slot OTA layout, and size optimization.
It has its own OTA artifact and POG Home model identifier. Bluetooth and
displays are off; the legacy development profile is left unchanged.

`components/pogmic/` provides **local microphone diagnostics** and an exclusive
streaming interface used by the [POG AI voice profile](POG_AI_VOICE.md). It captures signed 24-bit I2S data in 32-bit stereo slots,
selects left or right, and measures mono PCM. It uses I2S1 with independent
BCLK/WS at 16 kHz, or as a slave of AirPlay's I2S0 with shared clocks at the
configured speaker rate (44.1 or 48 kHz). Shared mode uses 32-bit speaker slots
and expands the existing PCM16 output without changing its amplitude. No
recording is kept or sent over the network.
Microphone signal reception has been confirmed by the user on the wiring below;
full acoustic performance remains unverified.

The separate `wrover-e-voice` profile now implements authenticated POG AI
association, WebSocket v3, Opus capture/reply and manual listening controls.
See [POG AI voice](POG_AI_VOICE.md) for setup, privacy and validation limits.
Wake word and acoustic echo cancellation are not implemented.

## Connected board inspected on 2026-08-31

Read-only USB identification found an ESP32-D0WD-V3 revision 3.1, 4 MB flash,
with PSRAM initialization reported by the old firmware, `ESP32 Modular Base`.
A complete private 4 MB backup was taken outside this repository. The attached
microphone/amplifier models and wiring could not be identified over USB.
The old source tree describes camera/LED wiring; it is not evidence of the
current audio connections.

At the user's request, the normal WROVER-E AirPlay image **0.1.50** was then
flashed over USB on 2026-08-31. esptool verified the written data hash. Serial
boot verification confirmed 4 MB flash, 8 MB physical PSRAM (4 MB mapped into
the heap), a successful PSRAM memory test, the mounted compact SPIFFS and the
HTTP server on port 80. No crash was observed during the 22-second boot capture.
The device starts in `ESP32-AirPlay-Setup` at `192.168.4.1`; the new installation
has not yet been provisioned with home Wi-Fi credentials. AirPlay playback and
microphone capture have not been validated on the hardware.

The user subsequently identified shared I2S clocks **SCK/BCLK=32, WS=33**, with
**microphone SD=35** and **amplifier DIN=34**. GPIO34 is input-only on ESP32:
it cannot drive amplifier DIN. The user confirmed moving that wire to GPIO25
with power disconnected, then reconnecting the board.
The microphone model remains unidentified; L/R was resolved during the test below.

The original flashed reference mapping does not match these clocks. A local,
ignored `wrover-e-shared-test` profile prepares BCLK=32, WS=33, speaker DIN=25
and microphone SD=35. After the wiring confirmation, this profile was flashed
over USB on 2026-08-31. All five written images passed esptool hash verification.
Existing NVS settings were backed up privately and preserved during the flash.
The subsequent 25-second serial boot check showed the WROVER-E board identity,
successful PSRAM test, mounted SPIFFS and the HTTP server, with no crash.
The board still starts in `ESP32-AirPlay-Setup` at `192.168.4.1`; no home Wi-Fi
connection or speaker clock initialization was observed during that check.
The first user-run microphone test completed but showed -96/-96 dBFS on the
configured left slot. The user then reported that the microphone's L/R pin was
unconnected; its model remains unknown. The user subsequently confirmed
connecting the labelled L/R input to GND, then reported changing levels and
audio reception in the existing left-slot test. This validates microphone
signal reception according to the user's test; intelligibility, amplifier
playback, echo cancellation and POG AI transport have not been established.
The device was subsequently reachable as `bureau.local` on Wi-Fi.

Working connected-board wiring: **BCLK=32, WS=33, microphone SD=35,
amplifier DIN=25, microphone L/R=GND (left slot)**. The later two-slot diagnostic
and `wrover-e-voice` firmware were subsequently flashed to this same board on
2026-08-31, preserving NVS after a full private backup. Boot, PSRAM, Wi-Fi and
the updated interface were verified; the voice conversation still needs an
authenticated POG AI association and a real audio test (see
[voice validation](POG_AI_VOICE.md#validation)).
The generic `wrover-e` profile remains microphone-unconfigured.
The new `wrover-e-voice` release profile preserves this confirmed wiring and
has its own OTA identity; use it for the voice implementation. Installing a
generic WROVER-E image would restore the reference wiring.

## Audio wiring to confirm before microphone activation

The release's reference output pins are inherited from the generic WROVER
audio arrangement; they are **not** an assertion about the connected board:

| Signal | Reference GPIO |
|---|---:|
| Amplifier BCLK | 33 |
| Amplifier WS/LRCLK | 25 |
| Amplifier DIN | 32 |
| MCLK | disabled |
| Microphone BCLK / WS / DATA | all `-1` (unconfigured) |

Confirm the microphone type (I2S/PDM/analog), BCLK, WS, DATA and L/R selection,
and the amplifier type and connections. A camera left attached may drive pins
needed for audio. Never probe candidate clocks by driving unknown GPIOs.

After confirming an I2S microphone, set `CONFIG_POG_MIC_BCLK_GPIO`,
`CONFIG_POG_MIC_WS_GPIO`, `CONFIG_POG_MIC_DATA_GPIO` and optionally
`CONFIG_POG_MIC_RIGHT_SLOT` in the board's local SDK configuration. Do not
commit local `sdkconfig.*` files. `CONFIG_POG_MICROPHONE=y` builds the driver
on WROVER-E, but no GPIO is configured or listening started automatically.
For other ESP32/S3 profiles the feature defaults off.

For shared clocks, also enable `CONFIG_POG_MIC_SHARED_CLOCKS`. This mode is
currently limited to ESP32 with I2S output and Bluetooth A2DP disabled, so the
speaker rate stays fixed. The diagnostic refuses capture before I2S0 is
initialized (normally after Wi-Fi connects). I2S1 runs only as a **slave**,
and capture cleanup leaves speaker BCLK/WS intact. A temporary loss of the
speaker clock can end the test with a timeout. The local level test does not resample;
`pogvoice` performs the rate conversion for voice streaming.

The capture path rejects missing/duplicate pins, input-only clock pins,
flash/PSRAM/strapping/console pins and conflicts with known audio, LED, button
and amplifier settings. Shared mode allows only the exact speaker clock pair;
it still rejects a runtime LED/button/amp reservation on either clock and an
input-only speaker DIN. TFT, Ethernet and non-I2S-output configurations are
not yet supported by the diagnostic adapter. Do not change peripheral GPIO
settings during a measurement.

## Local test

After flashing confirmed wiring and configuring an administrator password,
open **System → Microphone · test local**. The test button captures for five
seconds and releases the receive channel. The UI displays current and maximum
RMS level in dBFS. The updated diagnostic measures **both left and right slots**
from the same capture, plus raw nonzero-word counts, so a silent configured
slot does not hide activity on the other slot. It does not switch the configured
channel automatically. Raw nonzero data below the -96 dBFS display floor is
distinguished from all-zero input; levels use the full signed 32-bit words.
Only numeric summaries are logged at test completion, never raw audio.
Silence, floating input and wrong channel selection cannot
reliably establish whether a physical microphone is present; compare readings
while speaking and while quiet.

The same operation is available through authenticated
`POST /api/microphone/test` (fixed five seconds). It is forbidden while the
administrator password is unset. Poll `GET /api/audio/stats` and inspect
`microphone`: `active`, `samples`, `rms_dbfs`, `peak_rms_dbfs`, and `error`.
`shared_clocks`, `clock_ready` and `sample_rate` describe the capture clock.
`left_rms_dbfs`, `right_rms_dbfs`, `left_peak_rms_dbfs`, `right_peak_rms_dbfs`,
`left_nonzero_words` and `right_nonzero_words` describe the two slots. Existing
`rms_dbfs` and `peak_rms_dbfs` still refer to the configured slot.
`202` means the test was scheduled; a driver failure is reported in `error`.
`satellite_ready` is true only with a paired voice profile and configured,
ready clocks; it does not indicate an active network connection. A GET/poll
never starts audio capture.

## Build and verify

Use a PlatformIO shell, without ESP-IDF's shell environment:

```sh
pio run -e wrover-e
pio run -e wrover-e -t buildfs
cc -std=c11 -Wall -Wextra -Werror -I components/pogmic/include \
  test_host/test_pogmic_pcm.c components/pogmic/pogmic_pcm.c -lm \
  -o /tmp/pog-airplay-mic-test
/tmp/pog-airplay-mic-test
```

The compact filesystem stages `data/www/` in the build directory. Both
PlatformIO and ESP-IDF release builds use the same staging script; `data/`
is not modified. Rebuild from a fresh target SDK configuration when changing
defaults, and check the generated `CONFIG_BOARD_WROVER_E=y` and flash size.

References: [Espressif WROVER-E datasheet](https://documentation.espressif.com/esp32-wrover-e_esp32-wrover-ie_datasheet_en.html)
and [ESP-IDF I2S controller modes](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/i2s.html).
The [ESP32 GPIO reference](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/gpio.html)
documents GPIO34–39 as input-only.
ESP32 simplex TX/RX clocks cannot be independent on the same controller;
independent microphone clocks require the second controller.
