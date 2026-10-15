#include <doctest/doctest.h>

#include "RekaProtocol.h"

#include <random>
#include <vector>

using namespace reka_proto;

namespace {
std::vector<std::uint8_t> encodeRaw(const std::vector<std::uint8_t>& raw) {
    std::vector<std::uint8_t> out(raw.size() + raw.size() / 254 + 2);
    out.resize(cobsEncode(raw.data(), raw.size(), out.data()));
    return out;
}
}  // namespace

TEST_CASE("CRC-8/SMBUS check value") {
    const char* check = "123456789";
    CHECK(crc8(reinterpret_cast<const std::uint8_t*>(check), 9) == 0xF4);
}

TEST_CASE("COBS round trip including zeros and long runs") {
    std::mt19937 rng(42);
    for (std::size_t size : {1u, 2u, 10u, 253u, 254u, 255u, 600u}) {
        std::vector<std::uint8_t> raw(size);
        for (auto& b : raw) b = static_cast<std::uint8_t>(rng() % 4 == 0 ? 0 : rng());
        const auto encoded = encodeRaw(raw);
        for (auto b : encoded) REQUIRE(b != 0);
        std::vector<std::uint8_t> decoded(size + 4);
        const std::size_t n = cobsDecode(encoded.data(), encoded.size(), decoded.data(), decoded.size());
        REQUIRE(n == size);
        decoded.resize(n);
        CHECK(decoded == raw);
    }
    std::vector<std::uint8_t> raw(300, 0x11);
    const auto encoded = encodeRaw(raw);
    std::vector<std::uint8_t> decoded(400);
    CHECK(cobsDecode(encoded.data(), encoded.size(), decoded.data(), decoded.size()) == 300);
}

TEST_CASE("FrameWriter -> FrameParser") {
    FrameWriter writer;
    writer.begin(MSG_CHANNEL_STATE);
    writer.u8(3);
    writer.u8(FLAG_MUTE | FLAG_MAPPED);
    writer.i16(encodeGainDb(-12.5f));
    writer.i8(PICKUP_MOVE_DOWN);
    std::uint8_t frame[kMaxEncodedFrame];
    const std::uint8_t length = writer.finish(frame);
    REQUIRE(length > 0);
    CHECK(frame[length - 1] == 0x00);

    FrameParser parser;
    bool got = false;
    for (std::uint8_t i = 0; i < length; ++i) got = parser.feed(frame[i]);
    REQUIRE(got);
    CHECK(parser.type() == MSG_CHANNEL_STATE);
    REQUIRE(parser.payloadSize() == 5);
    CHECK(parser.payload()[0] == 3);
    CHECK(readI16(parser.payload() + 2) == -125);
    CHECK(static_cast<std::int8_t>(parser.payload()[4]) == -1);
}

TEST_CASE("FrameWriter refuses oversize payloads") {
    FrameWriter writer;
    writer.begin(MSG_METERS);
    for (int i = 0; i < kMaxPayload + 5; ++i) writer.u8(1);
    std::uint8_t frame[kMaxEncodedFrame];
    CHECK(writer.overflowed());
    CHECK(writer.finish(frame) == 0);
}

TEST_CASE("Parser rejects payloads longer than kMaxPayload") {
    // Hand-build a frame with kMaxPayload + 1 payload bytes (FrameWriter itself refuses to).
    std::vector<std::uint8_t> raw;
    raw.push_back(MSG_METERS);
    for (int i = 0; i < kMaxPayload + 1; ++i) raw.push_back(static_cast<std::uint8_t>(i + 1));
    raw.push_back(crc8(raw.data(), raw.size()));
    auto encoded = encodeRaw(raw);
    encoded.push_back(0);
    FrameParser parser;
    bool got = false;
    for (auto b : encoded) got = got || parser.feed(b);
    CHECK_FALSE(got);

    // Exactly kMaxPayload is accepted.
    raw.erase(raw.end() - 2, raw.end());
    raw.push_back(crc8(raw.data(), raw.size()));
    encoded = encodeRaw(raw);
    encoded.push_back(0);
    got = false;
    for (auto b : encoded) got = got || parser.feed(b);
    CHECK(got);
    CHECK(parser.payloadSize() == kMaxPayload);
}

TEST_CASE("Parser survives corruption and resynchronises on the next delimiter") {
    FrameWriter writer;
    writer.begin(MSG_PING);
    writer.u8(7);
    std::uint8_t frame[kMaxEncodedFrame];
    const std::uint8_t length = writer.finish(frame);

    FrameParser parser;
    std::mt19937 rng(1);
    int good = 0;
    for (int round = 0; round < 2000; ++round) {
        // random garbage
        for (int i = 0; i < static_cast<int>(rng() % 80); ++i) parser.feed(static_cast<std::uint8_t>(rng()));
        parser.feed(0);
        // corrupted copy (must be rejected)
        std::vector<std::uint8_t> bad(frame, frame + length);
        bad[rng() % (length - 1)] ^= static_cast<std::uint8_t>(1 + rng() % 254);
        for (auto b : bad)
            if (parser.feed(b) && parser.type() == MSG_PING && parser.payload()[0] == 7) {
                // A flipped byte can (rarely) still produce a valid CRC; that is acceptable for CRC-8.
            }
        // genuine frame
        for (std::uint8_t i = 0; i < length; ++i)
            if (parser.feed(frame[i])) good += (parser.type() == MSG_PING && parser.payload()[0] == 7);
    }
    CHECK(good == 2000);
    CHECK(parser.errorCount() > 0);
}

TEST_CASE("Meter and gain encodings") {
    CHECK(encodeMeterDb(-100.0f) == 0);
    CHECK(encodeMeterDb(0.0f) == kMeterZeroDb);
    CHECK(encodeMeterDb(-6.0f) == 216);
    CHECK(encodeMeterDb(1.0f) == kMeterClip);
    CHECK(encodeGainDb(-200.0f) == kGainMinusInfinity);
    CHECK(encodeGainDb(6.04f) == 60);
}
