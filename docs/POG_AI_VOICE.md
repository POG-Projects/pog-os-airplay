# POG AirPlay voice satellite

The `wrover-e-voice` profile adds a **push-to-talk or local wake-word** POG AI satellite
to the AirPlay speaker. POG AI transcribes and interprets the request and routes
house commands to POG Home. The firmware does not duplicate that intent logic.

## Hardware and build

This profile targets an ESP32-WROVER-E with 4 MB flash and PSRAM, with this
confirmed wiring. It must not be flashed to a differently wired board.

| Connection | ESP32 GPIO |
|---|---:|
| Microphone and amplifier BCLK / SCK | 32 |
| Microphone and amplifier WS | 33 |
| Microphone SD → ESP32 input | 35 |
| ESP32 output → amplifier DIN | 25 |
| Microphone L/R | GND (left slot) |

GPIO34–39 are input-only; amplifier DIN must not be connected to GPIO34.
I2S0 drives shared clocks with 32-bit stereo slots. I2S1 receives as a slave;
stopping microphone capture leaves the speaker clocks running. Bluetooth A2DP
is disabled so the clock rate remains fixed. The generic `wrover-e` profile
keeps its reference wiring and has no voice capability enabled.

Use PlatformIO, with no ESP-IDF environment in the same shell:

```sh
pio run -e wrover-e-voice
pio run -e wrover-e-voice -t buildfs
```

The profile has a separate OTA identity and release image,
`firmware-wrover-e-voice.bin`, to preserve the wiring on subsequent updates.
The main control panel is gzip-compressed and embedded in the application, so an application OTA
also updates the voice controls. Secondary pages remain in SPIFFS. PlatformIO
tracks the assembly `.incbin` HTML inputs through `scripts/embedded_web.py`;
CMake tracks them through `OBJECT_DEPENDS`. This prevents an incremental build
from silently retaining an older UI after an HTML-only edit.
Preserve NVS when flashing. Do not erase the full chip: Wi-Fi, administrator
password, Home association and voice credentials are stored there.

## Association and use

1. Set an administrator password on the speaker and log into its local UI.
2. In the POG AI administration, generate a temporary device enrolment code
   for the intended room/profile. This is separate from the POG Home association.
3. Open **Système → Voix · POG AI** on the speaker. Enter the POG AI
   API origin and the code, then choose **Associer l’enceinte**. The local
   cleartext consent appears only for an HTTP address and remains unchecked.
4. Choose **Parler à POG AI**. Wait for **Je t’écoute** before speaking.
   **Terminer ma demande** sends the captured request
   immediately; otherwise capture ends after 15 seconds, or as soon as POG AI
   starts its reply. **Annuler** stops the
   local capture/playback and sends an abort when connected.
5. The response plays through the amplifier. AirPlay audio is locally silenced
   and drained during a voice turn, then resumes at the sender's current point;
   the sender itself is not paused.

For the current home node the API origin is `http://192.168.129.50:8766`.
That deployment requires explicitly enabling the local cleartext option:
**the enrolment code, token and audio travel unencrypted on that LAN**.
Use only a trusted network. HTTPS/WSS with a certificate trusted by the ESP
certificate bundle is the default; the client does not accept self-signed
certificates, redirects, a different bootstrap WebSocket hostname, or a TLS
downgrade. Cleartext destinations must resolve to private/link-local IPv4.
Use the node's numeric address on a cleartext LAN to avoid DNS ambiguity.
Only URL origins are currently accepted, without paths, userinfo or query
strings. The bootstrap WebSocket may use a different port on the same host.

The token returned by `POST /api/v1/enroll/claim` is bound to the Wi-Fi MAC
(`Device-Id`), saved in NVS, and never returned by the speaker's status API.
The public `/xiaozhi/ota/` bootstrap only supplies the WebSocket address; its
empty token cannot replace enrolled credentials. Device kind is `speaker`.
**Oublier l'association** clears local credentials; revoke the device in POG AI
administration as well to invalidate the server credential.
Connection information, frame counters and unpairing are under **Connexion et
détails**. Unavailable conversation actions stay hidden during association;
once paired, only the actions relevant to the current state are shown.

## Behavior and limits

- Wake detection is off by default. With it off, each turn requires the
  authenticated **Parler** action. Enabling it is an authenticated, persistent
  choice: local detection resumes after reboot and after a reply. No voice
  network connection starts until a selected wake word or **Parler**.
- Uplink is mono PCM resampled from the shared clock to 16 kHz, encoded as
  Opus CELT (`LOWDELAY`, complexity 0) 60 ms frames at 24 kbit/s. Downlink accepts negotiated Opus mono at
  8/12/16/24/48 kHz and 10/20/40/60 ms, resampled to the speaker's fixed rate.
- WebSocket protocol v3 includes strict binary lengths and bounded assembly
  of fragmented messages. Audio queues, capture time and response waits are
  bounded. Disconnection ends the turn. Wake mode returns to local detection;
  it does not retry or retransmit the previous request.
- The reply gain is 30% multiplied by the user's master output ceiling;
  the existing EQ, channel selection and final limiter still apply.
- There is no acoustic echo cancellation or barge-in. `aec=false` is
  explicitly announced. The detector is paused during the entire conversation
  and for at least 1.5 seconds afterwards; music/TV may still cause false wakes.
- No audio file is saved by this firmware. Streaming buffers and the latest
  returned text are transient RAM data; text is cleared on the next turn or
  local unpair. POG AI's own processing/retention rules apply on the server.
  NVS is not encrypted by this development profile: protect physical access.
- A single audio worker reserves a 40 KB internal stack at boot on core 0,
  before Wi-Fi starts. Wake inference, pairing and conversations run sequentially
  on this worker and return to its blocking queue; they never delete/recreate
  the stack. Pairing also suspends wake capture. Microphone DMA draining runs
  on core 0 at higher priority. AirPlay decoding/playback remain on core 1.
  The microphone queue is 64 KB in PSRAM (about 740 ms); FreeRTOS control
  structures stay internal. PCM decode/conversion scratch and resampler work
  arrays use PSRAM explicitly (about 17 KB). FIR conversion uses at most 32 interpolated phases
  to avoid exhausting internal RAM with many small filter allocations.
- Reply playback waits for 240 ms of PCM before starting and consumes complete
  output blocks. Only the final tail is padded with silence. A real underrun
  triggers rebuffering and increments **Interruptions audio** in the details.
  This prevents fragmented arrivals from becoming alternating sound/silence.

Authenticated `POST /api/voice/action` accepts `enroll`, `start`, `finish`,
`cancel`, `forget`, and `wake`. Wake configuration requires boolean `enabled`
and integer `model` (0: Hey Jarvis, 1: Okay Nabu, 2: Alexa). Enrolment also needs `api_url`, `code` and the optional
boolean `allow_insecure_lan`. `202` acknowledges scheduling, not completion.
Poll `GET /api/audio/stats` → `voice` for state, errors, text and frame counts.
The microphone's `satellite_ready` means configured/paired with clocks ready,
not an established network connection. Local diagnostics cannot start during
a voice turn, and streaming cannot start during an existing microphone test.

## Local wake word and ARGB

In **Système → Voix · POG AI**, choose **Hey Jarvis**, **Okay Nabu** or
**Alexa**, then **Activer le réveil vocal**. Only the selected model runs.
“Hey Pog” needs its own trained model and is not advertised as supported.
Detection processes microphone PCM locally; it neither saves recordings nor
sends ambient audio to POG AI. After a wake, wait for **Je t’écoute** (or cyan
LEDs) before the command: this implementation does not retain command pre-roll.
A conversation still has the 15-second capture bound. **Couper le micro ·
désactiver le réveil** cancels a turn and disables the saved wake preference.
**Annuler** cancels only the current turn; the enabled detector resumes later.
A failed NVS save is reported and mutes RAM immediately, but cannot guarantee
that the previous saved preference changed: resolve the error before reboot.

The standalone `components/pogwake` component uses the bundled Apache-2.0
microWakeWord models (provenance and hashes under `models/`) with pinned
TFLite Micro / ESP-NN / speech-feature dependencies. Models are compressed at
build time, verified/decompressed by ESP32 ROM miniz into PSRAM one at a time,
and freed before the voice task starts. The microphone queue is 32 KB in PSRAM;
queue overflow stops detection with an error instead of silently dropping data.
No model is downloaded at runtime. The 4 MB partition layout is unchanged.
Wake configuration and triggering are serialized to prevent a pending detection
from starting a new turn after mute. OTA holds detection off and waits for audio
tasks to stop before flash writes; errors resume the prior saved mode.

An **already enabled and correctly wired** ARGB strip displays temporary voice
feedback: violet connection, cyan microphone level, amber waiting, green reply
level, and red for four seconds on error. Then its normal lamp/music/group
visual returns. Existing brightness and off settings are respected; firmware
does not guess a GPIO or enable an unconfigured strip. A local wake itself is
followed by connection feedback; idle wake mode leaves the usual lamp effect.

## Validation

Host tests cover PCM conversion/GPIO conflicts, v3 framing, fragmented WS
messages, endpoint trust constraints, and real FIR sample-rate conversion,
including passband level and alias rejection. Compile the existing resampler
separately because its inherited warnings differ from the new code's policy:

```sh
cc -std=c11 -fsanitize=address,undefined -c components/audio-resampler/resampler.c -o /tmp/pog-resampler.o
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I components/pogvoice/include -I components/audio-resampler \
  test_host/test_pogvoice.c components/pogvoice/pogvoice_protocol.c \
  components/pogvoice/pogvoice_resample.c /tmp/pog-resampler.o -lm -o /tmp/pog-voice-test
/tmp/pog-voice-test
```

Software verification on 2026-08-31 passed for `wrover-e-voice` (application
and SPIFFS), `wrover-e` and `xiao-s3`. The host microphone/voice tests passed
with AddressSanitizer and UndefinedBehaviorSanitizer. The local browser
simulation exercised association, HTTP consent, listening, finish, response
completion and cancellation. POG AI's existing gateway, enrolment and audio
suites also passed (106 tests); those test the server contract, not this ESP
firmware's runtime behavior.

The user confirmed microphone signal reception on the earlier local-test
firmware after connecting L/R to GND. On 2026-08-31 the voice profile was then
flashed to the same ESP32, MAC `00:70:07:19:ae:ac`, after a complete private
4 MB backup and a partition-layout check. All five written images passed
esptool hash verification; NVS was preserved. A 28-second serial capture
confirmed the expected application ELF hash (`c5ebf3e2e…`), successful PSRAM
test, Wi-Fi reconnection and AirPlay ready, with no boot errors or panic.
`bureau.local` served the new voice controls and still required the existing
administrator password. No microphone streaming was started by these checks.

Real Opus CPU/memory load, intelligibility, amplifier playback, enrolment and
a Home command round trip still require validation after association in the
authenticated speaker UI. Browser simulations, successful builds and a healthy
boot do not establish those results.

The revised voice UI was installed on the same board later on 2026-08-31
(application ELF `33255cc50…`). Application, OTA metadata and SPIFFS writes
passed hash verification, with a fresh private settings backup and no NVS
erase. After a healthy reboot, the HTML served by `bureau.local` matched the
updated source byte for byte, and administrator authentication was retained.
This UI pass did not enrol the device or start microphone streaming. Ready,
listening and error views were inspected with read-only local fixtures, with
mobile previews at 320 and 390 CSS pixels. The incremental-build regression
was reproduced and checked by changing only the HTML, rebuilding and verifying
the exact modified page in the application image before restoring the final UI.

After the user unlocked the speaker UI, the installed page was reloaded and a
real five-second local microphone test completed successfully: left-channel
peak RMS was -15.6 dBFS, with the right channel below -96 dBFS, matching L/R
tied to GND. The home node's public bootstrap also returned its expected
protocol-v3 gateway at `ws://192.168.129.50:8765`. The speaker remains unpaired;
creating its enrolment code requires a separate POG Auth approval in the POG
Web administration. No voice streaming or assistant reply was tested in this
check.

After POG Auth approval, real enrolment succeeded against the home node. The
speaker UI displayed **Prête à t’écouter**, and POG AI registered a `speaker`,
renamed **Bureau** with zone **Bureau**. The web administration discarded the
one-time POST enrolment code when it polled the code-free GET status; the code
was recovered from the authorised server console for this association. The
two affected POG Web screens have a local fix to retain the creation response
separately from status polling and clear it on close/expiry.

Opening the USB serial monitor caused a hardware reset and invalidated the
speaker's web login, despite requesting inactive DTR/RTS before opening it.
The monitor was closed. The boot log showed a healthy restart, without a panic;
do not reopen that port while relying on a live web session. The first voice
start action did not execute because the UI was locked again. A successful
association therefore still does not establish microphone streaming or an
audible assistant response.

The subsequent unlocked runtime test preserved the association and reached
POG AI's authenticated protocol-v3 session. It failed before sending audio:
I2S DMA allocation returned `ESP_ERR_NO_MEM`. FreeRTOS's default stream-buffer
allocator had placed the 8 KB microphone queue in internal RAM. Voice queues
now use the IDF capability-aware allocation/deletion APIs with PSRAM storage;
task stacks and the driver's DMA buffers remain internal. Capture allocation
errors are distinguished in the UI, and completion logs report internal DMA
free/largest-block and task stack headroom without audio or credentials.

The corrected `wrover-e-voice` application passed its build and the existing
ASan/UBSan protocol/resampling tests. Authenticated network OTA transferred
1,829,888 bytes to the inactive slot: the device logged **SHA-256 verified OK**
and **OTA update successful (buffered)**. Candidate application ELF SHA-256 is
`380dca8c4ddbb7d77410ba456f1b33463badcc7417377349fc192fa0261811d5`.
No serial port was opened for this update. After the update, the device became
unreachable and its USB serial port also disappeared from the Mac; power and
the next boot must be checked before declaring the new image or capture
functional. Microphone upload, reply playback and a Home command remain
unverified.

After reconnecting power, the DMA-fix image booted from OTA slot 1 and retained
both associations. Starting a conversation then caused a panic with a
corrupted backtrace. A serial capture confirmed two failures with ELF
`380dca8c4…`. Stack exhaustion was suspected, but the corrupted backtraces
did not establish its cause. Espressif's audio-codec README recommends about
40 KB of stack for encoders, compared with the voice task's original 16 KB.
The encoder now explicitly uses Opus's
restricted-lowdelay/CELT mode, and its 2.5 KB packet scratch buffers live in the
PSRAM connection context rather than on the task stack. Stacks remain in
internal RAM; they must not be moved to PSRAM on this board.

This second correction passed the WROVER voice and XIAO-S3 builds and was
installed over authenticated OTA (1,830,000 bytes, SHA-256
`e2cf159dafee96dcdf870a4df3f6af96ca8210675c6727e3a7e9833a2358bdc0`).
Serial boot output confirmed ELF `b9915d473…` and **AirPlay ready** without
another panic during startup. A subsequent real voice start still rebooted
the board; the earlier timed serial capture had already expired, so it did
not capture that failure. CELT alone did not resolve the runtime problem.

A third correction reserves a 40 KB internal task stack, keeps FreeRTOS queue
and stream-buffer control structures in internal RAM, and places only the
8 KB microphone stream payload in PSRAM. Phase logs report internal free RAM,
largest free block and stack headroom without logging audio or credentials.
The WROVER voice and XIAO-S3 builds passed. Authenticated OTA installed
1,831,280 bytes with SHA-256
`560eedd78ef40003a99d5e365f9cb1127cb6a56599cbdbc8b75686c83ba1f491`;
the continuously open USB capture confirmed ELF `1767b06c3…` and
**AirPlay ready**. The next start reached the authenticated gateway and ended
without rebooting, but failed to start the microphone task. Internal RAM was
85 KB before the 40 KB voice task and 33 KB after WebSocket setup. Two
160-phase FIR banks then preferred internal RAM for hundreds of small
allocations. The resamplers now use at most 32 phases with interpolation,
retaining the 32-tap lowpass. ASan/UBSan checks of passband level, alias
rejection and chunk-size consistency still passed.

The 32-phase image (`f2b09b1b9…`) captured live microphone data and uploaded
18 frames without a panic, then hit the 8 KB microphone queue limit. With a
64 KB PSRAM queue, the next image (`d19b0ba60…`) uploaded 395 frames before the
backlog filled after about eight seconds. CELT encoding averaged 8.5 ms and
WebSocket sending 6.7 ms per 20 ms frame; total processing took about 22 ms.
Increasing the queue alone therefore does not fix sustained throughput.
Stack headroom after encoding remained about 20 KB. The next candidate uses
Opus VOIP mode with the 40 KB internal stack and retains timing diagnostics.
End-to-end voice and speaker reply remain unverified at this stage.

Further tests found VOIP slower than CELT on this board. Pinning voice/capture
to core 0 also removes accidental contention with AirPlay's core-1 decoder.
CELT uplink packets were changed to 60 ms to amortize network scheduling. The
`ef5d334ac…` image then completed a full 15-second capture: 661,600 microphone
samples and 251 packets, confirmed as 15.1 seconds of audio by the gateway.
The backlog stayed around 3,200–3,520 bytes at six/twelve seconds instead of
growing to overflow. Stack headroom remained 17,808 bytes. A silence-only
turn still waits until the existing response timeout if the server sends no
transcription or reply.

The gateway can start TTS before the device's 15-second limit. The firmware
now stops and joins microphone capture before accepting that response, drops
the unsent microphone tail, and keeps the mic off during playback. Three
subsequent spoken turns completed without firmware errors; one sent 80 uplink
packets and decoded 54 reply packets. The UI returned to ready with the
assistant's answer. The user confirmed speech playback but reported crackling.
The following `c2e0661c…` image adds a 240 ms playout prebuffer, complete-block
reads, final-tail draining, and underrun counters. Host tests cover fragmented
arrivals, rebuffering and replies shorter than the prebuffer. Both WROVER voice
and XIAO-S3 builds passed; sound quality still needs confirmation on this image.

Separately, one test ended when the home node's POG AI container was OOM-killed
at its 6 GiB limit (`OOMKilled=true`, exit 137). Swarm restarted it automatically;
the ESP did not reset. This server memory failure is not resolved by these
firmware changes and remains a deployment/runtime issue to investigate.

The next user test reported a truncated spoken answer. With `c2e0661c…`, the
speaker received 51 reply packets and played 44,982 samples (51 × 882), then
returned to ready. All received PCM was played; one underrun was recorded.
The server transport in `pogvox/hub/pipeline/transport.py` was sending TTS
controls from `process_frame`, ahead of its background paced audio queue,
and sending `tts stop` before flushing the serializer's tail. The speaker
therefore closed a reply while audio was still waiting on the server.

The server correction sends controls from the media sender's ordered
`push_frame` hook and flushes remaining encoded audio before `tts stop`.
Regression tests run the real Pipecat sender with two 2,005 ms replies and
10/20 ms PCM chunks, decode all 101 Opus packets per reply, and require every
packet to arrive between start and stop. They failed before the change and
passed afterwards. The 91 focused pipeline/transport/serializer/gateway/text
tests passed, as did the same sender check in an isolated container based on
the home node's image. Only the transport file is included in the local test
image `pogai:reply-order-fix-20260831`; unrelated workspace changes are excluded.
The previous image, service settings and a consistent SQLite backup are kept
on the node under `/var/lib/pog/deploy-backup-pogai-reply-order-20260831`.
The new container was healthy with HTTP 200, its installed transport hash
matched the tested source, and all service settings other than the image
were unchanged. This is a local test deployment, not a channel release.

Two subsequent user-triggered turns completed on the physical speaker:
counting from one to ten (150 received packets, 132,300 played samples,
two underruns) and naming Belgium's capital (108 packets, 95,256 samples,
one underrun). Both counts equal packets × 882 output samples and neither
turn rebooted the ESP. Confirmation by listening is still needed; the remaining
playout underruns and the separate server memory failure are not resolved
by correcting the server's stop ordering.

Local wake/ARGB integration was installed on Bureau later on 2026-08-31
(ELF `a089f8e26…`, application 1,932,336 bytes). WROVER voice and XIAO-S3
builds, host voice/resampling, microphone and ARGB checks passed. Compressed
pages and all three packed models were verified against their source bytes.
Wi-Fi and both associations survived application-only OTA. The user confirmed
four LEDs wired to GPIO12; the strip was enabled with count 4 and the existing
128/255 brightness. GPIO12 is a boot strapping pin: avoid external pull-ups
at reset, and use a non-strapping pin if the strip affects boot.

The local Jarvis detector ran for more than two minutes before a real wake.
A silence/no-reply turn reached its bounded timeout and rearmed. A second
wake uploaded 70 packets, received 146 reply packets, played 128,772 samples
(146 × 882), then rearmed in about 1.7 seconds without a reboot. Two playout
underruns remained. These logs establish capture, reply drain and rearming,
not subjective sound quality or the physically observed LED colors.

The final application (`749d7ddc7…`, 1,932,352 bytes) also treats an explicit
mute as a normal microphone stop rather than an I2S error. It passed both
board builds and was installed over authenticated OTA while local detection
was active. OTA stopped the detector before writing. The reboot restored
Hey Jarvis detection and all four GPIO12 LEDs without opening the web UI;
another real wake started capture while the page was still locked. Muting
that active turn stopped capture with `ESP_OK`, disabled wake and returned
the UI to ready. No GPIO wiring or partition layout was changed by OTA.

Okay Nabu and Alexa both initialized and ran on the board. Alexa also produced
a real wake event; not every trial received a server reply, so this does not
establish end-to-end speech success for all three words. One later voice
connection ended unexpectedly while the server container remained running
(`OOMKilled=false`); it was not an ESP reboot. These occasional connection
failures and remaining playout underruns are separate from local detection.

The user selected **Hey Pog** as the intended satellite wake phrase. There is
no trained Hey Pog model in this firmware yet: Hey Jarvis remains the active
word on Bureau. A custom model must be trained and evaluated before adding
the phrase; renaming the existing model would not change what it recognizes.

### Reusable audio stack, 2026-08-31

The subsequent "Mémoire insuffisante" report was reproduced in device logs:
77,843 bytes of internal heap remained, but its largest free block was only
38,912 bytes when a new 40,960-byte voice stack was requested. Earlier turns
had started with a 45,056-byte contiguous block. This is internal heap
fragmentation, separate from the previously observed server OOM.

`pogvoice_worker` now reserves that stack once at boot and uses a static task,
static queue control and one pending job. Both wake and conversation jobs
release their transient resources and return normally, so a subsequent turn
needs no new large contiguous stack. The same worker handles enrolment;
the existing busy gates and nonblocking queue reject overlapping requests.
It blocks without capture or network activity when idle. DMA, kernel controls
and the stack stay in internal RAM; CPU-only PCM scratch uses PSRAM explicitly.

`test_host/test_pogvoice_worker.c` runs the production dispatcher with a fake
fragmented heap and deterministic RTOS queue. It checks allocation-failure
cleanup and retry, idempotent initialization, queue-full rejection, job order
and priorities, then 2,000 sequential jobs without another stack allocation
even when the largest free block has fallen to 10 KB. Compile instructions
are in the test header. This host test does not measure hardware scheduling,
I2S allocations or wake/Opus latency; those require device validation.

The corrected image (ELF `99b9a7c22…`, application 1,932,992 bytes) passed both
WROVER voice and XIAO-S3 builds and was installed by authenticated OTA on
Bureau. The reboot restored local wake, associations and LED settings. One
real wake completed a spoken reply (301 received packets, 265,482 played
samples = 301 × 882), then rearmed. Three subsequent manual capture/stop or
cancel cycles also started successfully and returned to local wake. The
largest internal free block at those starts was only 9,728–10,752 bytes;
the 40,960-byte reserved stack no longer had to fit in that fragmented heap.
Stack headroom remained 17,756 bytes, and the internal heap returned to about
35 KB between jobs. No ESP reboot or memory error occurred in these four
cycles. This is a short hardware check, not a long-duration soak test.

Internal DMA headroom is still tight (about 5–6 KB during capture), and the
full reply recorded five playout underruns. The memory correction does not
establish interruption-free audio; Wi-Fi was also weak during this test.
No partition, wiring, model label or server service was changed by this fix.

### Reply resampler and Hey Pog candidate, 2026-08-31

Reply timing instrumentation separated Opus decode, sample-rate conversion plus
queue write, and binary-frame arrival gaps. On the original floating-point FIR,
regular 20 ms reply packets needed about 11.8 ms to decode and 12.2–12.6 ms to
convert/queue, so processing could not sustain real time. A separate reply also
contained a 2.2-second network gap; firmware DSP cannot conceal a gap that large.
The voice resampler now uses a 32-tap, 33-phase Q15 polyphase FIR with linear
phase interpolation and persistent history. Host checks still cover passband,
alias rejection, block-boundary invariance and all supported rate conversions.
Both WROVER voice and XIAO-S3 builds pass. The optimized image was installed;
its final per-filter CPU measurement and subjective sound check remain pending.

The reproducible `scripts/hey_pog/` workflow synthesizes French positives and
near-miss negatives with Piper's 125-speaker MLS model plus nine macOS French
voices. It splits by speaker, preserves source/model hashes, varies room/noise,
gain, rate and phrase position, and exports an integer streaming TFLite model.
Generated corpora, environments and rejected candidates stay outside Git.

Several real candidates were trained, quantized and evaluated. The latest is
39,840 bytes and recognizes held-out clear macOS voices well, but remains
unacceptable across all held-out Piper voices and streaming near misses. It is
marked rejected in its private metrics and is neither embedded nor offered in
the UI. The exporter now reproduces the firmware's rolling five-score detector;
both held-out splits require at least 85% recall and no more than 1% synthetic
false accepts before a candidate can pass. Physical room and ambient-hour tests
remain additional gates after that synthetic gate.

### Opt-in device corpus and fixed-point image, 2026-09-01

The fixed-point reply path plus exact timing instrumentation was rebuilt and
installed on Bureau by serial flash (ELF SHA-256 `94ffd31d9e7c…`, application
1,930,831 bytes). It retained the device password, Wi-Fi, POG Home/AI
associations and GPIO settings. The authenticated system endpoint reports the
expected `wrover-e-voice` environment, about 2.47 MB free heap, and Wi-Fi RSSI
of -85 dBm. That weak link is consistent with the independently measured
2.2-second reply gap; DSP optimization cannot hide missing network packets.
One post-install spoken turn is still needed for the new `resample_cpu_us`
measurement and subjective full-answer check.

Because the synthetic Hey Pog candidates did not pass the false-wake gate, the
firmware now has an authenticated, intentionally UI-less
`POST /api/microphone/sample` training endpoint. It captures exactly two
seconds into PSRAM, returns mono 16-bit WAV with `Cache-Control: no-store`, and
does nothing without an authenticated POST. An unauthenticated physical-device
check returned 401 without opening the microphone. The accompanying
`collect_device_samples.py` requires a separate Enter key for every clip,
keeps password/token in memory only, and saves clips on the operator's Mac.
`add_device_samples.py` keeps distinct utterances across train, validation and
test sets and refuses an accidental second merge into the same corpus.

The operator subsequently authorized and completed 30 “Hey Pog” captures and
30 prompted near misses. All 60 files are mono 16-bit, two seconds long, have
useful level and no clipping, and remain outside Git on the operator's Mac. A
local Whisper pass confirmed that the original captures contain the prompted
speech; no audio was uploaded. The first energy-only trimming attempt was
discarded after it demonstrably removed “Hey” or “Pog” from some clips.

The corrected corpus keeps the complete bounded captures and computes frontend
history before selecting the two-second feature window, matching continuous
device operation. The training exporter also pins the documented 0–26 feature
range during int8 calibration and exposes the negative-class penalty. Multiple
40- and 56-channel streaming and full-window candidates were evaluated after
these corrections. None met the per-source gate: the best strict-threshold
recall remained well below 85%, while thresholds with useful recall admitted
too many deliberately confusable phrases. Those candidates remain rejected and
outside Git. Bureau therefore still uses the proven Hey Jarvis model; no model
was relabelled or flashed as Hey Pog.

The capture additions pass WROVER voice and XIAO-S3 builds. The WROVER image
uses 83,256 bytes of static RAM and 1,930,831 of its 1,966,080-byte application
partition. A future accepted Hey Pog model will replace the compressed
30,625-byte Hey Jarvis model rather than add a fourth model; the rejected
candidate compresses to 20,006 bytes, so replacement also recovers flash.
