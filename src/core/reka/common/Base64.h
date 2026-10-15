#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace reka {

std::string base64Encode(const std::uint8_t* data, std::size_t size);
inline std::string base64Encode(const std::vector<std::uint8_t>& data) {
    return base64Encode(data.data(), data.size());
}

// Returns std::nullopt on malformed input. Whitespace is ignored.
std::optional<std::vector<std::uint8_t>> base64Decode(std::string_view text);

}  // namespace reka
