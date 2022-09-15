/*
 * Copyright (c) 2022, Redis Ltd.
 * Copyright (c) 2016, Salvatore Sanfilippo <antirez at gmail dot com>
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
#include "fmacros.h"
#include "config.h"
#include "syscheck.h"
#include "sds.h"
#include "anet.h"

#include <time.h>
#include <sys/resource.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>

#ifdef __linux__
#include <sys/mman.h>
#endif


#ifdef __linux__
static sds read_sysfs_line(char *path) {
    char buf[256];
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    if (!fgets(buf, sizeof(buf), f)) {
        fclose(f);
        return NULL;
    }
    fclose(f);
    sds res = sdsnew(buf);
    res = sdstrim(res, " \n");
    return res;
}

/* Verify our clokcsource implementation doesn't go through a system call (uses vdso).
 * Going through a system call to check the time degrades Redis performance. */
//验证我们的 clockcsource 实现没有通过系统调用（使用 vdso）。通过系统调用检查时间会降低 Redis 的性能。
static int checkClocksource(sds *error_msg) {
    unsigned long test_time_us, system_hz;
    struct timespec ts;
    unsigned long long start_us;
    struct rusage ru_start, ru_end;

    system_hz = sysconf(_SC_CLK_TCK);

    if (getrusage(RUSAGE_SELF, &ru_start) != 0)
        return 0;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
        return 0;
    }
    start_us = (ts.tv_sec * 1000000 + ts.tv_nsec / 1000);

    /* clock_gettime() busy loop of 5 times system tick (for a system_hz of 100 this is 50ms)
     * Using system_hz is required to ensure accurate measurements from getrusage().
     * If our clocksource is configured correctly (vdso) this will result in no system calls.
     * If our clocksource is inefficient it'll waste most of the busy loop in the kernel. */
    /**
     * clock_gettime() 5 次系统滴答的繁忙循环（对于 100 的 system_hz，这是 50 毫秒）
     * 需要使用 system_hz 来确保 getrusage() 的准确测量。如果我们的时钟源配置正确（vdso），这将导致没有系统调用。
     * 如果我们的时钟源效率低下，它将浪费内核中大部分繁忙的循环。
     * */
    test_time_us = 5 * 1000000 / system_hz;
    while (1) {
        unsigned long long d;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
            return 0;
        d = (ts.tv_sec * 1000000 + ts.tv_nsec / 1000) - start_us;
        if (d >= test_time_us) break;
    }
    if (getrusage(RUSAGE_SELF, &ru_end) != 0)
        return 0;

    long long stime_us = (ru_end.ru_stime.tv_sec * 1000000 + ru_end.ru_stime.tv_usec) - (ru_start.ru_stime.tv_sec * 1000000 + ru_start.ru_stime.tv_usec);
    long long utime_us = (ru_end.ru_utime.tv_sec * 1000000 + ru_end.ru_utime.tv_usec) - (ru_start.ru_utime.tv_sec * 1000000 + ru_start.ru_utime.tv_usec);

    /* If more than 10% of the process time was in system calls
     * we probably have an inefficient clocksource, print a warning */
    //如果超过 10% 的进程时间在系统调用中，我们可能有一个低效的时钟源，打印一个警告
    if (stime_us * 10 > stime_us + utime_us) {
        sds avail = read_sysfs_line("/sys/devices/system/clocksource/clocksource0/available_clocksource");
        sds curr = read_sysfs_line("/sys/devices/system/clocksource/clocksource0/current_clocksource");
        *error_msg = sdscatprintf(sdsempty(),
           "Slow system clocksource detected. This can result in degraded performance. "
           "Consider changing the system's clocksource. "
           "Current clocksource: %s. Available clocksources: %s. "
           "For example: run the command 'echo tsc > /sys/devices/system/clocksource/clocksource0/current_clocksource' as root. "
           "To permanently change the system's clocksource you'll need to set the 'clocksource=' kernel command line parameter.",
           curr ? curr : "", avail ? avail : "");
        sdsfree(avail);
        sdsfree(curr);
        return -1;
    } else {
        return 1;
    }
}

/* Verify we're not using the `xen` clocksource. The xen hypervisor's default clocksource is slow and affects
 * Redis's performance. This has been measured on ec2 xen based instances. ec2 recommends using the non-default
 * tsc clock source for these instances. */
/**
 * 验证我们没有使用 `xen` 时钟源。 xen 管理程序的默认时钟源很慢，会影响 Redis 的性能。
 * 这是在基于 ec2 xen 的实例上测量的。 ec2 建议对这些实例使用非默认 tsc 时钟源。
 * */
int checkXenClocksource(sds *error_msg) {
    sds curr = read_sysfs_line("/sys/devices/system/clocksource/clocksource0/current_clocksource");
    int res = 1;
    if (curr == NULL) {
        res = 0;
    } else if (strcmp(curr, "xen") == 0) {
        *error_msg = sdsnew(
            "Your system is configured to use the 'xen' clocksource which might lead to degraded performance. "
            "Check the result of the [slow-clocksource] system check: run 'redis-server --check-system' to check if "
            "the system's clocksource isn't degrading performance.");
        res = -1;
    }
    sdsfree(curr);
    return res;
}

/* Verify overcommit is enabled.
 * When overcommit memory is disabled Linux will kill the forked child of a background save
 * if we don't have enough free memory to satisfy double the current memory usage even though
 * the forked child uses copy-on-write to reduce its actual memory usage. */
/**
 * 验证是否启用了过量使用。当禁用过度使用内存时，如果我们没有足够的空闲内存来满足当前内存使用量的两倍，
 * Linux 将杀死后台保存的分叉子节点，即使分叉子节点使用写时复制来减少其实际内存使用量。
 * */
int checkOvercommit(sds *error_msg) {
    FILE *fp = fopen("/proc/sys/vm/overcommit_memory","r");
    char buf[64];

    if (!fp) return 0;
    if (fgets(buf,64,fp) == NULL) {
        fclose(fp);
        return 0;
    }
    fclose(fp);

    if (strtol(buf, NULL, 10) == 0) {
        *error_msg = sdsnew(
            "overcommit_memory is set to 0! Background save may fail under low memory condition. "
            "To fix this issue add 'vm.overcommit_memory = 1' to /etc/sysctl.conf and then reboot or run the "
            "command 'sysctl vm.overcommit_memory=1' for this to take effect.");
        return -1;
    } else {
        return 1;
    }
}

/* Make sure transparent huge pages aren't always enabled. When they are this can cause copy-on-write logic
 * to consume much more memory and reduce performance during forks. */
//确保不总是启用透明大页面。当它们出现时，这可能会导致写时复制逻辑消耗更多内存并降低分叉期间的性能。
int checkTHPEnabled(sds *error_msg) {
    char buf[1024];

    FILE *fp = fopen("/sys/kernel/mm/transparent_hugepage/enabled","r");
    if (!fp) return 0;
    if (fgets(buf,sizeof(buf),fp) == NULL) {
        fclose(fp);
        return 0;
    }
    fclose(fp);

    if (strstr(buf,"[always]") != NULL) {
        *error_msg = sdsnew(
            "You have Transparent Huge Pages (THP) support enabled in your kernel. "
            "This will create latency and memory usage issues with Redis. "
            "To fix this issue run the command 'echo madvise > /sys/kernel/mm/transparent_hugepage/enabled' as root, "
            "and add it to your /etc/rc.local in order to retain the setting after a reboot. "
            "Redis must be restarted after THP is disabled (set to 'madvise' or 'never').");
        return -1;
    } else {
        return 1;
    }
}

#ifdef __arm64__
/* Get size in kilobytes of the Shared_Dirty pages of the calling process for the
 * memory map corresponding to the provided address, or -1 on error. */
static int smapsGetSharedDirty(unsigned long addr) {
    int ret, in_mapping = 0, val = -1;
    unsigned long from, to;
    char buf[64];
    FILE *f;

    f = fopen("/proc/self/smaps", "r");
    if (!f) return -1;

    while (1) {
        if (!fgets(buf, sizeof(buf), f))
            break;

        ret = sscanf(buf, "%lx-%lx", &from, &to);
        if (ret == 2)
            in_mapping = from <= addr && addr < to;

        if (in_mapping && !memcmp(buf, "Shared_Dirty:", 13)) {
            sscanf(buf, "%*s %d", &val);
            /* If parsing fails, we remain with val == -1 */
            break;
        }
    }

    fclose(f);
    return val;
}

/* Older arm64 Linux kernels have a bug that could lead to data corruption
 * during background save in certain scenarios. This function checks if the
 * kernel is affected.
 * The bug was fixed in commit ff1712f953e27f0b0718762ec17d0adb15c9fd0b
 * titled: "arm64: pgtable: Ensure dirty bit is preserved across pte_wrprotect()"
 */
int checkLinuxMadvFreeForkBug(sds *error_msg) {
    int ret, pipefd[2] = { -1, -1 };
    pid_t pid;
    char *p = NULL, *q;
    int res = 1;
    long page_size = sysconf(_SC_PAGESIZE);
    long map_size = 3 * page_size;

    /* Create a memory map that's in our full control (not one used by the allocator). */
    p = mmap(NULL, map_size, PROT_READ, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) {
        return 0;
    }

    q = p + page_size;

    /* Split the memory map in 3 pages by setting their protection as RO|RW|RO to prevent
     * Linux from merging this memory map with adjacent VMAs. */
    ret = mprotect(q, page_size, PROT_READ | PROT_WRITE);
    if (ret < 0) {
        res = 0;
        goto exit;
    }

    /* Write to the page once to make it resident */
    *(volatile char*)q = 0;

    /* Tell the kernel that this page is free to be reclaimed. */
#ifndef MADV_FREE
#define MADV_FREE 8
#endif
    ret = madvise(q, page_size, MADV_FREE);
    if (ret < 0) {
        /* MADV_FREE is not available on older kernels that are presumably
         * not affected. */
        if (errno == EINVAL) goto exit;

        res = 0;
        goto exit;
    }

    /* Write to the page after being marked for freeing, this is supposed to take
     * ownership of that page again. */
    *(volatile char*)q = 0;

    /* Create a pipe for the child to return the info to the parent. */
    ret = anetPipe(pipefd, 0, 0);
    if (ret < 0) {
        res = 0;
        goto exit;
    }

    /* Fork the process. */
    pid = fork();
    if (pid < 0) {
        res = 0;
        goto exit;
    } else if (!pid) {
        /* Child: check if the page is marked as dirty, page_size in kb.
         * A value of 0 means the kernel is affected by the bug. */
        ret = smapsGetSharedDirty((unsigned long) q);
        if (!ret)
            res = -1;
        else if (ret == -1)     /* Failed to read */
            res = 0;

        ret = write(pipefd[1], &res, sizeof(res)); /* Assume success, ignore return value*/
        exit(0);
    } else {
        /* Read the result from the child. */
        ret = read(pipefd[0], &res, sizeof(res));
        if (ret < 0) {
            res = 0;
        }

        /* Reap the child pid. */
        waitpid(pid, NULL, 0);
    }

exit:
    /* Cleanup */
    if (pipefd[0] != -1) close(pipefd[0]);
    if (pipefd[1] != -1) close(pipefd[1]);
    if (p != NULL) munmap(p, map_size);

    if (res == -1)
        *error_msg = sdsnew(
            "Your kernel has a bug that could lead to data corruption during background save. "
            "Please upgrade to the latest stable kernel.");

    return res;
}
#endif /* __arm64__ */
#endif /* __linux__ */

/*
 * Standard system check interface:
 * Each check has a name `name` and a functions pointer `check_fn`.
 * `check_fn` should return:
 *   -1 in case the check fails.
 *   1 in case the check passes.
 *   0 in case the check could not be completed (usually because of some unexpected failed system call).
 *   When (and only when) the check fails and -1 is returned and error description is places in a new sds pointer to by
 *   the single `sds*` argument to `check_fn`. This message should be freed by the caller via `sdsfree()`.
 */
/**
 * 标准系统检查接口：每个检查都有一个名称`name`和一个函数指针`check_fn`。 `check_fn`
 * 应该返回：
 *   -1 以防检查失败。
 *   1 如果检查通过。
 *   0 以防检查无法完成（通常是由于某些意外的失败系统调用）。
 * 当（且仅当）检查失败并返回 -1 并且错误描述被放置在新的 sds 指针中，该指针由 `check_fn` 的单个 `sds` 参数指向。
 * 调用者应通过 `sdsfree()` 释放此消息。
 * */
typedef struct {
    const char *name;
    int (*check_fn)(sds*);
} check;

check checks[] = {
#ifdef __linux__
    {.name = "slow-clocksource", .check_fn = checkClocksource},
    {.name = "xen-clocksource", .check_fn = checkXenClocksource},
    {.name = "overcommit", .check_fn = checkOvercommit},
    {.name = "THP", .check_fn = checkTHPEnabled},
#ifdef __arm64__
    {.name = "madvise-free-fork-bug", .check_fn = checkLinuxMadvFreeForkBug},
#endif
#endif
    {.name = NULL, .check_fn = NULL}
};

/* Performs various system checks, returns 0 if any check fails, 1 otherwise. */
//执行各种系统检查，如果任何检查失败则返回 0，否则返回 1。
int syscheck(void) {
    check *cur_check = checks;
    int ret = 1;
    sds err_msg = NULL;
    while (cur_check->check_fn) {
        int res = cur_check->check_fn(&err_msg);
        printf("[%s]...", cur_check->name);
        if (res == 0) {
            printf("skipped\n");
        } else if (res == 1) {
            printf("OK\n");
        } else {
            printf("WARNING:\n");
            printf("%s\n", err_msg);
            sdsfree(err_msg);
            ret = 0;
        }
        cur_check++;
    }

    return ret;
}
