#include "reka/hardware/SerialPort.h"

namespace reka {

bool isKnownMicrocontrollerVendor(std::uint16_t vendorId) noexcept {
    switch (vendorId) {
        case 0x2341:  // Arduino SA
        case 0x2A03:  // Arduino.org
        case 0x1B4F:  // SparkFun (Pro Micro)
        case 0x16C0:  // PJRC / Van Ooijen (Teensy)
        case 0x303A:  // Espressif (native USB)
        case 0x10C4:  // Silicon Labs CP210x (ESP32 dev boards)
        case 0x1A86:  // WCH CH340/CH9102 (clones)
        case 0x0403:  // FTDI
        case 0x239A:  // Adafruit
        case 0x2E8A:  // Raspberry Pi (RP2040)
            return true;
        default:
            return false;
    }
}

}  // namespace reka
