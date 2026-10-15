// =====================================================================================================
//  RekaProtocol.h - REKA control-surface wire protocol (shared by REKA.exe and REKA.ino)
//
//  This header is the single source of truth for the serial protocol. It is compiled by:
//    * avr-gcc (C++11, 2.5 KB RAM on a Pro Micro) -> no heap, no STL, no exceptions, no float in hot paths
//    * the desktop C++20 toolchain (reka_core includes it from firmware/REKA)
//
//  Transport : USB CDC / UART, 115200 8N1 (baud is ignored by native-USB boards but kept for UART boards)
//  Framing   : COBS-encoded frames terminated by 0x00.  Raw frame = [type][payload ...][crc8]
//  Integrity : CRC-8/SMBUS (poly 0x07, init 0x00) over [type][payload]
//  Endianness: multi-byte integers are big-endian
//
//  See docs/HARDWARE_PROTOCOL.md for the full message reference and the connection state machine.
// =====================================================================================================
#ifndef REKA_PROTOCOL_H
#define REKA_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace reka_proto {

static const uint8_t  kProtocolVersion   = 1;
static const uint8_t  kMaxChannels       = 16;
static const uint8_t  kNameLength        = 8;    // channel names shown on the OLED
static const uint8_t  kProfileNameLength = 16;
static const uint8_t  kMaxPayload        = 40;   // payload bytes, excluding type and crc
static const uint8_t  kMaxRawFrame       = kMaxPayload + 2;          // type + payload + crc
static const uint8_t  kMaxEncodedFrame   = kMaxRawFrame + 2 + 1;     // COBS overhead + 0x00 delimiter
static const uint32_t kDefaultBaud       = 115200UL;
static const uint8_t  kMagic[4]          = {'R', 'E', 'K', 'A'};  // trailer of MSG_HELLO

// ---- Meter encoding: 1 byte per channel ------------------------------------------------------------
//   0          -> silence (<= -60 dBFS)
//   1..240     -> -60 dBFS .. 0 dBFS in 0.25 dB steps   (dB = level / 4 - 60)
//   255        -> clipped (> 0 dBFS)
static const uint8_t kMeterFloorDb     = 60;
static const uint8_t kMeterStepsPerDb  = 4;
static const uint8_t kMeterZeroDb      = 240;
static const uint8_t kMeterClip        = 255;

// ---- Gain encoding: int16 tenths of a dB, kGainMinusInfinity == fader fully down -------------------
static const int16_t kGainMinusInfinity = -32768;

enum MessageType {
    // device -> host
    MSG_HELLO          = 0x01,  // [proto][fwMajor][fwMinor][numFaders][capabilities]['R']['E']['K']['A']
    MSG_FADER          = 0x02,  // [channel][value u16]            value: 0..65535 (normalised position)
    MSG_BUTTON         = 0x03,  // [channel][buttonId][pressed]
    MSG_FADER_SNAPSHOT = 0x04,  // [first][count][value u16]*count  full resync of physical positions
    MSG_PONG           = 0x05,  // [token]
    MSG_ENCODER        = 0x06,  // [channel][delta i8]             optional rotary encoders

    // host -> device
    MSG_HELLO_REQUEST  = 0x81,  // [proto]
    MSG_CHANNEL_STATE  = 0x82,  // [channel][flags][gain i16][pickup i8]
    MSG_CHANNEL_NAME   = 0x83,  // [channel][len][chars...]         len <= kNameLength
    MSG_METERS         = 0x84,  // [first][count][level u8]*count
    MSG_PROFILE_NAME   = 0x85,  // [len][chars...]                 len <= kProfileNameLength
    MSG_PING           = 0x86,  // [token]
    MSG_SELECT         = 0x87,  // [channel]                        channel shown large on the OLED
    MSG_DISPLAY_CONFIG = 0x88,  // [ledBrightness][oledContrast][numMappedChannels]
};

enum ButtonId { BUTTON_MUTE = 0, BUTTON_SOLO = 1, BUTTON_FX = 2, BUTTON_SELECT = 3 };

enum ChannelFlags {
    FLAG_MUTE      = 0x01,
    FLAG_SOLO      = 0x02,
    FLAG_FX_BYPASS = 0x04,
    FLAG_MAPPED    = 0x08,  // a strip is bound to this physical channel
    FLAG_SELECTED  = 0x10,
    FLAG_CLIP      = 0x20,  // sticky clip indicator (host clears it)
    FLAG_SOLO_SAFE = 0x40,  // channel is implicitly muted by another channel's solo
};

enum Capability {
    CAP_FADERS   = 0x01,
    CAP_BUTTONS  = 0x02,
    CAP_LEDS     = 0x04,
    CAP_OLED     = 0x08,
    CAP_USB_MIDI = 0x10,
    CAP_ENCODERS = 0x20,
};

// Pickup hint sent with MSG_CHANNEL_STATE (non-motorised faders use "soft takeover").
enum PickupHint { PICKUP_SYNCED = 0, PICKUP_MOVE_UP = 1, PICKUP_MOVE_DOWN = -1 };

// ---- CRC-8/SMBUS ------------------------------------------------------------------------------------
// Bitwise implementation: no 256-byte table, which matters on a 2.5 KB RAM / 28 KB flash ATmega32U4.
inline uint8_t crc8(const uint8_t* data, size_t length) {
    uint8_t crc = 0x00;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit)
            crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x07) : static_cast<uint8_t>(crc << 1);
    }
    return crc;
}

// ---- COBS (Consistent Overhead Byte Stuffing) --------------------------------------------------------
// Encodes `length` bytes into `out` (capacity >= length + length / 254 + 1). Returns the encoded size.
// The 0x00 frame delimiter is NOT appended.
inline size_t cobsEncode(const uint8_t* in, size_t length, uint8_t* out) {
    size_t readIndex = 0, writeIndex = 1, codeIndex = 0;
    uint8_t code = 1;
    while (readIndex < length) {
        if (in[readIndex] == 0) {
            out[codeIndex] = code;
            code = 1;
            codeIndex = writeIndex++;
            ++readIndex;
        } else {
            out[writeIndex++] = in[readIndex++];
            if (++code == 0xFF) {
                out[codeIndex] = code;
                code = 1;
                codeIndex = writeIndex++;
            }
        }
    }
    out[codeIndex] = code;
    return writeIndex;
}

// Decodes a COBS block (without the trailing 0x00). Returns the decoded size, or 0 on malformed input.
inline size_t cobsDecode(const uint8_t* in, size_t length, uint8_t* out, size_t outCapacity) {
    size_t readIndex = 0, writeIndex = 0;
    while (readIndex < length) {
        const uint8_t code = in[readIndex];
        if (code == 0 || readIndex + code > length) return 0;
        ++readIndex;
        for (uint8_t i = 1; i < code; ++i) {
            if (writeIndex >= outCapacity || readIndex >= length) return 0;
            const uint8_t byte = in[readIndex++];
            if (byte == 0) return 0;
            out[writeIndex++] = byte;
        }
        if (code != 0xFF && readIndex != length) {
            if (writeIndex >= outCapacity) return 0;
            out[writeIndex++] = 0;
        }
    }
    return writeIndex;
}

// ---- Value helpers ------------------------------------------------------------------------------------
inline uint16_t readU16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
inline int16_t  readI16(const uint8_t* p) { return static_cast<int16_t>(readU16(p)); }

// Desktop side: dBFS -> meter byte.
inline uint8_t encodeMeterDb(float db) {
    if (db > 0.05f) return kMeterClip;
    if (db <= -static_cast<float>(kMeterFloorDb)) return 0;
    const float steps = (db + static_cast<float>(kMeterFloorDb)) * static_cast<float>(kMeterStepsPerDb);
    const int level = static_cast<int>(steps + 0.5f);
    return static_cast<uint8_t>(level < 1 ? 1 : (level > kMeterZeroDb ? kMeterZeroDb : level));
}

// Desktop side: dB -> int16 tenths.
inline int16_t encodeGainDb(float db) {
    if (db <= -100.0f) return kGainMinusInfinity;
    const float tenths = db * 10.0f;
    return static_cast<int16_t>(tenths >= 0.0f ? tenths + 0.5f : tenths - 0.5f);
}

// ---- Frame writer -------------------------------------------------------------------------------------
class FrameWriter {
public:
    FrameWriter() : length_(0), overflow_(false) {}

    void begin(uint8_t type) {
        raw_[0] = type;
        length_ = 1;
        overflow_ = false;
    }
    void u8(uint8_t value) {
        if (length_ < kMaxRawFrame - 1) raw_[length_++] = value;
        else overflow_ = true;
    }
    void i8(int8_t value) { u8(static_cast<uint8_t>(value)); }
    void u16(uint16_t value) {
        u8(static_cast<uint8_t>(value >> 8));
        u8(static_cast<uint8_t>(value & 0xFF));
    }
    void i16(int16_t value) { u16(static_cast<uint16_t>(value)); }
    // Length-prefixed string, truncated to maxLength.
    void str(const char* text, uint8_t maxLength) {
        uint8_t n = 0;
        while (text != 0 && n < maxLength && text[n] != '\0') ++n;
        u8(n);
        for (uint8_t i = 0; i < n; ++i) u8(static_cast<uint8_t>(text[i]));
    }
    bool overflowed() const { return overflow_; }

    // Appends the CRC, COBS-encodes into `out` (capacity >= kMaxEncodedFrame) and appends the 0x00
    // delimiter. Returns the number of bytes to transmit, or 0 if the payload overflowed.
    uint8_t finish(uint8_t* out) {
        if (overflow_) return 0;
        raw_[length_] = crc8(raw_, length_);
        const size_t encoded = cobsEncode(raw_, static_cast<size_t>(length_) + 1, out);
        out[encoded] = 0x00;
        return static_cast<uint8_t>(encoded + 1);
    }

private:
    uint8_t raw_[kMaxRawFrame];
    uint8_t length_;
    bool overflow_;
};

// ---- Streaming frame parser ---------------------------------------------------------------------------
// Feed bytes one at a time; when feed() returns true a CRC-valid frame is available until the next feed().
class FrameParser {
public:
    FrameParser() : encodedLength_(0), rawLength_(0), discarding_(false), errors_(0) {}

    bool feed(uint8_t byte) {
        if (byte != 0x00) {
            if (discarding_) return false;
            if (encodedLength_ >= sizeof(encoded_)) {  // oversize frame: drop until next delimiter
                discarding_ = true;
                encodedLength_ = 0;
                ++errors_;
                return false;
            }
            encoded_[encodedLength_++] = byte;
            return false;
        }
        // Delimiter
        const uint8_t length = encodedLength_;
        encodedLength_ = 0;
        if (discarding_) {
            discarding_ = false;
            return false;
        }
        if (length == 0) return false;  // back-to-back delimiters are legal keep-alives
        const size_t decoded = cobsDecode(encoded_, length, raw_, sizeof(raw_));
        if (decoded < 2 || crc8(raw_, decoded - 1) != raw_[decoded - 1]) {
            ++errors_;
            return false;
        }
        rawLength_ = static_cast<uint8_t>(decoded);
        return true;
    }

    uint8_t type() const { return raw_[0]; }
    const uint8_t* payload() const { return raw_ + 1; }
    uint8_t payloadSize() const { return static_cast<uint8_t>(rawLength_ - 2); }
    uint16_t errorCount() const { return errors_; }
    void reset() {
        encodedLength_ = 0;
        discarding_ = false;
    }

private:
    uint8_t encoded_[kMaxEncodedFrame];
    uint8_t raw_[kMaxRawFrame + 1];
    uint8_t encodedLength_;
    uint8_t rawLength_;
    bool discarding_;
    uint16_t errors_;
};

}  // namespace reka_proto

#endif  // REKA_PROTOCOL_H
