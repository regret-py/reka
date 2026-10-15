// =====================================================================================================
//  REKA.ino - firmware for the REKA physical control surface
//
//  Hardware (see docs/HARDWARE_WIRING.md):
//    * up to 16 linear faders on one CD74HC4067 (analog SIG -> ADC pin)
//    * up to 3 more CD74HC4067s used as digital inputs: MUTE, SOLO and FX-bypass buttons to GND
//    * all four multiplexers share the S0..S3 select lines
//    * one WS2812B LED per channel, one SSD1306 128x64 I2C OLED
//
//  Host link: COBS/CRC-8 framed binary protocol over USB CDC / UART (RekaProtocol.h, shared with REKA.exe).
//  The host is the single source of truth: buttons only report presses, LEDs and the OLED only show what
//  the host sends back. Optional class-compliant USB-MIDI runs alongside (REKA_USB_MIDI).
//
//  Design rules for the main loop: nothing may block. Serial and MIDI writes only happen when the
//  endpoint has room, the OLED is pushed one 128-byte page per loop() iteration, the LED strip is
//  refreshed only when a pixel changed and at most ~60 Hz, and the scan loop uses integer maths only.
//
//  Board support: ATmega32U4 (SparkFun Pro Micro), Teensy 4.1, ESP32 DevKit. C++11.
//
//  Note for maintainers: PlatformIO generates prototypes for every column-0 function in this file without
//  evaluating #if (the Arduino IDE does evaluate it). So: free functions are defined unconditionally and
//  before use, feature code lives inside their bodies (#if, or `if (!REKA_ENABLE_x) return;` so it stays
//  type-checked), signatures use built-in types only, and the OLED renderer is a struct of static members.
//  This keeps every feature combination free of warnings.
// =====================================================================================================
#include <Arduino.h>

#include "RekaConfig.h"
#include "RekaProtocol.h"

#if REKA_ENABLE_LEDS
#include <Adafruit_NeoPixel.h>
#endif
#if REKA_ENABLE_OLED
#include <U8g2lib.h>
#include <Wire.h>
#endif
#if REKA_USB_MIDI && defined(ARDUINO_ARCH_AVR)
#include <MIDIUSB.h>
#endif

// ---- Build-time sanity checks ------------------------------------------------------------------------
#if NUM_CHANNELS < 1 || NUM_CHANNELS > 16
#error "NUM_CHANNELS must be between 1 and 16 (one CD74HC4067 per input group)"
#endif
#if REKA_ADC_BITS < 8 || REKA_ADC_BITS > 15
#error "REKA_ADC_BITS must be between 8 and 15"
#endif
#if REKA_USB_MIDI
#if defined(ARDUINO_ARCH_ESP32)
#error "REKA_USB_MIDI is not available on the ESP32 DevKit (no native USB). Set REKA_USB_MIDI to 0."
#elif defined(ARDUINO_ARCH_AVR) && !defined(USBCON)
#error "REKA_USB_MIDI needs a native-USB AVR (ATmega32U4: Pro Micro / Leonardo / Micro)."
#elif defined(TEENSYDUINO) && !defined(MIDI_INTERFACE)
#error "REKA_USB_MIDI on Teensy needs Tools > USB Type > \"Serial + MIDI\" (-D USB_MIDI_SERIAL)."
#endif
#endif
#if defined(TEENSYDUINO) && !defined(CDC_STATUS_INTERFACE)
#error "REKA needs the USB serial port on Teensy: choose a USB Type that includes Serial."
#endif

static_assert(NUM_CHANNELS <= reka_proto::kMaxChannels, "NUM_CHANNELS exceeds the protocol limit");
static_assert(REKA_FADER_EMA_SHIFT >= 1 && REKA_FADER_EMA_SHIFT <= 8, "REKA_FADER_EMA_SHIFT out of range");
static_assert(REKA_FADER_EMA_FAST_SHIFT >= 1 && REKA_FADER_EMA_FAST_SHIFT <= REKA_FADER_EMA_SHIFT,
              "REKA_FADER_EMA_FAST_SHIFT must be between 1 and REKA_FADER_EMA_SHIFT");
static_assert(REKA_FADER_END_ZONE < 16384, "REKA_FADER_END_ZONE too large");
static_assert(REKA_DEBOUNCE_MS >= 1 && REKA_DEBOUNCE_MS < 200, "REKA_DEBOUNCE_MS must fit the 8-bit timer");
static_assert(REKA_FADER_MIN_INTERVAL_MS < 200, "REKA_FADER_MIN_INTERVAL_MS must fit the 8-bit timer");
static_assert(REKA_CLIP_HOLD_MS < 30000, "REKA_CLIP_HOLD_MS must fit the 16-bit timer");
static_assert(REKA_MIDI_TX_CHANNEL >= 1 && REKA_MIDI_TX_CHANNEL <= 16, "MIDI channels are 1..16");
static_assert(REKA_MIDI_RX_CHANNEL >= 1 && REKA_MIDI_RX_CHANNEL <= 16, "MIDI channels are 1..16");

// ---- Constants ---------------------------------------------------------------------------------------
static const uint8_t kGroupCount = 3;  // button groups, index == reka_proto::ButtonId (MUTE, SOLO, FX)
static const uint8_t kButtonPins[kGroupCount] = {REKA_PIN_MUTE_SIG, REKA_PIN_SOLO_SIG, REKA_PIN_FX_SIG};
static const uint8_t kButtonQueueSize = 16;  // power of two
static const uint8_t kSnapshotPerFrame = (reka_proto::kMaxPayload - 2) / 2;
static const int32_t kEmaRound = 1L << (REKA_FADER_EMA_SHIFT - 1);
static const int32_t kEmaFastRound = 1L << (REKA_FADER_EMA_FAST_SHIFT - 1);
// Fader travel between the two end zones is stretched to 0..65535 with a Q16 multiplier. Because the
// stretched input never exceeds kFaderSpan, x * kFaderStretch stays below 2^32.
static const uint16_t kFaderSpan = 65535U - 2U * REKA_FADER_END_ZONE;
static const uint32_t kFaderStretch = (65535UL << 16) / kFaderSpan;

static const uint8_t kOledWidth = 128;
static const uint8_t kMidiStatusCC = 0xB0;
static const uint8_t kMidiStatusNoteOn = 0x90;
static const uint8_t kMidiStatusNoteOff = 0x80;

enum LinkMode { LINK_NONE = 0, LINK_HOST = 1, LINK_MIDI = 2 };

// ---- State -------------------------------------------------------------------------------------------
struct FaderState {
    uint16_t filtered;   // EMA of the scaled ADC samples, 0..65535
    uint16_t value;      // reported position: end zones + hysteresis applied (what the host sees)
    uint16_t sent;       // last position delivered to the host (MSG_FADER or snapshot)
    uint8_t lastSendMs;  // low byte of millis() at the last MSG_FADER (rate limiting)
    uint8_t midiSent;    // last 7-bit CC value sent over USB-MIDI
};

struct ChannelState {  // mirror of what the host told us about a channel
    uint8_t flags;     // reka_proto::ChannelFlags
    uint8_t meter;     // 0..240, 255 = clip (see RekaProtocol.h)
    int8_t pickup;     // reka_proto::PickupHint
    int16_t gain;      // tenths of a dB, kGainMinusInfinity = fader fully down
    char name[reka_proto::kNameLength + 1];
};

static FaderState gFader[NUM_CHANNELS];
static ChannelState gChan[NUM_CHANNELS];
static char gProfile[reka_proto::kProfileNameLength + 1];

// Buttons: bit n of each mask = channel n. Debounce timestamps are the low byte of millis().
static uint16_t gBtnRaw[kGroupCount];
static uint16_t gBtnStable[kGroupCount];
static uint8_t gBtnChangeMs[kGroupCount][NUM_CHANNELS];
static uint8_t gBtnQueue[kButtonQueueSize];  // pending MSG_BUTTON: ch | group << 4 | pressed << 7
static uint8_t gBtnQueueHead;
static uint8_t gBtnQueueCount;

static uint8_t gMuxSelect = 0xFF;  // select lines currently driven (0xFF = unknown)
#if REKA_SCAN_INTERVAL_US > 0
static uint32_t gLastScanUs;
#endif

// Host link
static reka_proto::FrameParser gParser;
static uint32_t gLastHeardMs;
static bool gHostConnected;
static bool gMidiMode;  // LEDs/OLED driven by incoming USB-MIDI because no serial host is connected
static bool gHelloPending;
static bool gSnapshotPending;
static uint8_t gSnapshotNext;
static bool gPongPending;
static uint8_t gPongToken;
static uint8_t gFaderCursor;  // round-robin start for MSG_FADER so no channel starves the others
#if REKA_NATIVE_USB_SERIAL
static bool gPortWasOpen;
#endif

// Presentation
static uint8_t gSelected;
static uint8_t gNumMapped = NUM_CHANNELS;
static uint8_t gLedBrightness = REKA_LED_BRIGHTNESS;
static uint8_t gOledContrast = REKA_OLED_CONTRAST;
static bool gContrastPending;
static bool gOledDirty = true;
static uint16_t gClipMask;  // channels currently showing the white clip flash
static uint16_t gClipStartMs[NUM_CHANNELS];

static uint32_t gLedLastMs;
#if REKA_ENABLE_LEDS
static Adafruit_NeoPixel gStrip(NUM_CHANNELS, REKA_PIN_LED_DATA, REKA_LED_TYPE);
#endif

#if REKA_ENABLE_OLED
// AVR: 128-byte page buffer (U8G2_..._1_). Elsewhere: full 1 KB frame buffer (U8G2_..._F_).
#if REKA_OLED_PAGE_MODE
static U8G2_SSD1306_128X64_NONAME_1_HW_I2C gOled(REKA_OLED_ROTATION, U8X8_PIN_NONE);
#elif defined(ARDUINO_ARCH_ESP32)
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C gOled(REKA_OLED_ROTATION, U8X8_PIN_NONE, REKA_PIN_I2C_SCL,
                                                 REKA_PIN_I2C_SDA);
#else
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C gOled(REKA_OLED_ROTATION, U8X8_PIN_NONE);
#endif
static bool gOledPresent;
static bool gOledBusy;        // a frame is being transferred across loop() iterations
#if !REKA_OLED_PAGE_MODE
static uint8_t gOledRow;  // full-buffer mode: next tile row to transfer
#endif
static uint32_t gOledLastMs;  // start of the last frame
static uint8_t gViewMode;     // LinkMode latched at frame start so all pages show the same screen
static uint8_t gViewSel;      // selected channel latched at frame start
#endif

#if REKA_USB_MIDI && defined(ARDUINO_ARCH_AVR)
// MIDIUSB's write path calls USB_Send(), which busy-waits up to 250 ms when nobody reads the endpoint.
// To check for room first we need the IN endpoint number, a protected member of the PluggableUSB module.
// A pointer-to-member formed inside a derived class reads it legally without creating another instance.
struct RekaMidiEndpoint : public MIDI_ {
    static uint8_t txEndpoint() {
        return static_cast<uint8_t>(MidiUSB.*(&RekaMidiEndpoint::pluggedEndpoint) + 1);
    }
};
#endif

// =====================================================================================================
//  Small helpers
// =====================================================================================================
static inline uint8_t scaleBrightness(uint8_t value, uint8_t scale) {
    return static_cast<uint8_t>((static_cast<uint16_t>(value) * (static_cast<uint16_t>(scale) + 1U)) >> 8);
}

static inline uint8_t addSaturate(uint8_t a, uint8_t b) {
    const uint16_t sum = static_cast<uint16_t>(static_cast<uint16_t>(a) + b);
    return sum > 255U ? 255U : static_cast<uint8_t>(sum);
}

static inline bool channelMapped(uint8_t ch) {
    return (gChan[ch].flags & reka_proto::FLAG_MAPPED) != 0 && ch < gNumMapped;
}

static inline uint8_t linkMode() {
    if (gHostConnected) return LINK_HOST;
    return gMidiMode ? LINK_MIDI : LINK_NONE;
}

static void selectChannel(uint8_t ch) {
    if (ch == gSelected) return;
    gSelected = ch;
    gOledDirty = true;
}

static void triggerClip(uint8_t ch) {
    gClipMask = static_cast<uint16_t>(gClipMask | (1U << ch));
    gClipStartMs[ch] = static_cast<uint16_t>(millis());
}

// Forget everything the host (or MIDI) told us: used whenever the link (re)starts.
static void resetSurfaceState() {
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ++ch) {
        ChannelState& c = gChan[ch];
        c.flags = 0;
        c.meter = 0;
        c.pickup = reka_proto::PICKUP_SYNCED;
        c.gain = reka_proto::kGainMinusInfinity;
        c.name[0] = '\0';
    }
    gProfile[0] = '\0';
    gNumMapped = NUM_CHANNELS;
    gClipMask = 0;
    gOledDirty = true;
}

// =====================================================================================================
//  USB-MIDI
//  The four transport primitives below are the only board-specific code. With REKA_USB_MIDI == 0 they
//  report "not ready" / "nothing received" and everything built on them folds away at compile time.
// =====================================================================================================
static bool midiTxReady() {
#if REKA_USB_MIDI && defined(ARDUINO_ARCH_AVR)
    // Strictly more than one 4-byte packet: filling the bank exactly makes USB_Send() wait for a ZLP slot.
    return USB_SendSpace(RekaMidiEndpoint::txEndpoint()) > 4;
#elif REKA_USB_MIDI && defined(TEENSYDUINO)
    return true;  // Teensy's usbMIDI gives up after one 40 ms timeout and then fails fast while unread
#else
    return false;
#endif
}

static void midiSend(uint8_t status, uint8_t data1, uint8_t data2) {
#if REKA_USB_MIDI && defined(ARDUINO_ARCH_AVR)
    const midiEventPacket_t packet = {static_cast<uint8_t>(status >> 4), status, data1, data2};
    MidiUSB.sendMIDI(packet);
#elif REKA_USB_MIDI && defined(TEENSYDUINO)
    const uint8_t channel = static_cast<uint8_t>((status & 0x0F) + 1);
    switch (status & 0xF0) {
        case kMidiStatusCC: usbMIDI.sendControlChange(data1, data2, channel); break;
        case kMidiStatusNoteOn: usbMIDI.sendNoteOn(data1, data2, channel); break;
        case kMidiStatusNoteOff: usbMIDI.sendNoteOff(data1, data2, channel); break;
        default: break;
    }
#else
    (void)status;
    (void)data1;
    (void)data2;
#endif
}

static void midiFlush() {
#if REKA_USB_MIDI && defined(ARDUINO_ARCH_AVR)
    MidiUSB.flush();
#elif REKA_USB_MIDI && defined(TEENSYDUINO)
    usbMIDI.send_now();
#endif
}

// Next received note-on / note-off / control-change message, as a MIDI status byte plus two data bytes.
// Other message types are skipped. Returns false when nothing (more) is pending.
static bool midiRead(uint8_t* status, uint8_t* data1, uint8_t* data2) {
#if REKA_USB_MIDI && defined(ARDUINO_ARCH_AVR)
    for (uint8_t i = 0; i < 16; ++i) {
        const midiEventPacket_t rx = MidiUSB.read();
        if (rx.header == 0) return false;
        const uint8_t cin = rx.header & 0x0F;  // USB-MIDI code index number
        if (cin == 0x08 || cin == 0x09 || cin == 0x0B) {
            *status = rx.byte1;
            *data1 = rx.byte2;
            *data2 = rx.byte3;
            return true;
        }
    }
    return false;
#elif REKA_USB_MIDI && defined(TEENSYDUINO)
    for (uint8_t i = 0; i < 16 && usbMIDI.read(); ++i) {
        const uint8_t type = usbMIDI.getType();
        if (type == usbMIDI.NoteOn || type == usbMIDI.NoteOff || type == usbMIDI.ControlChange) {
            *status = static_cast<uint8_t>(type | ((usbMIDI.getChannel() - 1) & 0x0F));
            *data1 = usbMIDI.getData1();
            *data2 = usbMIDI.getData2();
            return true;
        }
    }
    return false;
#else
    (void)status;
    (void)data1;
    (void)data2;
    return false;
#endif
}

static void midiSendButton(uint8_t ch, uint8_t group, bool pressed) {
    static const uint8_t kNoteBase[kGroupCount] = {REKA_MIDI_NOTE_MUTE, REKA_MIDI_NOTE_SOLO, REKA_MIDI_NOTE_FX};
    if (!midiTxReady()) return;  // MIDI disabled or nobody listening: drop rather than block
    const uint8_t status = static_cast<uint8_t>((pressed ? kMidiStatusNoteOn : kMidiStatusNoteOff) |
                                                (REKA_MIDI_TX_CHANNEL - 1));
    midiSend(status, static_cast<uint8_t>((kNoteBase[group] + ch) & 0x7F), pressed ? 127 : 0);
    midiFlush();
}

// Faders -> CC. Only changed 7-bit values are sent; a value that cannot be sent now is retried next loop.
static void serviceMidiTx() {
    bool sent = false;
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ++ch) {
        FaderState& f = gFader[ch];
        const uint8_t v7 = static_cast<uint8_t>(f.value >> 9);
        if (v7 == f.midiSent) continue;
        if (!midiTxReady()) break;
        midiSend(kMidiStatusCC | (REKA_MIDI_TX_CHANNEL - 1), static_cast<uint8_t>((REKA_MIDI_CC_BASE + ch) & 0x7F),
                 v7);
        f.midiSent = v7;
        sent = true;
    }
    if (sent) midiFlush();
}

static void enterMidiMode() {
    if (gMidiMode) return;
    gMidiMode = true;
    resetSurfaceState();
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ++ch) gChan[ch].flags = reka_proto::FLAG_MAPPED;
}

// MIDI feedback from a DAW drives the LEDs/OLED only while no serial host is connected.
static void handleMidiIn(uint8_t status, uint8_t data1, uint8_t data2) {
    if ((status & 0x0F) != REKA_MIDI_RX_CHANNEL - 1) return;
    if (gHostConnected) return;  // the serial host is the single source of truth
    const uint8_t type = status & 0xF0;
    if (type == kMidiStatusCC) {
        const uint8_t ch = static_cast<uint8_t>(data1 - REKA_MIDI_CC_BASE);
        if (ch >= NUM_CHANNELS) return;
        enterMidiMode();
        // 0..127 -> meter byte 0..240 (127 * 121 / 64 = 240)
        const uint8_t level = static_cast<uint8_t>((static_cast<uint16_t>(data2 & 0x7F) * 121U) >> 6);
        if (gChan[ch].meter != level) {
            gChan[ch].meter = level;
            gOledDirty = true;
        }
    } else if (type == kMidiStatusNoteOn || type == kMidiStatusNoteOff) {
        uint8_t flag;
        uint8_t ch;
        if ((ch = static_cast<uint8_t>(data1 - REKA_MIDI_NOTE_MUTE)) < NUM_CHANNELS) {
            flag = reka_proto::FLAG_MUTE;
        } else if ((ch = static_cast<uint8_t>(data1 - REKA_MIDI_NOTE_SOLO)) < NUM_CHANNELS) {
            flag = reka_proto::FLAG_SOLO;
        } else if ((ch = static_cast<uint8_t>(data1 - REKA_MIDI_NOTE_FX)) < NUM_CHANNELS) {
            flag = reka_proto::FLAG_FX_BYPASS;
        } else {
            return;
        }
        enterMidiMode();
        const bool on = type == kMidiStatusNoteOn && data2 != 0;
        const uint8_t flags = on ? static_cast<uint8_t>(gChan[ch].flags | flag)
                                 : static_cast<uint8_t>(gChan[ch].flags & ~flag);
        if (flags != gChan[ch].flags) {
            gChan[ch].flags = flags;
            gOledDirty = true;
        }
    }
}

static void pollMidi() {
    uint8_t status, data1, data2;
    for (uint8_t i = 0; i < 16 && midiRead(&status, &data1, &data2); ++i) handleMidiIn(status, data1, data2);
}

// =====================================================================================================
//  Host link: transmit side
// =====================================================================================================
// True when a frame with `payloadLength` bytes can be written without blocking. Frame on the wire =
// type + payload + crc, plus one COBS code byte (frames are < 254 bytes) and the 0x00 delimiter.
static bool txReady(uint8_t payloadLength) {
    const int frameLength = payloadLength + 4;
#if defined(ARDUINO_ARCH_AVR) && defined(USBCON)
    // ATmega32U4 CDC: `if (Serial)` contains delay(10), so poll DTR directly. Like MIDI above, writing
    // exactly the free space would make USB_Send() wait for a ZLP slot, hence the strict comparison.
    return Serial.dtr() && Serial.availableForWrite() > frameLength;
#elif defined(TEENSYDUINO)
    return Serial.dtr() && Serial.availableForWrite() >= frameLength;
#else
    return Serial.availableForWrite() >= frameLength;
#endif
}

static void sendFrame(reka_proto::FrameWriter& writer) {
    uint8_t encoded[reka_proto::kMaxEncodedFrame];
    const uint8_t length = writer.finish(encoded);
    if (length != 0) Serial.write(encoded, length);
}

static uint8_t capabilities() {
    uint8_t caps = reka_proto::CAP_FADERS;
#if REKA_ENABLE_BUTTONS
    for (uint8_t g = 0; g < kGroupCount; ++g) {
        if (kButtonPins[g] != REKA_PIN_NONE) caps |= reka_proto::CAP_BUTTONS;
    }
#endif
#if REKA_ENABLE_LEDS
    caps |= reka_proto::CAP_LEDS;
#endif
#if REKA_ENABLE_OLED
    if (gOledPresent) caps |= reka_proto::CAP_OLED;
#endif
#if REKA_USB_MIDI
    caps |= reka_proto::CAP_USB_MIDI;
#endif
    return caps;
}

// Sends whatever is pending, in priority order, as long as the port has room. Anything that does not
// fit stays pending and is retried on the next loop() iteration.
static void serviceHostTx() {
    reka_proto::FrameWriter w;

    if (gPongPending && txReady(1)) {
        w.begin(reka_proto::MSG_PONG);
        w.u8(gPongToken);
        sendFrame(w);
        gPongPending = false;
    }

    // HELLO, then the snapshot, before any other device -> host traffic.
    if (gHelloPending) {
        if (!txReady(5 + sizeof(reka_proto::kMagic))) return;
        w.begin(reka_proto::MSG_HELLO);
        w.u8(reka_proto::kProtocolVersion);
        w.u8(REKA_FW_VERSION_MAJOR);
        w.u8(REKA_FW_VERSION_MINOR);
        w.u8(NUM_CHANNELS);
        w.u8(capabilities());
        for (uint8_t i = 0; i < sizeof(reka_proto::kMagic); ++i) w.u8(reka_proto::kMagic[i]);
        sendFrame(w);
        gHelloPending = false;
        gSnapshotPending = true;
        gSnapshotNext = 0;
    }
    while (gSnapshotPending) {
        uint8_t count = static_cast<uint8_t>(NUM_CHANNELS - gSnapshotNext);
        if (count > kSnapshotPerFrame) count = kSnapshotPerFrame;
        if (!txReady(static_cast<uint8_t>(2 + 2 * count))) return;
        w.begin(reka_proto::MSG_FADER_SNAPSHOT);
        w.u8(gSnapshotNext);
        w.u8(count);
        for (uint8_t i = 0; i < count; ++i) {
            FaderState& f = gFader[gSnapshotNext + i];
            w.u16(f.value);
            f.sent = f.value;
        }
        sendFrame(w);
        gSnapshotNext = static_cast<uint8_t>(gSnapshotNext + count);
        if (gSnapshotNext >= NUM_CHANNELS) gSnapshotPending = false;
    }

    if (!gHostConnected) return;  // events only go to a host that is actually talking to us

    while (gBtnQueueCount != 0) {
        if (!txReady(3)) return;
        const uint8_t event = gBtnQueue[gBtnQueueHead];
        gBtnQueueHead = static_cast<uint8_t>((gBtnQueueHead + 1) & (kButtonQueueSize - 1));
        --gBtnQueueCount;
        w.begin(reka_proto::MSG_BUTTON);
        w.u8(event & 0x0F);
        w.u8((event >> 4) & 0x07);
        w.u8((event & 0x80) ? 1 : 0);
        sendFrame(w);
    }

    // Faders: rate limited per channel, but `value` persists, so the resting position always goes out.
    const uint8_t now8 = static_cast<uint8_t>(millis());
    for (uint8_t i = 0; i < NUM_CHANNELS; ++i) {
        uint8_t ch = static_cast<uint8_t>(gFaderCursor + i);
        if (ch >= NUM_CHANNELS) ch = static_cast<uint8_t>(ch - NUM_CHANNELS);
        FaderState& f = gFader[ch];
        if (f.value == f.sent) continue;
        if (static_cast<uint8_t>(now8 - f.lastSendMs) < REKA_FADER_MIN_INTERVAL_MS) continue;
        if (!txReady(3)) break;
        w.begin(reka_proto::MSG_FADER);
        w.u8(ch);
        w.u16(f.value);
        sendFrame(w);
        f.sent = f.value;
        f.lastSendMs = now8;
    }
    if (++gFaderCursor >= NUM_CHANNELS) gFaderCursor = 0;
}

static void enqueueButton(uint8_t ch, uint8_t group, bool pressed) {
    if (gBtnQueueCount >= kButtonQueueSize) return;  // host is not draining: drop the newest event
    const uint8_t slot = static_cast<uint8_t>((gBtnQueueHead + gBtnQueueCount) & (kButtonQueueSize - 1));
    gBtnQueue[slot] = static_cast<uint8_t>(ch | (group << 4) | (pressed ? 0x80 : 0x00));
    ++gBtnQueueCount;
}

// =====================================================================================================
//  Host link: receive side
// =====================================================================================================
static void markHostAlive() {
    gLastHeardMs = millis();
    if (!gHostConnected) {
        gHostConnected = true;
        gMidiMode = false;
        resetSurfaceState();  // the host re-sends the state it wants us to show
    }
}

static void onChannelState(const uint8_t* p, uint8_t n) {
    if (n < 5 || p[0] >= NUM_CHANNELS) return;
    ChannelState& c = gChan[p[0]];
    const uint8_t flags = p[1];
    const int16_t gain = reka_proto::readI16(p + 2);
    const int8_t pickup = static_cast<int8_t>(p[4]);
    if ((flags & reka_proto::FLAG_CLIP) && !(c.flags & reka_proto::FLAG_CLIP)) triggerClip(p[0]);
    if (c.flags != flags || c.gain != gain || c.pickup != pickup) {
        c.flags = flags;
        c.gain = gain;
        c.pickup = pickup;
        gOledDirty = true;
    }
}

// Copies a length-prefixed string, replacing anything the OLED fonts cannot draw.
static void copyName(char* dst, const uint8_t* src, uint8_t length) {
    for (uint8_t i = 0; i < length; ++i) {
        const uint8_t c = src[i];
        dst[i] = (c >= 0x20 && c <= 0x7E) ? static_cast<char>(c) : '?';
    }
    dst[length] = '\0';
}

static void onChannelName(const uint8_t* p, uint8_t n) {
    if (n < 2) return;
    const uint8_t ch = p[0];
    const uint8_t length = p[1];
    if (ch >= NUM_CHANNELS || length > reka_proto::kNameLength || n < 2 + length) return;
    copyName(gChan[ch].name, p + 2, length);
    gOledDirty = true;
}

static void onMeters(const uint8_t* p, uint8_t n) {
    if (n < 2) return;
    const uint8_t first = p[0];
    const uint8_t count = p[1];
    if (n < 2 + count) return;
    for (uint8_t i = 0; i < count; ++i) {
        const uint16_t ch = static_cast<uint16_t>(first + i);
        if (ch >= NUM_CHANNELS) break;  // the host may have more strips than we have faders
        const uint8_t level = p[2 + i];
        if (level == reka_proto::kMeterClip) triggerClip(static_cast<uint8_t>(ch));
        if (gChan[ch].meter != level) {
            gChan[ch].meter = level;
            gOledDirty = true;
        }
    }
}

static void onProfileName(const uint8_t* p, uint8_t n) {
    if (n < 1) return;
    const uint8_t length = p[0];
    if (length > reka_proto::kProfileNameLength || n < 1 + length) return;
    copyName(gProfile, p + 1, length);
    gOledDirty = true;
}

static void onDisplayConfig(const uint8_t* p, uint8_t n) {
    if (n < 3) return;
    gLedBrightness = p[0];
    if (p[1] != gOledContrast) {
        gOledContrast = p[1];
        gContrastPending = true;
    }
    const uint8_t mapped = p[2] > NUM_CHANNELS ? static_cast<uint8_t>(NUM_CHANNELS) : p[2];
    if (mapped != gNumMapped) {
        gNumMapped = mapped;
        gOledDirty = true;
    }
}

static void handleFrame() {
    const uint8_t type = gParser.type();
    if (type < 0x80) return;  // device -> host types (e.g. a loopback) are not host traffic
    const uint8_t* p = gParser.payload();
    const uint8_t n = gParser.payloadSize();
    markHostAlive();  // any CRC-valid host frame keeps the link up, even a type we do not know

    switch (type) {
        case reka_proto::MSG_HELLO_REQUEST:
            if (n >= 1) {
                gHelloPending = true;  // (re)starts HELLO + snapshot
                gSnapshotPending = false;
            }
            break;
        case reka_proto::MSG_PING:
            if (n >= 1) {
                gPongToken = p[0];
                gPongPending = true;
            }
            break;
        case reka_proto::MSG_CHANNEL_STATE: onChannelState(p, n); break;
        case reka_proto::MSG_CHANNEL_NAME: onChannelName(p, n); break;
        case reka_proto::MSG_METERS: onMeters(p, n); break;
        case reka_proto::MSG_PROFILE_NAME: onProfileName(p, n); break;
        case reka_proto::MSG_SELECT:
            if (n >= 1 && p[0] < NUM_CHANNELS) selectChannel(p[0]);
            break;
        case reka_proto::MSG_DISPLAY_CONFIG: onDisplayConfig(p, n); break;
        default: break;  // newer protocol revision: ignore
    }
}

static void pollHostRx() {
    // Bounded so a flood of host data cannot starve the scan loop; the rest is read next iteration.
    for (uint8_t budget = 96; budget != 0; --budget) {
        const int c = Serial.read();
        if (c < 0) break;
        if (gParser.feed(static_cast<uint8_t>(c))) handleFrame();
    }
}

static void updateLink(uint32_t now) {
#if REKA_NATIVE_USB_SERIAL
    // Native USB knows when the host application opens/closes the port (DTR).
    const bool open = Serial.dtr();
    if (open && !gPortWasOpen) {
        gHelloPending = true;  // announce ourselves to whoever just opened the port
        gSnapshotPending = false;
        gParser.reset();
    } else if (!open && gPortWasOpen && gHostConnected) {
        gHostConnected = false;  // port closed: no need to wait for the timeout
        gBtnQueueCount = 0;
        gOledDirty = true;
    }
    gPortWasOpen = open;
#endif
    if (gHostConnected && static_cast<uint32_t>(now - gLastHeardMs) > REKA_HOST_TIMEOUT_MS) {
        gHostConnected = false;
        gBtnQueueCount = 0;
        gOledDirty = true;
    }
}

// =====================================================================================================
//  Surface scanning: faders and buttons
// =====================================================================================================
static void muxSelect(uint8_t ch) {
    const uint8_t changed = static_cast<uint8_t>(ch ^ gMuxSelect);  // only touch lines that change
    if (changed & 0x01) digitalWrite(REKA_PIN_MUX_S0, (ch & 0x01) ? HIGH : LOW);
    if (changed & 0x02) digitalWrite(REKA_PIN_MUX_S1, (ch & 0x02) ? HIGH : LOW);
    if (changed & 0x04) digitalWrite(REKA_PIN_MUX_S2, (ch & 0x04) ? HIGH : LOW);
    if (changed & 0x08) digitalWrite(REKA_PIN_MUX_S3, (ch & 0x08) ? HIGH : LOW);
    gMuxSelect = ch;
}

// Reads the currently selected fader and scales it to 0..65535 (bit replication: full scale -> 65535).
static uint16_t readFaderSample() {
#if defined(ARDUINO_ARCH_AVR)
    // The ADC sample-and-hold capacitor still holds the previous mux channel; a throw-away conversion
    // lets it settle on the new one before the conversion we keep.
    (void)analogRead(REKA_PIN_FADER_SIG);
#endif
    const unsigned int raw = static_cast<unsigned int>(analogRead(REKA_PIN_FADER_SIG));
    const uint16_t value =
        static_cast<uint16_t>((raw << (16 - REKA_ADC_BITS)) | (raw >> (2 * REKA_ADC_BITS - 16)));
#if REKA_FADER_INVERT
    return static_cast<uint16_t>(65535U - value);
#else
    return value;
#endif
}

// Filtered value -> reported position: exact 0 / 65535 inside the end zones, linear in between.
static uint16_t faderPosition(uint16_t filtered) {
    if (filtered <= REKA_FADER_END_ZONE) return 0;
    const uint16_t x = static_cast<uint16_t>(filtered - REKA_FADER_END_ZONE);
    if (x >= kFaderSpan) return 65535U;
    return static_cast<uint16_t>((static_cast<uint32_t>(x) * kFaderStretch) >> 16);
}

static void processFader(uint8_t ch, uint16_t sample) {
    FaderState& f = gFader[ch];
    // Two-speed integer EMA with rounding: filtered += (sample - filtered) / 2^SHIFT, with the fast
    // coefficient while the fader is clearly moving. Never overshoots the sample.
    const int32_t diff = static_cast<int32_t>(sample) - static_cast<int32_t>(f.filtered);
    const bool moving = diff > REKA_FADER_FAST_THRESHOLD || diff < -REKA_FADER_FAST_THRESHOLD;
    const int32_t step = moving ? ((diff + kEmaFastRound) >> REKA_FADER_EMA_FAST_SHIFT)
                                : ((diff + kEmaRound) >> REKA_FADER_EMA_SHIFT);
    f.filtered = static_cast<uint16_t>(static_cast<int32_t>(f.filtered) + step);

    // Hysteresis: report only moves larger than the deadband, but always land exactly on the ends.
    const uint16_t target = faderPosition(f.filtered);
    const uint16_t delta = target > f.value ? static_cast<uint16_t>(target - f.value)
                                            : static_cast<uint16_t>(f.value - target);
    const bool reachedEnd = (target == 0 || target == 65535U) && target != f.value;
    if (delta > REKA_FADER_DEADBAND || reachedEnd) {
        f.value = target;
        selectChannel(ch);  // the touched channel becomes the one shown large on the OLED
        gOledDirty = true;
    }
}

static void onButtonEdge(uint8_t ch, uint8_t group, bool pressed) {
    if (pressed) selectChannel(ch);
    gOledDirty = true;  // the waiting screen shows held buttons (bring-up aid)
    if (gHostConnected) enqueueButton(ch, group, pressed);  // the host answers with MSG_CHANNEL_STATE
    midiSendButton(ch, group, pressed);
}

// Time-based debounce: a level must be stable for REKA_DEBOUNCE_MS before it becomes the new state.
static void scanButtons(uint8_t ch, uint8_t now8) {
    if (!REKA_ENABLE_BUTTONS) return;  // compiled out, but still type-checked
    const uint16_t bit = static_cast<uint16_t>(1U << ch);
    for (uint8_t g = 0; g < kGroupCount; ++g) {
        const uint8_t pin = kButtonPins[g];
        if (pin == REKA_PIN_NONE) continue;
        const bool down = digitalRead(pin) == LOW;  // INPUT_PULLUP, switch to GND
        if (down != ((gBtnRaw[g] & bit) != 0)) {
            gBtnRaw[g] ^= bit;
            gBtnChangeMs[g][ch] = now8;
        } else if (down != ((gBtnStable[g] & bit) != 0) &&
                   static_cast<uint8_t>(now8 - gBtnChangeMs[g][ch]) >= REKA_DEBOUNCE_MS) {
            gBtnStable[g] ^= bit;
            onButtonEdge(ch, g, down);
        }
    }
}

static void scanSurface() {
#if REKA_SCAN_INTERVAL_US > 0
    const uint32_t nowUs = micros();
    if (static_cast<uint32_t>(nowUs - gLastScanUs) < REKA_SCAN_INTERVAL_US) return;
    gLastScanUs = nowUs;
#endif
    const uint8_t now8 = static_cast<uint8_t>(millis());
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ++ch) {
        muxSelect(ch);
        delayMicroseconds(REKA_MUX_SETTLE_US);
        const uint16_t sample = readFaderSample();
        scanButtons(ch, now8);  // after the conversion: the button SIG pull-ups get extra settling time
        processFader(ch, sample);
    }
}

// Boot: start the filters at the real fader positions and adopt held buttons without sending events.
static void seedInputs() {
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ++ch) {
        muxSelect(ch);
        delayMicroseconds(REKA_MUX_SETTLE_US + 50);
        uint32_t sum = 0;
        for (uint8_t i = 0; i < 4; ++i) sum += readFaderSample();
        FaderState& f = gFader[ch];
        f.filtered = static_cast<uint16_t>(sum >> 2);
        f.value = faderPosition(f.filtered);
        f.sent = f.value;
        f.midiSent = static_cast<uint8_t>(f.value >> 9);
        f.lastSendMs = 0;
#if REKA_ENABLE_BUTTONS
        for (uint8_t g = 0; g < kGroupCount; ++g) {
            if (kButtonPins[g] == REKA_PIN_NONE || digitalRead(kButtonPins[g]) != LOW) continue;
            gBtnRaw[g] = static_cast<uint16_t>(gBtnRaw[g] | (1U << ch));
            gBtnStable[g] = static_cast<uint16_t>(gBtnStable[g] | (1U << ch));
        }
#endif
    }
}

// =====================================================================================================
//  WS2812B LEDs
// =====================================================================================================
// Strip primitives: the only NeoPixel-specific code. Colours are NeoPixel-packed 0x00RRGGBB.
static bool ledSetPixel(uint8_t ch, uint32_t color) {
#if REKA_ENABLE_LEDS
    if (gStrip.getPixelColor(ch) == color) return false;  // exact: setBrightness() is never used
    gStrip.setPixelColor(ch, color);
    return true;
#else
    (void)ch;
    (void)color;
    return false;
#endif
}

static void ledShow() {
#if REKA_ENABLE_LEDS
    gStrip.show();  // disables interrupts for ~30 us per LED, so it is only called when a pixel changed
#endif
}

// Colour of one channel while a host (or MIDI) drives the surface, before global brightness.
static void channelColor(uint8_t ch, uint16_t now16, uint8_t* rgb) {
    rgb[0] = rgb[1] = rgb[2] = 0;
    if (!channelMapped(ch)) return;  // unmapped: off
    const ChannelState& c = gChan[ch];
    if (gClipMask & (1U << ch)) {  // clip: white flash
        rgb[0] = rgb[1] = rgb[2] = 255;
        return;
    }
    if (c.flags & reka_proto::FLAG_MUTE) {  // mute: solid red
        rgb[0] = 255;
        return;
    }
    if (c.flags & reka_proto::FLAG_SOLO) {
        // solo: amber, pulsing at ~1 Hz between ~38 % and 100 %
        const uint8_t phase = static_cast<uint8_t>(now16 >> 2);
        const uint8_t tri = phase < 128 ? static_cast<uint8_t>(phase << 1) : static_cast<uint8_t>((255 - phase) << 1);
        const uint8_t level = static_cast<uint8_t>(96 + ((static_cast<uint16_t>(tri) * 5U) >> 3));
        rgb[0] = level;
        rgb[1] = scaleBrightness(110, level);
    } else if (c.flags & reka_proto::FLAG_SOLO_SAFE) {
        rgb[0] = 48;  // implicitly muted by another channel's solo: dim red
    } else {
        // Level: green below -30 dB, yellow at -15 dB, red at 0 dB; brighter as the level rises.
        const uint8_t m = c.meter > reka_proto::kMeterZeroDb ? reka_proto::kMeterZeroDb : c.meter;
        const uint8_t red = m <= 120 ? 0 : (m >= 180 ? 255 : static_cast<uint8_t>(((m - 120) * 17U) >> 2));
        const uint8_t green = m <= 180 ? 255 : static_cast<uint8_t>(((240 - m) * 17U) >> 2);
        const uint8_t intensity = static_cast<uint8_t>(20 + ((static_cast<uint16_t>(m) * 5U) >> 3));
        rgb[0] = scaleBrightness(red, intensity);
        rgb[1] = scaleBrightness(green, intensity);
    }
    if (c.flags & reka_proto::FLAG_FX_BYPASS) {  // FX bypass: cyan tint
        rgb[1] = addSaturate(rgb[1], 40);
        rgb[2] = addSaturate(rgb[2], 72);
    }
}

// No host: slow (~4 s) breathing blue, eased so it lingers near dark.
static void idleColor(uint32_t now, uint8_t* rgb) {
    const uint16_t phase = static_cast<uint16_t>(now >> 3) & 0x1FF;
    const uint8_t tri = phase < 256 ? static_cast<uint8_t>(phase) : static_cast<uint8_t>(511 - phase);
    uint8_t level = static_cast<uint8_t>((static_cast<uint16_t>(tri) * tri) >> 8);
    if (level < 6) level = 6;
    rgb[0] = 0;
    rgb[1] = static_cast<uint8_t>(level >> 4);
    rgb[2] = level;
}

// Recomputes every LED at most every REKA_LED_FRAME_MS and pushes the strip only if something changed.
static void serviceLeds(uint32_t now) {
    if (!REKA_ENABLE_LEDS) return;  // compiled out, but still type-checked
    if (static_cast<uint32_t>(now - gLedLastMs) < REKA_LED_FRAME_MS) return;
    gLedLastMs = now;
    const uint16_t now16 = static_cast<uint16_t>(now);
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ++ch) {
        const uint16_t bit = static_cast<uint16_t>(1U << ch);
        if ((gClipMask & bit) && static_cast<uint16_t>(now16 - gClipStartMs[ch]) >= REKA_CLIP_HOLD_MS) {
            gClipMask = static_cast<uint16_t>(gClipMask & ~bit);
        }
    }
    const bool idle = linkMode() == LINK_NONE;
    bool changed = false;
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ++ch) {
        uint8_t rgb[3];
        if (idle) {
            idleColor(now, rgb);
        } else {
            channelColor(ch, now16, rgb);
        }
        const uint32_t color = (static_cast<uint32_t>(scaleBrightness(rgb[0], gLedBrightness)) << 16) |
                               (static_cast<uint32_t>(scaleBrightness(rgb[1], gLedBrightness)) << 8) |
                               scaleBrightness(rgb[2], gLedBrightness);
        if (ledSetPixel(ch, color)) changed = true;
    }
    if (changed) ledShow();
}

// =====================================================================================================
//  SSD1306 OLED
// =====================================================================================================
#if REKA_ENABLE_OLED
// Screen layout (128 x 64). Static members of one struct so the whole renderer compiles out with the
// OLED. In page mode draw() runs once per 8-pixel page, so it must draw the same thing every time:
// it only reads state latched at frame start (gViewMode, gViewSel) plus the live channel data.
struct OledView {
    // Writes v in decimal plus a terminator; returns the number of characters.
    static uint8_t formatUint(char* out, uint16_t v) {
        char tmp[5];
        uint8_t n = 0;
        do {
            tmp[n++] = static_cast<char>('0' + v % 10U);
            v = static_cast<uint16_t>(v / 10U);
        } while (v != 0 && n < sizeof(tmp));
        for (uint8_t i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
        out[n] = '\0';
        return n;
    }

    // Gain in tenths of a dB -> "-12.3", "+3.0", "0.0" or "-inf" (needs 8 bytes).
    static uint8_t formatGain(char* out, int16_t tenths) {
        if (tenths == reka_proto::kGainMinusInfinity) {
            out[0] = '-';
            out[1] = 'i';
            out[2] = 'n';
            out[3] = 'f';
            out[4] = '\0';
            return 4;
        }
        uint8_t n = 0;
        uint16_t magnitude;
        if (tenths < 0) {
            out[n++] = '-';
            magnitude = static_cast<uint16_t>(-static_cast<int32_t>(tenths));
        } else {
            if (tenths > 0) out[n++] = '+';
            magnitude = static_cast<uint16_t>(tenths);
        }
        n = static_cast<uint8_t>(n + formatUint(out + n, static_cast<uint16_t>(magnitude / 10U)));
        out[n++] = '.';
        out[n++] = static_cast<char>('0' + magnitude % 10U);
        out[n] = '\0';
        return n;
    }

    // Flash-resident string at a baseline position (current font).
    static void text(uint8_t x, uint8_t y, const __FlashStringHelper* s) {
        gOled.setCursor(x, y);
        gOled.print(s);
    }

    // Status badge in the gain row, 6x10 font. state: 0 = off (outline), 1 = on (filled),
    // 2 = implied (double outline: muted by another channel's solo).
    static void badge(uint8_t x, uint8_t width, const __FlashStringHelper* label, uint8_t chars, uint8_t state) {
        const uint8_t y = 31;
        const uint8_t height = 11;
        const uint8_t textX = static_cast<uint8_t>(x + (width - 6 * chars) / 2);
        if (state == 1) {
            gOled.drawBox(x, y, width, height);
            gOled.setDrawColor(0);
            text(textX, static_cast<uint8_t>(y + 9), label);
            gOled.setDrawColor(1);
            return;
        }
        gOled.drawFrame(x, y, width, height);
        if (state == 2) {
            gOled.drawFrame(static_cast<u8g2_uint_t>(x + 1), static_cast<u8g2_uint_t>(y + 1),
                            static_cast<u8g2_uint_t>(width - 2), static_cast<u8g2_uint_t>(height - 2));
        }
        text(textX, static_cast<uint8_t>(y + 9), label);
    }

    // Soft-takeover hint: 9 x 5 triangle pointing the way the physical fader has to move.
    static void pickupArrow(uint8_t x, uint8_t y, bool up) {
        for (uint8_t i = 0; i < 5; ++i) {
            const uint8_t row = static_cast<uint8_t>(up ? y + i : y + 4 - i);
            gOled.drawHLine(static_cast<u8g2_uint_t>(x + 4 - i), row, static_cast<u8g2_uint_t>(2 * i + 1));
        }
    }

    // One bar per channel across the full width: live fader positions (waiting screen) or meters.
    // Fader mode also marks held buttons in the three rows below the bars: left = mute, middle = solo,
    // right = FX (a bring-up aid that needs no host).
    static void strip(uint8_t top, uint8_t height, bool showFaders) {
        const uint8_t slot = kOledWidth / NUM_CHANNELS;
        const uint8_t barWidth = slot > 3 ? static_cast<uint8_t>(slot - 2) : 1;
        const uint8_t x0 = static_cast<uint8_t>((kOledWidth - slot * NUM_CHANNELS) / 2 + (slot - barWidth) / 2);
        const uint8_t bottom = static_cast<uint8_t>(top + height - 1);
        for (uint8_t ch = 0; ch < NUM_CHANNELS; ++ch) {
            const uint8_t x = static_cast<uint8_t>(x0 + ch * slot);
            uint8_t h;
            if (showFaders) {
                h = static_cast<uint8_t>(1 + ((static_cast<uint32_t>(gFader[ch].value) * (height - 1)) >> 16));
                const uint8_t markWidth = barWidth >= 3 ? static_cast<uint8_t>(barWidth / 3) : 1;
                for (uint8_t g = 0; g < kGroupCount; ++g) {
                    if (!(gBtnStable[g] & (1U << ch))) continue;
                    const uint8_t mx = static_cast<uint8_t>(x + g * (barWidth - markWidth) / 2);
                    gOled.drawBox(mx, static_cast<u8g2_uint_t>(bottom + 2), markWidth, 3);
                }
            } else {
                const ChannelState& c = gChan[ch];
                if (ch == gViewSel) gOled.drawHLine(x, static_cast<u8g2_uint_t>(bottom + 2), barWidth);
                if (!channelMapped(ch)) {
                    gOled.drawPixel(static_cast<u8g2_uint_t>(x + barWidth / 2), bottom);  // unmapped: a dot
                    continue;
                }
                if (c.flags & (reka_proto::FLAG_MUTE | reka_proto::FLAG_SOLO_SAFE)) {
                    gOled.drawFrame(x, top, barWidth, height);  // muted: hollow bar
                    continue;
                }
                if (c.meter >= reka_proto::kMeterZeroDb) {
                    h = height;
                } else {
                    h = static_cast<uint8_t>((static_cast<uint16_t>(c.meter) * height + reka_proto::kMeterZeroDb - 1) /
                                             reka_proto::kMeterZeroDb);
                    if (h == 0) h = 1;  // mapped but silent: baseline
                }
            }
            gOled.drawBox(x, static_cast<u8g2_uint_t>(bottom + 1 - h), barWidth, h);
        }
    }

    // (a) No host: identity, protocol version, live fader positions.
    static void waitingScreen() {
        char buf[8];
        gOled.drawBox(0, 0, kOledWidth, 16);
        gOled.setDrawColor(0);
        gOled.setFont(u8g2_font_helvB12_tr);
        text(3, 14, F("REKA"));
        gOled.setFont(u8g2_font_6x10_tr);
        uint8_t n = 0;
        buf[n++] = 'v';
        n = static_cast<uint8_t>(n + formatUint(buf + n, REKA_FW_VERSION_MAJOR));
        buf[n++] = '.';
        n = static_cast<uint8_t>(n + formatUint(buf + n, REKA_FW_VERSION_MINOR));
        gOled.drawStr(static_cast<u8g2_uint_t>(kOledWidth - 3 - 6 * n), 12, buf);
        gOled.setDrawColor(1);

        text(0, 28, F("Waiting for REKA..."));
        text(0, 40, F("proto v"));
        formatUint(buf, reka_proto::kProtocolVersion);
        gOled.drawStr(42, 40, buf);
        n = formatUint(buf, NUM_CHANNELS);
        gOled.drawStr(60, 40, buf);
        text(static_cast<uint8_t>(60 + 6 * n), 40, F(" ch"));
#if REKA_USB_MIDI
        text(104, 40, F("MIDI"));
#endif
        strip(44, 16, true);  // handy during bring-up: every fader and button, no host needed
    }

    // (b) Host (or MIDI) connected: selected channel large, mini meters for all channels.
    static void channelScreen() {
        const uint8_t ch = gViewSel;
        const ChannelState& c = gChan[ch];
        const bool midi = gViewMode == LINK_MIDI;
        char buf[12];

        // Header: profile name (or mode) and "channel/total"
        gOled.setFont(u8g2_font_6x10_tr);
        if (midi) {
            text(0, 8, F("USB-MIDI"));
        } else if (gProfile[0] != '\0') {
            gOled.drawStr(0, 8, gProfile);
        } else {
            text(0, 8, F("REKA"));
        }
        uint8_t n = formatUint(buf, static_cast<uint16_t>(ch + 1));
        buf[n++] = '/';
        n = static_cast<uint8_t>(n + formatUint(buf + n, NUM_CHANNELS));
        gOled.drawStr(static_cast<u8g2_uint_t>(kOledWidth - 6 * n), 8, buf);
        gOled.drawHLine(0, 10, kOledWidth);

        // Channel name, large ("Ch n" until the host names it)
        gOled.setFont(u8g2_font_helvB12_tr);
        if (c.name[0] != '\0') {
            gOled.drawStr(0, 27, c.name);
        } else {
            buf[0] = 'C';
            buf[1] = 'h';
            buf[2] = ' ';
            formatUint(buf + 3, static_cast<uint16_t>(ch + 1));
            gOled.drawStr(0, 27, buf);
        }
        if (!midi && (c.flags & reka_proto::FLAG_CLIP)) {
            gOled.setFont(u8g2_font_6x10_tr);
            gOled.drawBox(100, 15, 28, 12);
            gOled.setDrawColor(0);
            text(102, 24, F("CLIP"));
            gOled.setDrawColor(1);
        }

        // Gain from the host, or the fader position in MIDI mode (the DAW's gain is unknown)
        gOled.setFont(u8g2_font_helvB12_tr);
        if (midi) {
            n = formatUint(buf, static_cast<uint16_t>((static_cast<uint32_t>(gFader[ch].value) * 100U + 32767U) >> 16));
            buf[n++] = '%';
            buf[n] = '\0';
            gOled.drawStr(0, 43, buf);
        } else {
            formatGain(buf, c.gain);
            const uint8_t width = static_cast<uint8_t>(gOled.drawStr(0, 43, buf));
            gOled.setFont(u8g2_font_6x10_tr);
            text(static_cast<uint8_t>(width + 3), 43, F("dB"));
        }

        // Mute / solo / FX-bypass badges
        gOled.setFont(u8g2_font_6x10_tr);
        const uint8_t muteState =
            (c.flags & reka_proto::FLAG_MUTE) ? 1 : ((c.flags & reka_proto::FLAG_SOLO_SAFE) ? 2 : 0);
        badge(84, 12, F("M"), 1, muteState);
        badge(98, 12, F("S"), 1, (c.flags & reka_proto::FLAG_SOLO) ? 1 : 0);
        badge(112, 16, F("FX"), 2, (c.flags & reka_proto::FLAG_FX_BYPASS) ? 1 : 0);

        // Physical fader position + soft-takeover arrow
        gOled.drawFrame(0, 45, 114, 6);
        const uint8_t fill = static_cast<uint8_t>((static_cast<uint32_t>(gFader[ch].value) * 111U) >> 16);
        if (fill != 0) gOled.drawBox(2, 47, fill, 2);
        if (!midi && c.pickup != reka_proto::PICKUP_SYNCED) pickupArrow(118, 45, c.pickup > 0);

        // Mini meters for every channel, the selected one underlined
        strip(53, 9, false);
    }

    static void draw() {
        if (gViewMode == LINK_NONE) {
            waitingScreen();
        } else {
            channelScreen();
        }
    }
};
#endif  // REKA_ENABLE_OLED

static void oledBegin() {
#if REKA_ENABLE_OLED
#if defined(ARDUINO_ARCH_ESP32)
    Wire.begin(REKA_PIN_I2C_SDA, REKA_PIN_I2C_SCL);
#else
    Wire.begin();
#endif
    Wire.setClock(REKA_I2C_CLOCK_HZ);
    // Probe first: without a display there is no point spending loop time on I2C transfers.
    Wire.beginTransmission(REKA_OLED_I2C_ADDRESS);
    gOledPresent = Wire.endTransmission() == 0;
    if (!gOledPresent) return;
    gOled.setI2CAddress(REKA_OLED_I2C_ADDRESS << 1);
    gOled.setBusClock(REKA_I2C_CLOCK_HZ);  // U8g2 re-applies this before every transfer
    gOled.begin();
    Wire.setClock(REKA_I2C_CLOCK_HZ);
#if defined(ARDUINO_ARCH_AVR)
    Wire.setWireTimeout(25000UL, true);  // a glitching bus must never hang the scan loop
#endif
    gOled.setFontMode(1);  // transparent text, so inverted badges work
    gOled.setContrast(gOledContrast);
    gOledDirty = true;
#endif
}

// Non-blocking renderer: at most one 128-byte page (1/8 of the screen, ~3 ms at 400 kHz) per call.
//   Page mode (AVR): firstPage(), then draw() + nextPage() once per loop() until nextPage() returns 0.
//   Full buffer: draw() the whole frame once, then push one tile row per loop() with
//   updateDisplayArea(); a plain nextPage()/sendBuffer() would push all 1 KB (~25 ms) in one go.
static void serviceOled(uint32_t now) {
#if REKA_ENABLE_OLED
    if (!gOledPresent) return;
    if (!gOledBusy) {
        if (gContrastPending) {
            gContrastPending = false;
            gOled.setContrast(gOledContrast);
        }
        if (!gOledDirty || static_cast<uint32_t>(now - gOledLastMs) < REKA_OLED_FRAME_MS) return;
        gOledDirty = false;
        gOledLastMs = now;
        gViewMode = linkMode();
        gViewSel = gSelected;
        gOledBusy = true;
#if REKA_OLED_PAGE_MODE
        gOled.firstPage();
#else
        gOled.clearBuffer();
        OledView::draw();
        gOledRow = 0;
#endif
    }
#if REKA_OLED_PAGE_MODE
    OledView::draw();
    if (!gOled.nextPage()) gOledBusy = false;
#else
    gOled.updateDisplayArea(0, gOledRow, gOled.getBufferTileWidth(), 1);
    if (++gOledRow >= gOled.getBufferTileHeight()) gOledBusy = false;
#endif
#else
    (void)now;
#endif
}

// =====================================================================================================
//  Arduino entry points
// =====================================================================================================
void setup() {
    pinMode(REKA_PIN_MUX_S0, OUTPUT);
    pinMode(REKA_PIN_MUX_S1, OUTPUT);
    pinMode(REKA_PIN_MUX_S2, OUTPUT);
    pinMode(REKA_PIN_MUX_S3, OUTPUT);
    muxSelect(0);
#if REKA_ENABLE_BUTTONS
    for (uint8_t g = 0; g < kGroupCount; ++g) {
        if (kButtonPins[g] != REKA_PIN_NONE) pinMode(kButtonPins[g], INPUT_PULLUP);
    }
#endif
#if defined(TEENSYDUINO)
    analogReadResolution(REKA_ADC_BITS);
    analogReadAveraging(4);
#elif defined(ARDUINO_ARCH_ESP32)
    analogReadResolution(REKA_ADC_BITS);
    analogSetPinAttenuation(REKA_PIN_FADER_SIG, ADC_11db);  // full 0..3.3 V range
    Serial.setTxBufferSize(256);
#endif
    Serial.begin(REKA_SERIAL_BAUD);

#if REKA_ENABLE_LEDS
    gStrip.begin();
    gStrip.show();  // all off
#endif
    resetSurfaceState();
    seedInputs();
    oledBegin();
    gHelloPending = true;  // HELLO + snapshot as soon as the port can take them
}

void loop() {
    pollHostRx();
    pollMidi();
    scanSurface();
    const uint32_t now = millis();
    updateLink(now);
    serviceHostTx();
    serviceMidiTx();
    serviceLeds(now);
    serviceOled(now);
}
