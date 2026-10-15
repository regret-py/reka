// =====================================================================================================
//  RekaConfig.h - user-tunable configuration for the REKA control-surface firmware
//
//  Everything a builder may want to change lives here: channel count, feature toggles, pin maps,
//  fader filtering, debounce, LED / OLED settings and USB-MIDI mapping. Every value is wrapped in
//  #ifndef so it can also be overridden from the build system (platformio.ini build_flags, or
//  arduino-cli --build-property) without editing this file.
//
//  See docs/HARDWARE_WIRING.md for the matching wiring diagrams and the bring-up checklist.
// =====================================================================================================
#ifndef REKA_CONFIG_H
#define REKA_CONFIG_H

// ---- Firmware identity -------------------------------------------------------------------------------
#define REKA_FW_VERSION_MAJOR 1
#define REKA_FW_VERSION_MINOR 0

// ---- Channel count -----------------------------------------------------------------------------------
// Number of physical channel strips (fader + optional mute/solo/FX buttons + one WS2812B LED each).
// One CD74HC4067 serves up to 16 channels, so the valid range is 1..16.
#ifndef NUM_CHANNELS
#define NUM_CHANNELS 8
#endif

// ---- Feature toggles (1 = enabled, 0 = compiled out) -------------------------------------------------
#ifndef REKA_ENABLE_OLED
#define REKA_ENABLE_OLED 1  // SSD1306 128x64 I2C display (U8g2 library)
#endif
#ifndef REKA_ENABLE_LEDS
#define REKA_ENABLE_LEDS 1  // one WS2812B per channel (Adafruit NeoPixel library)
#endif
#ifndef REKA_ENABLE_BUTTONS
#define REKA_ENABLE_BUTTONS 1  // mute / solo / FX-bypass button multiplexers
#endif
// USB-MIDI class-compliant output (faders -> CC, buttons -> notes) alongside the serial protocol.
//   ATmega32U4 : needs the MIDIUSB library (arduino-libraries/MIDIUSB).
//   Teensy     : needs a USB Type that includes both Serial and MIDI (Tools > USB Type > "Serial + MIDI").
//   ESP32      : not available (the DevKit's USB port is a USB-UART bridge, not native USB).
// PlatformIO sets this per environment in platformio.ini; the Arduino IDE default is off.
#ifndef REKA_USB_MIDI
#define REKA_USB_MIDI 0
#endif

// ---- Pin maps ----------------------------------------------------------------------------------------
// Marker for "not connected". A button multiplexer whose SIG pin is REKA_PIN_NONE is disabled, which is
// how you build a surface with, say, faders and mute buttons only.
#define REKA_PIN_NONE 255

// All CD74HC4067s (1 analog fader mux + up to 3 digital button muxes) share the S0..S3 select lines.
//   REKA_PIN_FADER_SIG : analog input wired to the fader mux common (SIG) pin
//   REKA_PIN_*_SIG     : digital inputs (INPUT_PULLUP) wired to the button muxes' SIG pins
//   REKA_PIN_LED_DATA  : WS2812B data-in (through a 330 ohm series resistor)
#if defined(ARDUINO_ARCH_AVR)
// SparkFun Pro Micro 5V/16MHz (ATmega32U4). I2C is fixed in hardware: SDA = D2, SCL = D3.
#ifndef REKA_PIN_MUX_S0
#define REKA_PIN_MUX_S0 4
#endif
#ifndef REKA_PIN_MUX_S1
#define REKA_PIN_MUX_S1 5
#endif
#ifndef REKA_PIN_MUX_S2
#define REKA_PIN_MUX_S2 6
#endif
#ifndef REKA_PIN_MUX_S3
#define REKA_PIN_MUX_S3 7
#endif
#ifndef REKA_PIN_FADER_SIG
#define REKA_PIN_FADER_SIG A0
#endif
#ifndef REKA_PIN_MUTE_SIG
#define REKA_PIN_MUTE_SIG 8
#endif
#ifndef REKA_PIN_SOLO_SIG
#define REKA_PIN_SOLO_SIG 9
#endif
#ifndef REKA_PIN_FX_SIG
#define REKA_PIN_FX_SIG 10
#endif
#ifndef REKA_PIN_LED_DATA
#define REKA_PIN_LED_DATA 16
#endif

#elif defined(TEENSYDUINO)
// Teensy 4.1. I2C (Wire) is SDA = 18, SCL = 19. 3.3 V logic, inputs are NOT 5 V tolerant.
#ifndef REKA_PIN_MUX_S0
#define REKA_PIN_MUX_S0 2
#endif
#ifndef REKA_PIN_MUX_S1
#define REKA_PIN_MUX_S1 3
#endif
#ifndef REKA_PIN_MUX_S2
#define REKA_PIN_MUX_S2 4
#endif
#ifndef REKA_PIN_MUX_S3
#define REKA_PIN_MUX_S3 5
#endif
#ifndef REKA_PIN_FADER_SIG
#define REKA_PIN_FADER_SIG A0  // pin 14
#endif
#ifndef REKA_PIN_MUTE_SIG
#define REKA_PIN_MUTE_SIG 6
#endif
#ifndef REKA_PIN_SOLO_SIG
#define REKA_PIN_SOLO_SIG 7
#endif
#ifndef REKA_PIN_FX_SIG
#define REKA_PIN_FX_SIG 8
#endif
#ifndef REKA_PIN_LED_DATA
#define REKA_PIN_LED_DATA 9
#endif

#elif defined(ARDUINO_ARCH_ESP32)
// ESP32 DevKit (WROOM-32). The fader SIG MUST be an ADC1 pin (GPIO32..39): ADC2 is unusable once the
// radio is on. GPIO34..39 have no internal pull-ups, so the button SIG pins must NOT be on 34..39.
// GPIO 0/2/5/12/15 are boot-strapping pins and 6..11 belong to the flash chip: avoid them.
#ifndef REKA_PIN_MUX_S0
#define REKA_PIN_MUX_S0 18
#endif
#ifndef REKA_PIN_MUX_S1
#define REKA_PIN_MUX_S1 19
#endif
#ifndef REKA_PIN_MUX_S2
#define REKA_PIN_MUX_S2 23
#endif
#ifndef REKA_PIN_MUX_S3
#define REKA_PIN_MUX_S3 13
#endif
#ifndef REKA_PIN_FADER_SIG
#define REKA_PIN_FADER_SIG 36  // "VP", ADC1_CH0
#endif
#ifndef REKA_PIN_MUTE_SIG
#define REKA_PIN_MUTE_SIG 25
#endif
#ifndef REKA_PIN_SOLO_SIG
#define REKA_PIN_SOLO_SIG 26
#endif
#ifndef REKA_PIN_FX_SIG
#define REKA_PIN_FX_SIG 27
#endif
#ifndef REKA_PIN_LED_DATA
#define REKA_PIN_LED_DATA 4
#endif
#ifndef REKA_PIN_I2C_SDA
#define REKA_PIN_I2C_SDA 21
#endif
#ifndef REKA_PIN_I2C_SCL
#define REKA_PIN_I2C_SCL 22
#endif

#else
#error "REKA: unsupported board. Add a pin map for it in RekaConfig.h (AVR, Teensy and ESP32 are provided)."
#endif

// ---- Fader acquisition & filtering -------------------------------------------------------------------
// Native ADC resolution. The firmware scales it to the protocol's 0..65535 range.
#ifndef REKA_ADC_BITS
#if defined(ARDUINO_ARCH_AVR)
#define REKA_ADC_BITS 10
#else
#define REKA_ADC_BITS 12  // Teensy: analogReadResolution(12); ESP32: native 12-bit
#endif
#endif

// Time to let the CD74HC4067 outputs and the wiring settle after the select lines change, in
// microseconds. AVR additionally performs a throw-away conversion (~110 us) before the real one.
#ifndef REKA_MUX_SETTLE_US
#if defined(ARDUINO_ARCH_AVR)
#define REKA_MUX_SETTLE_US 5
#else
#define REKA_MUX_SETTLE_US 10
#endif
#endif

// Minimum time between two full scans, in microseconds (0 = scan every loop()). Fast MCUs are throttled
// to 1 kHz so the EMA time constant below means roughly the same thing on every board.
#ifndef REKA_SCAN_INTERVAL_US
#if defined(ARDUINO_ARCH_AVR)
#define REKA_SCAN_INTERVAL_US 0
#else
#define REKA_SCAN_INTERVAL_US 1000
#endif
#endif

// Two-speed exponential moving average (integer fixed point): filtered += (sample - filtered) / 2^SHIFT.
//   REKA_FADER_EMA_SHIFT      : used while the fader rests -> heavy smoothing of ADC noise
//   REKA_FADER_EMA_FAST_SHIFT : used when a sample is more than REKA_FADER_FAST_THRESHOLD (0..65535 units)
//                               away from the filtered value, i.e. the fader is being moved -> low lag
// Time constant ~= 2^SHIFT scans. The threshold must stay well above the ADC noise.
#ifndef REKA_FADER_EMA_SHIFT
#if defined(ARDUINO_ARCH_AVR)
#define REKA_FADER_EMA_SHIFT 3
#else
#define REKA_FADER_EMA_SHIFT 4
#endif
#endif
#ifndef REKA_FADER_EMA_FAST_SHIFT
#define REKA_FADER_EMA_FAST_SHIFT 1
#endif
#ifndef REKA_FADER_FAST_THRESHOLD
#if defined(ARDUINO_ARCH_AVR)
#define REKA_FADER_FAST_THRESHOLD 1536  // 24 LSB
#elif defined(ARDUINO_ARCH_ESP32)
#define REKA_FADER_FAST_THRESHOLD 2048  // 128 LSB
#else
#define REKA_FADER_FAST_THRESHOLD 1024  // 64 LSB
#endif
#endif

// Hysteresis in 0..65535 units: a new position is reported only when the filtered value moves further
// than this from the last reported one. 1 LSB = 64 units at 10 bit, 16 units at 12 bit.
#ifndef REKA_FADER_DEADBAND
#if defined(ARDUINO_ARCH_AVR)
#define REKA_FADER_DEADBAND 160  // ~2.5 LSB
#elif defined(ARDUINO_ARCH_ESP32)
#define REKA_FADER_DEADBAND 288  // ~18 LSB: the ESP32 ADC is noisy
#else
#define REKA_FADER_DEADBAND 128  // ~8 LSB
#endif
#endif

// Dead zone at each end of the travel, in 0..65535 units (~1 %). Inside it the fader reports exactly
// 0 or 65535; the remaining travel is stretched linearly over the full range.
#ifndef REKA_FADER_END_ZONE
#if defined(ARDUINO_ARCH_ESP32)
#define REKA_FADER_END_ZONE 1600  // the ESP32 ADC is non-linear near both rails
#else
#define REKA_FADER_END_ZONE 640
#endif
#endif

// Set to 1 if your faders read 0 at the top (wiper/rail wired the other way round).
#ifndef REKA_FADER_INVERT
#define REKA_FADER_INVERT 0
#endif

// Per-channel rate limit for MSG_FADER (the latest value is always sent once the fader rests).
#ifndef REKA_FADER_MIN_INTERVAL_MS
#define REKA_FADER_MIN_INTERVAL_MS 4
#endif

// ---- Buttons -----------------------------------------------------------------------------------------
// A button change is accepted after its raw level has been stable for this long.
#ifndef REKA_DEBOUNCE_MS
#define REKA_DEBOUNCE_MS 5
#endif

// ---- WS2812B LEDs ------------------------------------------------------------------------------------
// Default global brightness 0..255 (the host can override it with MSG_DISPLAY_CONFIG).
#ifndef REKA_LED_BRIGHTNESS
#define REKA_LED_BRIGHTNESS 64
#endif
// NeoPixel colour order / speed flags.
#ifndef REKA_LED_TYPE
#define REKA_LED_TYPE (NEO_GRB + NEO_KHZ800)
#endif
// Minimum time between two strip.show() calls (show() disables interrupts while it clocks data out).
#ifndef REKA_LED_FRAME_MS
#define REKA_LED_FRAME_MS 17  // ~60 Hz
#endif
// How long a clip event holds the channel LED white.
#ifndef REKA_CLIP_HOLD_MS
#define REKA_CLIP_HOLD_MS 300
#endif

// ---- SSD1306 OLED ------------------------------------------------------------------------------------
#ifndef REKA_OLED_I2C_ADDRESS
#define REKA_OLED_I2C_ADDRESS 0x3C  // 7-bit address (some modules use 0x3D)
#endif
#ifndef REKA_I2C_CLOCK_HZ
#define REKA_I2C_CLOCK_HZ 400000UL
#endif
#ifndef REKA_OLED_CONTRAST
#define REKA_OLED_CONTRAST 160  // 0..255, the host can override it with MSG_DISPLAY_CONFIG
#endif
#ifndef REKA_OLED_FLIP
#define REKA_OLED_FLIP 0  // 1 = rotate the picture 180 degrees
#endif
// 1 = U8g2 page buffer (128 B RAM, the frame is drawn 8 times, once per page), 0 = full 1 KB frame buffer
// (drawn once, then transferred one tile row per loop()). Either way the transfer is spread across loop().
#ifndef REKA_OLED_PAGE_MODE
#if defined(ARDUINO_ARCH_AVR)
#define REKA_OLED_PAGE_MODE 1
#else
#define REKA_OLED_PAGE_MODE 0
#endif
#endif
// Minimum time between two OLED frames (dirty-driven, so a static screen costs nothing).
#ifndef REKA_OLED_FRAME_MS
#if defined(ARDUINO_ARCH_AVR)
#define REKA_OLED_FRAME_MS 80  // ~12 fps: a frame is 8 page transfers spread over 8 loop() iterations
#else
#define REKA_OLED_FRAME_MS 50  // ~20 fps
#endif
#endif

// ---- Host link ---------------------------------------------------------------------------------------
#ifndef REKA_SERIAL_BAUD
#define REKA_SERIAL_BAUD 115200UL
#endif
// No valid frame from the host for this long = disconnected (idle animation, waiting screen).
#ifndef REKA_HOST_TIMEOUT_MS
#define REKA_HOST_TIMEOUT_MS 3000UL
#endif

// ---- USB-MIDI mapping (only used when REKA_USB_MIDI = 1) ---------------------------------------------
// Device -> computer, on REKA_MIDI_TX_CHANNEL:
//   fader n : CC (REKA_MIDI_CC_BASE + n), 7-bit position
//   buttons : note (REKA_MIDI_NOTE_MUTE/SOLO/FX + n), velocity 127 on press, note-off on release
// Computer -> device, on REKA_MIDI_RX_CHANNEL (used for the LEDs only while no serial host is connected):
//   CC (REKA_MIDI_CC_BASE + n) = meter level 0..127, notes (same numbers as above) = mute/solo/FX state
#ifndef REKA_MIDI_TX_CHANNEL
#define REKA_MIDI_TX_CHANNEL 1  // 1..16
#endif
#ifndef REKA_MIDI_RX_CHANNEL
#define REKA_MIDI_RX_CHANNEL 2  // 1..16
#endif
#ifndef REKA_MIDI_CC_BASE
#define REKA_MIDI_CC_BASE 1
#endif
#ifndef REKA_MIDI_NOTE_MUTE
#define REKA_MIDI_NOTE_MUTE 0
#endif
#ifndef REKA_MIDI_NOTE_SOLO
#define REKA_MIDI_NOTE_SOLO 16
#endif
#ifndef REKA_MIDI_NOTE_FX
#define REKA_MIDI_NOTE_FX 32
#endif

// ---- Derived settings (not user-tunable) -------------------------------------------------------------
// Kept here rather than in REKA.ino: the sketch's prototype generator does not evaluate #if, so a macro
// defined in two #if branches of the .ino triggers spurious "redefined" warnings.

// The 32U4 and Teensy enumerate as native USB CDC devices and know whether the host opened the port (DTR).
#if (defined(ARDUINO_ARCH_AVR) && defined(USBCON)) || defined(TEENSYDUINO)
#define REKA_NATIVE_USB_SERIAL 1
#else
#define REKA_NATIVE_USB_SERIAL 0
#endif

#if REKA_OLED_FLIP
#define REKA_OLED_ROTATION U8G2_R2
#else
#define REKA_OLED_ROTATION U8G2_R0
#endif

#endif  // REKA_CONFIG_H
