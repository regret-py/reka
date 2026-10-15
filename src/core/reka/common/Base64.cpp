#include "reka/common/Base64.h"

#include <array>

namespace reka {

namespace {
constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

constexpr std::array<std::int8_t, 256> makeDecodeTable() {
    std::array<std::int8_t, 256> table{};
    for (auto& entry : table) entry = -1;
    for (int i = 0; i < 64; ++i) table[static_cast<unsigned char>(kAlphabet[i])] = static_cast<std::int8_t>(i);
    return table;
}
constexpr auto kDecodeTable = makeDecodeTable();
}  // namespace

std::string base64Encode(const std::uint8_t* data, std::size_t size) {
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 2 < size; i += 3) {
        const std::uint32_t n = (std::uint32_t(data[i]) << 16) | (std::uint32_t(data[i + 1]) << 8) | data[i + 2];
        out.push_back(kAlphabet[(n >> 18) & 63]);
        out.push_back(kAlphabet[(n >> 12) & 63]);
        out.push_back(kAlphabet[(n >> 6) & 63]);
        out.push_back(kAlphabet[n & 63]);
    }
    if (i < size) {
        std::uint32_t n = std::uint32_t(data[i]) << 16;
        if (i + 1 < size) n |= std::uint32_t(data[i + 1]) << 8;
        out.push_back(kAlphabet[(n >> 18) & 63]);
        out.push_back(kAlphabet[(n >> 12) & 63]);
        out.push_back(i + 1 < size ? kAlphabet[(n >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> base64Decode(std::string_view text) {
    std::vector<std::uint8_t> out;
    out.reserve(text.size() * 3 / 4);
    std::uint32_t buffer = 0;
    int bits = 0;
    int padding = 0;
    for (const char c : text) {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        if (c == '=') {
            ++padding;
            continue;
        }
        if (padding > 0) return std::nullopt;  // data after padding
        const std::int8_t value = kDecodeTable[static_cast<unsigned char>(c)];
        if (value < 0) return std::nullopt;
        buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xFF));
        }
    }
    if (padding > 2) return std::nullopt;
    return out;
}

}  // namespace reka
