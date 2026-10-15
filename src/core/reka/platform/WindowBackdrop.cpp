#include "reka/platform/WindowBackdrop.h"

#if defined(_WIN32)
#include <windows.h>
#include <dwmapi.h>
#endif

namespace reka {

WindowBackdrop windowBackdropFromString(const std::string& text) noexcept {
    if (text == "mica") return WindowBackdrop::Mica;
    if (text == "acrylic") return WindowBackdrop::Acrylic;
    if (text == "micaAlt") return WindowBackdrop::MicaAlt;
    return WindowBackdrop::None;
}

#if defined(_WIN32)

bool applyWindowBackdrop(void* nativeWindowHandle, WindowBackdrop backdrop, bool darkMode) noexcept {
    HWND hwnd = static_cast<HWND>(nativeWindowHandle);
    if (hwnd == nullptr) return false;

    // Values from dwmapi.h (Windows 11 SDK); spelled out so older SDKs and MinGW headers still build.
    constexpr DWORD kUseImmersiveDarkMode = 20;  // DWMWA_USE_IMMERSIVE_DARK_MODE
    constexpr DWORD kSystemBackdropType = 38;    // DWMWA_SYSTEMBACKDROP_TYPE (22H2+)
    const BOOL dark = darkMode ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, kUseImmersiveDarkMode, &dark, sizeof(dark));

    int type = 1;  // DWMSBT_NONE
    switch (backdrop) {
        case WindowBackdrop::Mica: type = 2; break;     // DWMSBT_MAINWINDOW
        case WindowBackdrop::Acrylic: type = 3; break;  // DWMSBT_TRANSIENTWINDOW
        case WindowBackdrop::MicaAlt: type = 4; break;  // DWMSBT_TABBEDWINDOW
        case WindowBackdrop::None: break;
    }
    if (FAILED(DwmSetWindowAttribute(hwnd, kSystemBackdropType, &type, sizeof(type)))) return false;
    // Let the backdrop show through the client area (the UI paints translucent panels on top), or undo
    // that when switching back to None so the painted fallback is not composited over the DWM frame.
    const int extend = backdrop != WindowBackdrop::None ? -1 : 0;
    const MARGINS margins{extend, extend, extend, extend};
    DwmExtendFrameIntoClientArea(hwnd, &margins);
    return true;
}

#else

bool applyWindowBackdrop(void*, WindowBackdrop, bool) noexcept { return false; }

#endif

}  // namespace reka
