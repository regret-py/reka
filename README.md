# REKA

**REKA** is a modular, low-latency audio mixer for Windows 10/11, built as a replacement for Voicemeeter. It has no
fixed number of strips or buses, a free-form routing matrix, a VST3 rack on every strip with crash-proof
sandboxing, drift-locked multi-device I/O, virtual audio endpoints, a fully themeable UI and a physical control
surface (`REKA.ino`).

```
REKA.exe ───── engine (reka_core, real-time, JUCE-free) ──── ASIO / WASAPI exclusive & shared / DirectSound
   │                                                    └─── secondary devices, virtual endpoints, per-app capture
   ├── REKA-PluginHost.exe ×N   (out-of-process scanning + one sandbox per plugin)
   └── USB serial / MIDI ──── REKA.ino on Pro Micro · Teensy 4.x · ESP32 (faders, buttons, WS2812B, SSD1306)
```

## Features

| Area | What you get |
|---|---|
| **Engine** | 32-bit float audio with 64-bit filter state, 44.1–192 kHz; any number of input strips and buses; per-strip polarity, stereo / mono / swap / L / R summing, 8-band parametric EQ, unlimited inserts, fader, pan/balance (3 pan laws), mute, solo-in-place, peak/RMS/LUFS meters; every change de-clicked |
| **Routing matrix** | any input → any combination of buses, each route with its own gain, pan, polarity, pre/post-fader tap and 8-band EQ; automatic plugin delay compensation (including latency-matched bypass) |
| **Devices** | one master device clocks the engine (ASIO, WASAPI exclusive/low-latency/shared, DirectSound, or a software clock); any number of extra devices at any sample rate through drift-corrected resampling (64-tap sinc, PI-locked to ±0.5 %); hot-plug recovery |
| **Virtual I/O** | `REKA Input N` / `REKA Output N` endpoints through the REKA driver interface, third-party cable detection, and **per-application capture** (WASAPI process loopback, no driver needed) |
| **VST3 host** | background, out-of-process scanning with blacklisting; in-process or **sandboxed** hosting (one `REKA-PluginHost.exe` per plugin, same-block shared-memory audio, dry pass-through on stall, automatic relaunch from the last state after a crash); plugin editor windows; drag & drop from the browser |
| **UI engine** | JSON themes (palette, gradients, frosted glass, metrics, fonts), Peak/RMS/VU/LUFS meters with configurable ballistics and LED segments, faders or knobs per strip, template-driven strip layout, dockable / hideable / resizable panels, live theme editor; Windows 11 Mica/Acrylic backdrop |
| **Profiles** | one `.reka` JSON file holds devices, virtual I/O, mixer, plugin states, control surface and UI; loading is a **hot reload** that keeps audio running; external edits reload live; autosave |
| **Control surface** | `REKA.ino`: 16 faders through a CD74HC4067 with two-speed EMA filtering, mute/solo/FX buttons through shared-select muxes with debouncing, WS2812B status LEDs, SSD1306 OLED (names, dB, meters, pickup arrows), COBS/CRC-8 binary protocol at 115200 baud, optional USB-MIDI; soft takeover in the app |
| **MIDI** | any MIDI controller: MIDI learn, soft takeover, LED/motor feedback |

## Status - what is verified and what is not

| Component | Verified here |
|---|---|
| `reka_core` engine, DSP, profiles, protocol, hardware bridge, sandbox IPC | 41 unit test cases, 315k assertions (DSP, EBU R128 loudness, SRC quality, drift lock, routing, PDC, hot reload, protocol fuzzing, simulated control surface, cross-thread sandbox IPC). Pass on Linux under ASan+UBSan and ThreadSanitizer |
| Plugin hosting | integration test against a purpose-built VST3: out-of-process scan, in-process and sandboxed hosting, latency, state, crash containment and relaunch |
| `REKA` / `REKA-PluginHost` | build warning-free on Linux; the app starts, loads profiles (with live sandboxed plugins), renders and shuts down cleanly under ASan+UBSan (headless Xvfb `--smoke-test`) |
| Windows-only code (WASAPI, process loopback, driver client, serial, MMCSS, DWM) | compiles and links warning-free with MinGW-w64; MSVC is built by CI. **Not yet run on a real Windows machine** |
| `REKA.ino` | builds warning-free for Pro Micro (87 % flash / 59 % RAM), Teensy 4.1 and ESP32; host-side simulation of the sketch. **Not yet run on physical hardware** |
| REKA Virtual Audio **kernel driver** | **not included**: a kernel-mode, Microsoft-signed component. The interface is specified and the user-mode side is implemented. Third-party cables and per-app capture work without it. See [docs/VIRTUAL_AUDIO_DRIVER.md](docs/VIRTUAL_AUDIO_DRIVER.md) |

## Building (Windows)

Requirements: Visual Studio 2022 (Desktop C++), CMake ≥ 3.22, Git. JUCE 8.0.12, nlohmann/json and doctest are fetched
automatically at pinned versions.

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release
build\src\app\REKA_artefacts\Release\REKA.exe
```

`REKA-PluginHost.exe` is copied next to `REKA.exe` automatically; ship both together.

| Option | Default | |
|---|---|---|
| `REKA_ENABLE_ASIO` + `REKA_ASIO_SDK_DIR` | OFF | ASIO support. Needs the Steinberg ASIO SDK (not redistributed: download it from Steinberg and accept its licence) |
| `REKA_BUILD_APP` | ON | OFF builds and tests only `reka_core` (no JUCE needed) |
| `REKA_BUILD_TESTS` | ON | doctest unit tests |
| `REKA_BUILD_INTEGRATION_TESTS` | OFF | test VST3 + plugin-hosting test |

Linux and macOS builds are supported for development: the engine, UI and plugin hosting run, and the Windows-only
virtual I/O reports itself as unsupported.

## Firmware

```bash
cd firmware && pio run -e promicro -t upload      # or: teensy41, esp32dev
```

or open `firmware/REKA/REKA.ino` in the Arduino IDE (libraries: U8g2, Adafruit NeoPixel, MIDIUSB on the Pro Micro).
Pins, channel count and features live in `firmware/REKA/RekaConfig.h`. Wiring, bill of materials and bring-up are
covered in [docs/HARDWARE_WIRING.md](docs/HARDWARE_WIRING.md). REKA finds the controller automatically
(Settings → Control surface).

## Documentation

| | |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | project tree, class hierarchy, **threading model**, signal flow, clock domains, sandbox, hot reload, verification matrix |
| [docs/PROFILE_FORMAT.md](docs/PROFILE_FORMAT.md) | the `.reka` file |
| [docs/THEMES.md](docs/THEMES.md) | theme and layout JSON, turning a mockup into a REKA design |
| [docs/HARDWARE_PROTOCOL.md](docs/HARDWARE_PROTOCOL.md) | serial protocol between REKA.exe and REKA.ino |
| [docs/HARDWARE_WIRING.md](docs/HARDWARE_WIRING.md) | building the control surface |
| [docs/VIRTUAL_AUDIO_DRIVER.md](docs/VIRTUAL_AUDIO_DRIVER.md) | virtual endpoints, cables, per-app capture, driver contract |

## Licensing

REKA's own code carries no licence yet: add one before distributing. REKA links **JUCE 8**, which is dual-licensed
(AGPLv3 or a commercial JUCE licence): a closed-source release of `REKA.exe` needs a commercial JUCE licence. VST3
hosting uses the VST3 SDK bundled with JUCE, and ASIO needs the Steinberg ASIO SDK; check both licences for your
distribution model.
