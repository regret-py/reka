#pragma once

// Minimal COM helpers for the Windows virtual-audio layer: a ComPtr without WRL/ATL, a tolerant
// CoInitializeEx scope and readable HRESULT text. Internal to reka_core (Windows only).

#if defined(_WIN32)

#include "reka/platform/Win32Util.h"

#include <windows.h>
#include <objbase.h>
#include <audioclient.h>

#include <cstddef>
#include <cstring>  // PropVariantInit may expand to memset
#include <string>
#include <utility>

namespace reka::win {

template <typename T>
class ComPtr {
public:
    ComPtr() noexcept = default;
    ComPtr(std::nullptr_t) noexcept {}
    ComPtr(const ComPtr& other) noexcept : ptr_(other.ptr_) {
        if (ptr_ != nullptr) ptr_->AddRef();
    }
    ComPtr(ComPtr&& other) noexcept : ptr_(std::exchange(other.ptr_, nullptr)) {}
    ~ComPtr() { reset(); }

    ComPtr& operator=(ComPtr other) noexcept {
        std::swap(ptr_, other.ptr_);
        return *this;
    }

    // Takes ownership of an already AddRef'ed pointer.
    static ComPtr adopt(T* raw) noexcept {
        ComPtr result;
        result.ptr_ = raw;
        return result;
    }

    T* get() const noexcept { return ptr_; }
    T* operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

    // Out-parameter access: releases the current object first.
    T** put() noexcept {
        reset();
        return &ptr_;
    }
    void** putVoid() noexcept { return reinterpret_cast<void**>(put()); }

    void reset() noexcept {
        if (ptr_ != nullptr) {
            T* old = ptr_;
            ptr_ = nullptr;
            old->Release();
        }
    }

    T* detach() noexcept { return std::exchange(ptr_, nullptr); }

    template <typename U>
    HRESULT as(ComPtr<U>& out) const noexcept {
        if (ptr_ == nullptr) return E_POINTER;
        return ptr_->QueryInterface(__uuidof(U), out.putVoid());
    }

private:
    T* ptr_ = nullptr;
};

// CoInitializeEx for the current thread's lifetime of this object. A thread that already lives in another
// apartment (RPC_E_CHANGED_MODE, e.g. a JUCE message thread in an STA) is fine to use as is.
class ScopedCoInit {
public:
    explicit ScopedCoInit(DWORD model = COINIT_MULTITHREADED) noexcept : hr_(CoInitializeEx(nullptr, model)) {}
    ~ScopedCoInit() {
        if (SUCCEEDED(hr_)) CoUninitialize();
    }
    ScopedCoInit(const ScopedCoInit&) = delete;
    ScopedCoInit& operator=(const ScopedCoInit&) = delete;

    bool ok() const noexcept { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }
    HRESULT result() const noexcept { return hr_; }

private:
    HRESULT hr_;
};

// Frees a CoTaskMemAlloc'ed buffer (endpoint ids, display names, mix formats).
template <typename T>
class CoTaskMemPtr {
public:
    CoTaskMemPtr() noexcept = default;
    ~CoTaskMemPtr() {
        if (ptr_ != nullptr) CoTaskMemFree(ptr_);
    }
    CoTaskMemPtr(const CoTaskMemPtr&) = delete;
    CoTaskMemPtr& operator=(const CoTaskMemPtr&) = delete;

    T* get() const noexcept { return ptr_; }
    T** put() noexcept {
        if (ptr_ != nullptr) CoTaskMemFree(ptr_);
        ptr_ = nullptr;
        return &ptr_;
    }

private:
    T* ptr_ = nullptr;
};

// PROPVARIANT that is cleared on scope exit.
class ScopedPropVariant {
public:
    ScopedPropVariant() noexcept { PropVariantInit(&value_); }
    ~ScopedPropVariant() { PropVariantClear(&value_); }
    ScopedPropVariant(const ScopedPropVariant&) = delete;
    ScopedPropVariant& operator=(const ScopedPropVariant&) = delete;

    PROPVARIANT* get() noexcept { return &value_; }
    const PROPVARIANT& value() const noexcept { return value_; }

private:
    PROPVARIANT value_;
};

// "the audio device was removed or disabled (0x88890004)"
inline std::string hresultMessage(HRESULT hr) {
    const char* text = nullptr;
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED) text = "the audio device was removed or disabled";
    else if (hr == AUDCLNT_E_DEVICE_IN_USE) text = "the audio device is used in exclusive mode by another application";
    else if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT) text = "the audio format is not supported";
    else if (hr == AUDCLNT_E_SERVICE_NOT_RUNNING) text = "the Windows Audio service is not running";
    else if (hr == AUDCLNT_E_ENDPOINT_CREATE_FAILED) text = "Windows could not create the audio endpoint";
    else if (hr == AUDCLNT_E_CPUUSAGE_EXCEEDED) text = "the audio engine exceeded its CPU budget";
    else if (hr == AUDCLNT_E_RESOURCES_INVALIDATED) text = "the audio resources were invalidated";
    else if (hr == AUDCLNT_E_ALREADY_INITIALIZED) text = "the audio client is already initialised";
    else if (hr == AUDCLNT_E_WRONG_ENDPOINT_TYPE) text = "wrong audio endpoint type";
    else if (hr == AUDCLNT_E_BUFFER_SIZE_ERROR) text = "invalid audio buffer size";
    else if (hr == AUDCLNT_E_INVALID_STREAM_FLAG) text = "invalid audio stream flags";
    else if (hr == E_NOTIMPL) text = "not supported by this version of Windows";
    else if (hr == E_ACCESSDENIED) text = "access denied";
    else if (hr == E_OUTOFMEMORY) text = "out of memory";
    else if (hr == E_INVALIDARG) text = "invalid argument";
    else if (hr == E_NOINTERFACE) text = "interface not supported";
    else if (hr == REGDB_E_CLASSNOTREG) text = "COM class not registered";

    std::string message = text != nullptr ? std::string(text) : systemMessageText(static_cast<DWORD>(hr));
    if (message.empty()) message = "error";
    return message + " (" + hexCode(static_cast<unsigned long>(hr)) + ")";
}

}  // namespace reka::win

#endif  // _WIN32
