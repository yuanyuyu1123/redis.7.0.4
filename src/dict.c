/* Hash Tables Implementation.
 *
 * This file implements in memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto resize if needed
 * tables of power of two in size are used, collisions are handled by
 * chaining. See the source code for more information... :)
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <sys/time.h>

#include "dict.h"
#include "zmalloc.h"
#include "redisassert.h"

/* Using dictEnableResize() / dictDisableResize() we make possible to
 * enable/disable resizing of the hash table as needed. This is very important
 * for Redis, as we use copy-on-write and don't want to move too much memory
 * around when there is a child performing saving operations.
 *
 * Note that even when dict_can_resize is set to 0, not all resizes are
 * prevented: a hash table is still allowed to grow if the ratio between
 * the number of elements and the buckets > dict_force_resize_ratio. */
/**
 * 使用 dictEnableResize() dictDisableResize() 我们可以根据需要启用哈希表的可调整大小。
 * 这对 Redis 来说非常重要，因为我们使用写时复制，并且不想在有子执行保存操作时移动太多内存。
 * 请注意，即使将 dict_can_resize 设置为 0，也不会阻止所有调整大小：
 * 如果元素数量与存储桶之间的比率 > dict_force_resize_ratio，则仍然允许哈希表增长。*/
static int dict_can_resize = 1;
static unsigned int dict_force_resize_ratio = 5;

/* -------------------------- private prototypes ---------------------------- */

static int _dictExpandIfNeeded(dict *d);
static signed char _dictNextExp(unsigned long size);
static long _dictKeyIndex(dict *d, const void *key, uint64_t hash, dictEntry **existing);
static int _dictInit(dict *d, dictType *type);

/* -------------------------- hash functions -------------------------------- */

static uint8_t dict_hash_function_seed[16];

void dictSetHashFunctionSeed(uint8_t *seed) {
    memcpy(dict_hash_function_seed,seed,sizeof(dict_hash_function_seed));
}

uint8_t *dictGetHashFunctionSeed(void) {
    return dict_hash_function_seed;
}

/* The default hashing function uses SipHash implementation
 * in siphash.c. */
//默认散列函数使用 siphash.c 中的 SipHash 实现。
uint64_t siphash(const uint8_t *in, const size_t inlen, const uint8_t *k);
uint64_t siphash_nocase(const uint8_t *in, const size_t inlen, const uint8_t *k);

uint64_t dictGenHashFunction(const void *key, size_t len) {
    return siphash(key,len,dict_hash_function_seed);
}

uint64_t dictGenCaseHashFunction(const unsigned char *buf, size_t len) {
    return siphash_nocase(buf,len,dict_hash_function_seed);
}

/* ----------------------------- API implementation ------------------------- */

/* Reset hash table parameters already initialized with _dictInit()*/
//重置已使用 _dictInit() 初始化的哈希表参数
static void _dictReset(dict *d, int htidx)
{
    d->ht_table[htidx] = NULL;
    d->ht_size_exp[htidx] = -1;
    d->ht_used[htidx] = 0;
}

/* Create a new hash table */
//创建一个新的哈希表
dict *dictCreate(dictType *type)
{
    dict *d = zmalloc(sizeof(*d));

    _dictInit(d,type);
    return d;
}

/* Initialize the hash table */
//初始化哈希表
int _dictInit(dict *d, dictType *type)
{
    _dictReset(d, 0);
    _dictReset(d, 1);
    d->type = type;
    d->rehashidx = -1;
    d->pauserehash = 0;
    return DICT_OK;
}

/* Resize the table to the minimal size that contains all the elements,
 * but with the invariant of a USED/BUCKETS ratio near to <= 1 */
//将表调整为包含所有元素的最小大小，但 USEDBUCKETS 比率的不变量接近 <= 1
int dictResize(dict *d)
{
    unsigned long minimal;

    if (!dict_can_resize || dictIsRehashing(d)) return DICT_ERR;
    minimal = d->ht_used[0];
    if (minimal < DICT_HT_INITIAL_SIZE)
        minimal = DICT_HT_INITIAL_SIZE;
    return dictExpand(d, minimal);
}

/* Expand or create the hash table,
 * when malloc_failed is non-NULL, it'll avoid panic if malloc fails (in which case it'll be set to 1).
 * Returns DICT_OK if expand was performed, and DICT_ERR if skipped. */
/**扩展或创建哈希表，当 malloc_failed 为非 NULL 时，如果 malloc 失败，
 它将避免恐慌（在这种情况下，它将被设置为 1）。
 如果执行了扩展，则返回 DICT_OK，如果跳过则返回 DICT_ERR。*/
int _dictExpand(dict *d, unsigned long size, int* malloc_failed)
{
    if (malloc_failed) *malloc_failed = 0;

    /* the size is invalid if it is smaller than the number of
     * elements already inside the hash table */
    //如果大小小于哈希表中已有元素的数量，则大小无效
    if (dictIsRehashing(d) || d->ht_used[0] > size)
        return DICT_ERR;

    /* the new hash table */
    //新的哈希表
    dictEntry **new_ht_table;
    unsigned long new_ht_used;
    signed char new_ht_size_exp = _dictNextExp(size);

    /* Detect overflows */
    //检测溢出
    size_t newsize = 1ul<<new_ht_size_exp;
    if (newsize < size || newsize * sizeof(dictEntry*) < newsize)
        return DICT_ERR;

    /* Rehashing to the same table size is not useful. */
    //重新散列到相同的表大小是没有用的。
    if (new_ht_size_exp == d->ht_size_exp[0]) return DICT_ERR;

    /* Allocate the new hash table and initialize all pointers to NULL */
    //分配新的哈希表并将所有指针初始化为 NULL
    if (malloc_failed) {
        new_ht_table = ztrycalloc(newsize*sizeof(dictEntry*));
        *malloc_failed = new_ht_table == NULL;
        if (*malloc_failed)
            return DICT_ERR;
    } else
        new_ht_table = zcalloc(newsize*sizeof(dictEntry*));

    new_ht_used = 0;

    /* Is this the first initialization? If so it's not really a rehashing
     * we just set the first hash table so that it can accept keys. */
    //这是第一次初始化吗？如果是这样，这并不是真正的重新散列，我们只需设置第一个散列表，以便它可以接受密钥。
    if (d->ht_table[0] == NULL) {
        d->ht_size_exp[0] = new_ht_size_exp;
        d->ht_used[0] = new_ht_used;
        d->ht_table[0] = new_ht_table;
        return DICT_OK;
    }

    /* Prepare a second hash table for incremental rehashing */
    //为增量重新散列准备第二个散列表
    d->ht_size_exp[1] = new_ht_size_exp;
    d->ht_used[1] = new_ht_used;
    d->ht_table[1] = new_ht_table;
    d->rehashidx = 0;
    return DICT_OK;
}

/* return DICT_ERR if expand was not performed */
//如果未执行扩展，则返回 DICT_ERR
int dictExpand(dict *d, unsigned long size) {
    return _dictExpand(d, size, NULL);
}

/* return DICT_ERR if expand failed due to memory allocation failure */
//如果由于内存分配失败而导致扩展失败，则返回 DICT_ERR
int dictTryExpand(dict *d, unsigned long size) {
    int malloc_failed;
    _dictExpand(d, size, &malloc_failed);
    return malloc_failed? DICT_ERR : DICT_OK;
}

/* Performs N steps of incremental rehashing. Returns 1 if there are still
 * keys to move from the old to the new hash table, otherwise 0 is returned.
 *
 * Note that a rehashing step consists in moving a bucket (that may have more
 * than one key as we use chaining) from the old to the new hash table, however
 * since part of the hash table may be composed of empty spaces, it is not
 * guaranteed that this function will rehash even a single bucket, since it
 * will visit at max N*10 empty buckets in total, otherwise the amount of
 * work it does would be unbound and the function may block for a long time. */
/**
 * 执行 N 步增量重新散列。如果仍有键从旧哈希表移动到新哈希表，则返回 1，否则返回 0。
 * 请注意，重新散列步骤包括将存储桶（在我们使用链接时可能具有多个键）从旧散列表移动到新散列表，
 * 但是由于散列表的一部分可能由空格组成，因此不能保证这个函数甚至会重新散列一个桶，
 * 因为它总共会访问最多 N10 个空桶，否则它所做的工作量将不受限制，并且该函数可能会阻塞很长时间。
 * */
int dictRehash(dict *d, int n) {
    int empty_visits = n*10; /* Max number of empty buckets to visit. 要访问的最大空桶数。*/
    if (!dictIsRehashing(d)) return 0;

    while(n-- && d->ht_used[0] != 0) {
        dictEntry *de, *nextde;

        /* Note that rehashidx can't overflow as we are sure there are more
         * elements because ht[0].used != 0 */
        //请注意，rehashidx 不会溢出，因为我们确信还有更多元素，因为 ht[0].used != 0
        assert(DICTHT_SIZE(d->ht_size_exp[0]) > (unsigned long)d->rehashidx);
        while(d->ht_table[0][d->rehashidx] == NULL) {
            d->rehashidx++;
            if (--empty_visits == 0) return 1;
        }
        de = d->ht_table[0][d->rehashidx];
        /* Move all the keys in this bucket from the old to the new hash HT */
        //将这个桶中的所有键从旧的哈希 HT 移动到新的哈希 HT
        while(de) {
            uint64_t h;

            nextde = de->next;
            /* Get the index in the new hash table */
            //获取新哈希表中的索引
            h = dictHashKey(d, de->key) & DICTHT_SIZE_MASK(d->ht_size_exp[1]);
            de->next = d->ht_table[1][h];
            d->ht_table[1][h] = de;
            d->ht_used[0]--;
            d->ht_used[1]++;
            de = nextde;
        }
        d->ht_table[0][d->rehashidx] = NULL;
        d->rehashidx++;
    }

    /* Check if we already rehashed the whole table... */
    //检查我们是否已经重新散列了整个表......
    if (d->ht_used[0] == 0) {
        zfree(d->ht_table[0]);
        /* Copy the new ht onto the old one */
        //将新的 ht 复制到旧的 ht 上
        d->ht_table[0] = d->ht_table[1];
        d->ht_used[0] = d->ht_used[1];
        d->ht_size_exp[0] = d->ht_size_exp[1];
        _dictReset(d, 1);
        d->rehashidx = -1;
        return 0;
    }

    /* More to rehash... */
    //更多要重述...
    return 1;
}

long long timeInMilliseconds(void) {
    struct timeval tv;

    gettimeofday(&tv,NULL);
    return (((long long)tv.tv_sec)*1000)+(tv.tv_usec/1000);
}

/* Rehash in ms+"delta" milliseconds. The value of "delta" is larger 
 * than 0, and is smaller than 1 in most cases. The exact upper bound 
 * depends on the running time of dictRehash(d,100).*/
/**
 * 在 ms+"delta" 毫秒内重新散列。 “delta”的值大于0，大多数情况下小于1。
 * 确切的上限取决于 dictRehash(d,100) 的运行时间。
 * */
int dictRehashMilliseconds(dict *d, int ms) {
    if (d->pauserehash > 0) return 0;

    long long start = timeInMilliseconds();
    int rehashes = 0;

    while(dictRehash(d,100)) {
        rehashes += 100;
        if (timeInMilliseconds()-start > ms) break;
    }
    return rehashes;
}

/* This function performs just a step of rehashing, and only if hashing has
 * not been paused for our hash table. When we have iterators in the
 * middle of a rehashing we can't mess with the two hash tables otherwise
 * some elements can be missed or duplicated.
 *
 * This function is called by common lookup or update operations in the
 * dictionary so that the hash table automatically migrates from H1 to H2
 * while it is actively used. */
/**
 * 这个函数只执行一个重新散列的步骤，并且只有在我们的散列表没有暂停散列的情况下。
 * 当我们在重新散列的中间有迭代器时，我们不能弄乱两个散列表，否则一些元素可能会丢失或重复。
 * 该函数由字典中的常见查找或更新操作调用，以便哈希表在活跃使用时自动从 H1 迁移到 H2。
 * */
static void _dictRehashStep(dict *d) {
    if (d->pauserehash == 0) dictRehash(d,1);
}

/* Add an element to the target hash table */
//将元素添加到目标哈希表
int dictAdd(dict *d, void *key, void *val)
{
    dictEntry *entry = dictAddRaw(d,key,NULL);

    if (!entry) return DICT_ERR;
    dictSetVal(d, entry, val);
    return DICT_OK;
}

/* Low level add or find:
 * This function adds the entry but instead of setting a value returns the
 * dictEntry structure to the user, that will make sure to fill the value
 * field as they wish.
 *
 * This function is also directly exposed to the user API to be called
 * mainly in order to store non-pointers inside the hash value, example:
 *
 * entry = dictAddRaw(dict,mykey,NULL);
 * if (entry != NULL) dictSetSignedIntegerVal(entry,1000);
 *
 * Return values:
 *
 * If key already exists NULL is returned, and "*existing" is populated
 * with the existing entry if existing is not NULL.
 *
 * If key was added, the hash entry is returned to be manipulated by the caller.
 */
/**
 * 低级添加或查找：此函数添加条目，但不是设置值，
 * 而是将 dictEntry 结构返回给用户，这将确保根据需要填充值字段。
 * 这个函数也直接暴露给用户API被调用，主要是为了在hash值里面存储非指针，例如：
 *   entry = dictAddRaw(dict,mykey,NULL);
 *   if (entry != NULL) dictSetSignedIntegerVal(entry,1000);
 * 返回值：如果键已经存在则返回NULL，如果existing不为NULL，则用现有条目填充“existing”。
 * 如果添加了键，则返回哈希条目以供调用者操作。*/
dictEntry *dictAddRaw(dict *d, void *key, dictEntry **existing)
{
    long index;
    dictEntry *entry;
    int htidx;

    if (dictIsRehashing(d)) _dictRehashStep(d);

    /* Get the index of the new element, or -1 if
     * the element already exists. */
    //获取新元素的索引，如果元素已经存在，则为 -1。
    if ((index = _dictKeyIndex(d, key, dictHashKey(d,key), existing)) == -1)
        return NULL;

    /* Allocate the memory and store the new entry.
     * Insert the element in top, with the assumption that in a database
     * system it is more likely that recently added entries are accessed
     * more frequently. */
    /**分配内存并存储新条目。在顶部插入元素，假设在数据库系统中，最近添加的条目更有可能被更频繁地访问。*/
    htidx = dictIsRehashing(d) ? 1 : 0;
    size_t metasize = dictMetadataSize(d);
    entry = zmalloc(sizeof(*entry) + metasize);
    if (metasize > 0) {
        memset(dictMetadata(entry), 0, metasize);
    }
    entry->next = d->ht_table[htidx][index];
    d->ht_table[htidx][index] = entry;
    d->ht_used[htidx]++;

    /* Set the hash entry fields. */
    //设置哈希条目字段。
    dictSetKey(d, entry, key);
    return entry;
}

/* Add or Overwrite:
 * Add an element, discarding the old value if the key already exists.
 * Return 1 if the key was added from scratch, 0 if there was already an
 * element with such key and dictReplace() just performed a value update
 * operation. */
/**
 * 添加或覆盖：添加一个元素，如果键已经存在则丢弃旧值。
 * 如果键是从头开始添加的，则返回 1，如果已经存在具有该键的元素并且 dictReplace() 刚刚执行了值更新操作，则返回 0。
 * */
int dictReplace(dict *d, void *key, void *val)
{
    dictEntry *entry, *existing, auxentry;

    /* Try to add the element. If the key
     * does not exists dictAdd will succeed. */
    //尝试添加元素。如果键不存在 dictAdd 将成功。
    entry = dictAddRaw(d,key,&existing);
    if (entry) {
        dictSetVal(d, entry, val);
        return 1;
    }

    /* Set the new value and free the old one. Note that it is important
     * to do that in this order, as the value may just be exactly the same
     * as the previous one. In this context, think to reference counting,
     * you want to increment (set), and then decrement (free), and not the
     * reverse. */
    /**
     * 设置新值并释放旧值。请注意，按此顺序执行此操作很重要，因为该值可能与前一个完全相同。
     * 在这种情况下，考虑到引用计数，您想要递增（设置），然后递减（自由），而不是相反。*/
    auxentry = *existing;
    dictSetVal(d, existing, val);
    dictFreeVal(d, &auxentry);
    return 0;
}

/* Add or Find:
 * dictAddOrFind() is simply a version of dictAddRaw() that always
 * returns the hash entry of the specified key, even if the key already
 * exists and can't be added (in that case the entry of the already
 * existing key is returned.)
 *
 * See dictAddRaw() for more information. */
/**
 * 添加或查找：dictAddOrFind() 只是 dictAddRaw() 的一个版本，它总是返回指定键的哈希条目，
 * 即使键已经存在并且无法添加（在这种情况下，已经存在的键的条目是返回。）
 * 有关更多信息，请参阅 dictAddRaw()。
 * */
dictEntry *dictAddOrFind(dict *d, void *key) {
    dictEntry *entry, *existing;
    entry = dictAddRaw(d,key,&existing);
    return entry ? entry : existing;
}

/* Search and remove an element. This is a helper function for
 * dictDelete() and dictUnlink(), please check the top comment
 * of those functions. */
//搜索并删除元素。这是 dictDelete() 和 dictUnlink() 的辅助函数，请查看这些函数的顶部注释。
static dictEntry *dictGenericDelete(dict *d, const void *key, int nofree) {
    uint64_t h, idx;
    dictEntry *he, *prevHe;
    int table;

    /* dict is empty 字典为空*/
    if (dictSize(d) == 0) return NULL;

    if (dictIsRehashing(d)) _dictRehashStep(d);
    h = dictHashKey(d, key);

    for (table = 0; table <= 1; table++) {
        idx = h & DICTHT_SIZE_MASK(d->ht_size_exp[table]);
        he = d->ht_table[table][idx];
        prevHe = NULL;
        while(he) {
            if (key==he->key || dictCompareKeys(d, key, he->key)) {
                /* Unlink the element from the list 从列表中取消链接元素*/
                if (prevHe)
                    prevHe->next = he->next;
                else
                    d->ht_table[table][idx] = he->next;
                if (!nofree) {
                    dictFreeUnlinkedEntry(d, he);
                }
                d->ht_used[table]--;
                return he;
            }
            prevHe = he;
            he = he->next;
        }
        if (!dictIsRehashing(d)) break;
    }
    return NULL; /* not found */
}

/* Remove an element, returning DICT_OK on success or DICT_ERR if the
 * element was not found. */
//删除一个元素，成功返回 DICT_OK，如果未找到该元素则返回 DICT_ERR。
int dictDelete(dict *ht, const void *key) {
    return dictGenericDelete(ht,key,0) ? DICT_OK : DICT_ERR;
}

/* Remove an element from the table, but without actually releasing
 * the key, value and dictionary entry. The dictionary entry is returned
 * if the element was found (and unlinked from the table), and the user
 * should later call `dictFreeUnlinkedEntry()` with it in order to release it.
 * Otherwise if the key is not found, NULL is returned.
 *
 * This function is useful when we want to remove something from the hash
 * table but want to use its value before actually deleting the entry.
 * Without this function the pattern would require two lookups:
 *
 *  entry = dictFind(...);
 *  // Do something with entry
 *  dictDelete(dictionary,entry);
 *
 * Thanks to this function it is possible to avoid this, and use
 * instead:
 *
 * entry = dictUnlink(dictionary,entry);
 * // Do something with entry
 * dictFreeUnlinkedEntry(entry); // <- This does not need to lookup again.
 */
/**
 * 从表中删除一个元素，但不实际释放键、值和字典条目。
 * 如果找到该元素（并从表中取消链接），则返回字典条目，用户稍后应调用 `dictFreeUnlinkedEntry()` 以释放它。
 * 否则，如果找不到键，则返回 NULL。
 * 当我们想从哈希表中删除一些东西但想在实际删除条目之前使用它的值时，这个函数很有用。
 * 如果没有这个函数，模式将需要两次查找：
 * entry = dictFind(...);
 * // Do something with entry
 * dictDelete(dictionary,entry);
 * 做一些事情多亏了这个功能，可以避免这种情况，而是使用：
 * entry = dictUnlink(dictionary,entry);
 * // Do something with entry
 * dictFreeUnlinkedEntry(entry); <- 这不需要再次查找。
 * */
dictEntry *dictUnlink(dict *d, const void *key) {
    return dictGenericDelete(d,key,1);
}

/* You need to call this function to really free the entry after a call
 * to dictUnlink(). It's safe to call this function with 'he' = NULL. */
//在调用 dictUnlink() 后，您需要调用此函数才能真正释放条目。使用 'he' = NULL 调用此函数是安全的。
void dictFreeUnlinkedEntry(dict *d, dictEntry *he) {
    if (he == NULL) return;
    dictFreeKey(d, he);
    dictFreeVal(d, he);
    zfree(he);
}

/* Destroy an entire dictionary 销毁整个字典*/
int _dictClear(dict *d, int htidx, void(callback)(dict*)) {
    unsigned long i;

    /* Free all the elements 释放所有元素*/
    for (i = 0; i < DICTHT_SIZE(d->ht_size_exp[htidx]) && d->ht_used[htidx] > 0; i++) {
        dictEntry *he, *nextHe;

        if (callback && (i & 65535) == 0) callback(d);

        if ((he = d->ht_table[htidx][i]) == NULL) continue;
        while(he) {
            nextHe = he->next;
            dictFreeKey(d, he);
            dictFreeVal(d, he);
            zfree(he);
            d->ht_used[htidx]--;
            he = nextHe;
        }
    }
    /* Free the table and the allocated cache structure */
    //释放表和分配的缓存结构
    zfree(d->ht_table[htidx]);
    /* Re-initialize the table 重新初始化表*/
    _dictReset(d, htidx);
    return DICT_OK; /* never fails 从未失败*/
}

/* Clear & Release the hash table */
//清除并释放哈希表
void dictRelease(dict *d)
{
    _dictClear(d,0,NULL);
    _dictClear(d,1,NULL);
    zfree(d);
}

dictEntry *dictFind(dict *d, const void *key)
{
    dictEntry *he;
    uint64_t h, idx, table;

    if (dictSize(d) == 0) return NULL; /* dict is empty */
    if (dictIsRehashing(d)) _dictRehashStep(d);
    h = dictHashKey(d, key);
    for (table = 0; table <= 1; table++) {
        idx = h & DICTHT_SIZE_MASK(d->ht_size_exp[table]);
        he = d->ht_table[table][idx];
        while(he) {
            if (key==he->key || dictCompareKeys(d, key, he->key))
                return he;
            he = he->next;
        }
        if (!dictIsRehashing(d)) return NULL;
    }
    return NULL;
}

void *dictFetchValue(dict *d, const void *key) {
    dictEntry *he;

    he = dictFind(d,key);
    return he ? dictGetVal(he) : NULL;
}

/* A fingerprint is a 64 bit number that represents the state of the dictionary
 * at a given time, it's just a few dict properties xored together.
 * When an unsafe iterator is initialized, we get the dict fingerprint, and check
 * the fingerprint again when the iterator is released.
 * If the two fingerprints are different it means that the user of the iterator
 * performed forbidden operations against the dictionary while iterating. */
/**
 * 指纹是一个 64 位数字，表示给定时间字典的状态，它只是几个 dict 属性异或在一起。
 * 当一个不安全的迭代器被初始化时，我们得到dict指纹，并在迭代器被释放时再次检查指纹。
 * 如果两个指纹不同，则意味着迭代器的用户在迭代时对字典执行了禁止的操作。
 * */
unsigned long long dictFingerprint(dict *d) {
    unsigned long long integers[6], hash = 0;
    int j;

    integers[0] = (long) d->ht_table[0];
    integers[1] = d->ht_size_exp[0];
    integers[2] = d->ht_used[0];
    integers[3] = (long) d->ht_table[1];
    integers[4] = d->ht_size_exp[1];
    integers[5] = d->ht_used[1];

    /* We hash N integers by summing every successive integer with the integer
     * hashing of the previous sum. Basically:
     *
     * Result = hash(hash(hash(int1)+int2)+int3) ...
     *
     * This way the same set of integers in a different order will (likely) hash
     * to a different number. */
    /**
     * 我们通过将每个连续整数与前一个和的整数散列相加来散列 N 个整数。基本上：
     * Result = hash(hash(hash(int1)+int2)+int3) ...
     * 这样，同一组不同顺序的整数将（可能）散列到不同的数字。
     * */
    for (j = 0; j < 6; j++) {
        hash += integers[j];
        /* For the hashing step we use Tomas Wang's 64 bit integer hash. */
        //对于散列步骤，我们使用 Tomas Wang 的 64 位整数散列。
        hash = (~hash) + (hash << 21); // hash = (hash << 21) - hash - 1;
        hash = hash ^ (hash >> 24);
        hash = (hash + (hash << 3)) + (hash << 8); // hash * 265
        hash = hash ^ (hash >> 14);
        hash = (hash + (hash << 2)) + (hash << 4); // hash * 21
        hash = hash ^ (hash >> 28);
        hash = hash + (hash << 31);
    }
    return hash;
}

dictIterator *dictGetIterator(dict *d)
{
    dictIterator *iter = zmalloc(sizeof(*iter));

    iter->d = d;
    iter->table = 0;
    iter->index = -1;
    iter->safe = 0;
    iter->entry = NULL;
    iter->nextEntry = NULL;
    return iter;
}

dictIterator *dictGetSafeIterator(dict *d) {
    dictIterator *i = dictGetIterator(d);

    i->safe = 1;
    return i;
}

dictEntry *dictNext(dictIterator *iter)
{
    while (1) {
        if (iter->entry == NULL) {
            if (iter->index == -1 && iter->table == 0) {
                if (iter->safe)
                    dictPauseRehashing(iter->d);
                else
                    iter->fingerprint = dictFingerprint(iter->d);
            }
            iter->index++;
            if (iter->index >= (long) DICTHT_SIZE(iter->d->ht_size_exp[iter->table])) {
                if (dictIsRehashing(iter->d) && iter->table == 0) {
                    iter->table++;
                    iter->index = 0;
                } else {
                    break;
                }
            }
            iter->entry = iter->d->ht_table[iter->table][iter->index];
        } else {
            iter->entry = iter->nextEntry;
        }
        if (iter->entry) {
            /* We need to save the 'next' here, the iterator user
             * may delete the entry we are returning. */
            //我们需要在这里保存"next"，迭代器用户可能会删除我们返回的条目。
            iter->nextEntry = iter->entry->next;
            return iter->entry;
        }
    }
    return NULL;
}

void dictReleaseIterator(dictIterator *iter)
{
    if (!(iter->index == -1 && iter->table == 0)) {
        if (iter->safe)
            dictResumeRehashing(iter->d);
        else
            assert(iter->fingerprint == dictFingerprint(iter->d));
    }
    zfree(iter);
}

/* Return a random entry from the hash table. Useful to
 * implement randomized algorithms */
//从哈希表中返回一个随机条目。用于实现随机算法
dictEntry *dictGetRandomKey(dict *d)
{
    dictEntry *he, *orighe;
    unsigned long h;
    int listlen, listele;

    if (dictSize(d) == 0) return NULL;
    if (dictIsRehashing(d)) _dictRehashStep(d);
    if (dictIsRehashing(d)) {
        unsigned long s0 = DICTHT_SIZE(d->ht_size_exp[0]);
        do {
            /* We are sure there are no elements in indexes from 0
             * to rehashidx-1 */
            //我们确定从 0 到 rehashidx-1 的索引中没有元素
            h = d->rehashidx + (randomULong() % (dictSlots(d) - d->rehashidx));
            he = (h >= s0) ? d->ht_table[1][h - s0] : d->ht_table[0][h];
        } while(he == NULL);
    } else {
        unsigned long m = DICTHT_SIZE_MASK(d->ht_size_exp[0]);
        do {
            h = randomULong() & m;
            he = d->ht_table[0][h];
        } while(he == NULL);
    }

    /* Now we found a non empty bucket, but it is a linked
     * list and we need to get a random element from the list.
     * The only sane way to do so is counting the elements and
     * select a random index. */
    /**
     * 现在我们找到了一个非空的桶，但它是一个链表，我们需要从链表中获取一个随机元素。
     * 这样做的唯一明智的方法是计算元素并选择一个随机索引。
     * */
    listlen = 0;
    orighe = he;
    while(he) {
        he = he->next;
        listlen++;
    }
    listele = random() % listlen;
    he = orighe;
    while(listele--) he = he->next;
    return he;
}

/* This function samples the dictionary to return a few keys from random
 * locations.
 *
 * It does not guarantee to return all the keys specified in 'count', nor
 * it does guarantee to return non-duplicated elements, however it will make
 * some effort to do both things.
 *
 * Returned pointers to hash table entries are stored into 'des' that
 * points to an array of dictEntry pointers. The array must have room for
 * at least 'count' elements, that is the argument we pass to the function
 * to tell how many random elements we need.
 *
 * The function returns the number of items stored into 'des', that may
 * be less than 'count' if the hash table has less than 'count' elements
 * inside, or if not enough elements were found in a reasonable amount of
 * steps.
 *
 * Note that this function is not suitable when you need a good distribution
 * of the returned items, but only when you need to "sample" a given number
 * of continuous elements to run some kind of algorithm or to produce
 * statistics. However the function is much faster than dictGetRandomKey()
 * at producing N elements. */
/**
 * 此函数对字典进行采样以从随机位置返回一些键。
 * 它不保证返回 'count' 中指定的所有键，也不保证返回不重复的元素，但是它会付出一些努力来做这两件事。
 * 返回的指向哈希表条目的指针存储在指向 dictEntry 指针数组的“des”中。
 * 数组必须至少有 'count' 个元素的空间，这是我们传递给函数的参数，以告知我们需要多少随机元素。
 * 该函数返回存储在“des”中的项目数，如果哈希表内部的“count”个元素少于“count”，
 * 或者在合理数量的步骤中没有找到足够的元素，则该值可能小于“count”。
 * 请注意，当您需要返回项目的良好分布时，此函数不适合，
 * 但仅当您需要“采样”给定数量的连续元素以运行某种算法或产生统计数据时。
 * 然而，该函数在生成 N 个元素时比 dictGetRandomKey() 快得多。
 * */
unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count) {
    unsigned long j; /* internal hash table id, 0 or 1. */
    unsigned long tables; /* 1 or 2 tables? */
    unsigned long stored = 0, maxsizemask;
    unsigned long maxsteps;

    if (dictSize(d) < count) count = dictSize(d);
    maxsteps = count*10;

    /* Try to do a rehashing work proportional to 'count'. */
    //尝试进行与“计数”成比例的重新散列工作。
    for (j = 0; j < count; j++) {
        if (dictIsRehashing(d))
            _dictRehashStep(d);
        else
            break;
    }

    tables = dictIsRehashing(d) ? 2 : 1;
    maxsizemask = DICTHT_SIZE_MASK(d->ht_size_exp[0]);
    if (tables > 1 && maxsizemask < DICTHT_SIZE_MASK(d->ht_size_exp[1]))
        maxsizemask = DICTHT_SIZE_MASK(d->ht_size_exp[1]);

    /* Pick a random point inside the larger table. */
    //在较大的表格中选择一个随机点。
    unsigned long i = randomULong() & maxsizemask;
    unsigned long emptylen = 0; /* Continuous empty entries so far. 到目前为止连续的空条目。*/
    while(stored < count && maxsteps--) {
        for (j = 0; j < tables; j++) {
            /* Invariant of the dict.c rehashing: up to the indexes already
             * visited in ht[0] during the rehashing, there are no populated
             * buckets, so we can skip ht[0] for indexes between 0 and idx-1. */
            /**
             * dict.c 重新散列的不变性：直到在重新散列期间已经在 ht[0] 中访问过的索引，
             * 没有填充的桶，因此我们可以跳过 ht[0] 用于 0 和 idx-1 之间的索引。
             * */
            if (tables == 2 && j == 0 && i < (unsigned long) d->rehashidx) {
                /* Moreover, if we are currently out of range in the second
                 * table, there will be no elements in both tables up to
                 * the current rehashing index, so we jump if possible.
                 * (this happens when going from big to small table). */
                /**
                 * 此外，如果我们当前在第二个表中超出范围，则两个表中都不会存在直到当前重新散列索引的元素，
                 * 因此我们会尽可能跳转。 （从大桌子到小桌子时会发生这种情况）。
                 * */
                if (i >= DICTHT_SIZE(d->ht_size_exp[1]))
                    i = d->rehashidx;
                else
                    continue;
            }
            if (i >= DICTHT_SIZE(d->ht_size_exp[j])) continue; /* Out of range for this table. 超出此表的范围。*/
            dictEntry *he = d->ht_table[j][i];

            /* Count contiguous empty buckets, and jump to other
             * locations if they reach 'count' (with a minimum of 5). */
            //计算连续的空桶，如果它们达到“计数”（至少 5 个），则跳转到其他位置。
            if (he == NULL) {
                emptylen++;
                if (emptylen >= 5 && emptylen > count) {
                    i = randomULong() & maxsizemask;
                    emptylen = 0;
                }
            } else {
                emptylen = 0;
                while (he) {
                    /* Collect all the elements of the buckets found non
                     * empty while iterating. */
                    //收集迭代时发现的非空桶的所有元素。
                    *des = he;
                    des++;
                    he = he->next;
                    stored++;
                    if (stored == count) return stored;
                }
            }
        }
        i = (i+1) & maxsizemask;
    }
    return stored;
}

/* This is like dictGetRandomKey() from the POV of the API, but will do more
 * work to ensure a better distribution of the returned element.
 *
 * This function improves the distribution because the dictGetRandomKey()
 * problem is that it selects a random bucket, then it selects a random
 * element from the chain in the bucket. However elements being in different
 * chain lengths will have different probabilities of being reported. With
 * this function instead what we do is to consider a "linear" range of the table
 * that may be constituted of N buckets with chains of different lengths
 * appearing one after the other. Then we report a random element in the range.
 * In this way we smooth away the problem of different chain lengths. */
/**
 * 这类似于 API 的 POV 中的 dictGetRandomKey()，但会做更多的工作来确保返回元素的更好分布。
 * 这个函数改进了分布，因为 dictGetRandomKey() 的问题是它选择了一个随机桶，
 * 然后它从桶中的链中选择一个随机元素。
 * 然而，不同链长的元素将具有不同的被报告概率。
 * 使用这个函数，我们所做的是考虑表格的“线性”范围，该范围可能由 N 个桶组成，
 * 其中不同长度的链一个接一个地出现。然后我们报告范围内的随机元素。
 * 通过这种方式，我们解决了不同链长的问题。
 * */
#define GETFAIR_NUM_ENTRIES 15
dictEntry *dictGetFairRandomKey(dict *d) {
    dictEntry *entries[GETFAIR_NUM_ENTRIES];
    unsigned int count = dictGetSomeKeys(d,entries,GETFAIR_NUM_ENTRIES);
    /* Note that dictGetSomeKeys() may return zero elements in an unlucky
     * run() even if there are actually elements inside the hash table. So
     * when we get zero, we call the true dictGetRandomKey() that will always
     * yield the element if the hash table has at least one. */
    /**
     * 注意 dictGetSomeKeys() 可能会在不幸的 run() 中返回零个元素，即使哈希表中确实有元素。
     * 所以当我们得到零时，我们调用真正的 dictGetRandomKey() 如果哈希表至少有一个，它将总是产生元素。*/
    if (count == 0) return dictGetRandomKey(d);
    unsigned int idx = rand() % count;
    return entries[idx];
}

/* Function to reverse bits. Algorithm from:
 * http://graphics.stanford.edu/~seander/bithacks.html#ReverseParallel */
//位反转功能。
static unsigned long rev(unsigned long v) {
    unsigned long s = CHAR_BIT * sizeof(v); // bit size; must be power of 2 位大小；必须是 2 的幂
    unsigned long mask = ~0UL;
    while ((s >>= 1) > 0) {
        mask ^= (mask << s);
        v = ((v >> s) & mask) | ((v << s) & ~mask);
    }
    return v;
}

/* dictScan() is used to iterate over the elements of a dictionary.
 *
 * Iterating works the following way:
 *
 * 1) Initially you call the function using a cursor (v) value of 0.
 * 2) The function performs one step of the iteration, and returns the
 *    new cursor value you must use in the next call.
 * 3) When the returned cursor is 0, the iteration is complete.
 *
 * The function guarantees all elements present in the
 * dictionary get returned between the start and end of the iteration.
 * However it is possible some elements get returned multiple times.
 *
 * For every element returned, the callback argument 'fn' is
 * called with 'privdata' as first argument and the dictionary entry
 * 'de' as second argument.
 *
 * HOW IT WORKS.
 *
 * The iteration algorithm was designed by Pieter Noordhuis.
 * The main idea is to increment a cursor starting from the higher order
 * bits. That is, instead of incrementing the cursor normally, the bits
 * of the cursor are reversed, then the cursor is incremented, and finally
 * the bits are reversed again.
 *
 * This strategy is needed because the hash table may be resized between
 * iteration calls.
 *
 * dict.c hash tables are always power of two in size, and they
 * use chaining, so the position of an element in a given table is given
 * by computing the bitwise AND between Hash(key) and SIZE-1
 * (where SIZE-1 is always the mask that is equivalent to taking the rest
 *  of the division between the Hash of the key and SIZE).
 *
 * For example if the current hash table size is 16, the mask is
 * (in binary) 1111. The position of a key in the hash table will always be
 * the last four bits of the hash output, and so forth.
 *
 * WHAT HAPPENS IF THE TABLE CHANGES IN SIZE?
 *
 * If the hash table grows, elements can go anywhere in one multiple of
 * the old bucket: for example let's say we already iterated with
 * a 4 bit cursor 1100 (the mask is 1111 because hash table size = 16).
 *
 * If the hash table will be resized to 64 elements, then the new mask will
 * be 111111. The new buckets you obtain by substituting in ??1100
 * with either 0 or 1 can be targeted only by keys we already visited
 * when scanning the bucket 1100 in the smaller hash table.
 *
 * By iterating the higher bits first, because of the inverted counter, the
 * cursor does not need to restart if the table size gets bigger. It will
 * continue iterating using cursors without '1100' at the end, and also
 * without any other combination of the final 4 bits already explored.
 *
 * Similarly when the table size shrinks over time, for example going from
 * 16 to 8, if a combination of the lower three bits (the mask for size 8
 * is 111) were already completely explored, it would not be visited again
 * because we are sure we tried, for example, both 0111 and 1111 (all the
 * variations of the higher bit) so we don't need to test it again.
 *
 * WAIT... YOU HAVE *TWO* TABLES DURING REHASHING!
 *
 * Yes, this is true, but we always iterate the smaller table first, then
 * we test all the expansions of the current cursor into the larger
 * table. For example if the current cursor is 101 and we also have a
 * larger table of size 16, we also test (0)101 and (1)101 inside the larger
 * table. This reduces the problem back to having only one table, where
 * the larger one, if it exists, is just an expansion of the smaller one.
 *
 * LIMITATIONS
 *
 * This iterator is completely stateless, and this is a huge advantage,
 * including no additional memory used.
 *
 * The disadvantages resulting from this design are:
 *
 * 1) It is possible we return elements more than once. However this is usually
 *    easy to deal with in the application level.
 * 2) The iterator must return multiple elements per call, as it needs to always
 *    return all the keys chained in a given bucket, and all the expansions, so
 *    we are sure we don't miss keys moving during rehashing.
 * 3) The reverse cursor is somewhat hard to understand at first, but this
 *    comment is supposed to help.
 */
/**
 * dictScan() 用于迭代字典的元素。
 * 迭代的工作方式如下：
 * 1) 最初，您使用游标 (v) 值 0 调用该函数。
 * 2) 该函数执行迭代的一步，并返回您必须在下一次调用中使用的新游标值。
 * 3）当返回的游标为0时，迭代完成。
 * 该函数保证字典中存在的所有元素在迭代的开始和结束之间返回。
 * 但是，某些元素可能会多次返回。
 * 对于返回的每个元素，回调参数“fn”被调用，“privdata”作为第一个参数，字典条目“de”作为第二个参数。
 * 这个怎么运作。迭代算法由 Pieter Noordhuis 设计。
 * 主要思想是从高位开始增加光标。
 * 也就是说，不是正常递增光标，而是将光标的位反转，然后将光标递增，最后再次反转位。
 * 这个策略是必要的，因为哈希表可能会在迭代调用之间调整大小。
 * dict.c 哈希表的大小始终是 2 的幂，并且它们使用链接，
 * 因此给定表中元素的位置是通过计算 Hash(key) 和 SIZE-1（其中 SIZE-1 是总是相当于取键的哈希和大小之间的其余部分的掩码）。
 * 例如，如果当前哈希表大小为 16，则掩码为（二进制）1111。
 * 哈希表中键的位置将始终是哈希输出的最后四位，依此类推。
 * 如果表的大小发生变化会发生什么？如果哈希表增长，元素可以在旧桶的倍数中的任何位置：
 * 例如，假设我们已经使用 4 位游标 1100 进行了迭代（掩码是 1111，因为哈希表大小 = 16）。
 * 如果哈希表将调整大小为 64 个元素，则新掩码将为 111111。
 * 通过将 ??1100 替换为 0 或 1 获得的新存储桶只能由我们在扫描存储桶 1100 时已访问过的键作为目标较小的哈希表。
 * 通过先迭代高位，由于反转计数器，如果表大小变大，则不需要重新启动游标。
 * 它将继续使用末尾没有“1100”的游标进行迭代，并且也没有已经探索的最后 4 位的任何其他组合。
 * 类似地，当表大小随着时间而缩小时，例如从 16 到 8，如果低三位的组合（大小 8 的掩码为 111）已经被完全探索过，
 * 则不会再次访问它，因为我们确定我们例如，尝试了 0111 和 1111（高位的所有变体），所以我们不需要再次测试它。
 * 等等……你有两张桌子正在重新散列！是的，这是真的，但我们总是先迭代较小的表，
 * 然后我们将当前游标的所有扩展测试到较大的表中。
 * 例如，如果当前游标是 101，并且我们还有一个大小为 16 的较大表，我们还在较大的表中测试 (0)101 和 (1)101。
 * 这将问题减少到只有一张表，其中较大的一张（如果存在）只是较小一张的扩展。
 * 限制 这个迭代器是完全无状态的，这是一个巨大的优势，包括不使用额外的内存。
 * 这种设计的缺点是：
 * 1）我们可能多次返回元素。但是，这通常在应用程序级别很容易处理。
 * 2) 迭代器每次调用必须返回多个元素，因为它需要始终返回给定存储桶中链接的所有键以及所有扩展，
 *     因此我们确保不会错过重新散列期间移动的键。
 * 3）反向光标一开始有点难以理解，但这个评论应该会有所帮助。
 * */
unsigned long dictScan(dict *d,
                       unsigned long v,
                       dictScanFunction *fn,
                       dictScanBucketFunction* bucketfn,
                       void *privdata)
{
    int htidx0, htidx1;
    const dictEntry *de, *next;
    unsigned long m0, m1;

    if (dictSize(d) == 0) return 0;

    /* This is needed in case the scan callback tries to do dictFind or alike. */
    //如果扫描回调尝试执行 dictFind 或类似操作，则需要这样做。
    dictPauseRehashing(d);

    if (!dictIsRehashing(d)) {
        htidx0 = 0;
        m0 = DICTHT_SIZE_MASK(d->ht_size_exp[htidx0]);

        /* Emit entries at cursor 在光标处发出条目*/
        if (bucketfn) bucketfn(d, &d->ht_table[htidx0][v & m0]);
        de = d->ht_table[htidx0][v & m0];
        while (de) {
            next = de->next;
            fn(privdata, de);
            de = next;
        }

        /* Set unmasked bits so incrementing the reversed cursor
         * operates on the masked bits */
        //设置未屏蔽位，以便递增反向光标对屏蔽位进行操作
        v |= ~m0;

        /* Increment the reverse cursor 增加反向光标*/
        v = rev(v);
        v++;
        v = rev(v);

    } else {
        htidx0 = 0;
        htidx1 = 1;

        /* Make sure t0 is the smaller and t1 is the bigger table */
        //确保 t0 是较小的，而 t1 是较大的表
        if (DICTHT_SIZE(d->ht_size_exp[htidx0]) > DICTHT_SIZE(d->ht_size_exp[htidx1])) {
            htidx0 = 1;
            htidx1 = 0;
        }

        m0 = DICTHT_SIZE_MASK(d->ht_size_exp[htidx0]);
        m1 = DICTHT_SIZE_MASK(d->ht_size_exp[htidx1]);

        /* Emit entries at cursor 在光标处发出条目*/
        if (bucketfn) bucketfn(d, &d->ht_table[htidx0][v & m0]);
        de = d->ht_table[htidx0][v & m0];
        while (de) {
            next = de->next;
            fn(privdata, de);
            de = next;
        }

        /* Iterate over indices in larger table that are the expansion
         * of the index pointed to by the cursor in the smaller table */
        //迭代较大表中的索引，这些索引是较小表中游标指向的索引的扩展
        do {
            /* Emit entries at cursor 在光标处发出条目*/
            if (bucketfn) bucketfn(d, &d->ht_table[htidx1][v & m1]);
            de = d->ht_table[htidx1][v & m1];
            while (de) {
                next = de->next;
                fn(privdata, de);
                de = next;
            }

            /* Increment the reverse cursor not covered by the smaller mask.*/
            //增加较小掩码未覆盖的反向光标。
            v |= ~m1;
            v = rev(v);
            v++;
            v = rev(v);

            /* Continue while bits covered by mask difference is non-zero */
            //当掩码差异覆盖的位不为零时继续
        } while (v & (m0 ^ m1));
    }

    dictResumeRehashing(d);

    return v;
}

/* ------------------------- private functions ------------------------------ */

/* Because we may need to allocate huge memory chunk at once when dict
 * expands, we will check this allocation is allowed or not if the dict
 * type has expandAllowed member function. */
/**
 * 因为我们可能需要在dict扩展时一次分配巨大的内存块，如果dict类型具有expandAllowed成员函数，我们将检查是否允许分配。
 * */
static int dictTypeExpandAllowed(dict *d) {
    if (d->type->expandAllowed == NULL) return 1;
    return d->type->expandAllowed(
                    DICTHT_SIZE(_dictNextExp(d->ht_used[0] + 1)) * sizeof(dictEntry*),
                    (double)d->ht_used[0] / DICTHT_SIZE(d->ht_size_exp[0]));
}

/* Expand the hash table if needed 如果需要，展开哈希表*/
static int _dictExpandIfNeeded(dict *d)
{
    /* Incremental rehashing already in progress. Return. */
    //增量重新散列已经在进行中。返回。
    if (dictIsRehashing(d)) return DICT_OK;

    /* If the hash table is empty expand it to the initial size. */
    //如果哈希表为空，则将其扩展为初始大小。
    if (DICTHT_SIZE(d->ht_size_exp[0]) == 0) return dictExpand(d, DICT_HT_INITIAL_SIZE);

    /* If we reached the 1:1 ratio, and we are allowed to resize the hash
     * table (global setting) or we should avoid it but the ratio between
     * elements/buckets is over the "safe" threshold, we resize doubling
     * the number of buckets. */
    /**
     * 如果我们达到了 1:1 的比例，并且我们被允许调整哈希表的大小（全局设置）
     * 或者我们应该避免它但元素桶之间的比例超过“安全”阈值，我们调整桶的数量加倍。
     * */
    if (d->ht_used[0] >= DICTHT_SIZE(d->ht_size_exp[0]) &&
        (dict_can_resize ||
         d->ht_used[0]/ DICTHT_SIZE(d->ht_size_exp[0]) > dict_force_resize_ratio) &&
        dictTypeExpandAllowed(d))
    {
        return dictExpand(d, d->ht_used[0] + 1);
    }
    return DICT_OK;
}

/* TODO: clz optimization */
/* Our hash table capability is a power of two */
//我们的哈希表功能是 2 的幂
static signed char _dictNextExp(unsigned long size)
{
    unsigned char e = DICT_HT_INITIAL_EXP;

    if (size >= LONG_MAX) return (8*sizeof(long)-1);
    while(1) {
        if (((unsigned long)1<<e) >= size)
            return e;
        e++;
    }
}

/* Returns the index of a free slot that can be populated with
 * a hash entry for the given 'key'.
 * If the key already exists, -1 is returned
 * and the optional output parameter may be filled.
 *
 * Note that if we are in the process of rehashing the hash table, the
 * index is always returned in the context of the second (new) hash table. */
/**
 * 返回可用给定“键”的哈希条目填充的空闲槽的索引。
 * 如果键已经存在，则返回 -1 并且可以填充可选的输出参数。
 * 请注意，如果我们在重新散列哈希表的过程中，索引总是在第二个（新）哈希表的上下文中返回。
 * */
static long _dictKeyIndex(dict *d, const void *key, uint64_t hash, dictEntry **existing)
{
    unsigned long idx, table;
    dictEntry *he;
    if (existing) *existing = NULL;

    /* Expand the hash table if needed 如果需要，展开哈希表*/
    if (_dictExpandIfNeeded(d) == DICT_ERR)
        return -1;
    for (table = 0; table <= 1; table++) {
        idx = hash & DICTHT_SIZE_MASK(d->ht_size_exp[table]);
        /* Search if this slot does not already contain the given key */
        //搜索此插槽是否尚未包含给定的键
        he = d->ht_table[table][idx];
        while(he) {
            if (key==he->key || dictCompareKeys(d, key, he->key)) {
                if (existing) *existing = he;
                return -1;
            }
            he = he->next;
        }
        if (!dictIsRehashing(d)) break;
    }
    return idx;
}

void dictEmpty(dict *d, void(callback)(dict*)) {
    _dictClear(d,0,callback);
    _dictClear(d,1,callback);
    d->rehashidx = -1;
    d->pauserehash = 0;
}

void dictEnableResize(void) {
    dict_can_resize = 1;
}

void dictDisableResize(void) {
    dict_can_resize = 0;
}

uint64_t dictGetHash(dict *d, const void *key) {
    return dictHashKey(d, key);
}

/* Finds the dictEntry reference by using pointer and pre-calculated hash.
 * oldkey is a dead pointer and should not be accessed.
 * the hash value should be provided using dictGetHash.
 * no string / key comparison is performed.
 * return value is the reference to the dictEntry if found, or NULL if not found. */
/**
 * 使用指针和预先计算的哈希查找 dictEntry 引用。
 * oldkey 是一个死指针，不应访问。应使用 dictGetHash 提供哈希值。
 * 不执行字符串键比较。如果找到，则返回值是对 dictEntry 的引用，如果没有找到，则返回 NULL。
 * */
dictEntry **dictFindEntryRefByPtrAndHash(dict *d, const void *oldptr, uint64_t hash) {
    dictEntry *he, **heref;
    unsigned long idx, table;

    if (dictSize(d) == 0) return NULL; /* dict is empty */
    for (table = 0; table <= 1; table++) {
        idx = hash & DICTHT_SIZE_MASK(d->ht_size_exp[table]);
        heref = &d->ht_table[table][idx];
        he = *heref;
        while(he) {
            if (oldptr==he->key)
                return heref;
            heref = &he->next;
            he = *heref;
        }
        if (!dictIsRehashing(d)) return NULL;
    }
    return NULL;
}

/* ------------------------------- Debugging ---------------------------------*/

#define DICT_STATS_VECTLEN 50
size_t _dictGetStatsHt(char *buf, size_t bufsize, dict *d, int htidx) {
    unsigned long i, slots = 0, chainlen, maxchainlen = 0;
    unsigned long totchainlen = 0;
    unsigned long clvector[DICT_STATS_VECTLEN];
    size_t l = 0;

    if (d->ht_used[htidx] == 0) {
        return snprintf(buf,bufsize,
            "No stats available for empty dictionaries\n");
    }

    /* Compute stats. 计算统计数据。*/
    for (i = 0; i < DICT_STATS_VECTLEN; i++) clvector[i] = 0;
    for (i = 0; i < DICTHT_SIZE(d->ht_size_exp[htidx]); i++) {
        dictEntry *he;

        if (d->ht_table[htidx][i] == NULL) {
            clvector[0]++;
            continue;
        }
        slots++;
        /* For each hash entry on this slot... */
        //对于此插槽上的每个哈希条目...
        chainlen = 0;
        he = d->ht_table[htidx][i];
        while(he) {
            chainlen++;
            he = he->next;
        }
        clvector[(chainlen < DICT_STATS_VECTLEN) ? chainlen : (DICT_STATS_VECTLEN-1)]++;
        if (chainlen > maxchainlen) maxchainlen = chainlen;
        totchainlen += chainlen;
    }

    /* Generate human readable stats.生成人类可读的统计数据。 */
    l += snprintf(buf+l,bufsize-l,
        "Hash table %d stats (%s):\n"
        " table size: %lu\n"
        " number of elements: %lu\n"
        " different slots: %lu\n"
        " max chain length: %lu\n"
        " avg chain length (counted): %.02f\n"
        " avg chain length (computed): %.02f\n"
        " Chain length distribution:\n",
        htidx, (htidx == 0) ? "main hash table" : "rehashing target",
        DICTHT_SIZE(d->ht_size_exp[htidx]), d->ht_used[htidx], slots, maxchainlen,
        (float)totchainlen/slots, (float)d->ht_used[htidx]/slots);

    for (i = 0; i < DICT_STATS_VECTLEN-1; i++) {
        if (clvector[i] == 0) continue;
        if (l >= bufsize) break;
        l += snprintf(buf+l,bufsize-l,
            "   %ld: %ld (%.02f%%)\n",
            i, clvector[i], ((float)clvector[i]/DICTHT_SIZE(d->ht_size_exp[htidx]))*100);
    }

    /* Unlike snprintf(), return the number of characters actually written. */
    //与 snprintf() 不同，返回实际写入的字符数。
    if (bufsize) buf[bufsize-1] = '\0';
    return strlen(buf);
}

void dictGetStats(char *buf, size_t bufsize, dict *d) {
    size_t l;
    char *orig_buf = buf;
    size_t orig_bufsize = bufsize;

    l = _dictGetStatsHt(buf,bufsize,d,0);
    buf += l;
    bufsize -= l;
    if (dictIsRehashing(d) && bufsize > 0) {
        _dictGetStatsHt(buf,bufsize,d,1);
    }
    /* Make sure there is a NULL term at the end. */
    //确保最后有一个 NULL 术语。
    if (orig_bufsize) orig_buf[orig_bufsize-1] = '\0';
}

/* ------------------------------- Benchmark ---------------------------------*/

#ifdef REDIS_TEST
#include "testhelp.h"

#define UNUSED(V) ((void) V)

uint64_t hashCallback(const void *key) {
    return dictGenHashFunction((unsigned char*)key, strlen((char*)key));
}

int compareCallback(dict *d, const void *key1, const void *key2) {
    int l1,l2;
    UNUSED(d);

    l1 = strlen((char*)key1);
    l2 = strlen((char*)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

void freeCallback(dict *d, void *val) {
    UNUSED(d);

    zfree(val);
}

char *stringFromLongLong(long long value) {
    char buf[32];
    int len;
    char *s;

    len = sprintf(buf,"%lld",value);
    s = zmalloc(len+1);
    memcpy(s, buf, len);
    s[len] = '\0';
    return s;
}

dictType BenchmarkDictType = {
    hashCallback,
    NULL,
    NULL,
    compareCallback,
    freeCallback,
    NULL,
    NULL
};

#define start_benchmark() start = timeInMilliseconds()
#define end_benchmark(msg) do { \
    elapsed = timeInMilliseconds()-start; \
    printf(msg ": %ld items in %lld ms\n", count, elapsed); \
} while(0)

/* ./redis-server test dict [<count> | --accurate] */
int dictTest(int argc, char **argv, int flags) {
    long j;
    long long start, elapsed;
    dict *dict = dictCreate(&BenchmarkDictType);
    long count = 0;
    int accurate = (flags & REDIS_TEST_ACCURATE);

    if (argc == 4) {
        if (accurate) {
            count = 5000000;
        } else {
            count = strtol(argv[3],NULL,10);
        }
    } else {
        count = 5000;
    }

    start_benchmark();
    for (j = 0; j < count; j++) {
        int retval = dictAdd(dict,stringFromLongLong(j),(void*)j);
        assert(retval == DICT_OK);
    }
    end_benchmark("Inserting");
    assert((long)dictSize(dict) == count);

    /* Wait for rehashing. */
    while (dictIsRehashing(dict)) {
        dictRehashMilliseconds(dict,100);
    }

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(j);
        dictEntry *de = dictFind(dict,key);
        assert(de != NULL);
        zfree(key);
    }
    end_benchmark("Linear access of existing elements");

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(j);
        dictEntry *de = dictFind(dict,key);
        assert(de != NULL);
        zfree(key);
    }
    end_benchmark("Linear access of existing elements (2nd round)");

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(rand() % count);
        dictEntry *de = dictFind(dict,key);
        assert(de != NULL);
        zfree(key);
    }
    end_benchmark("Random access of existing elements");

    start_benchmark();
    for (j = 0; j < count; j++) {
        dictEntry *de = dictGetRandomKey(dict);
        assert(de != NULL);
    }
    end_benchmark("Accessing random keys");

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(rand() % count);
        key[0] = 'X';
        dictEntry *de = dictFind(dict,key);
        assert(de == NULL);
        zfree(key);
    }
    end_benchmark("Accessing missing");

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(j);
        int retval = dictDelete(dict,key);
        assert(retval == DICT_OK);
        key[0] += 17; /* Change first number to letter. */
        retval = dictAdd(dict,key,(void*)j);
        assert(retval == DICT_OK);
    }
    end_benchmark("Removing and adding");
    dictRelease(dict);
    return 0;
}
#endif
