#pragma once

#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(JSOC_BUILD_SHARED)
#define JSOC_API __declspec(dllexport)
#elif defined(JSOC_USE_SHARED)
#define JSOC_API __declspec(dllimport)
#else
#define JSOC_API
#endif
#define JSOC_INTERNAL
#else
#if defined(JSOC_BUILD_SHARED) || defined(JSOC_USE_SHARED)
#define JSOC_API __attribute__((visibility("default")))
#define JSOC_INTERNAL __attribute__((visibility("hidden")))
#else
#define JSOC_API
#define JSOC_INTERNAL
#endif
#endif

#if defined(__GNUC__) || defined(__clang__)
#define JSOC_NODISCARD __attribute__((warn_unused_result))
#define JSOC_PURE __attribute__((pure))
#define JSOC_CONST __attribute__((const))
#define JSOC_COLD __attribute__((cold))
#define JSOC_NOINLINE __attribute__((noinline))
#define JSOC_ALWAYS_INLINE __attribute__((always_inline))
#define JSOC_ASSUME_ALIGNED(x, a) __builtin_assume_aligned((x), (a))
#define JSOC_LIKELY(x) __builtin_expect(!!(x), 1)
#define JSOC_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define JSOC_UNREACHABLE() __builtin_unreachable()
#else
#define JSOC_NODISCARD
#define JSOC_PURE
#define JSOC_CONST
#define JSOC_COLD
#define JSOC_NOINLINE
#define JSOC_ALWAYS_INLINE inline
#define JSOC_ASSUME_ALIGNED(x, a) (x)
#define JSOC_LIKELY(x) (x)
#define JSOC_UNLIKELY(x) (x)
#define JSOC_UNREACHABLE()
#endif

#if __STDC_VERSION__ >= 202311L
#define JSOC_C23 1
#else
#define JSOC_C23 0
#endif

#if defined(__has_builtin)
#if __has_builtin(__builtin_add_overflow)
#define JSOC_HAS_CKDINT 1
#endif
#endif

#ifndef JSOC_HAS_CKDINT
#define JSOC_HAS_CKDINT 0
#endif