#pragma once

// Small Windows-only helpers shared by the Win32 platform layer (serial ports, virtual audio): UTF-8 <->
// UTF-16 conversion and human-readable Win32 error text. Internal to reka_core; never include it from a
// header that is compiled on other platforms.

#if defined(_WIN32)

#include <windows.h>

#include <cstdio>
#include <string>
#include <string_view>

namespace reka::win {

inline std::wstring toWide(std::string_view text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

inline std::string toUtf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length, nullptr, nullptr);
    return out;
}

inline std::string toUtf8(const wchar_t* text) { return text != nullptr ? toUtf8(std::wstring_view(text)) : std::string(); }

// "0x80070005"
inline std::string hexCode(unsigned long code) {
    char text[16] = {};
    std::snprintf(text, sizeof(text), "0x%08lX", code);
    return text;
}

// The system message for a Win32 error code or an HRESULT, without the trailing period / line break.
// Empty when Windows has no text for it.
inline std::string systemMessageText(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::string text;
    if (length > 0 && buffer != nullptr) text = toUtf8(std::wstring_view(buffer, length));
    if (buffer != nullptr) LocalFree(buffer);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '.'))
        text.pop_back();
    return text;
}

// "Access is denied (error 5)"
inline std::string win32ErrorMessage(DWORD code) {
    const std::string text = systemMessageText(code);
    return (text.empty() ? std::string("Windows error") : text) + " (error " + std::to_string(code) + ")";
}

}  // namespace reka::win

#endif  // _WIN32
