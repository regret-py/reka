#pragma once

#include <cstdint>

#if defined(__SSE__) || defined(_M_X64) || defined(_M_AMD64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#include <xmmintrin.h>
#define REKA_HAS_SSE_CSR 1
#else
#define REKA_HAS_SSE_CSR 0
#endif

namespace reka {

// Enables flush-to-zero / denormals-are-zero for the lifetime of the object. Every real-time callback
// entry point creates one: IIR filters decaying into denormal range otherwise cost 10-100x CPU.
class ScopedFlushDenormals {
public:
    ScopedFlushDenormals() noexcept {
#if REKA_HAS_SSE_CSR
        saved_ = _mm_getcsr();
        _mm_setcsr(saved_ | 0x8040u);  // FTZ (bit 15) | DAZ (bit 6)
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
        std::uint64_t fpcr;
        asm volatile("mrs %0, fpcr" : "=r"(fpcr));
        saved_ = fpcr;
        asm volatile("msr fpcr, %0" : : "r"(fpcr | (1ull << 24)));  // FZ
#endif
    }

    ~ScopedFlushDenormals() noexcept {
#if REKA_HAS_SSE_CSR
        _mm_setcsr(static_cast<unsigned int>(saved_));
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
        asm volatile("msr fpcr, %0" : : "r"(saved_));
#endif
    }

    ScopedFlushDenormals(const ScopedFlushDenormals&) = delete;
    ScopedFlushDenormals& operator=(const ScopedFlushDenormals&) = delete;

private:
    std::uint64_t saved_ = 0;
};

}  // namespace reka
