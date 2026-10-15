#pragma once

#include <string>

namespace reka {

enum class WindowBackdrop { None, Mica, Acrylic, MicaAlt };

WindowBackdrop windowBackdropFromString(const std::string& text) noexcept;

// Windows 11 22H2+: applies a DWM system backdrop (Mica / Acrylic) and immersive dark mode to a native
// top-level window (HWND passed as void*). Returns false on older Windows or other platforms; callers
// then fall back to the painted glass effect of the theme.
bool applyWindowBackdrop(void* nativeWindowHandle, WindowBackdrop backdrop, bool darkMode) noexcept;

}  // namespace reka
