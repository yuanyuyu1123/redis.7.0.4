/* Implementation of EXPIRE (keys with fixed time to live).
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

/*-----------------------------------------------------------------------------
 * Incremental collection of expired keys.
 *
 * When keys are accessed they are expired on-access. However we need a
 * mechanism in order to ensure keys are eventually removed when expired even
 * if no access is performed on them.
 *----------------------------------------------------------------------------*/

/* Helper function for the activeExpireCycle() function.
 * This function will try to expire the key that is stored in the hash table
 * entry 'de' of the 'expires' hash table of a Redis database.
 *
 * If the key is found to be expired, it is removed from the database and
 * 1 is returned. Otherwise no operation is performed and 0 is returned.
 *
 * When a key is expired, server.stat_expiredkeys is incremented.
 *
 * The parameter 'now' is the current time in milliseconds as is passed
 * to the function to avoid too many gettimeofday() syscalls. */
/**
 * -------------------------------------------------- ---------------------------------------
 * 过期密钥的增量收集。
 * 当密钥被访问时，它们在访问时过期。然而，我们需要一种机制来确保密钥在过期时最终被删除，即使没有对其执行访问。
 * -------------------------------------------------- --------------------------
 * activeExpireCycle() 函数的辅助函数。此函数将尝试使存储在 Redis 数据库的“过期”哈希表的哈希表条目“de”中的键过期。
 * 如果发现密钥过期，则将其从数据库中删除并返回 1。否则不执行任何操作并返回 0。
 * 当密钥过期时， server.stat_expiredkeys 会增加。
 * 参数 'now' 是以毫秒为单位的当前时间，因为它被传递给函数以避免过多的 gettimeofday() 系统调用。
 * */
int activeExpireCycleTryExpire(redisDb *db, dictEntry *de, long long now) {
    long long t = dictGetSignedIntegerVal(de);
    if (now > t) {
        sds key = dictGetKey(de);
        robj *keyobj = createStringObject(key,sdslen(key));
        deleteExpiredKeyAndPropagate(db,keyobj);
        decrRefCount(keyobj);
        return 1;
    } else {
        return 0;
    }
}

/* Try to expire a few timed out keys. The algorithm used is adaptive and
 * will use few CPU cycles if there are few expiring keys, otherwise
 * it will get more aggressive to avoid that too much memory is used by
 * keys that can be removed from the keyspace.
 *
 * Every expire cycle tests multiple databases: the next call will start
 * again from the next db. No more than CRON_DBS_PER_CALL databases are
 * tested at every iteration.
 *
 * The function can perform more or less work, depending on the "type"
 * argument. It can execute a "fast cycle" or a "slow cycle". The slow
 * cycle is the main way we collect expired cycles: this happens with
 * the "server.hz" frequency (usually 10 hertz).
 *
 * However the slow cycle can exit for timeout, since it used too much time.
 * For this reason the function is also invoked to perform a fast cycle
 * at every event loop cycle, in the beforeSleep() function. The fast cycle
 * will try to perform less work, but will do it much more often.
 *
 * The following are the details of the two expire cycles and their stop
 * conditions:
 *
 * If type is ACTIVE_EXPIRE_CYCLE_FAST the function will try to run a
 * "fast" expire cycle that takes no longer than ACTIVE_EXPIRE_CYCLE_FAST_DURATION
 * microseconds, and is not repeated again before the same amount of time.
 * The cycle will also refuse to run at all if the latest slow cycle did not
 * terminate because of a time limit condition.
 *
 * If type is ACTIVE_EXPIRE_CYCLE_SLOW, that normal expire cycle is
 * executed, where the time limit is a percentage of the REDIS_HZ period
 * as specified by the ACTIVE_EXPIRE_CYCLE_SLOW_TIME_PERC define. In the
 * fast cycle, the check of every database is interrupted once the number
 * of already expired keys in the database is estimated to be lower than
 * a given percentage, in order to avoid doing too much work to gain too
 * little memory.
 *
 * The configured expire "effort" will modify the baseline parameters in
 * order to do more work in both the fast and slow expire cycles.
 */
/**
 * 尝试使一些超时的密钥过期。
 * 所使用的算法是自适应的，如果过期密钥很少，它将使用很少的 CPU 周期，否则将变得更加积极，
 * 以避免可以从密钥空间中删除的密钥使用过多的内存。每个过期周期都会测试多个数据库：
 * 下一次调用将从下一个数据库重新开始。每次迭代测试不超过 CRON_DBS_PER_CALL 数据库。
 * 该函数可以执行或多或少的工作，具体取决于“类型”参数。它可以执行“快循环”或“慢循环”。
 * 慢周期是我们收集过期周期的主要方式：这发生在“server.hz”频率（通常为 10 赫兹）。
 * 然而，慢循环可以退出超时，因为它使用了太多时间。
 * 因此，在 beforeSleep() 函数中，还调用该函数以在每个事件循环周期执行快速循环。
 * 快速循环将尝试执行更少的工作，但会更频繁地执行此操作。以下是两个过期周期及其停止条件的详细信息：
 *   如果类型为 ACTIVE_EXPIRE_CYCLE_FAST，该函数将尝试运行一个“快速”过期周期，
 *     该周期不超过 ACTIVE_EXPIRE_CYCLE_FAST_DURATION 微秒，并且在相同的时间量之前不会再次重复.
 *     如果最近的慢循环由于时间限制条件没有终止，则循环也将完全拒绝运行。
 *   如果 type 是 ACTIVE_EXPIRE_CYCLE_SLOW，则执行正常的过期周期，
 *     其中时间限制是 ACTIVE_EXPIRE_CYCLE_SLOW_TIME_PERC 定义指定的 REDIS_HZ 周期的百分比。
 *     在快速循环中，一旦估计数据库中已经过期的键的数量低于给定的百分比，就会中断对每个数据库的检查，
 *     以避免做太多工作而获得太少的内存。
 * 配置的过期“努力”将修改基线参数，以便在快速和慢速过期周期中做更多的工作。
 * */

#define ACTIVE_EXPIRE_CYCLE_KEYS_PER_LOOP 20 /* Keys for each DB loop. 每个 DB 循环的键。*/
#define ACTIVE_EXPIRE_CYCLE_FAST_DURATION 1000 /* Microseconds. */
#define ACTIVE_EXPIRE_CYCLE_SLOW_TIME_PERC 25 /* Max % of CPU to use. */
#define ACTIVE_EXPIRE_CYCLE_ACCEPTABLE_STALE 10 /* % of stale keys after which
                                                   we do extra efforts. 过时密钥的百分比，之后我们会做额外的努力。*/

void activeExpireCycle(int type) {
    /* Adjust the running parameters according to the configured expire
     * effort. The default effort is 1, and the maximum configurable effort
     * is 10. */
    //根据配置的过期努力调整运行参数。默认工作量为 1，最大可配置工作量为 10。
    unsigned long
    effort = server.active_expire_effort-1, /* Rescale from 0 to 9. 从 0 重新调整到 9。*/
    config_keys_per_loop = ACTIVE_EXPIRE_CYCLE_KEYS_PER_LOOP +
                           ACTIVE_EXPIRE_CYCLE_KEYS_PER_LOOP/4*effort,
    config_cycle_fast_duration = ACTIVE_EXPIRE_CYCLE_FAST_DURATION +
                                 ACTIVE_EXPIRE_CYCLE_FAST_DURATION/4*effort,
    config_cycle_slow_time_perc = ACTIVE_EXPIRE_CYCLE_SLOW_TIME_PERC +
                                  2*effort,
    config_cycle_acceptable_stale = ACTIVE_EXPIRE_CYCLE_ACCEPTABLE_STALE-
                                    effort;

    /* This function has some global state in order to continue the work
     * incrementally across calls. */
    //此函数具有一些全局状态，以便在调用之间以增量方式继续工作。
    static unsigned int current_db = 0; /* Next DB to test. 下一个要测试的数据库。*/
    static int timelimit_exit = 0;      /* Time limit hit in previous call?  上次通话中的时间限制？*/
    static long long last_fast_cycle = 0; /* When last fast cycle ran. 上次快速循环运行的时间。*/

    int j, iteration = 0;
    int dbs_per_call = CRON_DBS_PER_CALL;
    long long start = ustime(), timelimit, elapsed;

    /* When clients are paused the dataset should be static not just from the
     * POV of clients not being able to write, but also from the POV of
     * expires and evictions of keys not being performed. */
    //当客户端暂停时，数据集应该是静态的，不仅来自客户端无法写入的 POV，而且来自过期和未执行键的驱逐的 POV。
    if (checkClientPauseTimeoutAndReturnIfPaused()) return;

    if (type == ACTIVE_EXPIRE_CYCLE_FAST) {
        /* Don't start a fast cycle if the previous cycle did not exit
         * for time limit, unless the percentage of estimated stale keys is
         * too high. Also never repeat a fast cycle for the same period
         * as the fast cycle total duration itself. */
        /** 如果前一个循环没有退出时间限制，则不要启动快速循环，除非估计的陈旧密钥的百分比太高。
         * 也不要在与快速循环总持续时间本身相同的周期内重复快速循环。*/
        if (!timelimit_exit &&
            server.stat_expired_stale_perc < config_cycle_acceptable_stale)
            return;

        if (start < last_fast_cycle + (long long)config_cycle_fast_duration*2)
            return;

        last_fast_cycle = start;
    }

    /* We usually should test CRON_DBS_PER_CALL per iteration, with
     * two exceptions:
     *
     * 1) Don't test more DBs than we have.
     * 2) If last time we hit the time limit, we want to scan all DBs
     * in this iteration, as there is work to do in some DB and we don't want
     * expired keys to use memory for too much time. */
    /**
     * 我们通常应该在每次迭代中测试 CRON_DBS_PER_CALL，但有两个例外：
     *   1）不要测试比我们拥有的更多的 DB。
     *   2）如果上次我们达到了时间限制，我们想在这个迭代中扫描所有的数据库，
     *     因为在一些数据库中有工作要做，我们不希望过期的键使用内存太长时间。
     * */
    if (dbs_per_call > server.dbnum || timelimit_exit)
        dbs_per_call = server.dbnum;

    /* We can use at max 'config_cycle_slow_time_perc' percentage of CPU
     * time per iteration. Since this function gets called with a frequency of
     * server.hz times per second, the following is the max amount of
     * microseconds we can spend in this function. */
    /**我们可以使用每次迭代 CPU 时间的最大“config_cycle_slow_time_perc”百分比。
     * 由于此函数以每秒 server.hz 次的频率调用，因此以下是我们可以在此函数中花费的最大微秒数。*/
    timelimit = config_cycle_slow_time_perc*1000000/server.hz/100;
    timelimit_exit = 0;
    if (timelimit <= 0) timelimit = 1;

    if (type == ACTIVE_EXPIRE_CYCLE_FAST)
        timelimit = config_cycle_fast_duration; /* in microseconds. */

    /* Accumulate some global stats as we expire keys, to have some idea
     * about the number of keys that are already logically expired, but still
     * existing inside the database. */
    //在我们过期键时累积一些全局统计信息，以了解逻辑上已经过期但仍存在于数据库中的键的数量。
    long total_sampled = 0;
    long total_expired = 0;

    /* Sanity: There can't be any pending commands to propagate when
     * we're in cron */
    //理智：当我们在 cron 中时，不能有任何挂起的命令可以传播
    serverAssert(server.also_propagate.numops == 0);
    server.core_propagates = 1;
    server.propagate_no_multi = 1;

    for (j = 0; j < dbs_per_call && timelimit_exit == 0; j++) {
        /* Expired and checked in a single loop. */
        //过期并在一个循环中检查。
        unsigned long expired, sampled;

        redisDb *db = server.db+(current_db % server.dbnum);

        /* Increment the DB now so we are sure if we run out of time
         * in the current DB we'll restart from the next. This allows to
         * distribute the time evenly across DBs. */
        //现在增加数据库，以便我们确定如果我们在当前数据库中用完时间，我们将从下一个重新启动。这允许在 DB 之间平均分配时间。
        current_db++;

        /* Continue to expire if at the end of the cycle there are still
         * a big percentage of keys to expire, compared to the number of keys
         * we scanned. The percentage, stored in config_cycle_acceptable_stale
         * is not fixed, but depends on the Redis configured "expire effort". */
        /**如果与我们扫描的密钥数量相比，在循环结束时仍有很大比例的密钥过期，则继续过期。
         * 存储在 config_cycle_acceptable_stale 中的百分比不是固定的，而是取决于 Redis 配置的“过期工作量”。*/
        do {
            unsigned long num, slots;
            long long now, ttl_sum;
            int ttl_samples;
            iteration++;

            /* If there is nothing to expire try next DB ASAP. */
            //如果没有什么可以过期，请尽快尝试下一个数据库。
            if ((num = dictSize(db->expires)) == 0) {
                db->avg_ttl = 0;
                break;
            }
            slots = dictSlots(db->expires);
            now = mstime();

            /* When there are less than 1% filled slots, sampling the key
             * space is expensive, so stop here waiting for better times...
             * The dictionary will be resized asap. */
            //当填充的插槽少于 1% 时，对键空间进行采样是昂贵的，因此请在此停止等待更好的时机......字典将尽快调整大小。
            if (slots > DICT_HT_INITIAL_SIZE &&
                (num*100/slots < 1)) break;

            /* The main collection cycle. Sample random keys among keys
             * with an expire set, checking for expired ones. */
            //主要收集周期。在具有过期集的密钥中采样随机密钥，检查过期密钥。
            expired = 0;
            sampled = 0;
            ttl_sum = 0;
            ttl_samples = 0;

            if (num > config_keys_per_loop)
                num = config_keys_per_loop;

            /* Here we access the low level representation of the hash table
             * for speed concerns: this makes this code coupled with dict.c,
             * but it hardly changed in ten years.
             *
             * Note that certain places of the hash table may be empty,
             * so we want also a stop condition about the number of
             * buckets that we scanned. However scanning for free buckets
             * is very fast: we are in the cache line scanning a sequential
             * array of NULL pointers, so we can scan a lot more buckets
             * than keys in the same time. */
            /**在这里，出于速度考虑，我们访问哈希表的低级表示：这使得这段代码与 dict.c 耦合，但十年来几乎没有改变。
             * 请注意，哈希表的某些位置可能是空的，因此我们还需要一个关于我们扫描的桶数的停止条件。
             * 然而，扫描空闲桶非常快：我们在缓存行中扫描空指针的顺序数组，因此我们可以同时扫描比键多得多的桶。*/
            long max_buckets = num*20;
            long checked_buckets = 0;

            while (sampled < num && checked_buckets < max_buckets) {
                for (int table = 0; table < 2; table++) {
                    if (table == 1 && !dictIsRehashing(db->expires)) break;

                    unsigned long idx = db->expires_cursor;
                    idx &= DICTHT_SIZE_MASK(db->expires->ht_size_exp[table]);
                    dictEntry *de = db->expires->ht_table[table][idx];
                    long long ttl;

                    /* Scan the current bucket of the current table. */
                    //扫描当前表的当前桶。
                    checked_buckets++;
                    while(de) {
                        /* Get the next entry now since this entry may get
                         * deleted. */
                        //立即获取下一个条目，因为此条目可能会被删除。
                        dictEntry *e = de;
                        de = de->next;

                        ttl = dictGetSignedIntegerVal(e)-now;
                        if (activeExpireCycleTryExpire(db,e,now)) expired++;
                        if (ttl > 0) {
                            /* We want the average TTL of keys yet
                             * not expired. */
                            //我们想要尚未过期的密钥的平均 TTL。
                            ttl_sum += ttl;
                            ttl_samples++;
                        }
                        sampled++;
                    }
                }
                db->expires_cursor++;
            }
            total_expired += expired;
            total_sampled += sampled;

            /* Update the average TTL stats for this database. */
            //更新此数据库的平均 TTL 统计信息。
            if (ttl_samples) {
                long long avg_ttl = ttl_sum/ttl_samples;

                /* Do a simple running average with a few samples.
                 * We just use the current estimate with a weight of 2%
                 * and the previous estimate with a weight of 98%. */
                //用几个样本做一个简单的运行平均。我们只使用权重为 2% 的当前估计值和权重为 98% 的先前估计值。
                if (db->avg_ttl == 0) db->avg_ttl = avg_ttl;
                db->avg_ttl = (db->avg_ttl/50)*49 + (avg_ttl/50);
            }

            /* We can't block forever here even if there are many keys to
             * expire. So after a given amount of milliseconds return to the
             * caller waiting for the other active expire cycle. */
            //即使有很多密钥要过期，我们也不能在这里永远阻塞。所以在给定的毫秒数之后返回调用者等待另一个活跃的过期周期。
            if ((iteration & 0xf) == 0) { /* check once every 16 iterations. 每 16 次迭代检查一次。*/
                elapsed = ustime()-start;
                if (elapsed > timelimit) {
                    timelimit_exit = 1;
                    server.stat_expired_time_cap_reached_count++;
                    break;
                }
            }
            /* We don't repeat the cycle for the current database if there are
             * an acceptable amount of stale keys (logically expired but yet
             * not reclaimed). */
            //如果有可接受数量的陈旧密钥（逻辑上过期但尚未回收），我们不会为当前数据库重复循环。
        } while (sampled == 0 ||
                 (expired*100/sampled) > config_cycle_acceptable_stale);
    }

    serverAssert(server.core_propagates); /* This function should not be re-entrant 这个函数不应该是可重入的*/

    /* Propagate all DELs */
    propagatePendingCommands();

    server.core_propagates = 0;
    server.propagate_no_multi = 0;

    elapsed = ustime()-start;
    server.stat_expire_cycle_time_used += elapsed;
    latencyAddSampleIfNeeded("expire-cycle",elapsed/1000);

    /* Update our estimate of keys existing but yet to be expired.
     * Running average with this sample accounting for 5%. */
    //更新我们对现有但尚未过期的密钥的估计。此样本的运行平均值占 5%。
    double current_perc;
    if (total_sampled) {
        current_perc = (double)total_expired/total_sampled;
    } else
        current_perc = 0;
    server.stat_expired_stale_perc = (current_perc*0.05)+
                                     (server.stat_expired_stale_perc*0.95);
}

/*-----------------------------------------------------------------------------
 * Expires of keys created in writable slaves
 *
 * Normally slaves do not process expires: they wait the masters to synthesize
 * DEL operations in order to retain consistency. However writable slaves are
 * an exception: if a key is created in the slave and an expire is assigned
 * to it, we need a way to expire such a key, since the master does not know
 * anything about such a key.
 *
 * In order to do so, we track keys created in the slave side with an expire
 * set, and call the expireSlaveKeys() function from time to time in order to
 * reclaim the keys if they already expired.
 *
 * Note that the use case we are trying to cover here, is a popular one where
 * slaves are put in writable mode in order to compute slow operations in
 * the slave side that are mostly useful to actually read data in a more
 * processed way. Think at sets intersections in a tmp key, with an expire so
 * that it is also used as a cache to avoid intersecting every time.
 *
 * This implementation is currently not perfect but a lot better than leaking
 * the keys as implemented in 3.2.
 * 在可写从属中创建的密钥过期
 * 通常从属不会处理过期：它们等待主控合成 DEL 操作以保持一致性。但是可写从属是一个例外：
 * 如果在从属中创建了一个密钥并为其分配了过期时间，我们需要一种方法来使这样的密钥过期，因为主控对这样的密钥一无所知。
 * 为了做到这一点，我们跟踪在从端创建的带有过期集的密钥，并不时调用 expireSlaveKeys() 函数，以便在它们已经过期时回收密钥。
 * 请注意，我们在此处尝试介绍的用例是一种流行的用例，其中从站置于可写模式以计算从站端的慢速操作，
 * 这些操作对于以更处理的方式实际读取数据非常有用。考虑在 tmp 键中设置交集，并带有过期时间，以便它也用作缓存以避免每次相交。
 * 这种实现目前并不完美，但比 3.2 中实现的泄露密钥要好得多。
 *----------------------------------------------------------------------------*/

/* The dictionary where we remember key names and database ID of keys we may
 * want to expire from the slave. Since this function is not often used we
 * don't even care to initialize the database at startup. We'll do it once
 * the feature is used the first time, that is, when rememberSlaveKeyWithExpire()
 * is called.
 *
 * The dictionary has an SDS string representing the key as the hash table
 * key, while the value is a 64 bit unsigned integer with the bits corresponding
 * to the DB where the keys may exist set to 1. Currently the keys created
 * with a DB id > 63 are not expired, but a trivial fix is to set the bitmap
 * to the max 64 bit unsigned value when we know there is a key with a DB
 * ID greater than 63, and check all the configured DBs in such a case. */
/**我们记住键名和数据库 ID 的字典，我们可能希望从从站过期。由于这个函数不经常使用，我们甚至不需要在启动时初始化数据库。
 * 我们将在第一次使用该功能时执行此操作，即在调用 rememberSlaveKeyWithExpire() 时。
 * 字典有一个 SDS 字符串，表示作为哈希表键的键，而值是一个 64 位无符号整数，对应于可能存在键的 DB 的位设置为 1。
 * 目前使用 DB id > 63 创建的键没有过期，但一个简单的解决方法是，当我们知道有一个 DB ID 大于 63 的键时，
 * 将位图设置为最大 64 位无符号值，并在这种情况下检查所有配置的 DB。*/
dict *slaveKeysWithExpire = NULL;

/* Check the set of keys created by the master with an expire set in order to
 * check if they should be evicted. */
//检查 master 创建的带有过期集的密钥集，以检查它们是否应该被驱逐。
void expireSlaveKeys(void) {
    if (slaveKeysWithExpire == NULL ||
        dictSize(slaveKeysWithExpire) == 0) return;

    int cycles = 0, noexpire = 0;
    mstime_t start = mstime();
    while(1) {
        dictEntry *de = dictGetRandomKey(slaveKeysWithExpire);
        sds keyname = dictGetKey(de);
        uint64_t dbids = dictGetUnsignedIntegerVal(de);
        uint64_t new_dbids = 0;

        /* Check the key against every database corresponding to the
         * bits set in the value bitmap. */
        //检查与值位图中设置的位相对应的每个数据库的键。
        int dbid = 0;
        while(dbids && dbid < server.dbnum) {
            if ((dbids & 1) != 0) {
                redisDb *db = server.db+dbid;
                dictEntry *expire = dictFind(db->expires,keyname);
                int expired = 0;

                if (expire &&
                    activeExpireCycleTryExpire(server.db+dbid,expire,start))
                {
                    expired = 1;
                }

                /* If the key was not expired in this DB, we need to set the
                 * corresponding bit in the new bitmap we set as value.
                 * At the end of the loop if the bitmap is zero, it means we
                 * no longer need to keep track of this key. */
                /**如果这个 DB 中的 key 没有过期，我们需要在我们设置为 value 的新位图中设置相应的位。
                 * 在循环结束时，如果位图为零，这意味着我们不再需要跟踪这个键。*/
                if (expire && !expired) {
                    noexpire++;
                    new_dbids |= (uint64_t)1 << dbid;
                }
            }
            dbid++;
            dbids >>= 1;
        }

        /* Set the new bitmap as value of the key, in the dictionary
         * of keys with an expire set directly in the writable slave. Otherwise
         * if the bitmap is zero, we no longer need to keep track of it. */
        //将新位图设置为键的值，在键的字典中直接在可写从属中设置过期。否则，如果位图为零，我们不再需要跟踪它。
        if (new_dbids)
            dictSetUnsignedIntegerVal(de,new_dbids);
        else
            dictDelete(slaveKeysWithExpire,keyname);

        /* Stop conditions: found 3 keys we can't expire in a row or
         * time limit was reached. */
        //停止条件：找到 3 个我们不能连续过期的密钥或达到时间限制。
        cycles++;
        if (noexpire > 3) break;
        if ((cycles % 64) == 0 && mstime()-start > 1) break;
        if (dictSize(slaveKeysWithExpire) == 0) break;
    }
}

/* Track keys that received an EXPIRE or similar command in the context
 * of a writable slave. */
//跟踪在可写从属上下文中接收到 EXPIRE 或类似命令的键。
void rememberSlaveKeyWithExpire(redisDb *db, robj *key) {
    if (slaveKeysWithExpire == NULL) {
        static dictType dt = {
            dictSdsHash,                /* hash function */
            NULL,                       /* key dup */
            NULL,                       /* val dup */
            dictSdsKeyCompare,          /* key compare */
            dictSdsDestructor,          /* key destructor */
            NULL,                       /* val destructor */
            NULL                        /* allow to expand */
        };
        slaveKeysWithExpire = dictCreate(&dt);
    }
    if (db->id > 63) return;

    dictEntry *de = dictAddOrFind(slaveKeysWithExpire,key->ptr);
    /* If the entry was just created, set it to a copy of the SDS string
     * representing the key: we don't want to need to take those keys
     * in sync with the main DB. The keys will be removed by expireSlaveKeys()
     * as it scans to find keys to remove. */
    /**如果条目刚刚创建，请将其设置为代表密钥的 SDS 字符串的副本：我们不希望将这些密钥与主数据库同步。
     * expireSlaveKeys() 将在扫描以查找要删除的密钥时删除密钥。*/
    if (de->key == key->ptr) {
        de->key = sdsdup(key->ptr);
        dictSetUnsignedIntegerVal(de,0);
    }

    uint64_t dbids = dictGetUnsignedIntegerVal(de);
    dbids |= (uint64_t)1 << db->id;
    dictSetUnsignedIntegerVal(de,dbids);
}

/* Return the number of keys we are tracking. */
//返回我们正在跟踪的键的数量。
size_t getSlaveKeyWithExpireCount(void) {
    if (slaveKeysWithExpire == NULL) return 0;
    return dictSize(slaveKeysWithExpire);
}

/* Remove the keys in the hash table. We need to do that when data is
 * flushed from the server. We may receive new keys from the master with
 * the same name/db and it is no longer a good idea to expire them.
 *
 * Note: technically we should handle the case of a single DB being flushed
 * but it is not worth it since anyway race conditions using the same set
 * of key names in a writable slave and in its master will lead to
 * inconsistencies. This is just a best-effort thing we do. */
/**删除哈希表中的键。当数据从服务器刷新时，我们需要这样做。
 * 我们可能会从 master 收到具有相同 namedb 的新密钥，让它们过期不再是一个好主意。
 * 注意：从技术上讲，我们应该处理单个数据库被刷新的情况，但这不值得，
 * 因为无论如何在可写从属和其主控中使用相同的键名集的竞争条件会导致不一致。这只是我们尽力而为的事情。*/
void flushSlaveKeysWithExpireList(void) {
    if (slaveKeysWithExpire) {
        dictRelease(slaveKeysWithExpire);
        slaveKeysWithExpire = NULL;
    }
}

int checkAlreadyExpired(long long when) {
    /* EXPIRE with negative TTL, or EXPIREAT with a timestamp into the past
     * should never be executed as a DEL when load the AOF or in the context
     * of a slave instance.
     *
     * Instead we add the already expired key to the database with expire time
     * (possibly in the past) and wait for an explicit DEL from the master. */
    /**在加载 AOF 或在从属实例的上下文中时，不应将带有负 TTL 的 EXPIRE 或带有过去时间戳的 EXPIREAT 作为 DEL 执行。
     * 相反，我们将已经过期的密钥添加到具有过期时间（可能是过去）的数据库中，并等待来自主服务器的显式 DEL。*/
    return (when <= mstime() && !server.loading && !server.masterhost);
}

#define EXPIRE_NX (1<<0)
#define EXPIRE_XX (1<<1)
#define EXPIRE_GT (1<<2)
#define EXPIRE_LT (1<<3)

/* Parse additional flags of expire commands
 *
 * Supported flags:
 * - NX: set expiry only when the key has no expiry
 * - XX: set expiry only when the key has an existing expiry
 * - GT: set expiry only when the new expiry is greater than current one
 * - LT: set expiry only when the new expiry is less than current one */
/**
 * 解析过期命令的附加标志 支持的标志：
 *   - NX：仅在密钥没有过期时设置过期
 *   - XX：仅在密钥已有过期时设置过期
 *   - GT：仅在新过期大于当前过期时设置过期
 *   - LT：仅在新到期时间小于当前到期时间时设置到期时间
 * */
int parseExtendedExpireArgumentsOrReply(client *c, int *flags) {
    int nx = 0, xx = 0, gt = 0, lt = 0;

    int j = 3;
    while (j < c->argc) {
        char *opt = c->argv[j]->ptr;
        if (!strcasecmp(opt,"nx")) {
            *flags |= EXPIRE_NX;
            nx = 1;
        } else if (!strcasecmp(opt,"xx")) {
            *flags |= EXPIRE_XX;
            xx = 1;
        } else if (!strcasecmp(opt,"gt")) {
            *flags |= EXPIRE_GT;
            gt = 1;
        } else if (!strcasecmp(opt,"lt")) {
            *flags |= EXPIRE_LT;
            lt = 1;
        } else {
            addReplyErrorFormat(c, "Unsupported option %s", opt);
            return C_ERR;
        }
        j++;
    }

    if ((nx && xx) || (nx && gt) || (nx && lt)) {
        addReplyError(c, "NX and XX, GT or LT options at the same time are not compatible");
        return C_ERR;
    }

    if (gt && lt) {
        addReplyError(c, "GT and LT options at the same time are not compatible");
        return C_ERR;
    }

    return C_OK;
}

/*-----------------------------------------------------------------------------
 * Expires Commands
 *----------------------------------------------------------------------------*/

/* This is the generic command implementation for EXPIRE, PEXPIRE, EXPIREAT
 * and PEXPIREAT. Because the command second argument may be relative or absolute
 * the "basetime" argument is used to signal what the base time is (either 0
 * for *AT variants of the command, or the current time for relative expires).
 *
 * unit is either UNIT_SECONDS or UNIT_MILLISECONDS, and is only used for
 * the argv[2] parameter. The basetime is always specified in milliseconds.
 *
 * Additional flags are supported and parsed via parseExtendedExpireArguments */
/** 这是 EXPIRE、PEXPIRE、EXPIREAT 和 PEXPIREAT 的通用命令实现。
 * 因为命令的第二个参数可能是相对的或绝对的，所以“basetime”参数用于指示基准时间是什么
 * （命令的 AT 变体为 0，或者相对到期的当前时间）。
 * unit 是 UNIT_SECONDS 或 UNIT_MILLISECONDS，仅用于 argv[2] 参数。基准时间始终以毫秒为单位。
 * 通过 parseExtendedExpireArguments 支持和解析其他标志*/
void expireGenericCommand(client *c, long long basetime, int unit) {
    robj *key = c->argv[1], *param = c->argv[2];
    long long when; /* unix time in milliseconds when the key will expire. 密钥到期时的 unix 时间（以毫秒为单位）。*/
    long long current_expire = -1;
    int flag = 0;

    /* checking optional flags 检查可选标志*/
    if (parseExtendedExpireArgumentsOrReply(c, &flag) != C_OK) {
        return;
    }

    if (getLongLongFromObjectOrReply(c, param, &when, NULL) != C_OK)
        return;

    /* EXPIRE allows negative numbers, but we can at least detect an
     * overflow by either unit conversion or basetime addition. */
    //EXPIRE 允许负数，但我们至少可以通过单位转换或基本时间加法来检测溢出。
    if (unit == UNIT_SECONDS) {
        if (when > LLONG_MAX / 1000 || when < LLONG_MIN / 1000) {
            addReplyErrorExpireTime(c);
            return;
        }
        when *= 1000;
    }

    if (when > LLONG_MAX - basetime) {
        addReplyErrorExpireTime(c);
        return;
    }
    when += basetime;

    /* No key, return zero. */
    if (lookupKeyWrite(c->db,key) == NULL) {
        addReply(c,shared.czero);
        return;
    }

    if (flag) {
        current_expire = getExpire(c->db, key);

        /* NX option is set, check current expiry */
        if (flag & EXPIRE_NX) {
            if (current_expire != -1) {
                addReply(c,shared.czero);
                return;
            }
        }

        /* XX option is set, check current expiry */
        if (flag & EXPIRE_XX) {
            if (current_expire == -1) {
                /* reply 0 when the key has no expiry */
                addReply(c,shared.czero);
                return;
            }
        }

        /* GT option is set, check current expiry */
        if (flag & EXPIRE_GT) {
            /* When current_expire is -1, we consider it as infinite TTL,
             * so expire command with gt always fail the GT. */
            //当 current_expire 为 -1 时，我们认为它是无限的 TTL，因此带有 gt 的 expire 命令总是使 GT 失败。
            if (when <= current_expire || current_expire == -1) {
                /* reply 0 when the new expiry is not greater than current */
                //当新的到期时间不大于当前时回复 0
                addReply(c,shared.czero);
                return;
            }
        }

        /* LT option is set, check current expiry */
        if (flag & EXPIRE_LT) {
            /* When current_expire -1, we consider it as infinite TTL,
             * but 'when' can still be negative at this point, so if there is
             * an expiry on the key and it's not less than current, we fail the LT. */
            //当 current_expire -1 时，我们认为它是无限的 TTL，但此时 'when' 仍然可以是负数，
            // 所以如果密钥过期并且不小于当前，我们会失败 LT。
            if (current_expire != -1 && when >= current_expire) {
                /* reply 0 when the new expiry is not less than current */
                //当新到期不小于当前时回复 0
                addReply(c,shared.czero);
                return;
            }
        }
    }

    if (checkAlreadyExpired(when)) {
        robj *aux;

        int deleted = server.lazyfree_lazy_expire ? dbAsyncDelete(c->db,key) :
                                                    dbSyncDelete(c->db,key);
        serverAssertWithInfo(c,key,deleted);
        server.dirty++;

        /* Replicate/AOF this as an explicit DEL or UNLINK. */
        //将此 AOF 复制为显式 DEL 或 UNLINK。
        aux = server.lazyfree_lazy_expire ? shared.unlink : shared.del;
        rewriteClientCommandVector(c,2,aux,key);
        signalModifiedKey(c,c->db,key);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",key,c->db->id);
        addReply(c, shared.cone);
        return;
    } else {
        setExpire(c,c->db,key,when);
        addReply(c,shared.cone);
        /* Propagate as PEXPIREAT millisecond-timestamp */
        //传播为 PEXPIREAT 毫秒时间戳
        robj *when_obj = createStringObjectFromLongLong(when);
        rewriteClientCommandVector(c, 3, shared.pexpireat, key, when_obj);
        decrRefCount(when_obj);
        signalModifiedKey(c,c->db,key);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"expire",key,c->db->id);
        server.dirty++;
        return;
    }
}

/* EXPIRE key seconds [ NX | XX | GT | LT] */
void expireCommand(client *c) {
    expireGenericCommand(c,mstime(),UNIT_SECONDS);
}

/* EXPIREAT key unix-time-seconds [ NX | XX | GT | LT] */
void expireatCommand(client *c) {
    expireGenericCommand(c,0,UNIT_SECONDS);
}

/* PEXPIRE key milliseconds [ NX | XX | GT | LT] */
void pexpireCommand(client *c) {
    expireGenericCommand(c,mstime(),UNIT_MILLISECONDS);
}

/* PEXPIREAT key unix-time-milliseconds [ NX | XX | GT | LT] */
void pexpireatCommand(client *c) {
    expireGenericCommand(c,0,UNIT_MILLISECONDS);
}

/* Implements TTL, PTTL, EXPIRETIME and PEXPIRETIME */
void ttlGenericCommand(client *c, int output_ms, int output_abs) {
    long long expire, ttl = -1;

    /* If the key does not exist at all, return -2 */
    //如果密钥根本不存在，则返回 -2
    if (lookupKeyReadWithFlags(c->db,c->argv[1],LOOKUP_NOTOUCH) == NULL) {
        addReplyLongLong(c,-2);
        return;
    }

    /* The key exists. Return -1 if it has no expire, or the actual
     * TTL value otherwise. */
    //密钥存在。如果没有过期，则返回 -1，否则返回实际 TTL 值。
    expire = getExpire(c->db,c->argv[1]);
    if (expire != -1) {
        ttl = output_abs ? expire : expire-mstime();
        if (ttl < 0) ttl = 0;
    }
    if (ttl == -1) {
        addReplyLongLong(c,-1);
    } else {
        addReplyLongLong(c,output_ms ? ttl : ((ttl+500)/1000));
    }
}

/* TTL key */
void ttlCommand(client *c) {
    ttlGenericCommand(c, 0, 0);
}

/* PTTL key */
void pttlCommand(client *c) {
    ttlGenericCommand(c, 1, 0);
}

/* EXPIRETIME key */
void expiretimeCommand(client *c) {
    ttlGenericCommand(c, 0, 1);
}

/* PEXPIRETIME key */
void pexpiretimeCommand(client *c) {
    ttlGenericCommand(c, 1, 1);
}

/* PERSIST key */
void persistCommand(client *c) {
    if (lookupKeyWrite(c->db,c->argv[1])) {
        if (removeExpire(c->db,c->argv[1])) {
            signalModifiedKey(c,c->db,c->argv[1]);
            notifyKeyspaceEvent(NOTIFY_GENERIC,"persist",c->argv[1],c->db->id);
            addReply(c,shared.cone);
            server.dirty++;
        } else {
            addReply(c,shared.czero);
        }
    } else {
        addReply(c,shared.czero);
    }
}

/* TOUCH key1 [key2 key3 ... keyN] */
void touchCommand(client *c) {
    int touched = 0;
    for (int j = 1; j < c->argc; j++)
        if (lookupKeyRead(c->db,c->argv[j]) != NULL) touched++;
    addReplyLongLong(c,touched);
}
