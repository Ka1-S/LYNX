#pragma once

// Enables flush-to-zero/denormals-are-zero on the thread while the object exists. Reverb tails,
// filters and delays decay into denormal numbers, which on x86 cost tens of times more
// per operation; zeroing them changes nothing audible.
#if defined(__SSE__) || defined(_M_X64) || defined(__x86_64__) || defined(__aarch64__) || defined(_M_ARM64) // x86 SSE, or ARM64 through core/Simd.h (sse2neon)
 #include "core/Simd.h"
#endif

namespace lynx
{
class ScopedFlushDenormals
{
public:
    ScopedFlushDenormals() noexcept
    {
#if defined(__SSE__) || defined(_M_X64) || defined(__x86_64__) || defined(__aarch64__) || defined(_M_ARM64) // x86 SSE, or ARM64 through core/Simd.h (sse2neon)
        saved = _mm_getcsr();
        _mm_setcsr (saved | 0x8040u); // FTZ (bit 15) + DAZ (bit 6)
#endif
    }

    ~ScopedFlushDenormals()
    {
#if defined(__SSE__) || defined(_M_X64) || defined(__x86_64__) || defined(__aarch64__) || defined(_M_ARM64) // x86 SSE, or ARM64 through core/Simd.h (sse2neon)
        _mm_setcsr (saved);
#endif
    }

    ScopedFlushDenormals (const ScopedFlushDenormals&) = delete;
    ScopedFlushDenormals& operator= (const ScopedFlushDenormals&) = delete;

private:
    unsigned int saved = 0;
};

// Keeps this thread's floating-point mode (MXCSR, and the x87 control word on x86-64 GCC/Clang) across foreign code —
// a hosted plugin's process call (round 165i). Some plugins change it (Delphi-built ones load their own default;
// others turn flush-to-zero off), which then slowed down or changed LYNX's own DSP for the rest of the block. FL Studio
// does the same: it re-asserts its x87 control word and MXCSR (FTZ, plus DAZ when the CPU has it) at 71 places of
// FLEngine, e.g. before its render lock (REA_rodada165i). Restores only when the mode changed (ldmxcsr is not free).
class KeepFloatMode
{
public:
    KeepFloatMode() noexcept
    {
#if defined(__SSE__) || defined(_M_X64) || defined(__x86_64__) || defined(__aarch64__) || defined(_M_ARM64)
        csr = _mm_getcsr();
#endif
#if defined(__x86_64__) && defined(__GNUC__)
        __asm__ volatile ("fnstcw %0" : "=m" (cw));
#endif
    }

    ~KeepFloatMode()
    {
#if defined(__SSE__) || defined(_M_X64) || defined(__x86_64__) || defined(__aarch64__) || defined(_M_ARM64)
        if (_mm_getcsr() != csr)
            _mm_setcsr (csr);
#endif
#if defined(__x86_64__) && defined(__GNUC__)
        unsigned short now = 0;
        __asm__ volatile ("fnstcw %0" : "=m" (now));
        if (now != cw)
            __asm__ volatile ("fnclex\n\tfldcw %0" : : "m" (cw));
#endif
    }

    KeepFloatMode (const KeepFloatMode&) = delete;
    KeepFloatMode& operator= (const KeepFloatMode&) = delete;

private:
    unsigned int csr = 0;
    unsigned short cw = 0;
};
} // namespace lynx
