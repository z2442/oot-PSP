#ifndef ASSERT_H
#define ASSERT_H

#if !defined(__GNUC__) && !defined(__attribute__)
#define __attribute__(x)
#endif

// Runtime assertions
#if defined(TARGET_PSP) || defined(PLATFORM_PSP)
__attribute__((noreturn)) void oot_psp_assert(const char* assertion, const char* file, int line);
#else
__attribute__((noreturn)) void __assert(const char* assertion, const char* file, int line);
#endif

// assert for matching
#ifndef NDEBUG
# ifndef NON_MATCHING
#  if defined(TARGET_PSP) || defined(PLATFORM_PSP)
#   define ASSERT(cond, msg, file, line) ((cond) ? ((void)0) : oot_psp_assert(msg, file, line))
#  else
#   define ASSERT(cond, msg, file, line) ((cond) ? ((void)0) : __assert(msg, file, line))
#  endif
# else
#  if defined(TARGET_PSP) || defined(PLATFORM_PSP)
#   define ASSERT(cond, msg, file, line) ((cond) ? ((void)0) : oot_psp_assert(#cond, __FILE__, __LINE__))
#  else
#   define ASSERT(cond, msg, file, line) ((cond) ? ((void)0) : __assert(#cond, __FILE__, __LINE__))
#  endif
# endif
#else
# define ASSERT(cond, msg, file, line) ((void)0)
#endif

// standard assert macro
#ifndef NDEBUG
# define assert(cond) ASSERT(cond, #cond, __FILE__, __LINE__)
#else
# define assert(cond) ((void)0)
#endif

// Static/compile-time assertions

#if !defined(__sgi) && (__GNUC__ >= 5 || __STDC_VERSION__ >= 201112L)
# define static_assert(cond, msg) _Static_assert(cond, msg)
#else
# ifndef GLUE
#  define GLUE(a, b) a##b
# endif
# ifndef GLUE2
#  define GLUE2(a, b) GLUE(a, b)
# endif

# define static_assert(cond, msg) typedef char GLUE2(static_assertion_failed, __LINE__)[(cond) ? 1 : -1]
#endif

#endif
