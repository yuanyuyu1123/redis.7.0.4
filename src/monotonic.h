#ifndef __MONOTONIC_H
#define __MONOTONIC_H
/* The monotonic clock is an always increasing clock source.  It is unrelated to
 * the actual time of day and should only be used for relative timings.  The
 * monotonic clock is also not guaranteed to be chronologically precise; there
 * may be slight skew/shift from a precise clock.
 *
 * Depending on system architecture, the monotonic time may be able to be
 * retrieved much faster than a normal clock source by using an instruction
 * counter on the CPU.  On x86 architectures (for example), the RDTSC
 * instruction is a very fast clock source for this purpose.
 */
/**
 * 单调时钟是一个始终递增的时钟源。它与一天中的实际时间无关，只能用于相对时间。
 * 单调时钟也不能保证在时间上是精确的；精确的时钟可能会有轻微的偏差。
 * 根据系统架构，通过使用 CPU 上的指令计数器，单调时间的检索速度可能比正常时钟源快得多。
 * 在 x86 架构上（例如），RDTSC 指令是用于此目的的非常快的时钟源。
 * */

#include "fmacros.h"
#include <stdint.h>
#include <unistd.h>

/* A counter in micro-seconds.  The 'monotime' type is provided for variables
 * holding a monotonic time.  This will help distinguish & document that the
 * variable is associated with the monotonic clock and should not be confused
 * with other types of time.*/
/**
 * 以微秒为单位的计数器。为保持单调时间的变量提供了 'monotime' 类型。
 * 这将有助于区分和记录该变量与单调时钟相关联，不应与其他类型的时间混淆。
 * */
typedef uint64_t monotime;

/* Retrieve counter of micro-seconds relative to an arbitrary point in time.  */
//检索相对于任意时间点的微秒计数器。
extern monotime (*getMonotonicUs)(void);

typedef enum monotonic_clock_type {
    MONOTONIC_CLOCK_POSIX,
    MONOTONIC_CLOCK_HW,
} monotonic_clock_type;

/* Call once at startup to initialize the monotonic clock.  Though this only
 * needs to be called once, it may be called additional times without impact.
 * Returns a printable string indicating the type of clock initialized.
 * (The returned string is static and doesn't need to be freed.)  */
/**
 * 在启动时调用一次以初始化单调时钟。虽然这只需要调用一次，但它可能会被调用多次而不会产生影响。
 * 返回一个可打印的字符串，指示初始化的时钟类型。 （返回的字符串是静态的，不需要释放。）
 * */
const char *monotonicInit();

/* Return a string indicating the type of monotonic clock being used. */
//返回一个字符串，指示正在使用的单调时钟的类型。
const char *monotonicInfoString();

/* Return the type of monotonic clock being used. */
//返回正在使用的单调时钟的类型。
monotonic_clock_type monotonicGetType();

/* Functions to measure elapsed time.
 * 测量经过时间的功能。
 * Example:
 *     monotime myTimer;
 *     elapsedStart(&myTimer);
 *     while (elapsedMs(myTimer) < 10) {} // loops for 10ms
 */
static inline void elapsedStart(monotime *start_time) {
    *start_time = getMonotonicUs();
}

static inline uint64_t elapsedUs(monotime start_time) {
    return getMonotonicUs() - start_time;
}

static inline uint64_t elapsedMs(monotime start_time) {
    return elapsedUs(start_time) / 1000;
}

#endif
