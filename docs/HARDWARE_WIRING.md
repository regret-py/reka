# REKA control surface — hardware wiring guide

This guide covers building the physical REKA surface: faders, mute/solo/FX buttons, one RGB LED per channel and
a small OLED, driven by a Pro Micro, Teensy 4.1 or ESP32. It goes with the firmware in `firmware/REKA/`.
All pin numbers and tunables live in `firmware/REKA/RekaConfig.h`. The wire protocol is defined in
`firmware/REKA/RekaProtocol.h`.

- [1. Overview](#1-overview)
- [2. Bill of materials](#2-bill-of-materials)
- [3. Pin assignments](#3-pin-assignments)
- [4. CD74HC4067 multiplexers](#4-cd74hc4067-multiplexers)
- [5. Faders](#5-faders)
- [6. Buttons](#6-buttons)
- [7. WS2812B LEDs](#7-ws2812b-leds)
- [8. SSD1306 OLED](#8-ssd1306-oled)
- [9. Building and flashing](#9-building-and-flashing)
- [10. On-device behaviour](#10-on-device-behaviour)
- [11. USB-MIDI mode](#11-usb-midi-mode)
- [12. Notes for the host application](#12-notes-for-the-host-application)
- [13. Bring-up checklist](#13-bring-up-checklist)
- [14. Troubleshooting](#14-troubleshooting)

---

## 1. Overview

Each channel strip has one linear fader, up to three momentary buttons (MUTE, SOLO, FX bypass) and one WS2812B
LED. `NUM_CHANNELS` (default 8, maximum 16) sets how many strips are scanned.

Four CD74HC4067 16-channel multiplexers share the same four select lines S0..S3:

| Mux | Mux pins Cn | Mux SIG goes to |
|-----|-------------|-----------------|
| FADER mux | fader wipers (analog) | one ADC pin |
| MUTE mux | mute buttons to GND | one digital pin, `INPUT_PULLUP` |
| SOLO mux | solo buttons to GND | one digital pin, `INPUT_PULLUP` |
| FX mux | FX-bypass buttons to GND | one digital pin, `INPUT_PULLUP` |

For every select step n the firmware reads fader n and the three buttons of channel n. The button muxes are
optional: set a group's SIG pin to `REKA_PIN_NONE` (255) to drop it, or set `REKA_ENABLE_BUTTONS 0` to drop all
three.

```
                          +-----------------------------+
   USB (CDC serial        |        MCU                  |
   [+ USB-MIDI]) ---------+ USB                         |
                          |                             |        +------------------+
                          | S0 S1 S2 S3 ----------------+---+--->| CD74HC4067 FADER |<-- C0..C15: fader wipers
                          |                             |   |    |  SIG --------------> ADC pin (FADER_SIG)
                          | FADER_SIG (ADC) <-----------+   |    +------------------+
                          |                             |   +--->| CD74HC4067 MUTE  |<-- C0..C15: buttons to GND
                          | MUTE_SIG  (INPUT_PULLUP) <--+   |    |  SIG --------------> MUTE_SIG
                          |                             |   +--->| CD74HC4067 SOLO  |<-- C0..C15: buttons to GND
                          | SOLO_SIG  (INPUT_PULLUP) <--+   |    |  SIG --------------> SOLO_SIG
                          |                             |   +--->| CD74HC4067 FX    |<-- C0..C15: buttons to GND
                          | FX_SIG    (INPUT_PULLUP) <--+        |  SIG --------------> FX_SIG
                          |                             |        +------------------+
                          | LED_DATA ---[330R]--[level shifter on 3.3 V MCUs]--> DIN  WS2812B #1 -> #2 -> ... -> #N
                          |                             |                       5V ==+== 1000 uF ==+== GND
                          | SDA / SCL ------------------+----------------------> SSD1306 128x64 (I2C 0x3C)
                          +-----------------------------+
   All mux EN pins -> GND. All grounds common. Mux VCC = fader VCC = MCU logic voltage (5 V Pro Micro, 3.3 V Teensy/ESP32).
```

## 2. Bill of materials

Quantities are for N channels (N = `NUM_CHANNELS`).

| Qty | Part | Notes |
|-----|------|-------|
| 1 | MCU: SparkFun **Pro Micro 5 V/16 MHz** (ATmega32U4), **Teensy 4.1**, or **ESP32 DevKit** (WROOM-32) | Pro Micro and Teensy have native USB (serial + optional USB-MIDI). The ESP32 DevKit uses a USB-UART bridge (serial only). |
| 1 | CD74HC4067 breakout (fader mux) | 2–6 V supply. Any "16-channel analog multiplexer" breakout with S0..S3/SIG/EN works. |
| 0–3 | CD74HC4067 breakout (button muxes) | One each for MUTE, SOLO and FX, whichever you build. |
| N | Linear slide potentiometer, **10 kΩ, linear taper ("B10K")** | 45, 60 or 100 mm travel. Not audio/log taper. |
| up to 3N | Momentary push button, normally open | Mute/solo/FX. |
| N | WS2812B LED (strip segment, 5050 module or "NeoPixel" breakout) | One per channel, chained. |
| 1 | SSD1306 128×64 OLED, I2C (0.96") | 1.3" modules often use the **SH1106** controller instead: see §8. |
| 1 | 330 Ω resistor | Series resistor in the LED data line, close to the first LED. |
| 1 | 1000 µF / ≥ 6.3 V electrolytic capacitor | Across 5 V/GND at the LED chain input. |
| 4–5 | 100 nF ceramic capacitor | Decoupling: one at each mux VCC, one at the OLED. |
| 1 | 74AHCT125 (or SN74AHCT1G125 / 74HCT245) | **Teensy and ESP32 only**: 3.3 V → 5 V level shifter for the LED data line. |
| 0–3 | 10 kΩ resistor | Optional external pull-ups on the button mux SIG lines (long cables). |
| 0–3N | 10 kΩ + 100 nF | Optional per-button hardware debounce for noisy switches (§6). |
| — | Wire, headers, enclosure, USB cable | Use a data-capable USB cable. |

## 3. Pin assignments

Defaults from `RekaConfig.h`. Override any of them there, or with `-D` build flags.

| Function | Pro Micro (ATmega32U4) | Teensy 4.1 | ESP32 DevKit |
|----------|------------------------|------------|--------------|
| Mux S0 | D4 | 2 | GPIO18 |
| Mux S1 | D5 | 3 | GPIO19 |
| Mux S2 | D6 | 4 | GPIO23 |
| Mux S3 | D7 | 5 | GPIO13 |
| Fader mux SIG (analog) | A0 (D18) | A0 (14) | GPIO36 (VP, ADC1_CH0) |
| MUTE mux SIG | D8 | 6 | GPIO25 |
| SOLO mux SIG | D9 | 7 | GPIO26 |
| FX mux SIG | D10 | 8 | GPIO27 |
| WS2812B data | D16 | 9 | GPIO4 |
| I2C SDA | D2 (fixed) | 18 (fixed) | GPIO21 |
| I2C SCL | D3 (fixed) | 19 (fixed) | GPIO22 |
| Logic / analog supply | VCC = 5 V | 3.3 V | 3V3 = 3.3 V |
| LED supply (5 V) | VCC (USB 5 V) or external 5 V | VIN (USB 5 V) or external 5 V | 5V/VIN pin or external 5 V |

Board-specific rules:

- **Teensy 4.1:** inputs are **not 5 V tolerant**. Power the muxes, the faders and the OLED from **3.3 V**.
- **ESP32:**
  - The fader SIG must be an **ADC1** pin (GPIO32–39). ADC2 is unavailable while the radio runs.
  - GPIO34–39 are input-only and have **no internal pull-ups**, so the button SIG pins must not use them.
  - Avoid the strapping pins (0, 2, 5, 12, 15) and the flash pins (6–11).
- **Pro Micro:** everything runs at 5 V. Keep D0/D1 (UART) free if you want them for debugging.

## 4. CD74HC4067 multiplexers

All multiplexers are wired identically except for what hangs on C0..C15 and where SIG goes.

| Mux pin | Connect to |
|---------|------------|
| VCC | MCU logic voltage (5 V Pro Micro, 3.3 V Teensy/ESP32) + 100 nF to GND at the pin |
| GND | GND |
| EN (active low) | GND (always enabled) |
| S0, S1, S2, S3 | The shared select lines: **every mux gets the same four wires** |
| SIG | Fader mux → `FADER_SIG` (ADC). Button muxes → `MUTE_SIG` / `SOLO_SIG` / `FX_SIG` |
| C0 … C15 | Channel 1 … 16 (C0 = channel 1 = first LED = leftmost strip) |

- Channel n uses input Cn on every mux. Channels above `NUM_CHANNELS` are never selected, so unused inputs can stay
  open.
- Select-line order matters: S0 is the least significant bit. If two channels look swapped (for example 2↔3 or 1↔8),
  two select wires are crossed.
- After every select change the firmware waits `REKA_MUX_SETTLE_US` (5 µs AVR, 10 µs others). On AVR it also
  discards one ADC conversion so the sample-and-hold capacitor settles on the new channel. The button pins are read
  after the conversion, which gives the pull-ups extra time to recharge the SIG line.

## 5. Faders

Each fader is a linear 10 kΩ potentiometer used as a voltage divider:

| Fader pin | Connect to |
|-----------|------------|
| End 1 ("top" end of travel) | Analog VCC: **5 V on the Pro Micro, 3.3 V on Teensy 4.1 and ESP32** |
| End 2 ("bottom" end) | GND |
| Wiper | Fader mux input Cn |

- **ESP32 and Teensy:** never feed 5 V to the pots. The wiper would exceed the ADC's maximum input voltage.
- If a fader reads backwards (full at the bottom), swap its two end connections. To flip all faders at once, set
  `REKA_FADER_INVERT 1`.
- Do **not** put a large capacitor on the fader mux SIG line. Its RC with the pot impedance would smear neighbouring
  channels together. If anything, use at most ~1 nF there.
- Use short ground returns and run fader GND and VCC as a star from the MCU. Analog noise mostly comes from shared
  return paths with the LED current, so keep the LED 5 V/GND wiring separate (see §7).
- 10-bit (Pro Micro) or 12-bit (Teensy, ESP32) readings are scaled to the protocol's 0..65535.

What the firmware does with the readings:

- **Filtering.** A two-speed integer EMA smooths noise heavily while a fader rests and follows quickly while it
  moves (`REKA_FADER_EMA_SHIFT`, `REKA_FADER_EMA_FAST_SHIFT`, `REKA_FADER_FAST_THRESHOLD`).
- **Deadband.** A new position is only reported when it moves more than `REKA_FADER_DEADBAND`.
- **End zones.** About 1 % at each end of travel reports exactly 0 or 65535; the rest is stretched linearly
  (`REKA_FADER_END_ZONE`; 2.4 % on ESP32, whose ADC is non-linear near the rails).
- **Rate limit.** At most one `MSG_FADER` per channel every 4 ms, and the final resting value is always sent.

## 6. Buttons

| Button pin | Connect to |
|------------|------------|
| Contact 1 | Button mux input Cn |
| Contact 2 | GND |

- The mux SIG pin is configured `INPUT_PULLUP`, so a pressed button reads LOW. Inputs that are not wired read HIGH
  (released).
- The firmware debounces in software: a new level must be stable for `REKA_DEBOUNCE_MS` (5 ms). It sends
  `MSG_BUTTON` on both press and release.
- **External pull-ups (optional):** for cables longer than ~30 cm, add a 10 kΩ resistor from each button mux SIG
  line to the logic VCC. The internal pull-ups (20–50 kΩ) charge the mux's capacitance slowly after the select lines
  change. The firmware already allows for that, but a stronger pull-up adds margin.
- **Hardware debounce for noisy buttons (optional):** add **100 nF from Cn to GND together with 10 kΩ from Cn to
  VCC** at each button (τ ≈ 1 ms). The per-channel resistor is required. Behind a multiplexer the MCU's pull-up only
  reaches a channel while it is selected (1/N of the time), so a capacitor without its own pull-up would make
  releases very slow.

## 7. WS2812B LEDs

| Connection | Detail |
|------------|--------|
| MCU `LED_DATA` → **330 Ω** → DIN of LED #1 | Put the resistor near the first LED. |
| DOUT of LED k → DIN of LED k+1 | LED #1 = channel 1, … LED #N = channel N. |
| 5 V and GND to the chain | At the first LED, with **1000 µF** across 5 V/GND. For long chains, also feed power at the far end. |
| GND | Common with the MCU ground. |

**Level shifting (Teensy 4.1, ESP32).** WS2812B at 5 V needs a data-high level of at least 0.7 × VDD = 3.5 V, so
3.3 V logic is out of spec and may flicker. Route the data line through a 74AHCT125 powered from 5 V (input 3.3 V
logic, output 5 V). An alternative is to power the LEDs from ~4.3 V: put a silicon diode in series with their
supply, so VIH becomes ~3.0 V. The Pro Micro runs at 5 V and needs no shifter.

**Colour order.** `REKA_LED_TYPE` defaults to `NEO_GRB + NEO_KHZ800`. If red and green look swapped, use `NEO_RGB`.

**Power budget.**
- A WS2812B draws about 1 mA when dark and up to ~20 mA per colour at full value (60 mA white).
- The firmware scales every colour by the global brightness B (`REKA_LED_BRIGHTNESS`, default 64; the host can change
  it with `MSG_DISPLAY_CONFIG`).
- Worst case is every channel clipping at once (all LEDs white):

| LEDs | B = 64 (default) | B = 128 | B = 255 |
|------|------------------|---------|---------|
| 8 | ≈ 0.12 A | ≈ 0.24 A | ≈ 0.48 A |
| 16 | ≈ 0.24 A | ≈ 0.49 A | ≈ 0.96 A |

- A USB 2.0 port supplies 500 mA in total. Keep N × B ≤ ~1500 when bus-powered (MCU + OLED take ~50–150 mA on top), or
  feed the LEDs from a separate 5 V supply with its ground tied to the MCU ground.
- The Pro Micro's polyfuse trips around 500 mA.
- Normal operation draws far less than the worst case: level colours are dim, and mute/solo use one or two colours.

The firmware only calls `strip.show()` when a pixel actually changed, at most ~60 times per second.
`show()` blocks interrupts for ~30 µs per LED.

## 8. SSD1306 OLED

| OLED pin | Pro Micro | Teensy 4.1 | ESP32 |
|----------|-----------|------------|-------|
| VCC | 5 V (module regulator) or 3.3 V | **3.3 V** | **3.3 V** |
| GND | GND | GND | GND |
| SDA | D2 | 18 | GPIO21 |
| SCL | D3 | 19 | GPIO22 |

- Default I2C address 0x3C (7-bit). Some modules are strapped to 0x3D: set `REKA_OLED_I2C_ADDRESS`. The bus runs at
  400 kHz.
- Most modules carry 4.7–10 kΩ pull-ups to their own VCC. On 3.3 V MCUs, power the module from 3.3 V so SDA/SCL are
  never pulled to 5 V. Add 4.7 kΩ pull-ups only if your module has none.
- At boot the firmware probes the address. Without a display the OLED code is skipped and `CAP_OLED` is not reported
  to the host.
- **1.3" modules are often SH1106, not SSD1306.** They show a 2-pixel-shifted or garbled picture. Change the two
  constructors in `REKA.ino` to `U8G2_SH1106_128X64_NONAME_1_HW_I2C` / `..._F_HW_I2C`.
- Display mounted upside down: set `REKA_OLED_FLIP 1`.
- The display is drawn without blocking:
  - The Pro Micro uses U8g2's 128-byte page buffer, drawing and sending one 8-pixel page per `loop()` iteration.
  - Teensy and ESP32 draw into a full frame buffer, then send one 8-pixel row per iteration.
  - Either way a single I2C transfer is ~3 ms, never a full-frame (~25 ms) stall.
  - Redraws happen only when something changed: at most ~12 fps on the Pro Micro, ~20 fps elsewhere.

## 9. Building and flashing

**PlatformIO** (from `firmware/`):

```
pio run                         # all three boards
pio run -e promicro -t upload   # or teensy41 / esp32dev
```

The environments:
- `promicro`: SparkFun Pro Micro 16 MHz, USB-MIDI on.
- `teensy41`: USB type Serial + MIDI.
- `esp32dev`: no USB-MIDI.

**Arduino IDE.** Open `firmware/REKA/REKA.ino`.

1. Install the libraries **U8g2** and **Adafruit NeoPixel**. On a Pro Micro with USB-MIDI, also install **MIDIUSB**.
2. Pick the board:
   - **Pro Micro:** "SparkFun Pro Micro" (SparkFun AVR Boards), 5 V/16 MHz. Arduino AVR core 1.8.3 or newer is
     required (for `Wire.setWireTimeout`).
   - **Teensy:** Teensy 4.1 (Teensyduino), Tools > USB Type > **Serial + MIDI**.
   - **ESP32:** "ESP32 Dev Module" (arduino-esp32 2.0.x; 2.0.17 tested).
3. USB-MIDI is **off by default** in the IDE. To enable it on a Pro Micro or Teensy, set `#define REKA_USB_MIDI 1` in
   `RekaConfig.h`. On Teensy this needs a USB type that includes MIDI; the build stops with a clear error otherwise.

Resource use with 8 channels:

| Build | Flash | RAM |
|-------|-------|-----|
| Pro Micro, USB-MIDI on | ~25.0 KB of 28 KB | ~1.5 KB of 2.5 KB |
| Pro Micro, USB-MIDI off | ~23.3 KB | ~1.2 KB |
| Pro Micro, 16 channels with MIDI | ~25.0 KB | ~1.7 KB |

The LED buffer (3 bytes per LED) is allocated once at start-up and is not included in these figures.

## 10. On-device behaviour

### Connection

- The device announces itself (`MSG_HELLO`, then a `MSG_FADER_SNAPSHOT` of every fader position):
  - at boot,
  - whenever the host opens the port (native-USB boards),
  - whenever the host sends `MSG_HELLO_REQUEST`.
- Any valid frame from the host marks it **connected**.
- **Disconnected** means 3 s without a valid frame, or the host closing the port. The surface then shows the idle
  animation and the waiting screen.
- On every new connection the device forgets all channel names, states, meters and the profile name. The host
  re-sends what it wants shown.
- The host is the single source of truth:
  - A button press only sends `MSG_BUTTON`. The LED and OLED change when the host answers with
    `MSG_CHANNEL_STATE`.
  - While no host is connected, button presses are not sent (they still go out as USB-MIDI notes, if enabled).

### LED colours (one LED per channel)

| State | Colour |
|-------|--------|
| No host connected | Slow "breathing" blue on all LEDs (~4 s cycle) |
| Channel not mapped (no `FLAG_MAPPED`, or channel ≥ `numMappedChannels`) | Off |
| Mapped, playing | Dim level colour from the meter: green below −30 dBFS → yellow at −15 dBFS → red at 0 dBFS. It gets brighter as the level rises; silence is a faint green. |
| Muted (`FLAG_MUTE`) | Solid red |
| Soloed (`FLAG_SOLO`) | Amber, pulsing ~1 Hz |
| Implicitly muted by another channel's solo (`FLAG_SOLO_IMPLIED_MUTE`) | Dim red |
| FX bypassed (`FLAG_FX_BYPASS`) | Cyan tint added to the level/solo colour (not to mute red) |
| Clip (meter byte 255, or `FLAG_CLIP` becoming set) | White flash, held ~300 ms (re-triggered while the meter keeps clipping) |

Priority, highest first: clip → mute → solo → implicit mute → level colour. Everything is scaled by the global LED
brightness.

### OLED screens

**Waiting screen** (no host):

- "REKA" title bar with the firmware version.
- "Waiting for REKA...", the protocol version, the channel count, and "MIDI" when USB-MIDI is enabled.
- A live bar per fader showing its physical position.
- Small markers under a channel's bar while one of its buttons is held: left = mute, middle = solo, right = FX.

This makes the waiting screen a complete hardware test without the app.

**Channel screen** (host connected), top to bottom:

1. **Header:** the profile name and "selected channel / total", e.g. `3/8`.
2. **Channel name**, large. It shows `Ch n` until the host names the channel. A `CLIP` badge appears while
   `FLAG_CLIP` is set.
3. **Gain** in dB with one decimal (`-12.3 dB`, `+3.5 dB`), or `-inf dB` for a fully-down fader.
4. **Badges:** `M` (mute), `S` (solo), `FX` (FX bypass). Filled = active. A double-outlined `M` means the channel is
   implicitly muted by another channel's solo.
5. **Fader bar:** the **physical** fader position. A **▲ or ▼ arrow** appears when the host reports a soft-takeover
   mismatch (`pickup != 0`). Move the physical fader in the arrow's direction until it reaches the software value and
   takes over; the arrow then disappears.
6. **Mini meters** for all channels: a filled bar is the level, a hollow full-height bar is muted, a single dot is
   unmapped, and an underline marks the selected channel.

**Selected channel:** the last channel whose fader or button was touched on the surface, or the one the host sets
with `MSG_SELECT`, whichever happened last.

**Display settings:** the host sets the OLED contrast and LED brightness with `MSG_DISPLAY_CONFIG`.

## 11. USB-MIDI mode

When built with `REKA_USB_MIDI 1` (Pro Micro and Teensy only), the device also enumerates as a class-compliant
USB-MIDI device. MIDI runs **in addition to** the REKA serial protocol. The numbers below are the defaults from
`RekaConfig.h`.

**Device → computer, MIDI channel 1** (`REKA_MIDI_TX_CHANNEL`):

| Control | Message |
|---------|---------|
| Fader n (n = 0…15) | Control Change **CC 1+n**, value 0…127 (sent only when the 7-bit value changes) |
| Mute button n | Note **0+n**: note-on velocity 127 on press, note-off velocity 0 on release |
| Solo button n | Note **16+n** (same velocities) |
| FX button n | Note **32+n** (same velocities) |

**Computer → device, MIDI channel 2** (`REKA_MIDI_RX_CHANNEL`). This drives the LEDs/OLED **only while no REKA serial
host is connected**:

| Message | Effect |
|---------|--------|
| CC 1+n, value 0…127 | Meter level of channel n (127 = 0 dBFS; the LED goes green → yellow → red) |
| Note 0+n on/off | Mute state of channel n (note-on with velocity > 0 = on; note-off or velocity 0 = off) |
| Note 16+n on/off | Solo state |
| Note 32+n on/off | FX-bypass state |

- The first such message switches the surface into MIDI feedback mode: all channels are treated as mapped, and the
  OLED header reads `USB-MIDI` with the fader position in percent instead of a gain.
- As soon as the REKA app connects over serial, it takes over and the MIDI feedback is ignored.
- If no application reads the MIDI port, outgoing MIDI messages are dropped rather than stalling the surface.

## 12. Notes for the host application

- **Port settings.** Open the port at 115200 baud with **DTR asserted**.
  - The ATmega32U4's USB serial silently drops everything it sends while DTR is low.
  - The firmware also waits for DTR on Teensy.
  - Assert RTS too: on ESP32 DevKits, DTR and RTS drive the auto-reset circuit. Both asserted, or both clear,
    leaves the board running; asserting only one holds it in reset or boot mode.
- **Startup output.** An ESP32 prints ROM boot text at 115200 baud after a reset. The COBS framing resynchronises at
  the next 0x00, so simply drop undecodable bytes.
- **Duplicate HELLO.** On native-USB boards, opening the port and then sending `MSG_HELLO_REQUEST` produces two
  HELLO + snapshot sequences. Both are valid.
- **Keep-alive.** Send `MSG_PING` (or any frame) at least every second. The device declares the host gone after 3 s.
- **After each HELLO,** send the profile name, channel names, `MSG_CHANNEL_STATE` for every channel (with
  `FLAG_MAPPED`), `MSG_DISPLAY_CONFIG` and meters. The device starts each connection with a blank state.
- **Validation.** Frames with unknown types, short payloads or out-of-range channels are ignored. Channel and profile
  names must be printable ASCII (other bytes are shown as `?`).
- **`MSG_FADER`** values are 0…65535. 0 and 65535 are exact at the ends of travel. Each channel sends at most every
  4 ms, and the last value is always delivered.
- **`FLAG_SOLO_IMPLIED_MUTE`** means the channel is silenced because another channel is soloed (solo-in-place); the LED shows it dim red.

## 13. Bring-up checklist

1. **Power test without the MCU fitted** (or with nothing connected to its USB). Check that 5 V and 3.3 V reach
   every mux VCC, fader end and the OLED, and that no rail is shorted to GND.
2. **Flash the firmware.** The OLED shows the **waiting screen** and all LEDs **breathe blue**.
   - Blank OLED: see §14 (address, SDA/SCL swapped, SH1106).
   - Dark LEDs: check DIN versus DOUT, the 330 Ω resistor, the level shifter and the common ground.
3. **Faders.** Move each fader through its full travel. Exactly one bar should follow, and it should reach empty and
   full at the two ends.
   - Bar moves the wrong way: swap the pot ends (§5).
   - Wrong bar moves: check the S0..S3 wiring.
   - Bars twitch at rest: check fader GND/VCC and keep LED current off the analog ground.
4. **Buttons.** Press every button. A marker appears under its channel's bar: left = mute, middle = solo, right = FX.
   - A marker on the wrong channel: select-line issue.
   - A marker that never appears: check the button's GND and the mux SIG pin.
   - A marker that stays on: the button is shorted, or the SIG pin is wrong.
5. **Connect the REKA app.** The screen switches to the channel view with the profile name. LEDs light per mapping
   and meters move with audio.
6. **Mute round trip.** Press a mute button. The LED turns red only after the app confirms. If nothing happens, the
   app is not receiving `MSG_BUTTON` (port settings, DTR).
7. **Soft takeover.** Change a channel's gain in the app. The ▲/▼ arrow appears; moving the fader past the software
   value removes it.
8. **USB-MIDI** (if enabled). In a MIDI monitor, faders show as CC 1–16 and buttons as notes on channel 1.
9. **Soak test.** Leave the surface idle for a few minutes with the app connected. No fader messages should arrive
   from untouched faders, and the LEDs/OLED should not flicker.

## 14. Troubleshooting

| Symptom | Likely cause / fix |
|---------|--------------------|
| OLED stays black | Wrong address (try `REKA_OLED_I2C_ADDRESS 0x3D`); SDA/SCL swapped; 3.3 V MCU with an OLED pulling the bus to 5 V; no pull-ups at all. |
| OLED picture shifted by 2 px or noisy | SH1106 controller: change the constructor (§8). |
| OLED upside down | `REKA_OLED_FLIP 1`. |
| LEDs flicker or show random colours (Teensy/ESP32) | No level shifter on the data line; long unterminated data wire; missing 330 Ω. |
| LEDs red/green swapped | `REKA_LED_TYPE (NEO_RGB + NEO_KHZ800)`. |
| Surface resets or USB disconnects when many LEDs light | LED current exceeds the USB budget: lower the brightness or use an external 5 V supply (§7). |
| Two faders move one bar, or bars are in the wrong order | Select lines crossed, or two mux S pins not on the shared bus. |
| Fader never reaches the end | Pot end not at full VCC/GND; on ESP32, increase `REKA_FADER_END_ZONE`. |
| Fader values jitter | Noisy analog supply or shared ground with the LEDs. Increase `REKA_FADER_DEADBAND` or `REKA_FADER_EMA_SHIFT`. |
| A button triggers its neighbour | Pull-up too weak for the cable length: add 10 kΩ external pull-ups on the button SIG lines, or increase `REKA_MUX_SETTLE_US`. |
| App sees no data from a Pro Micro | Host opened the port without DTR (§12), or opened it at 1200 baud (which resets the Pro Micro into its bootloader). |
| ESP32 reboots when the app connects | DTR/RTS toggling the auto-reset circuit (§12). |
| Teensy build error about USB type | Choose Tools > USB Type > "Serial + MIDI", or set `REKA_USB_MIDI 0`. |
