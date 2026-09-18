/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * SDL3 build configuration for the ESP-IDF host (POSIX/Linux) target.
 *
 * SDL3 normally generates this header from include/build_config/SDL_build_config.h.cmake
 * with the results of the platform checks of its own CMake project. This port
 * only needs the SDL video and event path, and it must build inside an ESP-IDF
 * application, so the configuration is fixed here:
 *
 *   - the C library is assumed to be the glibc of the host,
 *   - the video subsystem keeps the offscreen and dummy drivers (the KMSDRM
 *     driver is enabled by CMake when libdrm/EGL are found) and only the
 *     software renderer,
 *   - every subsystem the LCD panel does not use is disabled: audio, camera,
 *     joystick, haptic, hidapi, sensor, power, dialog, tray and GPU,
 *   - SDL threads are used, and the timer and RTC implementations are the
 *     POSIX ones,
 *   - the dynamic API is off, because SDL is linked statically into the
 *     application.
 *
 * Keep this file in sync with the SDL version of the submodule. It is
 * intentionally limited to the macros this port enables.
 */
#ifndef SDL_build_config_h_
#define SDL_build_config_h_

/* General platform specific identifiers */
#include <SDL3/SDL_platform_defines.h>

/*
 * SDL has no option for the dynamic API, it enables it unless an internal file
 * of the library is edited. It is not useful for this port, because SDL is
 * linked statically into the application, so the API is switched off here.
 *
 * SDL includes this file before src/dynapi/SDL_dynapi.h in every translation
 * unit, and the guard of SDL_dynapi.h makes sure the value cannot be changed
 * afterwards: src/dynapi/SDL_dynapi.h refuses a definition on the command line
 * with an #error, which is why the value is not a compile definition.
 */
#define SDL_DYNAMIC_API 0
#define SDL_dynapi_h_ 1

/* Compiler support */
#define HAVE_GCC_ATOMICS 1

/* Useful headers */
#define HAVE_FLOAT_H 1
#define HAVE_STDARG_H 1
#define HAVE_STDDEF_H 1
#define HAVE_STDINT_H 1

/* Comment this if you want to build without any C library requirements */
#define HAVE_LIBC 1
#ifdef HAVE_LIBC

/* Useful headers */
#define HAVE_ALLOCA_H 1
#define HAVE_ICONV_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_LIMITS_H 1
#define HAVE_MATH_H 1
#define HAVE_MEMORY_H 1
#define HAVE_SIGNAL_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_WCHAR_H 1

/* C library functions */
#define HAVE_DLOPEN 1
#define HAVE_MALLOC 1
#define HAVE_FDATASYNC 1
#define HAVE_GETENV 1
#define HAVE_GETHOSTNAME 1
#define HAVE_SETENV 1
#define HAVE_PUTENV 1
#define HAVE_UNSETENV 1
#define HAVE_ABS 1
#define HAVE_BCOPY 1
#define HAVE_MEMSET 1
#define HAVE_MEMCPY 1
#define HAVE_MEMMOVE 1
#define HAVE_MEMCMP 1
#define HAVE_WCSLEN 1
#define HAVE_WCSNLEN 1
#define HAVE_WCSLCPY 1
#define HAVE_WCSLCAT 1
#define HAVE_WCSSTR 1
#define HAVE_WCSCMP 1
#define HAVE_WCSNCMP 1
#define HAVE_WCSTOL 1
#define HAVE_STRLEN 1
#define HAVE_STRNLEN 1
#define HAVE_STRLCPY 1
#define HAVE_STRLCAT 1
#define HAVE_STRPBRK 1
#define HAVE_INDEX 1
#define HAVE_RINDEX 1
#define HAVE_STRCHR 1
#define HAVE_STRRCHR 1
#define HAVE_STRSTR 1
#define HAVE_STRTOK_R 1
#define HAVE_STRTOL 1
#define HAVE_STRTOUL 1
#define HAVE_STRTOLL 1
#define HAVE_STRTOULL 1
#define HAVE_STRTOD 1
#define HAVE_ATOI 1
#define HAVE_ATOF 1
#define HAVE_STRCMP 1
#define HAVE_STRNCMP 1
#define HAVE_VSSCANF 1
#define HAVE_VSNPRINTF 1
#define HAVE_ACOS 1
#define HAVE_ACOSF 1
#define HAVE_ASIN 1
#define HAVE_ASINF 1
#define HAVE_ATAN 1
#define HAVE_ATANF 1
#define HAVE_ATAN2 1
#define HAVE_ATAN2F 1
#define HAVE_CEIL 1
#define HAVE_CEILF 1
#define HAVE_COPYSIGN 1
#define HAVE_COPYSIGNF 1
#define HAVE_COS 1
#define HAVE_COSF 1
#define HAVE_EXP 1
#define HAVE_EXPF 1
#define HAVE_FABS 1
#define HAVE_FABSF 1
#define HAVE_FLOOR 1
#define HAVE_FLOORF 1
#define HAVE_FMOD 1
#define HAVE_FMODF 1
#define HAVE_ISINF 1
#define HAVE_ISINFF 1
#define HAVE_ISINF_FLOAT_MACRO 1
#define HAVE_ISNAN 1
#define HAVE_ISNANF 1
#define HAVE_ISNAN_FLOAT_MACRO 1
#define HAVE_LOG 1
#define HAVE_LOGF 1
#define HAVE_LOG10 1
#define HAVE_LOG10F 1
#define HAVE_LROUND 1
#define HAVE_LROUNDF 1
#define HAVE_MODF 1
#define HAVE_MODFF 1
#define HAVE_POW 1
#define HAVE_POWF 1
#define HAVE_ROUND 1
#define HAVE_ROUNDF 1
#define HAVE_SCALBN 1
#define HAVE_SCALBNF 1
#define HAVE_SIN 1
#define HAVE_SINF 1
#define HAVE_SQRT 1
#define HAVE_SQRTF 1
#define HAVE_TAN 1
#define HAVE_TANF 1
#define HAVE_TRUNC 1
#define HAVE_TRUNCF 1
#define HAVE_FOPEN64 1
#define HAVE_FSEEKO 1
#define HAVE_FSEEKO64 1
#define HAVE_MEMFD_CREATE 1
#define HAVE_POSIX_FALLOCATE 1
#define HAVE_SIGACTION 1
#define HAVE_SIGTIMEDWAIT 1
#define HAVE_SA_SIGACTION 1
#define HAVE_ST_MTIM 1
#define HAVE_SETJMP 1
#define HAVE_NANOSLEEP 1
#define HAVE_GMTIME_R 1
#define HAVE_LOCALTIME_R 1
#define HAVE_NL_LANGINFO 1
#define HAVE_SYSCONF 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_GETPAGESIZE 1
#define HAVE_ICONV 1
#define HAVE_SEM_TIMEDWAIT 1
#define HAVE_GETAUXVAL 1
#define HAVE_PPOLL 1
#define HAVE__EXIT 1
#define HAVE_GETRESUID 1
#define HAVE_GETRESGID 1

#endif /* HAVE_LIBC */

#define HAVE_INOTIFY_INIT1 1
#define HAVE_INOTIFY 1
#define HAVE_O_CLOEXEC 1

/* SDL internal assertion support */
#define SDL_DEFAULT_ASSERT_LEVEL_CONFIGURED 1
#define SDL_DEFAULT_ASSERT_LEVEL 2

/* Allow disabling of major subsystems */
#define SDL_AUDIO_DISABLED 1
#define SDL_GPU_DISABLED 1
#define SDL_CAMERA_DISABLED 1
#define SDL_JOYSTICK_DISABLED 1
#define SDL_HAPTIC_DISABLED 1
#define SDL_HIDAPI_DISABLED 1
#define SDL_POWER_DISABLED 1
#define SDL_SENSOR_DISABLED 1
#define SDL_DIALOG_DISABLED 1

/* Enable various threading systems */
#define SDL_THREAD_GENERIC_COND_SUFFIX 1
#define SDL_THREAD_GENERIC_RWLOCK_SUFFIX 1
#define SDL_THREAD_PTHREAD 1
#define SDL_THREAD_PTHREAD_RECURSIVE_MUTEX 1

/* Enable various shared object loading systems */
#define SDL_LOADSO_DLOPEN 1

/* Enable various RTC systems */
#define SDL_TIME_UNIX 1

/* Enable various timer systems */
#define SDL_TIMER_UNIX 1

/* Enable various process implementations */
#define SDL_PROCESS_POSIX 1

/* Enable various video drivers */
#define SDL_VIDEO_DRIVER_DUMMY 1
#define SDL_VIDEO_DRIVER_OFFSCREEN 1

/* Enable various render drivers (software only) */
#define SDL_VIDEO_RENDER_SW 1

/* Enable system filesystem support */
#define SDL_FILESYSTEM_UNIX 1

/* Enable system FSops support */
#define SDL_FSOPS_POSIX 1

/* Enable tray subsystem */
#define SDL_TRAY_DUMMY 1

/* Configure use of intrinsics: the ESP-IDF host build does not use any */
#define SDL_DISABLE_SSE 1
#define SDL_DISABLE_SSE2 1
#define SDL_DISABLE_SSE3 1
#define SDL_DISABLE_SSE4_1 1
#define SDL_DISABLE_SSE4_2 1
#define SDL_DISABLE_AVX 1
#define SDL_DISABLE_AVX2 1
#define SDL_DISABLE_AVX512F 1
#define SDL_DISABLE_MMX 1
#define SDL_DISABLE_LSX 1
#define SDL_DISABLE_LASX 1
#define SDL_DISABLE_NEON 1

#endif /* SDL_build_config_h_ */
