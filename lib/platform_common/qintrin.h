#pragma once

// Header file for the inclusion of platform specific x86/x64 intrinsics header files.

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#else
// Non-x86 builds emulate the required AVX2 and SSE intrinsics through SIMDe.
// Scalar intrinsics missing from SIMDe use compiler builtins below.
#ifndef __cdecl
#define __cdecl
#endif
#ifndef __stdcall
#define __stdcall
#endif

#define SIMDE_ENABLE_NATIVE_ALIASES
#include "simde/x86/sse2.h"
#include "simde/x86/sse4.2.h"
#include "simde/x86/avx.h"
#include "simde/x86/avx2.h"

#include <cstdint>
#include <cstdlib>
#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

// Map the x86 cycle counter to the native ARM timer.
static inline unsigned long long __rdtsc(void)
{
#if defined(__APPLE__)
    return (unsigned long long)mach_absolute_time();
#elif defined(__aarch64__)
    unsigned long long v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    return 0ULL;
#endif
}

// Provide the signed MSVC multiply intrinsic used by math_lib.h.
static inline long long _mul128(long long a, long long b, long long* high)
{
    const __int128 product = (__int128)a * (__int128)b;
    *high = (long long)(product >> 64);
    return (long long)product;
}

// FourQ uses these scalar carry helpers.
static inline unsigned char _addcarry_u64(
    unsigned char carryIn,
    unsigned long long a,
    unsigned long long b,
    unsigned long long* output)
{
    const __uint128_t sum = (__uint128_t)a + (__uint128_t)b + (__uint128_t)carryIn;
    *output = (unsigned long long)sum;
    return (unsigned char)(sum >> 64);
}

static inline unsigned char _subborrow_u64(
    unsigned char borrowIn,
    unsigned long long a,
    unsigned long long b,
    unsigned long long* output)
{
    const __uint128_t difference = (__uint128_t)a - (__uint128_t)b - (__uint128_t)borrowIn;
    *output = (unsigned long long)difference;
    return (unsigned char)((difference >> 64) & 1);
}

static inline unsigned long long _lzcnt_u64(unsigned long long value)
{
    return value ? (unsigned long long)__builtin_clzll(value) : 64ULL;
}

static inline unsigned long long __lzcnt64(unsigned long long value)
{
    return value ? (unsigned long long)__builtin_clzll(value) : 64ULL;
}

static inline unsigned int __lzcnt(unsigned int value)
{
    return value ? (unsigned int)__builtin_clz(value) : 32u;
}

// SIMDe requires constant shifts, while score_common.h uses runtime values.
#undef _mm256_srli_epi64
#undef _mm256_slli_epi64
static inline __m256i _mm256_srli_epi64(__m256i value, int shift)
{
    unsigned long long lanes[4];
    __builtin_memcpy(lanes, &value, 32);

    for (int i = 0; i < 4; i++)
    {
        lanes[i] = shift <= 0 ? lanes[i] : (shift >= 64 ? 0ULL : (lanes[i] >> shift));
    }

    __m256i result;
    __builtin_memcpy(&result, lanes, 32);
    return result;
}

static inline __m256i _mm256_slli_epi64(__m256i value, int shift)
{
    unsigned long long lanes[4];
    __builtin_memcpy(lanes, &value, 32);

    for (int i = 0; i < 4; i++)
    {
        lanes[i] = shift <= 0 ? lanes[i] : (shift >= 64 ? 0ULL : (lanes[i] << shift));
    }

    __m256i result;
    __builtin_memcpy(&result, lanes, 32);
    return result;
}

static inline unsigned long long _tzcnt_u64(unsigned long long value)
{
    return value ? (unsigned long long)__builtin_ctzll(value) : 64ULL;
}

static inline unsigned int _blsr_u32(unsigned int value)
{
    return value & (value - 1u);
}

static inline unsigned long long _blsr_u64(unsigned long long value)
{
    return value & (value - 1ULL);
}

static inline unsigned int __popcnt(unsigned int value)
{
    return (unsigned int)__builtin_popcount(value);
}

static inline unsigned long long __popcnt64(unsigned long long value)
{
    return (unsigned long long)__builtin_popcountll(value);
}

static inline unsigned char _BitScanForward(unsigned long* index, unsigned int mask)
{
    if (!mask)
    {
        return 0;
    }
    *index = (unsigned long)__builtin_ctz(mask);
    return 1;
}

static inline unsigned long long _andn_u64(unsigned long long a, unsigned long long b)
{
    return (~a) & b;
}

static inline unsigned int _andn_u32(unsigned int a, unsigned int b)
{
    return (~a) & b;
}

// ARM has no x86 CPUID features.
static inline void __cpuid(int info[4], int leaf)
{
    (void)leaf;
    info[0] = 0;
    info[1] = 0;
    info[2] = 0;
    info[3] = 0;
}

// Testnet ARM builds use a non-cryptographic fallback for RDRAND.
static inline unsigned long long __qinit_xorshift64(void)
{
    static unsigned long long state = 0x9e3779b97f4a7c15ULL;
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state ^ __rdtsc();
}

static inline int _rdrand64_step(unsigned long long* output)
{
    *output = __qinit_xorshift64();
    return 1;
}

static inline int _rdrand32_step(unsigned int* output)
{
    *output = (unsigned int)__qinit_xorshift64();
    return 1;
}

// Expose the emulated AVX2 path after SIMDe selects its non-native implementation.
#ifndef __AVX2__
#define __AVX2__ 1
#endif
#endif

// four_q.h uses this MSVC scalar intrinsic; same body as platform/concurrency.h so either order is fine.
#if defined(__linux__) || defined(__APPLE__)
#ifndef _byteswap_ulong
#define _byteswap_ulong(x) __builtin_bswap32(x)
#endif

// assets.h getUniverseDigest uses this MSVC scalar intrinsic.
static inline unsigned char _BitScanForward64(unsigned long* index, unsigned long long mask)
{
    if (!mask)
        return 0;
    *index = (unsigned long)__builtin_ctzll(mask);
    return 1;
}
#endif
