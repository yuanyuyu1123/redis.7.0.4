#include "monotonic.h"
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#undef NDEBUG
#include <assert.h>


/* The function pointer for clock retrieval.  */
//时钟检索的函数指针。
monotime (*getMonotonicUs)(void) = NULL;

static char monotonic_info_string[32];


/* Using the processor clock (aka TSC on x86) can provide improved performance
 * throughout Redis wherever the monotonic clock is used.  The processor clock
 * is significantly faster than calling 'clock_getting' (POSIX).  While this is
 * generally safe on modern systems, this link provides additional information
 * about use of the x86 TSC: http://oliveryang.net/2015/09/pitfalls-of-TSC-usage
 *
 * To use the processor clock, either uncomment this line, or build with
 *   CFLAGS="-DUSE_PROCESSOR_CLOCK"
#define USE_PROCESSOR_CLOCK
 */
/**
 * 在使用单调时钟的任何地方，使用处理器时钟（在 x86 上也称为 TSC）可以提高整个 Redis 的性能。
 * 处理器时钟比调用“clock_getting”（POSIX）快得多。虽然这在现代系统上通常是安全的，
 * 但此链接提供了有关使用 x86 TSC 的其他信息：http:oliveryang.net201509pitfalls-of-TSC-usage
 * 要使用处理器时钟，请取消注释此行，或使用 CFLAGS="- DUSE_PROCESSOR_CLOCK" 定义 USE_PROCESSOR_CLOCK
 * */


#if defined(USE_PROCESSOR_CLOCK) && defined(__x86_64__) && defined(__linux__)
#include <regex.h>
#include <x86intrin.h>

static long mono_ticksPerMicrosecond = 0;

static monotime getMonotonicUs_x86() {
    return __rdtsc() / mono_ticksPerMicrosecond;
}

static void monotonicInit_x86linux() {
    const int bufflen = 256;
    char buf[bufflen];
    regex_t cpuGhzRegex, constTscRegex;
    const size_t nmatch = 2;
    regmatch_t pmatch[nmatch];
    int constantTsc = 0;
    int rc;

    /* Determine the number of TSC ticks in a micro-second.  This is
     * a constant value matching the standard speed of the processor.
     * On modern processors, this speed remains constant even though
     * the actual clock speed varies dynamically for each core.  */
    rc = regcomp(&cpuGhzRegex, "^model name\\s+:.*@ ([0-9.]+)GHz", REG_EXTENDED);
    assert(rc == 0);

    /* Also check that the constant_tsc flag is present.  (It should be
     * unless this is a really old CPU.  */
    rc = regcomp(&constTscRegex, "^flags\\s+:.* constant_tsc", REG_EXTENDED);
    assert(rc == 0);

    FILE *cpuinfo = fopen("/proc/cpuinfo", "r");
    if (cpuinfo != NULL) {
        while (fgets(buf, bufflen, cpuinfo) != NULL) {
            if (regexec(&cpuGhzRegex, buf, nmatch, pmatch, 0) == 0) {
                buf[pmatch[1].rm_eo] = '\0';
                double ghz = atof(&buf[pmatch[1].rm_so]);
                mono_ticksPerMicrosecond = (long)(ghz * 1000);
                break;
            }
        }
        while (fgets(buf, bufflen, cpuinfo) != NULL) {
            if (regexec(&constTscRegex, buf, nmatch, pmatch, 0) == 0) {
                constantTsc = 1;
                break;
            }
        }

        fclose(cpuinfo);
    }
    regfree(&cpuGhzRegex);
    regfree(&constTscRegex);

    if (mono_ticksPerMicrosecond == 0) {
        fprintf(stderr, "monotonic: x86 linux, unable to determine clock rate");
        return;
    }
    if (!constantTsc) {
        fprintf(stderr, "monotonic: x86 linux, 'constant_tsc' flag not present");
        return;
    }

    snprintf(monotonic_info_string, sizeof(monotonic_info_string),
            "X86 TSC @ %ld ticks/us", mono_ticksPerMicrosecond);
    getMonotonicUs = getMonotonicUs_x86;
}
#endif


#if defined(USE_PROCESSOR_CLOCK) && defined(__aarch64__)
static long mono_ticksPerMicrosecond = 0;

/* Read the clock value.  */
static inline uint64_t __cntvct() {
    uint64_t virtual_timer_value;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(virtual_timer_value));
    return virtual_timer_value;
}

/* Read the Count-timer Frequency.  */
static inline uint32_t cntfrq_hz() {
    uint64_t virtual_freq_value;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(virtual_freq_value));
    return (uint32_t)virtual_freq_value;    /* top 32 bits are reserved */
}

static monotime getMonotonicUs_aarch64() {
    return __cntvct() / mono_ticksPerMicrosecond;
}

static void monotonicInit_aarch64() {
    mono_ticksPerMicrosecond = (long)cntfrq_hz() / 1000L / 1000L;
    if (mono_ticksPerMicrosecond == 0) {
        fprintf(stderr, "monotonic: aarch64, unable to determine clock rate");
        return;
    }

    snprintf(monotonic_info_string, sizeof(monotonic_info_string),
            "ARM CNTVCT @ %ld ticks/us", mono_ticksPerMicrosecond);
    getMonotonicUs = getMonotonicUs_aarch64;
}
#endif


static monotime getMonotonicUs_posix() {
    /* clock_gettime() is specified in POSIX.1b (1993).  Even so, some systems
     * did not support this until much later.  CLOCK_MONOTONIC is technically
     * optional and may not be supported - but it appears to be universal.
     * If this is not supported, provide a system-specific alternate version.  */
    /**
     * clock_gettime() 在 POSIX.1b (1993) 中指定。即便如此，一些系统直到很久以后才支持这一点。
     * CLOCK_MONOTONIC 在技术上是可选的，可能不受支持 - 但它似乎是通用的。如果不支持，请提供特定于系统的备用版本。
     * */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

static void monotonicInit_posix() {
    /* Ensure that CLOCK_MONOTONIC is supported.  This should be supported
     * on any reasonably current OS.  If the assertion below fails, provide
     * an appropriate alternate implementation.  */
    //确保支持 CLOCK_MONOTONIC。这应该在任何合理的当前操作系统上得到支持。如果以下断言失败，请提供适当的替代实现。
    struct timespec ts;
    int rc = clock_gettime(CLOCK_MONOTONIC, &ts);
    assert(rc == 0);

    snprintf(monotonic_info_string, sizeof(monotonic_info_string),
            "POSIX clock_gettime");
    getMonotonicUs = getMonotonicUs_posix;
}



const char * monotonicInit() {
    #if defined(USE_PROCESSOR_CLOCK) && defined(__x86_64__) && defined(__linux__)
    if (getMonotonicUs == NULL) monotonicInit_x86linux();
    #endif

    #if defined(USE_PROCESSOR_CLOCK) && defined(__aarch64__)
    if (getMonotonicUs == NULL) monotonicInit_aarch64();
    #endif

    if (getMonotonicUs == NULL) monotonicInit_posix();

    return monotonic_info_string;
}

const char *monotonicInfoString() {
    return monotonic_info_string;
}

monotonic_clock_type monotonicGetType() {
    if (getMonotonicUs == getMonotonicUs_posix)
        return MONOTONIC_CLOCK_POSIX;
    return MONOTONIC_CLOCK_HW;
}
