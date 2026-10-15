#include "reka/common/Types.h"

namespace reka {

const char* toString(ChannelMode mode) noexcept {
    switch (mode) {
        case ChannelMode::Stereo: return "stereo";
        case ChannelMode::Mono: return "mono";
        case ChannelMode::SwapLR: return "swap";
        case ChannelMode::LeftOnly: return "left";
        case ChannelMode::RightOnly: return "right";
    }
    return "stereo";
}

bool fromString(const std::string& text, ChannelMode& mode) noexcept {
    if (text == "stereo") mode = ChannelMode::Stereo;
    else if (text == "mono") mode = ChannelMode::Mono;
    else if (text == "swap") mode = ChannelMode::SwapLR;
    else if (text == "left") mode = ChannelMode::LeftOnly;
    else if (text == "right") mode = ChannelMode::RightOnly;
    else return false;
    return true;
}

}  // namespace reka
