/* Maxmemory directive handling (LRU eviction and other policies).
 *
 * ----------------------------------------------------------------------------
 *
 * Copyright (c) 2009-2016, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include "server.h"
#include "bio.h"
#include "atomicvar.h"
#include "script.h"
#include <math.h>

/* ----------------------------------------------------------------------------
 * Data structures
 * --------------------------------------------------------------------------*/

/* To improve the quality of the LRU approximation we take a set of keys
 * that are good candidate for eviction across performEvictions() calls.
 *
 * Entries inside the eviction pool are taken ordered by idle time, putting
 * greater idle times to the right (ascending order).
 *
 * When an LFU policy is used instead, a reverse frequency indication is used
 * instead of the idle time, so that we still evict by larger value (larger
 * inverse frequency means to evict keys with the least frequent accesses).
 *
 * Empty entries have the key pointer set to NULL. */
/**为了提高 LRU 近似的质量，我们采用一组键，它们是跨 performEvictions() 调用的驱逐的良好候选者。
 * 驱逐池中的条目按空闲时间排序，将更大的空闲时间放在右边（升序）。
 * 当使用 LFU 策略时，使用反向频率指示代替空闲时间，因此我们仍然按较大的值逐出（较大的反向频率意味着逐出访问频率最低的键）。
 * 空条目的键指针设置为 NULL。*/
#define EVPOOL_SIZE 16
#define EVPOOL_CACHED_SDS_SIZE 255
struct evictionPoolEntry {
    unsigned long long idle;    /* Object idle time (inverse frequency for LFU) 对象空闲时间（LFU 的反频率）*/
    sds key;                    /* Key name. */
    sds cached;                 /* Cached SDS object for key name. 键名的缓存 SDS 对象。*/
    int dbid;                   /* Key DB number. */
};

static struct evictionPoolEntry *EvictionPoolLRU;

/* ----------------------------------------------------------------------------
 * Implementation of eviction, aging and LRU
 * 驱逐、老化和LRU的实施
 * --------------------------------------------------------------------------*/

/* Return the LRU clock, based on the clock resolution. This is a time
 * in a reduced-bits format that can be used to set and check the
 * object->lru field of redisObject structures. */
//根据时钟分辨率返回 LRU 时钟。这是一个减少位格式的时间，可用于设置和检查 redisObject 结构的 object->lru 字段。
unsigned int getLRUClock(void) {
    return (mstime()/LRU_CLOCK_RESOLUTION) & LRU_CLOCK_MAX;
}

/* This function is used to obtain the current LRU clock.
 * If the current resolution is lower than the frequency we refresh the
 * LRU clock (as it should be in production servers) we return the
 * precomputed value, otherwise we need to resort to a system call. */
/**该函数用于获取当前的 LRU 时钟。如果当前分辨率低于我们刷新 LRU 时钟的频率
 * （因为它应该在生产服务器中）我们返回预先计算的值，否则我们需要求助于系统调用。*/
unsigned int LRU_CLOCK(void) {
    unsigned int lruclock;
    if (1000/server.hz <= LRU_CLOCK_RESOLUTION) {
        atomicGet(server.lruclock,lruclock);
    } else {
        lruclock = getLRUClock();
    }
    return lruclock;
}

/* Given an object returns the min number of milliseconds the object was never
 * requested, using an approximated LRU algorithm. */
//给定一个对象，使用近似的 LRU 算法返回该对象从未被请求的最小毫秒数。
unsigned long long estimateObjectIdleTime(robj *o) {
    unsigned long long lruclock = LRU_CLOCK();
    if (lruclock >= o->lru) {
        return (lruclock - o->lru) * LRU_CLOCK_RESOLUTION;
    } else {
        return (lruclock + (LRU_CLOCK_MAX - o->lru)) *
                    LRU_CLOCK_RESOLUTION;
    }
}

/* LRU approximation algorithm
 *
 * Redis uses an approximation of the LRU algorithm that runs in constant
 * memory. Every time there is a key to expire, we sample N keys (with
 * N very small, usually in around 5) to populate a pool of best keys to
 * evict of M keys (the pool size is defined by EVPOOL_SIZE).
 *
 * The N keys sampled are added in the pool of good keys to expire (the one
 * with an old access time) if they are better than one of the current keys
 * in the pool.
 *
 * After the pool is populated, the best key we have in the pool is expired.
 * However note that we don't remove keys from the pool when they are deleted
 * so the pool may contain keys that no longer exist.
 *
 * When we try to evict a key, and all the entries in the pool don't exist
 * we populate it again. This time we'll be sure that the pool has at least
 * one key that can be evicted, if there is at least one key that can be
 * evicted in the whole database. */
/**LRU 近似算法 Redis 使用在恒定内存中运行的 LRU 算法的近似值。
 * 每次有一个密钥过期，我们采样 N 个密钥（N 非常小，通常在 5 左右）来填充一个最佳密钥池，
 * 以驱逐 M 个密钥（池大小由 EVPOOL_SIZE 定义）。如果采样的 N 个密钥优于池中的当前密钥之一，
 * 则将其添加到好密钥池中以过期（具有旧访问时间的密钥）。填充池后，我们在池中拥有的最佳密钥已过期。
 * 但是请注意，当密钥被删除时，我们不会从池中删除它们，因此池可能包含不再存在的密钥。
 * 当我们试图驱逐一个键，并且池中的所有条目都不存在时，我们会再次填充它。
 * 这一次我们将确保池中至少有一个可以被驱逐的密钥，如果整个数据库中至少有一个可以被驱逐的密钥。*/

/* Create a new eviction pool. */
void evictionPoolAlloc(void) {
    struct evictionPoolEntry *ep;
    int j;

    ep = zmalloc(sizeof(*ep)*EVPOOL_SIZE);
    for (j = 0; j < EVPOOL_SIZE; j++) {
        ep[j].idle = 0;
        ep[j].key = NULL;
        ep[j].cached = sdsnewlen(NULL,EVPOOL_CACHED_SDS_SIZE);
        ep[j].dbid = 0;
    }
    EvictionPoolLRU = ep;
}

/* This is a helper function for performEvictions(), it is used in order
 * to populate the evictionPool with a few entries every time we want to
 * expire a key. Keys with idle time bigger than one of the current
 * keys are added. Keys are always added if there are free entries.
 *
 * We insert keys on place in ascending order, so keys with the smaller
 * idle time are on the left, and keys with the higher idle time on the
 * right. */
/**这是 performEvictions() 的辅助函数，它用于在每次我们想要使密钥过期时用一些条目填充 evictionPool。
 * 添加空闲时间大于当前密钥之一的密钥。如果有空闲条目，则始终添加密钥。
 * 我们按升序插入键，因此空闲时间较短的键位于左侧，空闲时间较长的键位于右侧。*/

void evictionPoolPopulate(int dbid, dict *sampledict, dict *keydict, struct evictionPoolEntry *pool) {
    int j, k, count;
    dictEntry *samples[server.maxmemory_samples];

    count = dictGetSomeKeys(sampledict,samples,server.maxmemory_samples);
    for (j = 0; j < count; j++) {
        unsigned long long idle;
        sds key;
        robj *o;
        dictEntry *de;

        de = samples[j];
        key = dictGetKey(de);

        /* If the dictionary we are sampling from is not the main
         * dictionary (but the expires one) we need to lookup the key
         * again in the key dictionary to obtain the value object. */
        //如果我们从中采样的字典不是主字典（而是过期字典），我们需要在键字典中再次查找键以获取值对象。
        if (server.maxmemory_policy != MAXMEMORY_VOLATILE_TTL) {
            if (sampledict != keydict) de = dictFind(keydict, key);
            o = dictGetVal(de);
        }

        /* Calculate the idle time according to the policy. This is called
         * idle just because the code initially handled LRU, but is in fact
         * just a score where an higher score means better candidate. */
        //根据策略计算空闲时间。这被称为空闲只是因为代码最初处理的是 LRU，但实际上只是一个分数，分数越高意味着候选者越好。
        if (server.maxmemory_policy & MAXMEMORY_FLAG_LRU) {
            idle = estimateObjectIdleTime(o);
        } else if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
            /* When we use an LRU policy, we sort the keys by idle time
             * so that we expire keys starting from greater idle time.
             * However when the policy is an LFU one, we have a frequency
             * estimation, and we want to evict keys with lower frequency
             * first. So inside the pool we put objects using the inverted
             * frequency subtracting the actual frequency to the maximum
             * frequency of 255. */
            /**当我们使用 LRU 策略时，我们按空闲时间对密钥进行排序，以便我们从更大的空闲时间开始使密钥过期。
             * 然而，当策略是 LFU 策略时，我们有一个频率估计，我们希望首先驱逐频率较低的键。
             * 因此，在池中，我们使用反转频率减去实际频率到最大频率 255 放置对象。*/
            idle = 255-LFUDecrAndReturn(o);
        } else if (server.maxmemory_policy == MAXMEMORY_VOLATILE_TTL) {
            /* In this case the sooner the expire the better. */
            //在这种情况下，越早过期越好。
            idle = ULLONG_MAX - (long)dictGetVal(de);
        } else {
            serverPanic("Unknown eviction policy in evictionPoolPopulate()");
        }

        /* Insert the element inside the pool.
         * First, find the first empty bucket or the first populated
         * bucket that has an idle time smaller than our idle time. */
        //将元素插入池中。首先，找到空闲时间小于我们空闲时间的第一个空桶或第一个填充桶。
        k = 0;
        while (k < EVPOOL_SIZE &&
               pool[k].key &&
               pool[k].idle < idle) k++;
        if (k == 0 && pool[EVPOOL_SIZE-1].key != NULL) {
            /* Can't insert if the element is < the worst element we have
             * and there are no empty buckets. */
            //如果元素 < 我们拥有的最差元素并且没有空桶，则无法插入。
            continue;
        } else if (k < EVPOOL_SIZE && pool[k].key == NULL) {
            /* Inserting into empty position. No setup needed before insert. */
            //插入空位。插入前无需设置。
        } else {
            /* Inserting in the middle. Now k points to the first element
             * greater than the element to insert.  */
            //插入中间。现在 k 指向第一个大于要插入的元素的元素。
            if (pool[EVPOOL_SIZE-1].key == NULL) {
                /* Free space on the right? Insert at k shifting
                 * all the elements from k to end to the right.
                 * 右边的空闲空间？在 k 处插入，将所有元素从 k 到 end 向右移动。*/

                /* Save SDS before overwriting. 覆盖前保存 SDS。*/
                sds cached = pool[EVPOOL_SIZE-1].cached;
                memmove(pool+k+1,pool+k,
                    sizeof(pool[0])*(EVPOOL_SIZE-k-1));
                pool[k].cached = cached;
            } else {
                /* No free space on right? Insert at k-1 右边没有空闲空间？在 k-1 处插入*/
                k--;
                /* Shift all elements on the left of k (included) to the
                 * left, so we discard the element with smaller idle time. */
                //将k（包括）左边的所有元素向左移动，所以我们丢弃空闲时间较小的元素。
                sds cached = pool[0].cached; /* Save SDS before overwriting. 覆盖前保存 SDS。*/
                if (pool[0].key != pool[0].cached) sdsfree(pool[0].key);
                memmove(pool,pool+1,sizeof(pool[0])*k);
                pool[k].cached = cached;
            }
        }

        /* Try to reuse the cached SDS string allocated in the pool entry,
         * because allocating and deallocating this object is costly
         * (according to the profiler, not my fantasy. Remember:
         * premature optimization bla bla bla. */
        //尝试重用池条目中分配的缓存 SDS 字符串，因为分配和解除分配此对象的成本很高
        // （根据分析器，不是我的幻想。记住：过早优化 bla bla bla。
        int klen = sdslen(key);
        if (klen > EVPOOL_CACHED_SDS_SIZE) {
            pool[k].key = sdsdup(key);
        } else {
            memcpy(pool[k].cached,key,klen+1);
            sdssetlen(pool[k].cached,klen);
            pool[k].key = pool[k].cached;
        }
        pool[k].idle = idle;
        pool[k].dbid = dbid;
    }
}

/* ----------------------------------------------------------------------------
 * LFU (Least Frequently Used) implementation.

 * We have 24 total bits of space in each object in order to implement
 * an LFU (Least Frequently Used) eviction policy, since we re-use the
 * LRU field for this purpose.
 *
 * We split the 24 bits into two fields:
 * LFU（最不常用）实现。为了实现 LFU（最不常用）驱逐策略，我们在每个对象中总共有 24 位空间，
 * 因为我们为此目的重用了 LRU 字段。我们将 24 位分成两个字段：
 *
 *          16 bits      8 bits
 *     +----------------+--------+
 *     + Last decr time | LOG_C  |
 *     +----------------+--------+
 *
 * LOG_C is a logarithmic counter that provides an indication of the access
 * frequency. However this field must also be decremented otherwise what used
 * to be a frequently accessed key in the past, will remain ranked like that
 * forever, while we want the algorithm to adapt to access pattern changes.
 * LOG_C 是一个对数计数器，提供访问频率的指示。然而，这个字段也必须递减，
 * 否则过去经常访问的密钥将永远保持这样的排名，而我们希望算法适应访问模式的变化。
 *
 * So the remaining 16 bits are used in order to store the "decrement time",
 * a reduced-precision Unix time (we take 16 bits of the time converted
 * in minutes since we don't care about wrapping around) where the LOG_C
 * counter is halved if it has an high value, or just decremented if it
 * has a low value.
 * 所以剩余的 16 位用于存储“递减时间”，即降低精度的 Unix 时间（我们将 16 位的时间转换为分钟，因为我们不关心回绕），
 * 其中 LOG_C 计数器减半如果它具有高值，或者如果它具有低值则减少。
 *
 * New keys don't start at zero, in order to have the ability to collect
 * some accesses before being trashed away, so they start at LFU_INIT_VAL.
 * The logarithmic increment performed on LOG_C takes care of LFU_INIT_VAL
 * when incrementing the key, so that keys starting at LFU_INIT_VAL
 * (or having a smaller value) have a very high chance of being incremented
 * on access.
 * 新密钥不会从零开始，以便能够在被丢弃之前收集一些访问权限，因此它们从 LFU_INIT_VAL 开始。
 * 在 LOG_C 上执行的对数递增在递增密钥时会处理 LFU_INIT_VAL，
 * 因此从 LFU_INIT_VAL（或具有较小值）开始的密钥在访问时很有可能被递增。
 *
 * During decrement, the value of the logarithmic counter is halved if
 * its current value is greater than two times the LFU_INIT_VAL, otherwise
 * it is just decremented by one.
 * 在递减过程中，如果对数计数器的当前值大于 LFU_INIT_VAL 的两倍，则对数计数器的值减半，否则只递减 1。
 * --------------------------------------------------------------------------*/

/* Return the current time in minutes, just taking the least significant
 * 16 bits. The returned time is suitable to be stored as LDT (last decrement
 * time) for the LFU implementation. */
//以分钟为单位返回当前时间，仅取最低有效 16 位。返回的时间适合存储为 LDT（最后递减时间）以用于 LFU 实现。
unsigned long LFUGetTimeInMinutes(void) {
    return (server.unixtime/60) & 65535;
}

/* Given an object last access time, compute the minimum number of minutes
 * that elapsed since the last access. Handle overflow (ldt greater than
 * the current 16 bits minutes time) considering the time as wrapping
 * exactly once. */
//给定一个对象的上次访问时间，计算自上次访问以来经过的最小分钟数。
// 处理溢出（ldt 大于当前的 16 位分钟时间），将时间视为恰好回绕一次。
unsigned long LFUTimeElapsed(unsigned long ldt) {
    unsigned long now = LFUGetTimeInMinutes();
    if (now >= ldt) return now-ldt;
    return 65535-ldt+now;
}

/* Logarithmically increment a counter. The greater is the current counter value
 * the less likely is that it gets really incremented. Saturate it at 255. */
//对数递增一个计数器。当前计数器值越大，它真正增加的可能性就越小。在 255 饱和。
uint8_t LFULogIncr(uint8_t counter) {
    if (counter == 255) return 255;
    double r = (double)rand()/RAND_MAX;
    double baseval = counter - LFU_INIT_VAL;
    if (baseval < 0) baseval = 0;
    double p = 1.0/(baseval*server.lfu_log_factor+1);
    if (r < p) counter++;
    return counter;
}

/* If the object decrement time is reached decrement the LFU counter but
 * do not update LFU fields of the object, we update the access time
 * and counter in an explicit way when the object is really accessed.
 * And we will times halve the counter according to the times of
 * elapsed time than server.lfu_decay_time.
 * Return the object frequency counter.
 * 如果达到对象递减时间，则递减 LFU 计数器但不更新对象的 LFU 字段，当对象真正被访问时，
 * 我们会以显式方式更新访问时间和计数器。我们将根据经过的时间将计数器减半，
 * 而不是 server.lfu_decay_time。返回对象频率计数器。
 *
 * This function is used in order to scan the dataset for the best object
 * to fit: as we check for the candidate, we incrementally decrement the
 * counter of the scanned objects if needed.
 * 此函数用于扫描数据集以找到最适合的对象：当我们检查候选对象时，如果需要，我们会递增地减少扫描对象的计数器。*/
unsigned long LFUDecrAndReturn(robj *o) {
    unsigned long ldt = o->lru >> 8;
    unsigned long counter = o->lru & 255;
    unsigned long num_periods = server.lfu_decay_time ? LFUTimeElapsed(ldt) / server.lfu_decay_time : 0;
    if (num_periods)
        counter = (num_periods > counter) ? 0 : counter - num_periods;
    return counter;
}

/* We don't want to count AOF buffers and slaves output buffers as
 * used memory: the eviction should use mostly data size, because
 * it can cause feedback-loop when we push DELs into them, putting
 * more and more DELs will make them bigger, if we count them, we
 * need to evict more keys, and then generate more DELs, maybe cause
 * massive eviction loop, even all keys are evicted.
 *
 * This function returns the sum of AOF and replication buffer. */
/**我们不想将 AOF 缓冲区和从属输出缓冲区计为已用内存：驱逐应该主要使用数据大小，因为当我们将 DEL 推入其中时会导致反馈循环，
 * 如果放入越来越多的 DEL 会使它们变得更大，如果我们计算它们，我们需要驱逐更多的密钥，然后生成更多的 DEL，
 * 可能会导致大规模的驱逐循环，甚至所有的密钥都被驱逐。此函数返回 AOF 和复制缓冲区的总和。*/
size_t freeMemoryGetNotCountedMemory(void) {
    size_t overhead = 0;

    /* Since all replicas and replication backlog share global replication
     * buffer, we think only the part of exceeding backlog size is the extra
     * separate consumption of replicas.
     *
     * Note that although the backlog is also initially incrementally grown
     * (pushing DELs consumes memory), it'll eventually stop growing and
     * remain constant in size, so even if its creation will cause some
     * eviction, it's capped, and also here to stay (no resonance effect)
     *
     * Note that, because we trim backlog incrementally in the background,
     * backlog size may exceeds our setting if slow replicas that reference
     * vast replication buffer blocks disconnect. To avoid massive eviction
     * loop, we don't count the delayed freed replication backlog into used
     * memory even if there are no replicas, i.e. we still regard this memory
     * as replicas'. */
    /**由于所有副本和复制积压共享全局复制缓冲区，我们认为只有超出积压大小的部分是副本的额外单独消耗。
     * 请注意，虽然积压工作最初也是逐渐增长的（推送 DEL 会消耗内存），但它最终会停止增长并保持大小不变，
     * 因此即使它的创建会导致一些驱逐，它也会被封顶，并且会保留下来（没有共振效果）
     * 请注意，由于我们在后台逐步修剪积压，如果引用大量复制缓冲区块的慢速副本断开连接，积压大小可能会超过我们的设置。
     * 为了避免大规模的驱逐循环，即使没有副本，我们也不会将延迟释放的复制积压计入已用内存，即我们仍然将此内存视为副本。*/
    if ((long long)server.repl_buffer_mem > server.repl_backlog_size) {
        /* We use list structure to manage replication buffer blocks, so backlog
         * also occupies some extra memory, we can't know exact blocks numbers,
         * we only get approximate size according to per block size. */
        //我们使用列表结构来管理复制缓冲区块，因此积压也占用了一些额外的内存，
        // 我们无法知道确切的块数，我们只能根据每个块的大小得到大概的大小。
        size_t extra_approx_size =
            (server.repl_backlog_size/PROTO_REPLY_CHUNK_BYTES + 1) *
            (sizeof(replBufBlock)+sizeof(listNode));
        size_t counted_mem = server.repl_backlog_size + extra_approx_size;
        if (server.repl_buffer_mem > counted_mem) {
            overhead += (server.repl_buffer_mem - counted_mem);
        }
    }

    if (server.aof_state != AOF_OFF) {
        overhead += sdsAllocSize(server.aof_buf);
    }
    return overhead;
}

/* Get the memory status from the point of view of the maxmemory directive:
 * if the memory used is under the maxmemory setting then C_OK is returned.
 * Otherwise, if we are over the memory limit, the function returns
 * C_ERR.
 *
 * The function may return additional info via reference, only if the
 * pointers to the respective arguments is not NULL. Certain fields are
 * populated only when C_ERR is returned:
 *
 *  'total'     total amount of bytes used.
 *              (Populated both for C_ERR and C_OK)
 *
 *  'logical'   the amount of memory used minus the slaves/AOF buffers.
 *              (Populated when C_ERR is returned)
 *
 *  'tofree'    the amount of memory that should be released
 *              in order to return back into the memory limits.
 *              (Populated when C_ERR is returned)
 *
 *  'level'     this usually ranges from 0 to 1, and reports the amount of
 *              memory currently used. May be > 1 if we are over the memory
 *              limit.
 *              (Populated both for C_ERR and C_OK)
 */
/**从 maxmemory 指令的角度获取内存状态：如果使用的内存低于 maxmemory 设置，则返回 C_OK。
 * 否则，如果超出内存限制，该函数将返回 C_ERR。仅当指向相应参数的指针不为 NULL 时，该函数才能通过引用返回附加信息。
 * 仅当返回 C_ERR 时才会填充某些字段：
 *   'total' 使用的总字节数。 （为 C_ERR 和 C_OK 填充）
 *   'logical' 使用的内存量减去 slavesAOF 缓冲区。 （在返回 C_ERR 时填充）
 *   'tofree' 为了返回内存限制而应该释放的内存量。（在返回 C_ERR 时填充）
 *   'level' 这通常范围从 0 到 1，并报告当前使用的内存量。如果我们超过内存限制，则可能 > 1。为 C_ERR 和 C_OK 填充）*/
int getMaxmemoryState(size_t *total, size_t *logical, size_t *tofree, float *level) {
    size_t mem_reported, mem_used, mem_tofree;

    /* Check if we are over the memory usage limit. If we are not, no need
     * to subtract the slaves output buffers. We can just return ASAP. */
    //检查我们是否超过了内存使用限制。如果我们不是，则无需减去从属输出缓冲区。我们可以尽快返回。
    mem_reported = zmalloc_used_memory();
    if (total) *total = mem_reported;

    /* We may return ASAP if there is no need to compute the level. */
    //如果不需要计算级别，我们可能会尽快返回。
    if (!server.maxmemory) {
        if (level) *level = 0;
        return C_OK;
    }
    if (mem_reported <= server.maxmemory && !level) return C_OK;

    /* Remove the size of slaves output buffers and AOF buffer from the
     * count of used memory. */
    //从已用内存计数中删除从属输出缓冲区和 AOF 缓冲区的大小。
    mem_used = mem_reported;
    size_t overhead = freeMemoryGetNotCountedMemory();
    mem_used = (mem_used > overhead) ? mem_used-overhead : 0;

    /* Compute the ratio of memory usage. 计算内存使用率。 */
    if (level) *level = (float)mem_used / (float)server.maxmemory;

    if (mem_reported <= server.maxmemory) return C_OK;

    /* Check if we are still over the memory limit. 检查我们是否仍然超过内存限制。*/
    if (mem_used <= server.maxmemory) return C_OK;

    /* Compute how much memory we need to free. 计算我们需要释放多少内存。*/
    mem_tofree = mem_used - server.maxmemory;

    if (logical) *logical = mem_used;
    if (tofree) *tofree = mem_tofree;

    return C_ERR;
}

/* Return 1 if used memory is more than maxmemory after allocating more memory,
 * return 0 if not. Redis may reject user's requests or evict some keys if used
 * memory exceeds maxmemory, especially, when we allocate huge memory at once. */
/**如果分配更多内存后使用的内存大于 maxmemory，则返回 1，否则返回 0。
 * 如果使用的内存超过 maxmemory，Redis 可能会拒绝用户的请求或驱逐一些键，特别是当我们一次分配大量内存时。*/
int overMaxmemoryAfterAlloc(size_t moremem) {
    if (!server.maxmemory) return  0; /* No limit. */

    /* Check quickly. */
    size_t mem_used = zmalloc_used_memory();
    if (mem_used + moremem <= server.maxmemory) return 0;

    size_t overhead = freeMemoryGetNotCountedMemory();
    mem_used = (mem_used > overhead) ? mem_used - overhead : 0;
    return mem_used + moremem > server.maxmemory;
}

/* The evictionTimeProc is started when "maxmemory" has been breached and
 * could not immediately be resolved.  This will spin the event loop with short
 * eviction cycles until the "maxmemory" condition has resolved or there are no
 * more evictable items.  */
/** evictionTimeProc 在“maxmemory”被破坏且无法立即解决时启动。
 * 这将以较短的驱逐周期旋转事件循环，直到“maxmemory”条件解决或不再有可驱逐的项目。*/
static int isEvictionProcRunning = 0;
static int evictionTimeProc(
        struct aeEventLoop *eventLoop, long long id, void *clientData) {
    UNUSED(eventLoop);
    UNUSED(id);
    UNUSED(clientData);

    if (performEvictions() == EVICT_RUNNING) return 0;  /* keep evicting 继续驱逐*/

    /* For EVICT_OK - things are good, no need to keep evicting.
     * For EVICT_FAIL - there is nothing left to evict.  */
    //对于 EVICT_OK - 情况很好，无需继续驱逐。对于 EVICT_FAIL - 没有什么可以驱逐的了。
    isEvictionProcRunning = 0;
    return AE_NOMORE;
}

void startEvictionTimeProc(void) {
    if (!isEvictionProcRunning) {
        isEvictionProcRunning = 1;
        aeCreateTimeEvent(server.el, 0,
                evictionTimeProc, NULL, NULL);
    }
}

/* Check if it's safe to perform evictions.
 *   Returns 1 if evictions can be performed
 *   Returns 0 if eviction processing should be skipped
 */
//检查执行驱逐是否安全。如果可以执行驱逐，则返回 1 如果应该跳过驱逐处理，则返回 0
static int isSafeToPerformEvictions(void) {
    /* - There must be no script in timeout condition.
     * - Nor we are loading data right now.  */
    //- 超时条件下必须没有脚本。
    // - 我们现在也没有加载数据。
    if (isInsideYieldingLongCommand() || server.loading) return 0;

    /* By default replicas should ignore maxmemory
     * and just be masters exact copies. */
    //默认情况下，副本应该忽略 maxmemory 并且只是主副本。
    if (server.masterhost && server.repl_slave_ignore_maxmemory) return 0;

    /* When clients are paused the dataset should be static not just from the
     * POV of clients not being able to write, but also from the POV of
     * expires and evictions of keys not being performed. */
    //当客户端暂停时，数据集应该是静态的，不仅来自客户端无法写入的 POV，而且来自过期和未执行键的驱逐的 POV。
    if (checkClientPauseTimeoutAndReturnIfPaused()) return 0;

    return 1;
}

/* Algorithm for converting tenacity (0-100) to a time limit.  */
//将韧性 (0-100) 转换为时间限制的算法。
static unsigned long evictionTimeLimitUs() {
    serverAssert(server.maxmemory_eviction_tenacity >= 0);
    serverAssert(server.maxmemory_eviction_tenacity <= 100);

    if (server.maxmemory_eviction_tenacity <= 10) {
        /* A linear progression from 0..500us  从 0..500us 的线性进展*/
        return 50uL * server.maxmemory_eviction_tenacity;
    }

    if (server.maxmemory_eviction_tenacity < 100) {
        /* A 15% geometric progression, resulting in a limit of ~2 min at tenacity==99  */
        //15% 的几何级数，导致在韧度 ==99 时的限制约为 2 分钟
        return (unsigned long)(500.0 * pow(1.15, server.maxmemory_eviction_tenacity - 10.0));
    }

    return ULONG_MAX;   /* No limit to eviction time 驱逐时间没有限制*/
}

/* Check that memory usage is within the current "maxmemory" limit.  If over
 * "maxmemory", attempt to free memory by evicting data (if it's safe to do so).
 *
 * It's possible for Redis to suddenly be significantly over the "maxmemory"
 * setting.  This can happen if there is a large allocation (like a hash table
 * resize) or even if the "maxmemory" setting is manually adjusted.  Because of
 * this, it's important to evict for a managed period of time - otherwise Redis
 * would become unresponsive while evicting.
 *
 * The goal of this function is to improve the memory situation - not to
 * immediately resolve it.  In the case that some items have been evicted but
 * the "maxmemory" limit has not been achieved, an aeTimeProc will be started
 * which will continue to evict items until memory limits are achieved or
 * nothing more is evictable.
 *
 * This should be called before execution of commands.  If EVICT_FAIL is
 * returned, commands which will result in increased memory usage should be
 * rejected.
 *
 * Returns:
 *   EVICT_OK       - memory is OK or it's not possible to perform evictions now
 *   EVICT_RUNNING  - memory is over the limit, but eviction is still processing
 *   EVICT_FAIL     - memory is over the limit, and there's nothing to evict
 * */
/**
 * 检查内存使用量是否在当前的“maxmemory”限制内。如果超过“maxmemory”，尝试通过逐出数据来释放内存（如果这样做是安全的）。
 * Redis 可能会突然显着超过“maxmemory”设置。如果分配量很大（如哈希表调整大小），或者即使手动调整了“maxmemory”设置，
 * 也会发生这种情况。因此，在一段可控的时间内驱逐很重要——否则 Redis 在驱逐时会变得无响应。
 * 此功能的目标是改善记忆状况 - 而不是立即解决它。在某些项目已被驱逐但尚未达到“最大内存”限制的情况下，
 * 将启动一个 aeTimeProc，它将继续驱逐项目，直到达到内存限制或没有其他可驱逐的内容。
 * 这应该在执行命令之前调用。如果返回 EVICT_FAIL，则应拒绝会导致内存使用量增加的命令。
 * 返回：
 *   EVICT_OK - 内存正常，或者现在无法执行驱逐
 *   EVICT_RUNNING - 内存超过限制，但驱逐仍在处理中
 *   EVICT_FAIL - 内存超出限制，没有什么可以驱逐的
 * */
int performEvictions(void) {
    /* Note, we don't goto update_metrics here because this check skips eviction
     * as if it wasn't triggered. it's a fake EVICT_OK. */
    //请注意，我们在这里没有转到 update_metrics ，因为此检查会跳过驱逐，就好像它没有被触发一样。这是一个假的 EVICT_OK。
    if (!isSafeToPerformEvictions()) return EVICT_OK;

    int keys_freed = 0;
    size_t mem_reported, mem_tofree;
    long long mem_freed; /* May be negative */
    mstime_t latency, eviction_latency;
    long long delta;
    int slaves = listLength(server.slaves);
    int result = EVICT_FAIL;

    if (getMaxmemoryState(&mem_reported,NULL,&mem_tofree,NULL) == C_OK) {
        result = EVICT_OK;
        goto update_metrics;
    }

    if (server.maxmemory_policy == MAXMEMORY_NO_EVICTION) {
        result = EVICT_FAIL;  /* We need to free memory, but policy forbids. 我们需要释放内存，但政策禁止。*/
        goto update_metrics;
    }

    unsigned long eviction_time_limit_us = evictionTimeLimitUs();

    mem_freed = 0;

    latencyStartMonitor(latency);

    monotime evictionTimer;
    elapsedStart(&evictionTimer);

    /* Unlike active-expire and blocked client, we can reach here from 'CONFIG SET maxmemory'
     * so we have to back-up and restore server.core_propagates. */
    //与 active-expire 和阻塞客户端不同，我们可以从 'CONFIG SET maxmemory' 到达这里，
    // 因此我们必须备份和恢复 server.core_propagates。
    int prev_core_propagates = server.core_propagates;
    serverAssert(server.also_propagate.numops == 0);
    server.core_propagates = 1;
    server.propagate_no_multi = 1;

    while (mem_freed < (long long)mem_tofree) {
        int j, k, i;
        static unsigned int next_db = 0;
        sds bestkey = NULL;
        int bestdbid;
        redisDb *db;
        dict *dict;
        dictEntry *de;

        if (server.maxmemory_policy & (MAXMEMORY_FLAG_LRU|MAXMEMORY_FLAG_LFU) ||
            server.maxmemory_policy == MAXMEMORY_VOLATILE_TTL)
        {
            struct evictionPoolEntry *pool = EvictionPoolLRU;

            while(bestkey == NULL) {
                unsigned long total_keys = 0, keys;

                /* We don't want to make local-db choices when expiring keys,
                 * so to start populate the eviction pool sampling keys from
                 * every DB. */
                //我们不想在过期键时做出本地数据库选择，因此开始从每个数据库填充驱逐池采样键。
                for (i = 0; i < server.dbnum; i++) {
                    db = server.db+i;
                    dict = (server.maxmemory_policy & MAXMEMORY_FLAG_ALLKEYS) ?
                            db->dict : db->expires;
                    if ((keys = dictSize(dict)) != 0) {
                        evictionPoolPopulate(i, dict, db->dict, pool);
                        total_keys += keys;
                    }
                }
                if (!total_keys) break; /* No keys to evict. */

                /* Go backward from best to worst element to evict. */
                //从最好到最差的元素倒退到驱逐。
                for (k = EVPOOL_SIZE-1; k >= 0; k--) {
                    if (pool[k].key == NULL) continue;
                    bestdbid = pool[k].dbid;

                    if (server.maxmemory_policy & MAXMEMORY_FLAG_ALLKEYS) {
                        de = dictFind(server.db[bestdbid].dict,
                            pool[k].key);
                    } else {
                        de = dictFind(server.db[bestdbid].expires,
                            pool[k].key);
                    }

                    /* Remove the entry from the pool. */
                    if (pool[k].key != pool[k].cached)
                        sdsfree(pool[k].key);
                    pool[k].key = NULL;
                    pool[k].idle = 0;

                    /* If the key exists, is our pick. Otherwise it is
                     * a ghost and we need to try the next element. */
                    //如果密钥存在，就是我们的选择。否则它是一个幽灵，我们需要尝试下一个元素。
                    if (de) {
                        bestkey = dictGetKey(de);
                        break;
                    } else {
                        /* Ghost... Iterate again. */
                    }
                }
            }
        }

        /* volatile-random and allkeys-random policy */
        //volatile-random 和 allkeys-random 策略
        else if (server.maxmemory_policy == MAXMEMORY_ALLKEYS_RANDOM ||
                 server.maxmemory_policy == MAXMEMORY_VOLATILE_RANDOM)
        {
            /* When evicting a random key, we try to evict a key for
             * each DB, so we use the static 'next_db' variable to
             * incrementally visit all DBs. */
            //当逐出一个随机键时，我们尝试逐出每个 DB 的一个键，因此我们使用静态“next_db”变量来增量访问所有 DB。
            for (i = 0; i < server.dbnum; i++) {
                j = (++next_db) % server.dbnum;
                db = server.db+j;
                dict = (server.maxmemory_policy == MAXMEMORY_ALLKEYS_RANDOM) ?
                        db->dict : db->expires;
                if (dictSize(dict) != 0) {
                    de = dictGetRandomKey(dict);
                    bestkey = dictGetKey(de);
                    bestdbid = j;
                    break;
                }
            }
        }

        /* Finally remove the selected key. */
        //最后删除选定的键。
        if (bestkey) {
            db = server.db+bestdbid;
            robj *keyobj = createStringObject(bestkey,sdslen(bestkey));
            /* We compute the amount of memory freed by db*Delete() alone.
             * It is possible that actually the memory needed to propagate
             * the DEL in AOF and replication link is greater than the one
             * we are freeing removing the key, but we can't account for
             * that otherwise we would never exit the loop.
             *
             * Same for CSC invalidation messages generated by signalModifiedKey.
             *
             * AOF and Output buffer memory will be freed eventually so
             * we only care about memory used by the key space. */
            /**我们单独计算 dbDelete() 释放的内存量。实际上，在 AOF 和复制链接中传播 DEL 所需的内存
             * 可能大于我们在删除密钥时释放的内存，但我们无法解释这一点，否则我们将永远不会退出循环。
             * 与 signalModifiedKey 生成的 CSC 失效消息相同。 AOF 和输出缓冲内存最终会被释放，
             * 所以我们只关心键空间使用的内存。*/
            delta = (long long) zmalloc_used_memory();
            latencyStartMonitor(eviction_latency);
            if (server.lazyfree_lazy_eviction)
                dbAsyncDelete(db,keyobj);
            else
                dbSyncDelete(db,keyobj);
            latencyEndMonitor(eviction_latency);
            latencyAddSampleIfNeeded("eviction-del",eviction_latency);
            delta -= (long long) zmalloc_used_memory();
            mem_freed += delta;
            server.stat_evictedkeys++;
            signalModifiedKey(NULL,db,keyobj);
            notifyKeyspaceEvent(NOTIFY_EVICTED, "evicted",
                keyobj, db->id);
            propagateDeletion(db,keyobj,server.lazyfree_lazy_eviction);
            decrRefCount(keyobj);
            keys_freed++;

            if (keys_freed % 16 == 0) {
                /* When the memory to free starts to be big enough, we may
                 * start spending so much time here that is impossible to
                 * deliver data to the replicas fast enough, so we force the
                 * transmission here inside the loop. */
                /**当要释放的内存开始足够大时，我们可能会开始在这里花费太多时间，
                 * 以至于无法足够快地将数据传递到副本，因此我们强制在循环内进行传输。*/
                if (slaves) flushSlavesOutputBuffers();

                /* Normally our stop condition is the ability to release
                 * a fixed, pre-computed amount of memory. However when we
                 * are deleting objects in another thread, it's better to
                 * check, from time to time, if we already reached our target
                 * memory, since the "mem_freed" amount is computed only
                 * across the dbAsyncDelete() call, while the thread can
                 * release the memory all the time. */
                /**通常我们的停止条件是释放固定的、预先计算的内存量的能力。
                 * 但是，当我们在另一个线程中删除对象时，最好不时检查我们是否已经到达目标内存，
                 * 因为“mem_freed”数量仅在 dbAsyncDelete() 调用中计算，而线程可以释放无时无刻不在记忆。*/
                if (server.lazyfree_lazy_eviction) {
                    if (getMaxmemoryState(NULL,NULL,NULL,NULL) == C_OK) {
                        break;
                    }
                }

                /* After some time, exit the loop early - even if memory limit
                 * hasn't been reached.  If we suddenly need to free a lot of
                 * memory, don't want to spend too much time here.  */
                //一段时间后，尽早退出循环 - 即使尚未达到内存限制。如果我们突然需要释放大量内存，不想在这里花费太多时间。
                if (elapsedUs(evictionTimer) > eviction_time_limit_us) {
                    // We still need to free memory - start eviction timer proc
                    //我们仍然需要释放内存 - 启动 eviction timer proc
                    startEvictionTimeProc();
                    break;
                }
            }
        } else {
            goto cant_free; /* nothing to free... */
        }
    }
    /* at this point, the memory is OK, or we have reached the time limit */
    //至此，内存OK，或者我们已经到了时间限制
    result = (isEvictionProcRunning) ? EVICT_RUNNING : EVICT_OK;

cant_free:
    if (result == EVICT_FAIL) {
        /* At this point, we have run out of evictable items.  It's possible
         * that some items are being freed in the lazyfree thread.  Perform a
         * short wait here if such jobs exist, but don't wait long.  */
        /**在这一点上，我们已经用完了可驱逐的项目。有可能某些项目正在lazyfree 线程中被释放。
         * 如果存在此类作业，请在此处执行短暂等待，但不要等待太久。*/
        if (bioPendingJobsOfType(BIO_LAZY_FREE)) {
            usleep(eviction_time_limit_us);
            if (getMaxmemoryState(NULL,NULL,NULL,NULL) == C_OK) {
                result = EVICT_OK;
            }
        }
    }

    serverAssert(server.core_propagates); /* This function should not be re-entrant 这个函数不应该是可重入的*/

    /* Propagate all DELs  传播所有 DEL*/
    propagatePendingCommands();

    server.core_propagates = prev_core_propagates;
    server.propagate_no_multi = 0;

    latencyEndMonitor(latency);
    latencyAddSampleIfNeeded("eviction-cycle",latency);

update_metrics:
    if (result == EVICT_RUNNING || result == EVICT_FAIL) {
        if (server.stat_last_eviction_exceeded_time == 0)
            elapsedStart(&server.stat_last_eviction_exceeded_time);
    } else if (result == EVICT_OK) {
        if (server.stat_last_eviction_exceeded_time != 0) {
            server.stat_total_eviction_exceeded_time += elapsedUs(server.stat_last_eviction_exceeded_time);
            server.stat_last_eviction_exceeded_time = 0;
        }
    }
    return result;
}
