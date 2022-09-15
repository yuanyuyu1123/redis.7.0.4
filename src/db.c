/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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
#include "cluster.h"
#include "atomicvar.h"
#include "latency.h"
#include "script.h"
#include "functions.h"

#include <signal.h>
#include <ctype.h>

/*-----------------------------------------------------------------------------
 * C-level DB API
 *----------------------------------------------------------------------------*/

int expireIfNeeded(redisDb *db, robj *key, int force_delete_expired);
int keyIsExpired(redisDb *db, robj *key);

/* Update LFU when an object is accessed.
 * Firstly, decrement the counter if the decrement time is reached.
 * Then logarithmically increment the counter, and update the access time. */
//访问对象时更新 LFU。首先，如果达到递减时间，则递减计数器。然后对数递增计数器，并更新访问时间。
void updateLFU(robj *val) {
    unsigned long counter = LFUDecrAndReturn(val);
    counter = LFULogIncr(counter);
    val->lru = (LFUGetTimeInMinutes()<<8) | counter;
}

/* Lookup a key for read or write operations, or return NULL if the key is not
 * found in the specified DB. This function implements the functionality of
 * lookupKeyRead(), lookupKeyWrite() and their ...WithFlags() variants.
 *
 * Side-effects of calling this function:
 *
 * 1. A key gets expired if it reached it's TTL.
 * 2. The key's last access time is updated.
 * 3. The global keys hits/misses stats are updated (reported in INFO).
 * 4. If keyspace notifications are enabled, a "keymiss" notification is fired.
 *
 * Flags change the behavior of this command:
 *
 *  LOOKUP_NONE (or zero): No special flags are passed.
 *  LOOKUP_NOTOUCH: Don't alter the last access time of the key.
 *  LOOKUP_NONOTIFY: Don't trigger keyspace event on key miss.
 *  LOOKUP_NOSTATS: Don't increment key hits/misses counters.
 *  LOOKUP_WRITE: Prepare the key for writing (delete expired keys even on
 *                replicas, use separate keyspace stats and events (TODO)).
 *
 * Note: this function also returns NULL if the key is logically expired but
 * still existing, in case this is a replica and the LOOKUP_WRITE is not set.
 * Even if the key expiry is master-driven, we can correctly report a key is
 * expired on replicas even if the master is lagging expiring our key via DELs
 * in the replication link. */
/**
 * 查找用于读取或写入操作的键，如果在指定的 DB 中找不到该键，则返回 NULL。
 * 此函数实现了lookupKeyRead()、lookupKeyWrite() 及其...WithFlags() 变体的功能。
 * 调用此函数的副作用：
 *   1. 如果密钥达到 TTL，则密钥过期。
 *   2. 更新密钥的最后访问时间。
 *   3. 更新全局键命中率统计信息（在 INFO 中报告）。
 *   4. 如果启用了键空间通知，则会触发“keymiss”通知。
 * 标志更改此命令的行为：
 *   LOOKUP_NONE（或零）：不传递特殊标志。
 *   LOOKUP_NOTOUCH：不要更改密钥的最后访问时间。
 *   LOOKUP_NONOTIFY：不触发键未命中的键空间事件。
 *   LOOKUP_NOSTATS：不增加键命中计数器。
 *   LOOKUP_WRITE：准备要写入的密钥（即使在副本上也删除过期的密钥，使用单独的密钥空间统计信息和事件（TODO））。
 * 注意：如果密钥在逻辑上已过期但仍然存在，则此函数也会返回 NULL，以防这是副本且未设置 LOOKUP_WRITE。
 * 即使密钥过期是主驱动的，我们也可以正确地报告副本上的密钥已过期，即使主通过复制链接中的 DEL 延迟我们的密钥过期也是如此。
 * */
robj *lookupKey(redisDb *db, robj *key, int flags) {
    dictEntry *de = dictFind(db->dict,key->ptr);
    robj *val = NULL;
    if (de) {
        val = dictGetVal(de);
        /* Forcing deletion of expired keys on a replica makes the replica
         * inconsistent with the master. We forbid it on readonly replicas, but
         * we have to allow it on writable replicas to make write commands
         * behave consistently.
         *
         * It's possible that the WRITE flag is set even during a readonly
         * command, since the command may trigger events that cause modules to
         * perform additional writes. */
        /**
         * 强制删除副本上的过期键会使副本与主服务器不一致。
         * 我们在只读副本上禁止它，但我们必须在可写副本上允许它，以使写入命令的行为一致。
         * 即使在只读命令期间也可能设置 WRITE 标志，因为该命令可能会触发导致模块执行额外写入的事件。
         * */
        int is_ro_replica = server.masterhost && server.repl_slave_ro;
        int force_delete_expired = flags & LOOKUP_WRITE && !is_ro_replica;
        if (expireIfNeeded(db, key, force_delete_expired)) {
            /* The key is no longer valid. 密钥不再有效。*/
            val = NULL;
        }
    }

    if (val) {
        /* Update the access time for the ageing algorithm.
         * Don't do it if we have a saving child, as this will trigger
         * a copy on write madness. */
        //更新老化算法的访问时间。如果我们有一个正在拯救的孩子，就不要这样做，因为这会触发一个写疯狂的副本。
        if (!hasActiveChildProcess() && !(flags & LOOKUP_NOTOUCH)){
            if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
                updateLFU(val);
            } else {
                val->lru = LRU_CLOCK();
            }
        }

        if (!(flags & (LOOKUP_NOSTATS | LOOKUP_WRITE)))
            server.stat_keyspace_hits++;
        /* TODO: Use separate hits stats for WRITE */
    } else {
        if (!(flags & (LOOKUP_NONOTIFY | LOOKUP_WRITE)))
            notifyKeyspaceEvent(NOTIFY_KEY_MISS, "keymiss", key, db->id);
        if (!(flags & (LOOKUP_NOSTATS | LOOKUP_WRITE)))
            server.stat_keyspace_misses++;
        /* TODO: Use separate misses stats and notify event for WRITE */
    }

    return val;
}

/* Lookup a key for read operations, or return NULL if the key is not found
 * in the specified DB.
 *
 * This API should not be used when we write to the key after obtaining
 * the object linked to the key, but only for read only operations.
 *
 * This function is equivalent to lookupKey(). The point of using this function
 * rather than lookupKey() directly is to indicate that the purpose is to read
 * the key. */
/**
 * 查找读取操作的键，如果在指定的 DB 中找不到该键，则返回 NULL。
 * 这个API不应该在我们获取到key所链接的对象后写入key的时候使用，而只能用于只读操作。
 * 这个函数相当于lookupKey()。使用这个函数而不是直接使用lookupKey() 的目的是为了表明目的是读取密钥。
 * */
robj *lookupKeyReadWithFlags(redisDb *db, robj *key, int flags) {
    serverAssert(!(flags & LOOKUP_WRITE));
    return lookupKey(db, key, flags);
}

/* Like lookupKeyReadWithFlags(), but does not use any flag, which is the
 * common case. */
//与lookupKeyReadWithFlags() 类似，但不使用任何标志，这是常见的情况。
robj *lookupKeyRead(redisDb *db, robj *key) {
    return lookupKeyReadWithFlags(db,key,LOOKUP_NONE);
}

/* Lookup a key for write operations, and as a side effect, if needed, expires
 * the key if its TTL is reached. It's equivalent to lookupKey() with the
 * LOOKUP_WRITE flag added.
 *
 * Returns the linked value object if the key exists or NULL if the key
 * does not exist in the specified DB. */
/**
 * 查找写操作的键，如果需要，如果达到其 TTL，则作为副作用，使键过期。
 * 它相当于添加了 LOOKUP_WRITE 标志的 lookupKey()。
 * 如果键存在，则返回链接的值对象；如果指定的 DB 中不存在键，则返回 NULL。
 * */
robj *lookupKeyWriteWithFlags(redisDb *db, robj *key, int flags) {
    return lookupKey(db, key, flags | LOOKUP_WRITE);
}

robj *lookupKeyWrite(redisDb *db, robj *key) {
    return lookupKeyWriteWithFlags(db, key, LOOKUP_NONE);
}

robj *lookupKeyReadOrReply(client *c, robj *key, robj *reply) {
    robj *o = lookupKeyRead(c->db, key);
    if (!o) addReplyOrErrorObject(c, reply);
    return o;
}

robj *lookupKeyWriteOrReply(client *c, robj *key, robj *reply) {
    robj *o = lookupKeyWrite(c->db, key);
    if (!o) addReplyOrErrorObject(c, reply);
    return o;
}

/* Add the key to the DB. It's up to the caller to increment the reference
 * counter of the value if needed.
 *
 * The program is aborted if the key already exists. */
//将密钥添加到数据库。如果需要，由调用者增加值的引用计数器。如果密钥已经存在，程序将中止。
void dbAdd(redisDb *db, robj *key, robj *val) {
    sds copy = sdsdup(key->ptr);
    dictEntry *de = dictAddRaw(db->dict, copy, NULL);
    serverAssertWithInfo(NULL, key, de != NULL);
    dictSetVal(db->dict, de, val);
    signalKeyAsReady(db, key, val->type);
    if (server.cluster_enabled) slotToKeyAddEntry(de, db);
    notifyKeyspaceEvent(NOTIFY_NEW,"new",key,db->id);
}

/* This is a special version of dbAdd() that is used only when loading
 * keys from the RDB file: the key is passed as an SDS string that is
 * retained by the function (and not freed by the caller).
 *
 * Moreover this function will not abort if the key is already busy, to
 * give more control to the caller, nor will signal the key as ready
 * since it is not useful in this context.
 *
 * The function returns 1 if the key was added to the database, taking
 * ownership of the SDS string, otherwise 0 is returned, and is up to the
 * caller to free the SDS string. */
/**
 * 这是 dbAdd() 的特殊版本，仅在从 RDB 文件加载密钥时使用：密钥作为 SDS 字符串传递，由函数保留（而不是由调用者释放）。
 * 此外，如果键已经忙，则此函数不会中止，以便为调用者提供更多控制权，也不会发出键就绪的信号，因为它在这种情况下没有用处。
 * 如果密钥已添加到数据库，则该函数返回 1，获取 SDS 字符串的所有权，否则返回 0，并由调用者释放 SDS 字符串。
 * */
int dbAddRDBLoad(redisDb *db, sds key, robj *val) {
    dictEntry *de = dictAddRaw(db->dict, key, NULL);
    if (de == NULL) return 0;
    dictSetVal(db->dict, de, val);
    if (server.cluster_enabled) slotToKeyAddEntry(de, db);
    return 1;
}

/* Overwrite an existing key with a new value. Incrementing the reference
 * count of the new value is up to the caller.
 * This function does not modify the expire time of the existing key.
 *
 * The program is aborted if the key was not already present. */
//用新值覆盖现有键。增加新值的引用计数取决于调用者。此函数不会修改现有密钥的过期时间。如果密钥不存在，程序将中止。
void dbOverwrite(redisDb *db, robj *key, robj *val) {
    dictEntry *de = dictFind(db->dict,key->ptr);

    serverAssertWithInfo(NULL,key,de != NULL);
    dictEntry auxentry = *de;
    robj *old = dictGetVal(de);
    if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
        val->lru = old->lru;
    }
    /* Although the key is not really deleted from the database, we regard 
     * overwrite as two steps of unlink+add, so we still need to call the unlink
     * callback of the module. */
    //虽然key并没有真正从数据库中删除，但是我们把overwrite看成是unlink+add两步，所以还是需要调用模块的unlink回调。
    moduleNotifyKeyUnlink(key,old,db->id);
    /* We want to try to unblock any client using a blocking XREADGROUP */
    //我们想尝试使用阻塞 XREADGROUP 解除对任何客户端的阻塞
    if (old->type == OBJ_STREAM)
        signalKeyAsReady(db,key,old->type);
    dictSetVal(db->dict, de, val);

    if (server.lazyfree_lazy_server_del) {
        freeObjAsync(key,old,db->id);
        dictSetVal(db->dict, &auxentry, NULL);
    }

    dictFreeVal(db->dict, &auxentry);
}

/* High level Set operation. This function can be used in order to set
 * a key, whatever it was existing or not, to a new object.
 *
 * 1) The ref count of the value object is incremented.
 * 2) clients WATCHing for the destination key notified.
 * 3) The expire time of the key is reset (the key is made persistent),
 *    unless 'SETKEY_KEEPTTL' is enabled in flags.
 * 4) The key lookup can take place outside this interface outcome will be
 *    delivered with 'SETKEY_ALREADY_EXIST' or 'SETKEY_DOESNT_EXIST'
 *
 * All the new keys in the database should be created via this interface.
 * The client 'c' argument may be set to NULL if the operation is performed
 * in a context where there is no clear client performing the operation. */
/**
 * 高电平设置操作。此函数可用于将键设置为新对象，无论它是否存在。
 *   1) 值对象的引用计数递增。
 *   2) 客户端正在等待通知的目标密钥。
 *   3) 重置密钥的过期时间（使密钥持久化），除非在标志中启用了“SETKEY_KEEPTTL”。
 *   4) 可以在此接口之外进行键查找结果将通过“SETKEY_ALREADY_EXIST”或“SETKEY_DOESNT_EXIST”
 * 传递数据库中的所有新键都应通过该接口创建。如果在没有明确的客户端执行操作的上下文中执行操作，则客户端“c”参数可以设置为 NULL。
 * */
void setKey(client *c, redisDb *db, robj *key, robj *val, int flags) {
    int keyfound = 0;

    if (flags & SETKEY_ALREADY_EXIST)
        keyfound = 1;
    else if (!(flags & SETKEY_DOESNT_EXIST))
        keyfound = (lookupKeyWrite(db,key) != NULL);

    if (!keyfound) {
        dbAdd(db,key,val);
    } else {
        dbOverwrite(db,key,val);
    }
    incrRefCount(val);
    if (!(flags & SETKEY_KEEPTTL)) removeExpire(db,key);
    if (!(flags & SETKEY_NO_SIGNAL)) signalModifiedKey(c,db,key);
}

/* Return a random key, in form of a Redis object.
 * If there are no keys, NULL is returned.
 *
 * The function makes sure to return keys not already expired. */
//以 Redis 对象的形式返回一个随机键。如果没有键，则返回 NULL。该函数确保返回尚未过期的密钥。
robj *dbRandomKey(redisDb *db) {
    dictEntry *de;
    int maxtries = 100;
    int allvolatile = dictSize(db->dict) == dictSize(db->expires);

    while(1) {
        sds key;
        robj *keyobj;

        de = dictGetFairRandomKey(db->dict);
        if (de == NULL) return NULL;

        key = dictGetKey(de);
        keyobj = createStringObject(key,sdslen(key));
        if (dictFind(db->expires,key)) {
            if (allvolatile && server.masterhost && --maxtries == 0) {
                /* If the DB is composed only of keys with an expire set,
                 * it could happen that all the keys are already logically
                 * expired in the slave, so the function cannot stop because
                 * expireIfNeeded() is false, nor it can stop because
                 * dictGetFairRandomKey() returns NULL (there are keys to return).
                 * To prevent the infinite loop we do some tries, but if there
                 * are the conditions for an infinite loop, eventually we
                 * return a key name that may be already expired. */
                /**
                 * 如果数据库仅由设置了过期的键组成，则可能会发生从服务器中所有键在逻辑上已经过期的情况，
                 * 因此该函数无法停止，因为 expireIfNeeded() 为假，也无法停止，
                 * 因为 dictGetFairRandomKey() 返回 NULL （有返回键）。
                 * 为了防止无限循环，我们做了一些尝试，但是如果有无限循环的条件，最终我们会返回一个可能已经过期的键名。
                 * */
                return keyobj;
            }
            if (expireIfNeeded(db,keyobj,0)) {
                decrRefCount(keyobj);
                continue; /* search for another key. This expired. 搜索另一个密钥。这个过期了。*/
            }
        }
        return keyobj;
    }
}

/* Helper for sync and async delete. 同步和异步删除的助手。*/
static int dbGenericDelete(redisDb *db, robj *key, int async) {
    /* Deleting an entry from the expires dict will not free the sds of
     * the key, because it is shared with the main dictionary. */
    //从 expires 字典中删除条目不会释放密钥的 sds，因为它与主字典共享。
    if (dictSize(db->expires) > 0) dictDelete(db->expires,key->ptr);
    dictEntry *de = dictUnlink(db->dict,key->ptr);
    if (de) {
        robj *val = dictGetVal(de);
        /* Tells the module that the key has been unlinked from the database. */
        //告诉模块密钥已从数据库中取消链接。
        moduleNotifyKeyUnlink(key,val,db->id);
        /* We want to try to unblock any client using a blocking XREADGROUP */
        //我们想尝试使用阻塞 XREADGROUP 解除对任何客户端的阻塞
        if (val->type == OBJ_STREAM)
            signalKeyAsReady(db,key,val->type);
        if (async) {
            freeObjAsync(key, val, db->id);
            dictSetVal(db->dict, de, NULL);
        }
        if (server.cluster_enabled) slotToKeyDelEntry(de, db);
        dictFreeUnlinkedEntry(db->dict,de);
        return 1;
    } else {
        return 0;
    }
}

/* Delete a key, value, and associated expiration entry if any, from the DB */
//从数据库中删除键、值和关联的过期条目（如果有）
int dbSyncDelete(redisDb *db, robj *key) {
    return dbGenericDelete(db, key, 0);
}

/* Delete a key, value, and associated expiration entry if any, from the DB. If
 * the value consists of many allocations, it may be freed asynchronously. */
/**从数据库中删除键、值和关联的过期条目（如果有）。如果该值包含许多分配，则可以异步释放它。*/
int dbAsyncDelete(redisDb *db, robj *key) {
    return dbGenericDelete(db, key, 1);
}

/* This is a wrapper whose behavior depends on the Redis lazy free
 * configuration. Deletes the key synchronously or asynchronously. */
/**这是一个包装器，其行为取决于 Redis 惰性配置。同步或异步删除密钥。*/
int dbDelete(redisDb *db, robj *key) {
    return dbGenericDelete(db, key, server.lazyfree_lazy_server_del);
}

/* Prepare the string object stored at 'key' to be modified destructively
 * to implement commands like SETBIT or APPEND.
 *
 * An object is usually ready to be modified unless one of the two conditions
 * are true:
 *
 * 1) The object 'o' is shared (refcount > 1), we don't want to affect
 *    other users.
 * 2) The object encoding is not "RAW".
 *
 * If the object is found in one of the above conditions (or both) by the
 * function, an unshared / not-encoded copy of the string object is stored
 * at 'key' in the specified 'db'. Otherwise the object 'o' itself is
 * returned.
 *
 * USAGE:
 *
 * The object 'o' is what the caller already obtained by looking up 'key'
 * in 'db', the usage pattern looks like this:
 *
 * o = lookupKeyWrite(db,key);
 * if (checkType(c,o,OBJ_STRING)) return;
 * o = dbUnshareStringValue(db,key,o);
 *
 * At this point the caller is ready to modify the object, for example
 * using an sdscat() call to append some data, or anything else.
 */
/**
 * 准备存储在“key”中的字符串对象，以进行破坏性修改以实现 SETBIT 或 APPEND 等命令。
 * 一个对象通常可以被修改，除非两个条件之一为真：
 *   1) 对象 'o' 是共享的 (refcount > 1)，我们不想影响其他用户。
 *   2）对象编码不是“RAW”。
 * 如果函数在上述条件之一（或两者）中找到对象，则将字符串对象的非共享未编码副本存储在指定“db”中的“key”处。
 * 否则返回对象 'o' 本身。用法：对象 'o' 是调用者通过在 'db' 中查找 'key' 已经获得的对象，使用模式如下所示：
 *   o = lookupKeyWrite(db,key);
 *   if (checkType(c,o,OBJ_STRING)) return；
 *   o = dbUnshareStringValue(db,key,o);
 * 此时，调用者已准备好修改对象，例如使用 sdscat() 调用来附加一些数据或其他任何内容。
 * */
robj *dbUnshareStringValue(redisDb *db, robj *key, robj *o) {
    serverAssert(o->type == OBJ_STRING);
    if (o->refcount != 1 || o->encoding != OBJ_ENCODING_RAW) {
        robj *decoded = getDecodedObject(o);
        o = createRawStringObject(decoded->ptr, sdslen(decoded->ptr));
        decrRefCount(decoded);
        dbOverwrite(db,key,o);
    }
    return o;
}

/* Remove all keys from the database(s) structure. The dbarray argument
 * may not be the server main DBs (could be a temporary DB).
 *
 * The dbnum can be -1 if all the DBs should be emptied, or the specified
 * DB index if we want to empty only a single database.
 * The function returns the number of keys removed from the database(s). */
/**
 * 从数据库结构中删除所有键。 dbarray 参数可能不是服务器主数据库（可能是临时数据库）。
 * 如果应该清空所有数据库，则 dbnum 可以是 -1，如果我们只想清空单个数据库，则可以是指定的数据库索引。
 * 该函数返回从数据库中删除的键数。
 * */
long long emptyDbStructure(redisDb *dbarray, int dbnum, int async,
                           void(callback)(dict*))
{
    long long removed = 0;
    int startdb, enddb;

    if (dbnum == -1) {
        startdb = 0;
        enddb = server.dbnum-1;
    } else {
        startdb = enddb = dbnum;
    }

    for (int j = startdb; j <= enddb; j++) {
        removed += dictSize(dbarray[j].dict);
        if (async) {
            emptyDbAsync(&dbarray[j]);
        } else {
            dictEmpty(dbarray[j].dict,callback);
            dictEmpty(dbarray[j].expires,callback);
        }
        /* Because all keys of database are removed, reset average ttl. */
        //因为数据库的所有键都被删除了，所以重置平均 ttl。
        dbarray[j].avg_ttl = 0;
        dbarray[j].expires_cursor = 0;
    }

    return removed;
}

/* Remove all data (keys and functions) from all the databases in a
 * Redis server. If callback is given the function is called from
 * time to time to signal that work is in progress.
 *
 * The dbnum can be -1 if all the DBs should be flushed, or the specified
 * DB number if we want to flush only a single Redis database number.
 *
 * Flags are be EMPTYDB_NO_FLAGS if no special flags are specified or
 * EMPTYDB_ASYNC if we want the memory to be freed in a different thread
 * and the function to return ASAP. EMPTYDB_NOFUNCTIONS can also be set
 * to specify that we do not want to delete the functions.
 *
 * On success the function returns the number of keys removed from the
 * database(s). Otherwise -1 is returned in the specific case the
 * DB number is out of range, and errno is set to EINVAL. */
/**
 * 从 Redis 服务器中的所有数据库中删除所有数据（键和函数）。如果给定回调，则不时调用该函数以指示工作正在进行中。
 * 如果应该刷新所有 DB，则 dbnum 可以是 -1，如果我们只想刷新单个 Redis 数据库编号，则可以是指定的 DB 编号。
 * 如果没有指定特殊标志，则标志为 EMPTYDB_NO_FLAGS；如果我们希望在不同线程中释放内存并且函数尽快返回，
 * 则标志为 EMPTYDB_ASYNC。也可以设置 EMPTYDB_NOFUNCTIONS 来指定我们不想删除的函数。
 * 成功时，该函数返回从数据库中删除的键数。否则，在 DB 编号超出范围的特定情况下返回 -1，并将 errno 设置为 EINVAL。
 * */
long long emptyData(int dbnum, int flags, void(callback)(dict*)) {
    int async = (flags & EMPTYDB_ASYNC);
    int with_functions = !(flags & EMPTYDB_NOFUNCTIONS);
    RedisModuleFlushInfoV1 fi = {REDISMODULE_FLUSHINFO_VERSION,!async,dbnum};
    long long removed = 0;

    if (dbnum < -1 || dbnum >= server.dbnum) {
        errno = EINVAL;
        return -1;
    }

    /* Fire the flushdb modules event. 触发 flushdb 模块事件。*/
    moduleFireServerEvent(REDISMODULE_EVENT_FLUSHDB,
                          REDISMODULE_SUBEVENT_FLUSHDB_START,
                          &fi);

    /* Make sure the WATCHed keys are affected by the FLUSH* commands.
     * Note that we need to call the function while the keys are still
     * there. */
    /**确保 WATCHed 键受 FLUSH 命令的影响。请注意，我们需要在键仍然存在时调用该函数。*/
    signalFlushedDb(dbnum, async);

    /* Empty redis database structure. */
    /**空的 redis 数据库结构。*/
    removed = emptyDbStructure(server.db, dbnum, async, callback);

    /* Flush slots to keys map if enable cluster, we can flush entire
     * slots to keys map whatever dbnum because only support one DB
     * in cluster mode. */
    /**如果启用集群，将槽刷新到键映射，我们可以将整个槽刷新到键映射任何 dbnum，因为在集群模式下仅支持一个 DB。*/
    if (server.cluster_enabled) slotToKeyFlush(server.db);

    if (dbnum == -1) flushSlaveKeysWithExpireList();

    if (with_functions) {
        serverAssert(dbnum == -1);
        functionsLibCtxClearCurrent(async);
    }

    /* Also fire the end event. Note that this event will fire almost
     * immediately after the start event if the flush is asynchronous. */
    //同时触发结束事件。请注意，如果刷新是异步的，则此事件几乎会在启动事件之后立即触发。
    moduleFireServerEvent(REDISMODULE_EVENT_FLUSHDB,
                          REDISMODULE_SUBEVENT_FLUSHDB_END,
                          &fi);

    return removed;
}

/* Initialize temporary db on replica for use during diskless replication. */
//初始化副本上的临时数据库以在无盘复制期间使用。
redisDb *initTempDb(void) {
    redisDb *tempDb = zcalloc(sizeof(redisDb)*server.dbnum);
    for (int i=0; i<server.dbnum; i++) {
        tempDb[i].dict = dictCreate(&dbDictType);
        tempDb[i].expires = dictCreate(&dbExpiresDictType);
        tempDb[i].slots_to_keys = NULL;
    }

    if (server.cluster_enabled) {
        /* Prepare temp slot to key map to be written during async diskless replication. */
        //准备临时槽到要在异步无盘复制期间写入的键映射。
        slotToKeyInit(tempDb);
    }

    return tempDb;
}

/* Discard tempDb, this can be slow (similar to FLUSHALL), but it's always async. */
//丢弃 tempDb，这可能很慢（类似于 FLUSHALL），但它始终是异步的。
void discardTempDb(redisDb *tempDb, void(callback)(dict*)) {
    int async = 1;

    /* Release temp DBs. 释放临时数据库。*/
    emptyDbStructure(tempDb, -1, async, callback);
    for (int i=0; i<server.dbnum; i++) {
        dictRelease(tempDb[i].dict);
        dictRelease(tempDb[i].expires);
    }

    if (server.cluster_enabled) {
        /* Release temp slot to key map. 将临时槽释放到键映射。*/
        slotToKeyDestroy(tempDb);
    }

    zfree(tempDb);
}

int selectDb(client *c, int id) {
    if (id < 0 || id >= server.dbnum)
        return C_ERR;
    c->db = &server.db[id];
    return C_OK;
}

long long dbTotalServerKeyCount() {
    long long total = 0;
    int j;
    for (j = 0; j < server.dbnum; j++) {
        total += dictSize(server.db[j].dict);
    }
    return total;
}

/*-----------------------------------------------------------------------------
 * Hooks for key space changes.
 *
 * Every time a key in the database is modified the function
 * signalModifiedKey() is called.
 *
 * Every time a DB is flushed the function signalFlushDb() is called.
 * 键空间更改的挂钩。
 * 每次修改数据库中的键时，都会调用函数 signalModifiedKey()。
 * 每次刷新数据库时，都会调用函数 signalFlushDb()。
 *----------------------------------------------------------------------------*/

/* Note that the 'c' argument may be NULL if the key was modified out of
 * a context of a client. */
//请注意，如果密钥是在客户端上下文之外修改的，则“c”参数可能为 NULL。
void signalModifiedKey(client *c, redisDb *db, robj *key) {
    touchWatchedKey(db,key);
    trackingInvalidateKey(c,key,1);
}

void signalFlushedDb(int dbid, int async) {
    int startdb, enddb;
    if (dbid == -1) {
        startdb = 0;
        enddb = server.dbnum-1;
    } else {
        startdb = enddb = dbid;
    }

    for (int j = startdb; j <= enddb; j++) {
        scanDatabaseForDeletedStreams(&server.db[j], NULL);
        touchAllWatchedKeysInDb(&server.db[j], NULL);
    }

    trackingInvalidateKeysOnFlush(async);

    /* Changes in this method may take place in swapMainDbWithTempDb as well,
     * where we execute similar calls, but with subtle differences as it's
     * not simply flushing db. */
    /**此方法的更改也可能发生在 swapMainDbWithTempDb 中，我们执行类似的调用，但有细微的差别，因为它不仅仅是刷新 db。*/
}

/*-----------------------------------------------------------------------------
 * Type agnostic commands operating on the key space 键入在键空间上操作的不可知命令
 *----------------------------------------------------------------------------*/

/* Return the set of flags to use for the emptyDb() call for FLUSHALL
 * and FLUSHDB commands.
 *
 * sync: flushes the database in an sync manner.
 * async: flushes the database in an async manner.
 * no option: determine sync or async according to the value of lazyfree-lazy-user-flush.
 *
 * On success C_OK is returned and the flags are stored in *flags, otherwise
 * C_ERR is returned and the function sends an error to the client. */
/**
 * 返回用于 FLUSHALL 和 FLUSHDB 命令的 emptyDb() 调用的标志集。
 *   sync：以同步方式刷新数据库。
 *   async：以异步方式刷新数据库。
 *   no option：根据lazyfree-lazy-user-flush的值来判断是同步还是异步。
 * 成功时返回 C_OK 并将标志存储在标志中，否则返回 C_ERR 并且函数向客户端发送错误。
 * */
int getFlushCommandFlags(client *c, int *flags) {
    /* Parse the optional ASYNC option. */
    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"sync")) {
        *flags = EMPTYDB_NO_FLAGS;
    } else if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"async")) {
        *flags = EMPTYDB_ASYNC;
    } else if (c->argc == 1) {
        *flags = server.lazyfree_lazy_user_flush ? EMPTYDB_ASYNC : EMPTYDB_NO_FLAGS;
    } else {
        addReplyErrorObject(c,shared.syntaxerr);
        return C_ERR;
    }
    return C_OK;
}

/* Flushes the whole server data set. 刷新整个服务器数据集。*/
void flushAllDataAndResetRDB(int flags) {
    server.dirty += emptyData(-1,flags,NULL);
    if (server.child_type == CHILD_TYPE_RDB) killRDBChild();
    if (server.saveparamslen > 0) {
        rdbSaveInfo rsi, *rsiptr;
        rsiptr = rdbPopulateSaveInfo(&rsi);
        rdbSave(SLAVE_REQ_NONE,server.rdb_filename,rsiptr);
    }

#if defined(USE_JEMALLOC)
    /* jemalloc 5 doesn't release pages back to the OS when there's no traffic.
     * for large databases, flushdb blocks for long anyway, so a bit more won't
     * harm and this way the flush and purge will be synchronous. */
    /**当没有流量时，jemalloc 5 不会将页面释放回操作系统。对于大型数据库，flushdb 无论如何都会阻塞很长时间，
     * 所以多一点不会有害，这样刷新和清除将是同步的。*/
    if (!(flags & EMPTYDB_ASYNC))
        jemalloc_purge();
#endif
}

/* FLUSHDB [ASYNC]
 *
 * Flushes the currently SELECTed Redis DB. 刷新当前 SELECTed Redis DB。*/
void flushdbCommand(client *c) {
    int flags;

    if (getFlushCommandFlags(c,&flags) == C_ERR) return;
    /* flushdb should not flush the functions */
    //flushdb 不应该刷新函数
    server.dirty += emptyData(c->db->id,flags | EMPTYDB_NOFUNCTIONS,NULL);

    /* Without the forceCommandPropagation, when DB was already empty,
     * FLUSHDB will not be replicated nor put into the AOF. */
    //如果没有 forceCommandPropagation，当 DB 已经为空时，FLUSHDB 将不会被复制也不会放入 AOF。
    forceCommandPropagation(c, PROPAGATE_REPL | PROPAGATE_AOF);

    addReply(c,shared.ok);

#if defined(USE_JEMALLOC)
    /* jemalloc 5 doesn't release pages back to the OS when there's no traffic.
     * for large databases, flushdb blocks for long anyway, so a bit more won't
     * harm and this way the flush and purge will be synchronous. */
    /**当没有流量时，jemalloc 5 不会将页面释放回操作系统。对于大型数据库，flushdb 无论如何都会阻塞很长时间，
     * 所以多一点不会有害，这样刷新和清除将是同步的。*/
    if (!(flags & EMPTYDB_ASYNC))
        jemalloc_purge();
#endif
}

/* FLUSHALL [ASYNC]
 *
 * Flushes the whole server data set. 刷新整个服务器数据集。*/
void flushallCommand(client *c) {
    int flags;
    if (getFlushCommandFlags(c,&flags) == C_ERR) return;
    /* flushall should not flush the functions    flushall 不应该刷新函数*/
    flushAllDataAndResetRDB(flags | EMPTYDB_NOFUNCTIONS);

    /* Without the forceCommandPropagation, when DBs were already empty,
     * FLUSHALL will not be replicated nor put into the AOF. */
    //如果没有 forceCommandPropagation，当 DB 已经为空时，FLUSHALL 将不会被复制也不会放入 AOF。
    forceCommandPropagation(c, PROPAGATE_REPL | PROPAGATE_AOF);

    addReply(c,shared.ok);
}

/* This command implements DEL and LAZYDEL. 此命令实现 DEL 和 LAZYDEL。*/
void delGenericCommand(client *c, int lazy) {
    int numdel = 0, j;

    for (j = 1; j < c->argc; j++) {
        expireIfNeeded(c->db,c->argv[j],0);
        int deleted  = lazy ? dbAsyncDelete(c->db,c->argv[j]) :
                              dbSyncDelete(c->db,c->argv[j]);
        if (deleted) {
            signalModifiedKey(c,c->db,c->argv[j]);
            notifyKeyspaceEvent(NOTIFY_GENERIC,
                "del",c->argv[j],c->db->id);
            server.dirty++;
            numdel++;
        }
    }
    addReplyLongLong(c,numdel);
}

void delCommand(client *c) {
    delGenericCommand(c,server.lazyfree_lazy_user_del);
}

void unlinkCommand(client *c) {
    delGenericCommand(c,1);
}

/* EXISTS key1 key2 ... key_N.
 * Return value is the number of keys existing. 返回值是现有键的数量。*/
void existsCommand(client *c) {
    long long count = 0;
    int j;

    for (j = 1; j < c->argc; j++) {
        if (lookupKeyReadWithFlags(c->db,c->argv[j],LOOKUP_NOTOUCH)) count++;
    }
    addReplyLongLong(c,count);
}

void selectCommand(client *c) {
    int id;

    if (getIntFromObjectOrReply(c, c->argv[1], &id, NULL) != C_OK)
        return;

    if (server.cluster_enabled && id != 0) {
        addReplyError(c,"SELECT is not allowed in cluster mode");
        return;
    }
    if (selectDb(c,id) == C_ERR) {
        addReplyError(c,"DB index is out of range");
    } else {
        addReply(c,shared.ok);
    }
}

void randomkeyCommand(client *c) {
    robj *key;

    if ((key = dbRandomKey(c->db)) == NULL) {
        addReplyNull(c);
        return;
    }

    addReplyBulk(c,key);
    decrRefCount(key);
}

void keysCommand(client *c) {
    dictIterator *di;
    dictEntry *de;
    sds pattern = c->argv[1]->ptr;
    int plen = sdslen(pattern), allkeys;
    unsigned long numkeys = 0;
    void *replylen = addReplyDeferredLen(c);

    di = dictGetSafeIterator(c->db->dict);
    allkeys = (pattern[0] == '*' && plen == 1);
    while((de = dictNext(di)) != NULL) {
        sds key = dictGetKey(de);
        robj *keyobj;

        if (allkeys || stringmatchlen(pattern,plen,key,sdslen(key),0)) {
            keyobj = createStringObject(key,sdslen(key));
            if (!keyIsExpired(c->db,keyobj)) {
                addReplyBulk(c,keyobj);
                numkeys++;
            }
            decrRefCount(keyobj);
        }
    }
    dictReleaseIterator(di);
    setDeferredArrayLen(c,replylen,numkeys);
}

/* This callback is used by scanGenericCommand in order to collect elements
 * returned by the dictionary iterator into a list. */
//scanGenericCommand 使用此回调将字典迭代器返回的元素收集到列表中。
void scanCallback(void *privdata, const dictEntry *de) {
    void **pd = (void**) privdata;
    list *keys = pd[0];
    robj *o = pd[1];
    robj *key, *val = NULL;

    if (o == NULL) {
        sds sdskey = dictGetKey(de);
        key = createStringObject(sdskey, sdslen(sdskey));
    } else if (o->type == OBJ_SET) {
        sds keysds = dictGetKey(de);
        key = createStringObject(keysds,sdslen(keysds));
    } else if (o->type == OBJ_HASH) {
        sds sdskey = dictGetKey(de);
        sds sdsval = dictGetVal(de);
        key = createStringObject(sdskey,sdslen(sdskey));
        val = createStringObject(sdsval,sdslen(sdsval));
    } else if (o->type == OBJ_ZSET) {
        sds sdskey = dictGetKey(de);
        key = createStringObject(sdskey,sdslen(sdskey));
        val = createStringObjectFromLongDouble(*(double*)dictGetVal(de),0);
    } else {
        serverPanic("Type not handled in SCAN callback.");
    }

    listAddNodeTail(keys, key);
    if (val) listAddNodeTail(keys, val);
}

/* Try to parse a SCAN cursor stored at object 'o':
 * if the cursor is valid, store it as unsigned integer into *cursor and
 * returns C_OK. Otherwise return C_ERR and send an error to the
 * client. */
//尝试解析存储在对象“o”中的 SCAN 游标：如果游标有效，则将其作为无符号整数存储到游标中并返回 C_OK。
// 否则返回 C_ERR 并向客户端发送错误。
int parseScanCursorOrReply(client *c, robj *o, unsigned long *cursor) {
    char *eptr;

    /* Use strtoul() because we need an *unsigned* long, so
     * getLongLongFromObject() does not cover the whole cursor space. */
    //使用 strtoul() 因为我们需要一个 unsigned long，所以 getLongLongFromObject() 不会覆盖整个游标空间。
    errno = 0;
    *cursor = strtoul(o->ptr, &eptr, 10);
    if (isspace(((char*)o->ptr)[0]) || eptr[0] != '\0' || errno == ERANGE)
    {
        addReplyError(c, "invalid cursor");
        return C_ERR;
    }
    return C_OK;
}

/* This command implements SCAN, HSCAN and SSCAN commands.
 * If object 'o' is passed, then it must be a Hash, Set or Zset object, otherwise
 * if 'o' is NULL the command will operate on the dictionary associated with
 * the current database.
 *
 * When 'o' is not NULL the function assumes that the first argument in
 * the client arguments vector is a key so it skips it before iterating
 * in order to parse options.
 *
 * In the case of a Hash object the function returns both the field and value
 * of every element on the Hash. */
/**
 * 该命令执行 SCAN、HSCAN 和 SSCAN 命令。如果传递了对象'o'，那么它必须是Hash、Set 或Zset 对象，否则如果'o' 为NULL，
 * 该命令将对与当前数据库关联的字典进行操作。当 'o' 不为 NULL 时，该函数假定客户端参数向量中的第一个参数是一个键，
 * 因此它在迭代之前跳过它以解析选项。对于 Hash 对象，该函数返回 Hash 上每个元素的字段和值。
 * */
void scanGenericCommand(client *c, robj *o, unsigned long cursor) {
    int i, j;
    list *keys = listCreate();
    listNode *node, *nextnode;
    long count = 10;
    sds pat = NULL;
    sds typename = NULL;
    int patlen = 0, use_pattern = 0;
    dict *ht;

    /* Object must be NULL (to iterate keys names), or the type of the object
     * must be Set, Sorted Set, or Hash. */
    //Object 必须为 NULL（以迭代键名），或者对象的类型必须是 Set、Sorted Set 或 Hash。
    serverAssert(o == NULL || o->type == OBJ_SET || o->type == OBJ_HASH ||
                o->type == OBJ_ZSET);

    /* Set i to the first option argument. The previous one is the cursor. */
    //将 i 设置为第一个选项参数。前一个是光标。
    i = (o == NULL) ? 2 : 3; /* Skip the key argument if needed. 如果需要，跳过关键参数。*/

    /* Step 1: Parse options. 第 1 步：解析选项。*/
    while (i < c->argc) {
        j = c->argc - i;
        if (!strcasecmp(c->argv[i]->ptr, "count") && j >= 2) {
            if (getLongFromObjectOrReply(c, c->argv[i+1], &count, NULL)
                != C_OK)
            {
                goto cleanup;
            }

            if (count < 1) {
                addReplyErrorObject(c,shared.syntaxerr);
                goto cleanup;
            }

            i += 2;
        } else if (!strcasecmp(c->argv[i]->ptr, "match") && j >= 2) {
            pat = c->argv[i+1]->ptr;
            patlen = sdslen(pat);

            /* The pattern always matches if it is exactly "*", so it is
             * equivalent to disabling it. */
            //如果模式恰好是“”，则该模式始终匹配，因此相当于禁用它。
            use_pattern = !(patlen == 1 && pat[0] == '*');

            i += 2;
        } else if (!strcasecmp(c->argv[i]->ptr, "type") && o == NULL && j >= 2) {
            /* SCAN for a particular type only applies to the db dict */
            //特定类型的 SCAN 仅适用于 db dict
            typename = c->argv[i+1]->ptr;
            i+= 2;
        } else {
            addReplyErrorObject(c,shared.syntaxerr);
            goto cleanup;
        }
    }

    /* Step 2: Iterate the collection.
     *
     * Note that if the object is encoded with a listpack, intset, or any other
     * representation that is not a hash table, we are sure that it is also
     * composed of a small number of elements. So to avoid taking state we
     * just return everything inside the object in a single call, setting the
     * cursor to zero to signal the end of the iteration. */
    /**
     * 第 2 步：迭代集合。请注意，如果对象是用 listpack、intset 或任何其他不是哈希表的表示形式编码的，
     * 我们确信它也是由少量元素组成的。因此，为了避免获取状态，我们只需在一次调用中返回对象内的所有内容，
     * 将光标设置为零以表示迭代结束。*/

    /* Handle the case of a hash table. 处理哈希表的情况。*/
    ht = NULL;
    if (o == NULL) {
        ht = c->db->dict;
    } else if (o->type == OBJ_SET && o->encoding == OBJ_ENCODING_HT) {
        ht = o->ptr;
    } else if (o->type == OBJ_HASH && o->encoding == OBJ_ENCODING_HT) {
        ht = o->ptr;
        count *= 2; /* We return key / value for this type. */
    } else if (o->type == OBJ_ZSET && o->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = o->ptr;
        ht = zs->dict;
        count *= 2; /* We return key / value for this type. 我们返回此类型的键值。*/
    }

    if (ht) {
        void *privdata[2];
        /* We set the max number of iterations to ten times the specified
         * COUNT, so if the hash table is in a pathological state (very
         * sparsely populated) we avoid to block too much time at the cost
         * of returning no or very few elements. */
        /**
         * 我们将最大迭代次数设置为指定 COUNT 的 10 倍，因此如果哈希表处于病态状态（非常稀疏），
         * 我们可以避免以不返回或返回很少元素为代价而阻塞太多时间。*/
        long maxiterations = count*10;

        /* We pass two pointers to the callback: the list to which it will
         * add new elements, and the object containing the dictionary so that
         * it is possible to fetch more data in a type-dependent way. */
        /**我们将两个指针传递给回调：它将添加新元素的列表，以及包含字典的对象，以便可以以依赖类型的方式获取更多数据。*/
        privdata[0] = keys;
        privdata[1] = o;
        do {
            cursor = dictScan(ht, cursor, scanCallback, NULL, privdata);
        } while (cursor &&
              maxiterations-- &&
              listLength(keys) < (unsigned long)count);
    } else if (o->type == OBJ_SET) {
        int pos = 0;
        int64_t ll;

        while(intsetGet(o->ptr,pos++,&ll))
            listAddNodeTail(keys,createStringObjectFromLongLong(ll));
        cursor = 0;
    } else if (o->type == OBJ_HASH || o->type == OBJ_ZSET) {
        unsigned char *p = lpFirst(o->ptr);
        unsigned char *vstr;
        int64_t vlen;
        unsigned char intbuf[LP_INTBUF_SIZE];

        while(p) {
            vstr = lpGet(p,&vlen,intbuf);
            listAddNodeTail(keys, createStringObject((char*)vstr,vlen));
            p = lpNext(o->ptr,p);
        }
        cursor = 0;
    } else {
        serverPanic("Not handled encoding in SCAN.");
    }

    /* Step 3: Filter elements. */
    node = listFirst(keys);
    while (node) {
        robj *kobj = listNodeValue(node);
        nextnode = listNextNode(node);
        int filter = 0;

        /* Filter element if it does not match the pattern. */
        //如果元素与模式不匹配，则过滤元素。
        if (use_pattern) {
            if (sdsEncodedObject(kobj)) {
                if (!stringmatchlen(pat, patlen, kobj->ptr, sdslen(kobj->ptr), 0))
                    filter = 1;
            } else {
                char buf[LONG_STR_SIZE];
                int len;

                serverAssert(kobj->encoding == OBJ_ENCODING_INT);
                len = ll2string(buf,sizeof(buf),(long)kobj->ptr);
                if (!stringmatchlen(pat, patlen, buf, len, 0)) filter = 1;
            }
        }

        /* Filter an element if it isn't the type we want. */
        //如果元素不是我们想要的类型，则过滤它。
        if (!filter && o == NULL && typename){
            robj* typecheck = lookupKeyReadWithFlags(c->db, kobj, LOOKUP_NOTOUCH);
            char* type = getObjectTypeName(typecheck);
            if (strcasecmp((char*) typename, type)) filter = 1;
        }

        /* Filter element if it is an expired key. 过滤元素，如果它是一个过期的键。*/
        if (!filter && o == NULL && expireIfNeeded(c->db, kobj, 0)) filter = 1;

        /* Remove the element and its associated value if needed. 如果需要，删除元素及其关联值。*/
        if (filter) {
            decrRefCount(kobj);
            listDelNode(keys, node);
        }

        /* If this is a hash or a sorted set, we have a flat list of
         * key-value elements, so if this element was filtered, remove the
         * value, or skip it if it was not filtered: we only match keys. */
        /**
         * 如果这是一个散列或排序集，我们有一个键值元素的平面列表，所以如果这个元素被过滤，
         * 删除这个值，或者如果它没有被过滤则跳过它：我们只匹配键。
         * */
        if (o && (o->type == OBJ_ZSET || o->type == OBJ_HASH)) {
            node = nextnode;
            serverAssert(node); /* assertion for valgrind (avoid NPD)   valgrind 的断言（避免 NPD）*/
            nextnode = listNextNode(node);
            if (filter) {
                kobj = listNodeValue(node);
                decrRefCount(kobj);
                listDelNode(keys, node);
            }
        }
        node = nextnode;
    }

    /* Step 4: Reply to the client.第四步：回复客户。 */
    addReplyArrayLen(c, 2);
    addReplyBulkLongLong(c,cursor);

    addReplyArrayLen(c, listLength(keys));
    while ((node = listFirst(keys)) != NULL) {
        robj *kobj = listNodeValue(node);
        addReplyBulk(c, kobj);
        decrRefCount(kobj);
        listDelNode(keys, node);
    }

cleanup:
    listSetFreeMethod(keys,decrRefCountVoid);
    listRelease(keys);
}

/* The SCAN command completely relies on scanGenericCommand. */
//SCAN 命令完全依赖于 scanGenericCommand。
void scanCommand(client *c) {
    unsigned long cursor;
    if (parseScanCursorOrReply(c,c->argv[1],&cursor) == C_ERR) return;
    scanGenericCommand(c,NULL,cursor);
}

void dbsizeCommand(client *c) {
    addReplyLongLong(c,dictSize(c->db->dict));
}

void lastsaveCommand(client *c) {
    addReplyLongLong(c,server.lastsave);
}

char* getObjectTypeName(robj *o) {
    char* type;
    if (o == NULL) {
        type = "none";
    } else {
        switch(o->type) {
        case OBJ_STRING: type = "string"; break;
        case OBJ_LIST: type = "list"; break;
        case OBJ_SET: type = "set"; break;
        case OBJ_ZSET: type = "zset"; break;
        case OBJ_HASH: type = "hash"; break;
        case OBJ_STREAM: type = "stream"; break;
        case OBJ_MODULE: {
            moduleValue *mv = o->ptr;
            type = mv->type->name;
        }; break;
        default: type = "unknown"; break;
        }
    }
    return type;
}

void typeCommand(client *c) {
    robj *o;
    o = lookupKeyReadWithFlags(c->db,c->argv[1],LOOKUP_NOTOUCH);
    addReplyStatus(c, getObjectTypeName(o));
}

void shutdownCommand(client *c) {
    int flags = SHUTDOWN_NOFLAGS;
    int abort = 0;
    for (int i = 1; i < c->argc; i++) {
        if (!strcasecmp(c->argv[i]->ptr,"nosave")) {
            flags |= SHUTDOWN_NOSAVE;
        } else if (!strcasecmp(c->argv[i]->ptr,"save")) {
            flags |= SHUTDOWN_SAVE;
        } else if (!strcasecmp(c->argv[i]->ptr, "now")) {
            flags |= SHUTDOWN_NOW;
        } else if (!strcasecmp(c->argv[i]->ptr, "force")) {
            flags |= SHUTDOWN_FORCE;
        } else if (!strcasecmp(c->argv[i]->ptr, "abort")) {
            abort = 1;
        } else {
            addReplyErrorObject(c,shared.syntaxerr);
            return;
        }
    }
    if ((abort && flags != SHUTDOWN_NOFLAGS) ||
        (flags & SHUTDOWN_NOSAVE && flags & SHUTDOWN_SAVE))
    {
        /* Illegal combo. */
        addReplyErrorObject(c,shared.syntaxerr);
        return;
    }

    if (abort) {
        if (abortShutdown() == C_OK)
            addReply(c, shared.ok);
        else
            addReplyError(c, "No shutdown in progress.");
        return;
    }

    if (!(flags & SHUTDOWN_NOW) && c->flags & CLIENT_DENY_BLOCKING) {
        addReplyError(c, "SHUTDOWN without NOW or ABORT isn't allowed for DENY BLOCKING client");
        return;
    }

    if (!(flags & SHUTDOWN_NOSAVE) && isInsideYieldingLongCommand()) {
        /* Script timed out. Shutdown allowed only with the NOSAVE flag. See
         * also processCommand where these errors are returned. */
        //脚本超时。仅使用 NOSAVE 标志才允许关闭。另请参阅返回这些错误的 processCommand。
        if (server.busy_module_yield_flags && server.busy_module_yield_reply) {
            addReplyErrorFormat(c, "-BUSY %s", server.busy_module_yield_reply);
        } else if (server.busy_module_yield_flags) {
            addReplyErrorObject(c, shared.slowmoduleerr);
        } else if (scriptIsEval()) {
            addReplyErrorObject(c, shared.slowevalerr);
        } else {
            addReplyErrorObject(c, shared.slowscripterr);
        }
        return;
    }

    blockClient(c, BLOCKED_SHUTDOWN);
    if (prepareForShutdown(flags) == C_OK) exit(0);
    /* If we're here, then shutdown is ongoing (the client is still blocked) or
     * failed (the client has received an error).
     * 如果我们在这里，则关闭正在进行（客户端仍然被阻止）或失败（客户端收到错误）。*/
}

void renameGenericCommand(client *c, int nx) {
    robj *o;
    long long expire;
    int samekey = 0;

    /* When source and dest key is the same, no operation is performed,
     * if the key exists, however we still return an error on unexisting key. */
    //当 source 和 dest key 相同时，不执行任何操作，如果 key 存在，但我们仍然返回 unexisting key 的错误。
    if (sdscmp(c->argv[1]->ptr,c->argv[2]->ptr) == 0) samekey = 1;

    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.nokeyerr)) == NULL)
        return;

    if (samekey) {
        addReply(c,nx ? shared.czero : shared.ok);
        return;
    }

    incrRefCount(o);
    expire = getExpire(c->db,c->argv[1]);
    if (lookupKeyWrite(c->db,c->argv[2]) != NULL) {
        if (nx) {
            decrRefCount(o);
            addReply(c,shared.czero);
            return;
        }
        /* Overwrite: delete the old key before creating the new one
         * with the same name. */
        //覆盖：在创建具有相同名称的新密钥之前删除旧密钥。
        dbDelete(c->db,c->argv[2]);
    }
    dbAdd(c->db,c->argv[2],o);
    if (expire != -1) setExpire(c,c->db,c->argv[2],expire);
    dbDelete(c->db,c->argv[1]);
    signalModifiedKey(c,c->db,c->argv[1]);
    signalModifiedKey(c,c->db,c->argv[2]);
    notifyKeyspaceEvent(NOTIFY_GENERIC,"rename_from",
        c->argv[1],c->db->id);
    notifyKeyspaceEvent(NOTIFY_GENERIC,"rename_to",
        c->argv[2],c->db->id);
    server.dirty++;
    addReply(c,nx ? shared.cone : shared.ok);
}

void renameCommand(client *c) {
    renameGenericCommand(c,0);
}

void renamenxCommand(client *c) {
    renameGenericCommand(c,1);
}

void moveCommand(client *c) {
    robj *o;
    redisDb *src, *dst;
    int srcid, dbid;
    long long expire;

    if (server.cluster_enabled) {
        addReplyError(c,"MOVE is not allowed in cluster mode");
        return;
    }

    /* Obtain source and target DB pointers */
    //获取源和目标数据库指针
    src = c->db;
    srcid = c->db->id;

    if (getIntFromObjectOrReply(c, c->argv[2], &dbid, NULL) != C_OK)
        return;

    if (selectDb(c,dbid) == C_ERR) {
        addReplyError(c,"DB index is out of range");
        return;
    }
    dst = c->db;
    selectDb(c,srcid); /* Back to the source DB */

    /* If the user is moving using as target the same
     * DB as the source DB it is probably an error. */
    //如果用户使用与源数据库相同的数据库作为目标进行移动，则可能是一个错误。
    if (src == dst) {
        addReplyErrorObject(c,shared.sameobjecterr);
        return;
    }

    /* Check if the element exists and get a reference */
    //检查元素是否存在并获取引用
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (!o) {
        addReply(c,shared.czero);
        return;
    }
    expire = getExpire(c->db,c->argv[1]);

    /* Return zero if the key already exists in the target DB */
    //如果目标数据库中已存在密钥，则返回零
    if (lookupKeyWrite(dst,c->argv[1]) != NULL) {
        addReply(c,shared.czero);
        return;
    }
    dbAdd(dst,c->argv[1],o);
    if (expire != -1) setExpire(c,dst,c->argv[1],expire);
    incrRefCount(o);

    /* OK! key moved, free the entry in the source DB */
    //好的！键移动，释放源数据库中的条目
    dbDelete(src,c->argv[1]);
    signalModifiedKey(c,src,c->argv[1]);
    signalModifiedKey(c,dst,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_GENERIC,
                "move_from",c->argv[1],src->id);
    notifyKeyspaceEvent(NOTIFY_GENERIC,
                "move_to",c->argv[1],dst->id);

    server.dirty++;
    addReply(c,shared.cone);
}

void copyCommand(client *c) {
    robj *o;
    redisDb *src, *dst;
    int srcid, dbid;
    long long expire;
    int j, replace = 0, delete = 0;

    /* Obtain source and target DB pointers 
     * Default target DB is the same as the source DB 
     * Parse the REPLACE option and targetDB option. */
    //获取源和目标 DB 指针 默认目标 DB 与源 DB 相同解析 REPLACE 选项和 targetDB 选项。
    src = c->db;
    dst = c->db;
    srcid = c->db->id;
    dbid = c->db->id;
    for (j = 3; j < c->argc; j++) {
        int additional = c->argc - j - 1;
        if (!strcasecmp(c->argv[j]->ptr,"replace")) {
            replace = 1;
        } else if (!strcasecmp(c->argv[j]->ptr, "db") && additional >= 1) {
            if (getIntFromObjectOrReply(c, c->argv[j+1], &dbid, NULL) != C_OK)
                return;

            if (selectDb(c, dbid) == C_ERR) {
                addReplyError(c,"DB index is out of range");
                return;
            }
            dst = c->db;
            selectDb(c,srcid); /* Back to the source DB */
            j++; /* Consume additional arg. */
        } else {
            addReplyErrorObject(c,shared.syntaxerr);
            return;
        }
    }

    if ((server.cluster_enabled == 1) && (srcid != 0 || dbid != 0)) {
        addReplyError(c,"Copying to another database is not allowed in cluster mode");
        return;
    }

    /* If the user select the same DB as
     * the source DB and using newkey as the same key
     * it is probably an error. */
    //如果用户选择与源 DB 相同的 DB 并使用 newkey 作为相同的键，则可能是错误。
    robj *key = c->argv[1];
    robj *newkey = c->argv[2];
    if (src == dst && (sdscmp(key->ptr, newkey->ptr) == 0)) {
        addReplyErrorObject(c,shared.sameobjecterr);
        return;
    }

    /* Check if the element exists and get a reference */
    //检查元素是否存在并获取引用
    o = lookupKeyRead(c->db, key);
    if (!o) {
        addReply(c,shared.czero);
        return;
    }
    expire = getExpire(c->db,key);

    /* Return zero if the key already exists in the target DB. 
     * If REPLACE option is selected, delete newkey from targetDB. */
    //如果密钥已存在于目标数据库中，则返回零。如果选择了 REPLACE 选项，则从 targetDB 中删除 newkey。
    if (lookupKeyWrite(dst,newkey) != NULL) {
        if (replace) {
            delete = 1;
        } else {
            addReply(c,shared.czero);
            return;
        }
    }

    /* Duplicate object according to object's type. */
    //根据对象的类型复制对象。
    robj *newobj;
    switch(o->type) {
        case OBJ_STRING: newobj = dupStringObject(o); break;
        case OBJ_LIST: newobj = listTypeDup(o); break;
        case OBJ_SET: newobj = setTypeDup(o); break;
        case OBJ_ZSET: newobj = zsetDup(o); break;
        case OBJ_HASH: newobj = hashTypeDup(o); break;
        case OBJ_STREAM: newobj = streamDup(o); break;
        case OBJ_MODULE:
            newobj = moduleTypeDupOrReply(c, key, newkey, dst->id, o);
            if (!newobj) return;
            break;
        default:
            addReplyError(c, "unknown type object");
            return;
    }

    if (delete) {
        dbDelete(dst,newkey);
    }

    dbAdd(dst,newkey,newobj);
    if (expire != -1) setExpire(c, dst, newkey, expire);

    /* OK! key copied */
    signalModifiedKey(c,dst,c->argv[2]);
    notifyKeyspaceEvent(NOTIFY_GENERIC,"copy_to",c->argv[2],dst->id);

    server.dirty++;
    addReply(c,shared.cone);
}

/* Helper function for dbSwapDatabases(): scans the list of keys that have
 * one or more blocked clients for B[LR]POP or other blocking commands
 * and signal the keys as ready if they are of the right type. See the comment
 * where the function is used for more info. */
/**
 * dbSwapDatabases() 的辅助函数：扫描具有一个或多个阻塞客户端的键列表以查找 B[LR]POP 或其他阻塞命令，
 * 并在键的类型正确时发出就绪信号。有关更多信息，请参阅使用该函数的注释。
 * */
void scanDatabaseForReadyKeys(redisDb *db) {
    dictEntry *de;
    dictIterator *di = dictGetSafeIterator(db->blocking_keys);
    while((de = dictNext(di)) != NULL) {
        robj *key = dictGetKey(de);
        dictEntry *kde = dictFind(db->dict,key->ptr);
        if (kde) {
            robj *value = dictGetVal(kde);
            signalKeyAsReady(db, key, value->type);
        }
    }
    dictReleaseIterator(di);
}

/* Since we are unblocking XREADGROUP clients in the event the
 * key was deleted/overwritten we must do the same in case the
 * database was flushed/swapped. */
//由于我们在密钥被删除覆盖的情况下解除阻塞 XREADGROUP 客户端，因此我们必须这样做，以防数据库被刷新交换。
void scanDatabaseForDeletedStreams(redisDb *emptied, redisDb *replaced_with) {
    /* Optimization: If no clients are in type BLOCKED_STREAM,
     * we can skip this loop. */
    //优化：如果 BLOCKED_STREAM 类型中没有客户端，我们可以跳过此循环。
    if (!server.blocked_clients_by_type[BLOCKED_STREAM]) return;

    dictEntry *de;
    dictIterator *di = dictGetSafeIterator(emptied->blocking_keys);
    while((de = dictNext(di)) != NULL) {
        robj *key = dictGetKey(de);
        int was_stream = 0, is_stream = 0;

        dictEntry *kde = dictFind(emptied->dict, key->ptr);
        if (kde) {
            robj *value = dictGetVal(kde);
            was_stream = value->type == OBJ_STREAM;
        }
        if (replaced_with) {
            dictEntry *kde = dictFind(replaced_with->dict, key->ptr);
            if (kde) {
                robj *value = dictGetVal(kde);
                is_stream = value->type == OBJ_STREAM;
            }
        }
        /* We want to try to unblock any client using a blocking XREADGROUP */
        //我们想尝试使用阻塞 XREADGROUP 解除对任何客户端的阻塞
        if (was_stream && !is_stream)
            signalKeyAsReady(emptied, key, OBJ_STREAM);
    }
    dictReleaseIterator(di);
}

/* Swap two databases at runtime so that all clients will magically see
 * the new database even if already connected. Note that the client
 * structure c->db points to a given DB, so we need to be smarter and
 * swap the underlying referenced structures, otherwise we would need
 * to fix all the references to the Redis DB structure.
 *
 * Returns C_ERR if at least one of the DB ids are out of range, otherwise
 * C_OK is returned. */
/**
 * 在运行时交换两个数据库，这样即使已经连接，所有客户端也会神奇地看到新数据库。
 * 请注意，客户端结构 c->db 指向给定的 DB，因此我们需要更聪明并交换底层引用的结构，
 * 否则我们需要修复对 Redis DB 结构的所有引用。如果至少有一个 DB id 超出范围，则返回 C_ERR，否则返回 C_OK。
 * */
int dbSwapDatabases(int id1, int id2) {
    if (id1 < 0 || id1 >= server.dbnum ||
        id2 < 0 || id2 >= server.dbnum) return C_ERR;
    if (id1 == id2) return C_OK;
    redisDb aux = server.db[id1];
    redisDb *db1 = &server.db[id1], *db2 = &server.db[id2];

    /* Swapdb should make transaction fail if there is any
     * client watching keys */
    //如果有任何客户端监视密钥，Swapdb 应该使事务失败
    touchAllWatchedKeysInDb(db1, db2);
    touchAllWatchedKeysInDb(db2, db1);

    /* Try to unblock any XREADGROUP clients if the key no longer exists. */
    //如果密钥不再存在，请尝试取消阻止任何 XREADGROUP 客户端。
    scanDatabaseForDeletedStreams(db1, db2);
    scanDatabaseForDeletedStreams(db2, db1);

    /* Swap hash tables. Note that we don't swap blocking_keys,
     * ready_keys and watched_keys, since we want clients to
     * remain in the same DB they were. */
    //交换哈希表。请注意，我们不会交换blocking_keys、ready_keys 和watched_keys，因为我们希望客户端保持在同一个数据库中。
    db1->dict = db2->dict;
    db1->expires = db2->expires;
    db1->avg_ttl = db2->avg_ttl;
    db1->expires_cursor = db2->expires_cursor;

    db2->dict = aux.dict;
    db2->expires = aux.expires;
    db2->avg_ttl = aux.avg_ttl;
    db2->expires_cursor = aux.expires_cursor;

    /* Now we need to handle clients blocked on lists: as an effect
     * of swapping the two DBs, a client that was waiting for list
     * X in a given DB, may now actually be unblocked if X happens
     * to exist in the new version of the DB, after the swap.
     *
     * However normally we only do this check for efficiency reasons
     * in dbAdd() when a list is created. So here we need to rescan
     * the list of clients blocked on lists and signal lists as ready
     * if needed. */
    /**
     * 现在我们需要处理在列表中被阻塞的客户端：作为交换两个 DB 的效果，一个在给定 DB 中等待列表 X 的客户端，
     * 如果 X 恰好存在于新版本的 DB 中，实际上可能会被解除阻塞，交换后。
     * 然而，通常我们只在创建列表时在 dbAdd() 中出于效率原因进行此检查。
     * 因此，如果需要，我们需要重新扫描列表和信号列表中被阻止的客户端列表。
     * */
    scanDatabaseForReadyKeys(db1);
    scanDatabaseForReadyKeys(db2);
    return C_OK;
}

/* Logically, this discards (flushes) the old main database, and apply the newly loaded
 * database (temp) as the main (active) database, the actual freeing of old database
 * (which will now be placed in the temp one) is done later. */
/**
 * 从逻辑上讲，这会丢弃（刷新）旧的主数据库，并将新加载的数据库（临时）用作主（活动）数据库，
 * 旧数据库的实际释放（现在将放置在临时数据库中）稍后完成。
 * */
void swapMainDbWithTempDb(redisDb *tempDb) {
    if (server.cluster_enabled) {
        /* Swap slots_to_keys from tempdb just loaded with main db slots_to_keys. */
        //从刚刚加载主数据库 slot_to_keys 的 tempdb 交换 slot_to_keys。
        clusterSlotToKeyMapping *aux = server.db->slots_to_keys;
        server.db->slots_to_keys = tempDb->slots_to_keys;
        tempDb->slots_to_keys = aux;
    }

    for (int i=0; i<server.dbnum; i++) {
        redisDb aux = server.db[i];
        redisDb *activedb = &server.db[i], *newdb = &tempDb[i];

        /* Swapping databases should make transaction fail if there is any
         * client watching keys. */
        //如果有任何客户端监视密钥，则交换数据库应该使事务失败。
        touchAllWatchedKeysInDb(activedb, newdb);

        /* Try to unblock any XREADGROUP clients if the key no longer exists. */
        //如果密钥不再存在，请尝试取消阻止任何 XREADGROUP 客户端。
        scanDatabaseForDeletedStreams(activedb, newdb);

        /* Swap hash tables. Note that we don't swap blocking_keys,
         * ready_keys and watched_keys, since clients 
         * remain in the same DB they were. */
        //交换哈希表。请注意，我们不会交换blocking_keys、ready_keys 和watched_keys，因为客户端仍然在同一个数据库中。
        activedb->dict = newdb->dict;
        activedb->expires = newdb->expires;
        activedb->avg_ttl = newdb->avg_ttl;
        activedb->expires_cursor = newdb->expires_cursor;

        newdb->dict = aux.dict;
        newdb->expires = aux.expires;
        newdb->avg_ttl = aux.avg_ttl;
        newdb->expires_cursor = aux.expires_cursor;

        /* Now we need to handle clients blocked on lists: as an effect
         * of swapping the two DBs, a client that was waiting for list
         * X in a given DB, may now actually be unblocked if X happens
         * to exist in the new version of the DB, after the swap.
         *
         * However normally we only do this check for efficiency reasons
         * in dbAdd() when a list is created. So here we need to rescan
         * the list of clients blocked on lists and signal lists as ready
         * if needed. */
        /**
         * 现在我们需要处理在列表中被阻塞的客户端：作为交换两个 DB 的效果，一个在给定 DB 中等待列表 X 的客户端，
         * 如果 X 恰好存在于新版本的 DB 中，实际上可能会被解除阻塞，交换后。
         * 然而，通常我们只在创建列表时在 dbAdd() 中出于效率原因进行此检查。
         * 因此，如果需要，我们需要重新扫描列表和信号列表中被阻止的客户端列表。
         * */
        scanDatabaseForReadyKeys(activedb);
    }

    trackingInvalidateKeysOnFlush(1);
    flushSlaveKeysWithExpireList();
}

/* SWAPDB db1 db2 */
void swapdbCommand(client *c) {
    int id1, id2;

    /* Not allowed in cluster mode: we have just DB 0 there. */
    //在集群模式下不允许：我们那里只有 DB 0。
    if (server.cluster_enabled) {
        addReplyError(c,"SWAPDB is not allowed in cluster mode");
        return;
    }

    /* Get the two DBs indexes. 获取两个数据库索引。*/
    if (getIntFromObjectOrReply(c, c->argv[1], &id1,
        "invalid first DB index") != C_OK)
        return;

    if (getIntFromObjectOrReply(c, c->argv[2], &id2,
        "invalid second DB index") != C_OK)
        return;

    /* Swap... */
    if (dbSwapDatabases(id1,id2) == C_ERR) {
        addReplyError(c,"DB index is out of range");
        return;
    } else {
        RedisModuleSwapDbInfo si = {REDISMODULE_SWAPDBINFO_VERSION,id1,id2};
        moduleFireServerEvent(REDISMODULE_EVENT_SWAPDB,0,&si);
        server.dirty++;
        addReply(c,shared.ok);
    }
}

/*-----------------------------------------------------------------------------
 * Expires API
 *----------------------------------------------------------------------------*/

int removeExpire(redisDb *db, robj *key) {
    /* An expire may only be removed if there is a corresponding entry in the
     * main dict. Otherwise, the key will never be freed. */
    //只有在主字典中有相应条目时，才能删除过期。否则，密钥将永远不会被释放。
    serverAssertWithInfo(NULL,key,dictFind(db->dict,key->ptr) != NULL);
    return dictDelete(db->expires,key->ptr) == DICT_OK;
}

/* Set an expire to the specified key. If the expire is set in the context
 * of an user calling a command 'c' is the client, otherwise 'c' is set
 * to NULL. The 'when' parameter is the absolute unix time in milliseconds
 * after which the key will no longer be considered valid. */
/**
 * 为指定的键设置过期时间。如果在用户调用命令'c' 的上下文中设置了过期时间，则'c' 是客户端，否则'c' 设置为NULL。
 * 'when' 参数是以毫秒为单位的绝对 unix 时间，在此之后密钥将不再被视为有效。
 * */
void setExpire(client *c, redisDb *db, robj *key, long long when) {
    dictEntry *kde, *de;

    /* Reuse the sds from the main dict in the expire dict */
    //在过期字典中重用主字典中的 sds
    kde = dictFind(db->dict,key->ptr);
    serverAssertWithInfo(NULL,key,kde != NULL);
    de = dictAddOrFind(db->expires,dictGetKey(kde));
    dictSetSignedIntegerVal(de,when);

    int writable_slave = server.masterhost && server.repl_slave_ro == 0;
    if (c && writable_slave && !(c->flags & CLIENT_MASTER))
        rememberSlaveKeyWithExpire(db,key);
}

/* Return the expire time of the specified key, or -1 if no expire
 * is associated with this key (i.e. the key is non volatile) */
//返回指定密钥的过期时间，如果没有过期与此密钥关联，则返回 -1（即密钥是非易失性的）
long long getExpire(redisDb *db, robj *key) {
    dictEntry *de;

    /* No expire? return ASAP 没有过期？尽快返回*/
    if (dictSize(db->expires) == 0 ||
       (de = dictFind(db->expires,key->ptr)) == NULL) return -1;

    /* The entry was found in the expire dict, this means it should also
     * be present in the main dict (safety check). */
    //该条目是在过期字典中找到的，这意味着它也应该存在于主字典中（安全检查）。
    serverAssertWithInfo(NULL,key,dictFind(db->dict,key->ptr) != NULL);
    return dictGetSignedIntegerVal(de);
}

/* Delete the specified expired key and propagate expire. */
//删除指定的过期键并传播过期。
void deleteExpiredKeyAndPropagate(redisDb *db, robj *keyobj) {
    mstime_t expire_latency;
    latencyStartMonitor(expire_latency);
    if (server.lazyfree_lazy_expire)
        dbAsyncDelete(db,keyobj);
    else
        dbSyncDelete(db,keyobj);
    latencyEndMonitor(expire_latency);
    latencyAddSampleIfNeeded("expire-del",expire_latency);
    notifyKeyspaceEvent(NOTIFY_EXPIRED,"expired",keyobj,db->id);
    signalModifiedKey(NULL, db, keyobj);
    propagateDeletion(db,keyobj,server.lazyfree_lazy_expire);
    server.stat_expiredkeys++;
}

/* Propagate expires into slaves and the AOF file.
 * When a key expires in the master, a DEL operation for this key is sent
 * to all the slaves and the AOF file if enabled.
 *
 * This way the key expiry is centralized in one place, and since both
 * AOF and the master->slave link guarantee operation ordering, everything
 * will be consistent even if we allow write operations against expiring
 * keys.
 *
 * This function may be called from:
 * 1. Within call(): Example: Lazy-expire on key access.
 *    In this case the caller doesn't have to do anything
 *    because call() handles server.also_propagate(); or
 * 2. Outside of call(): Example: Active-expire, eviction.
 *    In this the caller must remember to call
 *    propagatePendingCommands, preferably at the end of
 *    the deletion batch, so that DELs will be wrapped
 *    in MULTI/EXEC */
/**
 * 传播到从属和 AOF 文件中过期。当主服务器中的密钥过期时，该密钥的 DEL 操作将发送到所有从服务器和 AOF 文件（如果启用）。
 * 这样，密钥过期集中在一个地方，并且由于 AOF 和主->从链接都保证操作顺序，即使我们允许对过期密钥进行写操作，一切都会保持一致。
 * 可以从以下位置调用此函数：
 *   1. 在 call() 中： 示例：在密钥访问时延迟过期。在这种情况下，调用者不必做任何事情，
 *      因为 call() 处理 server.also_propagate();或
 *   2. 在 call() 之外：示例：Active-expire, eviction。
 *      在这种情况下，调用者必须记住调用propagatePendingCommands，最好在删除批处理结束时调用，
 *      以便将DELs 包装在MULTIEXEC 中
 * */
void propagateDeletion(redisDb *db, robj *key, int lazy) {
    robj *argv[2];

    argv[0] = lazy ? shared.unlink : shared.del;
    argv[1] = key;
    incrRefCount(argv[0]);
    incrRefCount(argv[1]);

    /* If the master decided to expire a key we must propagate it to replicas no matter what..
     * Even if module executed a command without asking for propagation. */
    //如果 master 决定使密钥过期，无论如何我们都必须将其传播到副本。即使模块执行了命令而不要求传播。
    int prev_replication_allowed = server.replication_allowed;
    server.replication_allowed = 1;
    alsoPropagate(db->id,argv,2,PROPAGATE_AOF|PROPAGATE_REPL);
    server.replication_allowed = prev_replication_allowed;

    decrRefCount(argv[0]);
    decrRefCount(argv[1]);
}

/* Check if the key is expired. 检查密钥是否过期。*/
int keyIsExpired(redisDb *db, robj *key) {
    mstime_t when = getExpire(db,key);
    mstime_t now;

    if (when < 0) return 0; /* No expire for this key */

    /* Don't expire anything while loading. It will be done later. */
    //加载时不要过期任何东西。稍后会完成。
    if (server.loading) return 0;

    /* If we are in the context of a Lua script, we pretend that time is
     * blocked to when the Lua script started. This way a key can expire
     * only the first time it is accessed and not in the middle of the
     * script execution, making propagation to slaves / AOF consistent.
     * See issue #1525 on Github for more information. */
    /**
     * 如果我们在 Lua 脚本的上下文中，我们假装时间被阻塞到 Lua 脚本开始的时候。
     * 这样一个键只能在第一次访问时过期，而不是在脚本执行的中间，这使得传播到从属 AOF 是一致的。
     * 有关更多信息，请参阅 Github 上的 issue 1525。
     * */
    if (server.script_caller) {
        now = scriptTimeSnapshot();
    }
    /* If we are in the middle of a command execution, we still want to use
     * a reference time that does not change: in that case we just use the
     * cached time, that we update before each call in the call() function.
     * This way we avoid that commands such as RPOPLPUSH or similar, that
     * may re-open the same key multiple times, can invalidate an already
     * open object in a next call, if the next call will see the key expired,
     * while the first did not. */
    /**
     * 如果我们正在执行命令，我们仍然希望使用一个不变的参考时间：
     * 在这种情况下，我们只使用缓存的时间，我们在 call() 函数中的每次调用之前更新它。
     * 通过这种方式，我们避免了诸如 RPOPLPUSH 或类似命令（可能会多次重新打开同一个密钥）在下一次调用中使已经打开的对象无效，
     * 如果下一次调用会看到密钥过期，而第一次没有。
     * */
    else if (server.fixed_time_expire > 0) {
        now = server.mstime;
    }
    /* For the other cases, we want to use the most fresh time we have. */
    //对于其他情况，我们希望使用我们拥有的最新鲜的时间。
    else {
        now = mstime();
    }

    /* The key expired if the current (virtual or real) time is greater
     * than the expire time of the key. */
    //如果当前（虚拟或真实）时间大于密钥的过期时间，则密钥过期。
    return now > when;
}

/* This function is called when we are going to perform some operation
 * in a given key, but such key may be already logically expired even if
 * it still exists in the database. The main way this function is called
 * is via lookupKey*() family of functions.
 *
 * The behavior of the function depends on the replication role of the
 * instance, because by default replicas do not delete expired keys. They
 * wait for DELs from the master for consistency matters. However even
 * replicas will try to have a coherent return value for the function,
 * so that read commands executed in the replica side will be able to
 * behave like if the key is expired even if still present (because the
 * master has yet to propagate the DEL).
 *
 * In masters as a side effect of finding a key which is expired, such
 * key will be evicted from the database. Also this may trigger the
 * propagation of a DEL/UNLINK command in AOF / replication stream.
 *
 * On replicas, this function does not delete expired keys by default, but
 * it still returns 1 if the key is logically expired. To force deletion
 * of logically expired keys even on replicas, set force_delete_expired to
 * a non-zero value. Note though that if the current client is executing
 * replicated commands from the master, keys are never considered expired.
 *
 * The return value of the function is 0 if the key is still valid,
 * otherwise the function returns 1 if the key is expired. */
/**
 * 当我们要对给定键执行某些操作时调用此函数，但是即使该键仍然存在于数据库中，它也可能在逻辑上已经过期。
 * 调用此函数的主要方式是通过 lookupKey() 系列函数。该函数的行为取决于实例的复制角色，因为默认情况下，副本不会删除过期的键。
 * 他们等待来自 master 的 DEL 来解决一致性问题。然而，即使是副本也会尝试为该函数提供一致的返回值，
 * 因此在副本端执行的读取命令将能够表现得就像密钥已过期，即使仍然存在（因为主服务器尚未传播 DEL） .
 * 在 master 中，作为找到过期密钥的副作用，此类密钥将从数据库中逐出。这也可能触发 DELUNLINK 命令在 AOF 复制流中的传播。
 * 在副本上，这个函数默认不会删除过期的key，但是如果key在逻辑上过期了，它仍然返回1。
 * 要强制删除副本上的逻辑过期键，请将 force_delete_expired 设置为非零值。
 * 请注意，如果当前客户端正在执行来自主服务器的复制命令，则密钥永远不会被视为过期。
 * 如果密钥仍然有效，该函数的返回值为 0，否则如果密钥过期，该函数返回 1。
 * */
int expireIfNeeded(redisDb *db, robj *key, int force_delete_expired) {
    if (!keyIsExpired(db,key)) return 0;

    /* If we are running in the context of a replica, instead of
     * evicting the expired key from the database, we return ASAP:
     * the replica key expiration is controlled by the master that will
     * send us synthesized DEL operations for expired keys. The
     * exception is when write operations are performed on writable
     * replicas.
     *
     * Still we try to return the right information to the caller,
     * that is, 0 if we think the key should be still valid, 1 if
     * we think the key is expired at this time.
     *
     * When replicating commands from the master, keys are never considered
     * expired. */
    /**
     * 如果我们在副本上下文中运行，而不是从数据库中清除过期密钥，我们会尽快返回：
     * 副本密钥过期由主控器控制，主控器将向我们发送针对过期密钥的合成 DEL 操作。
     * 例外情况是在可写副本上执行写操作时。我们仍然尝试向调用者返回正确的信息，即如果我们认为密钥应该仍然有效，则为 0，
     * 如果我们认为此时密钥已过期，则为 1。从主服务器复制命令时，密钥永远不会被视为过期。
     * */
    if (server.masterhost != NULL) {
        if (server.current_client == server.master) return 0;
        if (!force_delete_expired) return 1;
    }

    /* If clients are paused, we keep the current dataset constant,
     * but return to the client what we believe is the right state. Typically,
     * at the end of the pause we will properly expire the key OR we will
     * have failed over and the new primary will send us the expire. */
    /**
     * 如果客户端暂停，我们将保持当前数据集不变，但将我们认为正确的状态返回给客户端。
     * 通常，在暂停结束时，我们将正确地使密钥过期，或者我们将故障转移并且新的主节点将向我们发送过期消息。
     * */
    if (checkClientPauseTimeoutAndReturnIfPaused()) return 1;

    /* Delete the key */
    deleteExpiredKeyAndPropagate(db,key);
    return 1;
}

/* -----------------------------------------------------------------------------
 * API to get key arguments from commands 从命令中获取key参数的 API
 * ---------------------------------------------------------------------------*/

/* Prepare the getKeysResult struct to hold numkeys, either by using the
 * pre-allocated keysbuf or by allocating a new array on the heap.
 *
 * This function must be called at least once before starting to populate
 * the result, and can be called repeatedly to enlarge the result array.
 * 准备 getKeysResult 结构以保存 numkeys，方法是使用预分配的 keysbuf 或在堆上分配一个新数组。
 * 该函数必须在开始填充结果之前至少调用一次，并且可以重复调用以扩大结果数组。
 */
keyReference *getKeysPrepareResult(getKeysResult *result, int numkeys) {
    /* GETKEYS_RESULT_INIT initializes keys to NULL, point it to the pre-allocated stack
     * buffer here. */
    //GETKEYS_RESULT_INIT 将键初始化为 NULL，在此处将其指向预分配的堆栈缓冲区。
    if (!result->keys) {
        serverAssert(!result->numkeys);
        result->keys = result->keysbuf;
    }

    /* Resize if necessary 必要时调整大小*/
    if (numkeys > result->size) {
        if (result->keys != result->keysbuf) {
            /* We're not using a static buffer, just (re)alloc */
            //我们没有使用静态缓冲区，只是（重新）分配
            result->keys = zrealloc(result->keys, numkeys * sizeof(keyReference));
        } else {
            /* We are using a static buffer, copy its contents */
            //我们正在使用静态缓冲区，复制其内容
            result->keys = zmalloc(numkeys * sizeof(keyReference));
            if (result->numkeys)
                memcpy(result->keys, result->keysbuf, result->numkeys * sizeof(keyReference));
        }
        result->size = numkeys;
    }

    return result->keys;
}

/* Returns a bitmask with all the flags found in any of the key specs of the command.
 * The 'inv' argument means we'll return a mask with all flags that are missing in at least one spec. */
/**
 * 返回一个位掩码，其中包含在命令的任何关键规范中找到的所有标志。
 * 'inv' 参数意味着我们将返回一个掩码，其中包含至少一个规范中缺少的所有标志。
 * */
int64_t getAllKeySpecsFlags(struct redisCommand *cmd, int inv) {
    int64_t flags = 0;
    for (int j = 0; j < cmd->key_specs_num; j++) {
        keySpec *spec = cmd->key_specs + j;
        flags |= inv? ~spec->flags : spec->flags;
    }
    return flags;
}

/* Fetch the keys based of the provided key specs. Returns the number of keys found, or -1 on error.
 * There are several flags that can be used to modify how this function finds keys in a command.
 * 
 * GET_KEYSPEC_INCLUDE_NOT_KEYS: Return 'fake' keys as if they were keys.
 * GET_KEYSPEC_RETURN_PARTIAL:   Skips invalid and incomplete keyspecs but returns the keys
 *                               found in other valid keyspecs. 
 */
/**
 * 根据提供的密钥规范获取密钥。返回找到的键的数量，或 -1 错误。有几个标志可用于修改此函数在命令中查找键的方式。
 * GET_KEYSPEC_INCLUDE_NOT_KEYS：返回“假”密钥，就好像它们是密钥一样。
 * GET_KEYSPEC_RETURN_PARTIAL：跳过无效和不完整的密钥规范，但返回在其他有效密钥规范中找到的密钥。
 * */
int getKeysUsingKeySpecs(struct redisCommand *cmd, robj **argv, int argc, int search_flags, getKeysResult *result) {
    int j, i, k = 0, last, first, step;
    keyReference *keys;

    for (j = 0; j < cmd->key_specs_num; j++) {
        keySpec *spec = cmd->key_specs + j;
        serverAssert(spec->begin_search_type != KSPEC_BS_INVALID);
        /* Skip specs that represent 'fake' keys */
        //跳过代表“假”密钥的规范
        if ((spec->flags & CMD_KEY_NOT_KEY) && !(search_flags & GET_KEYSPEC_INCLUDE_NOT_KEYS)) {
            continue;
        }

        first = 0;
        if (spec->begin_search_type == KSPEC_BS_INDEX) {
            first = spec->bs.index.pos;
        } else if (spec->begin_search_type == KSPEC_BS_KEYWORD) {
            int start_index = spec->bs.keyword.startfrom > 0 ? spec->bs.keyword.startfrom : argc+spec->bs.keyword.startfrom;
            int end_index = spec->bs.keyword.startfrom > 0 ? argc-1: 1;
            for (i = start_index; i != end_index; i = start_index <= end_index ? i + 1 : i - 1) {
                if (i >= argc || i < 1)
                    break;
                if (!strcasecmp((char*)argv[i]->ptr,spec->bs.keyword.keyword)) {
                    first = i+1;
                    break;
                }
            }
            /* keyword not found */
            if (!first) {
                continue;
            }
        } else {
            /* unknown spec */
            goto invalid_spec;
        }

        if (spec->find_keys_type == KSPEC_FK_RANGE) {
            step = spec->fk.range.keystep;
            if (spec->fk.range.lastkey >= 0) {
                last = first + spec->fk.range.lastkey;
            } else {
                if (!spec->fk.range.limit) {
                    last = argc + spec->fk.range.lastkey;
                } else {
                    serverAssert(spec->fk.range.lastkey == -1);
                    last = first + ((argc-first)/spec->fk.range.limit + spec->fk.range.lastkey);
                }
            }
        } else if (spec->find_keys_type == KSPEC_FK_KEYNUM) {
            step = spec->fk.keynum.keystep;
            long long numkeys;
            if (spec->fk.keynum.keynumidx >= argc)
                goto invalid_spec;

            sds keynum_str = argv[first + spec->fk.keynum.keynumidx]->ptr;
            if (!string2ll(keynum_str,sdslen(keynum_str),&numkeys) || numkeys < 0) {
                /* Unable to parse the numkeys argument or it was invalid */
                //无法解析 numkeys 参数或无效
                goto invalid_spec;
            }

            first += spec->fk.keynum.firstkey;
            last = first + (int)numkeys-1;
        } else {
            /* unknown spec 未知规格*/
            goto invalid_spec;
        }

        int count = ((last - first)+1);
        keys = getKeysPrepareResult(result, count);

        /* First or last is out of bounds, which indicates a syntax error */
        //第一个或最后一个超出范围，表示语法错误
        if (last >= argc || last < first || first >= argc) {
            goto invalid_spec;
        }

        for (i = first; i <= last; i += step) {
            if (i >= argc || i < first) {
                /* Modules commands, and standard commands with a not fixed number
                 * of arguments (negative arity parameter) do not have dispatch
                 * time arity checks, so we need to handle the case where the user
                 * passed an invalid number of arguments here. In this case we
                 * return no keys and expect the command implementation to report
                 * an arity or syntax error. */
                /**
                 * 模块命令和参数数量不固定（负参数）的标准命令没有调度时间参数检查，
                 * 因此我们需要处理用户在这里传递的参数数量无效的情况。在这种情况下，我们不返回任何键，
                 * 并期望命令实现报告一个arity 或语法错误。
                 * */
                if (cmd->flags & CMD_MODULE || cmd->arity < 0) {
                    continue;
                } else {
                    serverPanic("Redis built-in command declared keys positions not matching the arity requirements.");
                }
            }
            keys[k].pos = i;
            keys[k++].flags = spec->flags;
        }

        /* Handle incomplete specs (only after we added the current spec
         * to `keys`, just in case GET_KEYSPEC_RETURN_PARTIAL was given) */
        //处理不完整的规范（仅在我们将当前规范添加到 `keys` 之后，以防给出 GET_KEYSPEC_RETURN_PARTIAL）
        if (spec->flags & CMD_KEY_INCOMPLETE) {
            goto invalid_spec;
        }

        /* Done with this spec 完成此规范*/
        continue;

invalid_spec:
        if (search_flags & GET_KEYSPEC_RETURN_PARTIAL) {
            continue;
        } else {
            result->numkeys = 0;
            return -1;
        }
    }

    result->numkeys = k;
    return k;
}

/* Return all the arguments that are keys in the command passed via argc / argv. 
 * This function will eventually replace getKeysFromCommand.
 *
 * The command returns the positions of all the key arguments inside the array,
 * so the actual return value is a heap allocated array of integers. The
 * length of the array is returned by reference into *numkeys.
 * 
 * Along with the position, this command also returns the flags that are
 * associated with how Redis will access the key.
 *
 * 'cmd' must be point to the corresponding entry into the redisCommand
 * table, according to the command name in argv[0]. */
/**
 * 返回通过 argc argv 传递的命令中作为键的所有参数。该函数最终将取代 getKeysFromCommand。
 * 该命令返回数组中所有键参数的位置，因此实际返回值是堆分配的整数数组。数组的长度通过引用返回到 numkeys。
 * 除了位置，此命令还返回与 Redis 将如何访问密钥相关联的标志。
 * 'cmd' 必须根据 argv[0] 中的命令名称指向 redisCommand 表中的相应条目。
 * */
int getKeysFromCommandWithSpecs(struct redisCommand *cmd, robj **argv, int argc, int search_flags, getKeysResult *result) {
    /* The command has at least one key-spec not marked as NOT_KEY */
    //该命令至少有一个未标记为 NOT_KEY 的 key-spec
    int has_keyspec = (getAllKeySpecsFlags(cmd, 1) & CMD_KEY_NOT_KEY);
    /* The command has at least one key-spec marked as VARIABLE_FLAGS */
    //该命令至少有一个键规范标记为 VARIABLE_FLAGS
    int has_varflags = (getAllKeySpecsFlags(cmd, 0) & CMD_KEY_VARIABLE_FLAGS);

    /* Flags indicating that we have a getkeys callback */
    //指示我们有一个 getkeys 回调的标志
    int has_module_getkeys = cmd->flags & CMD_MODULE_GETKEYS;

    /* The key-spec that's auto generated by RM_CreateCommand sets VARIABLE_FLAGS since no flags are given.
     * If the module provides getkeys callback, we'll prefer it, but if it didn't, we'll use key-spec anyway. */
    //由 RM_CreateCommand 自动生成的 key-spec 设置 VARIABLE_FLAGS 因为没有给出标志。
    // 如果模块提供 getkeys 回调，我们会更喜欢它，但如果它没有，我们还是会使用 key-spec。
    if ((cmd->flags & CMD_MODULE) && has_varflags && !has_module_getkeys)
        has_varflags = 0;

    /* We prefer key-specs if there are any, and their flags are reliable. */
    //如果有的话，我们更喜欢关键规格，并且它们的标志是可靠的。
    if (has_keyspec && !has_varflags) {
        int ret = getKeysUsingKeySpecs(cmd,argv,argc,search_flags,result);
        if (ret >= 0)
            return ret;
        /* If the specs returned with an error (probably an INVALID or INCOMPLETE spec),
         * fallback to the callback method.
         * 如果规范返回错误（可能是 INVALID 或 INCOMPLETE 规范），则回退到回调方法。*/
    }

    /* Resort to getkeys callback methods. 诉诸 getkeys 回调方法。*/
    if (has_module_getkeys)
        return moduleGetCommandKeysViaAPI(cmd,argv,argc,result);

    /* We use native getkeys as a last resort, since not all these native getkeys provide
     * flags properly (only the ones that correspond to INVALID, INCOMPLETE or VARIABLE_FLAGS do.*/
    //我们使用原生 getkeys 作为最后的手段，因为并非所有这些原生 getkeys 都正确提供标志
    // （只有对应于 INVALID、INCOMPLETE 或 VARIABLE_FLAGS 的标志才提供。
    if (cmd->getkeys_proc)
        return cmd->getkeys_proc(cmd,argv,argc,result);
    return 0;
}

/* This function returns a sanity check if the command may have keys. */
//如果命令可能有键，此函数将返回健全性检查。
int doesCommandHaveKeys(struct redisCommand *cmd) {
    return cmd->getkeys_proc ||                                 /* has getkeys_proc (non modules) 有 getkeys_proc（非模块）*/
        (cmd->flags & CMD_MODULE_GETKEYS) ||                    /* module with GETKEYS */
        (getAllKeySpecsFlags(cmd, 1) & CMD_KEY_NOT_KEY);        /* has at least one key-spec not marked as NOT_KEY
 *                                                                      至少有一个 key-spec 未标记为 NOT_KEY*/
}

/* A simplified channel spec table that contains all of the redis commands
 * and which channels they have and how they are accessed. */
//一个简化的通道规格表，其中包含所有 redis 命令以及它们拥有哪些通道以及如何访问它们。
typedef struct ChannelSpecs {
    redisCommandProc *proc; /* Command procedure to match against 要匹配的命令过程*/
    uint64_t flags;         /* CMD_CHANNEL_* flags for this command 此命令的 CMD_CHANNEL_ 标志*/
    int start;              /* The initial position of the first channel 第一个通道的初始位置*/
    int count;              /* The number of channels, or -1 if all remaining
                             * arguments are channels. 通道数，如果所有剩余参数都是通道，则为 -1。*/
} ChannelSpecs;

ChannelSpecs commands_with_channels[] = {
    {subscribeCommand, CMD_CHANNEL_SUBSCRIBE, 1, -1},
    {ssubscribeCommand, CMD_CHANNEL_SUBSCRIBE, 1, -1},
    {unsubscribeCommand, CMD_CHANNEL_UNSUBSCRIBE, 1, -1},
    {sunsubscribeCommand, CMD_CHANNEL_UNSUBSCRIBE, 1, -1},
    {psubscribeCommand, CMD_CHANNEL_PATTERN | CMD_CHANNEL_SUBSCRIBE, 1, -1},
    {punsubscribeCommand, CMD_CHANNEL_PATTERN | CMD_CHANNEL_UNSUBSCRIBE, 1, -1},
    {publishCommand, CMD_CHANNEL_PUBLISH, 1, 1},
    {spublishCommand, CMD_CHANNEL_PUBLISH, 1, 1},
    {NULL,0} /* Terminator. */
};

/* Returns 1 if the command may access any channels matched by the flags
 * argument. */
//如果命令可以访问与 flags 参数匹配的任何通道，则返回 1。
int doesCommandHaveChannelsWithFlags(struct redisCommand *cmd, int flags) {
    /* If a module declares get channels, we are just going to assume
     * has channels. This API is allowed to return false positives. */
    //如果一个模块声明了获取通道，我们将假设有通道。此 API 允许返回误报。
    if (cmd->flags & CMD_MODULE_GETCHANNELS) {
        return 1;
    }
    for (ChannelSpecs *spec = commands_with_channels; spec->proc != NULL; spec += 1) {
        if (cmd->proc == spec->proc) {
            return !!(spec->flags & flags);
        }
    }
    return 0;
}

/* Return all the arguments that are channels in the command passed via argc / argv. 
 * This function behaves similar to getKeysFromCommandWithSpecs, but with channels 
 * instead of keys.
 * 
 * The command returns the positions of all the channel arguments inside the array,
 * so the actual return value is a heap allocated array of integers. The
 * length of the array is returned by reference into *numkeys.
 * 
 * Along with the position, this command also returns the flags that are
 * associated with how Redis will access the channel.
 *
 * 'cmd' must be point to the corresponding entry into the redisCommand
 * table, according to the command name in argv[0]. */
/**
 * 返回通过 argc argv 传递的命令中的所有通道参数。此函数的行为类似于 getKeysFromCommandWithSpecs，但使用通道而不是键。
 * 该命令返回数组中所有通道参数的位置，因此实际返回值是堆分配的整数数组。
 * 数组的长度通过引用返回到 numkeys。除了位置，此命令还返回与 Redis 将如何访问通道相关联的标志。
 * 'cmd' 必须根据 argv[0] 中的命令名称指向 redisCommand 表中的相应条目。
 * */
int getChannelsFromCommand(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    keyReference *keys;
    /* If a module declares get channels, use that. */
    //如果模块声明了获取通道，请使用它。
    if (cmd->flags & CMD_MODULE_GETCHANNELS) {
        return moduleGetCommandChannelsViaAPI(cmd, argv, argc, result);
    }
    /* Otherwise check the channel spec table */
    //否则检查通道规格表
    for (ChannelSpecs *spec = commands_with_channels; spec != NULL; spec += 1) {
        if (cmd->proc == spec->proc) {
            int start = spec->start;
            int stop = (spec->count == -1) ? argc : start + spec->count;
            if (stop > argc) stop = argc;
            int count = 0;
            keys = getKeysPrepareResult(result, stop - start);
            for (int i = start; i < stop; i++ ) {
                keys[count].pos = i;
                keys[count++].flags = spec->flags;
            }
            result->numkeys = count;
            return count;
        }
    }
    return 0;
}

/* The base case is to use the keys position as given in the command table
 * (firstkey, lastkey, step).
 * This function works only on command with the legacy_range_key_spec,
 * all other commands should be handled by getkeys_proc. 
 * 
 * If the commands keyspec is incomplete, no keys will be returned, and the provided
 * keys function should be called instead.
 * 
 * NOTE: This function does not guarantee populating the flags for 
 * the keys, in order to get flags you should use getKeysUsingKeySpecs. */
/**
 * 基本情况是使用命令表中给定的键位置（firstkey、lastkey、step）。
 * 此功能仅适用于带有 legacy_range_key_spec 的命令，所有其他命令应由 getkeys_proc 处理。
 * 如果命令 keyspec 不完整，则不会返回任何键，而是应调用提供的键函数。
 * 注意：此函数不保证填充键的标志，为了获得标志，您应该使用 getKeysUsingKeySpecs。
 * */
int getKeysUsingLegacyRangeSpec(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    int j, i = 0, last, first, step;
    keyReference *keys;
    UNUSED(argv);

    if (cmd->legacy_range_key_spec.begin_search_type == KSPEC_BS_INVALID) {
        result->numkeys = 0;
        return 0;
    }

    first = cmd->legacy_range_key_spec.bs.index.pos;
    last = cmd->legacy_range_key_spec.fk.range.lastkey;
    if (last >= 0)
        last += first;
    step = cmd->legacy_range_key_spec.fk.range.keystep;

    if (last < 0) last = argc+last;

    int count = ((last - first)+1);
    keys = getKeysPrepareResult(result, count);

    for (j = first; j <= last; j += step) {
        if (j >= argc || j < first) {
            /* Modules commands, and standard commands with a not fixed number
             * of arguments (negative arity parameter) do not have dispatch
             * time arity checks, so we need to handle the case where the user
             * passed an invalid number of arguments here. In this case we
             * return no keys and expect the command implementation to report
             * an arity or syntax error. */
            /**
             * 模块命令和参数数量不固定（负参数）的标准命令没有调度时间参数检查，
             * 因此我们需要处理用户在这里传递的参数数量无效的情况。在这种情况下，我们不返回任何键，
             * 并期望命令实现报告一个arity 或语法错误。
             * */
            if (cmd->flags & CMD_MODULE || cmd->arity < 0) {
                result->numkeys = 0;
                return 0;
            } else {
                serverPanic("Redis built-in command declared keys positions not matching the arity requirements.");
            }
        }
        keys[i].pos = j;
        /* Flags are omitted from legacy key specs */
        //遗留密钥规范中省略了标志
        keys[i++].flags = 0;
    }
    result->numkeys = i;
    return i;
}

/* Return all the arguments that are keys in the command passed via argc / argv.
 *
 * The command returns the positions of all the key arguments inside the array,
 * so the actual return value is a heap allocated array of integers. The
 * length of the array is returned by reference into *numkeys.
 *
 * 'cmd' must be point to the corresponding entry into the redisCommand
 * table, according to the command name in argv[0].
 *
 * This function uses the command table if a command-specific helper function
 * is not required, otherwise it calls the command-specific function. */
/**
 * 返回通过 argc argv 传递的命令中作为键的所有参数。该命令返回数组中所有键参数的位置，因此实际返回值是堆分配的整数数组。
 * 数组的长度通过引用返回到 numkeys。 'cmd' 必须根据 argv[0] 中的命令名称指向 redisCommand 表中的相应条目。
 * 如果不需要特定于命令的辅助函数，则此函数使用命令表，否则它调用特定于命令的函数。
 * */
int getKeysFromCommand(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    if (cmd->flags & CMD_MODULE_GETKEYS) {
        return moduleGetCommandKeysViaAPI(cmd,argv,argc,result);
    } else if (cmd->getkeys_proc) {
        return cmd->getkeys_proc(cmd,argv,argc,result);
    } else {
        return getKeysUsingLegacyRangeSpec(cmd,argv,argc,result);
    }
}

/* Free the result of getKeysFromCommand. */
//释放 getKeysFromCommand 的结果。
void getKeysFreeResult(getKeysResult *result) {
    if (result && result->keys != result->keysbuf)
        zfree(result->keys);
}

/* Helper function to extract keys from following commands:
 * COMMAND [destkey] <num-keys> <key> [...] <key> [...] ... <options>
 *
 * eg:
 * ZUNION <num-keys> <key> <key> ... <key> <options>
 * ZUNIONSTORE <destkey> <num-keys> <key> <key> ... <key> <options>
 *
 * 'storeKeyOfs': destkey index, 0 means destkey not exists.
 * 'keyCountOfs': num-keys index.
 * 'firstKeyOfs': firstkey index.
 * 'keyStep': the interval of each key, usually this value is 1.
 * 
 * The commands using this functoin have a fully defined keyspec, so returning flags isn't needed. */
/**
 * 从以下命令中提取键的辅助函数：
 * COMMAND [destkey] <num-keys> <key> [...] <key> [...] ... <options>
 * 例如：
 * ZUNION <num-keys> <key > <key> ... <key> <options>
 * ZUNIONSTORE <destkey> <num-keys> <key> <key> ... <key> <options>
 *   'storeKeyOfs'：destkey索引，0表示destkey不存在。
 *   'keyCountOfs'：键数索引。
 *   'firstKeyOfs'：第一键索引。
 *   'keyStep'：每个键的间隔，通常这个值是1。
 * 使用这个函数的命令有一个完全定义的keyspec，所以不需要返回标志。
 * */
int genericGetKeys(int storeKeyOfs, int keyCountOfs, int firstKeyOfs, int keyStep,
                    robj **argv, int argc, getKeysResult *result) {
    int i, num;
    keyReference *keys;

    num = atoi(argv[keyCountOfs]->ptr);
    /* Sanity check. Don't return any key if the command is going to
     * reply with syntax error. (no input keys). */
    //完整性检查。如果命令将返回语法错误，则不要返回任何键。 （没有输入键）。
    if (num < 1 || num > (argc - firstKeyOfs)/keyStep) {
        result->numkeys = 0;
        return 0;
    }

    int numkeys = storeKeyOfs ? num + 1 : num;
    keys = getKeysPrepareResult(result, numkeys);
    result->numkeys = numkeys;

    /* Add all key positions for argv[firstKeyOfs...n] to keys[] */
    //将 argv[firstKeyOfs...n] 的所有关键位置添加到 keys[]
    for (i = 0; i < num; i++) {
        keys[i].pos = firstKeyOfs+(i*keyStep);
        keys[i].flags = 0;
    } 

    if (storeKeyOfs) {
        keys[num].pos = storeKeyOfs;
        keys[num].flags = 0;
    } 
    return result->numkeys;
}

int sintercardGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    UNUSED(cmd);
    return genericGetKeys(0, 1, 2, 1, argv, argc, result);
}

int zunionInterDiffStoreGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    UNUSED(cmd);
    return genericGetKeys(1, 2, 3, 1, argv, argc, result);
}

int zunionInterDiffGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    UNUSED(cmd);
    return genericGetKeys(0, 1, 2, 1, argv, argc, result);
}

int evalGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    UNUSED(cmd);
    return genericGetKeys(0, 2, 3, 1, argv, argc, result);
}

int functionGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    UNUSED(cmd);
    return genericGetKeys(0, 2, 3, 1, argv, argc, result);
}

int lmpopGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    UNUSED(cmd);
    return genericGetKeys(0, 1, 2, 1, argv, argc, result);
}

int blmpopGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    UNUSED(cmd);
    return genericGetKeys(0, 2, 3, 1, argv, argc, result);
}

int zmpopGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    UNUSED(cmd);
    return genericGetKeys(0, 1, 2, 1, argv, argc, result);
}

int bzmpopGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    UNUSED(cmd);
    return genericGetKeys(0, 2, 3, 1, argv, argc, result);
}

/* Helper function to extract keys from the SORT RO command.
 *
 * SORT <sort-key>
 *
 * The second argument of SORT is always a key, however an arbitrary number of
 * keys may be accessed while doing the sort (the BY and GET args), so the
 * key-spec declares incomplete keys which is why we have to provide a concrete
 * implementation to fetch the keys.
 *
 * This command declares incomplete keys, so the flags are correctly set for this function */
/**
 * 从 SORT RO 命令中提取键的辅助函数。
 * SORT <sort-key>
 * SORT 的第二个参数始终是一个键，但是在进行排序时可以访问任意数量的键（BY 和 GET 参数），
 * 因此 key-spec 声明了不完整的键，这就是我们有提供一个具体的实现来获取密钥。
 * 该命令声明了不完整的键，因此为该函数正确设置了标志
 * */
int sortROGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    keyReference *keys;
    UNUSED(cmd);
    UNUSED(argv);
    UNUSED(argc);

    keys = getKeysPrepareResult(result, 1);
    keys[0].pos = 1; /* <sort-key> is always present. */
    keys[0].flags = CMD_KEY_RO | CMD_KEY_ACCESS;
    return 1;
}

/* Helper function to extract keys from the SORT command.
 *
 * SORT <sort-key> ... STORE <store-key> ...
 *
 * The first argument of SORT is always a key, however a list of options
 * follow in SQL-alike style. Here we parse just the minimum in order to
 * correctly identify keys in the "STORE" option. 
 * 
 * This command declares incomplete keys, so the flags are correctly set for this function */
/**
 * 从 SORT 命令中提取键的辅助函数。
 * SORT <sort-key> ... STORE <store-key> ...
 * SORT 的第一个参数始终是一个键，但是后面是一个类似 SQL 风格的选项列表。
 * 在这里，我们只解析最小值，以便正确识别“STORE”选项中的键。该命令声明了不完整的键，因此为该函数正确设置了标志
 * */
int sortGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    int i, j, num, found_store = 0;
    keyReference *keys;
    UNUSED(cmd);

    num = 0;
    keys = getKeysPrepareResult(result, 2); /* Alloc 2 places for the worst case. 为最坏的情况分配 2 个位置。*/
    keys[num].pos = 1; /* <sort-key> is always present. <sort-key> 始终存在。*/
    keys[num++].flags = CMD_KEY_RO | CMD_KEY_ACCESS;

    /* Search for STORE option. By default we consider options to don't
     * have arguments, so if we find an unknown option name we scan the
     * next. However there are options with 1 or 2 arguments, so we
     * provide a list here in order to skip the right number of args. */
    /**
     * 搜索 STORE 选项。默认情况下，我们认为选项没有参数，所以如果我们找到一个未知的选项名称，我们会扫描下一个。
     * 但是也有带有 1 或 2 个参数的选项，因此我们在此处提供一个列表以跳过正确数量的参数。
     * */
    struct {
        char *name;
        int skip;
    } skiplist[] = {
        {"limit", 2},
        {"get", 1},
        {"by", 1},
        {NULL, 0} /* End of elements. */
    };

    for (i = 2; i < argc; i++) {
        for (j = 0; skiplist[j].name != NULL; j++) {
            if (!strcasecmp(argv[i]->ptr,skiplist[j].name)) {
                i += skiplist[j].skip;
                break;
            } else if (!strcasecmp(argv[i]->ptr,"store") && i+1 < argc) {
                /* Note: we don't increment "num" here and continue the loop
                 * to be sure to process the *last* "STORE" option if multiple
                 * ones are provided. This is same behavior as SORT. */
                //注意：我们在这里不增加“num”并继续循环以确保处理最后一个“STORE”选项，如果提供了多个选项。这与 SORT 的行为相同。
                found_store = 1;
                keys[num].pos = i+1; /* <store-key> */
                keys[num].flags = CMD_KEY_OW | CMD_KEY_UPDATE;
                break;
            }
        }
    }
    result->numkeys = num + found_store;
    return result->numkeys;
}

/* This command declares incomplete keys, so the flags are correctly set for this function */
//该命令声明了不完整的键，因此为该函数正确设置了标志
int migrateGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    int i, num, first;
    keyReference *keys;
    UNUSED(cmd);

    /* Assume the obvious form. 采取明显的形式。*/
    first = 3;
    num = 1;

    /* But check for the extended one with the KEYS option. */
    //但是使用 KEYS 选项检查扩展的。
    if (argc > 6) {
        for (i = 6; i < argc; i++) {
            if (!strcasecmp(argv[i]->ptr,"keys") &&
                sdslen(argv[3]->ptr) == 0)
            {
                first = i+1;
                num = argc-first;
                break;
            }
        }
    }

    keys = getKeysPrepareResult(result, num);
    for (i = 0; i < num; i++) {
        keys[i].pos = first+i;
        keys[i].flags = CMD_KEY_RW | CMD_KEY_ACCESS | CMD_KEY_DELETE;
    } 
    result->numkeys = num;
    return num;
}

/* Helper function to extract keys from following commands:
 * 从以下命令中提取键的辅助函数：
 * GEORADIUS key x y radius unit [WITHDIST] [WITHHASH] [WITHCOORD] [ASC|DESC]
 *                             [COUNT count] [STORE key] [STOREDIST key]
 * GEORADIUSBYMEMBER key member radius unit ... options ...
 * 
 * This command has a fully defined keyspec, so returning flags isn't needed.
 * 此命令具有完全定义的密钥规范，因此不需要返回标志。*/
int georadiusGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    int i, num;
    keyReference *keys;
    UNUSED(cmd);

    /* Check for the presence of the stored key in the command */
    //检查命令中是否存在存储的密钥
    int stored_key = -1;
    for (i = 5; i < argc; i++) {
        char *arg = argv[i]->ptr;
        /* For the case when user specifies both "store" and "storedist" options, the
         * second key specified would override the first key. This behavior is kept
         * the same as in georadiusCommand method.
         */
        //对于用户同时指定“store”和“storedist”选项的情况，指定的第二个键将覆盖第一个键。
        // 此行为与 georadiusCommand 方法中的行为相同。
        if ((!strcasecmp(arg, "store") || !strcasecmp(arg, "storedist")) && ((i+1) < argc)) {
            stored_key = i+1;
            i++;
        }
    }
    num = 1 + (stored_key == -1 ? 0 : 1);

    /* Keys in the command come from two places:
     * 命令中的键来自两个地方：
     * argv[1] = key,
     * argv[5...n] = stored key if present
     */
    keys = getKeysPrepareResult(result, num);

    /* Add all key positions to keys[] */
    keys[0].pos = 1;
    keys[0].flags = 0;
    if(num > 1) {
         keys[1].pos = stored_key;
         keys[1].flags = 0;
    }
    result->numkeys = num;
    return num;
}

/* XREAD [BLOCK <milliseconds>] [COUNT <count>] [GROUP <groupname> <ttl>]
 *       STREAMS key_1 key_2 ... key_N ID_1 ID_2 ... ID_N
 *
 * This command has a fully defined keyspec, so returning flags isn't needed. */
//此命令具有完全定义的密钥规范，因此不需要返回标志。
int xreadGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    int i, num = 0;
    keyReference *keys;
    UNUSED(cmd);

    /* We need to parse the options of the command in order to seek the first
     * "STREAMS" string which is actually the option. This is needed because
     * "STREAMS" could also be the name of the consumer group and even the
     * name of the stream key. */
    //我们需要解析命令的选项以寻找第一个实际上是选项的“STREAMS”字符串。
    // 这是必需的，因为“STREAMS”也可以是消费者组的名称，甚至是流键的名称。
    int streams_pos = -1;
    for (i = 1; i < argc; i++) {
        char *arg = argv[i]->ptr;
        if (!strcasecmp(arg, "block")) {
            i++; /* Skip option argument. */
        } else if (!strcasecmp(arg, "count")) {
            i++; /* Skip option argument. */
        } else if (!strcasecmp(arg, "group")) {
            i += 2; /* Skip option argument. */
        } else if (!strcasecmp(arg, "noack")) {
            /* Nothing to do. */
        } else if (!strcasecmp(arg, "streams")) {
            streams_pos = i;
            break;
        } else {
            break; /* Syntax error. */
        }
    }
    if (streams_pos != -1) num = argc - streams_pos - 1;

    /* Syntax error. */
    if (streams_pos == -1 || num == 0 || num % 2 != 0) {
        result->numkeys = 0;
        return 0;
    }
    num /= 2; /* We have half the keys as there are arguments because
                 there are also the IDs, one per key.
                 我们有一半的键，因为有参数，因为还有 ID，每个键一个。*/

    keys = getKeysPrepareResult(result, num);
    for (i = streams_pos+1; i < argc-num; i++) {
        keys[i-streams_pos-1].pos = i;
        keys[i-streams_pos-1].flags = 0; 
    } 
    result->numkeys = num;
    return num;
}

/* Helper function to extract keys from the SET command, which may have
 * a read flag if the GET argument is passed in. */
//从 SET 命令中提取密钥的辅助函数，如果传入 GET 参数，该命令可能具有读取标志。
int setGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    keyReference *keys;
    UNUSED(cmd);

    keys = getKeysPrepareResult(result, 1);
    keys[0].pos = 1; /* We always know the position */
    result->numkeys = 1;

    for (int i = 3; i < argc; i++) {
        char *arg = argv[i]->ptr;
        if ((arg[0] == 'g' || arg[0] == 'G') &&
            (arg[1] == 'e' || arg[1] == 'E') &&
            (arg[2] == 't' || arg[2] == 'T') && arg[3] == '\0')
        {
            keys[0].flags = CMD_KEY_RW | CMD_KEY_ACCESS | CMD_KEY_UPDATE;
            return 1;
        }
    }

    keys[0].flags = CMD_KEY_OW | CMD_KEY_UPDATE;
    return 1;
}

/* Helper function to extract keys from the BITFIELD command, which may be
 * read-only if the BITFIELD GET subcommand is used. */
//从 BITFIELD 命令中提取密钥的辅助函数，如果使用 BITFIELD GET 子命令，该命令可能是只读的。
int bitfieldGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    keyReference *keys;
    UNUSED(cmd);

    keys = getKeysPrepareResult(result, 1);
    keys[0].pos = 1; /* We always know the position 我们总是知道位置*/
    result->numkeys = 1;

    for (int i = 2; i < argc; i++) {
        int remargs = argc - i - 1; /* Remaining args other than current. 除当前以外的其余参数。*/
        char *arg = argv[i]->ptr;
        if (!strcasecmp(arg, "get") && remargs >= 2) {
            keys[0].flags = CMD_KEY_RO | CMD_KEY_ACCESS;
            return 1;
        }
    }

    keys[0].flags = CMD_KEY_RW | CMD_KEY_ACCESS | CMD_KEY_UPDATE;
    return 1;
}
