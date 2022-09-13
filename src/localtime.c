/*
 * Copyright (c) 2018, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <time.h>

/* This is a safe version of localtime() which contains no locks and is
 * fork() friendly. Even the _r version of localtime() cannot be used safely
 * in Redis. Another thread may be calling localtime() while the main thread
 * forks(). Later when the child process calls localtime() again, for instance
 * in order to log something to the Redis log, it may deadlock: in the copy
 * of the address space of the forked process the lock will never be released.
 *
 * This function takes the timezone 'tz' as argument, and the 'dst' flag is
 * used to check if daylight saving time is currently in effect. The caller
 * of this function should obtain such information calling tzset() ASAP in the
 * main() function to obtain the timezone offset from the 'timezone' global
 * variable. To obtain the daylight information, if it is currently active or not,
 * one trick is to call localtime() in main() ASAP as well, and get the
 * information from the tm_isdst field of the tm structure. However the daylight
 * time may switch in the future for long running processes, so this information
 * should be refreshed at safe times.
 *
 * Note that this function does not work for dates < 1/1/1970, it is solely
 * designed to work with what time(NULL) may return, and to support Redis
 * logging of the dates, it's not really a complete implementation. */
/**
 * 这是 localtime() 的安全版本，它不包含锁并且对 fork() 友好。即使是 _r 版本的 localtime() 也不能在 Redis 中安全使用。
 * 当主线程 forks() 时，另一个线程可能正在调用 localtime()。
 * 稍后当子进程再次调用 localtime() 时，例如为了将某些内容记录到 Redis 日志中，它可能会死锁：
 * 在分叉进程的地址空间副本中，锁永远不会被释放。此函数将时区“tz”作为参数，“dst”标志用于检查夏令时当前是否有效。
 * 此函数的调用者应尽快在 main() 函数中调用 tzset() 获取此类信息，以从“timezone”全局变量中获取时区偏移量。
 * 要获取日光信息，如果它当前是否处于活动状态，一个技巧是尽快在 main() 中调用 localtime()，
 * 并从 tm 结构的 tm_isdst 字段中获取信息。然而，日光时间可能会在将来切换到长时间运行的进程，因此应在安全时间刷新此信息。
 * 请注意，此函数不适用于日期 < 111970，它仅用于处理 time(NULL) 可能返回的时间，并支持 Redis 记录日期，它并不是真正的完整实现。
 * */
static int is_leap_year(time_t year) {
    if (year % 4) return 0;         /* A year not divisible by 4 is not leap. 不能被 4 整除的年份不是闰年。*/
    else if (year % 100) return 1;  /* If div by 4 and not 100 is surely leap. 如果 div 由 4 而不是 100 肯定是飞跃。*/
    else if (year % 400) return 0;  /* If div by 100 *and* not by 400 is not leap. 如果 div 100 而不是 400 不是飞跃。*/
    else return 1;                  /* If div by 100 and 400 is leap. 如果 div 100 和 400 是飞跃。*/
}

void nolocks_localtime(struct tm *tmp, time_t t, time_t tz, int dst) {
    const time_t secs_min = 60;
    const time_t secs_hour = 3600;
    const time_t secs_day = 3600*24;

    t -= tz;                            /* Adjust for timezone. 调整时区。*/
    t += 3600*dst;                      /* Adjust for daylight time. 调整为白天时间。*/
    time_t days = t / secs_day;         /* Days passed since epoch. 自纪元以来已经过去了几天。*/
    time_t seconds = t % secs_day;      /* Remaining seconds. 剩余秒数。*/

    tmp->tm_isdst = dst;
    tmp->tm_hour = seconds / secs_hour;
    tmp->tm_min = (seconds % secs_hour) / secs_min;
    tmp->tm_sec = (seconds % secs_hour) % secs_min;

    /* 1/1/1970 was a Thursday, that is, day 4 from the POV of the tm structure
     * where sunday = 0, so to calculate the day of the week we have to add 4
     * and take the modulo by 7. */
    //111970 是星期四，即从 tm 结构的 POV 算起的第 4 天，其中 sunday = 0，因此要计算星期几，我们必须加 4 并取模数 7。
    tmp->tm_wday = (days+4)%7;

    /* Calculate the current year. 计算当前年份。*/
    tmp->tm_year = 1970;
    while(1) {
        /* Leap years have one day more. 闰年多一天。*/
        time_t days_this_year = 365 + is_leap_year(tmp->tm_year);
        if (days_this_year > days) break;
        days -= days_this_year;
        tmp->tm_year++;
    }
    tmp->tm_yday = days;  /* Number of day of the current year. 当年的天数。*/

    /* We need to calculate in which month and day of the month we are. To do
     * so we need to skip days according to how many days there are in each
     * month, and adjust for the leap year that has one more day in February. */
    //我们需要计算我们所处的月份和日期。为此，我们需要根据每个月的天数跳过天数，并针对二月多一天的闰年进行调整。
    int mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    mdays[1] += is_leap_year(tmp->tm_year);

    tmp->tm_mon = 0;
    while(days >= mdays[tmp->tm_mon]) {
        days -= mdays[tmp->tm_mon];
        tmp->tm_mon++;
    }

    tmp->tm_mday = days+1;  /* Add 1 since our 'days' is zero-based. 添加 1，因为我们的“天数”是从零开始的。*/
    tmp->tm_year -= 1900;   /* Surprisingly tm_year is year-1900. 令人惊讶的是 tm_year 是 1900 年。*/
}

#ifdef LOCALTIME_TEST_MAIN
#include <stdio.h>

int main(void) {
    /* Obtain timezone and daylight info. */
    tzset(); /* Now 'timezone' global is populated. */
    time_t t = time(NULL);
    struct tm *aux = localtime(&t);
    int daylight_active = aux->tm_isdst;

    struct tm tm;
    char buf[1024];

    nolocks_localtime(&tm,t,timezone,daylight_active);
    strftime(buf,sizeof(buf),"%d %b %H:%M:%S",&tm);
    printf("[timezone: %d, dl: %d] %s\n", (int)timezone, (int)daylight_active, buf);
}
#endif
