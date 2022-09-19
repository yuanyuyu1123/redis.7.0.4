/*
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
#include "monotonic.h"
#include "cluster.h"
#include "slowlog.h"
#include "bio.h"
#include "latency.h"
#include "atomicvar.h"
#include "mt19937-64.h"
#include "functions.h"
#include "syscheck.h"

#include <time.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <limits.h>
#include <float.h>
#include <math.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <locale.h>
#include <sys/socket.h>
#include <sys/resource.h>

#ifdef __linux__
#include <sys/mman.h>
#endif

#if defined(HAVE_SYSCTL_KIPC_SOMAXCONN) || defined(HAVE_SYSCTL_KERN_SOMAXCONN)
#include <sys/sysctl.h>
#endif

/* Our shared "common" objects */

struct sharedObjectsStruct shared;

/* Global vars that are actually used as constants. The following double
 * values are used for double on-disk serialization, and are initialized
 * at runtime to avoid strange compiler optimizations. */
/**
 * 实际用作常量的全局变量。以下双精度值用于双磁盘序列化，并在运行时初始化以避免奇怪的编译器优化。
 * */

double R_Zero, R_PosInf, R_NegInf, R_Nan;

/*================================= Globals ================================= */

/* Global vars */
struct redisServer server; /* Server global state */

/*============================ Internal prototypes ========================== */

static inline int isShutdownInitiated(void);
int isReadyToShutdown(void);
int finishShutdown(void);
const char *replstateToString(int replstate);

/*============================ Utility functions ============================ */

/* We use a private localtime implementation which is fork-safe. The logging
 * function of Redis may be called from other threads. */
//我们使用分叉安全的私有本地时间实现。 Redis 的日志功能可能会被其他线程调用。
void nolocks_localtime(struct tm *tmp, time_t t, time_t tz, int dst);

/* Low level logging. To use only for very big messages, otherwise
 * serverLog() is to prefer. */
//低级日志记录。仅用于非常大的消息，否则 serverLog() 是首选。
void serverLogRaw(int level, const char *msg) {
    const int syslogLevelMap[] = { LOG_DEBUG, LOG_INFO, LOG_NOTICE, LOG_WARNING };
    const char *c = ".-*#";
    FILE *fp;
    char buf[64];
    int rawmode = (level & LL_RAW);
    int log_to_stdout = server.logfile[0] == '\0';

    level &= 0xff; /* clear flags */
    if (level < server.verbosity) return;

    fp = log_to_stdout ? stdout : fopen(server.logfile,"a");
    if (!fp) return;

    if (rawmode) {
        fprintf(fp,"%s",msg);
    } else {
        int off;
        struct timeval tv;
        int role_char;
        pid_t pid = getpid();

        gettimeofday(&tv,NULL);
        struct tm tm;
        nolocks_localtime(&tm,tv.tv_sec,server.timezone,server.daylight_active);
        off = strftime(buf,sizeof(buf),"%d %b %Y %H:%M:%S.",&tm);
        snprintf(buf+off,sizeof(buf)-off,"%03d",(int)tv.tv_usec/1000);
        if (server.sentinel_mode) {
            role_char = 'X'; /* Sentinel. */
        } else if (pid != server.pid) {
            role_char = 'C'; /* RDB / AOF writing child. */
        } else {
            role_char = (server.masterhost ? 'S':'M'); /* Slave or Master. */
        }
        fprintf(fp,"%d:%c %s %c %s\n",
            (int)getpid(),role_char, buf,c[level],msg);
    }
    fflush(fp);

    if (!log_to_stdout) fclose(fp);
    if (server.syslog_enabled) syslog(syslogLevelMap[level], "%s", msg);
}

/* Like serverLogRaw() but with printf-alike support. This is the function that
 * is used across the code. The raw version is only used in order to dump
 * the INFO output on crash. */
//与 serverLogRaw() 类似，但具有类似 printf 的支持。这是在代码中使用的函数。原始版本仅用于在崩溃时转储 INFO 输出。
void _serverLog(int level, const char *fmt, ...) {
    va_list ap;
    char msg[LOG_MAX_LEN];

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    serverLogRaw(level,msg);
}

/* Log a fixed message without printf-alike capabilities, in a way that is
 * safe to call from a signal handler.
 *
 * We actually use this only for signals that are not fatal from the point
 * of view of Redis. Signals that are going to kill the server anyway and
 * where we need printf-alike features are served by serverLog(). */
/**
 * 以从信号处理程序安全调用的方式记录没有类似 printf 功能的固定消息。
 * 我们实际上只将它用于从 Redis 的角度来看不是致命的信号。
 * 无论如何都会杀死服务器的信号以及我们需要类似 printf 功能的地方由 serverLog() 提供。
 * */
void serverLogFromHandler(int level, const char *msg) {
    int fd;
    int log_to_stdout = server.logfile[0] == '\0';
    char buf[64];

    if ((level&0xff) < server.verbosity || (log_to_stdout && server.daemonize))
        return;
    fd = log_to_stdout ? STDOUT_FILENO :
                         open(server.logfile, O_APPEND|O_CREAT|O_WRONLY, 0644);
    if (fd == -1) return;
    ll2string(buf,sizeof(buf),getpid());
    if (write(fd,buf,strlen(buf)) == -1) goto err;
    if (write(fd,":signal-handler (",17) == -1) goto err;
    ll2string(buf,sizeof(buf),time(NULL));
    if (write(fd,buf,strlen(buf)) == -1) goto err;
    if (write(fd,") ",2) == -1) goto err;
    if (write(fd,msg,strlen(msg)) == -1) goto err;
    if (write(fd,"\n",1) == -1) goto err;
err:
    if (!log_to_stdout) close(fd);
}

/* Return the UNIX time in microseconds */
long long ustime(void) {
    struct timeval tv;
    long long ust;

    gettimeofday(&tv, NULL);
    ust = ((long long)tv.tv_sec)*1000000;
    ust += tv.tv_usec;
    return ust;
}

/* Return the UNIX time in milliseconds */
mstime_t mstime(void) {
    return ustime()/1000;
}

/* After an RDB dump or AOF rewrite we exit from children using _exit() instead of
 * exit(), because the latter may interact with the same file objects used by
 * the parent process. However if we are testing the coverage normal exit() is
 * used in order to obtain the right coverage information. */
/**
 * 在 RDB 转储或 AOF 重写之后，我们使用 _exit() 而不是 exit() 从子进程中退出，因为后者可能与父进程使用的相同文件对象交互。
 * 但是，如果我们正在测试覆盖率，则使用普通 exit() 以获得正确的覆盖率信息。
 * */
void exitFromChild(int retcode) {
#ifdef COVERAGE_TEST
    exit(retcode);
#else
    _exit(retcode);
#endif
}

/*====================== Hash table type implementation  ==================== */

/* This is a hash table type that uses the SDS dynamic strings library as
 * keys and redis objects as values (objects can hold SDS strings,
 * lists, sets). */
/**
 * 这是一种哈希表类型，使用 SDS 动态字符串库作为键，使用 redis 对象作为值（对象可以保存 SDS 字符串、列表、集合）。
 * */

void dictVanillaFree(dict *d, void *val)
{
    UNUSED(d);
    zfree(val);
}

void dictListDestructor(dict *d, void *val)
{
    UNUSED(d);
    listRelease((list*)val);
}

int dictSdsKeyCompare(dict *d, const void *key1,
        const void *key2)
{
    int l1,l2;
    UNUSED(d);

    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

/* A case insensitive version used for the command lookup table and other
 * places where case insensitive non binary-safe comparison is needed. */
//不区分大小写的版本，用于命令查找表和其他需要不区分大小写的非二进制安全比较的地方。
int dictSdsKeyCaseCompare(dict *d, const void *key1,
        const void *key2)
{
    UNUSED(d);
    return strcasecmp(key1, key2) == 0;
}

void dictObjectDestructor(dict *d, void *val)
{
    UNUSED(d);
    if (val == NULL) return; /* Lazy freeing will set value to NULL. 延迟释放会将值设置为 NULL。*/
    decrRefCount(val);
}

void dictSdsDestructor(dict *d, void *val)
{
    UNUSED(d);
    sdsfree(val);
}

void *dictSdsDup(dict *d, const void *key) {
    UNUSED(d);
    return sdsdup((const sds) key);
}

int dictObjKeyCompare(dict *d, const void *key1,
        const void *key2)
{
    const robj *o1 = key1, *o2 = key2;
    return dictSdsKeyCompare(d, o1->ptr,o2->ptr);
}

uint64_t dictObjHash(const void *key) {
    const robj *o = key;
    return dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
}

uint64_t dictSdsHash(const void *key) {
    return dictGenHashFunction((unsigned char*)key, sdslen((char*)key));
}

uint64_t dictSdsCaseHash(const void *key) {
    return dictGenCaseHashFunction((unsigned char*)key, sdslen((char*)key));
}

/* Dict hash function for null terminated string */
//空终止字符串的字典哈希函数
uint64_t distCStrHash(const void *key) {
    return dictGenHashFunction((unsigned char*)key, strlen((char*)key));
}

/* Dict hash function for null terminated string */
//空终止字符串的字典哈希函数
uint64_t distCStrCaseHash(const void *key) {
    return dictGenCaseHashFunction((unsigned char*)key, strlen((char*)key));
}

/* Dict compare function for null terminated string */
int distCStrKeyCompare(dict *d, const void *key1, const void *key2) {
    int l1,l2;
    UNUSED(d);

    l1 = strlen((char*)key1);
    l2 = strlen((char*)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

/* Dict case insensitive compare function for null terminated string */
//空终止字符串的字典不区分大小写比较函数
int distCStrKeyCaseCompare(dict *d, const void *key1, const void *key2) {
    UNUSED(d);
    return strcasecmp(key1, key2) == 0;
}

int dictEncObjKeyCompare(dict *d, const void *key1, const void *key2)
{
    robj *o1 = (robj*) key1, *o2 = (robj*) key2;
    int cmp;

    if (o1->encoding == OBJ_ENCODING_INT &&
        o2->encoding == OBJ_ENCODING_INT)
            return o1->ptr == o2->ptr;

    /* Due to OBJ_STATIC_REFCOUNT, we avoid calling getDecodedObject() without
     * good reasons, because it would incrRefCount() the object, which
     * is invalid. So we check to make sure dictFind() works with static
     * objects as well. */
    /**
     * 由于 OBJ_STATIC_REFCOUNT，我们避免在没有充分理由的情况下调用 getDecodedObject()，因为它会增加无效的对象。
     * 所以我们检查以确保 dictFind() 也适用于静态对象。
     * */
    if (o1->refcount != OBJ_STATIC_REFCOUNT) o1 = getDecodedObject(o1);
    if (o2->refcount != OBJ_STATIC_REFCOUNT) o2 = getDecodedObject(o2);
    cmp = dictSdsKeyCompare(d,o1->ptr,o2->ptr);
    if (o1->refcount != OBJ_STATIC_REFCOUNT) decrRefCount(o1);
    if (o2->refcount != OBJ_STATIC_REFCOUNT) decrRefCount(o2);
    return cmp;
}

uint64_t dictEncObjHash(const void *key) {
    robj *o = (robj*) key;

    if (sdsEncodedObject(o)) {
        return dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
    } else if (o->encoding == OBJ_ENCODING_INT) {
        char buf[32];
        int len;

        len = ll2string(buf,32,(long)o->ptr);
        return dictGenHashFunction((unsigned char*)buf, len);
    } else {
        serverPanic("Unknown string encoding");
    }
}

/* Return 1 if currently we allow dict to expand. Dict may allocate huge
 * memory to contain hash buckets when dict expands, that may lead redis
 * rejects user's requests or evicts some keys, we can stop dict to expand
 * provisionally if used memory will be over maxmemory after dict expands,
 * but to guarantee the performance of redis, we still allow dict to expand
 * if dict load factor exceeds HASHTABLE_MAX_LOAD_FACTOR. */
/**
 * 如果当前我们允许 dict 扩展，则返回 1。 dict扩展时，dict可能会分配巨大的内存来容纳hash bucket，
 * 这可能导致redis拒绝用户的请求或驱逐一些key，如果dict扩展后使用的内存超过maxmemory，
 * 我们可以暂时停止dict扩展，但为了保证redis的性能，如果dict加载因子超过HASHTABLE_MAX_LOAD_FACTOR，我们仍然允许dict扩展。
 * */
int dictExpandAllowed(size_t moreMem, double usedRatio) {
    if (usedRatio <= HASHTABLE_MAX_LOAD_FACTOR) {
        return !overMaxmemoryAfterAlloc(moreMem);
    } else {
        return 1;
    }
}

/* Returns the size of the DB dict entry metadata in bytes. In cluster mode, the
 * metadata is used for constructing a doubly linked list of the dict entries
 * belonging to the same cluster slot. See the Slot to Key API in cluster.c. */
/**
 * 返回 DB dict 条目元数据的大小（以字节为单位）。在集群模式下，元数据用于构建属于同一集群槽的字典条目的双向链表。
 * 请参阅 cluster.c 中的 Slot to Key API。
 * */
size_t dictEntryMetadataSize(dict *d) {
    UNUSED(d);
    /* NOTICE: this also affects overhead_ht_slot_to_keys in getMemoryOverheadData.
     * If we ever add non-cluster related data here, that code must be modified too. */
    /**
     * 注意：这也会影响 getMemoryOverheadData 中的overhead_ht_slot_to_keys。
     * 如果我们在这里添加非集群相关的数据，那么也必须修改该代码。
     * */
    return server.cluster_enabled ? sizeof(clusterDictEntryMetadata) : 0;
}

/* Generic hash table type where keys are Redis Objects, Values
 * dummy pointers. */
//通用哈希表类型，其中键是 Redis 对象，值是虚拟指针。
dictType objectKeyPointerValueDictType = {
    dictEncObjHash,            /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictEncObjKeyCompare,      /* key compare */
    dictObjectDestructor,      /* key destructor */
    NULL,                      /* val destructor */
    NULL                       /* allow to expand */
};

/* Like objectKeyPointerValueDictType(), but values can be destroyed, if
 * not NULL, calling zfree(). */
//与 objectKeyPointerValueDictType() 类似，但值可以被销毁，如果不是 NULL，则调用 zfree()。
dictType objectKeyHeapPointerValueDictType = {
    dictEncObjHash,            /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictEncObjKeyCompare,      /* key compare */
    dictObjectDestructor,      /* key destructor */
    dictVanillaFree,           /* val destructor */
    NULL                       /* allow to expand */
};

/* Set dictionary type. Keys are SDS strings, values are not used. */
//设置字典类型。键是 SDS 字符串，不使用值。
dictType setDictType = {
    dictSdsHash,               /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictSdsKeyCompare,         /* key compare */
    dictSdsDestructor,         /* key destructor */
    NULL                       /* val destructor */
};

/* Sorted sets hash (note: a skiplist is used in addition to the hash table) */
//排序集散列（注意：除了散列表外，还使用了跳过列表）
dictType zsetDictType = {
    dictSdsHash,               /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictSdsKeyCompare,         /* key compare */
    NULL,                      /* Note: SDS string shared & freed by skiplist */
    NULL,                      /* val destructor */
    NULL                       /* allow to expand */
};

/* Db->dict, keys are sds strings, vals are Redis objects. */
//db->dict，keys 是 sds 字符串，vals 是 Redis 对象。
dictType dbDictType = {
    dictSdsHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCompare,          /* key compare */
    dictSdsDestructor,          /* key destructor */
    dictObjectDestructor,       /* val destructor */
    dictExpandAllowed,          /* allow to expand */
    dictEntryMetadataSize       /* size of entry metadata in bytes */
};

/* Db->expires */
dictType dbExpiresDictType = {
    dictSdsHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCompare,          /* key compare */
    NULL,                       /* key destructor */
    NULL,                       /* val destructor */
    dictExpandAllowed           /* allow to expand */
};

/* Command table. sds string -> command struct pointer. */
//命令表。 sds 字符串 -> 命令结构指针。
dictType commandTableDictType = {
    dictSdsCaseHash,            /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCaseCompare,      /* key compare */
    dictSdsDestructor,          /* key destructor */
    NULL,                       /* val destructor */
    NULL                        /* allow to expand */
};

/* Hash type hash table (note that small hashes are represented with listpacks) */
//散列类型散列表（注意小散列用listpacks表示）
dictType hashDictType = {
    dictSdsHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCompare,          /* key compare */
    dictSdsDestructor,          /* key destructor */
    dictSdsDestructor,          /* val destructor */
    NULL                        /* allow to expand */
};

/* Dict type without destructor 没有析构函数的字典类型 */
dictType sdsReplyDictType = {
    dictSdsHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCompare,          /* key compare */
    NULL,                       /* key destructor */
    NULL,                       /* val destructor */
    NULL                        /* allow to expand */
};

/* Keylist hash table type has unencoded redis objects as keys and
 * lists as values. It's used for blocking operations (BLPOP) and to
 * map swapped keys to a list of clients waiting for this keys to be loaded. */
/**Keylist 哈希表类型将未编码的 redis 对象作为键，将列表作为值。
 * 它用于阻塞操作 (BLPOP) 并将交换的密钥映射到等待加载此密钥的客户端列表。*/
dictType keylistDictType = {
    dictObjHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictObjKeyCompare,          /* key compare */
    dictObjectDestructor,       /* key destructor */
    dictListDestructor,         /* val destructor */
    NULL                        /* allow to expand */
};

/* Modules system dictionary type. Keys are module name,
 * values are pointer to RedisModule struct. */
/**模块系统字典类型。键是模块名称，值是指向 RedisModule 结构的指针。*/
dictType modulesDictType = {
    dictSdsCaseHash,            /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCaseCompare,      /* key compare */
    dictSdsDestructor,          /* key destructor */
    NULL,                       /* val destructor */
    NULL                        /* allow to expand */
};

/* Migrate cache dict type.迁移缓存字典类型。 */
dictType migrateCacheDictType = {
    dictSdsHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCompare,          /* key compare */
    dictSdsDestructor,          /* key destructor */
    NULL,                       /* val destructor */
    NULL                        /* allow to expand */
};

/* Dict for for case-insensitive search using null terminated C strings.
 * The keys stored in dict are sds though. */
/**使用空终止的 C 字符串进行不区分大小写的搜索的字典。存储在 dict 中的密钥虽然是 sds。*/
dictType stringSetDictType = {
    distCStrCaseHash,           /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    distCStrKeyCaseCompare,     /* key compare */
    dictSdsDestructor,          /* key destructor */
    NULL,                       /* val destructor */
    NULL                        /* allow to expand */
};

/* Dict for for case-insensitive search using null terminated C strings.
 * The key and value do not have a destructor. */
/**使用空终止的 C 字符串进行不区分大小写的搜索的字典。键和值没有析构函数。*/
dictType externalStringType = {
    distCStrCaseHash,           /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    distCStrKeyCaseCompare,     /* key compare */
    NULL,                       /* key destructor */
    NULL,                       /* val destructor */
    NULL                        /* allow to expand */
};

/* Dict for case-insensitive search using sds objects with a zmalloc
 * allocated object as the value. */
/**使用带有 zmalloc 分配对象作为值的 sds 对象进行不区分大小写搜索的字典。*/
dictType sdsHashDictType = {
    dictSdsCaseHash,            /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCaseCompare,      /* key compare */
    dictSdsDestructor,          /* key destructor */
    dictVanillaFree,            /* val destructor */
    NULL                        /* allow to expand */
};

int htNeedsResize(dict *dict) {
    long long size, used;

    size = dictSlots(dict);
    used = dictSize(dict);
    return (size > DICT_HT_INITIAL_SIZE &&
            (used*100/size < HASHTABLE_MIN_FILL));
}

/* If the percentage of used slots in the HT reaches HASHTABLE_MIN_FILL
 * we resize the hash table to save memory */
/**如果 HT 中已用插槽的百分比达到 HASHTABLE_MIN_FILL 我们调整哈希表的大小以节省内存*/
void tryResizeHashTables(int dbid) {
    if (htNeedsResize(server.db[dbid].dict))
        dictResize(server.db[dbid].dict);
    if (htNeedsResize(server.db[dbid].expires))
        dictResize(server.db[dbid].expires);
}

/* Our hash table implementation performs rehashing incrementally while
 * we write/read from the hash table. Still if the server is idle, the hash
 * table will use two tables for a long time. So we try to use 1 millisecond
 * of CPU time at every call of this function to perform some rehashing.
 *
 * The function returns 1 if some rehashing was performed, otherwise 0
 * is returned. */
/**当我们从哈希表中写入读取时，我们的哈希表实现会逐步执行重新哈希。
 * 仍然如果服务器空闲，哈希表会长时间使用两个表。
 * 因此，我们尝试在每次调用此函数时使用 1 毫秒的 CPU 时间来执行一些重新散列。
 * 如果执行了一些重新散列，则该函数返回 1，否则返回 0。*/
int incrementallyRehash(int dbid) {
    /* Keys dictionary */
    if (dictIsRehashing(server.db[dbid].dict)) {
        dictRehashMilliseconds(server.db[dbid].dict,1);
        return 1; /* already used our millisecond for this loop... 已经在这个循环中使用了我们的毫秒......*/
    }
    /* Expires */
    if (dictIsRehashing(server.db[dbid].expires)) {
        dictRehashMilliseconds(server.db[dbid].expires,1);
        return 1; /* already used our millisecond for this loop... 已经在这个循环中使用了我们的毫秒......*/
    }
    return 0;
}

/* This function is called once a background process of some kind terminates,
 * as we want to avoid resizing the hash tables when there is a child in order
 * to play well with copy-on-write (otherwise when a resize happens lots of
 * memory pages are copied). The goal of this function is to update the ability
 * for dict.c to resize the hash tables accordingly to the fact we have an
 * active fork child running. */
/**一旦某种类型的后台进程终止，就会调用此函数，因为我们希望避免在有子进程时调整哈希表的大小，
 * 以便更好地使用写时复制（否则当发生调整大小时，会复制大量内存页面）。
 * 这个函数的目标是更新 dict.c 的能力，以根据我们有一个活动的 fork 子运行的事实来调整哈希表的大小。*/
void updateDictResizePolicy(void) {
    if (!hasActiveChildProcess())
        dictEnableResize();
    else
        dictDisableResize();
}

const char *strChildType(int type) {
    switch(type) {
        case CHILD_TYPE_RDB: return "RDB";
        case CHILD_TYPE_AOF: return "AOF";
        case CHILD_TYPE_LDB: return "LDB";
        case CHILD_TYPE_MODULE: return "MODULE";
        default: return "Unknown";
    }
}

/* Return true if there are active children processes doing RDB saving,
 * AOF rewriting, or some side process spawned by a loaded module. */
/**如果有活动的子进程正在执行 RDB 保存、AOF 重写或由加载的模块产生的某些副进程，则返回 true。*/
int hasActiveChildProcess() {
    return server.child_pid != -1;
}

void resetChildState() {
    server.child_type = CHILD_TYPE_NONE;
    server.child_pid = -1;
    server.stat_current_cow_peak = 0;
    server.stat_current_cow_bytes = 0;
    server.stat_current_cow_updated = 0;
    server.stat_current_save_keys_processed = 0;
    server.stat_module_progress = 0;
    server.stat_current_save_keys_total = 0;
    updateDictResizePolicy();
    closeChildInfoPipe();
    moduleFireServerEvent(REDISMODULE_EVENT_FORK_CHILD,
                          REDISMODULE_SUBEVENT_FORK_CHILD_DIED,
                          NULL);
}

/* Return if child type is mutually exclusive with other fork children */
/**如果子类型与其他 fork 子类型互斥，则返回*/
int isMutuallyExclusiveChildType(int type) {
    return type == CHILD_TYPE_RDB || type == CHILD_TYPE_AOF || type == CHILD_TYPE_MODULE;
}

/* Returns true when we're inside a long command that yielded to the event loop. */
/**当我们在一个屈服于事件循环的长命令中时返回 true。*/
int isInsideYieldingLongCommand() {
    return scriptIsTimedout() || server.busy_module_yield_flags;
}

/* Return true if this instance has persistence completely turned off:
 * both RDB and AOF are disabled. */
/**如果此实例完全关闭持久性，则返回 true：RDB 和 AOF 都被禁用。*/
int allPersistenceDisabled(void) {
    return server.saveparamslen == 0 && server.aof_state == AOF_OFF;
}

/* ======================= Cron: called every 100 ms ======================== */

/* Add a sample to the operations per second array of samples. */
/**将样本添加到每秒操作的样本数组中。*/
void trackInstantaneousMetric(int metric, long long current_reading) {
    long long now = mstime();
    long long t = now - server.inst_metric[metric].last_sample_time;
    long long ops = current_reading -
                    server.inst_metric[metric].last_sample_count;
    long long ops_sec;

    ops_sec = t > 0 ? (ops*1000/t) : 0;

    server.inst_metric[metric].samples[server.inst_metric[metric].idx] =
        ops_sec;
    server.inst_metric[metric].idx++;
    server.inst_metric[metric].idx %= STATS_METRIC_SAMPLES;
    server.inst_metric[metric].last_sample_time = now;
    server.inst_metric[metric].last_sample_count = current_reading;
}

/* Return the mean of all the samples. */
/**返回所有样本的平均值。*/
long long getInstantaneousMetric(int metric) {
    int j;
    long long sum = 0;

    for (j = 0; j < STATS_METRIC_SAMPLES; j++)
        sum += server.inst_metric[metric].samples[j];
    return sum / STATS_METRIC_SAMPLES;
}

/* The client query buffer is an sds.c string that can end with a lot of
 * free space not used, this function reclaims space if needed.
 *
 * The function always returns 0 as it never terminates the client. */
/**客户端查询缓冲区是一个 sds.c 字符串，可以以大量未使用的可用空间结尾，此函数在需要时回收空间。
 * 该函数始终返回 0，因为它从不终止客户端。*/
int clientsCronResizeQueryBuffer(client *c) {
    size_t querybuf_size = sdsalloc(c->querybuf);
    time_t idletime = server.unixtime - c->lastinteraction;

    /* Only resize the query buffer if the buffer is actually wasting at least a
     * few kbytes */
    /**仅当缓冲区实际上浪费了至少几 kbytes 时才调整查询缓冲区的大小*/
    if (sdsavail(c->querybuf) > 1024*4) {
        /* There are two conditions to resize the query buffer: */
        /**调整查询缓冲区的大小有两个条件：*/
        if (idletime > 2) {
            /* 1) Query is idle for a long time. */
            /**1）查询长时间空闲。*/
            c->querybuf = sdsRemoveFreeSpace(c->querybuf);
        } else if (querybuf_size > PROTO_RESIZE_THRESHOLD && querybuf_size/2 > c->querybuf_peak) {
            /* 2) Query buffer is too big for latest peak and is larger than
             *    resize threshold. Trim excess space but only up to a limit,
             *    not below the recent peak and current c->querybuf (which will
             *    be soon get used). If we're in the middle of a bulk then make
             *    sure not to resize to less than the bulk length. */
            /**2) 查询缓冲区对于最新峰值来说太大并且大于调整大小阈值。
             * 修剪多余的空间，但只能达到一个限制，不低于最近的峰值和当前的 c->querybuf（很快就会被使用）。
             * 如果我们处于散装的中间，请确保不要将大小调整为小于散装长度。*/
            size_t resize = sdslen(c->querybuf);
            if (resize < c->querybuf_peak) resize = c->querybuf_peak;
            if (c->bulklen != -1 && resize < (size_t)c->bulklen) resize = c->bulklen;
            c->querybuf = sdsResize(c->querybuf, resize);
        }
    }

    /* Reset the peak again to capture the peak memory usage in the next
     * cycle. */
    /**再次重置峰值以捕获下一个周期的峰值内存使用量。*/
    c->querybuf_peak = sdslen(c->querybuf);
    /* We reset to either the current used, or currently processed bulk size,
     * which ever is bigger. */
    /**我们重置为当前使用的或当前处理的批量大小，以较大者为准。*/
    if (c->bulklen != -1 && (size_t)c->bulklen > c->querybuf_peak)
        c->querybuf_peak = c->bulklen;
    return 0;
}

/* The client output buffer can be adjusted to better fit the memory requirements.
 *
 * the logic is:
 * in case the last observed peak size of the buffer equals the buffer size - we double the size
 * in case the last observed peak size of the buffer is less than half the buffer size - we shrink by half.
 * The buffer peak will be reset back to the buffer position every server.reply_buffer_peak_reset_time milliseconds
 * The function always returns 0 as it never terminates the client. */
/**可以调整客户端输出缓冲区以更好地满足内存要求。
 * 逻辑是：如果缓冲区的最后观察到的峰值大小等于缓冲区大小
 *  - 我们将大小加倍以防缓冲区的最后观察到的峰值大小小于缓冲区大小的一半
 *  - 我们缩小一半。
 *  缓冲区峰值将在每个 server.reply_buffer_peak_reset_time 毫秒重置回缓冲区位置 该函数始终返回 0，
 *  因为它永远不会终止客户端。*/
int clientsCronResizeOutputBuffer(client *c, mstime_t now_ms) {

    size_t new_buffer_size = 0;
    char *oldbuf = NULL;
    const size_t buffer_target_shrink_size = c->buf_usable_size/2;
    const size_t buffer_target_expand_size = c->buf_usable_size*2;

    /* in case the resizing is disabled return immediately */
    //如果禁用调整大小，则立即返回
    if(!server.reply_buffer_resizing_enabled)
        return 0;

    if (buffer_target_shrink_size >= PROTO_REPLY_MIN_BYTES &&
        c->buf_peak < buffer_target_shrink_size )
    {
        new_buffer_size = max(PROTO_REPLY_MIN_BYTES,c->buf_peak+1);
        server.stat_reply_buffer_shrinks++;
    } else if (buffer_target_expand_size < PROTO_REPLY_CHUNK_BYTES*2 &&
        c->buf_peak == c->buf_usable_size)
    {
        new_buffer_size = min(PROTO_REPLY_CHUNK_BYTES,buffer_target_expand_size);
        server.stat_reply_buffer_expands++;
    }

    /* reset the peak value each server.reply_buffer_peak_reset_time seconds. in case the client will be idle
     * it will start to shrink.
     */
    /**重置每个 server.reply_buffer_peak_reset_time 秒的峰值。如果客户端空闲，它将开始缩小。*/
    if (server.reply_buffer_peak_reset_time >=0 &&
        now_ms - c->buf_peak_last_reset_time >= server.reply_buffer_peak_reset_time)
    {
        c->buf_peak = c->bufpos;
        c->buf_peak_last_reset_time = now_ms;
    }

    if (new_buffer_size) {
        oldbuf = c->buf;
        c->buf = zmalloc_usable(new_buffer_size, &c->buf_usable_size);
        memcpy(c->buf,oldbuf,c->bufpos);
        zfree(oldbuf);
    }
    return 0;
}

/* This function is used in order to track clients using the biggest amount
 * of memory in the latest few seconds. This way we can provide such information
 * in the INFO output (clients section), without having to do an O(N) scan for
 * all the clients.
 *
 * This is how it works. We have an array of CLIENTS_PEAK_MEM_USAGE_SLOTS slots
 * where we track, for each, the biggest client output and input buffers we
 * saw in that slot. Every slot corresponds to one of the latest seconds, since
 * the array is indexed by doing UNIXTIME % CLIENTS_PEAK_MEM_USAGE_SLOTS.
 *
 * When we want to know what was recently the peak memory usage, we just scan
 * such few slots searching for the maximum value. */
/**此功能用于跟踪最近几秒钟内使用最大内存量的客户端。
 * 通过这种方式，我们可以在 INFO 输出（客户端部分）中提供此类信息，而无需对所有客户端进行 O(N) 扫描。
 * 这就是它的工作原理。我们有一个 CLIENTS_PEAK_MEM_USAGE_SLOTS 插槽数组，
 * 我们跟踪每个插槽中我们看到的最大客户端输出和输入缓冲区。
 * 每个槽对应于最近的秒之一，因为数组是通过执行 UNIXTIME % CLIENTS_PEAK_MEM_USAGE_SLOTS 来索引的。
 * 当我们想知道最近的内存使用峰值是多少时，我们只需扫描这么几个槽以寻找最大值。*/
#define CLIENTS_PEAK_MEM_USAGE_SLOTS 8
size_t ClientsPeakMemInput[CLIENTS_PEAK_MEM_USAGE_SLOTS] = {0};
size_t ClientsPeakMemOutput[CLIENTS_PEAK_MEM_USAGE_SLOTS] = {0};

int clientsCronTrackExpansiveClients(client *c, int time_idx) {
    size_t in_usage = sdsZmallocSize(c->querybuf) + c->argv_len_sum +
	              (c->argv ? zmalloc_size(c->argv) : 0);
    size_t out_usage = getClientOutputBufferMemoryUsage(c);

    /* Track the biggest values observed so far in this slot. */
    /**跟踪迄今为止在此插槽中观察到的最大值。*/
    if (in_usage > ClientsPeakMemInput[time_idx]) ClientsPeakMemInput[time_idx] = in_usage;
    if (out_usage > ClientsPeakMemOutput[time_idx]) ClientsPeakMemOutput[time_idx] = out_usage;

    return 0; /* This function never terminates the client. 此函数永远不会终止客户端。*/
}

/* All normal clients are placed in one of the "mem usage buckets" according
 * to how much memory they currently use. We use this function to find the
 * appropriate bucket based on a given memory usage value. The algorithm simply
 * does a log2(mem) to ge the bucket. This means, for examples, that if a
 * client's memory usage doubles it's moved up to the next bucket, if it's
 * halved we move it down a bucket.
 * For more details see CLIENT_MEM_USAGE_BUCKETS documentation in server.h. */
/**根据他们当前使用的内存量，所有普通客户端都被放置在“内存使用桶”之一中。
 * 我们使用此函数根据给定的内存使用值找到合适的存储桶。该算法只是简单地执行 log2(mem) 来获取存储桶。
 * 这意味着，例如，如果客户端的内存使用量翻了一番，它将向上移动到下一个存储桶，如果它减半，我们将其移到下一个存储桶。
 * 有关更多详细信息，请参阅 server.h 中的 CLIENT_MEM_USAGE_BUCKETS 文档。*/
static inline clientMemUsageBucket *getMemUsageBucket(size_t mem) {
    int size_in_bits = 8*(int)sizeof(mem);
    int clz = mem > 0 ? __builtin_clzl(mem) : size_in_bits;
    int bucket_idx = size_in_bits - clz;
    if (bucket_idx > CLIENT_MEM_USAGE_BUCKET_MAX_LOG)
        bucket_idx = CLIENT_MEM_USAGE_BUCKET_MAX_LOG;
    else if (bucket_idx < CLIENT_MEM_USAGE_BUCKET_MIN_LOG)
        bucket_idx = CLIENT_MEM_USAGE_BUCKET_MIN_LOG;
    bucket_idx -= CLIENT_MEM_USAGE_BUCKET_MIN_LOG;
    return &server.client_mem_usage_buckets[bucket_idx];
}

/* This is called both on explicit clients when something changed their buffers,
 * so we can track clients' memory and enforce clients' maxmemory in real time,
 * and also from the clientsCron. We call it from the cron so we have updated
 * stats for non CLIENT_TYPE_NORMAL/PUBSUB clients and in case a configuration
 * change requires us to evict a non-active client.
 *
 * This also adds the client to the correct memory usage bucket. Each bucket contains
 * all clients with roughly the same amount of memory. This way we group
 * together clients consuming about the same amount of memory and can quickly
 * free them in case we reach maxmemory-clients (client eviction).
 */
/**当某些东西改变了他们的缓冲区时，这在显式客户端上调用，因此我们可以实时跟踪客户端的内存并强制客户端的最大内存，
 * 也可以从 clientsCron 调用。我们从 cron 调用它，因此我们更新了非 CLIENT_TYPE_NORMALPUBSUB 客户端的统计信息，
 * 以防配置更改需要我们驱逐非活动客户端。这也将客户端添加到正确的内存使用桶中。
 * 每个存储桶包含所有具有大致相同内存量的客户端。通过这种方式，我们将消耗大约相同内存量的客户端组合在一起，
 * 并且可以在达到 maxmemory-clients（客户端驱逐）的情况下快速释放它们。*/
int updateClientMemUsage(client *c) {
    serverAssert(io_threads_op == IO_THREADS_OP_IDLE);
    size_t mem = getClientMemoryUsage(c, NULL);
    int type = getClientType(c);

    /* Remove the old value of the memory used by the client from the old
     * category, and add it back. */
    /**从旧类别中删除客户端使用的内存的旧值，然后将其添加回来。*/
    if (type != c->last_memory_type) {
        server.stat_clients_type_memory[c->last_memory_type] -= c->last_memory_usage;
        server.stat_clients_type_memory[type] += mem;
        c->last_memory_type = type;
    } else {
        server.stat_clients_type_memory[type] += mem - c->last_memory_usage;
    }

    int allow_eviction =
            (type == CLIENT_TYPE_NORMAL || type == CLIENT_TYPE_PUBSUB) &&
            !(c->flags & CLIENT_NO_EVICT);

    /* Update the client in the mem usage buckets */
    /**更新内存使用桶中的客户端*/
    if (c->mem_usage_bucket) {
        c->mem_usage_bucket->mem_usage_sum -= c->last_memory_usage;
        /* If this client can't be evicted then remove it from the mem usage
         * buckets */
        /**如果无法驱逐此客户端，则将其从内存使用存储桶中删除*/
        if (!allow_eviction) {
            listDelNode(c->mem_usage_bucket->clients, c->mem_usage_bucket_node);
            c->mem_usage_bucket = NULL;
            c->mem_usage_bucket_node = NULL;
        }
    }
    if (allow_eviction) {
        clientMemUsageBucket *bucket = getMemUsageBucket(mem);
        bucket->mem_usage_sum += mem;
        if (bucket != c->mem_usage_bucket) {
            if (c->mem_usage_bucket)
                listDelNode(c->mem_usage_bucket->clients,
                            c->mem_usage_bucket_node);
            c->mem_usage_bucket = bucket;
            listAddNodeTail(bucket->clients, c);
            c->mem_usage_bucket_node = listLast(bucket->clients);
        }
    }

    /* Remember what we added, to remove it next time. */
    //记住我们添加的内容，下次删除它。
    c->last_memory_usage = mem;

    return 0;
}

/* Return the max samples in the memory usage of clients tracked by
 * the function clientsCronTrackExpansiveClients(). */
//返回由函数 clientsCronTrackExpansiveClients() 跟踪的客户端内存使用的最大样本。
void getExpansiveClientsInfo(size_t *in_usage, size_t *out_usage) {
    size_t i = 0, o = 0;
    for (int j = 0; j < CLIENTS_PEAK_MEM_USAGE_SLOTS; j++) {
        if (ClientsPeakMemInput[j] > i) i = ClientsPeakMemInput[j];
        if (ClientsPeakMemOutput[j] > o) o = ClientsPeakMemOutput[j];
    }
    *in_usage = i;
    *out_usage = o;
}

/* This function is called by serverCron() and is used in order to perform
 * operations on clients that are important to perform constantly. For instance
 * we use this function in order to disconnect clients after a timeout, including
 * clients blocked in some blocking command with a non-zero timeout.
 *
 * The function makes some effort to process all the clients every second, even
 * if this cannot be strictly guaranteed, since serverCron() may be called with
 * an actual frequency lower than server.hz in case of latency events like slow
 * commands.
 *
 * It is very important for this function, and the functions it calls, to be
 * very fast: sometimes Redis has tens of hundreds of connected clients, and the
 * default server.hz value is 10, so sometimes here we need to process thousands
 * of clients per second, turning this function into a source of latency.
 */
/**此函数由 serverCron() 调用，用于在客户端上执行对持续执行很重要的操作。
 * 例如，我们使用此函数在超时后断开客户端连接，包括在某些阻塞命令中被阻塞且超时时间不为零的客户端。
 * 该函数会努力每秒处理所有客户端，即使这不能得到严格保证，因为在延迟事件（如慢速命令）的情况下，
 * 可能会以低于 server.hz 的实际频率调用 serverCron()。
 * 这个函数和它调用的函数要非常快是非常重要的：
 *   有时 Redis 有上百个连接的客户端，默认的 server.hz 值为 10，所以有时我们需要处理数千个客户端其次，
 *   将此功能变成延迟的来源。*/
#define CLIENTS_CRON_MIN_ITERATIONS 5
void clientsCron(void) {
    /* Try to process at least numclients/server.hz of clients
     * per call. Since normally (if there are no big latency events) this
     * function is called server.hz times per second, in the average case we
     * process all the clients in 1 second. */
    /**尝试每次调用至少处理 numclientsserver.hz 的客户端。
     * 因为通常（如果没有大的延迟事件）这个函数每秒调用 server.hz 次，在平均情况下，我们在 1 秒内处理所有客户端。*/
    int numclients = listLength(server.clients);
    int iterations = numclients/server.hz;
    mstime_t now = mstime();

    /* Process at least a few clients while we are at it, even if we need
     * to process less than CLIENTS_CRON_MIN_ITERATIONS to meet our contract
     * of processing each client once per second. */
    /**在我们处理的时候至少处理几个客户端，即使我们需要处理
     * 少于 CLIENTS_CRON_MIN_ITERATIONS 来满足我们每秒处理每个客户端一次的合同。*/
    if (iterations < CLIENTS_CRON_MIN_ITERATIONS)
        iterations = (numclients < CLIENTS_CRON_MIN_ITERATIONS) ?
                     numclients : CLIENTS_CRON_MIN_ITERATIONS;


    int curr_peak_mem_usage_slot = server.unixtime % CLIENTS_PEAK_MEM_USAGE_SLOTS;
    /* Always zero the next sample, so that when we switch to that second, we'll
     * only register samples that are greater in that second without considering
     * the history of such slot.
     *
     * Note: our index may jump to any random position if serverCron() is not
     * called for some reason with the normal frequency, for instance because
     * some slow command is called taking multiple seconds to execute. In that
     * case our array may end containing data which is potentially older
     * than CLIENTS_PEAK_MEM_USAGE_SLOTS seconds: however this is not a problem
     * since here we want just to track if "recently" there were very expansive
     * clients from the POV of memory usage. */
    /**总是将下一个样本归零，这样当我们切换到那一秒时，我们只会记录在那一秒中更大的样本，而不考虑此类槽的历史。
     * 注意：如果由于某种原因没有以正常频率调用 serverCron()，我们的索引可能会跳转到任何随机位置，
     * 例如因为调用某些慢速命令需要多秒钟才能执行。在这种情况下，
     * 我们的数组可能最终包含可能早于 CLIENTS_PEAK_MEM_USAGE_SLOTS 秒的数据：
     *   但这不是问题，因为在这里我们只想跟踪“最近”是否有来自内存使用 POV 的非常广泛的客户端。*/
    int zeroidx = (curr_peak_mem_usage_slot+1) % CLIENTS_PEAK_MEM_USAGE_SLOTS;
    ClientsPeakMemInput[zeroidx] = 0;
    ClientsPeakMemOutput[zeroidx] = 0;


    while(listLength(server.clients) && iterations--) {
        client *c;
        listNode *head;

        /* Rotate the list, take the current head, process.
         * This way if the client must be removed from the list it's the
         * first element and we don't incur into O(N) computation. */
        /**旋转列表，取当前头，处理。这样，如果客户端必须从列表中删除，它是第一个元素，我们不会进行 O(N) 计算。*/
        listRotateTailToHead(server.clients);
        head = listFirst(server.clients);
        c = listNodeValue(head);
        /* The following functions do different service checks on the client.
         * The protocol is that they return non-zero if the client was
         * terminated. */
        /**以下函数在客户端上执行不同的服务检查。协议是如果客户端被终止，它们返回非零值。*/
        if (clientsCronHandleTimeout(c,now)) continue;
        if (clientsCronResizeQueryBuffer(c)) continue;
        if (clientsCronResizeOutputBuffer(c,now)) continue;

        if (clientsCronTrackExpansiveClients(c, curr_peak_mem_usage_slot)) continue;

        /* Iterating all the clients in getMemoryOverheadData() is too slow and
         * in turn would make the INFO command too slow. So we perform this
         * computation incrementally and track the (not instantaneous but updated
         * to the second) total memory used by clients using clientsCron() in
         * a more incremental way (depending on server.hz). */
        /**在 getMemoryOverheadData() 中迭代所有客户端太慢，进而会使 INFO 命令太慢。
         * 因此，我们以增量方式执行此计算，并使用 clientsCron() 以更增量的方式（取决于 server.hz）跟踪
         * 客户端使用的（不是瞬时的，而是更新到秒的）总内存。*/
        if (updateClientMemUsage(c)) continue;
        if (closeClientOnOutputBufferLimitReached(c, 0)) continue;
    }
}

/* This function handles 'background' operations we are required to do
 * incrementally in Redis databases, such as active key expiring, resizing,
 * rehashing. */
/**此函数处理我们需要在 Redis 数据库中增量执行的“后台”操作，例如活动密钥过期、调整大小、重新散列。*/
void databasesCron(void) {
    /* Expire keys by random sampling. Not required for slaves
     * as master will synthesize DELs for us. */
    /**通过随机抽样使密钥过期。 slave 不需要，因为 master 会为我们合成 DEL。*/
    if (server.active_expire_enabled) {
        if (iAmMaster()) {
            activeExpireCycle(ACTIVE_EXPIRE_CYCLE_SLOW);
        } else {
            expireSlaveKeys();
        }
    }

    /* Defrag keys gradually. 逐渐对键进行碎片整理。*/
    activeDefragCycle();

    /* Perform hash tables rehashing if needed, but only if there are no
     * other processes saving the DB on disk. Otherwise rehashing is bad
     * as will cause a lot of copy-on-write of memory pages. */
    /**如果需要，执行哈希表重新哈希，但前提是没有其他进程将数据库保存在磁盘上。
     * 否则，重新散列是不好的，因为会导致大量内存页面的写时复制。*/
    if (!hasActiveChildProcess()) {
        /* We use global counters so if we stop the computation at a given
         * DB we'll be able to start from the successive in the next
         * cron loop iteration. */
        /**我们使用全局计数器，因此如果我们在给定的数据库处停止计算，我们将能够在下一个 cron 循环迭代中从连续的开始。*/
        static unsigned int resize_db = 0;
        static unsigned int rehash_db = 0;
        int dbs_per_call = CRON_DBS_PER_CALL;
        int j;

        /* Don't test more DBs than we have. */
        if (dbs_per_call > server.dbnum) dbs_per_call = server.dbnum;

        /* Resize */
        for (j = 0; j < dbs_per_call; j++) {
            tryResizeHashTables(resize_db % server.dbnum);
            resize_db++;
        }

        /* Rehash */
        if (server.activerehashing) {
            for (j = 0; j < dbs_per_call; j++) {
                int work_done = incrementallyRehash(rehash_db);
                if (work_done) {
                    /* If the function did some work, stop here, we'll do
                     * more at the next cron loop. */
                    /**如果该函数做了一些工作，请停在这里，我们将在下一个 cron 循环中做更多的事情。*/
                    break;
                } else {
                    /* If this db didn't need rehash, we'll try the next one. */
                    /**如果这个 db 不需要 rehash，我们将尝试下一个。*/
                    rehash_db++;
                    rehash_db %= server.dbnum;
                }
            }
        }
    }
}

static inline void updateCachedTimeWithUs(int update_daylight_info, const long long ustime) {
    server.ustime = ustime;
    server.mstime = server.ustime / 1000;
    time_t unixtime = server.mstime / 1000;
    atomicSet(server.unixtime, unixtime);

    /* To get information about daylight saving time, we need to call
     * localtime_r and cache the result. However calling localtime_r in this
     * context is safe since we will never fork() while here, in the main
     * thread. The logging function will call a thread safe version of
     * localtime that has no locks. */
    /**要获取有关夏令时的信息，我们需要调用 localtime_r 并缓存结果。
     * 但是在这种情况下调用 localtime_r 是安全的，因为我们永远不会在主线程中 fork() 。
     * 日志记录函数将调用没有锁的本地时间线程安全版本。*/
    if (update_daylight_info) {
        struct tm tm;
        time_t ut = server.unixtime;
        localtime_r(&ut,&tm);
        server.daylight_active = tm.tm_isdst;
    }
}

/* We take a cached value of the unix time in the global state because with
 * virtual memory and aging there is to store the current time in objects at
 * every object access, and accuracy is not needed. To access a global var is
 * a lot faster than calling time(NULL).
 *
 * This function should be fast because it is called at every command execution
 * in call(), so it is possible to decide if to update the daylight saving
 * info or not using the 'update_daylight_info' argument. Normally we update
 * such info only when calling this function from serverCron() but not when
 * calling it from call(). */
/**我们在全局状态下获取unix时间的缓存值，因为随着虚拟内存和老化，每次对象访问时都会将当前时间存储在对象中，并且不需要准确性。
 *
 * 访问全局变量比调用 time(NULL) 快得多。这个函数应该很快，因为它在 call() 中的每个命令执行时被调用，
 * 因此可以决定是否更新夏令时信息或不使用 'update_daylight_info' 参数。
 * 通常我们仅在从 serverCron() 调用此函数时更新此类信息，而不是在从 call() 调用它时更新此类信息。*/
void updateCachedTime(int update_daylight_info) {
    const long long us = ustime();
    updateCachedTimeWithUs(update_daylight_info, us);
}

void checkChildrenDone(void) {
    int statloc = 0;
    pid_t pid;

    if ((pid = waitpid(-1, &statloc, WNOHANG)) != 0) {
        int exitcode = WIFEXITED(statloc) ? WEXITSTATUS(statloc) : -1;
        int bysignal = 0;

        if (WIFSIGNALED(statloc)) bysignal = WTERMSIG(statloc);

        /* sigKillChildHandler catches the signal and calls exit(), but we
         * must make sure not to flag lastbgsave_status, etc incorrectly.
         * We could directly terminate the child process via SIGUSR1
         * without handling it */
        /**sigKillChildHandler 捕获信号并调用 exit()，但我们必须确保不要错误地标记 lastbgsave_status 等。
         * 我们可以通过 SIGUSR1 直接终止子进程而不需要处理它*/
        if (exitcode == SERVER_CHILD_NOERROR_RETVAL) {
            bysignal = SIGUSR1;
            exitcode = 1;
        }

        if (pid == -1) {
            serverLog(LL_WARNING,"waitpid() returned an error: %s. "
                "child_type: %s, child_pid = %d",
                strerror(errno),
                strChildType(server.child_type),
                (int) server.child_pid);
        } else if (pid == server.child_pid) {
            if (server.child_type == CHILD_TYPE_RDB) {
                backgroundSaveDoneHandler(exitcode, bysignal);
            } else if (server.child_type == CHILD_TYPE_AOF) {
                backgroundRewriteDoneHandler(exitcode, bysignal);
            } else if (server.child_type == CHILD_TYPE_MODULE) {
                ModuleForkDoneHandler(exitcode, bysignal);
            } else {
                serverPanic("Unknown child type %d for child pid %d", server.child_type, server.child_pid);
                exit(1);
            }
            if (!bysignal && exitcode == 0) receiveChildInfo();
            resetChildState();
        } else {
            if (!ldbRemoveChild(pid)) {
                serverLog(LL_WARNING,
                          "Warning, detected child with unmatched pid: %ld",
                          (long) pid);
            }
        }

        /* start any pending forks immediately. 立即启动任何待处理的分叉。*/
        replicationStartPendingFork();
    }
}

/* Called from serverCron and cronUpdateMemoryStats to update cached memory metrics. */
/**从 serverCron 和 cronUpdateMemoryStats 调用以更新缓存的内存指标。*/
void cronUpdateMemoryStats() {
    /* Record the max memory used since the server was started. */
    /**记录服务器启动后使用的最大内存。*/
    if (zmalloc_used_memory() > server.stat_peak_memory)
        server.stat_peak_memory = zmalloc_used_memory();

    run_with_period(100) {
        /* Sample the RSS and other metrics here since this is a relatively slow call.
         * We must sample the zmalloc_used at the same time we take the rss, otherwise
         * the frag ratio calculate may be off (ratio of two samples at different times) */
        /**在此处对 RSS 和其他指标进行采样，因为这是一个相对较慢的调用。
         * 我们必须在取 rss 的同时对 zmalloc_used 进行采样，否则 frag ratio 计算可能会关闭（两个样本在不同时间的比率）*/
        server.cron_malloc_stats.process_rss = zmalloc_get_rss();
        server.cron_malloc_stats.zmalloc_used = zmalloc_used_memory();
        /* Sampling the allocator info can be slow too.
         * The fragmentation ratio it'll show is potentially more accurate
         * it excludes other RSS pages such as: shared libraries, LUA and other non-zmalloc
         * allocations, and allocator reserved pages that can be pursed (all not actual frag) */
        /**对分配器信息进行采样也可能很慢。它将显示的碎片率可能更准确，它排除了其他 RSS 页面，例如：
         *   共享库、LUA 和其他非 zmalloc 分配，以及可以占用的分配器保留页面（所有这些都不是实际碎片）*/
        zmalloc_get_allocator_info(&server.cron_malloc_stats.allocator_allocated,
                                   &server.cron_malloc_stats.allocator_active,
                                   &server.cron_malloc_stats.allocator_resident);
        /* in case the allocator isn't providing these stats, fake them so that
         * fragmentation info still shows some (inaccurate metrics) */
        /**如果分配器没有提供这些统计信息，请伪造它们，以便碎片信息仍然显示一些（不准确的指标）*/
        if (!server.cron_malloc_stats.allocator_resident) {
            /* LUA memory isn't part of zmalloc_used, but it is part of the process RSS,
             * so we must deduct it in order to be able to calculate correct
             * "allocator fragmentation" ratio */
            /**LUA 内存不是 zmalloc_used 的一部分，但它是进程 RSS 的一部分，
             * 所以我们必须扣除它才能计算正确的“分配器碎片”比率*/
            size_t lua_memory = evalMemory();
            server.cron_malloc_stats.allocator_resident = server.cron_malloc_stats.process_rss - lua_memory;
        }
        if (!server.cron_malloc_stats.allocator_active)
            server.cron_malloc_stats.allocator_active = server.cron_malloc_stats.allocator_resident;
        if (!server.cron_malloc_stats.allocator_allocated)
            server.cron_malloc_stats.allocator_allocated = server.cron_malloc_stats.zmalloc_used;
    }
}

/* This is our timer interrupt, called server.hz times per second.
 * Here is where we do a number of things that need to be done asynchronously.
 * For instance:
 *
 * - Active expired keys collection (it is also performed in a lazy way on
 *   lookup).
 * - Software watchdog.
 * - Update some statistic.
 * - Incremental rehashing of the DBs hash tables.
 * - Triggering BGSAVE / AOF rewrite, and handling of terminated children.
 * - Clients timeout of different kinds.
 * - Replication reconnection.
 * - Many more...
 *
 * Everything directly called here will be called server.hz times per second,
 * so in order to throttle execution of things we want to do less frequently
 * a macro is used: run_with_period(milliseconds) { .... }
 */
/**这是我们的定时器中断，每秒调用 server.hz 次。这是我们做一些需要异步完成的事情的地方。
 * 例如：
 *   - 活动的过期密钥收集（它也在查找时以惰性方式执行）。
 *   - 软件看门狗。 - 更新一些统计数据。
 *   - DBs 哈希表的增量重新散列。
 *   - 触发 BGSAVE AOF 重写，并处理终止的孩子。
 *   - 不同类型的客户端超时。
 *   - 复制重新连接。
 *   - 更多...
 * 这里直接调用的所有内容都将每秒调用 server.hz 次，因此为了限制我们想要减少执行频率的事情的执行，
 * 使用了宏：run_with_period(milliseconds) { .... }*/

int serverCron(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    int j;
    UNUSED(eventLoop);
    UNUSED(id);
    UNUSED(clientData);

    /* Software watchdog: deliver the SIGALRM that will reach the signal
     * handler if we don't return here fast enough. */
    /**软件看门狗：如果我们没有足够快地返回这里，则传递将到达信号处理程序的 SIGALRM。*/
    if (server.watchdog_period) watchdogScheduleSignal(server.watchdog_period);

    /* Update the time cache. */
    updateCachedTime(1);

    server.hz = server.config_hz;
    /* Adapt the server.hz value to the number of configured clients. If we have
     * many clients, we want to call serverCron() with an higher frequency. */
    /**使 server.hz 值适应已配置客户端的数量。如果我们有很多客户端，我们希望以更高的频率调用 serverCron()。*/
    if (server.dynamic_hz) {
        while (listLength(server.clients) / server.hz >
               MAX_CLIENTS_PER_CLOCK_TICK)
        {
            server.hz *= 2;
            if (server.hz > CONFIG_MAX_HZ) {
                server.hz = CONFIG_MAX_HZ;
                break;
            }
        }
    }

    /* for debug purposes: skip actual cron work if pause_cron is on */
    /**出于调试目的：如果 pause_cron 开启，则跳过实际的 cron 工作*/
    if (server.pause_cron) return 1000/server.hz;

    run_with_period(100) {
        long long stat_net_input_bytes, stat_net_output_bytes;
        long long stat_net_repl_input_bytes, stat_net_repl_output_bytes;
        atomicGet(server.stat_net_input_bytes, stat_net_input_bytes);
        atomicGet(server.stat_net_output_bytes, stat_net_output_bytes);
        atomicGet(server.stat_net_repl_input_bytes, stat_net_repl_input_bytes);
        atomicGet(server.stat_net_repl_output_bytes, stat_net_repl_output_bytes);

        trackInstantaneousMetric(STATS_METRIC_COMMAND,server.stat_numcommands);
        trackInstantaneousMetric(STATS_METRIC_NET_INPUT,
                stat_net_input_bytes + stat_net_repl_input_bytes);
        trackInstantaneousMetric(STATS_METRIC_NET_OUTPUT,
                stat_net_output_bytes + stat_net_repl_output_bytes);
        trackInstantaneousMetric(STATS_METRIC_NET_INPUT_REPLICATION,
                                 stat_net_repl_input_bytes);
        trackInstantaneousMetric(STATS_METRIC_NET_OUTPUT_REPLICATION,
                                 stat_net_repl_output_bytes);
    }

    /* We have just LRU_BITS bits per object for LRU information.
     * So we use an (eventually wrapping) LRU clock.
     *
     * Note that even if the counter wraps it's not a big problem,
     * everything will still work but some object will appear younger
     * to Redis. However for this to happen a given object should never be
     * touched for all the time needed to the counter to wrap, which is
     * not likely.
     *
     * Note that you can change the resolution altering the
     * LRU_CLOCK_RESOLUTION define. */
    /**对于 LRU 信息，每个对象只有 LRU_BITS 位。所以我们使用（最终包装的）LRU 时钟。
     * 请注意，即使计数器换行也不是什么大问题，一切仍然有效，但某些对象对 Redis 来说会显得更年轻。
     * 然而，要发生这种情况，在计数器包裹所需的所有时间内都不应该触摸给定的对象，这是不太可能的。
     * 请注意，您可以更改 LRU_CLOCK_RESOLUTION 定义的分辨率。*/
    unsigned int lruclock = getLRUClock();
    atomicSet(server.lruclock,lruclock);

    cronUpdateMemoryStats();

    /* We received a SIGTERM or SIGINT, shutting down here in a safe way, as it is
     * not ok doing so inside the signal handler. */
    /**我们收到了 SIGTERM 或 SIGINT，以安全的方式在此处关闭，因为在信号处理程序中这样做是不合适的。*/
    if (server.shutdown_asap && !isShutdownInitiated()) {
        int shutdownFlags = SHUTDOWN_NOFLAGS;
        if (server.last_sig_received == SIGINT && server.shutdown_on_sigint)
            shutdownFlags = server.shutdown_on_sigint;
        else if (server.last_sig_received == SIGTERM && server.shutdown_on_sigterm)
            shutdownFlags = server.shutdown_on_sigterm;

        if (prepareForShutdown(shutdownFlags) == C_OK) exit(0);
    } else if (isShutdownInitiated()) {
        if (server.mstime >= server.shutdown_mstime || isReadyToShutdown()) {
            if (finishShutdown() == C_OK) exit(0);
            /* Shutdown failed. Continue running. An error has been logged. */
            /**关机失败。继续运行。已记录错误。*/
        }
    }

    /* Show some info about non-empty databases 显示一些关于非空数据库的信息*/
    if (server.verbosity <= LL_VERBOSE) {
        run_with_period(5000) {
            for (j = 0; j < server.dbnum; j++) {
                long long size, used, vkeys;

                size = dictSlots(server.db[j].dict);
                used = dictSize(server.db[j].dict);
                vkeys = dictSize(server.db[j].expires);
                if (used || vkeys) {
                    serverLog(LL_VERBOSE,"DB %d: %lld keys (%lld volatile) in %lld slots HT.",j,used,vkeys,size);
                }
            }
        }
    }

    /* Show information about connected clients */
    //显示有关已连接客户端的信息
    if (!server.sentinel_mode) {
        run_with_period(5000) {
            serverLog(LL_DEBUG,
                "%lu clients connected (%lu replicas), %zu bytes in use",
                listLength(server.clients)-listLength(server.slaves),
                listLength(server.slaves),
                zmalloc_used_memory());
        }
    }

    /* We need to do a few operations on clients asynchronously. */
    /**我们需要对客户端异步执行一些操作。*/
    clientsCron();

    /* Handle background operations on Redis databases. */
    /**处理 Redis 数据库的后台操作。*/
    databasesCron();

    /* Start a scheduled AOF rewrite if this was requested by the user while
     * a BGSAVE was in progress. */
    /**如果在 BGSAVE 正在进行时用户请求，则启动计划的 AOF 重写。*/
    if (!hasActiveChildProcess() &&
        server.aof_rewrite_scheduled &&
        !aofRewriteLimited())
    {
        rewriteAppendOnlyFileBackground();
    }

    /* Check if a background saving or AOF rewrite in progress terminated. */
    /**检查正在进行的后台保存或 AOF 重写是否终止。*/
    if (hasActiveChildProcess() || ldbPendingChildren())
    {
        run_with_period(1000) receiveChildInfo();
        checkChildrenDone();
    } else {
        /* If there is not a background saving/rewrite in progress check if
         * we have to save/rewrite now. */
        /**如果没有正在进行的后台保存重写，请检查我们现在是否必须保存重写。*/
        for (j = 0; j < server.saveparamslen; j++) {
            struct saveparam *sp = server.saveparams+j;

            /* Save if we reached the given amount of changes,
             * the given amount of seconds, and if the latest bgsave was
             * successful or if, in case of an error, at least
             * CONFIG_BGSAVE_RETRY_DELAY seconds already elapsed. */
            /**如果我们达到了给定的更改量、给定的秒数，并且如果最新的 bgsave 成功，或者如果发生错误，
             * 至少 CONFIG_BGSAVE_RETRY_DELAY 秒已经过去，则保存。*/
            if (server.dirty >= sp->changes &&
                server.unixtime-server.lastsave > sp->seconds &&
                (server.unixtime-server.lastbgsave_try >
                 CONFIG_BGSAVE_RETRY_DELAY ||
                 server.lastbgsave_status == C_OK))
            {
                serverLog(LL_NOTICE,"%d changes in %d seconds. Saving...",
                    sp->changes, (int)sp->seconds);
                rdbSaveInfo rsi, *rsiptr;
                rsiptr = rdbPopulateSaveInfo(&rsi);
                rdbSaveBackground(SLAVE_REQ_NONE,server.rdb_filename,rsiptr);
                break;
            }
        }

        /* Trigger an AOF rewrite if needed. 如果需要，触发 AOF 重写。*/
        if (server.aof_state == AOF_ON &&
            !hasActiveChildProcess() &&
            server.aof_rewrite_perc &&
            server.aof_current_size > server.aof_rewrite_min_size)
        {
            long long base = server.aof_rewrite_base_size ?
                server.aof_rewrite_base_size : 1;
            long long growth = (server.aof_current_size*100/base) - 100;
            if (growth >= server.aof_rewrite_perc && !aofRewriteLimited()) {
                serverLog(LL_NOTICE,"Starting automatic rewriting of AOF on %lld%% growth",growth);
                rewriteAppendOnlyFileBackground();
            }
        }
    }
    /* Just for the sake of defensive programming, to avoid forgetting to
     * call this function when needed. */
    /**只是为了防御性编程，避免在需要的时候忘记调用这个函数。*/
    updateDictResizePolicy();


    /* AOF postponed flush: Try at every cron cycle if the slow fsync
     * completed. */
    /**AOF 延迟刷新：如果慢速 fsync 完成，则在每个 cron 周期尝试。*/
    if ((server.aof_state == AOF_ON || server.aof_state == AOF_WAIT_REWRITE) &&
        server.aof_flush_postponed_start)
    {
        flushAppendOnlyFile(0);
    }

    /* AOF write errors: in this case we have a buffer to flush as well and
     * clear the AOF error in case of success to make the DB writable again,
     * however to try every second is enough in case of 'hz' is set to
     * a higher frequency. */
    /**AOF 写入错误：在这种情况下，我们还有一个缓冲区要刷新，并在成功的情况下清除 AOF 错误以使 DB 再次可写，
     * 但是在“hz”设置为更高频率的情况下每秒尝试一次就足够了.*/
    run_with_period(1000) {
        if ((server.aof_state == AOF_ON || server.aof_state == AOF_WAIT_REWRITE) &&
            server.aof_last_write_status == C_ERR) 
            {
                flushAppendOnlyFile(0);
            }
    }

    /* Clear the paused clients state if needed. */
    //如果需要，清除暂停的客户端状态。
    checkClientPauseTimeoutAndReturnIfPaused();

    /* Replication cron function -- used to reconnect to master,
     * detect transfer failures, start background RDB transfers and so forth. 
     * 
     * If Redis is trying to failover then run the replication cron faster so
     * progress on the handshake happens more quickly. */
    /**复制 cron 功能——用于重新连接到 master、检测传输失败、启动后台 RDB 传输等。
     * 如果 Redis 尝试进行故障转移，则更快地运行复制 cron，以便更快地进行握手。*/
    if (server.failover_state != NO_FAILOVER) {
        run_with_period(100) replicationCron();
    } else {
        run_with_period(1000) replicationCron();
    }

    /* Run the Redis Cluster cron. 运行 Redis 集群 cron。*/
    run_with_period(100) {
        if (server.cluster_enabled) clusterCron();
    }

    /* Run the Sentinel timer if we are in sentinel mode. */
    //如果我们处于哨兵模式，请运行哨兵计时器。
    if (server.sentinel_mode) sentinelTimer();

    /* Cleanup expired MIGRATE cached sockets. */
    //清理过期的 MIGRATE 缓存套接字。
    run_with_period(1000) {
        migrateCloseTimedoutSockets();
    }

    /* Stop the I/O threads if we don't have enough pending work. */
    //如果我们没有足够的待处理工作，请停止 IO 线程。
    stopThreadedIOIfNeeded();

    /* Resize tracking keys table if needed. This is also done at every
     * command execution, but we want to be sure that if the last command
     * executed changes the value via CONFIG SET, the server will perform
     * the operation even if completely idle. */
    /**如果需要，调整跟踪键表的大小。这也是在每次命令执行时完成的，
     * 但是我们要确保如果执行的最后一个命令通过 CONFIG SET 更改了值，即使完全空闲，服务器也会执行该操作。*/
    if (server.tracking_clients) trackingLimitUsedSlots();

    /* Start a scheduled BGSAVE if the corresponding flag is set. This is
     * useful when we are forced to postpone a BGSAVE because an AOF
     * rewrite is in progress.
     *
     * Note: this code must be after the replicationCron() call above so
     * make sure when refactoring this file to keep this order. This is useful
     * because we want to give priority to RDB savings for replication. */
    /**如果设置了相应的标志，则启动计划的 BGSAVE。当我们因为正在进行 AOF 重写而被迫推迟 BGSAVE 时，这很有用。
     * 注意：此代码必须在上面的 replicationCron() 调用之后，因此请确保在重构此文件时保持此顺序。
     * 这很有用，因为我们希望优先考虑 RDB 节省以进行复制。*/
    if (!hasActiveChildProcess() &&
        server.rdb_bgsave_scheduled &&
        (server.unixtime-server.lastbgsave_try > CONFIG_BGSAVE_RETRY_DELAY ||
         server.lastbgsave_status == C_OK))
    {
        rdbSaveInfo rsi, *rsiptr;
        rsiptr = rdbPopulateSaveInfo(&rsi);
        if (rdbSaveBackground(SLAVE_REQ_NONE,server.rdb_filename,rsiptr) == C_OK)
            server.rdb_bgsave_scheduled = 0;
    }

    run_with_period(100) {
        if (moduleCount()) modulesCron();
    }

    /* Fire the cron loop modules event. 触发 cron 循环模块事件。*/
    RedisModuleCronLoopV1 ei = {REDISMODULE_CRON_LOOP_VERSION,server.hz};
    moduleFireServerEvent(REDISMODULE_EVENT_CRON_LOOP,
                          0,
                          &ei);

    server.cronloops++;
    return 1000/server.hz;
}


void blockingOperationStarts() {
    if(!server.blocking_op_nesting++){
        updateCachedTime(0);
        server.blocked_last_cron = server.mstime;
    }
}

void blockingOperationEnds() {
    if(!(--server.blocking_op_nesting)){
        server.blocked_last_cron = 0;
    }
}

/* This function fills in the role of serverCron during RDB or AOF loading, and
 * also during blocked scripts.
 * It attempts to do its duties at a similar rate as the configured server.hz,
 * and updates cronloops variable so that similarly to serverCron, the
 * run_with_period can be used. */
/**此函数在 RDB 或 AOF 加载期间以及在阻塞脚本期间填充 serverCron 的角色。
 * 它尝试以与配置的 server.hz 相似的速率完成其职责，并更新 cronloops 变量，以便与 serverCron 类似，
 * 可以使用 run_with_period。*/
void whileBlockedCron() {
    /* Here we may want to perform some cron jobs (normally done server.hz times
     * per second). */

    /* Since this function depends on a call to blockingOperationStarts, let's
     * make sure it was done. */
    /**在这里，我们可能想要执行一些 cron 作业（通常每秒执行 server.hz 次）。
     * 由于此函数依赖于对 blockingOperationStarts 的调用，因此让我们确保它已完成。*/
    serverAssert(server.blocked_last_cron);

    /* In case we where called too soon, leave right away. This way one time
     * jobs after the loop below don't need an if. and we don't bother to start
     * latency monitor if this function is called too often. */
    /**万一我们叫得太早了，马上离开。这样，在下面的循环之后的一次工作不需要 if。
     * 如果这个函数被调用得太频繁，我们也不会费心去启动延迟监视器。*/
    if (server.blocked_last_cron >= server.mstime)
        return;

    mstime_t latency;
    latencyStartMonitor(latency);

    /* In some cases we may be called with big intervals, so we may need to do
     * extra work here. This is because some of the functions in serverCron rely
     * on the fact that it is performed every 10 ms or so. For instance, if
     * activeDefragCycle needs to utilize 25% cpu, it will utilize 2.5ms, so we
     * need to call it multiple times. */
    /**在某些情况下，我们可能会以很大的间隔被调用，因此我们可能需要在这里做额外的工作。
     * 这是因为 serverCron 中的某些功能依赖于它每 10 毫秒左右执行一次的事实。
     * 例如，如果 activeDefragCycle 需要使用 25% cpu，它将使用 2.5ms，因此我们需要多次调用它。*/
    long hz_ms = 1000/server.hz;
    while (server.blocked_last_cron < server.mstime) {

        /* Defrag keys gradually. 逐渐对键进行碎片整理。*/
        activeDefragCycle();

        server.blocked_last_cron += hz_ms;

        /* Increment cronloop so that run_with_period works. */
        //增加 cronloop 以便 run_with_period 工作。
        server.cronloops++;
    }

    /* Other cron jobs do not need to be done in a loop. No need to check
     * server.blocked_last_cron since we have an early exit at the top. */
    /**其他 cron 作业不需要循环完成。无需检查 server.blocked_last_cron 因为我们在顶部有一个提前退出。*/

    /* Update memory stats during loading (excluding blocked scripts) */
    //在加载期间更新内存统计信息（不包括被阻止的脚本）
    if (server.loading) cronUpdateMemoryStats();

    latencyEndMonitor(latency);
    latencyAddSampleIfNeeded("while-blocked-cron",latency);

    /* We received a SIGTERM during loading, shutting down here in a safe way,
     * as it isn't ok doing so inside the signal handler. */
    /**我们在加载过程中收到了 SIGTERM，以安全的方式在此处关闭，因为在信号处理程序中这样做是不合适的。*/
    if (server.shutdown_asap && server.loading) {
        if (prepareForShutdown(SHUTDOWN_NOSAVE) == C_OK) exit(0);
        serverLog(LL_WARNING,"SIGTERM received but errors trying to shut down the server, check the logs for more information");
        server.shutdown_asap = 0;
        server.last_sig_received = 0;
    }
}

static void sendGetackToReplicas(void) {
    robj *argv[3];
    argv[0] = shared.replconf;
    argv[1] = shared.getack;
    argv[2] = shared.special_asterick; /* Not used argument. */
    replicationFeedSlaves(server.slaves, server.slaveseldb, argv, 3);
}

extern int ProcessingEventsWhileBlocked;

/* This function gets called every time Redis is entering the
 * main loop of the event driven library, that is, before to sleep
 * for ready file descriptors.
 *
 * Note: This function is (currently) called from two functions:
 * 1. aeMain - The main server loop
 * 2. processEventsWhileBlocked - Process clients during RDB/AOF load
 *
 * If it was called from processEventsWhileBlocked we don't want
 * to perform all actions (For example, we don't want to expire
 * keys), but we do need to perform some actions.
 *
 * The most important is freeClientsInAsyncFreeQueue but we also
 * call some other low-risk functions. */
/**每次 Redis 进入事件驱动库的主循环时都会调用此函数，也就是说，在为准备好的文件描述符休眠之前。
 * 注意：这个函数（当前）是从两个函数调用的：
 *   1. aeMain - 主服务器循环
 *   2. processEventsWhileBlocked - 在 RDBAOF 加载期间处理客户端
 * 如果它是从 processEventsWhileBlocked 调用的，我们不想执行所有操作（例如，我们不想让密钥过期），
 * 但我们确实需要执行一些操作。最重要的是 freeClientsInAsyncFreeQueue 但我们也调用了其他一些低风险的函数。*/
void beforeSleep(struct aeEventLoop *eventLoop) {
    UNUSED(eventLoop);

    size_t zmalloc_used = zmalloc_used_memory();
    if (zmalloc_used > server.stat_peak_memory)
        server.stat_peak_memory = zmalloc_used;

    /* Just call a subset of vital functions in case we are re-entering
     * the event loop from processEventsWhileBlocked(). Note that in this
     * case we keep track of the number of events we are processing, since
     * processEventsWhileBlocked() wants to stop ASAP if there are no longer
     * events to handle. */
    /**如果我们从 processEventsWhileBlocked() 重新进入事件循环，只需调用重要函数的子集。
     * 请注意，在这种情况下，我们会跟踪正在处理的事件数量，因为 processEventsWhileBlocked() 如果不再需要处理事件，
     * 则希望尽快停止。*/
    if (ProcessingEventsWhileBlocked) {
        uint64_t processed = 0;
        processed += handleClientsWithPendingReadsUsingThreads();
        processed += tlsProcessPendingData();
        if (server.aof_state == AOF_ON || server.aof_state == AOF_WAIT_REWRITE)
            flushAppendOnlyFile(0);
        processed += handleClientsWithPendingWrites();
        processed += freeClientsInAsyncFreeQueue();
        server.events_processed_while_blocked += processed;
        return;
    }

    /* Handle precise timeouts of blocked clients. */
    //处理被阻止客户端的精确超时。
    handleBlockedClientsTimeout();

    /* We should handle pending reads clients ASAP after event loop. */
    //我们应该在事件循环之后尽快处理挂起的读取客户端。
    handleClientsWithPendingReadsUsingThreads();

    /* Handle TLS pending data. (must be done before flushAppendOnlyFile) */
    //处理 TLS 待处理数据。 （必须在 flushAppendOnlyFile 之前完成）
    tlsProcessPendingData();

    /* If tls still has pending unread data don't sleep at all. */
    //如果 tls 仍有待处理的未读数据，则根本不要休眠。
    aeSetDontWait(server.el, tlsHasPendingData());

    /* Call the Redis Cluster before sleep function. Note that this function
     * may change the state of Redis Cluster (from ok to fail or vice versa),
     * so it's a good idea to call it before serving the unblocked clients
     * later in this function. */
    /**在 sleep 函数之前调用 Redis Cluster。
     * 请注意，此函数可能会更改 Redis 集群的状态（从 ok 变为失败，反之亦然），
     * 因此最好在稍后在此函数中为未阻塞的客户端提供服务之前调用它。*/
    if (server.cluster_enabled) clusterBeforeSleep();

    /* Run a fast expire cycle (the called function will return
     * ASAP if a fast cycle is not needed). */
    /**运行快速过期循环（如果不需要快速循环，被调用函数将尽快返回）。*/
    if (server.active_expire_enabled && server.masterhost == NULL)
        activeExpireCycle(ACTIVE_EXPIRE_CYCLE_FAST);

    /* Unblock all the clients blocked for synchronous replication
     * in WAIT. */
    /**在 WAIT 中取消阻止所有为同步复制而阻止的客户端。*/
    if (listLength(server.clients_waiting_acks))
        processClientsWaitingReplicas();

    /* Check if there are clients unblocked by modules that implement
     * blocking commands. */
    /**检查是否有客户端被实现阻塞命令的模块解除阻塞。*/
    if (moduleCount()) {
        moduleFireServerEvent(REDISMODULE_EVENT_EVENTLOOP,
                              REDISMODULE_SUBEVENT_EVENTLOOP_BEFORE_SLEEP,
                              NULL);
        moduleHandleBlockedClients();
    }

    /* Try to process pending commands for clients that were just unblocked. */
    /**尝试为刚刚解锁的客户端处理挂起的命令。*/
    if (listLength(server.unblocked_clients))
        processUnblockedClients();

    /* Send all the slaves an ACK request if at least one client blocked
     * during the previous event loop iteration. Note that we do this after
     * processUnblockedClients(), so if there are multiple pipelined WAITs
     * and the just unblocked WAIT gets blocked again, we don't have to wait
     * a server cron cycle in absence of other event loop events. See #6623.
     * 
     * We also don't send the ACKs while clients are paused, since it can
     * increment the replication backlog, they'll be sent after the pause
     * if we are still the master. */
    /**如果在上一次事件循环迭代期间至少有一个客户端被阻塞，则向所有从属发送 ACK 请求。
     * 请注意，我们在 processUnblockedClients() 之后执行此操作，
     * 因此如果有多个流水线 WAIT 并且刚刚未阻塞的 WAIT 再次被阻塞，
     * 我们不必在没有其他事件循环事件的情况下等待服务器 cron 周期。参见 6623。
     * 我们也不会在客户端暂停时发送 ACK，因为它可以增加复制积压，如果我们仍然是主服务器，它们将在暂停后发送。*/
    if (server.get_ack_from_slaves && !checkClientPauseTimeoutAndReturnIfPaused()) {
        sendGetackToReplicas();
        server.get_ack_from_slaves = 0;
    }

    /* We may have received updates from clients about their current offset. NOTE:
     * this can't be done where the ACK is received since failover will disconnect 
     * our clients. */
    /**我们可能已经收到客户关于他们当前偏移量的更新。注意：这不能在收到 ACK 的地方完成，因为故障转移会断开我们的客户端。*/
    updateFailoverStatus();

    /* Since we rely on current_client to send scheduled invalidation messages
     * we have to flush them after each command, so when we get here, the list
     * must be empty. */
    /**由于我们依赖 current_client 发送预定的失效消息，我们必须在每个命令之后刷新它们，所以当我们到达这里时，列表必须是空的。*/
    serverAssert(listLength(server.tracking_pending_keys) == 0);

    /* Send the invalidation messages to clients participating to the
     * client side caching protocol in broadcasting (BCAST) mode. */
    /**以广播 (BCAST) 模式将失效消息发送给参与客户端缓存协议的客户端。*/
    trackingBroadcastInvalidationMessages();

    /* Try to process blocked clients every once in while.
     *
     * Example: A module calls RM_SignalKeyAsReady from within a timer callback
     * (So we don't visit processCommand() at all).
     *
     * must be done before flushAppendOnlyFile, in case of appendfsync=always,
     * since the unblocked clients may write data. */
    /**尝试每隔一段时间处理一次被阻止的客户端。示例：模块从计时器回调中
     * 调用 RM_SignalKeyAsReady（因此我们根本不访问 processCommand()）。
     * 必须在 flushAppendOnlyFile 之前完成，在 appendfsync=always 的情况下，因为未阻塞的客户端可能会写入数据。*/
    handleClientsBlockedOnKeys();

    /* Write the AOF buffer on disk,
     * must be done before handleClientsWithPendingWritesUsingThreads,
     * in case of appendfsync=always. */
    /**将 AOF 缓冲区写入磁盘，必须在 handleClientsWithPendingWritesUsingThreads 之前完成，以防 appendfsync=always。*/
    if (server.aof_state == AOF_ON || server.aof_state == AOF_WAIT_REWRITE)
        flushAppendOnlyFile(0);

    /* Handle writes with pending output buffers. 使用挂起的输出缓冲区处理写入。*/
    handleClientsWithPendingWritesUsingThreads();

    /* Close clients that need to be closed asynchronous 关闭需要异步关闭的客户端*/
    freeClientsInAsyncFreeQueue();

    /* Incrementally trim replication backlog, 10 times the normal speed is
     * to free replication backlog as much as possible. */
    /**增量修剪复制积压，正常速度的10倍是尽可能释放复制积压。*/
    if (server.repl_backlog)
        incrementalTrimReplicationBacklog(10*REPL_BACKLOG_TRIM_BLOCKS_PER_CALL);

    /* Disconnect some clients if they are consuming too much memory. */
    /**如果某些客户端消耗太多内存，请断开它们的连接。*/
    evictClients();

    /* Before we are going to sleep, let the threads access the dataset by
     * releasing the GIL. Redis main thread will not touch anything at this
     * time. */
    /**在我们睡觉之前，让线程通过释放 GIL 来访问数据集。 Redis 主线程此时不会接触任何东西。*/
    if (moduleCount()) moduleReleaseGIL();

    /* Do NOT add anything below moduleReleaseGIL !!! 不要在 moduleReleaseGIL 下面添加任何东西！！！*/
}

/* This function is called immediately after the event loop multiplexing
 * API returned, and the control is going to soon return to Redis by invoking
 * the different events callbacks. */
/**该函数在事件循环多路复用API返回后立即调用，控件将通过调用不同的事件回调很快返回到Redis。*/
void afterSleep(struct aeEventLoop *eventLoop) {
    UNUSED(eventLoop);

    /* Do NOT add anything above moduleAcquireGIL !!! 不要在 moduleAcquireGIL 上面添加任何东西！！！*/

    /* Acquire the modules GIL so that their threads won't touch anything. */
    /**获取模块 GIL 以便它们的线程不会接触任何东西。*/
    if (!ProcessingEventsWhileBlocked) {
        if (moduleCount()) {
            mstime_t latency;
            latencyStartMonitor(latency);

            moduleAcquireGIL();
            moduleFireServerEvent(REDISMODULE_EVENT_EVENTLOOP,
                                  REDISMODULE_SUBEVENT_EVENTLOOP_AFTER_SLEEP,
                                  NULL);
            latencyEndMonitor(latency);
            latencyAddSampleIfNeeded("module-acquire-GIL",latency);
        }
    }
}

/* =========================== Server initialization ======================== */

void createSharedObjects(void) {
    int j;

    /* Shared command responses */
    shared.crlf = createObject(OBJ_STRING,sdsnew("\r\n"));
    shared.ok = createObject(OBJ_STRING,sdsnew("+OK\r\n"));
    shared.emptybulk = createObject(OBJ_STRING,sdsnew("$0\r\n\r\n"));
    shared.czero = createObject(OBJ_STRING,sdsnew(":0\r\n"));
    shared.cone = createObject(OBJ_STRING,sdsnew(":1\r\n"));
    shared.emptyarray = createObject(OBJ_STRING,sdsnew("*0\r\n"));
    shared.pong = createObject(OBJ_STRING,sdsnew("+PONG\r\n"));
    shared.queued = createObject(OBJ_STRING,sdsnew("+QUEUED\r\n"));
    shared.emptyscan = createObject(OBJ_STRING,sdsnew("*2\r\n$1\r\n0\r\n*0\r\n"));
    shared.space = createObject(OBJ_STRING,sdsnew(" "));
    shared.plus = createObject(OBJ_STRING,sdsnew("+"));

    /* Shared command error responses 共享命令错误响应*/
    shared.wrongtypeerr = createObject(OBJ_STRING,sdsnew(
        "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"));
    shared.err = createObject(OBJ_STRING,sdsnew("-ERR\r\n"));
    shared.nokeyerr = createObject(OBJ_STRING,sdsnew(
        "-ERR no such key\r\n"));
    shared.syntaxerr = createObject(OBJ_STRING,sdsnew(
        "-ERR syntax error\r\n"));
    shared.sameobjecterr = createObject(OBJ_STRING,sdsnew(
        "-ERR source and destination objects are the same\r\n"));
    shared.outofrangeerr = createObject(OBJ_STRING,sdsnew(
        "-ERR index out of range\r\n"));
    shared.noscripterr = createObject(OBJ_STRING,sdsnew(
        "-NOSCRIPT No matching script. Please use EVAL.\r\n"));
    shared.loadingerr = createObject(OBJ_STRING,sdsnew(
        "-LOADING Redis is loading the dataset in memory\r\n"));
    shared.slowevalerr = createObject(OBJ_STRING,sdsnew(
        "-BUSY Redis is busy running a script. You can only call SCRIPT KILL or SHUTDOWN NOSAVE.\r\n"));
    shared.slowscripterr = createObject(OBJ_STRING,sdsnew(
        "-BUSY Redis is busy running a script. You can only call FUNCTION KILL or SHUTDOWN NOSAVE.\r\n"));
    shared.slowmoduleerr = createObject(OBJ_STRING,sdsnew(
        "-BUSY Redis is busy running a module command.\r\n"));
    shared.masterdownerr = createObject(OBJ_STRING,sdsnew(
        "-MASTERDOWN Link with MASTER is down and replica-serve-stale-data is set to 'no'.\r\n"));
    shared.bgsaveerr = createObject(OBJ_STRING,sdsnew(
        "-MISCONF Redis is configured to save RDB snapshots, but it's currently unable to persist to disk. Commands that may modify the data set are disabled, because this instance is configured to report errors during writes if RDB snapshotting fails (stop-writes-on-bgsave-error option). Please check the Redis logs for details about the RDB error.\r\n"));
    shared.roslaveerr = createObject(OBJ_STRING,sdsnew(
        "-READONLY You can't write against a read only replica.\r\n"));
    shared.noautherr = createObject(OBJ_STRING,sdsnew(
        "-NOAUTH Authentication required.\r\n"));
    shared.oomerr = createObject(OBJ_STRING,sdsnew(
        "-OOM command not allowed when used memory > 'maxmemory'.\r\n"));
    shared.execaborterr = createObject(OBJ_STRING,sdsnew(
        "-EXECABORT Transaction discarded because of previous errors.\r\n"));
    shared.noreplicaserr = createObject(OBJ_STRING,sdsnew(
        "-NOREPLICAS Not enough good replicas to write.\r\n"));
    shared.busykeyerr = createObject(OBJ_STRING,sdsnew(
        "-BUSYKEY Target key name already exists.\r\n"));

    /* The shared NULL depends on the protocol version. 共享的 NULL 取决于协议版本。*/
    shared.null[0] = NULL;
    shared.null[1] = NULL;
    shared.null[2] = createObject(OBJ_STRING,sdsnew("$-1\r\n"));
    shared.null[3] = createObject(OBJ_STRING,sdsnew("_\r\n"));

    shared.nullarray[0] = NULL;
    shared.nullarray[1] = NULL;
    shared.nullarray[2] = createObject(OBJ_STRING,sdsnew("*-1\r\n"));
    shared.nullarray[3] = createObject(OBJ_STRING,sdsnew("_\r\n"));

    shared.emptymap[0] = NULL;
    shared.emptymap[1] = NULL;
    shared.emptymap[2] = createObject(OBJ_STRING,sdsnew("*0\r\n"));
    shared.emptymap[3] = createObject(OBJ_STRING,sdsnew("%0\r\n"));

    shared.emptyset[0] = NULL;
    shared.emptyset[1] = NULL;
    shared.emptyset[2] = createObject(OBJ_STRING,sdsnew("*0\r\n"));
    shared.emptyset[3] = createObject(OBJ_STRING,sdsnew("~0\r\n"));

    for (j = 0; j < PROTO_SHARED_SELECT_CMDS; j++) {
        char dictid_str[64];
        int dictid_len;

        dictid_len = ll2string(dictid_str,sizeof(dictid_str),j);
        shared.select[j] = createObject(OBJ_STRING,
            sdscatprintf(sdsempty(),
                "*2\r\n$6\r\nSELECT\r\n$%d\r\n%s\r\n",
                dictid_len, dictid_str));
    }
    shared.messagebulk = createStringObject("$7\r\nmessage\r\n",13);
    shared.pmessagebulk = createStringObject("$8\r\npmessage\r\n",14);
    shared.subscribebulk = createStringObject("$9\r\nsubscribe\r\n",15);
    shared.unsubscribebulk = createStringObject("$11\r\nunsubscribe\r\n",18);
    shared.ssubscribebulk = createStringObject("$10\r\nssubscribe\r\n", 17);
    shared.sunsubscribebulk = createStringObject("$12\r\nsunsubscribe\r\n", 19);
    shared.smessagebulk = createStringObject("$8\r\nsmessage\r\n", 14);
    shared.psubscribebulk = createStringObject("$10\r\npsubscribe\r\n",17);
    shared.punsubscribebulk = createStringObject("$12\r\npunsubscribe\r\n",19);

    /* Shared command names */
    shared.del = createStringObject("DEL",3);
    shared.unlink = createStringObject("UNLINK",6);
    shared.rpop = createStringObject("RPOP",4);
    shared.lpop = createStringObject("LPOP",4);
    shared.lpush = createStringObject("LPUSH",5);
    shared.rpoplpush = createStringObject("RPOPLPUSH",9);
    shared.lmove = createStringObject("LMOVE",5);
    shared.blmove = createStringObject("BLMOVE",6);
    shared.zpopmin = createStringObject("ZPOPMIN",7);
    shared.zpopmax = createStringObject("ZPOPMAX",7);
    shared.multi = createStringObject("MULTI",5);
    shared.exec = createStringObject("EXEC",4);
    shared.hset = createStringObject("HSET",4);
    shared.srem = createStringObject("SREM",4);
    shared.xgroup = createStringObject("XGROUP",6);
    shared.xclaim = createStringObject("XCLAIM",6);
    shared.script = createStringObject("SCRIPT",6);
    shared.replconf = createStringObject("REPLCONF",8);
    shared.pexpireat = createStringObject("PEXPIREAT",9);
    shared.pexpire = createStringObject("PEXPIRE",7);
    shared.persist = createStringObject("PERSIST",7);
    shared.set = createStringObject("SET",3);
    shared.eval = createStringObject("EVAL",4);

    /* Shared command argument */
    shared.left = createStringObject("left",4);
    shared.right = createStringObject("right",5);
    shared.pxat = createStringObject("PXAT", 4);
    shared.time = createStringObject("TIME",4);
    shared.retrycount = createStringObject("RETRYCOUNT",10);
    shared.force = createStringObject("FORCE",5);
    shared.justid = createStringObject("JUSTID",6);
    shared.entriesread = createStringObject("ENTRIESREAD",11);
    shared.lastid = createStringObject("LASTID",6);
    shared.default_username = createStringObject("default",7);
    shared.ping = createStringObject("ping",4);
    shared.setid = createStringObject("SETID",5);
    shared.keepttl = createStringObject("KEEPTTL",7);
    shared.absttl = createStringObject("ABSTTL",6);
    shared.load = createStringObject("LOAD",4);
    shared.createconsumer = createStringObject("CREATECONSUMER",14);
    shared.getack = createStringObject("GETACK",6);
    shared.special_asterick = createStringObject("*",1);
    shared.special_equals = createStringObject("=",1);
    shared.redacted = makeObjectShared(createStringObject("(redacted)",10));

    for (j = 0; j < OBJ_SHARED_INTEGERS; j++) {
        shared.integers[j] =
            makeObjectShared(createObject(OBJ_STRING,(void*)(long)j));
        shared.integers[j]->encoding = OBJ_ENCODING_INT;
    }
    for (j = 0; j < OBJ_SHARED_BULKHDR_LEN; j++) {
        shared.mbulkhdr[j] = createObject(OBJ_STRING,
            sdscatprintf(sdsempty(),"*%d\r\n",j));
        shared.bulkhdr[j] = createObject(OBJ_STRING,
            sdscatprintf(sdsempty(),"$%d\r\n",j));
        shared.maphdr[j] = createObject(OBJ_STRING,
            sdscatprintf(sdsempty(),"%%%d\r\n",j));
        shared.sethdr[j] = createObject(OBJ_STRING,
            sdscatprintf(sdsempty(),"~%d\r\n",j));
    }
    /* The following two shared objects, minstring and maxstring, are not
     * actually used for their value but as a special object meaning
     * respectively the minimum possible string and the maximum possible
     * string in string comparisons for the ZRANGEBYLEX command. */
    /**以下两个共享对象 minstring 和 maxstring 实际上并没有用于它们的值，
     * 而是作为一个特殊对象，分别表示 ZRANGEBYLEX 命令的字符串比较中的最小可能字符串和最大可能字符串。*/
    shared.minstring = sdsnew("minstring");
    shared.maxstring = sdsnew("maxstring");
}

void initServerConfig(void) {
    int j;
    char *default_bindaddr[CONFIG_DEFAULT_BINDADDR_COUNT] = CONFIG_DEFAULT_BINDADDR;

    initConfigValues();
    updateCachedTime(1);
    getRandomHexChars(server.runid,CONFIG_RUN_ID_SIZE);
    server.runid[CONFIG_RUN_ID_SIZE] = '\0';
    changeReplicationId();
    clearReplicationId2();
    server.hz = CONFIG_DEFAULT_HZ; /* Initialize it ASAP, even if it may get
                                      updated later after loading the config.
                                      This value may be used before the server
                                      is initialized.
                                      尽快初始化它，即使它可能会在加载配置后更新。该值可以在服务器初始化之前使用。*/
    server.timezone = getTimeZone(); /* Initialized by tzset(). 由 tzset() 初始化。*/
    server.configfile = NULL;
    server.executable = NULL;
    server.arch_bits = (sizeof(long) == 8) ? 64 : 32;
    server.bindaddr_count = CONFIG_DEFAULT_BINDADDR_COUNT;
    for (j = 0; j < CONFIG_DEFAULT_BINDADDR_COUNT; j++)
        server.bindaddr[j] = zstrdup(default_bindaddr[j]);
    server.ipfd.count = 0;
    server.tlsfd.count = 0;
    server.sofd = -1;
    server.active_expire_enabled = 1;
    server.skip_checksum_validation = 0;
    server.loading = 0;
    server.async_loading = 0;
    server.loading_rdb_used_mem = 0;
    server.aof_state = AOF_OFF;
    server.aof_rewrite_base_size = 0;
    server.aof_rewrite_scheduled = 0;
    server.aof_flush_sleep = 0;
    server.aof_last_fsync = time(NULL);
    server.aof_cur_timestamp = 0;
    atomicSet(server.aof_bio_fsync_status,C_OK);
    server.aof_rewrite_time_last = -1;
    server.aof_rewrite_time_start = -1;
    server.aof_lastbgrewrite_status = C_OK;
    server.aof_delayed_fsync = 0;
    server.aof_fd = -1;
    server.aof_selected_db = -1; /* Make sure the first time will not match 确保第一次不匹配*/
    server.aof_flush_postponed_start = 0;
    server.aof_last_incr_size = 0;
    server.active_defrag_running = 0;
    server.notify_keyspace_events = 0;
    server.blocked_clients = 0;
    memset(server.blocked_clients_by_type,0,
           sizeof(server.blocked_clients_by_type));
    server.shutdown_asap = 0;
    server.shutdown_flags = 0;
    server.shutdown_mstime = 0;
    server.cluster_module_flags = CLUSTER_MODULE_FLAG_NONE;
    server.migrate_cached_sockets = dictCreate(&migrateCacheDictType);
    server.next_client_id = 1; /* Client IDs, start from 1 .*/
    server.page_size = sysconf(_SC_PAGESIZE);
    server.pause_cron = 0;

    server.latency_tracking_info_percentiles_len = 3;
    server.latency_tracking_info_percentiles = zmalloc(sizeof(double)*(server.latency_tracking_info_percentiles_len));
    server.latency_tracking_info_percentiles[0] = 50.0;  /* p50 */
    server.latency_tracking_info_percentiles[1] = 99.0;  /* p99 */
    server.latency_tracking_info_percentiles[2] = 99.9;  /* p999 */

    unsigned int lruclock = getLRUClock();
    atomicSet(server.lruclock,lruclock);
    resetServerSaveParams();

    appendServerSaveParams(60*60,1);  /* save after 1 hour and 1 change 在 1 小时和 1 次更改后保存*/
    appendServerSaveParams(300,100);  /* save after 5 minutes and 100 changes */
    appendServerSaveParams(60,10000); /* save after 1 minute and 10000 changes */

    /* Replication related 复制相关*/
    server.masterhost = NULL;
    server.masterport = 6379;
    server.master = NULL;
    server.cached_master = NULL;
    server.master_initial_offset = -1;
    server.repl_state = REPL_STATE_NONE;
    server.repl_transfer_tmpfile = NULL;
    server.repl_transfer_fd = -1;
    server.repl_transfer_s = NULL;
    server.repl_syncio_timeout = CONFIG_REPL_SYNCIO_TIMEOUT;
    server.repl_down_since = 0; /* Never connected, repl is down since EVER.从未连接过，repl 从 EVER 开始就关闭了。 */
    server.master_repl_offset = 0;

    /* Replication partial resync backlog 复制部分重新同步积压*/
    server.repl_backlog = NULL;
    server.repl_no_slaves_since = time(NULL);

    /* Failover related */
    server.failover_end_time = 0;
    server.force_failover = 0;
    server.target_replica_host = NULL;
    server.target_replica_port = 0;
    server.failover_state = NO_FAILOVER;

    /* Client output buffer limits */
    for (j = 0; j < CLIENT_TYPE_OBUF_COUNT; j++)
        server.client_obuf_limits[j] = clientBufferLimitsDefaults[j];

    /* Linux OOM Score config */
    for (j = 0; j < CONFIG_OOM_COUNT; j++)
        server.oom_score_adj_values[j] = configOOMScoreAdjValuesDefaults[j];

    /* Double constants initialization  双常量初始化*/
    R_Zero = 0.0;
    R_PosInf = 1.0/R_Zero;
    R_NegInf = -1.0/R_Zero;
    R_Nan = R_Zero/R_Zero;

    /* Command table -- we initialize it here as it is part of the
     * initial configuration, since command names may be changed via
     * redis.conf using the rename-command directive. */
    /**命令表——我们在这里对其进行初始化，因为它是初始配置的一部分，
     * 因为命令名称可以通过 redis.conf 使用 rename-command 指令进行更改。*/
    server.commands = dictCreate(&commandTableDictType);
    server.orig_commands = dictCreate(&commandTableDictType);
    populateCommandTable();

    /* Debugging */
    server.watchdog_period = 0;
}

extern char **environ;

/* Restart the server, executing the same executable that started this
 * instance, with the same arguments and configuration file.
 *
 * The function is designed to directly call execve() so that the new
 * server instance will retain the PID of the previous one.
 *
 * The list of flags, that may be bitwise ORed together, alter the
 * behavior of this function:
 *
 * RESTART_SERVER_NONE              No flags.
 * RESTART_SERVER_GRACEFULLY        Do a proper shutdown before restarting.
 * RESTART_SERVER_CONFIG_REWRITE    Rewrite the config file before restarting.
 *
 * On success the function does not return, because the process turns into
 * a different process. On error C_ERR is returned. */
/**重新启动服务器，使用相同的参数和配置文件执行启动此实例的相同可执行文件。
 *
 * @return 该函数旨在直接调用 execve() 以便新的服务器实例将保留前一个的 PID。
 * 标志列表，可以按位或一起，改变这个函数的行为：
 *   RESTART_SERVER_NONE 没有标志。
 *   RESTART_SERVER_GRACEFULLY 在重新启动之前正确关闭。
 *   RESTART_SERVER_CONFIG_REWRITE 在重新启动之前重写配置文件。
 * 成功时，该函数不会返回，因为该过程会变成不同的过程。出错时返回 C_ERR。*/
int restartServer(int flags, mstime_t delay) {
    int j;

    /* Check if we still have accesses to the executable that started this
     * server instance. */
    /**检查我们是否仍然可以访问启动此服务器实例的可执行文件。*/
    if (access(server.executable,X_OK) == -1) {
        serverLog(LL_WARNING,"Can't restart: this process has no "
                             "permissions to execute %s", server.executable);
        return C_ERR;
    }

    /* Config rewriting. */
    if (flags & RESTART_SERVER_CONFIG_REWRITE &&
        server.configfile &&
        rewriteConfig(server.configfile, 0) == -1)
    {
        serverLog(LL_WARNING,"Can't restart: configuration rewrite process "
                             "failed: %s", strerror(errno));
        return C_ERR;
    }

    /* Perform a proper shutdown. We don't wait for lagging replicas though. */
    /**执行适当的关机。不过，我们不会等待滞后的副本。*/
    if (flags & RESTART_SERVER_GRACEFULLY &&
        prepareForShutdown(SHUTDOWN_NOW) != C_OK)
    {
        serverLog(LL_WARNING,"Can't restart: error preparing for shutdown");
        return C_ERR;
    }

    /* Close all file descriptors, with the exception of stdin, stdout, stderr
     * which are useful if we restart a Redis server which is not daemonized. */
    /**关闭所有文件描述符，除了 stdin、stdout、stderr 之外，如果我们重新启动没有守护进程的 Redis 服务器，它们会很有用。*/
    for (j = 3; j < (int)server.maxclients + 1024; j++) {
        /* Test the descriptor validity before closing it, otherwise
         * Valgrind issues a warning on close(). */
        /**在关闭之前测试描述符的有效性，否则 Valgrind 会在 close() 上发出警告。*/
        if (fcntl(j,F_GETFD) != -1) close(j);
    }

    /* Execute the server with the original command line. */
    /**使用原始命令行执行服务器。*/
    if (delay) usleep(delay*1000);
    zfree(server.exec_argv[0]);
    server.exec_argv[0] = zstrdup(server.executable);
    execve(server.executable,server.exec_argv,environ);

    /* If an error occurred here, there is nothing we can do, but exit. */
    /**如果这里发生错误，我们无能为力，只能退出。*/
    _exit(1);

    return C_ERR; /* Never reached. 从未达到。*/
}

/* This function will configure the current process's oom_score_adj according
 * to user specified configuration. This is currently implemented on Linux
 * only.
 *
 * A process_class value of -1 implies OOM_CONFIG_MASTER or OOM_CONFIG_REPLICA,
 * depending on current role.
 */
/**该函数会根据用户指定的配置配置当前进程的oom_score_adj。这目前仅在 Linux 上实现。
 * process_class 值为 -1 意味着 OOM_CONFIG_MASTER 或 OOM_CONFIG_REPLICA，具体取决于当前角色。*/
int setOOMScoreAdj(int process_class) {
    if (process_class == -1)
        process_class = (server.masterhost ? CONFIG_OOM_REPLICA : CONFIG_OOM_MASTER);

    serverAssert(process_class >= 0 && process_class < CONFIG_OOM_COUNT);

#ifdef HAVE_PROC_OOM_SCORE_ADJ
    /* The following statics are used to indicate Redis has changed the process's oom score.
     * And to save the original score so we can restore it later if needed.
     * We need this so when we disabled oom-score-adj (also during configuration rollback
     * when another configuration parameter was invalid and causes a rollback after
     * applying a new oom-score) we can return to the oom-score value from before our
     * adjustments. */
    /**以下静态数据用于指示 Redis 已更改进程的 oom 分数。
     * 并保存原始乐谱，以便我们以后可以在需要时恢复它。
     * 我们需要这个，所以当我们禁用 oom-score-adj 时
     * （也在配置回滚期间，当另一个配置参数无效并在应用新的 oom-score 后导致回滚）时，
     * 我们可以返回到调整之前的 oom-score 值。*/
    static int oom_score_adjusted_by_redis = 0;
    static int oom_score_adj_base = 0;

    int fd;
    int val;
    char buf[64];

    if (server.oom_score_adj != OOM_SCORE_ADJ_NO) {
        if (!oom_score_adjusted_by_redis) {
            oom_score_adjusted_by_redis = 1;
            /* Backup base value before enabling Redis control over oom score */
            /**在启用 Redis 控制 oom 分数之前备份基值*/
            fd = open("/proc/self/oom_score_adj", O_RDONLY);
            if (fd < 0 || read(fd, buf, sizeof(buf)) < 0) {
                serverLog(LL_WARNING, "Unable to read oom_score_adj: %s", strerror(errno));
                if (fd != -1) close(fd);
                return C_ERR;
            }
            oom_score_adj_base = atoi(buf);
            close(fd);
        }

        val = server.oom_score_adj_values[process_class];
        if (server.oom_score_adj == OOM_SCORE_RELATIVE)
            val += oom_score_adj_base;
        if (val > 1000) val = 1000;
        if (val < -1000) val = -1000;
    } else if (oom_score_adjusted_by_redis) {
        oom_score_adjusted_by_redis = 0;
        val = oom_score_adj_base;
    }
    else {
        return C_OK;
    }

    snprintf(buf, sizeof(buf) - 1, "%d\n", val);

    fd = open("/proc/self/oom_score_adj", O_WRONLY);
    if (fd < 0 || write(fd, buf, strlen(buf)) < 0) {
        serverLog(LL_WARNING, "Unable to write oom_score_adj: %s", strerror(errno));
        if (fd != -1) close(fd);
        return C_ERR;
    }

    close(fd);
    return C_OK;
#else
    /* Unsupported */
    return C_ERR;
#endif
}

/* This function will try to raise the max number of open files accordingly to
 * the configured max number of clients. It also reserves a number of file
 * descriptors (CONFIG_MIN_RESERVED_FDS) for extra operations of
 * persistence, listening sockets, log files and so forth.
 *
 * If it will not be possible to set the limit accordingly to the configured
 * max number of clients, the function will do the reverse setting
 * server.maxclients to the value that we can actually handle. */
/**此函数将尝试根据配置的最大客户端数来提高打开文件的最大数量。
 * 它还保留了一些文件描述符（CONFIG_MIN_RESERVED_FDS），用于持久性、侦听套接字、日志文件等的额外操作。
 * 如果无法根据配置的最大客户端数设置限制，则该函数会将 server.maxclients 反向设置为我们实际可以处理的值。*/
void adjustOpenFilesLimit(void) {
    rlim_t maxfiles = server.maxclients+CONFIG_MIN_RESERVED_FDS;
    struct rlimit limit;

    if (getrlimit(RLIMIT_NOFILE,&limit) == -1) {
        serverLog(LL_WARNING,"Unable to obtain the current NOFILE limit (%s), assuming 1024 and setting the max clients configuration accordingly.",
            strerror(errno));
        server.maxclients = 1024-CONFIG_MIN_RESERVED_FDS;
    } else {
        rlim_t oldlimit = limit.rlim_cur;

        /* Set the max number of files if the current limit is not enough
         * for our needs. */
        /**如果当前限制不足以满足我们的需要，请设置最大文件数。*/
        if (oldlimit < maxfiles) {
            rlim_t bestlimit;
            int setrlimit_error = 0;

            /* Try to set the file limit to match 'maxfiles' or at least
             * to the higher value supported less than maxfiles. */
            /**尝试将文件限制设置为匹配 'maxfiles' 或至少设置为小于 maxfiles 所支持的更高值。*/
            bestlimit = maxfiles;
            while(bestlimit > oldlimit) {
                rlim_t decr_step = 16;

                limit.rlim_cur = bestlimit;
                limit.rlim_max = bestlimit;
                if (setrlimit(RLIMIT_NOFILE,&limit) != -1) break;
                setrlimit_error = errno;

                /* We failed to set file limit to 'bestlimit'. Try with a
                 * smaller limit decrementing by a few FDs per iteration. */
                /**我们未能将文件限制设置为“bestlimit”。尝试使用较小的限制，每次迭代减少几个 FD。*/
                if (bestlimit < decr_step) {
                    bestlimit = oldlimit;
                    break;
                }
                bestlimit -= decr_step;
            }

            /* Assume that the limit we get initially is still valid if
             * our last try was even lower. */
            /**假设如果我们最后一次尝试更低，我们最初获得的限制仍然有效。*/
            if (bestlimit < oldlimit) bestlimit = oldlimit;

            if (bestlimit < maxfiles) {
                unsigned int old_maxclients = server.maxclients;
                server.maxclients = bestlimit-CONFIG_MIN_RESERVED_FDS;
                /* maxclients is unsigned so may overflow: in order
                 * to check if maxclients is now logically less than 1
                 * we test indirectly via bestlimit. */
                /**maxclients 是无符号的，因此可能会溢出：为了检查 maxclients 现在是否在逻辑上小于 1，我们通过 bestlimit 间接测试。*/
                if (bestlimit <= CONFIG_MIN_RESERVED_FDS) {
                    serverLog(LL_WARNING,"Your current 'ulimit -n' "
                        "of %llu is not enough for the server to start. "
                        "Please increase your open file limit to at least "
                        "%llu. Exiting.",
                        (unsigned long long) oldlimit,
                        (unsigned long long) maxfiles);
                    exit(1);
                }
                serverLog(LL_WARNING,"You requested maxclients of %d "
                    "requiring at least %llu max file descriptors.",
                    old_maxclients,
                    (unsigned long long) maxfiles);
                serverLog(LL_WARNING,"Server can't set maximum open files "
                    "to %llu because of OS error: %s.",
                    (unsigned long long) maxfiles, strerror(setrlimit_error));
                serverLog(LL_WARNING,"Current maximum open files is %llu. "
                    "maxclients has been reduced to %d to compensate for "
                    "low ulimit. "
                    "If you need higher maxclients increase 'ulimit -n'.",
                    (unsigned long long) bestlimit, server.maxclients);
            } else {
                serverLog(LL_NOTICE,"Increased maximum number of open files "
                    "to %llu (it was originally set to %llu).",
                    (unsigned long long) maxfiles,
                    (unsigned long long) oldlimit);
            }
        }
    }
}

/* Check that server.tcp_backlog can be actually enforced in Linux according
 * to the value of /proc/sys/net/core/somaxconn, or warn about it. */
/**根据 procsysnetcoresomaxconn 的值检查 server.tcp_backlog 是否可以在 Linux 中实际强制执行，或者警告它。*/
void checkTcpBacklogSettings(void) {
#if defined(HAVE_PROC_SOMAXCONN)
    FILE *fp = fopen("/proc/sys/net/core/somaxconn","r");
    char buf[1024];
    if (!fp) return;
    if (fgets(buf,sizeof(buf),fp) != NULL) {
        int somaxconn = atoi(buf);
        if (somaxconn > 0 && somaxconn < server.tcp_backlog) {
            serverLog(LL_WARNING,"WARNING: The TCP backlog setting of %d cannot be enforced because /proc/sys/net/core/somaxconn is set to the lower value of %d.", server.tcp_backlog, somaxconn);
        }
    }
    fclose(fp);
#elif defined(HAVE_SYSCTL_KIPC_SOMAXCONN)
    int somaxconn, mib[3];
    size_t len = sizeof(int);

    mib[0] = CTL_KERN;
    mib[1] = KERN_IPC;
    mib[2] = KIPC_SOMAXCONN;

    if (sysctl(mib, 3, &somaxconn, &len, NULL, 0) == 0) {
        if (somaxconn > 0 && somaxconn < server.tcp_backlog) {
            serverLog(LL_WARNING,"WARNING: The TCP backlog setting of %d cannot be enforced because kern.ipc.somaxconn is set to the lower value of %d.", server.tcp_backlog, somaxconn);
        }
    }
#elif defined(HAVE_SYSCTL_KERN_SOMAXCONN)
    int somaxconn, mib[2];
    size_t len = sizeof(int);

    mib[0] = CTL_KERN;
    mib[1] = KERN_SOMAXCONN;

    if (sysctl(mib, 2, &somaxconn, &len, NULL, 0) == 0) {
        if (somaxconn > 0 && somaxconn < server.tcp_backlog) {
            serverLog(LL_WARNING,"WARNING: The TCP backlog setting of %d cannot be enforced because kern.somaxconn is set to the lower value of %d.", server.tcp_backlog, somaxconn);
        }
    }
#elif defined(SOMAXCONN)
    if (SOMAXCONN < server.tcp_backlog) {
        serverLog(LL_WARNING,"WARNING: The TCP backlog setting of %d cannot be enforced because SOMAXCONN is set to the lower value of %d.", server.tcp_backlog, SOMAXCONN);
    }
#endif
}

void closeSocketListeners(socketFds *sfd) {
    int j;

    for (j = 0; j < sfd->count; j++) {
        if (sfd->fd[j] == -1) continue;

        aeDeleteFileEvent(server.el, sfd->fd[j], AE_READABLE);
        close(sfd->fd[j]);
    }

    sfd->count = 0;
}

/* Create an event handler for accepting new connections in TCP or TLS domain sockets.
 * This works atomically for all socket fds */
/**创建一个事件处理程序以接受 TCP 或 TLS 域套接字中的新连接。这适用于所有套接字 fd*/
int createSocketAcceptHandler(socketFds *sfd, aeFileProc *accept_handler) {
    int j;

    for (j = 0; j < sfd->count; j++) {
        if (aeCreateFileEvent(server.el, sfd->fd[j], AE_READABLE, accept_handler,NULL) == AE_ERR) {
            /* Rollback */
            for (j = j-1; j >= 0; j--) aeDeleteFileEvent(server.el, sfd->fd[j], AE_READABLE);
            return C_ERR;
        }
    }
    return C_OK;
}

/* Initialize a set of file descriptors to listen to the specified 'port'
 * binding the addresses specified in the Redis server configuration.
 *
 * The listening file descriptors are stored in the integer array 'fds'
 * and their number is set in '*count'.
 *
 * The addresses to bind are specified in the global server.bindaddr array
 * and their number is server.bindaddr_count. If the server configuration
 * contains no specific addresses to bind, this function will try to
 * bind * (all addresses) for both the IPv4 and IPv6 protocols.
 *
 * On success the function returns C_OK.
 *
 * On error the function returns C_ERR. For the function to be on
 * error, at least one of the server.bindaddr addresses was
 * impossible to bind, or no bind addresses were specified in the server
 * configuration but the function is not able to bind * for at least
 * one of the IPv4 or IPv6 protocols. */
/**初始化一组文件描述符以侦听绑定在 Redis 服务器配置中指定的地址的指定“端口”。
 * 监听文件描述符存储在整数数组“fds”中，它们的数量设置在“count”中。
 * 要绑定的地址在全局 server.bindaddr 数组中指定，它们的编号是 server.bindaddr_count。
 * 如果服务器配置不包含要绑定的特定地址，则此函数将尝试为 IPv4 和 IPv6 协议绑定（所有地址）。
 * 成功时，函数返回 C_OK。出错时，函数返回 C_ERR。要使函数出错，至少无法绑定 server.bindaddr 地址之一，
 * 或者在服务器配置中未指定绑定地址，但函数无法绑定 IPv4 或 IPv6 协议中的至少一个.*/
int listenToPort(int port, socketFds *sfd) {
    int j;
    char **bindaddr = server.bindaddr;

    /* If we have no bind address, we don't listen on a TCP socket */
    /**如果我们没有绑定地址，我们就不会监听 TCP 套接字*/
    if (server.bindaddr_count == 0) return C_OK;

    for (j = 0; j < server.bindaddr_count; j++) {
        char* addr = bindaddr[j];
        int optional = *addr == '-';
        if (optional) addr++;
        if (strchr(addr,':')) {
            /* Bind IPv6 address. */
            sfd->fd[sfd->count] = anetTcp6Server(server.neterr,port,addr,server.tcp_backlog);
        } else {
            /* Bind IPv4 address. */
            sfd->fd[sfd->count] = anetTcpServer(server.neterr,port,addr,server.tcp_backlog);
        }
        if (sfd->fd[sfd->count] == ANET_ERR) {
            int net_errno = errno;
            serverLog(LL_WARNING,
                "Warning: Could not create server TCP listening socket %s:%d: %s",
                addr, port, server.neterr);
            if (net_errno == EADDRNOTAVAIL && optional)
                continue;
            if (net_errno == ENOPROTOOPT     || net_errno == EPROTONOSUPPORT ||
                net_errno == ESOCKTNOSUPPORT || net_errno == EPFNOSUPPORT ||
                net_errno == EAFNOSUPPORT)
                continue;

            /* Rollback successful listens before exiting */
            /**退出前回滚成功监听*/
            closeSocketListeners(sfd);
            return C_ERR;
        }
        if (server.socket_mark_id > 0) anetSetSockMarkId(NULL, sfd->fd[sfd->count], server.socket_mark_id);
        anetNonBlock(NULL,sfd->fd[sfd->count]);
        anetCloexec(sfd->fd[sfd->count]);
        sfd->count++;
    }
    return C_OK;
}

/* Resets the stats that we expose via INFO or other means that we want
 * to reset via CONFIG RESETSTAT. The function is also used in order to
 * initialize these fields in initServer() at server startup. */
/**重置我们通过 INFO 或我们希望通过 CONFIG RESETSTAT 重置的其他方式公开的统计信息。
 *
 * @return 该函数还用于在服务器启动时初始化 initServer() 中的这些字段。*/
void resetServerStats(void) {
    int j;

    server.stat_numcommands = 0;
    server.stat_numconnections = 0;
    server.stat_expiredkeys = 0;
    server.stat_expired_stale_perc = 0;
    server.stat_expired_time_cap_reached_count = 0;
    server.stat_expire_cycle_time_used = 0;
    server.stat_evictedkeys = 0;
    server.stat_evictedclients = 0;
    server.stat_total_eviction_exceeded_time = 0;
    server.stat_last_eviction_exceeded_time = 0;
    server.stat_keyspace_misses = 0;
    server.stat_keyspace_hits = 0;
    server.stat_active_defrag_hits = 0;
    server.stat_active_defrag_misses = 0;
    server.stat_active_defrag_key_hits = 0;
    server.stat_active_defrag_key_misses = 0;
    server.stat_active_defrag_scanned = 0;
    server.stat_total_active_defrag_time = 0;
    server.stat_last_active_defrag_time = 0;
    server.stat_fork_time = 0;
    server.stat_fork_rate = 0;
    server.stat_total_forks = 0;
    server.stat_rejected_conn = 0;
    server.stat_sync_full = 0;
    server.stat_sync_partial_ok = 0;
    server.stat_sync_partial_err = 0;
    server.stat_io_reads_processed = 0;
    atomicSet(server.stat_total_reads_processed, 0);
    server.stat_io_writes_processed = 0;
    atomicSet(server.stat_total_writes_processed, 0);
    for (j = 0; j < STATS_METRIC_COUNT; j++) {
        server.inst_metric[j].idx = 0;
        server.inst_metric[j].last_sample_time = mstime();
        server.inst_metric[j].last_sample_count = 0;
        memset(server.inst_metric[j].samples,0,
            sizeof(server.inst_metric[j].samples));
    }
    server.stat_aof_rewrites = 0;
    server.stat_rdb_saves = 0;
    server.stat_aofrw_consecutive_failures = 0;
    atomicSet(server.stat_net_input_bytes, 0);
    atomicSet(server.stat_net_output_bytes, 0);
    atomicSet(server.stat_net_repl_input_bytes, 0);
    atomicSet(server.stat_net_repl_output_bytes, 0);
    server.stat_unexpected_error_replies = 0;
    server.stat_total_error_replies = 0;
    server.stat_dump_payload_sanitizations = 0;
    server.aof_delayed_fsync = 0;
    server.stat_reply_buffer_shrinks = 0;
    server.stat_reply_buffer_expands = 0;
    lazyfreeResetStats();
}

/* Make the thread killable at any time, so that kill threads functions
 * can work reliably (default cancelability type is PTHREAD_CANCEL_DEFERRED).
 * Needed for pthread_cancel used by the fast memory test used by the crash report. */
/**使线程随时可终止，以便终止线程函数能够可靠地工作（默认可取消类型为 PTHREAD_CANCEL_DEFERRED）。
 * 崩溃报告使用的快速内存测试使用的 pthread_cancel 需要。*/
void makeThreadKillable(void) {
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
}

void initServer(void) {
    int j;

    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    setupSignalHandlers();
    makeThreadKillable();

    if (server.syslog_enabled) {
        openlog(server.syslog_ident, LOG_PID | LOG_NDELAY | LOG_NOWAIT,
            server.syslog_facility);
    }

    /* Initialization after setting defaults from the config system. */
    /**从配置系统设置默认值后初始化。*/
    server.aof_state = server.aof_enabled ? AOF_ON : AOF_OFF;
    server.hz = server.config_hz;
    server.pid = getpid();
    server.in_fork_child = CHILD_TYPE_NONE;
    server.main_thread_id = pthread_self();
    server.current_client = NULL;
    server.errors = raxNew();
    server.fixed_time_expire = 0;
    server.in_nested_call = 0;
    server.clients = listCreate();
    server.clients_index = raxNew();
    server.clients_to_close = listCreate();
    server.slaves = listCreate();
    server.monitors = listCreate();
    server.clients_pending_write = listCreate();
    server.clients_pending_read = listCreate();
    server.clients_timeout_table = raxNew();
    server.replication_allowed = 1;
    server.slaveseldb = -1; /* Force to emit the first SELECT command. 强制发出第一个 SELECT 命令。*/
    server.unblocked_clients = listCreate();
    server.ready_keys = listCreate();
    server.tracking_pending_keys = listCreate();
    server.clients_waiting_acks = listCreate();
    server.get_ack_from_slaves = 0;
    server.client_pause_type = CLIENT_PAUSE_OFF;
    server.client_pause_end_time = 0;
    memset(server.client_pause_per_purpose, 0,
           sizeof(server.client_pause_per_purpose));
    server.postponed_clients = listCreate();
    server.events_processed_while_blocked = 0;
    server.system_memory_size = zmalloc_get_memory_size();
    server.blocked_last_cron = 0;
    server.blocking_op_nesting = 0;
    server.thp_enabled = 0;
    server.cluster_drop_packet_filter = -1;
    server.reply_buffer_peak_reset_time = REPLY_BUFFER_DEFAULT_PEAK_RESET_TIME;
    server.reply_buffer_resizing_enabled = 1;
    resetReplicationBuffer();

    if ((server.tls_port || server.tls_replication || server.tls_cluster)
                && tlsConfigure(&server.tls_ctx_config) == C_ERR) {
        serverLog(LL_WARNING, "Failed to configure TLS. Check logs for more info.");
        exit(1);
    }

    for (j = 0; j < CLIENT_MEM_USAGE_BUCKETS; j++) {
        server.client_mem_usage_buckets[j].mem_usage_sum = 0;
        server.client_mem_usage_buckets[j].clients = listCreate();
    }

    createSharedObjects();
    adjustOpenFilesLimit();
    const char *clk_msg = monotonicInit();
    serverLog(LL_NOTICE, "monotonic clock: %s", clk_msg);
    server.el = aeCreateEventLoop(server.maxclients+CONFIG_FDSET_INCR);
    if (server.el == NULL) {
        serverLog(LL_WARNING,
            "Failed creating the event loop. Error message: '%s'",
            strerror(errno));
        exit(1);
    }
    server.db = zmalloc(sizeof(redisDb)*server.dbnum);

    /* Open the TCP listening socket for the user commands. */
    /**为用户命令打开 TCP 侦听套接字。*/
    if (server.port != 0 &&
        listenToPort(server.port,&server.ipfd) == C_ERR) {
        /* Note: the following log text is matched by the test suite. */
        /**注意：以下日志文本由测试套件匹配。*/
        serverLog(LL_WARNING, "Failed listening on port %u (TCP), aborting.", server.port);
        exit(1);
    }
    if (server.tls_port != 0 &&
        listenToPort(server.tls_port,&server.tlsfd) == C_ERR) {
        /* Note: the following log text is matched by the test suite. */
        /**注意：以下日志文本由测试套件匹配。*/
        serverLog(LL_WARNING, "Failed listening on port %u (TLS), aborting.", server.tls_port);
        exit(1);
    }

    /* Open the listening Unix domain socket. */
    /**打开侦听的 Unix 域套接字。*/
    if (server.unixsocket != NULL) {
        unlink(server.unixsocket); /* don't care if this fails 不在乎这是否失败*/
        server.sofd = anetUnixServer(server.neterr,server.unixsocket,
            (mode_t)server.unixsocketperm, server.tcp_backlog);
        if (server.sofd == ANET_ERR) {
            serverLog(LL_WARNING, "Failed opening Unix socket: %s", server.neterr);
            exit(1);
        }
        anetNonBlock(NULL,server.sofd);
        anetCloexec(server.sofd);
    }

    /* Abort if there are no listening sockets at all. */
    /**如果根本没有侦听套接字，则中止。*/
    if (server.ipfd.count == 0 && server.tlsfd.count == 0 && server.sofd < 0) {
        serverLog(LL_WARNING, "Configured to not listen anywhere, exiting.");
        exit(1);
    }

    /* Create the Redis databases, and initialize other internal state. */
    /**创建 Redis 数据库，并初始化其他内部状态。*/
    for (j = 0; j < server.dbnum; j++) {
        server.db[j].dict = dictCreate(&dbDictType);
        server.db[j].expires = dictCreate(&dbExpiresDictType);
        server.db[j].expires_cursor = 0;
        server.db[j].blocking_keys = dictCreate(&keylistDictType);
        server.db[j].ready_keys = dictCreate(&objectKeyPointerValueDictType);
        server.db[j].watched_keys = dictCreate(&keylistDictType);
        server.db[j].id = j;
        server.db[j].avg_ttl = 0;
        server.db[j].defrag_later = listCreate();
        server.db[j].slots_to_keys = NULL; /* Set by clusterInit later on if necessary. 如有必要，稍后由 clusterInit 设置。*/
        listSetFreeMethod(server.db[j].defrag_later,(void (*)(void*))sdsfree);
    }
    evictionPoolAlloc(); /* Initialize the LRU keys pool. 初始化 LRU 密钥池。*/
    server.pubsub_channels = dictCreate(&keylistDictType);
    server.pubsub_patterns = dictCreate(&keylistDictType);
    server.pubsubshard_channels = dictCreate(&keylistDictType);
    server.cronloops = 0;
    server.in_exec = 0;
    server.busy_module_yield_flags = BUSY_MODULE_YIELD_NONE;
    server.busy_module_yield_reply = NULL;
    server.core_propagates = 0;
    server.propagate_no_multi = 0;
    server.module_ctx_nesting = 0;
    server.client_pause_in_transaction = 0;
    server.child_pid = -1;
    server.child_type = CHILD_TYPE_NONE;
    server.rdb_child_type = RDB_CHILD_TYPE_NONE;
    server.rdb_pipe_conns = NULL;
    server.rdb_pipe_numconns = 0;
    server.rdb_pipe_numconns_writing = 0;
    server.rdb_pipe_buff = NULL;
    server.rdb_pipe_bufflen = 0;
    server.rdb_bgsave_scheduled = 0;
    server.child_info_pipe[0] = -1;
    server.child_info_pipe[1] = -1;
    server.child_info_nread = 0;
    server.aof_buf = sdsempty();
    server.lastsave = time(NULL); /* At startup we consider the DB saved. 在启动时，我们认为数据库已保存。*/
    server.lastbgsave_try = 0;    /* At startup we never tried to BGSAVE. 在启动时，我们从未尝试过 BGSAVE。*/
    server.rdb_save_time_last = -1;
    server.rdb_save_time_start = -1;
    server.rdb_last_load_keys_expired = 0;
    server.rdb_last_load_keys_loaded = 0;
    server.dirty = 0;
    resetServerStats();
    /* A few stats we don't want to reset: server startup time, and peak mem. */
    /**一些我们不想重置的统计数据：服务器启动时间和峰值内存。*/
    server.stat_starttime = time(NULL);
    server.stat_peak_memory = 0;
    server.stat_current_cow_peak = 0;
    server.stat_current_cow_bytes = 0;
    server.stat_current_cow_updated = 0;
    server.stat_current_save_keys_processed = 0;
    server.stat_current_save_keys_total = 0;
    server.stat_rdb_cow_bytes = 0;
    server.stat_aof_cow_bytes = 0;
    server.stat_module_cow_bytes = 0;
    server.stat_module_progress = 0;
    for (int j = 0; j < CLIENT_TYPE_COUNT; j++)
        server.stat_clients_type_memory[j] = 0;
    server.stat_cluster_links_memory = 0;
    server.cron_malloc_stats.zmalloc_used = 0;
    server.cron_malloc_stats.process_rss = 0;
    server.cron_malloc_stats.allocator_allocated = 0;
    server.cron_malloc_stats.allocator_active = 0;
    server.cron_malloc_stats.allocator_resident = 0;
    server.lastbgsave_status = C_OK;
    server.aof_last_write_status = C_OK;
    server.aof_last_write_errno = 0;
    server.repl_good_slaves_count = 0;
    server.last_sig_received = 0;

    /* Create the timer callback, this is our way to process many background
     * operations incrementally, like clients timeout, eviction of unaccessed
     * expired keys and so forth. */
    /**创建计时器回调，这是我们增量处理许多后台操作的方式，例如客户端超时、未访问的过期键的驱逐等。*/
    if (aeCreateTimeEvent(server.el, 1, serverCron, NULL, NULL) == AE_ERR) {
        serverPanic("Can't create event loop timers.");
        exit(1);
    }

    /* Create an event handler for accepting new connections in TCP and Unix
     * domain sockets. */
    /**创建一个事件处理程序以接受 TCP 和 Unix 域套接字中的新连接。*/
    if (createSocketAcceptHandler(&server.ipfd, acceptTcpHandler) != C_OK) {
        serverPanic("Unrecoverable error creating TCP socket accept handler.");
    }
    if (createSocketAcceptHandler(&server.tlsfd, acceptTLSHandler) != C_OK) {
        serverPanic("Unrecoverable error creating TLS socket accept handler.");
    }
    if (server.sofd > 0 && aeCreateFileEvent(server.el,server.sofd,AE_READABLE,
        acceptUnixHandler,NULL) == AE_ERR) serverPanic("Unrecoverable error creating server.sofd file event.");


    /* Register a readable event for the pipe used to awake the event loop
     * from module threads. */
    /**为用于从模块线程唤醒事件循环的管道注册一个可读事件。*/
    if (aeCreateFileEvent(server.el, server.module_pipe[0], AE_READABLE,
        modulePipeReadable,NULL) == AE_ERR) {
            serverPanic(
                "Error registering the readable event for the module pipe.");
    }

    /* Register before and after sleep handlers (note this needs to be done
     * before loading persistence since it is used by processEventsWhileBlocked. */
    /**在睡眠处理程序之前和之后注册（注意这需要在加载持久性之前完成，因为它被 processEventsWhileBlocked 使用。*/
    aeSetBeforeSleepProc(server.el,beforeSleep);
    aeSetAfterSleepProc(server.el,afterSleep);

    /* 32 bit instances are limited to 4GB of address space, so if there is
     * no explicit limit in the user provided configuration we set a limit
     * at 3 GB using maxmemory with 'noeviction' policy'. This avoids
     * useless crashes of the Redis instance for out of memory. */
    /**32 位实例被限制为 4GB 的地址空间，因此如果用户提供的配置中没有明确的限制，
     * 我们使用带有“noeviction”策略的 maxmemory 将限制设置为 3GB。
     * 这避免了 Redis 实例因内存不足而导致的无用崩溃。*/
    if (server.arch_bits == 32 && server.maxmemory == 0) {
        serverLog(LL_WARNING,"Warning: 32 bit instance detected but no memory limit set. Setting 3 GB maxmemory limit with 'noeviction' policy now.");
        server.maxmemory = 3072LL*(1024*1024); /* 3 GB */
        server.maxmemory_policy = MAXMEMORY_NO_EVICTION;
    }

    if (server.cluster_enabled) clusterInit();
    scriptingInit(1);
    functionsInit();
    slowlogInit();
    latencyMonitorInit();

    /* Initialize ACL default password if it exists */
    /**初始化 ACL 默认密码（如果存在）*/
    ACLUpdateDefaultUserPassword(server.requirepass);

    applyWatchdogPeriod();
}

/* Some steps in server initialization need to be done last (after modules
 * are loaded).
 * Specifically, creation of threads due to a race bug in ld.so, in which
 * Thread Local Storage initialization collides with dlopen call.
 * 服务器初始化中的一些步骤需要最后完成（在加载模块之后）。
 * 具体来说，由于 ld.so 中的竞争错误而创建线程，其中线程本地存储初始化与 dlopen 调用发生冲突。
 * see: https://sourceware.org/bugzilla/show_bug.cgi?id=19329 */
void InitServerLast() {
    bioInit();
    initThreadedIO();
    set_jemalloc_bg_thread(server.jemalloc_bg_thread);
    server.initial_memory_usage = zmalloc_used_memory();
}

/* The purpose of this function is to try to "glue" consecutive range
 * key specs in order to build the legacy (first,last,step) spec
 * used by the COMMAND command.
 * By far the most common case is just one range spec (e.g. SET)
 * but some commands' ranges were split into two or more ranges
 * in order to have different flags for different keys (e.g. SMOVE,
 * first key is "RW ACCESS DELETE", second key is "RW INSERT").
 *
 * Additionally set the CMD_MOVABLE_KEYS flag for commands that may have key
 * names in their arguments, but the legacy range spec doesn't cover all of them.
 *
 * This function uses very basic heuristics and is "best effort":
 * 1. Only commands which have only "range" specs are considered.
 * 2. Only range specs with keystep of 1 are considered.
 * 3. The order of the range specs must be ascending (i.e.
 *    lastkey of spec[i] == firstkey-1 of spec[i+1]).
 *
 * This function will succeed on all native Redis commands and may
 * fail on module commands, even if it only has "range" specs that
 * could actually be "glued", in the following cases:
 * 1. The order of "range" specs is not ascending (e.g. the spec for
 *    the key at index 2 was added before the spec of the key at
 *    index 1).
 * 2. The "range" specs have keystep >1.
 *
 * If this functions fails it means that the legacy (first,last,step)
 * spec used by COMMAND will show 0,0,0. This is not a dire situation
 * because anyway the legacy (first,last,step) spec is to be deprecated
 * and one should use the new key specs scheme.
 */
/**此函数的目的是尝试“粘合”连续的范围键规范，以构建 COMMAND 命令使用的遗留（第一、最后、步骤）规范。
 * 到目前为止，最常见的情况只是一个范围规范（例如 SET），但一些命令的范围被分成两个或多个范围，
 * 以便为不同的键提供不同的标志（例如 SMOVE，第一个键是“RW ACCESS DELETE”，第二个键是“RW INSERT”）。
 * 此外，为参数中可能包含键名的命令设置 CMD_MOVABLE_KEYS 标志，但旧版范围规范并未涵盖所有这些。
 * 此功能使用非常基本的启发式方法并且是“尽力而为”：
 *   1. 仅考虑仅具有“范围”规范的命令。
 *   2. 只考虑 keystep 为 1 的范围规格。
 *   3. range specs 的顺序必须是升序的（即 spec[i] 的 lastkey == spec[i+1] 的 firstkey-1）。
 * 在以下情况下，此函数将在所有原生 Redis 命令上成功，并且可能在模块命令上失败，
 * 即使它只有“范围”规范实际上可以“粘合”，在以下情况下：
 *   1. “范围”规范的顺序不是升序（例如，索引 2 处的键的规范是在索引 1 处的键规范之前添加的）。
 *   2.“范围”规格的关键步长>1。
 * 如果此功能失败，则意味着 COMMAND 使用的旧版（第一、最后、步骤）规范将显示 0,0,0。
 * 这不是一个可怕的情况，因为无论如何旧的（第一，最后，步骤）规范将被弃用，并且应该使用新的关键规范方案。*/
void populateCommandLegacyRangeSpec(struct redisCommand *c) {
    memset(&c->legacy_range_key_spec, 0, sizeof(c->legacy_range_key_spec));

    /* Set the movablekeys flag if we have a GETKEYS flag for modules.
     * Note that for native redis commands, we always have keyspecs,
     * with enough information to rely on for movablekeys. */
    /**如果我们有模块的 GETKEYS 标志，则设置可移动键标志。
     * 请注意，对于本机 redis 命令，我们总是有 keyspecs，有足够的信息可以依赖于可移动键。*/
    if (c->flags & CMD_MODULE_GETKEYS)
        c->flags |= CMD_MOVABLE_KEYS;

    /* no key-specs, no keys, exit. 没有密钥规格，没有密钥，退出。*/
    if (c->key_specs_num == 0) {
        return;
    }

    if (c->key_specs_num == 1 &&
        c->key_specs[0].begin_search_type == KSPEC_BS_INDEX &&
        c->key_specs[0].find_keys_type == KSPEC_FK_RANGE)
    {
        /* Quick win, exactly one range spec. 快速获胜，恰好是一个范围规格。*/
        c->legacy_range_key_spec = c->key_specs[0];
        /* If it has the incomplete flag, set the movablekeys flag on the command. */
        /**如果它有不完整的标志，请在命令上设置可移动键标志。*/
        if (c->key_specs[0].flags & CMD_KEY_INCOMPLETE)
            c->flags |= CMD_MOVABLE_KEYS;
        return;
    }

    int firstkey = INT_MAX, lastkey = 0;
    int prev_lastkey = 0;
    for (int i = 0; i < c->key_specs_num; i++) {
        if (c->key_specs[i].begin_search_type != KSPEC_BS_INDEX ||
            c->key_specs[i].find_keys_type != KSPEC_FK_RANGE)
        {
            /* Found an incompatible (non range) spec, skip it, and set the movablekeys flag. */
            /**找到一个不兼容的（非范围）规范，跳过它，并设置可移动键标志。*/
            c->flags |= CMD_MOVABLE_KEYS;
            continue;
        }
        if (c->key_specs[i].fk.range.keystep != 1 ||
            (prev_lastkey && prev_lastkey != c->key_specs[i].bs.index.pos-1))
        {
            /* Found a range spec that's not plain (step of 1) or not consecutive to the previous one.
             * Skip it, and we set the movablekeys flag. */
            /**找到了一个范围规范，该规范不简单（步长为 1）或与前一个不连续。跳过它，我们设置可移动键标志。*/
            c->flags |= CMD_MOVABLE_KEYS;
            continue;
        }
        if (c->key_specs[i].flags & CMD_KEY_INCOMPLETE) {
            /* The spec we're using is incomplete, we can use it, but we also have to set the movablekeys flag. */
            /**我们使用的规范是不完整的，我们可以使用它，但我们还必须设置可移动键标志。*/
            c->flags |= CMD_MOVABLE_KEYS;
        }
        firstkey = min(firstkey, c->key_specs[i].bs.index.pos);
        /* Get the absolute index for lastkey (in the "range" spec, lastkey is relative to firstkey) */
        /**获取 lastkey 的绝对索引（在“range”规范中，lastkey 是相对于 firstkey）*/
        int lastkey_abs_index = c->key_specs[i].fk.range.lastkey;
        if (lastkey_abs_index >= 0)
            lastkey_abs_index += c->key_specs[i].bs.index.pos;
        /* For lastkey we use unsigned comparison to handle negative values correctly */
        /**对于 lastkey，我们使用无符号比较来正确处理负值*/
        lastkey = max((unsigned)lastkey, (unsigned)lastkey_abs_index);
        prev_lastkey = lastkey;
    }

    if (firstkey == INT_MAX) {
        /* Couldn't find range specs, the legacy range spec will remain empty, and we set the movablekeys flag. */
        /**找不到范围规范，旧的范围规范将保持为空，我们设置了可移动键标志。*/
        c->flags |= CMD_MOVABLE_KEYS;
        return;
    }

    serverAssert(firstkey != 0);
    serverAssert(lastkey != 0);

    c->legacy_range_key_spec.begin_search_type = KSPEC_BS_INDEX;
    c->legacy_range_key_spec.bs.index.pos = firstkey;
    c->legacy_range_key_spec.find_keys_type = KSPEC_FK_RANGE;
    c->legacy_range_key_spec.fk.range.lastkey =
            lastkey < 0 ? lastkey : (lastkey-firstkey); /* in the "range" spec, lastkey is relative to firstkey
                                                           在“范围”规范中，lastkey 是相对于 firstkey*/
    c->legacy_range_key_spec.fk.range.keystep = 1;
    c->legacy_range_key_spec.fk.range.limit = 0;
}

sds catSubCommandFullname(const char *parent_name, const char *sub_name) {
    return sdscatfmt(sdsempty(), "%s|%s", parent_name, sub_name);
}

void commandAddSubcommand(struct redisCommand *parent, struct redisCommand *subcommand, const char *declared_name) {
    if (!parent->subcommands_dict)
        parent->subcommands_dict = dictCreate(&commandTableDictType);

    subcommand->parent = parent; /* Assign the parent command 分配父命令*/
    subcommand->id = ACLGetCommandID(subcommand->fullname); /* Assign the ID used for ACL. 分配用于 ACL 的 ID。*/

    serverAssert(dictAdd(parent->subcommands_dict, sdsnew(declared_name), subcommand) == DICT_OK);
}

/* Set implicit ACl categories (see comment above the definition of
 * struct redisCommand). */
/**设置隐式 ACl 类别（见 struct redisCommand 定义上面的注释）。*/
void setImplicitACLCategories(struct redisCommand *c) {
    if (c->flags & CMD_WRITE)
        c->acl_categories |= ACL_CATEGORY_WRITE;
    /* Exclude scripting commands from the RO category. */
    /**从 RO 类别中排除脚本命令。*/
    if (c->flags & CMD_READONLY && !(c->acl_categories & ACL_CATEGORY_SCRIPTING))
        c->acl_categories |= ACL_CATEGORY_READ;
    if (c->flags & CMD_ADMIN)
        c->acl_categories |= ACL_CATEGORY_ADMIN|ACL_CATEGORY_DANGEROUS;
    if (c->flags & CMD_PUBSUB)
        c->acl_categories |= ACL_CATEGORY_PUBSUB;
    if (c->flags & CMD_FAST)
        c->acl_categories |= ACL_CATEGORY_FAST;
    if (c->flags & CMD_BLOCKING)
        c->acl_categories |= ACL_CATEGORY_BLOCKING;

    /* If it's not @fast is @slow in this binary world. */
    /**如果不是@fast，则在这个二进制世界中是@slow。*/
    if (!(c->acl_categories & ACL_CATEGORY_FAST))
        c->acl_categories |= ACL_CATEGORY_SLOW;
}

/* Recursively populate the args structure (setting num_args to the number of
 * subargs) and return the number of args. */
/**递归填充 args 结构（将 num_args 设置为 subargs 的数量）并返回 args 的数量。*/
int populateArgsStructure(struct redisCommandArg *args) {
    if (!args)
        return 0;
    int count = 0;
    while (args->name) {
        serverAssert(count < INT_MAX);
        args->num_args = populateArgsStructure(args->subargs);
        count++;
        args++;
    }
    return count;
}

/* Recursively populate the command structure.
 *
 * On success, the function return C_OK. Otherwise C_ERR is returned and we won't
 * add this command in the commands dict. */
/**递归填充命令结构。成功时，函数返回 C_OK。否则返回 C_ERR，我们不会在命令字典中添加此命令。*/
int populateCommandStructure(struct redisCommand *c) {
    /* If the command marks with CMD_SENTINEL, it exists in sentinel. */
    /**如果该命令带有 CMD_SENTINEL 标记，则它存在于哨兵中。*/
    if (!(c->flags & CMD_SENTINEL) && server.sentinel_mode)
        return C_ERR;

    /* If the command marks with CMD_ONLY_SENTINEL, it only exists in sentinel. */
    /**如果该命令带有 CMD_ONLY_SENTINEL 标记，则它只存在于哨兵中。*/
    if (c->flags & CMD_ONLY_SENTINEL && !server.sentinel_mode)
        return C_ERR;

    /* Translate the command string flags description into an actual
     * set of flags. */
    /**将命令字符串标志描述转换为一组实际的标志。*/
    setImplicitACLCategories(c);

    /* Redis commands don't need more args than STATIC_KEY_SPECS_NUM (Number of keys
     * specs can be greater than STATIC_KEY_SPECS_NUM only for module commands) */
    /**Redis 命令不需要比 STATIC_KEY_SPECS_NUM 更多的参数
     * （仅对于模块命令，键规范的数量可以大于 STATIC_KEY_SPECS_NUM）*/
    c->key_specs = c->key_specs_static;
    c->key_specs_max = STATIC_KEY_SPECS_NUM;

    /* We start with an unallocated histogram and only allocate memory when a command
     * has been issued for the first time */
    /**我们从未分配的直方图开始，仅在第一次发出命令时分配内存*/
    c->latency_histogram = NULL;

    for (int i = 0; i < STATIC_KEY_SPECS_NUM; i++) {
        if (c->key_specs[i].begin_search_type == KSPEC_BS_INVALID)
            break;
        c->key_specs_num++;
    }

    /* Count things so we don't have to use deferred reply in COMMAND reply. */
    /**计算事情，这样我们就不必在命令回复中使用延迟回复。*/
    while (c->history && c->history[c->num_history].since)
        c->num_history++;
    while (c->tips && c->tips[c->num_tips])
        c->num_tips++;
    c->num_args = populateArgsStructure(c->args);

    /* Handle the legacy range spec and the "movablekeys" flag (must be done after populating all key specs). */
    /**处理遗留范围规范和“movablekeys”标志（必须在填充所有关键规范后完成）。*/
    populateCommandLegacyRangeSpec(c);

    /* Assign the ID used for ACL. 分配用于 ACL 的 ID。*/
    c->id = ACLGetCommandID(c->fullname);

    /* Handle subcommands */
    if (c->subcommands) {
        for (int j = 0; c->subcommands[j].declared_name; j++) {
            struct redisCommand *sub = c->subcommands+j;

            sub->fullname = catSubCommandFullname(c->declared_name, sub->declared_name);
            if (populateCommandStructure(sub) == C_ERR)
                continue;

            commandAddSubcommand(c, sub, sub->declared_name);
        }
    }

    return C_OK;
}

extern struct redisCommand redisCommandTable[];

/* Populates the Redis Command Table dict from the static table in commands.c
 * which is auto generated from the json files in the commands folder. */
/**从 commands.c 中的静态表填充 Redis 命令表字典，该静态表是从命令文件夹中的 json 文件自动生成的。*/
void populateCommandTable(void) {
    int j;
    struct redisCommand *c;

    for (j = 0;; j++) {
        c = redisCommandTable + j;
        if (c->declared_name == NULL)
            break;

        int retval1, retval2;

        c->fullname = sdsnew(c->declared_name);
        if (populateCommandStructure(c) == C_ERR)
            continue;

        retval1 = dictAdd(server.commands, sdsdup(c->fullname), c);
        /* Populate an additional dictionary that will be unaffected
         * by rename-command statements in redis.conf. */
        /**填充一个不受 redis.conf 中 rename-command 语句影响的附加字典。*/
        retval2 = dictAdd(server.orig_commands, sdsdup(c->fullname), c);
        serverAssert(retval1 == DICT_OK && retval2 == DICT_OK);
    }
}

void resetCommandTableStats(dict* commands) {
    struct redisCommand *c;
    dictEntry *de;
    dictIterator *di;

    di = dictGetSafeIterator(commands);
    while((de = dictNext(di)) != NULL) {
        c = (struct redisCommand *) dictGetVal(de);
        c->microseconds = 0;
        c->calls = 0;
        c->rejected_calls = 0;
        c->failed_calls = 0;
        if(c->latency_histogram) {
            hdr_close(c->latency_histogram);
            c->latency_histogram = NULL;
        }
        if (c->subcommands_dict)
            resetCommandTableStats(c->subcommands_dict);
    }
    dictReleaseIterator(di);
}

void resetErrorTableStats(void) {
    raxFreeWithCallback(server.errors, zfree);
    server.errors = raxNew();
}

/* ========================== Redis OP Array API ============================ */

void redisOpArrayInit(redisOpArray *oa) {
    oa->ops = NULL;
    oa->numops = 0;
    oa->capacity = 0;
}

int redisOpArrayAppend(redisOpArray *oa, int dbid, robj **argv, int argc, int target) {
    redisOp *op;
    int prev_capacity = oa->capacity;

    if (oa->numops == 0) {
        oa->capacity = 16;
    } else if (oa->numops >= oa->capacity) {
        oa->capacity *= 2;
    }

    if (prev_capacity != oa->capacity)
        oa->ops = zrealloc(oa->ops,sizeof(redisOp)*oa->capacity);
    op = oa->ops+oa->numops;
    op->dbid = dbid;
    op->argv = argv;
    op->argc = argc;
    op->target = target;
    oa->numops++;
    return oa->numops;
}

void redisOpArrayFree(redisOpArray *oa) {
    while(oa->numops) {
        int j;
        redisOp *op;

        oa->numops--;
        op = oa->ops+oa->numops;
        for (j = 0; j < op->argc; j++)
            decrRefCount(op->argv[j]);
        zfree(op->argv);
    }
    zfree(oa->ops);
    redisOpArrayInit(oa);
}

/* ====================== Commands lookup and execution 命令查找和执行===================== */

int isContainerCommandBySds(sds s) {
    struct redisCommand *base_cmd = dictFetchValue(server.commands, s);
    int has_subcommands = base_cmd && base_cmd->subcommands_dict;
    return has_subcommands;
}

struct redisCommand *lookupSubcommand(struct redisCommand *container, sds sub_name) {
    return dictFetchValue(container->subcommands_dict, sub_name);
}

/* Look up a command by argv and argc
 *
 * If `strict` is not 0 we expect argc to be exact (i.e. argc==2
 * for a subcommand and argc==1 for a top-level command)
 * `strict` should be used every time we want to look up a command
 * name (e.g. in COMMAND INFO) rather than to find the command
 * a user requested to execute (in processCommand).
 */
/**通过 argv 和 argc 查找命令 如果 `strict` 不为 0，
 * 我们希望 argc 准确（即 argc==2 用于子命令，argc==1 用于顶级命令）每次都应使用 `strict`我们想查找
 * 命令名称（例如在 COMMAND INFO 中）而不是查找用户请求执行的命令（在 processCommand 中）。*/
struct redisCommand *lookupCommandLogic(dict *commands, robj **argv, int argc, int strict) {
    struct redisCommand *base_cmd = dictFetchValue(commands, argv[0]->ptr);
    int has_subcommands = base_cmd && base_cmd->subcommands_dict;
    if (argc == 1 || !has_subcommands) {
        if (strict && argc != 1)
            return NULL;
        /* Note: It is possible that base_cmd->proc==NULL (e.g. CONFIG) */
        /**注意：base_cmd->proc==NULL 是可能的（例如 CONFIG）*/
        return base_cmd;
    } else { /* argc > 1 && has_subcommands */
        if (strict && argc != 2)
            return NULL;
        /* Note: Currently we support just one level of subcommands */
        /**注意：目前我们只支持一级子命令*/
        return lookupSubcommand(base_cmd, argv[1]->ptr);
    }
}

struct redisCommand *lookupCommand(robj **argv, int argc) {
    return lookupCommandLogic(server.commands,argv,argc,0);
}

struct redisCommand *lookupCommandBySdsLogic(dict *commands, sds s) {
    int argc, j;
    sds *strings = sdssplitlen(s,sdslen(s),"|",1,&argc);
    if (strings == NULL)
        return NULL;
    if (argc > 2) {
        /* Currently we support just one level of subcommands */
        /**目前我们只支持一级子命令*/
        sdsfreesplitres(strings,argc);
        return NULL;
    }

    robj objects[argc];
    robj *argv[argc];
    for (j = 0; j < argc; j++) {
        initStaticStringObject(objects[j],strings[j]);
        argv[j] = &objects[j];
    }

    struct redisCommand *cmd = lookupCommandLogic(commands,argv,argc,1);
    sdsfreesplitres(strings,argc);
    return cmd;
}

struct redisCommand *lookupCommandBySds(sds s) {
    return lookupCommandBySdsLogic(server.commands,s);
}

struct redisCommand *lookupCommandByCStringLogic(dict *commands, const char *s) {
    struct redisCommand *cmd;
    sds name = sdsnew(s);

    cmd = lookupCommandBySdsLogic(commands,name);
    sdsfree(name);
    return cmd;
}

struct redisCommand *lookupCommandByCString(const char *s) {
    return lookupCommandByCStringLogic(server.commands,s);
}

/* Lookup the command in the current table, if not found also check in
 * the original table containing the original command names unaffected by
 * redis.conf rename-command statement.
 *
 * This is used by functions rewriting the argument vector such as
 * rewriteClientCommandVector() in order to set client->cmd pointer
 * correctly even if the command was renamed. */
/**在当前表中查找命令，如果未找到，还检查包含不受 redis.conf rename-command 语句影响的原始命令名称的原始表。
 * 这由重写参数向量的函数使用，例如 rewriteClientCommandVector() 以便正确设置 client->cmd 指针，即使命令被重命名。*/
struct redisCommand *lookupCommandOrOriginal(robj **argv ,int argc) {
    struct redisCommand *cmd = lookupCommandLogic(server.commands, argv, argc, 0);

    if (!cmd) cmd = lookupCommandLogic(server.orig_commands, argv, argc, 0);
    return cmd;
}

/* Commands arriving from the master client or AOF client, should never be rejected. */
/**来自主客户端或 AOF 客户端的命令永远不应被拒绝。*/
int mustObeyClient(client *c) {
    return c->id == CLIENT_ID_AOF || c->flags & CLIENT_MASTER;
}

static int shouldPropagate(int target) {
    if (!server.replication_allowed || target == PROPAGATE_NONE || server.loading)
        return 0;

    if (target & PROPAGATE_AOF) {
        if (server.aof_state != AOF_OFF)
            return 1;
    }
    if (target & PROPAGATE_REPL) {
        if (server.masterhost == NULL && (server.repl_backlog || listLength(server.slaves) != 0))
            return 1;
    }

    return 0;
}

/* Propagate the specified command (in the context of the specified database id)
 * to AOF and Slaves.
 *
 * flags are an xor between:
 * + PROPAGATE_NONE (no propagation of command at all)
 * + PROPAGATE_AOF (propagate into the AOF file if is enabled)
 * + PROPAGATE_REPL (propagate into the replication link)
 *
 * This is an internal low-level function and should not be called!
 *
 * The API for propagating commands is alsoPropagate().
 */
/**将指定的命令（在指定的数据库 id 的上下文中）传播到 AOF 和 Slaves。
 * flags 是以下之间的异或：
 *   + PROPAGATE_NONE（根本不传播命令）
 *   + PROPAGATE_AOF（如果启用，则传播到 AOF 文件中）
 *   + PROPAGATE_REPL（传播到复制链接）
 * 这是一个内部低级函数，不应调用！
 * 用于传播命令的 API 也是 Propagate()。*/
static void propagateNow(int dbid, robj **argv, int argc, int target) {
    if (!shouldPropagate(target))
        return;

    /* This needs to be unreachable since the dataset should be fixed during 
     * client pause, otherwise data may be lost during a failover. */
    /**这需要是不可访问的，因为数据集应该在客户端暂停期间修复，否则数据可能在故障转移期间丢失。*/
    serverAssert(!(areClientsPaused() && !server.client_pause_in_transaction));

    if (server.aof_state != AOF_OFF && target & PROPAGATE_AOF)
        feedAppendOnlyFile(dbid,argv,argc);
    if (target & PROPAGATE_REPL)
        replicationFeedSlaves(server.slaves,dbid,argv,argc);
}

/* Used inside commands to schedule the propagation of additional commands
 * after the current command is propagated to AOF / Replication.
 *
 * dbid is the database ID the command should be propagated into.
 * Arguments of the command to propagate are passed as an array of redis
 * objects pointers of len 'argc', using the 'argv' vector.
 *
 * The function does not take a reference to the passed 'argv' vector,
 * so it is up to the caller to release the passed argv (but it is usually
 * stack allocated).  The function automatically increments ref count of
 * passed objects, so the caller does not need to. */
/**在当前命令传播到 AOF 复制之后，在命令内部使用来安排其他命令的传播。
 * dbid 是命令应该传播到的数据库 ID。传播命令的参数作为 len 'argc' 的 redis 对象指针数组传递，使用 'argv' 向量。
 * 该函数不引用传递的'argv'向量，因此由调用者释放传递的argv（但它通常是堆栈分配的）。
 * 该函数自动增加传递对象的引用计数，因此调用者不需要。*/
void alsoPropagate(int dbid, robj **argv, int argc, int target) {
    robj **argvcopy;
    int j;

    if (!shouldPropagate(target))
        return;

    argvcopy = zmalloc(sizeof(robj*)*argc);
    for (j = 0; j < argc; j++) {
        argvcopy[j] = argv[j];
        incrRefCount(argv[j]);
    }
    redisOpArrayAppend(&server.also_propagate,dbid,argvcopy,argc,target);
}

/* It is possible to call the function forceCommandPropagation() inside a
 * Redis command implementation in order to to force the propagation of a
 * specific command execution into AOF / Replication. */
/**可以在 Redis 命令实现中调用函数 forceCommandPropagation() 以强制将特定命令执行传播到 AOF 复制中。*/
void forceCommandPropagation(client *c, int flags) {
    serverAssert(c->cmd->flags & (CMD_WRITE | CMD_MAY_REPLICATE));
    if (flags & PROPAGATE_REPL) c->flags |= CLIENT_FORCE_REPL;
    if (flags & PROPAGATE_AOF) c->flags |= CLIENT_FORCE_AOF;
}

/* Avoid that the executed command is propagated at all. This way we
 * are free to just propagate what we want using the alsoPropagate()
 * API. */
/**避免执行的命令被传播。这样我们就可以自由地使用alsoPropagate() API 传播我们想要的东西。*/
void preventCommandPropagation(client *c) {
    c->flags |= CLIENT_PREVENT_PROP;
}

/* AOF specific version of preventCommandPropagation(). */
void preventCommandAOF(client *c) {
    c->flags |= CLIENT_PREVENT_AOF_PROP;
}

/* Replication specific version of preventCommandPropagation(). */
void preventCommandReplication(client *c) {
    c->flags |= CLIENT_PREVENT_REPL_PROP;
}

/* Log the last command a client executed into the slowlog. */
/**将客户端执行的最后一个命令记录到慢日志中。*/
void slowlogPushCurrentCommand(client *c, struct redisCommand *cmd, ustime_t duration) {
    /* Some commands may contain sensitive data that should not be available in the slowlog. */
    /**某些命令可能包含不应在慢日志中可用的敏感数据。*/
    if (cmd->flags & CMD_SKIP_SLOWLOG)
        return;

    /* If command argument vector was rewritten, use the original
     * arguments. */
    /**如果命令参数向量被重写，请使用原始参数。*/
    robj **argv = c->original_argv ? c->original_argv : c->argv;
    int argc = c->original_argv ? c->original_argc : c->argc;
    slowlogPushEntryIfNeeded(c,argv,argc,duration);
}

/* This function is called in order to update the total command histogram duration.
 * The latency unit is nano-seconds.
 * If needed it will allocate the histogram memory and trim the duration to the upper/lower tracking limits*/
/**调用此函数是为了更新总命令直方图持续时间。延迟单位是纳秒。如果需要，它将分配直方图内存并将持续时间修剪到跟踪上限下限*/
void updateCommandLatencyHistogram(struct hdr_histogram **latency_histogram, int64_t duration_hist){
    if (duration_hist < LATENCY_HISTOGRAM_MIN_VALUE)
        duration_hist=LATENCY_HISTOGRAM_MIN_VALUE;
    if (duration_hist>LATENCY_HISTOGRAM_MAX_VALUE)
        duration_hist=LATENCY_HISTOGRAM_MAX_VALUE;
    if (*latency_histogram==NULL)
        hdr_init(LATENCY_HISTOGRAM_MIN_VALUE,LATENCY_HISTOGRAM_MAX_VALUE,LATENCY_HISTOGRAM_PRECISION,latency_histogram);
    hdr_record_value(*latency_histogram,duration_hist);
}

/* Handle the alsoPropagate() API to handle commands that want to propagate
 * multiple separated commands. Note that alsoPropagate() is not affected
 * by CLIENT_PREVENT_PROP flag. */
/**处理alsoPropagate() API 以处理想要传播多个分离命令的命令。
 * 请注意，alsoPropagate() 不受 CLIENT_PREVENT_PROP 标志的影响。*/
void propagatePendingCommands() {
    if (server.also_propagate.numops == 0)
        return;

    int j;
    redisOp *rop;
    int multi_emitted = 0;

    /* Wrap the commands in server.also_propagate array,
     * but don't wrap it if we are already in MULTI context,
     * in case the nested MULTI/EXEC.
     *
     * And if the array contains only one command, no need to
     * wrap it, since the single command is atomic. */
    /**将命令包装在 server.also_propagate 数组中，但如果我们已经在 MULTI 上下文中，则不要包装它，以防嵌套的 MULTIEXEC。
     * 如果数组只包含一个命令，则无需包装它，因为单个命令是原子的。*/
    if (server.also_propagate.numops > 1 && !server.propagate_no_multi) {
        /* We use the first command-to-propagate to set the dbid for MULTI,
         * so that the SELECT will be propagated beforehand */
        /**我们使用第一个命令传播来设置 MULTI 的 dbid，以便预先传播 SELECT*/
        int multi_dbid = server.also_propagate.ops[0].dbid;
        propagateNow(multi_dbid,&shared.multi,1,PROPAGATE_AOF|PROPAGATE_REPL);
        multi_emitted = 1;
    }

    for (j = 0; j < server.also_propagate.numops; j++) {
        rop = &server.also_propagate.ops[j];
        serverAssert(rop->target);
        propagateNow(rop->dbid,rop->argv,rop->argc,rop->target);
    }

    if (multi_emitted) {
        /* We take the dbid from last command so that propagateNow() won't inject another SELECT */
        /**我们从最后一个命令中获取 dbid，这样propagateNow() 就不会注入另一个 SELECT*/
        int exec_dbid = server.also_propagate.ops[server.also_propagate.numops-1].dbid;
        propagateNow(exec_dbid,&shared.exec,1,PROPAGATE_AOF|PROPAGATE_REPL);
    }

    redisOpArrayFree(&server.also_propagate);
}

/* Increment the command failure counters (either rejected_calls or failed_calls).
 * The decision which counter to increment is done using the flags argument, options are:
 * * ERROR_COMMAND_REJECTED - update rejected_calls
 * * ERROR_COMMAND_FAILED - update failed_calls
 *
 * The function also reset the prev_err_count to make sure we will not count the same error
 * twice, its possible to pass a NULL cmd value to indicate that the error was counted elsewhere.
 *
 * The function returns true if stats was updated and false if not. */
/**增加命令失败计数器（rejected_calls 或 failed_calls）。使用 flags 参数决定增加哪个计数器，选项有：
 *   ERROR_COMMAND_REJECTED - 更新拒绝调用
 *   ERROR_COMMAND_FAILED - 更新失败调用
 * 该函数还重置 prev_err_count 以确保我们不会两次计算相同的错误，
 * 它可以传递一个 NULL cmd 值表示该错误已在其他地方计算。
 * 如果统计信息已更新，则该函数返回 true，否则返回 false。*/
int incrCommandStatsOnError(struct redisCommand *cmd, int flags) {
    /* hold the prev error count captured on the last command execution */
    static long long prev_err_count = 0;
    int res = 0;
    if (cmd) {
        if ((server.stat_total_error_replies - prev_err_count) > 0) {
            if (flags & ERROR_COMMAND_REJECTED) {
                cmd->rejected_calls++;
                res = 1;
            } else if (flags & ERROR_COMMAND_FAILED) {
                cmd->failed_calls++;
                res = 1;
            }
        }
    }
    prev_err_count = server.stat_total_error_replies;
    return res;
}

/* Call() is the core of Redis execution of a command.
 *
 * The following flags can be passed:
 * CMD_CALL_NONE        No flags.
 * CMD_CALL_SLOWLOG     Check command speed and log in the slow log if needed.
 * CMD_CALL_STATS       Populate command stats.
 * CMD_CALL_PROPAGATE_AOF   Append command to AOF if it modified the dataset
 *                          or if the client flags are forcing propagation.
 * CMD_CALL_PROPAGATE_REPL  Send command to slaves if it modified the dataset
 *                          or if the client flags are forcing propagation.
 * CMD_CALL_PROPAGATE   Alias for PROPAGATE_AOF|PROPAGATE_REPL.
 * CMD_CALL_FULL        Alias for SLOWLOG|STATS|PROPAGATE.
 *
 * The exact propagation behavior depends on the client flags.
 * Specifically:
 *
 * 1. If the client flags CLIENT_FORCE_AOF or CLIENT_FORCE_REPL are set
 *    and assuming the corresponding CMD_CALL_PROPAGATE_AOF/REPL is set
 *    in the call flags, then the command is propagated even if the
 *    dataset was not affected by the command.
 * 2. If the client flags CLIENT_PREVENT_REPL_PROP or CLIENT_PREVENT_AOF_PROP
 *    are set, the propagation into AOF or to slaves is not performed even
 *    if the command modified the dataset.
 *
 * Note that regardless of the client flags, if CMD_CALL_PROPAGATE_AOF
 * or CMD_CALL_PROPAGATE_REPL are not set, then respectively AOF or
 * slaves propagation will never occur.
 *
 * Client flags are modified by the implementation of a given command
 * using the following API:
 *
 * forceCommandPropagation(client *c, int flags);
 * preventCommandPropagation(client *c);
 * preventCommandAOF(client *c);
 * preventCommandReplication(client *c);
 *
 */
/**call() 是 Redis 执行命令的核心。
 * 可以传递以下标志：
 *   CMD_CALL_NONE 无标志。
 *   CMD_CALL_SLOWLOG 检查命令速度并在需要时登录慢日志。
 *   CMD_CALL_STATS 填充命令统计信息。
 *   CMD_CALL_PROPAGATE_AOF 如果 AOF 修改了数据集或客户端标志强制传播，则将命令附加到 AOF。
 *   CMD_CALL_PROPAGATE_REPL 如果它修改了数据集或客户端标志强制传播，则向从属发送命令。
 *   CMD_CALL_PROPAGATE PROPAGATE_AOF|PROPAGATE_REPL 的别名。
 *   CMD_CALL_FULL SLOWLOG|STATS|PROPAGATE 的别名。
 * 确切的传播行为取决于客户端标志。具体来说：
 *   1. 如果设置了客户端标志 CLIENT_FORCE_AOF 或 CLIENT_FORCE_REPL 并假设在
 *      调用标志中设置了相应的 CMD_CALL_PROPAGATE_AOFREPL，那么即使数据集不受命令影响，也会传播命令。
 *   2. 如果设置了客户端标志 CLIENT_PREVENT_REPL_PROP 或 CLIENT_PREVENT_AOF_PROP，即使命令修改了数据集，
 *       也不会执行到 AOF 或从属设备的传播。
 * 请注意，无论客户端标志如何，如果 CMD_CALL_PROPAGATE_AOF 或 CMD_CALL_PROPAGATE_REPL 未设置，
 * 则分别不会发生 AOF 或从属传播。客户端标志通过使用以下 API 的给定命令的实现来修改：
 *   forceCommandPropagation(client c, int flags);
 *   preventCommandPropagation（client *c）;
 *   preventCommandAOF（client *c）;
 *   preventCommandReplication(client *c);*/
void call(client *c, int flags) {
    long long dirty;
    uint64_t client_old_flags = c->flags;
    struct redisCommand *real_cmd = c->realcmd;

    /* Initialization: clear the flags that must be set by the command on
     * demand, and initialize the array for additional commands propagation. */
    /**初始化：清除必须由命令按需设置的标志，并初始化数组以进行其他命令传播。*/
    c->flags &= ~(CLIENT_FORCE_AOF|CLIENT_FORCE_REPL|CLIENT_PREVENT_PROP);

    /* Redis core is in charge of propagation when the first entry point
     * of call() is processCommand().
     * The only other option to get to call() without having processCommand
     * as an entry point is if a module triggers RM_Call outside of call()
     * context (for example, in a timer).
     * In that case, the module is in charge of propagation.
     *
     * Because call() is re-entrant we have to cache and restore
     * server.core_propagates. */
    /**当 call() 的第一个入口点是 processCommand() 时，Redis 核心负责传播。
     * 在没有 processCommand 作为入口点的情况下到达 call() 的唯一其他选择是，
     * 如果模块在 call() 上下文之外（例如，在计时器中）触发 RM_Call。
     * 在这种情况下，模块负责传播。
     * 因为 call() 是可重入的，我们必须缓存和恢复 server.core_propagates。*/
    int prev_core_propagates = server.core_propagates;
    if (!server.core_propagates && !(flags & CMD_CALL_FROM_MODULE))
        server.core_propagates = 1;

    /* Call the command. */
    dirty = server.dirty;
    incrCommandStatsOnError(NULL, 0);

    const long long call_timer = ustime();

    /* Update cache time, in case we have nested calls we want to
     * update only on the first call*/
    /**更新缓存时间，如果我们有嵌套调用，我们只想在第一次调用时更新*/
    if (server.fixed_time_expire++ == 0) {
        updateCachedTimeWithUs(0,call_timer);
    }

    monotime monotonic_start = 0;
    if (monotonicGetType() == MONOTONIC_CLOCK_HW)
        monotonic_start = getMonotonicUs();

    server.in_nested_call++;
    c->cmd->proc(c);
    server.in_nested_call--;

    /* In order to avoid performance implication due to querying the clock using a system call 3 times,
     * we use a monotonic clock, when we are sure its cost is very low,
     * and fall back to non-monotonic call otherwise. */
    /**为了避免由于使用系统调用 3 次查询时钟而影响性能，我们使用单调时钟，
     * 当我们确定它的成本非常低时，否则回退到非单调调用。*/
    ustime_t duration;
    if (monotonicGetType() == MONOTONIC_CLOCK_HW)
        duration = getMonotonicUs() - monotonic_start;
    else
        duration = ustime() - call_timer;

    c->duration = duration;
    dirty = server.dirty-dirty;
    if (dirty < 0) dirty = 0;

    /* Update failed command calls if required. */
    /**如果需要，更新失败的命令调用。*/

    if (!incrCommandStatsOnError(real_cmd, ERROR_COMMAND_FAILED) && c->deferred_reply_errors) {
        /* When call is used from a module client, error stats, and total_error_replies
         * isn't updated since these errors, if handled by the module, are internal,
         * and not reflected to users. however, the commandstats does show these calls
         * (made by RM_Call), so it should log if they failed or succeeded. */
        /**当从模块客户端使用调用时，错误统计信息和 total_error_replies 不会更新，
         * 因为这些错误（如果由模块处理）是内部的，不会反映给用户。
         * 但是，commandstats 确实显示了这些调用（由 RM_Call 进行），因此它应该记录它们是否失败或成功。*/
        real_cmd->failed_calls++;
    }

    /* After executing command, we will close the client after writing entire
     * reply if it is set 'CLIENT_CLOSE_AFTER_COMMAND' flag. */
    /**执行命令后，如果设置了'CLIENT_CLOSE_AFTER_COMMAND'标志，我们将在写完整个回复后关闭客户端。*/
    if (c->flags & CLIENT_CLOSE_AFTER_COMMAND) {
        c->flags &= ~CLIENT_CLOSE_AFTER_COMMAND;
        c->flags |= CLIENT_CLOSE_AFTER_REPLY;
    }

    /* When EVAL is called loading the AOF we don't want commands called
     * from Lua to go into the slowlog or to populate statistics. */
    /**当调用 EVAL 加载 AOF 时，我们不希望从 Lua 调用的命令进入慢日志或填充统计信息。*/
    if (server.loading && c->flags & CLIENT_SCRIPT)
        flags &= ~(CMD_CALL_SLOWLOG | CMD_CALL_STATS);

    /* If the caller is Lua, we want to force the EVAL caller to propagate
     * the script if the command flag or client flag are forcing the
     * propagation. */
    /**如果调用者是 Lua，如果命令标志或客户端标志强制传播，我们希望强制 EVAL 调用者传播脚本。*/
    if (c->flags & CLIENT_SCRIPT && server.script_caller) {
        if (c->flags & CLIENT_FORCE_REPL)
            server.script_caller->flags |= CLIENT_FORCE_REPL;
        if (c->flags & CLIENT_FORCE_AOF)
            server.script_caller->flags |= CLIENT_FORCE_AOF;
    }

    /* Note: the code below uses the real command that was executed
     * c->cmd and c->lastcmd may be different, in case of MULTI-EXEC or
     * re-written commands such as EXPIRE, GEOADD, etc. */
    /**注意：下面的代码使用实际执行的命令 c->cmd 和 c->lastcmd 可能不同，
     * 以防 MULTI-EXEC 或重写命令，如 EXPIRE、GEOADD 等。*/

    /* Record the latency this command induced on the main thread.
     * unless instructed by the caller not to log. (happens when processing
     * a MULTI-EXEC from inside an AOF). */
    /**记录此命令在主线程上引起的延迟。除非来电者指示不要登录。 （在从 AOF 内部处理 MULTI-EXEC 时发生）。*/
    if (flags & CMD_CALL_SLOWLOG) {
        char *latency_event = (real_cmd->flags & CMD_FAST) ?
                               "fast-command" : "command";
        latencyAddSampleIfNeeded(latency_event,duration/1000);
    }

    /* Log the command into the Slow log if needed.
     * If the client is blocked we will handle slowlog when it is unblocked. */
    /**如果需要，将命令记录到慢速日志中。如果客户端被阻塞，我们将在它被解除阻塞时处理慢日志。*/
    if ((flags & CMD_CALL_SLOWLOG) && !(c->flags & CLIENT_BLOCKED))
        slowlogPushCurrentCommand(c, real_cmd, duration);

    /* Send the command to clients in MONITOR mode if applicable.
     * Administrative commands are considered too dangerous to be shown. */
    /**如果适用，以 MONITOR 模式将命令发送给客户端。管理命令被认为太危险而无法显示。*/
    if (!(c->cmd->flags & (CMD_SKIP_MONITOR|CMD_ADMIN))) {
        robj **argv = c->original_argv ? c->original_argv : c->argv;
        int argc = c->original_argv ? c->original_argc : c->argc;
        replicationFeedMonitors(c,server.monitors,c->db->id,argv,argc);
    }

    /* Clear the original argv.
     * If the client is blocked we will handle slowlog when it is unblocked. */
    /**清除原始 argv。如果客户端被阻塞，我们将在它被解除阻塞时处理慢日志。*/
    if (!(c->flags & CLIENT_BLOCKED))
        freeClientOriginalArgv(c);

    /* populate the per-command statistics that we show in INFO commandstats. */
    /**填充我们在 INFO commandstats 中显示的每个命令的统计信息。*/
    if (flags & CMD_CALL_STATS) {
        real_cmd->microseconds += duration;
        real_cmd->calls++;
        /* If the client is blocked we will handle latency stats when it is unblocked. */
        /**如果客户端被阻止，我们将在它被解除阻止时处理延迟统计信息。*/
        if (server.latency_tracking_enabled && !(c->flags & CLIENT_BLOCKED))
            updateCommandLatencyHistogram(&(real_cmd->latency_histogram), duration*1000);
    }

    /* Propagate the command into the AOF and replication link.
     * We never propagate EXEC explicitly, it will be implicitly
     * propagated if needed (see propagatePendingCommands).
     * Also, module commands take care of themselves */
    /**将命令传播到 AOF 和复制链接。我们从不显式传播 EXEC，如果需要，
     * 它将被隐式传播（请参阅propagatePendingCommands）。此外，模块命令会自行处理*/
    if (flags & CMD_CALL_PROPAGATE &&
        (c->flags & CLIENT_PREVENT_PROP) != CLIENT_PREVENT_PROP &&
        c->cmd->proc != execCommand &&
        !(c->cmd->flags & CMD_MODULE))
    {
        int propagate_flags = PROPAGATE_NONE;

        /* Check if the command operated changes in the data set. If so
         * set for replication / AOF propagation. */
        /**检查命令操作的数据集是否发生变化。如果这样设置为复制 AOF 传播。*/
        if (dirty) propagate_flags |= (PROPAGATE_AOF|PROPAGATE_REPL);

        /* If the client forced AOF / replication of the command, set
         * the flags regardless of the command effects on the data set. */
        /**如果客户端强制执行命令的 AOF 复制，请设置标志，而不考虑命令对数据集的影响。*/
        if (c->flags & CLIENT_FORCE_REPL) propagate_flags |= PROPAGATE_REPL;
        if (c->flags & CLIENT_FORCE_AOF) propagate_flags |= PROPAGATE_AOF;

        /* However prevent AOF / replication propagation if the command
         * implementation called preventCommandPropagation() or similar,
         * or if we don't have the call() flags to do so. */
        /**但是，如果命令实现调用了 preventCommandPropagation() 或类似的，
         * 或者我们没有 call() 标志来执行此操作，则阻止 AOF 复制传播。*/
        if (c->flags & CLIENT_PREVENT_REPL_PROP ||
            !(flags & CMD_CALL_PROPAGATE_REPL))
                propagate_flags &= ~PROPAGATE_REPL;
        if (c->flags & CLIENT_PREVENT_AOF_PROP ||
            !(flags & CMD_CALL_PROPAGATE_AOF))
                propagate_flags &= ~PROPAGATE_AOF;

        /* Call alsoPropagate() only if at least one of AOF / replication
         * propagation is needed. */
        /**仅当至少需要一个 AOF 复制传播时才调用alsoPropagate()。*/
        if (propagate_flags != PROPAGATE_NONE)
            alsoPropagate(c->db->id,c->argv,c->argc,propagate_flags);
    }

    /* Restore the old replication flags, since call() can be executed
     * recursively. */
    /**恢复旧的复制标志，因为 call() 可以递归执行。*/
    c->flags &= ~(CLIENT_FORCE_AOF|CLIENT_FORCE_REPL|CLIENT_PREVENT_PROP);
    c->flags |= client_old_flags &
        (CLIENT_FORCE_AOF|CLIENT_FORCE_REPL|CLIENT_PREVENT_PROP);

    /* If the client has keys tracking enabled for client side caching,
     * make sure to remember the keys it fetched via this command. Scripting
     * works a bit differently, where if the scripts executes any read command, it
     * remembers all of the declared keys from the script. */
    /**如果客户端为客户端缓存启用了密钥跟踪，请确保记住它通过此命令获取的密钥。
     * 脚本的工作方式略有不同，如果脚本执行任何读取命令，它会记住脚本中所有声明的键。*/
    if ((c->cmd->flags & CMD_READONLY) && (c->cmd->proc != evalRoCommand)
        && (c->cmd->proc != evalShaRoCommand) && (c->cmd->proc != fcallroCommand))
    {
        client *caller = (c->flags & CLIENT_SCRIPT && server.script_caller) ?
                            server.script_caller : c;
        if (caller->flags & CLIENT_TRACKING &&
            !(caller->flags & CLIENT_TRACKING_BCAST))
        {
            trackingRememberKeys(caller);
        }
    }

    server.fixed_time_expire--;
    server.stat_numcommands++;

    /* Record peak memory after each command and before the eviction that runs
     * before the next command. */
    /**在每个命令之后和在下一个命令之前运行的驱逐之前记录峰值内存。*/
    size_t zmalloc_used = zmalloc_used_memory();
    if (zmalloc_used > server.stat_peak_memory)
        server.stat_peak_memory = zmalloc_used;

    /* Do some maintenance job and cleanup */
    /**做一些维护工作和清理工作*/
    afterCommand(c);

    /* Client pause takes effect after a transaction has finished. This needs
     * to be located after everything is propagated. */
    /**客户端暂停在事务完成后生效。这需要在传播完所有内容之后定位。*/
    if (!server.in_exec && server.client_pause_in_transaction) {
        server.client_pause_in_transaction = 0;
    }

    server.core_propagates = prev_core_propagates;
}

/* Used when a command that is ready for execution needs to be rejected, due to
 * various pre-execution checks. it returns the appropriate error to the client.
 * If there's a transaction is flags it as dirty, and if the command is EXEC,
 * it aborts the transaction.
 * Note: 'reply' is expected to end with \r\n */
/**当由于各种执行前检查而需要拒绝准备执行的命令时使用。它将适当的错误返回给客户端。
 * 如果有事务将其标记为脏，并且如果命令为 EXEC，则中止事务。注意：“回复”应以 \r\n 结尾*/
void rejectCommand(client *c, robj *reply) {
    flagTransaction(c);
    if (c->cmd) c->cmd->rejected_calls++;
    if (c->cmd && c->cmd->proc == execCommand) {
        execCommandAbort(c, reply->ptr);
    } else {
        /* using addReplyError* rather than addReply so that the error can be logged. */
        /**使用 addReplyError 而不是 addReply 以便记录错误。*/
        addReplyErrorObject(c, reply);
    }
}

void rejectCommandSds(client *c, sds s) {
    flagTransaction(c);
    if (c->cmd) c->cmd->rejected_calls++;
    if (c->cmd && c->cmd->proc == execCommand) {
        execCommandAbort(c, s);
        sdsfree(s);
    } else {
        /* The following frees 's'.   以下释放's'。*/
        addReplyErrorSds(c, s);
    }
}

void rejectCommandFormat(client *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap,fmt);
    sds s = sdscatvprintf(sdsempty(),fmt,ap);
    va_end(ap);
    /* Make sure there are no newlines in the string, otherwise invalid protocol
     * is emitted (The args come from the user, they may contain any character). */
    /**确保字符串中没有换行符，否则会发出无效的协议（参数来自用户，它们可能包含任何字符）。*/
    sdsmapchars(s, "\r\n", "  ",  2);
    rejectCommandSds(c, s);
}

/* This is called after a command in call, we can do some maintenance job in it. */
/**这是在调用命令之后调用的，我们可以在其中做一些维护工作。*/
void afterCommand(client *c) {
    UNUSED(c);
    if (!server.in_nested_call) {
        /* If we are at the top-most call() we can propagate what we accumulated.
         * Should be done before trackingHandlePendingKeyInvalidations so that we
         * reply to client before invalidating cache (makes more sense) */
        /**如果我们在最顶层 call() 我们可以传播我们积累的东西。
         * 应该在 trackingHandlePendingKeyInvalidations 之前完成，以便我们在缓存无效之前回复客户端（更有意义）*/
        if (server.core_propagates)
            propagatePendingCommands();
        /* Flush pending invalidation messages only when we are not in nested call.
         * So the messages are not interleaved with transaction response. */
        /**仅当我们不在嵌套调用中时才刷新待处理的无效消息。因此消息不会与事务响应交错。*/
        trackingHandlePendingKeyInvalidations();
    }
}

/* Check if c->cmd exists, fills `err` with details in case it doesn't.
 * Return 1 if exists. */
/**检查 c->cmd 是否存在，如果不存在，则在 `err` 中填写详细信息。如果存在则返回 1。*/
int commandCheckExistence(client *c, sds *err) {
    if (c->cmd)
        return 1;
    if (!err)
        return 0;
    if (isContainerCommandBySds(c->argv[0]->ptr)) {
        /* If we can't find the command but argv[0] by itself is a command
         * it means we're dealing with an invalid subcommand. Print Help. */
        /**如果我们找不到命令但 argv[0] 本身就是一个命令，这意味着我们正在处理一个无效的子命令。打印帮助。*/
        sds cmd = sdsnew((char *)c->argv[0]->ptr);
        sdstoupper(cmd);
        *err = sdsnew(NULL);
        *err = sdscatprintf(*err, "unknown subcommand '%.128s'. Try %s HELP.",
                            (char *)c->argv[1]->ptr, cmd);
        sdsfree(cmd);
    } else {
        sds args = sdsempty();
        int i;
        for (i=1; i < c->argc && sdslen(args) < 128; i++)
            args = sdscatprintf(args, "'%.*s' ", 128-(int)sdslen(args), (char*)c->argv[i]->ptr);
        *err = sdsnew(NULL);
        *err = sdscatprintf(*err, "unknown command '%.128s', with args beginning with: %s",
                            (char*)c->argv[0]->ptr, args);
        sdsfree(args);
    }
    /* Make sure there are no newlines in the string, otherwise invalid protocol
     * is emitted (The args come from the user, they may contain any character). */
    /**确保字符串中没有换行符，否则会发出无效的协议（参数来自用户，它们可能包含任何字符）。*/
    sdsmapchars(*err, "\r\n", "  ",  2);
    return 0;
}

/* Check if c->argc is valid for c->cmd, fills `err` with details in case it isn't.
 * Return 1 if valid. */
/**检查 c->argc 是否对 c->cmd 有效，如果不是，则在 `err` 中填写详细信息。如果有效则返回 1。*/
int commandCheckArity(client *c, sds *err) {
    if ((c->cmd->arity > 0 && c->cmd->arity != c->argc) ||
        (c->argc < -c->cmd->arity))
    {
        if (err) {
            *err = sdsnew(NULL);
            *err = sdscatprintf(*err, "wrong number of arguments for '%s' command", c->cmd->fullname);
        }
        return 0;
    }

    return 1;
}

/* If this function gets called we already read a whole
 * command, arguments are in the client argv/argc fields.
 * processCommand() execute the command or prepare the
 * server for a bulk read from the client.
 *
 * If C_OK is returned the client is still alive and valid and
 * other operations can be performed by the caller. Otherwise
 * if C_ERR is returned the client was destroyed (i.e. after QUIT). */
/**如果调用此函数，我们已经读取了整个命令，参数位于客户端 argvargc 字段中。
 *
 * @return processCommand() 执行命令或准备服务器以从客户端进行批量读取。
 * 如果返回 C_OK，则客户端仍然活着且有效，调用者可以执行其他操作。
 * 否则，如果返回 C_ERR，则客户端被销毁（即在 QUIT 之后）。*/
int processCommand(client *c) {
    if (!scriptIsTimedout()) {
        /* Both EXEC and scripts call call() directly so there should be
         * no way in_exec or scriptIsRunning() is 1.
         * That is unless lua_timedout, in which case client may run
         * some commands. */
        /**EXEC 和脚本都直接调用 call()，所以 in_exec 或 scriptIsRunning() 不可能是 1。
         * 除非 lua_timedout，在这种情况下客户端可能会运行一些命令。*/
        serverAssert(!server.in_exec);
        serverAssert(!scriptIsRunning());
    }

    moduleCallCommandFilters(c);

    /* Handle possible security attacks. */
    /**处理可能的安全攻击。*/
    if (!strcasecmp(c->argv[0]->ptr,"host:") || !strcasecmp(c->argv[0]->ptr,"post")) {
        securityWarningCommand(c);
        return C_ERR;
    }

    /* If we're inside a module blocked context yielding that wants to avoid
     * processing clients, postpone the command. */
    /**如果我们在一个模块阻塞的上下文中，想要避免处理客户端，请推迟命令。*/
    if (server.busy_module_yield_flags != BUSY_MODULE_YIELD_NONE &&
        !(server.busy_module_yield_flags & BUSY_MODULE_YIELD_CLIENTS))
    {
        c->bpop.timeout = 0;
        blockClient(c,BLOCKED_POSTPONE);
        return C_OK;
    }

    /* Now lookup the command and check ASAP about trivial error conditions
     * such as wrong arity, bad command name and so forth. */
    /**现在查找命令并尽快检查琐碎的错误情况，例如错误的数量、错误的命令名称等。*/
    c->cmd = c->lastcmd = c->realcmd = lookupCommand(c->argv,c->argc);
    sds err;
    if (!commandCheckExistence(c, &err)) {
        rejectCommandSds(c, err);
        return C_OK;
    }
    if (!commandCheckArity(c, &err)) {
        rejectCommandSds(c, err);
        return C_OK;
    }

    /* Check if the command is marked as protected and the relevant configuration allows it */
    /**检查命令是否被标记为受保护并且相关配置是否允许*/
    if (c->cmd->flags & CMD_PROTECTED) {
        if ((c->cmd->proc == debugCommand && !allowProtectedAction(server.enable_debug_cmd, c)) ||
            (c->cmd->proc == moduleCommand && !allowProtectedAction(server.enable_module_cmd, c)))
        {
            rejectCommandFormat(c,"%s command not allowed. If the %s option is set to \"local\", "
                                  "you can run it from a local connection, otherwise you need to set this option "
                                  "in the configuration file, and then restart the server.",
                                  c->cmd->proc == debugCommand ? "DEBUG" : "MODULE",
                                  c->cmd->proc == debugCommand ? "enable-debug-command" : "enable-module-command");
            return C_OK;

        }
    }

    /* If we're executing a script, try to extract a set of command flags from
     * it, in case it declared them. Note this is just an attempt, we don't yet
     * know the script command is well formed.*/
    /**如果我们正在执行一个脚本，请尝试从中提取一组命令标志，以防它声明它们。
     * 请注意，这只是一种尝试，我们还不知道脚本命令的格式是否正确。*/
    uint64_t cmd_flags = c->cmd->flags;
    if (c->cmd->proc == evalCommand || c->cmd->proc == evalShaCommand ||
        c->cmd->proc == evalRoCommand || c->cmd->proc == evalShaRoCommand ||
        c->cmd->proc == fcallCommand || c->cmd->proc == fcallroCommand)
    {
        if (c->cmd->proc == fcallCommand || c->cmd->proc == fcallroCommand)
            cmd_flags = fcallGetCommandFlags(c, cmd_flags);
        else
            cmd_flags = evalGetCommandFlags(c, cmd_flags);
    }

    int is_read_command = (cmd_flags & CMD_READONLY) ||
                           (c->cmd->proc == execCommand && (c->mstate.cmd_flags & CMD_READONLY));
    int is_write_command = (cmd_flags & CMD_WRITE) ||
                           (c->cmd->proc == execCommand && (c->mstate.cmd_flags & CMD_WRITE));
    int is_denyoom_command = (cmd_flags & CMD_DENYOOM) ||
                             (c->cmd->proc == execCommand && (c->mstate.cmd_flags & CMD_DENYOOM));
    int is_denystale_command = !(cmd_flags & CMD_STALE) ||
                               (c->cmd->proc == execCommand && (c->mstate.cmd_inv_flags & CMD_STALE));
    int is_denyloading_command = !(cmd_flags & CMD_LOADING) ||
                                 (c->cmd->proc == execCommand && (c->mstate.cmd_inv_flags & CMD_LOADING));
    int is_may_replicate_command = (cmd_flags & (CMD_WRITE | CMD_MAY_REPLICATE)) ||
                                   (c->cmd->proc == execCommand && (c->mstate.cmd_flags & (CMD_WRITE | CMD_MAY_REPLICATE)));
    int is_deny_async_loading_command = (cmd_flags & CMD_NO_ASYNC_LOADING) ||
                                        (c->cmd->proc == execCommand && (c->mstate.cmd_flags & CMD_NO_ASYNC_LOADING));
    int obey_client = mustObeyClient(c);

    if (authRequired(c)) {
        /* AUTH and HELLO and no auth commands are valid even in
         * non-authenticated state. */
        /**AUTH 和 HELLO 以及 no auth 命令即使在非身份验证状态下也是有效的。*/
        if (!(c->cmd->flags & CMD_NO_AUTH)) {
            rejectCommand(c,shared.noautherr);
            return C_OK;
        }
    }

    if (c->flags & CLIENT_MULTI && c->cmd->flags & CMD_NO_MULTI) {
        rejectCommandFormat(c,"Command not allowed inside a transaction");
        return C_OK;
    }

    /* Check if the user can run this command according to the current
     * ACLs. */
    /**检查用户是否可以根据当前的 ACL 运行该命令。*/
    int acl_errpos;
    int acl_retval = ACLCheckAllPerm(c,&acl_errpos);
    if (acl_retval != ACL_OK) {
        addACLLogEntry(c,acl_retval,(c->flags & CLIENT_MULTI) ? ACL_LOG_CTX_MULTI : ACL_LOG_CTX_TOPLEVEL,acl_errpos,NULL,NULL);
        switch (acl_retval) {
        case ACL_DENIED_CMD:
        {
            rejectCommandFormat(c,
                "-NOPERM this user has no permissions to run "
                "the '%s' command", c->cmd->fullname);
            break;
        }
        case ACL_DENIED_KEY:
            rejectCommandFormat(c,
                "-NOPERM this user has no permissions to access "
                "one of the keys used as arguments");
            break;
        case ACL_DENIED_CHANNEL:
            rejectCommandFormat(c,
                "-NOPERM this user has no permissions to access "
                "one of the channels used as arguments");
            break;
        default:
            rejectCommandFormat(c, "no permission");
            break;
        }
        return C_OK;
    }

    /* If cluster is enabled perform the cluster redirection here.
     * However we don't perform the redirection if:
     * 1) The sender of this command is our master.
     * 2) The command has no key arguments. */
    /**如果启用了集群，请在此处执行集群重定向。
     * 但是，如果出现以下情况，我们不会执行重定向：
     *   1) 此命令的发送者是我们的主人。
     *   2) 该命令没有关键参数。*/
    if (server.cluster_enabled &&
        !mustObeyClient(c) &&
        !(!(c->cmd->flags&CMD_MOVABLE_KEYS) && c->cmd->key_specs_num == 0 &&
          c->cmd->proc != execCommand))
    {
        int error_code;
        clusterNode *n = getNodeByQuery(c,c->cmd,c->argv,c->argc,
                                        &c->slot,&error_code);
        if (n == NULL || n != server.cluster->myself) {
            if (c->cmd->proc == execCommand) {
                discardTransaction(c);
            } else {
                flagTransaction(c);
            }
            clusterRedirectClient(c,n,c->slot,error_code);
            c->cmd->rejected_calls++;
            return C_OK;
        }
    }

    /* Disconnect some clients if total clients memory is too high. We do this
     * before key eviction, after the last command was executed and consumed
     * some client output buffer memory. */
    /**如果总客户端内存太高，请断开一些客户端。我们在键逐出之前执行此操作，
     * 在执行最后一个命令并消耗一些客户端输出缓冲内存之后。*/
    evictClients();
    if (server.current_client == NULL) {
        /* If we evicted ourself then abort processing the command */
        /**如果我们驱逐自己然后中止处理命令*/
        return C_ERR;
    }

    /* Handle the maxmemory directive.
     *
     * Note that we do not want to reclaim memory if we are here re-entering
     * the event loop since there is a busy Lua script running in timeout
     * condition, to avoid mixing the propagation of scripts with the
     * propagation of DELs due to eviction. */
    /**处理 maxmemory 指令。
     * 请注意，如果我们在这里重新进入事件循环，我们不想回收内存，
     * 因为有一个繁忙的 Lua 脚本在超时条件下运行，以避免由于驱逐而将脚本的传播与 DEL 的传播混为一谈。*/
    if (server.maxmemory && !isInsideYieldingLongCommand()) {
        int out_of_memory = (performEvictions() == EVICT_FAIL);

        /* performEvictions may evict keys, so we need flush pending tracking
         * invalidation keys. If we don't do this, we may get an invalidation
         * message after we perform operation on the key, where in fact this
         * message belongs to the old value of the key before it gets evicted.*/
        /**performEvictions 可能会驱逐键，因此我们需要刷新挂起的跟踪失效键。
         * 如果我们不这样做，我们可能会在对 key 执行操作后收到一条无效消息，
         * 实际上该消息属于 key 在被驱逐之前的旧值。*/
        trackingHandlePendingKeyInvalidations();

        /* performEvictions may flush slave output buffers. This may result
         * in a slave, that may be the active client, to be freed. */
        /**performEvictions 可能会刷新从属输出缓冲区。这可能会导致从机（可能是活动客户端）被释放。*/
        if (server.current_client == NULL) return C_ERR;

        int reject_cmd_on_oom = is_denyoom_command;
        /* If client is in MULTI/EXEC context, queuing may consume an unlimited
         * amount of memory, so we want to stop that.
         * However, we never want to reject DISCARD, or even EXEC (unless it
         * contains denied commands, in which case is_denyoom_command is already
         * set. */
        /**如果客户端在 MULTIEXEC 上下文中，排队可能会消耗无限量的内存，所以我们想停止它。
         * 但是，我们永远不想拒绝 DISCARD，甚至 EXEC（除非它包含被拒绝的命令，
         * 在这种情况下 is_denyoom_command 已经设置。*/
        if (c->flags & CLIENT_MULTI &&
            c->cmd->proc != execCommand &&
            c->cmd->proc != discardCommand &&
            c->cmd->proc != quitCommand &&
            c->cmd->proc != resetCommand) {
            reject_cmd_on_oom = 1;
        }

        if (out_of_memory && reject_cmd_on_oom) {
            rejectCommand(c, shared.oomerr);
            return C_OK;
        }

        /* Save out_of_memory result at command start, otherwise if we check OOM
         * in the first write within script, memory used by lua stack and
         * arguments might interfere. We need to save it for EXEC and module
         * calls too, since these can call EVAL, but avoid saving it during an
         * interrupted / yielding busy script / module. */
        /**在命令开始时保存 out_of_memory 结果，否则如果我们在第一次写入脚本时
         * 检查 OOM，lua 堆栈和参数使用的内存可能会干扰。我们也需要为 EXEC 和模块调用保存它，
         * 因为它们可以调用 EVAL，但要避免在中断产生繁忙的脚本模块期间保存它。*/
        server.pre_command_oom_state = out_of_memory;
    }

    /* Make sure to use a reasonable amount of memory for client side
     * caching metadata. */
    /**确保为客户端缓存元数据使用合理数量的内存。*/
    if (server.tracking_clients) trackingLimitUsedSlots();

    /* Don't accept write commands if there are problems persisting on disk
     * unless coming from our master, in which case check the replica ignore
     * disk write error config to either log or crash. */
    /**如果磁盘上存在问题，除非来自我们的主服务器，否则不要接受写入命令，
     * 在这种情况下，请检查副本忽略磁盘写入错误配置以记录或崩溃。*/
    int deny_write_type = writeCommandsDeniedByDiskError();
    if (deny_write_type != DISK_ERROR_TYPE_NONE &&
        (is_write_command || c->cmd->proc == pingCommand))
    {
        if (obey_client) {
            if (!server.repl_ignore_disk_write_error && c->cmd->proc != pingCommand) {
                serverPanic("Replica was unable to write command to disk.");
            } else {
                static mstime_t last_log_time_ms = 0;
                const mstime_t log_interval_ms = 10000;
                if (server.mstime > last_log_time_ms + log_interval_ms) {
                    last_log_time_ms = server.mstime;
                    serverLog(LL_WARNING, "Replica is applying a command even though "
                                          "it is unable to write to disk.");
                }
            }
        } else {
            sds err = writeCommandsGetDiskErrorMessage(deny_write_type);
            /* remove the newline since rejectCommandSds adds it. */
            /**删除换行符，因为 rejectCommandSds 添加了它。*/
            sdssubstr(err, 0, sdslen(err)-2);
            rejectCommandSds(c, err);
            return C_OK;
        }
    }

    /* Don't accept write commands if there are not enough good slaves and
     * user configured the min-slaves-to-write option. */
    /**如果没有足够好的从站并且用户配置了 min-slaves-to-write 选项，则不要接受写入命令。*/
    if (is_write_command && !checkGoodReplicasStatus()) {
        rejectCommand(c, shared.noreplicaserr);
        return C_OK;
    }

    /* Don't accept write commands if this is a read only slave. But
     * accept write commands if this is our master. */
    /**如果这是一个只读从站，则不要接受写命令。但如果这是我们的主人，请接受写命令。*/
    if (server.masterhost && server.repl_slave_ro &&
        !obey_client &&
        is_write_command)
    {
        rejectCommand(c, shared.roslaveerr);
        return C_OK;
    }

    /* Only allow a subset of commands in the context of Pub/Sub if the
     * connection is in RESP2 mode. With RESP3 there are no limits. */
    /**如果连接处于 RESP2 模式，则仅允许 PubSub 上下文中的命令子集。 RESP3 没有限制。*/
    if ((c->flags & CLIENT_PUBSUB && c->resp == 2) &&
        c->cmd->proc != pingCommand &&
        c->cmd->proc != subscribeCommand &&
        c->cmd->proc != ssubscribeCommand &&
        c->cmd->proc != unsubscribeCommand &&
        c->cmd->proc != sunsubscribeCommand &&
        c->cmd->proc != psubscribeCommand &&
        c->cmd->proc != punsubscribeCommand &&
        c->cmd->proc != quitCommand &&
        c->cmd->proc != resetCommand) {
        rejectCommandFormat(c,
            "Can't execute '%s': only (P|S)SUBSCRIBE / "
            "(P|S)UNSUBSCRIBE / PING / QUIT / RESET are allowed in this context",
            c->cmd->fullname);
        return C_OK;
    }

    /* Only allow commands with flag "t", such as INFO, REPLICAOF and so on,
     * when replica-serve-stale-data is no and we are a replica with a broken
     * link with master. */
    /**仅当replica-serve-stale-data 为no 且我们是与master 的链接断开的副本时，
     * 才允许带有标志“t”的命令，例如INFO、REPLICAOF 等。*/
    if (server.masterhost && server.repl_state != REPL_STATE_CONNECTED &&
        server.repl_serve_stale_data == 0 &&
        is_denystale_command)
    {
        rejectCommand(c, shared.masterdownerr);
        return C_OK;
    }

    /* Loading DB? Return an error if the command has not the
     * CMD_LOADING flag. */
    /**加载数据库？如果命令没有 CMD_LOADING 标志，则返回错误。*/
    if (server.loading && !server.async_loading && is_denyloading_command) {
        rejectCommand(c, shared.loadingerr);
        return C_OK;
    }

    /* During async-loading, block certain commands. */
    /**在异步加载期间，阻止某些命令。*/
    if (server.async_loading && is_deny_async_loading_command) {
        rejectCommand(c,shared.loadingerr);
        return C_OK;
    }

    /* when a busy job is being done (script / module)
     * Only allow a limited number of commands.
     * Note that we need to allow the transactions commands, otherwise clients
     * sending a transaction with pipelining without error checking, may have
     * the MULTI plus a few initial commands refused, then the timeout
     * condition resolves, and the bottom-half of the transaction gets
     * executed, see Github PR #7022. */
    /**当繁忙的工作正在完成时（脚本模块）只允许有限数量的命令。
     * 请注意，我们需要允许事务命令，否则客户端发送带有流水线的事务而没有错误检查，
     * 可能会拒绝 MULTI 加上一些初始命令，然后超时条件解决，事务的下半部分被执行，请参阅Github PR 7022。*/
    if (isInsideYieldingLongCommand() && !(c->cmd->flags & CMD_ALLOW_BUSY)) {
        if (server.busy_module_yield_flags && server.busy_module_yield_reply) {
            rejectCommandFormat(c, "-BUSY %s", server.busy_module_yield_reply);
        } else if (server.busy_module_yield_flags) {
            rejectCommand(c, shared.slowmoduleerr);
        } else if (scriptIsEval()) {
            rejectCommand(c, shared.slowevalerr);
        } else {
            rejectCommand(c, shared.slowscripterr);
        }
        return C_OK;
    }

    /* Prevent a replica from sending commands that access the keyspace.
     * The main objective here is to prevent abuse of client pause check
     * from which replicas are exempt. */
    /**防止副本发送访问键空间的命令。这里的主要目标是防止滥用客户端暂停检查，副本可以免除。*/
    if ((c->flags & CLIENT_SLAVE) && (is_may_replicate_command || is_write_command || is_read_command)) {
        rejectCommandFormat(c, "Replica can't interact with the keyspace");
        return C_OK;
    }

    /* If the server is paused, block the client until
     * the pause has ended. Replicas are never paused. */
    /**如果服务器暂停，则阻止客户端，直到暂停结束。副本永远不会暂停。*/
    if (!(c->flags & CLIENT_SLAVE) && 
        ((server.client_pause_type == CLIENT_PAUSE_ALL) ||
        (server.client_pause_type == CLIENT_PAUSE_WRITE && is_may_replicate_command)))
    {
        c->bpop.timeout = 0;
        blockClient(c,BLOCKED_POSTPONE);
        return C_OK;       
    }

    /* Exec the command */
    if (c->flags & CLIENT_MULTI &&
        c->cmd->proc != execCommand &&
        c->cmd->proc != discardCommand &&
        c->cmd->proc != multiCommand &&
        c->cmd->proc != watchCommand &&
        c->cmd->proc != quitCommand &&
        c->cmd->proc != resetCommand)
    {
        queueMultiCommand(c, cmd_flags);
        addReply(c,shared.queued);
    } else {
        call(c,CMD_CALL_FULL);
        c->woff = server.master_repl_offset;
        if (listLength(server.ready_keys))
            handleClientsBlockedOnKeys();
    }

    return C_OK;
}

/* ====================== Error lookup and execution 错误查找和执行===================== */

void incrementErrorCount(const char *fullerr, size_t namelen) {
    struct redisError *error = raxFind(server.errors,(unsigned char*)fullerr,namelen);
    if (error == raxNotFound) {
        error = zmalloc(sizeof(*error));
        error->count = 0;
        raxInsert(server.errors,(unsigned char*)fullerr,namelen,error,NULL);
    }
    error->count++;
}

/*================================== Shutdown =============================== */

/* Close listening sockets. Also unlink the unix domain socket if
 * unlink_unix_socket is non-zero. */
/**关闭监听套接字。如果 unlink_unix_socket 不为零，也取消链接 unix 域套接字。*/
void closeListeningSockets(int unlink_unix_socket) {
    int j;

    for (j = 0; j < server.ipfd.count; j++) close(server.ipfd.fd[j]);
    for (j = 0; j < server.tlsfd.count; j++) close(server.tlsfd.fd[j]);
    if (server.sofd != -1) close(server.sofd);
    if (server.cluster_enabled)
        for (j = 0; j < server.cfd.count; j++) close(server.cfd.fd[j]);
    if (unlink_unix_socket && server.unixsocket) {
        serverLog(LL_NOTICE,"Removing the unix socket file.");
        if (unlink(server.unixsocket) != 0)
            serverLog(LL_WARNING,"Error removing the unix socket file: %s",strerror(errno));
    }
}

/* Prepare for shutting down the server. Flags:
 *
 * - SHUTDOWN_SAVE: Save a database dump even if the server is configured not to
 *   save any dump.
 *
 * - SHUTDOWN_NOSAVE: Don't save any database dump even if the server is
 *   configured to save one.
 *
 * - SHUTDOWN_NOW: Don't wait for replicas to catch up before shutting down.
 *
 * - SHUTDOWN_FORCE: Ignore errors writing AOF and RDB files on disk, which
 *   would normally prevent a shutdown.
 *
 * Unless SHUTDOWN_NOW is set and if any replicas are lagging behind, C_ERR is
 * returned and server.shutdown_mstime is set to a timestamp to allow a grace
 * period for the replicas to catch up. This is checked and handled by
 * serverCron() which completes the shutdown as soon as possible.
 *
 * If shutting down fails due to errors writing RDB or AOF files, C_ERR is
 * returned and an error is logged. If the flag SHUTDOWN_FORCE is set, these
 * errors are logged but ignored and C_OK is returned.
 *
 * On success, this function returns C_OK and then it's OK to call exit(0). */
/**准备关闭服务器。标志：
 *   - SHUTDOWN_SAVE：保存数据库转储，即使服务器配置为不保存任何转储。
 *   - SHUTDOWN_NOSAVE：即使服务器配置为保存一个，也不保存任何数据库转储。
 *   - SHUTDOWN_NOW：在关闭之前不要等待副本赶上。
 *   - SHUTDOWN_FORCE：忽略在磁盘上写入 AOF 和 RDB 文件的错误，这通常会阻止关机。
 * 除非设置了 SHUTDOWN_NOW 并且如果任何副本落后，则返回 C_ERR 并将 server.shutdown_mstime 设置为
 * 时间戳以允许副本赶上的宽限期。这由 serverCron() 检查和处理，它会尽快完成关闭。
 * 如果由于写入 RDB 或 AOF 文件的错误而导致关闭失败，则返回 C_ERR 并记录错误。
 * 如果设置了标志 SHUTDOWN_FORCE，这些错误将被记录但被忽略并返回 C_OK。
 * 成功时，此函数返回 C_OK，然后调用 exit(0) 即可。*/
int prepareForShutdown(int flags) {
    if (isShutdownInitiated()) return C_ERR;

    /* When SHUTDOWN is called while the server is loading a dataset in
     * memory we need to make sure no attempt is performed to save
     * the dataset on shutdown (otherwise it could overwrite the current DB
     * with half-read data).
     *
     * Also when in Sentinel mode clear the SAVE flag and force NOSAVE. */
    /**当服务器在内存中加载数据集时调用 SHUTDOWN 时，我们需要确保在关闭时不会尝试
     * 保存数据集（否则它可能会用半读数据覆盖当前数据库）。
     * 此外，在 Sentinel 模式下，清除 SAVE 标志并强制 NOSAVE。*/
    if (server.loading || server.sentinel_mode)
        flags = (flags & ~SHUTDOWN_SAVE) | SHUTDOWN_NOSAVE;

    server.shutdown_flags = flags;

    serverLog(LL_WARNING,"User requested shutdown...");
    if (server.supervised_mode == SUPERVISED_SYSTEMD)
        redisCommunicateSystemd("STOPPING=1\n");

    /* If we have any replicas, let them catch up the replication offset before
     * we shut down, to avoid data loss. */
    /**如果我们有任何副本，在我们关闭之前让它们赶上复制偏移量，以避免数据丢失。*/
    if (!(flags & SHUTDOWN_NOW) &&
        server.shutdown_timeout != 0 &&
        !isReadyToShutdown())
    {
        server.shutdown_mstime = server.mstime + server.shutdown_timeout * 1000;
        if (!areClientsPaused()) sendGetackToReplicas();
        pauseClients(PAUSE_DURING_SHUTDOWN, LLONG_MAX, CLIENT_PAUSE_WRITE);
        serverLog(LL_NOTICE, "Waiting for replicas before shutting down.");
        return C_ERR;
    }

    return finishShutdown();
}

static inline int isShutdownInitiated(void) {
    return server.shutdown_mstime != 0;
}

/* Returns 0 if there are any replicas which are lagging in replication which we
 * need to wait for before shutting down. Returns 1 if we're ready to shut
 * down now. */
/**如果有任何副本滞后于我们需要在关闭之前等待的复制，则返回 0。如果我们现在准备关闭，则返回 1。*/
int isReadyToShutdown(void) {
    if (listLength(server.slaves) == 0) return 1;  /* No replicas. */

    listIter li;
    listNode *ln;
    listRewind(server.slaves, &li);
    while ((ln = listNext(&li)) != NULL) {
        client *replica = listNodeValue(ln);
        if (replica->repl_ack_off != server.master_repl_offset) return 0;
    }
    return 1;
}

static void cancelShutdown(void) {
    server.shutdown_asap = 0;
    server.shutdown_flags = 0;
    server.shutdown_mstime = 0;
    server.last_sig_received = 0;
    replyToClientsBlockedOnShutdown();
    unpauseClients(PAUSE_DURING_SHUTDOWN);
}

/* Returns C_OK if shutdown was aborted and C_ERR if shutdown wasn't ongoing. */
/**如果关闭已中止，则返回 C_OK；如果关闭未进行，则返回 C_ERR。*/
int abortShutdown(void) {
    if (isShutdownInitiated()) {
        cancelShutdown();
    } else if (server.shutdown_asap) {
        /* Signal handler has requested shutdown, but it hasn't been initiated
         * yet. Just clear the flag. */
        /**信号处理程序已请求关闭，但尚未启动。只需清除标志。*/
        server.shutdown_asap = 0;
    } else {
        /* Shutdown neither initiated nor requested. */
        /**既未启动也未请求关闭。*/
        return C_ERR;
    }
    serverLog(LL_NOTICE, "Shutdown manually aborted.");
    return C_OK;
}

/* The final step of the shutdown sequence. Returns C_OK if the shutdown
 * sequence was successful and it's OK to call exit(). If C_ERR is returned,
 * it's not safe to call exit(). */
/**关机序列的最后一步。如果关闭序列成功并且可以调用 exit()，则返回 C_OK。如果返回 C_ERR，则调用 exit() 是不安全的。*/
int finishShutdown(void) {

    int save = server.shutdown_flags & SHUTDOWN_SAVE;
    int nosave = server.shutdown_flags & SHUTDOWN_NOSAVE;
    int force = server.shutdown_flags & SHUTDOWN_FORCE;

    /* Log a warning for each replica that is lagging. */
    /**为每个滞后的副本记录警告。*/
    listIter replicas_iter;
    listNode *replicas_list_node;
    int num_replicas = 0, num_lagging_replicas = 0;
    listRewind(server.slaves, &replicas_iter);
    while ((replicas_list_node = listNext(&replicas_iter)) != NULL) {
        client *replica = listNodeValue(replicas_list_node);
        num_replicas++;
        if (replica->repl_ack_off != server.master_repl_offset) {
            num_lagging_replicas++;
            long lag = replica->replstate == SLAVE_STATE_ONLINE ?
                time(NULL) - replica->repl_ack_time : 0;
            serverLog(LL_WARNING,
                      "Lagging replica %s reported offset %lld behind master, lag=%ld, state=%s.",
                      replicationGetSlaveName(replica),
                      server.master_repl_offset - replica->repl_ack_off,
                      lag,
                      replstateToString(replica->replstate));
        }
    }
    if (num_replicas > 0) {
        serverLog(LL_NOTICE,
                  "%d of %d replicas are in sync when shutting down.",
                  num_replicas - num_lagging_replicas,
                  num_replicas);
    }

    /* Kill all the Lua debugger forked sessions. */
    /**杀死所有 Lua 调试器分叉会话。*/
    ldbKillForkedSessions();

    /* Kill the saving child if there is a background saving in progress.
       We want to avoid race conditions, for instance our saving child may
       overwrite the synchronous saving did by SHUTDOWN. */
    /**如果正在进行后台保存，则杀死保存的孩子。我们希望避免竞争条件，例如我们的保存子可能会覆盖由 SHUTDOWN 所做的同步保存。*/
    if (server.child_type == CHILD_TYPE_RDB) {
        serverLog(LL_WARNING,"There is a child saving an .rdb. Killing it!");
        killRDBChild();
        /* Note that, in killRDBChild normally has backgroundSaveDoneHandler
         * doing it's cleanup, but in this case this code will not be reached,
         * so we need to call rdbRemoveTempFile which will close fd(in order
         * to unlink file actually) in background thread.
         * The temp rdb file fd may won't be closed when redis exits quickly,
         * but OS will close this fd when process exits. */
        /**请注意，在 killRDBChild 中通常有 backgroundSaveDoneHandler 进行清理，
         * 但在这种情况下将无法到达此代码，因此我们需要调用 rdbRemoveTempFile 它将在
         * 后台线程中关闭 fd（以便实际取消链接文件）。 redis 快速退出时，临时 rdb 文件 fd 可能不会关闭，
         * 但操作系统会在进程退出时关闭此 fd。*/
        rdbRemoveTempFile(server.child_pid, 0);
    }

    /* Kill module child if there is one. */
    /**如果有一个，则杀死模块子项。*/
    if (server.child_type == CHILD_TYPE_MODULE) {
        serverLog(LL_WARNING,"There is a module fork child. Killing it!");
        TerminateModuleForkChild(server.child_pid,0);
    }

    /* Kill the AOF saving child as the AOF we already have may be longer
     * but contains the full dataset anyway. */
    /**杀死 AOF 保存子，因为我们已经拥有的 AOF 可能更长，但无论如何都包含完整的数据集。*/
    if (server.child_type == CHILD_TYPE_AOF) {
        /* If we have AOF enabled but haven't written the AOF yet, don't
         * shutdown or else the dataset will be lost. */
        /**如果我们启用了 AOF 但尚未写入 AOF，请不要关闭，否则数据集将丢失。*/
        if (server.aof_state == AOF_WAIT_REWRITE) {
            if (force) {
                serverLog(LL_WARNING, "Writing initial AOF. Exit anyway.");
            } else {
                serverLog(LL_WARNING, "Writing initial AOF, can't exit.");
                goto error;
            }
        }
        serverLog(LL_WARNING,
                  "There is a child rewriting the AOF. Killing it!");
        killAppendOnlyChild();
    }
    if (server.aof_state != AOF_OFF) {
        /* Append only file: flush buffers and fsync() the AOF at exit */
        /**仅附加文件：刷新缓冲区和 fsync() 退出时的 AOF*/
        serverLog(LL_NOTICE,"Calling fsync() on the AOF file.");
        flushAppendOnlyFile(1);
        if (redis_fsync(server.aof_fd) == -1) {
            serverLog(LL_WARNING,"Fail to fsync the AOF file: %s.",
                                 strerror(errno));
        }
    }

    /* Create a new RDB file before exiting. */
    /**在退出之前创建一个新的 RDB 文件。*/
    if ((server.saveparamslen > 0 && !nosave) || save) {
        serverLog(LL_NOTICE,"Saving the final RDB snapshot before exiting.");
        if (server.supervised_mode == SUPERVISED_SYSTEMD)
            redisCommunicateSystemd("STATUS=Saving the final RDB snapshot\n");
        /* Snapshotting. Perform a SYNC SAVE and exit */
        /**快照。执行同步保存并退出*/
        rdbSaveInfo rsi, *rsiptr;
        rsiptr = rdbPopulateSaveInfo(&rsi);
        if (rdbSave(SLAVE_REQ_NONE,server.rdb_filename,rsiptr) != C_OK) {
            /* Ooops.. error saving! The best we can do is to continue
             * operating. Note that if there was a background saving process,
             * in the next cron() Redis will be notified that the background
             * saving aborted, handling special stuff like slaves pending for
             * synchronization... */
            /**哎呀..错误保存！我们能做的最好的就是继续运营。
             * 请注意，如果有后台保存过程，在下一个 cron() 中，Redis 将被通知后台保存中止，处理特殊的东西，
             * 例如等待同步的从属...*/
            if (force) {
                serverLog(LL_WARNING,"Error trying to save the DB. Exit anyway.");
            } else {
                serverLog(LL_WARNING,"Error trying to save the DB, can't exit.");
                if (server.supervised_mode == SUPERVISED_SYSTEMD)
                    redisCommunicateSystemd("STATUS=Error trying to save the DB, can't exit.\n");
                goto error;
            }
        }
    }

    /* Free the AOF manifest. */
    /**释放 AOF 清单。*/
    if (server.aof_manifest) aofManifestFree(server.aof_manifest);

    /* Fire the shutdown modules event. */
    /**触发关闭模块事件。*/
    moduleFireServerEvent(REDISMODULE_EVENT_SHUTDOWN,0,NULL);

    /* Remove the pid file if possible and needed. */
    /**如果可能和需要，删除 pid 文件。*/
    if (server.daemonize || server.pidfile) {
        serverLog(LL_NOTICE,"Removing the pid file.");
        unlink(server.pidfile);
    }

    /* Best effort flush of slave output buffers, so that we hopefully
     * send them pending writes. */
    /**从输出缓冲区的最大努力刷新，以便我们希望将它们发送到挂起的写入。*/
    flushSlavesOutputBuffers();

    /* Close the listening sockets. Apparently this allows faster restarts. */
    /**关闭监听套接字。显然，这允许更快的重新启动。*/
    closeListeningSockets(1);

    /* Unlock the cluster config file before shutdown */
    /**关机前解锁集群配置文件*/
    if (server.cluster_enabled && server.cluster_config_file_lock_fd != -1) {
        flock(server.cluster_config_file_lock_fd, LOCK_UN|LOCK_NB);
    }

    serverLog(LL_WARNING,"%s is now ready to exit, bye bye...",
        server.sentinel_mode ? "Sentinel" : "Redis");
    return C_OK;

error:
    serverLog(LL_WARNING, "Errors trying to shut down the server. Check the logs for more information.");
    cancelShutdown();
    return C_ERR;
}

/*================================== Commands =============================== */

/* Sometimes Redis cannot accept write commands because there is a persistence
 * error with the RDB or AOF file, and Redis is configured in order to stop
 * accepting writes in such situation. This function returns if such a
 * condition is active, and the type of the condition.
 *
 * Function return values:
 *
 * DISK_ERROR_TYPE_NONE:    No problems, we can accept writes.
 * DISK_ERROR_TYPE_AOF:     Don't accept writes: AOF errors.
 * DISK_ERROR_TYPE_RDB:     Don't accept writes: RDB errors.
 */
/**有时 Redis 无法接受写入命令，因为 RDB 或 AOF 文件存在持久性错误，并且 Redis 被配置为在这种情况下停止接受写入。
 * 如果这样的条件处于活动状态，此函数将返回，以及条件的类
 * 函数返回值：
 *   DISK_ERROR_TYPE_NONE：没问题，我们可以接受写入。
 *   DISK_ERROR_TYPE_AOF：不接受写入：AOF 错误。
 *   DISK_ERROR_TYPE_RDB：不接受写入：RDB 错误。*/
int writeCommandsDeniedByDiskError(void) {
    if (server.stop_writes_on_bgsave_err &&
        server.saveparamslen > 0 &&
        server.lastbgsave_status == C_ERR)
    {
        return DISK_ERROR_TYPE_RDB;
    } else if (server.aof_state != AOF_OFF) {
        if (server.aof_last_write_status == C_ERR) {
            return DISK_ERROR_TYPE_AOF;
        }
        /* AOF fsync error. */
        int aof_bio_fsync_status;
        atomicGet(server.aof_bio_fsync_status,aof_bio_fsync_status);
        if (aof_bio_fsync_status == C_ERR) {
            atomicGet(server.aof_bio_fsync_errno,server.aof_last_write_errno);
            return DISK_ERROR_TYPE_AOF;
        }
    }

    return DISK_ERROR_TYPE_NONE;
}

sds writeCommandsGetDiskErrorMessage(int error_code) {
    sds ret = NULL;
    if (error_code == DISK_ERROR_TYPE_RDB) {
        ret = sdsdup(shared.bgsaveerr->ptr);
    } else {
        ret = sdscatfmt(sdsempty(),
                "-MISCONF Errors writing to the AOF file: %s\r\n",
                strerror(server.aof_last_write_errno));
    }
    return ret;
}

/* The PING command. It works in a different way if the client is in
 * in Pub/Sub mode. */
/**PING 命令。如果客户端处于 Pub/Sub 模式，它会以不同的方式工作。*/
void pingCommand(client *c) {
    /* The command takes zero or one arguments. */
    /**该命令采用零个或一个参数。*/
    if (c->argc > 2) {
        addReplyErrorArity(c);
        return;
    }

    if (c->flags & CLIENT_PUBSUB && c->resp == 2) {
        addReply(c,shared.mbulkhdr[2]);
        addReplyBulkCBuffer(c,"pong",4);
        if (c->argc == 1)
            addReplyBulkCBuffer(c,"",0);
        else
            addReplyBulk(c,c->argv[1]);
    } else {
        if (c->argc == 1)
            addReply(c,shared.pong);
        else
            addReplyBulk(c,c->argv[1]);
    }
}

void echoCommand(client *c) {
    addReplyBulk(c,c->argv[1]);
}

void timeCommand(client *c) {
    struct timeval tv;

    /* gettimeofday() can only fail if &tv is a bad address so we
     * don't check for errors. */
    /**gettimeofday() 只有在 &tv 是错误地址时才会失败，因此我们不会检查错误。*/
    gettimeofday(&tv,NULL);
    addReplyArrayLen(c,2);
    addReplyBulkLongLong(c,tv.tv_sec);
    addReplyBulkLongLong(c,tv.tv_usec);
}

typedef struct replyFlagNames {
    uint64_t flag;
    const char *name;
} replyFlagNames;

/* Helper function to output flags. 输出标志的辅助函数。*/
void addReplyCommandFlags(client *c, uint64_t flags, replyFlagNames *replyFlags) {
    int count = 0, j=0;
    /* Count them so we don't have to use deferred reply. */
    /**计算它们，这样我们就不必使用延迟回复。*/
    while (replyFlags[j].name) {
        if (flags & replyFlags[j].flag)
            count++;
        j++;
    }

    addReplySetLen(c, count);
    j = 0;
    while (replyFlags[j].name) {
        if (flags & replyFlags[j].flag)
            addReplyStatus(c, replyFlags[j].name);
        j++;
    }
}

void addReplyFlagsForCommand(client *c, struct redisCommand *cmd) {
    replyFlagNames flagNames[] = {
        {CMD_WRITE,             "write"},
        {CMD_READONLY,          "readonly"},
        {CMD_DENYOOM,           "denyoom"},
        {CMD_MODULE,            "module"},
        {CMD_ADMIN,             "admin"},
        {CMD_PUBSUB,            "pubsub"},
        {CMD_NOSCRIPT,          "noscript"},
        {CMD_BLOCKING,          "blocking"},
        {CMD_LOADING,           "loading"},
        {CMD_STALE,             "stale"},
        {CMD_SKIP_MONITOR,      "skip_monitor"},
        {CMD_SKIP_SLOWLOG,      "skip_slowlog"},
        {CMD_ASKING,            "asking"},
        {CMD_FAST,              "fast"},
        {CMD_NO_AUTH,           "no_auth"},
        /* {CMD_MAY_REPLICATE,     "may_replicate"},, Hidden on purpose 故意隐藏*/
        /* {CMD_SENTINEL,          "sentinel"}, Hidden on purpose */
        /* {CMD_ONLY_SENTINEL,     "only_sentinel"}, Hidden on purpose */
        {CMD_NO_MANDATORY_KEYS, "no_mandatory_keys"},
        /* {CMD_PROTECTED,         "protected"}, Hidden on purpose */
        {CMD_NO_ASYNC_LOADING,  "no_async_loading"},
        {CMD_NO_MULTI,          "no_multi"},
        {CMD_MOVABLE_KEYS,      "movablekeys"},
        {CMD_ALLOW_BUSY,        "allow_busy"},
        {0,NULL}
    };
    addReplyCommandFlags(c, cmd->flags, flagNames);
}

void addReplyDocFlagsForCommand(client *c, struct redisCommand *cmd) {
    replyFlagNames docFlagNames[] = {
        {CMD_DOC_DEPRECATED,         "deprecated"},
        {CMD_DOC_SYSCMD,             "syscmd"},
        {0,NULL}
    };
    addReplyCommandFlags(c, cmd->doc_flags, docFlagNames);
}

void addReplyFlagsForKeyArgs(client *c, uint64_t flags) {
    replyFlagNames docFlagNames[] = {
        {CMD_KEY_RO,              "RO"},
        {CMD_KEY_RW,              "RW"},
        {CMD_KEY_OW,              "OW"},
        {CMD_KEY_RM,              "RM"},
        {CMD_KEY_ACCESS,          "access"},
        {CMD_KEY_UPDATE,          "update"},
        {CMD_KEY_INSERT,          "insert"},
        {CMD_KEY_DELETE,          "delete"},
        {CMD_KEY_NOT_KEY,         "not_key"},
        {CMD_KEY_INCOMPLETE,      "incomplete"},
        {CMD_KEY_VARIABLE_FLAGS,  "variable_flags"},
        {0,NULL}
    };
    addReplyCommandFlags(c, flags, docFlagNames);
}

/* Must match redisCommandArgType */
const char *ARG_TYPE_STR[] = {
    "string",
    "integer",
    "double",
    "key",
    "pattern",
    "unix-time",
    "pure-token",
    "oneof",
    "block",
};

void addReplyFlagsForArg(client *c, uint64_t flags) {
    replyFlagNames argFlagNames[] = {
        {CMD_ARG_OPTIONAL,          "optional"},
        {CMD_ARG_MULTIPLE,          "multiple"},
        {CMD_ARG_MULTIPLE_TOKEN,    "multiple_token"},
        {0,NULL}
    };
    addReplyCommandFlags(c, flags, argFlagNames);
}

void addReplyCommandArgList(client *c, struct redisCommandArg *args, int num_args) {
    addReplyArrayLen(c, num_args);
    for (int j = 0; j<num_args; j++) {
        /* Count our reply len so we don't have to use deferred reply. */
        /**计算我们的回复 len，这样我们就不必使用延迟回复。*/
        long maplen = 2;
        if (args[j].key_spec_index != -1) maplen++;
        if (args[j].token) maplen++;
        if (args[j].summary) maplen++;
        if (args[j].since) maplen++;
        if (args[j].deprecated_since) maplen++;
        if (args[j].flags) maplen++;
        if (args[j].type == ARG_TYPE_ONEOF || args[j].type == ARG_TYPE_BLOCK)
            maplen++;
        addReplyMapLen(c, maplen);

        addReplyBulkCString(c, "name");
        addReplyBulkCString(c, args[j].name);

        addReplyBulkCString(c, "type");
        addReplyBulkCString(c, ARG_TYPE_STR[args[j].type]);

        if (args[j].key_spec_index != -1) {
            addReplyBulkCString(c, "key_spec_index");
            addReplyLongLong(c, args[j].key_spec_index);
        }
        if (args[j].token) {
            addReplyBulkCString(c, "token");
            addReplyBulkCString(c, args[j].token);
        }
        if (args[j].summary) {
            addReplyBulkCString(c, "summary");
            addReplyBulkCString(c, args[j].summary);
        }
        if (args[j].since) {
            addReplyBulkCString(c, "since");
            addReplyBulkCString(c, args[j].since);
        }
        if (args[j].deprecated_since) {
            addReplyBulkCString(c, "deprecated_since");
            addReplyBulkCString(c, args[j].deprecated_since);
        }
        if (args[j].flags) {
            addReplyBulkCString(c, "flags");
            addReplyFlagsForArg(c, args[j].flags);
        }
        if (args[j].type == ARG_TYPE_ONEOF || args[j].type == ARG_TYPE_BLOCK) {
            addReplyBulkCString(c, "arguments");
            addReplyCommandArgList(c, args[j].subargs, args[j].num_args);
        }
    }
}

/* Must match redisCommandRESP2Type */
const char *RESP2_TYPE_STR[] = {
    "simple-string",
    "error",
    "integer",
    "bulk-string",
    "null-bulk-string",
    "array",
    "null-array",
};

/* Must match redisCommandRESP3Type */
const char *RESP3_TYPE_STR[] = {
    "simple-string",
    "error",
    "integer",
    "double",
    "bulk-string",
    "array",
    "map",
    "set",
    "bool",
    "null",
};

void addReplyCommandHistory(client *c, struct redisCommand *cmd) {
    addReplySetLen(c, cmd->num_history);
    for (int j = 0; j<cmd->num_history; j++) {
        addReplyArrayLen(c, 2);
        addReplyBulkCString(c, cmd->history[j].since);
        addReplyBulkCString(c, cmd->history[j].changes);
    }
}

void addReplyCommandTips(client *c, struct redisCommand *cmd) {
    addReplySetLen(c, cmd->num_tips);
    for (int j = 0; j<cmd->num_tips; j++) {
        addReplyBulkCString(c, cmd->tips[j]);
    }
}

void addReplyCommandKeySpecs(client *c, struct redisCommand *cmd) {
    addReplySetLen(c, cmd->key_specs_num);
    for (int i = 0; i < cmd->key_specs_num; i++) {
        int maplen = 3;
        if (cmd->key_specs[i].notes) maplen++;

        addReplyMapLen(c, maplen);

        if (cmd->key_specs[i].notes) {
            addReplyBulkCString(c, "notes");
            addReplyBulkCString(c,cmd->key_specs[i].notes);
        }

        addReplyBulkCString(c, "flags");
        addReplyFlagsForKeyArgs(c,cmd->key_specs[i].flags);

        addReplyBulkCString(c, "begin_search");
        switch (cmd->key_specs[i].begin_search_type) {
            case KSPEC_BS_UNKNOWN:
                addReplyMapLen(c, 2);
                addReplyBulkCString(c, "type");
                addReplyBulkCString(c, "unknown");

                addReplyBulkCString(c, "spec");
                addReplyMapLen(c, 0);
                break;
            case KSPEC_BS_INDEX:
                addReplyMapLen(c, 2);
                addReplyBulkCString(c, "type");
                addReplyBulkCString(c, "index");

                addReplyBulkCString(c, "spec");
                addReplyMapLen(c, 1);
                addReplyBulkCString(c, "index");
                addReplyLongLong(c, cmd->key_specs[i].bs.index.pos);
                break;
            case KSPEC_BS_KEYWORD:
                addReplyMapLen(c, 2);
                addReplyBulkCString(c, "type");
                addReplyBulkCString(c, "keyword");

                addReplyBulkCString(c, "spec");
                addReplyMapLen(c, 2);
                addReplyBulkCString(c, "keyword");
                addReplyBulkCString(c, cmd->key_specs[i].bs.keyword.keyword);
                addReplyBulkCString(c, "startfrom");
                addReplyLongLong(c, cmd->key_specs[i].bs.keyword.startfrom);
                break;
            default:
                serverPanic("Invalid begin_search key spec type %d", cmd->key_specs[i].begin_search_type);
        }

        addReplyBulkCString(c, "find_keys");
        switch (cmd->key_specs[i].find_keys_type) {
            case KSPEC_FK_UNKNOWN:
                addReplyMapLen(c, 2);
                addReplyBulkCString(c, "type");
                addReplyBulkCString(c, "unknown");

                addReplyBulkCString(c, "spec");
                addReplyMapLen(c, 0);
                break;
            case KSPEC_FK_RANGE:
                addReplyMapLen(c, 2);
                addReplyBulkCString(c, "type");
                addReplyBulkCString(c, "range");

                addReplyBulkCString(c, "spec");
                addReplyMapLen(c, 3);
                addReplyBulkCString(c, "lastkey");
                addReplyLongLong(c, cmd->key_specs[i].fk.range.lastkey);
                addReplyBulkCString(c, "keystep");
                addReplyLongLong(c, cmd->key_specs[i].fk.range.keystep);
                addReplyBulkCString(c, "limit");
                addReplyLongLong(c, cmd->key_specs[i].fk.range.limit);
                break;
            case KSPEC_FK_KEYNUM:
                addReplyMapLen(c, 2);
                addReplyBulkCString(c, "type");
                addReplyBulkCString(c, "keynum");

                addReplyBulkCString(c, "spec");
                addReplyMapLen(c, 3);
                addReplyBulkCString(c, "keynumidx");
                addReplyLongLong(c, cmd->key_specs[i].fk.keynum.keynumidx);
                addReplyBulkCString(c, "firstkey");
                addReplyLongLong(c, cmd->key_specs[i].fk.keynum.firstkey);
                addReplyBulkCString(c, "keystep");
                addReplyLongLong(c, cmd->key_specs[i].fk.keynum.keystep);
                break;
            default:
                serverPanic("Invalid find_keys key spec type %d", cmd->key_specs[i].begin_search_type);
        }
    }
}

/* Reply with an array of sub-command using the provided reply callback. */
/**使用提供的回复回调回复一组子命令。*/
void addReplyCommandSubCommands(client *c, struct redisCommand *cmd, void (*reply_function)(client*, struct redisCommand*), int use_map) {
    if (!cmd->subcommands_dict) {
        addReplySetLen(c, 0);
        return;
    }

    if (use_map)
        addReplyMapLen(c, dictSize(cmd->subcommands_dict));
    else
        addReplyArrayLen(c, dictSize(cmd->subcommands_dict));
    dictEntry *de;
    dictIterator *di = dictGetSafeIterator(cmd->subcommands_dict);
    while((de = dictNext(di)) != NULL) {
        struct redisCommand *sub = (struct redisCommand *)dictGetVal(de);
        if (use_map)
            addReplyBulkCBuffer(c, sub->fullname, sdslen(sub->fullname));
        reply_function(c, sub);
    }
    dictReleaseIterator(di);
}

/* Must match redisCommandGroup 必须匹配 redisCommandGroup*/
const char *COMMAND_GROUP_STR[] = {
    "generic",
    "string",
    "list",
    "set",
    "sorted-set",
    "hash",
    "pubsub",
    "transactions",
    "connection",
    "server",
    "scripting",
    "hyperloglog",
    "cluster",
    "sentinel",
    "geo",
    "stream",
    "bitmap",
    "module"
};

/* Output the representation of a Redis command. Used by the COMMAND command and COMMAND INFO. */
/**输出 Redis 命令的表示形式。由 COMMAND 命令和 COMMAND INFO 使用。*/
void addReplyCommandInfo(client *c, struct redisCommand *cmd) {
    if (!cmd) {
        addReplyNull(c);
    } else {
        int firstkey = 0, lastkey = 0, keystep = 0;
        if (cmd->legacy_range_key_spec.begin_search_type != KSPEC_BS_INVALID) {
            firstkey = cmd->legacy_range_key_spec.bs.index.pos;
            lastkey = cmd->legacy_range_key_spec.fk.range.lastkey;
            if (lastkey >= 0)
                lastkey += firstkey;
            keystep = cmd->legacy_range_key_spec.fk.range.keystep;
        }

        addReplyArrayLen(c, 10);
        addReplyBulkCBuffer(c, cmd->fullname, sdslen(cmd->fullname));
        addReplyLongLong(c, cmd->arity);
        addReplyFlagsForCommand(c, cmd);
        addReplyLongLong(c, firstkey);
        addReplyLongLong(c, lastkey);
        addReplyLongLong(c, keystep);
        addReplyCommandCategories(c, cmd);
        addReplyCommandTips(c, cmd);
        addReplyCommandKeySpecs(c, cmd);
        addReplyCommandSubCommands(c, cmd, addReplyCommandInfo, 0);
    }
}

/* Output the representation of a Redis command. Used by the COMMAND DOCS. */
/**输出 Redis 命令的表示形式。由 COMMAND DOCS 使用。*/
void addReplyCommandDocs(client *c, struct redisCommand *cmd) {
    /* Count our reply len so we don't have to use deferred reply. */
    /**计算我们的回复 len，这样我们就不必使用延迟回复。*/
    long maplen = 1;
    if (cmd->summary) maplen++;
    if (cmd->since) maplen++;
    if (cmd->flags & CMD_MODULE) maplen++;
    if (cmd->complexity) maplen++;
    if (cmd->doc_flags) maplen++;
    if (cmd->deprecated_since) maplen++;
    if (cmd->replaced_by) maplen++;
    if (cmd->history) maplen++;
    if (cmd->args) maplen++;
    if (cmd->subcommands_dict) maplen++;
    addReplyMapLen(c, maplen);

    if (cmd->summary) {
        addReplyBulkCString(c, "summary");
        addReplyBulkCString(c, cmd->summary);
    }
    if (cmd->since) {
        addReplyBulkCString(c, "since");
        addReplyBulkCString(c, cmd->since);
    }

    /* Always have the group, for module commands the group is always "module". */
    /**总是有组，对于模块命令，组总是“模块”。*/
    addReplyBulkCString(c, "group");
    addReplyBulkCString(c, COMMAND_GROUP_STR[cmd->group]);

    if (cmd->complexity) {
        addReplyBulkCString(c, "complexity");
        addReplyBulkCString(c, cmd->complexity);
    }
    if (cmd->flags & CMD_MODULE) {
        addReplyBulkCString(c, "module");
        addReplyBulkCString(c, moduleNameFromCommand(cmd));
    }
    if (cmd->doc_flags) {
        addReplyBulkCString(c, "doc_flags");
        addReplyDocFlagsForCommand(c, cmd);
    }
    if (cmd->deprecated_since) {
        addReplyBulkCString(c, "deprecated_since");
        addReplyBulkCString(c, cmd->deprecated_since);
    }
    if (cmd->replaced_by) {
        addReplyBulkCString(c, "replaced_by");
        addReplyBulkCString(c, cmd->replaced_by);
    }
    if (cmd->history) {
        addReplyBulkCString(c, "history");
        addReplyCommandHistory(c, cmd);
    }
    if (cmd->args) {
        addReplyBulkCString(c, "arguments");
        addReplyCommandArgList(c, cmd->args, cmd->num_args);
    }
    if (cmd->subcommands_dict) {
        addReplyBulkCString(c, "subcommands");
        addReplyCommandSubCommands(c, cmd, addReplyCommandDocs, 1);
    }
}

/* Helper for COMMAND GETKEYS and GETKEYSANDFLAGS */
void getKeysSubcommandImpl(client *c, int with_flags) {
    struct redisCommand *cmd = lookupCommand(c->argv+2,c->argc-2);
    getKeysResult result = GETKEYS_RESULT_INIT;
    int j;

    if (!cmd) {
        addReplyError(c,"Invalid command specified");
        return;
    } else if (!doesCommandHaveKeys(cmd)) {
        addReplyError(c,"The command has no key arguments");
        return;
    } else if ((cmd->arity > 0 && cmd->arity != c->argc-2) ||
               ((c->argc-2) < -cmd->arity))
    {
        addReplyError(c,"Invalid number of arguments specified for command");
        return;
    }

    if (!getKeysFromCommandWithSpecs(cmd,c->argv+2,c->argc-2,GET_KEYSPEC_DEFAULT,&result)) {
        if (cmd->flags & CMD_NO_MANDATORY_KEYS) {
            addReplyArrayLen(c,0);
        } else {
            addReplyError(c,"Invalid arguments specified for command");
        }
    } else {
        addReplyArrayLen(c,result.numkeys);
        for (j = 0; j < result.numkeys; j++) {
            if (!with_flags) {
                addReplyBulk(c,c->argv[result.keys[j].pos+2]);
            } else {
                addReplyArrayLen(c,2);
                addReplyBulk(c,c->argv[result.keys[j].pos+2]);
                addReplyFlagsForKeyArgs(c,result.keys[j].flags);
            }
        }
    }
    getKeysFreeResult(&result);
}

/* COMMAND GETKEYSANDFLAGS cmd arg1 arg2 ... */
void commandGetKeysAndFlagsCommand(client *c) {
    getKeysSubcommandImpl(c, 1);
}

/* COMMAND GETKEYS cmd arg1 arg2 ... */
void getKeysSubcommand(client *c) {
    getKeysSubcommandImpl(c, 0);
}

/* COMMAND (no args) */
void commandCommand(client *c) {
    dictIterator *di;
    dictEntry *de;

    addReplyArrayLen(c, dictSize(server.commands));
    di = dictGetIterator(server.commands);
    while ((de = dictNext(di)) != NULL) {
        addReplyCommandInfo(c, dictGetVal(de));
    }
    dictReleaseIterator(di);
}

/* COMMAND COUNT */
void commandCountCommand(client *c) {
    addReplyLongLong(c, dictSize(server.commands));
}

typedef enum {
    COMMAND_LIST_FILTER_MODULE,
    COMMAND_LIST_FILTER_ACLCAT,
    COMMAND_LIST_FILTER_PATTERN,
} commandListFilterType;

typedef struct {
    commandListFilterType type;
    sds arg;
    struct {
        int valid;
        union {
            uint64_t aclcat;
            void *module_handle;
        } u;
    } cache;
} commandListFilter;

int shouldFilterFromCommandList(struct redisCommand *cmd, commandListFilter *filter) {
    switch (filter->type) {
        case (COMMAND_LIST_FILTER_MODULE):
            if (!filter->cache.valid) {
                filter->cache.u.module_handle = moduleGetHandleByName(filter->arg);
                filter->cache.valid = 1;
            }
            return !moduleIsModuleCommand(filter->cache.u.module_handle, cmd);
        case (COMMAND_LIST_FILTER_ACLCAT): {
            if (!filter->cache.valid) {
                filter->cache.u.aclcat = ACLGetCommandCategoryFlagByName(filter->arg);
                filter->cache.valid = 1;
            }
            uint64_t cat = filter->cache.u.aclcat;
            if (cat == 0)
                return 1; /* Invalid ACL category */
            return (!(cmd->acl_categories & cat));
            break;
        }
        case (COMMAND_LIST_FILTER_PATTERN):
            return !stringmatchlen(filter->arg, sdslen(filter->arg), cmd->fullname, sdslen(cmd->fullname), 1);
        default:
            serverPanic("Invalid filter type %d", filter->type);
    }
}

/* COMMAND LIST FILTERBY (MODULE <module-name>|ACLCAT <cat>|PATTERN <pattern>) */
void commandListWithFilter(client *c, dict *commands, commandListFilter filter, int *numcmds) {
    dictEntry *de;
    dictIterator *di = dictGetIterator(commands);

    while ((de = dictNext(di)) != NULL) {
        struct redisCommand *cmd = dictGetVal(de);
        if (!shouldFilterFromCommandList(cmd,&filter)) {
            addReplyBulkCBuffer(c, cmd->fullname, sdslen(cmd->fullname));
            (*numcmds)++;
        }

        if (cmd->subcommands_dict) {
            commandListWithFilter(c, cmd->subcommands_dict, filter, numcmds);
        }
    }
    dictReleaseIterator(di);
}

/* COMMAND LIST */
void commandListWithoutFilter(client *c, dict *commands, int *numcmds) {
    dictEntry *de;
    dictIterator *di = dictGetIterator(commands);

    while ((de = dictNext(di)) != NULL) {
        struct redisCommand *cmd = dictGetVal(de);
        addReplyBulkCBuffer(c, cmd->fullname, sdslen(cmd->fullname));
        (*numcmds)++;

        if (cmd->subcommands_dict) {
            commandListWithoutFilter(c, cmd->subcommands_dict, numcmds);
        }
    }
    dictReleaseIterator(di);
}

/* COMMAND LIST [FILTERBY (MODULE <module-name>|ACLCAT <cat>|PATTERN <pattern>)] */
void commandListCommand(client *c) {

    /* Parse options. */
    int i = 2, got_filter = 0;
    commandListFilter filter = {0};
    for (; i < c->argc; i++) {
        int moreargs = (c->argc-1) - i; /* Number of additional arguments. */
        char *opt = c->argv[i]->ptr;
        if (!strcasecmp(opt,"filterby") && moreargs == 2) {
            char *filtertype = c->argv[i+1]->ptr;
            if (!strcasecmp(filtertype,"module")) {
                filter.type = COMMAND_LIST_FILTER_MODULE;
            } else if (!strcasecmp(filtertype,"aclcat")) {
                filter.type = COMMAND_LIST_FILTER_ACLCAT;
            } else if (!strcasecmp(filtertype,"pattern")) {
                filter.type = COMMAND_LIST_FILTER_PATTERN;
            } else {
                addReplyErrorObject(c,shared.syntaxerr);
                return;
            }
            got_filter = 1;
            filter.arg = c->argv[i+2]->ptr;
            i += 2;
        } else {
            addReplyErrorObject(c,shared.syntaxerr);
            return;
        }
    }

    int numcmds = 0;
    void *replylen = addReplyDeferredLen(c);

    if (got_filter) {
        commandListWithFilter(c, server.commands, filter, &numcmds);
    } else {
        commandListWithoutFilter(c, server.commands, &numcmds);
    }

    setDeferredArrayLen(c,replylen,numcmds);
}

/* COMMAND INFO [<command-name> ...] */
void commandInfoCommand(client *c) {
    int i;

    if (c->argc == 2) {
        dictIterator *di;
        dictEntry *de;
        addReplyArrayLen(c, dictSize(server.commands));
        di = dictGetIterator(server.commands);
        while ((de = dictNext(di)) != NULL) {
            addReplyCommandInfo(c, dictGetVal(de));
        }
        dictReleaseIterator(di);
    } else {
        addReplyArrayLen(c, c->argc-2);
        for (i = 2; i < c->argc; i++) {
            addReplyCommandInfo(c, lookupCommandBySds(c->argv[i]->ptr));
        }
    }
}

/* COMMAND DOCS [command-name [command-name ...]] */
void commandDocsCommand(client *c) {
    int i;
    if (c->argc == 2) {
        /* Reply with an array of all commands 回复所有命令的数组*/
        dictIterator *di;
        dictEntry *de;
        addReplyMapLen(c, dictSize(server.commands));
        di = dictGetIterator(server.commands);
        while ((de = dictNext(di)) != NULL) {
            struct redisCommand *cmd = dictGetVal(de);
            addReplyBulkCBuffer(c, cmd->fullname, sdslen(cmd->fullname));
            addReplyCommandDocs(c, cmd);
        }
        dictReleaseIterator(di);
    } else {
        /* Reply with an array of the requested commands (if we find them) */
        /**回复一组请求的命令（如果我们找到它们）*/
        int numcmds = 0;
        void *replylen = addReplyDeferredLen(c);
        for (i = 2; i < c->argc; i++) {
            struct redisCommand *cmd = lookupCommandBySds(c->argv[i]->ptr);
            if (!cmd)
                continue;
            addReplyBulkCBuffer(c, cmd->fullname, sdslen(cmd->fullname));
            addReplyCommandDocs(c, cmd);
            numcmds++;
        }
        setDeferredMapLen(c,replylen,numcmds);
    }
}

/* COMMAND GETKEYS arg0 arg1 arg2 ... */
void commandGetKeysCommand(client *c) {
    getKeysSubcommand(c);
}

/* COMMAND HELP */
void commandHelpCommand(client *c) {
    const char *help[] = {
"(no subcommand)",
"    Return details about all Redis commands.",
"COUNT",
"    Return the total number of commands in this Redis server.",
"LIST",
"    Return a list of all commands in this Redis server.",
"INFO [<command-name> ...]",
"    Return details about multiple Redis commands.",
"    If no command names are given, documentation details for all",
"    commands are returned.",
"DOCS [<command-name> ...]",
"    Return documentation details about multiple Redis commands.",
"    If no command names are given, documentation details for all",
"    commands are returned.",
"GETKEYS <full-command>",
"    Return the keys from a full Redis command.",
"GETKEYSANDFLAGS <full-command>",
"    Return the keys and the access flags from a full Redis command.",
NULL
    };

    addReplyHelp(c, help);
}

/* Convert an amount of bytes into a human readable string in the form
 * of 100B, 2G, 100M, 4K, and so forth. */
/**将一定数量的字节转换为 100B、2G、100M、4K 等形式的人类可读字符串。*/
void bytesToHuman(char *s, unsigned long long n) {
    double d;

    if (n < 1024) {
        /* Bytes */
        sprintf(s,"%lluB",n);
    } else if (n < (1024*1024)) {
        d = (double)n/(1024);
        sprintf(s,"%.2fK",d);
    } else if (n < (1024LL*1024*1024)) {
        d = (double)n/(1024*1024);
        sprintf(s,"%.2fM",d);
    } else if (n < (1024LL*1024*1024*1024)) {
        d = (double)n/(1024LL*1024*1024);
        sprintf(s,"%.2fG",d);
    } else if (n < (1024LL*1024*1024*1024*1024)) {
        d = (double)n/(1024LL*1024*1024*1024);
        sprintf(s,"%.2fT",d);
    } else if (n < (1024LL*1024*1024*1024*1024*1024)) {
        d = (double)n/(1024LL*1024*1024*1024*1024);
        sprintf(s,"%.2fP",d);
    } else {
        /* Let's hope we never need this */
        sprintf(s,"%lluB",n);
    }
}

/* Fill percentile distribution of latencies. 填充延迟的百分位分布。*/
sds fillPercentileDistributionLatencies(sds info, const char* histogram_name, struct hdr_histogram* histogram) {
    info = sdscatfmt(info,"latency_percentiles_usec_%s:",histogram_name);
    for (int j = 0; j < server.latency_tracking_info_percentiles_len; j++) {
        char fbuf[128];
        size_t len = sprintf(fbuf, "%f", server.latency_tracking_info_percentiles[j]);
        len = trimDoubleString(fbuf, len);
        info = sdscatprintf(info,"p%s=%.3f", fbuf,
            ((double)hdr_value_at_percentile(histogram,server.latency_tracking_info_percentiles[j]))/1000.0f);
        if (j != server.latency_tracking_info_percentiles_len-1)
            info = sdscatlen(info,",",1);
        }
    info = sdscatprintf(info,"\r\n");
    return info;
}

const char *replstateToString(int replstate) {
    switch (replstate) {
    case SLAVE_STATE_WAIT_BGSAVE_START:
    case SLAVE_STATE_WAIT_BGSAVE_END:
        return "wait_bgsave";
    case SLAVE_STATE_SEND_BULK:
        return "send_bulk";
    case SLAVE_STATE_ONLINE:
        return "online";
    default:
        return "";
    }
}

/* Characters we sanitize on INFO output to maintain expected format. 我们对 INFO 输出中的字符进行清理以保持预期的格式。*/
static char unsafe_info_chars[] = "#:\n\r";
static char unsafe_info_chars_substs[] = "____";   /* Must be same length as above 必须与上述长度相同*/

/* Returns a sanitized version of s that contains no unsafe info string chars.
 * If no unsafe characters are found, simply returns s. Caller needs to
 * free tmp if it is non-null on return.
 */
/**返回不包含不安全信息字符串字符的 s 的净化版本。如果没有找到不安全字符，则简单地返回 s。
 * 如果返回时非空，调用者需要释放 tmp。*/
const char *getSafeInfoString(const char *s, size_t len, char **tmp) {
    *tmp = NULL;
    if (mempbrk(s, len, unsafe_info_chars,sizeof(unsafe_info_chars)-1)
        == NULL) return s;
    char *new = *tmp = zmalloc(len + 1);
    memcpy(new, s, len);
    new[len] = '\0';
    return memmapchars(new, len, unsafe_info_chars, unsafe_info_chars_substs,
                       sizeof(unsafe_info_chars)-1);
}

sds genRedisInfoStringCommandStats(sds info, dict *commands) {
    struct redisCommand *c;
    dictEntry *de;
    dictIterator *di;
    di = dictGetSafeIterator(commands);
    while((de = dictNext(di)) != NULL) {
        char *tmpsafe;
        c = (struct redisCommand *) dictGetVal(de);
        if (c->calls || c->failed_calls || c->rejected_calls) {
            info = sdscatprintf(info,
                "cmdstat_%s:calls=%lld,usec=%lld,usec_per_call=%.2f"
                ",rejected_calls=%lld,failed_calls=%lld\r\n",
                getSafeInfoString(c->fullname, sdslen(c->fullname), &tmpsafe), c->calls, c->microseconds,
                (c->calls == 0) ? 0 : ((float)c->microseconds/c->calls),
                c->rejected_calls, c->failed_calls);
            if (tmpsafe != NULL) zfree(tmpsafe);
        }
        if (c->subcommands_dict) {
            info = genRedisInfoStringCommandStats(info, c->subcommands_dict);
        }
    }
    dictReleaseIterator(di);

    return info;
}

sds genRedisInfoStringLatencyStats(sds info, dict *commands) {
    struct redisCommand *c;
    dictEntry *de;
    dictIterator *di;
    di = dictGetSafeIterator(commands);
    while((de = dictNext(di)) != NULL) {
        char *tmpsafe;
        c = (struct redisCommand *) dictGetVal(de);
        if (c->latency_histogram) {
            info = fillPercentileDistributionLatencies(info,
                getSafeInfoString(c->fullname, sdslen(c->fullname), &tmpsafe),
                c->latency_histogram);
            if (tmpsafe != NULL) zfree(tmpsafe);
        }
        if (c->subcommands_dict) {
            info = genRedisInfoStringLatencyStats(info, c->subcommands_dict);
        }
    }
    dictReleaseIterator(di);

    return info;
}

/* Takes a null terminated sections list, and adds them to the dict. */
/**获取一个空终止的部分列表，并将它们添加到字典中。*/
void addInfoSectionsToDict(dict *section_dict, char **sections) {
    while (*sections) {
        sds section = sdsnew(*sections);
        if (dictAdd(section_dict, section, NULL)==DICT_ERR)
            sdsfree(section);
        sections++;
    }
}

/* Cached copy of the default sections, as an optimization. */
/**默认部分的缓存副本，作为优化。*/
static dict *cached_default_info_sections = NULL;

void releaseInfoSectionDict(dict *sec) {
    if (sec != cached_default_info_sections)
        dictRelease(sec);
}

/* Create a dictionary with unique section names to be used by genRedisInfoString.
 * 'argv' and 'argc' are list of arguments for INFO.
 * 'defaults' is an optional null terminated list of default sections.
 * 'out_all' and 'out_everything' are optional.
 * The resulting dictionary should be released with releaseInfoSectionDict. */
/**创建一个具有唯一节名称的字典，供 genRedisInfoString 使用。 'argv' 和 'argc' 是 INFO 的参数列表。
 * 'defaults' 是一个可选的以 null 结尾的默认部分列表。 “out_all”和“out_everything”是可选的。
 * 生成的字典应该与 releaseInfoSectionDict 一起发布。*/
dict *genInfoSectionDict(robj **argv, int argc, char **defaults, int *out_all, int *out_everything) {
    char *default_sections[] = {
        "server", "clients", "memory", "persistence", "stats", "replication",
        "cpu", "module_list", "errorstats", "cluster", "keyspace", NULL};
    if (!defaults)
        defaults = default_sections;

    if (argc == 0) {
        /* In this case we know the dict is not gonna be modified, so we cache
         * it as an optimization for a common case. */
        /**在这种情况下，我们知道 dict 不会被修改，因此我们将其缓存为常见情况的优化。*/
        if (cached_default_info_sections)
            return cached_default_info_sections;
        cached_default_info_sections = dictCreate(&stringSetDictType);
        dictExpand(cached_default_info_sections, 16);
        addInfoSectionsToDict(cached_default_info_sections, defaults);
        return cached_default_info_sections;
    }

    dict *section_dict = dictCreate(&stringSetDictType);
    dictExpand(section_dict, min(argc,16));
    for (int i = 0; i < argc; i++) {
        if (!strcasecmp(argv[i]->ptr,"default")) {
            addInfoSectionsToDict(section_dict, defaults);
        } else if (!strcasecmp(argv[i]->ptr,"all")) {
            if (out_all) *out_all = 1;
        } else if (!strcasecmp(argv[i]->ptr,"everything")) {
            if (out_everything) *out_everything = 1;
            if (out_all) *out_all = 1;
        } else {
            sds section = sdsnew(argv[i]->ptr);
            if (dictAdd(section_dict, section, NULL) != DICT_OK)
                sdsfree(section);
        }
    }
    return section_dict;
}

/* Create the string returned by the INFO command. This is decoupled
 * by the INFO command itself as we need to report the same information
 * on memory corruption problems. */
/**创建 INFO 命令返回的字符串。这与 INFO 命令本身分离，因为我们需要报告有关内存损坏问题的相同信息。*/
sds genRedisInfoString(dict *section_dict, int all_sections, int everything) {
    sds info = sdsempty();
    time_t uptime = server.unixtime-server.stat_starttime;
    int j;
    int sections = 0;
    if (everything) all_sections = 1;

    /* Server */
    if (all_sections || (dictFind(section_dict,"server") != NULL)) {
        static int call_uname = 1;
        static struct utsname name;
        char *mode;
        char *supervised;

        if (server.cluster_enabled) mode = "cluster";
        else if (server.sentinel_mode) mode = "sentinel";
        else mode = "standalone";

        if (server.supervised) {
            if (server.supervised_mode == SUPERVISED_UPSTART) supervised = "upstart";
            else if (server.supervised_mode == SUPERVISED_SYSTEMD) supervised = "systemd";
            else supervised = "unknown";
        } else {
            supervised = "no";
        }

        if (sections++) info = sdscat(info,"\r\n");

        if (call_uname) {
            /* Uname can be slow and is always the same output. Cache it. */
            /**Uname 可能很慢，并且始终是相同的输出。缓存它。*/
            uname(&name);
            call_uname = 0;
        }

        unsigned int lruclock;
        atomicGet(server.lruclock,lruclock);
        info = sdscatfmt(info,
            "# Server\r\n"
            "redis_version:%s\r\n"
            "redis_git_sha1:%s\r\n"
            "redis_git_dirty:%i\r\n"
            "redis_build_id:%s\r\n"
            "redis_mode:%s\r\n"
            "os:%s %s %s\r\n"
            "arch_bits:%i\r\n"
            "monotonic_clock:%s\r\n"
            "multiplexing_api:%s\r\n"
            "atomicvar_api:%s\r\n"
            "gcc_version:%i.%i.%i\r\n"
            "process_id:%I\r\n"
            "process_supervised:%s\r\n"
            "run_id:%s\r\n"
            "tcp_port:%i\r\n"
            "server_time_usec:%I\r\n"
            "uptime_in_seconds:%I\r\n"
            "uptime_in_days:%I\r\n"
            "hz:%i\r\n"
            "configured_hz:%i\r\n"
            "lru_clock:%u\r\n"
            "executable:%s\r\n"
            "config_file:%s\r\n"
            "io_threads_active:%i\r\n",
            REDIS_VERSION,
            redisGitSHA1(),
            strtol(redisGitDirty(),NULL,10) > 0,
            redisBuildIdString(),
            mode,
            name.sysname, name.release, name.machine,
            server.arch_bits,
            monotonicInfoString(),
            aeGetApiName(),
            REDIS_ATOMIC_API,
#ifdef __GNUC__
            __GNUC__,__GNUC_MINOR__,__GNUC_PATCHLEVEL__,
#else
            0,0,0,
#endif
            (int64_t) getpid(),
            supervised,
            server.runid,
            server.port ? server.port : server.tls_port,
            (int64_t)server.ustime,
            (int64_t)uptime,
            (int64_t)(uptime/(3600*24)),
            server.hz,
            server.config_hz,
            lruclock,
            server.executable ? server.executable : "",
            server.configfile ? server.configfile : "",
            server.io_threads_active);

        /* Conditional properties 条件属性*/
        if (isShutdownInitiated()) {
            info = sdscatfmt(info,
                "shutdown_in_milliseconds:%I\r\n",
                (int64_t)(server.shutdown_mstime - server.mstime));
        }
    }

    /* Clients */
    if (all_sections || (dictFind(section_dict,"clients") != NULL)) {
        size_t maxin, maxout;
        getExpansiveClientsInfo(&maxin,&maxout);
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Clients\r\n"
            "connected_clients:%lu\r\n"
            "cluster_connections:%lu\r\n"
            "maxclients:%u\r\n"
            "client_recent_max_input_buffer:%zu\r\n"
            "client_recent_max_output_buffer:%zu\r\n"
            "blocked_clients:%d\r\n"
            "tracking_clients:%d\r\n"
            "clients_in_timeout_table:%llu\r\n",
            listLength(server.clients)-listLength(server.slaves),
            getClusterConnectionsCount(),
            server.maxclients,
            maxin, maxout,
            server.blocked_clients,
            server.tracking_clients,
            (unsigned long long) raxSize(server.clients_timeout_table));
    }

    /* Memory */
    if (all_sections || (dictFind(section_dict,"memory") != NULL)) {
        char hmem[64];
        char peak_hmem[64];
        char total_system_hmem[64];
        char used_memory_lua_hmem[64];
        char used_memory_vm_total_hmem[64];
        char used_memory_scripts_hmem[64];
        char used_memory_rss_hmem[64];
        char maxmemory_hmem[64];
        size_t zmalloc_used = zmalloc_used_memory();
        size_t total_system_mem = server.system_memory_size;
        const char *evict_policy = evictPolicyToString();
        long long memory_lua = evalMemory();
        long long memory_functions = functionsMemory();
        struct redisMemOverhead *mh = getMemoryOverheadData();

        /* Peak memory is updated from time to time by serverCron() so it
         * may happen that the instantaneous value is slightly bigger than
         * the peak value. This may confuse users, so we update the peak
         * if found smaller than the current memory usage. */
        /**峰值内存由 serverCron() 不时更新，因此可能会发生瞬时值略大于峰值的情况。
         * 这可能会使用户感到困惑，因此如果发现小于当前内存使用量，我们会更新峰值。*/
        if (zmalloc_used > server.stat_peak_memory)
            server.stat_peak_memory = zmalloc_used;

        bytesToHuman(hmem,zmalloc_used);
        bytesToHuman(peak_hmem,server.stat_peak_memory);
        bytesToHuman(total_system_hmem,total_system_mem);
        bytesToHuman(used_memory_lua_hmem,memory_lua);
        bytesToHuman(used_memory_vm_total_hmem,memory_functions + memory_lua);
        bytesToHuman(used_memory_scripts_hmem,mh->lua_caches + mh->functions_caches);
        bytesToHuman(used_memory_rss_hmem,server.cron_malloc_stats.process_rss);
        bytesToHuman(maxmemory_hmem,server.maxmemory);

        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Memory\r\n"
            "used_memory:%zu\r\n"
            "used_memory_human:%s\r\n"
            "used_memory_rss:%zu\r\n"
            "used_memory_rss_human:%s\r\n"
            "used_memory_peak:%zu\r\n"
            "used_memory_peak_human:%s\r\n"
            "used_memory_peak_perc:%.2f%%\r\n"
            "used_memory_overhead:%zu\r\n"
            "used_memory_startup:%zu\r\n"
            "used_memory_dataset:%zu\r\n"
            "used_memory_dataset_perc:%.2f%%\r\n"
            "allocator_allocated:%zu\r\n"
            "allocator_active:%zu\r\n"
            "allocator_resident:%zu\r\n"
            "total_system_memory:%lu\r\n"
            "total_system_memory_human:%s\r\n"
            "used_memory_lua:%lld\r\n" /* deprecated, renamed to used_memory_vm_eval 已弃用，重命名为 used_memory_vm_eval*/
            "used_memory_vm_eval:%lld\r\n"
            "used_memory_lua_human:%s\r\n" /* deprecated */
            "used_memory_scripts_eval:%lld\r\n"
            "number_of_cached_scripts:%lu\r\n"
            "number_of_functions:%lu\r\n"
            "number_of_libraries:%lu\r\n"
            "used_memory_vm_functions:%lld\r\n"
            "used_memory_vm_total:%lld\r\n"
            "used_memory_vm_total_human:%s\r\n"
            "used_memory_functions:%lld\r\n"
            "used_memory_scripts:%lld\r\n"
            "used_memory_scripts_human:%s\r\n"
            "maxmemory:%lld\r\n"
            "maxmemory_human:%s\r\n"
            "maxmemory_policy:%s\r\n"
            "allocator_frag_ratio:%.2f\r\n"
            "allocator_frag_bytes:%zu\r\n"
            "allocator_rss_ratio:%.2f\r\n"
            "allocator_rss_bytes:%zd\r\n"
            "rss_overhead_ratio:%.2f\r\n"
            "rss_overhead_bytes:%zd\r\n"
            "mem_fragmentation_ratio:%.2f\r\n"
            "mem_fragmentation_bytes:%zd\r\n"
            "mem_not_counted_for_evict:%zu\r\n"
            "mem_replication_backlog:%zu\r\n"
            "mem_total_replication_buffers:%zu\r\n"
            "mem_clients_slaves:%zu\r\n"
            "mem_clients_normal:%zu\r\n"
            "mem_cluster_links:%zu\r\n"
            "mem_aof_buffer:%zu\r\n"
            "mem_allocator:%s\r\n"
            "active_defrag_running:%d\r\n"
            "lazyfree_pending_objects:%zu\r\n"
            "lazyfreed_objects:%zu\r\n",
            zmalloc_used,
            hmem,
            server.cron_malloc_stats.process_rss,
            used_memory_rss_hmem,
            server.stat_peak_memory,
            peak_hmem,
            mh->peak_perc,
            mh->overhead_total,
            mh->startup_allocated,
            mh->dataset,
            mh->dataset_perc,
            server.cron_malloc_stats.allocator_allocated,
            server.cron_malloc_stats.allocator_active,
            server.cron_malloc_stats.allocator_resident,
            (unsigned long)total_system_mem,
            total_system_hmem,
            memory_lua,
            memory_lua,
            used_memory_lua_hmem,
            (long long) mh->lua_caches,
            dictSize(evalScriptsDict()),
            functionsNum(),
            functionsLibNum(),
            memory_functions,
            memory_functions + memory_lua,
            used_memory_vm_total_hmem,
            (long long) mh->functions_caches,
            (long long) mh->lua_caches + (long long) mh->functions_caches,
            used_memory_scripts_hmem,
            server.maxmemory,
            maxmemory_hmem,
            evict_policy,
            mh->allocator_frag,
            mh->allocator_frag_bytes,
            mh->allocator_rss,
            mh->allocator_rss_bytes,
            mh->rss_extra,
            mh->rss_extra_bytes,
            mh->total_frag,       /* This is the total RSS overhead, including
                                     fragmentation, but not just it. This field
                                     (and the next one) is named like that just
                                     for backward compatibility.
                                     这是总的 RSS 开销，包括碎片，但不仅仅是碎片。
                                     这个字段（和下一个）被命名只是为了向后兼容。*/
            mh->total_frag_bytes,
            freeMemoryGetNotCountedMemory(),
            mh->repl_backlog,
            server.repl_buffer_mem,
            mh->clients_slaves,
            mh->clients_normal,
            mh->cluster_links,
            mh->aof_buffer,
            ZMALLOC_LIB,
            server.active_defrag_running,
            lazyfreeGetPendingObjectsCount(),
            lazyfreeGetFreedObjectsCount()
        );
        freeMemoryOverheadData(mh);
    }

    /* Persistence */
    if (all_sections || (dictFind(section_dict,"persistence") != NULL)) {
        if (sections++) info = sdscat(info,"\r\n");
        double fork_perc = 0;
        if (server.stat_module_progress) {
            fork_perc = server.stat_module_progress * 100;
        } else if (server.stat_current_save_keys_total) {
            fork_perc = ((double)server.stat_current_save_keys_processed / server.stat_current_save_keys_total) * 100;
        }
        int aof_bio_fsync_status;
        atomicGet(server.aof_bio_fsync_status,aof_bio_fsync_status);

        info = sdscatprintf(info,
            "# Persistence\r\n"
            "loading:%d\r\n"
            "async_loading:%d\r\n"
            "current_cow_peak:%zu\r\n"
            "current_cow_size:%zu\r\n"
            "current_cow_size_age:%lu\r\n"
            "current_fork_perc:%.2f\r\n"
            "current_save_keys_processed:%zu\r\n"
            "current_save_keys_total:%zu\r\n"
            "rdb_changes_since_last_save:%lld\r\n"
            "rdb_bgsave_in_progress:%d\r\n"
            "rdb_last_save_time:%jd\r\n"
            "rdb_last_bgsave_status:%s\r\n"
            "rdb_last_bgsave_time_sec:%jd\r\n"
            "rdb_current_bgsave_time_sec:%jd\r\n"
            "rdb_saves:%lld\r\n"
            "rdb_last_cow_size:%zu\r\n"
            "rdb_last_load_keys_expired:%lld\r\n"
            "rdb_last_load_keys_loaded:%lld\r\n"
            "aof_enabled:%d\r\n"
            "aof_rewrite_in_progress:%d\r\n"
            "aof_rewrite_scheduled:%d\r\n"
            "aof_last_rewrite_time_sec:%jd\r\n"
            "aof_current_rewrite_time_sec:%jd\r\n"
            "aof_last_bgrewrite_status:%s\r\n"
            "aof_rewrites:%lld\r\n"
            "aof_rewrites_consecutive_failures:%lld\r\n"
            "aof_last_write_status:%s\r\n"
            "aof_last_cow_size:%zu\r\n"
            "module_fork_in_progress:%d\r\n"
            "module_fork_last_cow_size:%zu\r\n",
            (int)(server.loading && !server.async_loading),
            (int)server.async_loading,
            server.stat_current_cow_peak,
            server.stat_current_cow_bytes,
            server.stat_current_cow_updated ? (unsigned long) elapsedMs(server.stat_current_cow_updated) / 1000 : 0,
            fork_perc,
            server.stat_current_save_keys_processed,
            server.stat_current_save_keys_total,
            server.dirty,
            server.child_type == CHILD_TYPE_RDB,
            (intmax_t)server.lastsave,
            (server.lastbgsave_status == C_OK) ? "ok" : "err",
            (intmax_t)server.rdb_save_time_last,
            (intmax_t)((server.child_type != CHILD_TYPE_RDB) ?
                -1 : time(NULL)-server.rdb_save_time_start),
            server.stat_rdb_saves,
            server.stat_rdb_cow_bytes,
            server.rdb_last_load_keys_expired,
            server.rdb_last_load_keys_loaded,
            server.aof_state != AOF_OFF,
            server.child_type == CHILD_TYPE_AOF,
            server.aof_rewrite_scheduled,
            (intmax_t)server.aof_rewrite_time_last,
            (intmax_t)((server.child_type != CHILD_TYPE_AOF) ?
                -1 : time(NULL)-server.aof_rewrite_time_start),
            (server.aof_lastbgrewrite_status == C_OK) ? "ok" : "err",
            server.stat_aof_rewrites,
            server.stat_aofrw_consecutive_failures,
            (server.aof_last_write_status == C_OK &&
                aof_bio_fsync_status == C_OK) ? "ok" : "err",
            server.stat_aof_cow_bytes,
            server.child_type == CHILD_TYPE_MODULE,
            server.stat_module_cow_bytes);

        if (server.aof_enabled) {
            info = sdscatprintf(info,
                "aof_current_size:%lld\r\n"
                "aof_base_size:%lld\r\n"
                "aof_pending_rewrite:%d\r\n"
                "aof_buffer_length:%zu\r\n"
                "aof_pending_bio_fsync:%llu\r\n"
                "aof_delayed_fsync:%lu\r\n",
                (long long) server.aof_current_size,
                (long long) server.aof_rewrite_base_size,
                server.aof_rewrite_scheduled,
                sdslen(server.aof_buf),
                bioPendingJobsOfType(BIO_AOF_FSYNC),
                server.aof_delayed_fsync);
        }

        if (server.loading) {
            double perc = 0;
            time_t eta, elapsed;
            off_t remaining_bytes = 1;

            if (server.loading_total_bytes) {
                perc = ((double)server.loading_loaded_bytes / server.loading_total_bytes) * 100;
                remaining_bytes = server.loading_total_bytes - server.loading_loaded_bytes;
            } else if(server.loading_rdb_used_mem) {
                perc = ((double)server.loading_loaded_bytes / server.loading_rdb_used_mem) * 100;
                remaining_bytes = server.loading_rdb_used_mem - server.loading_loaded_bytes;
                /* used mem is only a (bad) estimation of the rdb file size, avoid going over 100% */
                /**used mem 只是对 rdb 文件大小的（错误）估计，避免超过 100%*/
                if (perc > 99.99) perc = 99.99;
                if (remaining_bytes < 1) remaining_bytes = 1;
            }

            elapsed = time(NULL)-server.loading_start_time;
            if (elapsed == 0) {
                eta = 1; /* A fake 1 second figure if we don't have
                            enough info 如果我们没有足够的信息，一个假的 1 秒数字*/
            } else {
                eta = (elapsed*remaining_bytes)/(server.loading_loaded_bytes+1);
            }

            info = sdscatprintf(info,
                "loading_start_time:%jd\r\n"
                "loading_total_bytes:%llu\r\n"
                "loading_rdb_used_mem:%llu\r\n"
                "loading_loaded_bytes:%llu\r\n"
                "loading_loaded_perc:%.2f\r\n"
                "loading_eta_seconds:%jd\r\n",
                (intmax_t) server.loading_start_time,
                (unsigned long long) server.loading_total_bytes,
                (unsigned long long) server.loading_rdb_used_mem,
                (unsigned long long) server.loading_loaded_bytes,
                perc,
                (intmax_t)eta
            );
        }
    }

    /* Stats */
    if (all_sections  || (dictFind(section_dict,"stats") != NULL)) {
        long long stat_total_reads_processed, stat_total_writes_processed;
        long long stat_net_input_bytes, stat_net_output_bytes;
        long long stat_net_repl_input_bytes, stat_net_repl_output_bytes;
        long long current_eviction_exceeded_time = server.stat_last_eviction_exceeded_time ?
            (long long) elapsedUs(server.stat_last_eviction_exceeded_time): 0;
        long long current_active_defrag_time = server.stat_last_active_defrag_time ?
            (long long) elapsedUs(server.stat_last_active_defrag_time): 0;
        atomicGet(server.stat_total_reads_processed, stat_total_reads_processed);
        atomicGet(server.stat_total_writes_processed, stat_total_writes_processed);
        atomicGet(server.stat_net_input_bytes, stat_net_input_bytes);
        atomicGet(server.stat_net_output_bytes, stat_net_output_bytes);
        atomicGet(server.stat_net_repl_input_bytes, stat_net_repl_input_bytes);
        atomicGet(server.stat_net_repl_output_bytes, stat_net_repl_output_bytes);

        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Stats\r\n"
            "total_connections_received:%lld\r\n"
            "total_commands_processed:%lld\r\n"
            "instantaneous_ops_per_sec:%lld\r\n"
            "total_net_input_bytes:%lld\r\n"
            "total_net_output_bytes:%lld\r\n"
            "total_net_repl_input_bytes:%lld\r\n"
            "total_net_repl_output_bytes:%lld\r\n"
            "instantaneous_input_kbps:%.2f\r\n"
            "instantaneous_output_kbps:%.2f\r\n"
            "instantaneous_input_repl_kbps:%.2f\r\n"
            "instantaneous_output_repl_kbps:%.2f\r\n"
            "rejected_connections:%lld\r\n"
            "sync_full:%lld\r\n"
            "sync_partial_ok:%lld\r\n"
            "sync_partial_err:%lld\r\n"
            "expired_keys:%lld\r\n"
            "expired_stale_perc:%.2f\r\n"
            "expired_time_cap_reached_count:%lld\r\n"
            "expire_cycle_cpu_milliseconds:%lld\r\n"
            "evicted_keys:%lld\r\n"
            "evicted_clients:%lld\r\n"
            "total_eviction_exceeded_time:%lld\r\n"
            "current_eviction_exceeded_time:%lld\r\n"
            "keyspace_hits:%lld\r\n"
            "keyspace_misses:%lld\r\n"
            "pubsub_channels:%ld\r\n"
            "pubsub_patterns:%lu\r\n"
            "pubsubshard_channels:%lu\r\n"
            "latest_fork_usec:%lld\r\n"
            "total_forks:%lld\r\n"
            "migrate_cached_sockets:%ld\r\n"
            "slave_expires_tracked_keys:%zu\r\n"
            "active_defrag_hits:%lld\r\n"
            "active_defrag_misses:%lld\r\n"
            "active_defrag_key_hits:%lld\r\n"
            "active_defrag_key_misses:%lld\r\n"
            "total_active_defrag_time:%lld\r\n"
            "current_active_defrag_time:%lld\r\n"
            "tracking_total_keys:%lld\r\n"
            "tracking_total_items:%lld\r\n"
            "tracking_total_prefixes:%lld\r\n"
            "unexpected_error_replies:%lld\r\n"
            "total_error_replies:%lld\r\n"
            "dump_payload_sanitizations:%lld\r\n"
            "total_reads_processed:%lld\r\n"
            "total_writes_processed:%lld\r\n"
            "io_threaded_reads_processed:%lld\r\n"
            "io_threaded_writes_processed:%lld\r\n"
            "reply_buffer_shrinks:%lld\r\n"
            "reply_buffer_expands:%lld\r\n",
            server.stat_numconnections,
            server.stat_numcommands,
            getInstantaneousMetric(STATS_METRIC_COMMAND),
            stat_net_input_bytes + stat_net_repl_input_bytes,
            stat_net_output_bytes + stat_net_repl_output_bytes,
            stat_net_repl_input_bytes,
            stat_net_repl_output_bytes,
            (float)getInstantaneousMetric(STATS_METRIC_NET_INPUT)/1024,
            (float)getInstantaneousMetric(STATS_METRIC_NET_OUTPUT)/1024,
            (float)getInstantaneousMetric(STATS_METRIC_NET_INPUT_REPLICATION)/1024,
            (float)getInstantaneousMetric(STATS_METRIC_NET_OUTPUT_REPLICATION)/1024,
            server.stat_rejected_conn,
            server.stat_sync_full,
            server.stat_sync_partial_ok,
            server.stat_sync_partial_err,
            server.stat_expiredkeys,
            server.stat_expired_stale_perc*100,
            server.stat_expired_time_cap_reached_count,
            server.stat_expire_cycle_time_used/1000,
            server.stat_evictedkeys,
            server.stat_evictedclients,
            (server.stat_total_eviction_exceeded_time + current_eviction_exceeded_time) / 1000,
            current_eviction_exceeded_time / 1000,
            server.stat_keyspace_hits,
            server.stat_keyspace_misses,
            dictSize(server.pubsub_channels),
            dictSize(server.pubsub_patterns),
            dictSize(server.pubsubshard_channels),
            server.stat_fork_time,
            server.stat_total_forks,
            dictSize(server.migrate_cached_sockets),
            getSlaveKeyWithExpireCount(),
            server.stat_active_defrag_hits,
            server.stat_active_defrag_misses,
            server.stat_active_defrag_key_hits,
            server.stat_active_defrag_key_misses,
            (server.stat_total_active_defrag_time + current_active_defrag_time) / 1000,
            current_active_defrag_time / 1000,
            (unsigned long long) trackingGetTotalKeys(),
            (unsigned long long) trackingGetTotalItems(),
            (unsigned long long) trackingGetTotalPrefixes(),
            server.stat_unexpected_error_replies,
            server.stat_total_error_replies,
            server.stat_dump_payload_sanitizations,
            stat_total_reads_processed,
            stat_total_writes_processed,
            server.stat_io_reads_processed,
            server.stat_io_writes_processed,
            server.stat_reply_buffer_shrinks,
            server.stat_reply_buffer_expands);
    }

    /* Replication */
    if (all_sections || (dictFind(section_dict,"replication") != NULL)) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Replication\r\n"
            "role:%s\r\n",
            server.masterhost == NULL ? "master" : "slave");
        if (server.masterhost) {
            long long slave_repl_offset = 1;
            long long slave_read_repl_offset = 1;

            if (server.master) {
                slave_repl_offset = server.master->reploff;
                slave_read_repl_offset = server.master->read_reploff;
            } else if (server.cached_master) {
                slave_repl_offset = server.cached_master->reploff;
                slave_read_repl_offset = server.cached_master->read_reploff;
            }

            info = sdscatprintf(info,
                "master_host:%s\r\n"
                "master_port:%d\r\n"
                "master_link_status:%s\r\n"
                "master_last_io_seconds_ago:%d\r\n"
                "master_sync_in_progress:%d\r\n"
                "slave_read_repl_offset:%lld\r\n"
                "slave_repl_offset:%lld\r\n"
                ,server.masterhost,
                server.masterport,
                (server.repl_state == REPL_STATE_CONNECTED) ?
                    "up" : "down",
                server.master ?
                ((int)(server.unixtime-server.master->lastinteraction)) : -1,
                server.repl_state == REPL_STATE_TRANSFER,
                slave_read_repl_offset,
                slave_repl_offset
            );

            if (server.repl_state == REPL_STATE_TRANSFER) {
                double perc = 0;
                if (server.repl_transfer_size) {
                    perc = ((double)server.repl_transfer_read / server.repl_transfer_size) * 100;
                }
                info = sdscatprintf(info,
                    "master_sync_total_bytes:%lld\r\n"
                    "master_sync_read_bytes:%lld\r\n"
                    "master_sync_left_bytes:%lld\r\n"
                    "master_sync_perc:%.2f\r\n"
                    "master_sync_last_io_seconds_ago:%d\r\n",
                    (long long) server.repl_transfer_size,
                    (long long) server.repl_transfer_read,
                    (long long) (server.repl_transfer_size - server.repl_transfer_read),
                    perc,
                    (int)(server.unixtime-server.repl_transfer_lastio)
                );
            }

            if (server.repl_state != REPL_STATE_CONNECTED) {
                info = sdscatprintf(info,
                    "master_link_down_since_seconds:%jd\r\n",
                    server.repl_down_since ?
                    (intmax_t)(server.unixtime-server.repl_down_since) : -1);
            }
            info = sdscatprintf(info,
                "slave_priority:%d\r\n"
                "slave_read_only:%d\r\n"
                "replica_announced:%d\r\n",
                server.slave_priority,
                server.repl_slave_ro,
                server.replica_announced);
        }

        info = sdscatprintf(info,
            "connected_slaves:%lu\r\n",
            listLength(server.slaves));

        /* If min-slaves-to-write is active, write the number of slaves
         * currently considered 'good'. */
        /**如果 min-slaves-to-write 处于活动状态，则写入当前认为“好”的从属设备的数量。*/
        if (server.repl_min_slaves_to_write &&
            server.repl_min_slaves_max_lag) {
            info = sdscatprintf(info,
                "min_slaves_good_slaves:%d\r\n",
                server.repl_good_slaves_count);
        }

        if (listLength(server.slaves)) {
            int slaveid = 0;
            listNode *ln;
            listIter li;

            listRewind(server.slaves,&li);
            while((ln = listNext(&li))) {
                client *slave = listNodeValue(ln);
                char ip[NET_IP_STR_LEN], *slaveip = slave->slave_addr;
                int port;
                long lag = 0;

                if (!slaveip) {
                    if (connPeerToString(slave->conn,ip,sizeof(ip),&port) == -1)
                        continue;
                    slaveip = ip;
                }
                const char *state = replstateToString(slave->replstate);
                if (state[0] == '\0') continue;
                if (slave->replstate == SLAVE_STATE_ONLINE)
                    lag = time(NULL) - slave->repl_ack_time;

                info = sdscatprintf(info,
                    "slave%d:ip=%s,port=%d,state=%s,"
                    "offset=%lld,lag=%ld\r\n",
                    slaveid,slaveip,slave->slave_listening_port,state,
                    slave->repl_ack_off, lag);
                slaveid++;
            }
        }
        info = sdscatprintf(info,
            "master_failover_state:%s\r\n"
            "master_replid:%s\r\n"
            "master_replid2:%s\r\n"
            "master_repl_offset:%lld\r\n"
            "second_repl_offset:%lld\r\n"
            "repl_backlog_active:%d\r\n"
            "repl_backlog_size:%lld\r\n"
            "repl_backlog_first_byte_offset:%lld\r\n"
            "repl_backlog_histlen:%lld\r\n",
            getFailoverStateString(),
            server.replid,
            server.replid2,
            server.master_repl_offset,
            server.second_replid_offset,
            server.repl_backlog != NULL,
            server.repl_backlog_size,
            server.repl_backlog ? server.repl_backlog->offset : 0,
            server.repl_backlog ? server.repl_backlog->histlen : 0);
    }

    /* CPU */
    if (all_sections || (dictFind(section_dict,"cpu") != NULL)) {
        if (sections++) info = sdscat(info,"\r\n");

        struct rusage self_ru, c_ru;
        getrusage(RUSAGE_SELF, &self_ru);
        getrusage(RUSAGE_CHILDREN, &c_ru);
        info = sdscatprintf(info,
        "# CPU\r\n"
        "used_cpu_sys:%ld.%06ld\r\n"
        "used_cpu_user:%ld.%06ld\r\n"
        "used_cpu_sys_children:%ld.%06ld\r\n"
        "used_cpu_user_children:%ld.%06ld\r\n",
        (long)self_ru.ru_stime.tv_sec, (long)self_ru.ru_stime.tv_usec,
        (long)self_ru.ru_utime.tv_sec, (long)self_ru.ru_utime.tv_usec,
        (long)c_ru.ru_stime.tv_sec, (long)c_ru.ru_stime.tv_usec,
        (long)c_ru.ru_utime.tv_sec, (long)c_ru.ru_utime.tv_usec);
#ifdef RUSAGE_THREAD
        struct rusage m_ru;
        getrusage(RUSAGE_THREAD, &m_ru);
        info = sdscatprintf(info,
            "used_cpu_sys_main_thread:%ld.%06ld\r\n"
            "used_cpu_user_main_thread:%ld.%06ld\r\n",
            (long)m_ru.ru_stime.tv_sec, (long)m_ru.ru_stime.tv_usec,
            (long)m_ru.ru_utime.tv_sec, (long)m_ru.ru_utime.tv_usec);
#endif  /* RUSAGE_THREAD */
    }

    /* Modules */
    if (all_sections || (dictFind(section_dict,"module_list") != NULL) || (dictFind(section_dict,"modules") != NULL)) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,"# Modules\r\n");
        info = genModulesInfoString(info);
    }

    /* Command statistics */
    if (all_sections || (dictFind(section_dict,"commandstats") != NULL)) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info, "# Commandstats\r\n");
        info = genRedisInfoStringCommandStats(info, server.commands);
    }

    /* Error statistics */
    if (all_sections || (dictFind(section_dict,"errorstats") != NULL)) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscat(info, "# Errorstats\r\n");
        raxIterator ri;
        raxStart(&ri,server.errors);
        raxSeek(&ri,"^",NULL,0);
        struct redisError *e;
        while(raxNext(&ri)) {
            char *tmpsafe;
            e = (struct redisError *) ri.data;
            info = sdscatprintf(info,
                "errorstat_%.*s:count=%lld\r\n",
                (int)ri.key_len, getSafeInfoString((char *) ri.key, ri.key_len, &tmpsafe), e->count);
            if (tmpsafe != NULL) zfree(tmpsafe);
        }
        raxStop(&ri);
    }

    /* Latency by percentile distribution per command */
    /**每个命令按百分位分布的延迟*/
    if (all_sections || (dictFind(section_dict,"latencystats") != NULL)) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info, "# Latencystats\r\n");
        if (server.latency_tracking_enabled) {
            info = genRedisInfoStringLatencyStats(info, server.commands);
        }
    }

    /* Cluster */
    if (all_sections || (dictFind(section_dict,"cluster") != NULL)) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
        "# Cluster\r\n"
        "cluster_enabled:%d\r\n",
        server.cluster_enabled);
    }

    /* Key space */
    if (all_sections || (dictFind(section_dict,"keyspace") != NULL)) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info, "# Keyspace\r\n");
        for (j = 0; j < server.dbnum; j++) {
            long long keys, vkeys;

            keys = dictSize(server.db[j].dict);
            vkeys = dictSize(server.db[j].expires);
            if (keys || vkeys) {
                info = sdscatprintf(info,
                    "db%d:keys=%lld,expires=%lld,avg_ttl=%lld\r\n",
                    j, keys, vkeys, server.db[j].avg_ttl);
            }
        }
    }

    /* Get info from modules.
     * if user asked for "everything" or "modules", or a specific section
     * that's not found yet. */
    /**从模块中获取信息。如果用户要求“一切”或“模块”，或尚未找到的特定部分。*/
    if (everything || dictFind(section_dict, "modules") != NULL || sections < (int)dictSize(section_dict)) {

        info = modulesCollectInfo(info,
                                  everything || dictFind(section_dict, "modules") != NULL ? NULL: section_dict,
                                  0, /* not a crash report */
                                  sections);
    }
    return info;
}

/* INFO [<section> [<section> ...]] */
void infoCommand(client *c) {
    if (server.sentinel_mode) {
        sentinelInfoCommand(c);
        return;
    }
    int all_sections = 0;
    int everything = 0;
    dict *sections_dict = genInfoSectionDict(c->argv+1, c->argc-1, NULL, &all_sections, &everything);
    sds info = genRedisInfoString(sections_dict, all_sections, everything);
    addReplyVerbatim(c,info,sdslen(info),"txt");
    sdsfree(info);
    releaseInfoSectionDict(sections_dict);
    return;
}

void monitorCommand(client *c) {
    if (c->flags & CLIENT_DENY_BLOCKING) {
        /**
         * A client that has CLIENT_DENY_BLOCKING flag on
         * expects a reply per command and so can't execute MONITOR. */
        addReplyError(c, "MONITOR isn't allowed for DENY BLOCKING client");
        return;
    }

    /* ignore MONITOR if already slave or in monitor mode */
    if (c->flags & CLIENT_SLAVE) return;

    c->flags |= (CLIENT_SLAVE|CLIENT_MONITOR);
    listAddNodeTail(server.monitors,c);
    addReply(c,shared.ok);
}

/* =================================== Main! ================================ */

int checkIgnoreWarning(const char *warning) {
    int argc, j;
    sds *argv = sdssplitargs(server.ignore_warnings, &argc);
    if (argv == NULL)
        return 0;

    for (j = 0; j < argc; j++) {
        char *flag = argv[j];
        if (!strcasecmp(flag, warning))
            break;
    }
    sdsfreesplitres(argv,argc);
    return j < argc;
}

#ifdef __linux__
#include <sys/prctl.h>
/* since linux-3.5, kernel supports to set the state of the "THP disable" flag
 * for the calling thread. PR_SET_THP_DISABLE is defined in linux/prctl.h */
/**从 linux-3.5 开始，内核支持为调用线程设置“THP disable”标志的状态。 PR_SET_THP_DISABLE 在 linuxprctl.h 中定义*/
static int THPDisable(void) {
    int ret = -EINVAL;

    if (!server.disable_thp)
        return ret;

#ifdef PR_SET_THP_DISABLE
    ret = prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0);
#endif

    return ret;
}

void linuxMemoryWarnings(void) {
    sds err_msg = NULL;
    if (checkOvercommit(&err_msg) < 0) {
        serverLog(LL_WARNING,"WARNING %s", err_msg);
        sdsfree(err_msg);
    }
    if (checkTHPEnabled(&err_msg) < 0) {
        server.thp_enabled = 1;
        if (THPDisable() == 0) {
            server.thp_enabled = 0;
        } else {
            serverLog(LL_WARNING, "WARNING %s", err_msg);
        }
        sdsfree(err_msg);
    }
}
#endif /* __linux__ */

void createPidFile(void) {
    /* If pidfile requested, but no pidfile defined, use
     * default pidfile path */
    /**如果请求了 pidfile，但未定义 pidfile，则使用默认 pidfile 路径*/
    if (!server.pidfile) server.pidfile = zstrdup(CONFIG_DEFAULT_PID_FILE);

    /* Try to write the pid file in a best-effort way. */
    /**尝试以尽力而为的方式编写 pid 文件。*/
    FILE *fp = fopen(server.pidfile,"w");
    if (fp) {
        fprintf(fp,"%d\n",(int)getpid());
        fclose(fp);
    }
}

void daemonize(void) {
    int fd;

    if (fork() != 0) exit(0); /* parent exits */
    setsid(); /* create a new session */

    /* Every output goes to /dev/null. If Redis is daemonized but
     * the 'logfile' is set to 'stdout' in the configuration file
     * it will not log at all. */
    /**每个输出都转到 devnull。如果 Redis 是守护进程，但配置文件中的“logfile”设置为“stdout”，则它根本不会记录。*/
    if ((fd = open("/dev/null", O_RDWR, 0)) != -1) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
    }
}

void version(void) {
    printf("Redis server v=%s sha=%s:%d malloc=%s bits=%d build=%llx\n",
        REDIS_VERSION,
        redisGitSHA1(),
        atoi(redisGitDirty()) > 0,
        ZMALLOC_LIB,
        sizeof(long) == 4 ? 32 : 64,
        (unsigned long long) redisBuildId());
    exit(0);
}

void usage(void) {
    fprintf(stderr,"Usage: ./redis-server [/path/to/redis.conf] [options] [-]\n");
    fprintf(stderr,"       ./redis-server - (read config from stdin)\n");
    fprintf(stderr,"       ./redis-server -v or --version\n");
    fprintf(stderr,"       ./redis-server -h or --help\n");
    fprintf(stderr,"       ./redis-server --test-memory <megabytes>\n");
    fprintf(stderr,"       ./redis-server --check-system\n");
    fprintf(stderr,"\n");
    fprintf(stderr,"Examples:\n");
    fprintf(stderr,"       ./redis-server (run the server with default conf)\n");
    fprintf(stderr,"       echo 'maxmemory 128mb' | ./redis-server -\n");
    fprintf(stderr,"       ./redis-server /etc/redis/6379.conf\n");
    fprintf(stderr,"       ./redis-server --port 7777\n");
    fprintf(stderr,"       ./redis-server --port 7777 --replicaof 127.0.0.1 8888\n");
    fprintf(stderr,"       ./redis-server /etc/myredis.conf --loglevel verbose -\n");
    fprintf(stderr,"       ./redis-server /etc/myredis.conf --loglevel verbose\n\n");
    fprintf(stderr,"Sentinel mode:\n");
    fprintf(stderr,"       ./redis-server /etc/sentinel.conf --sentinel\n");
    exit(1);
}

void redisAsciiArt(void) {
#include "asciilogo.h"
    char *buf = zmalloc(1024*16);
    char *mode;

    if (server.cluster_enabled) mode = "cluster";
    else if (server.sentinel_mode) mode = "sentinel";
    else mode = "standalone";

    /* Show the ASCII logo if: log file is stdout AND stdout is a
     * tty AND syslog logging is disabled. Also show logo if the user
     * forced us to do so via redis.conf. */
    /**在以下情况下显示 ASCII 徽标：日志文件是标准输出并且标准输出是 tty 并且 syslog 日志记录被禁用。
     * 如果用户通过 redis.conf 强迫我们这样做，也会显示徽标。*/
    int show_logo = ((!server.syslog_enabled &&
                      server.logfile[0] == '\0' &&
                      isatty(fileno(stdout))) ||
                     server.always_show_logo);

    if (!show_logo) {
        serverLog(LL_NOTICE,
            "Running mode=%s, port=%d.",
            mode, server.port ? server.port : server.tls_port
        );
    } else {
        snprintf(buf,1024*16,ascii_logo,
            REDIS_VERSION,
            redisGitSHA1(),
            strtol(redisGitDirty(),NULL,10) > 0,
            (sizeof(long) == 8) ? "64" : "32",
            mode, server.port ? server.port : server.tls_port,
            (long) getpid()
        );
        serverLogRaw(LL_NOTICE|LL_RAW,buf);
    }
    zfree(buf);
}

int changeBindAddr(void) {
    /* Close old TCP and TLS servers */
    closeSocketListeners(&server.ipfd);
    closeSocketListeners(&server.tlsfd);

    /* Bind to the new port */
    if ((server.port != 0 && listenToPort(server.port, &server.ipfd) != C_OK) ||
        (server.tls_port != 0 && listenToPort(server.tls_port, &server.tlsfd) != C_OK)) {
        serverLog(LL_WARNING, "Failed to bind");

        closeSocketListeners(&server.ipfd);
        closeSocketListeners(&server.tlsfd);
        return C_ERR;
    }

    /* Create TCP and TLS event handlers */
    /**创建 TCP 和 TLS 事件处理程序*/
    if (createSocketAcceptHandler(&server.ipfd, acceptTcpHandler) != C_OK) {
        serverPanic("Unrecoverable error creating TCP socket accept handler.");
    }
    if (createSocketAcceptHandler(&server.tlsfd, acceptTLSHandler) != C_OK) {
        serverPanic("Unrecoverable error creating TLS socket accept handler.");
    }

    if (server.set_proc_title) redisSetProcTitle(NULL);

    return C_OK;
}

int changeListenPort(int port, socketFds *sfd, aeFileProc *accept_handler) {
    socketFds new_sfd = {{0}};

    /* Close old servers */
    closeSocketListeners(sfd);

    /* Just close the server if port disabled 如果端口禁用，只需关闭服务器*/
    if (port == 0) {
        if (server.set_proc_title) redisSetProcTitle(NULL);
        return C_OK;
    }

    /* Bind to the new port */
    if (listenToPort(port, &new_sfd) != C_OK) {
        return C_ERR;
    }

    /* Create event handlers */
    if (createSocketAcceptHandler(&new_sfd, accept_handler) != C_OK) {
        closeSocketListeners(&new_sfd);
        return C_ERR;
    }

    /* Copy new descriptors */
    sfd->count = new_sfd.count;
    memcpy(sfd->fd, new_sfd.fd, sizeof(new_sfd.fd));

    if (server.set_proc_title) redisSetProcTitle(NULL);

    return C_OK;
}

static void sigShutdownHandler(int sig) {
    char *msg;

    switch (sig) {
    case SIGINT:
        msg = "Received SIGINT scheduling shutdown...";
        break;
    case SIGTERM:
        msg = "Received SIGTERM scheduling shutdown...";
        break;
    default:
        msg = "Received shutdown signal, scheduling shutdown...";
    };

    /* SIGINT is often delivered via Ctrl+C in an interactive session.
     * If we receive the signal the second time, we interpret this as
     * the user really wanting to quit ASAP without waiting to persist
     * on disk and without waiting for lagging replicas. */
    /**SIGINT 通常在交互式会话中通过 Ctrl+C 传递。如果我们第二次收到该信号，
     * 我们会将其解释为用户真的想尽快退出，而不是等待在磁盘上持久化，也不需要等待滞后的副本。*/
    if (server.shutdown_asap && sig == SIGINT) {
        serverLogFromHandler(LL_WARNING, "You insist... exiting now.");
        rdbRemoveTempFile(getpid(), 1);
        exit(1); /* Exit with an error since this was not a clean shutdown. */
    } else if (server.loading) {
        msg = "Received shutdown signal during loading, scheduling shutdown.";
    }

    serverLogFromHandler(LL_WARNING, msg);
    server.shutdown_asap = 1;
    server.last_sig_received = sig;
}

void setupSignalHandlers(void) {
    struct sigaction act;

    /* When the SA_SIGINFO flag is set in sa_flags then sa_sigaction is used.
     * Otherwise, sa_handler is used. */
    /**如果在 sa_flags 中设置了 SA_SIGINFO 标志，则使用 sa_sigaction。否则，使用 sa_handler。*/
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    act.sa_handler = sigShutdownHandler;
    sigaction(SIGTERM, &act, NULL);
    sigaction(SIGINT, &act, NULL);

    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_NODEFER | SA_RESETHAND | SA_SIGINFO;
    act.sa_sigaction = sigsegvHandler;
    if(server.crashlog_enabled) {
        sigaction(SIGSEGV, &act, NULL);
        sigaction(SIGBUS, &act, NULL);
        sigaction(SIGFPE, &act, NULL);
        sigaction(SIGILL, &act, NULL);
        sigaction(SIGABRT, &act, NULL);
    }
    return;
}

void removeSignalHandlers(void) {
    struct sigaction act;
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_NODEFER | SA_RESETHAND;
    act.sa_handler = SIG_DFL;
    sigaction(SIGSEGV, &act, NULL);
    sigaction(SIGBUS, &act, NULL);
    sigaction(SIGFPE, &act, NULL);
    sigaction(SIGILL, &act, NULL);
    sigaction(SIGABRT, &act, NULL);
}

/* This is the signal handler for children process. It is currently useful
 * in order to track the SIGUSR1, that we send to a child in order to terminate
 * it in a clean way, without the parent detecting an error and stop
 * accepting writes because of a write error condition. */
/**这是子进程的信号处理程序。它目前用于跟踪 SIGUSR1，我们将其发送给子节点以便以干净的方式终止它，
 * 而父节点不会检测到错误并由于写入错误条件而停止接受写入。*/
static void sigKillChildHandler(int sig) {
    UNUSED(sig);
    int level = server.in_fork_child == CHILD_TYPE_MODULE? LL_VERBOSE: LL_WARNING;
    serverLogFromHandler(level, "Received SIGUSR1 in child, exiting now.");
    exitFromChild(SERVER_CHILD_NOERROR_RETVAL);
}

void setupChildSignalHandlers(void) {
    struct sigaction act;

    /* When the SA_SIGINFO flag is set in sa_flags then sa_sigaction is used.
     * Otherwise, sa_handler is used. */
    /**如果在 sa_flags 中设置了 SA_SIGINFO 标志，则使用 sa_sigaction。否则，使用 sa_handler。*/
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    act.sa_handler = sigKillChildHandler;
    sigaction(SIGUSR1, &act, NULL);
}

/* After fork, the child process will inherit the resources
 * of the parent process, e.g. fd(socket or flock) etc.
 * should close the resources not used by the child process, so that if the
 * parent restarts it can bind/lock despite the child possibly still running. */
/**fork 后，子进程会继承父进程的资源，例如fd(socket orflock) 等应该关闭子进程未使用的资源，
 * 这样如果父进程重新启动它可以绑定锁，尽管子进程可能仍在运行。*/
void closeChildUnusedResourceAfterFork() {
    closeListeningSockets(0);
    if (server.cluster_enabled && server.cluster_config_file_lock_fd != -1)
        close(server.cluster_config_file_lock_fd);  /* don't care if this fails 不在乎这是否失败*/

    /* Clear server.pidfile, this is the parent pidfile which should not
     * be touched (or deleted) by the child (on exit / crash) */
    /**清除 server.pidfile，这是父 pidfile，不应被子进程触摸（或删除）（退出崩溃时）*/
    zfree(server.pidfile);
    server.pidfile = NULL;
}

/* purpose is one of CHILD_TYPE_ types 用途是 CHILD_TYPE_ 类型之一*/
int redisFork(int purpose) {
    if (isMutuallyExclusiveChildType(purpose)) {
        if (hasActiveChildProcess()) {
            errno = EEXIST;
            return -1;
        }

        openChildInfoPipe();
    }

    int childpid;
    long long start = ustime();
    if ((childpid = fork()) == 0) {
        /* Child.
         *
         * The order of setting things up follows some reasoning:
         * Setup signal handlers first because a signal could fire at any time.
         * Adjust OOM score before everything else to assist the OOM killer if
         * memory resources are low.
         * 设置的顺序遵循一些推理：
         *   首先设置信号处理程序，因为信号可能随时触发。如果内存资源不足，请先调整 OOM 分数以协助 OOM 杀手。
         */
        server.in_fork_child = purpose;
        setupChildSignalHandlers();
        setOOMScoreAdj(CONFIG_OOM_BGCHILD);
        dismissMemoryInChild();
        closeChildUnusedResourceAfterFork();
    } else {
        /* Parent */
        if (childpid == -1) {
            int fork_errno = errno;
            if (isMutuallyExclusiveChildType(purpose)) closeChildInfoPipe();
            errno = fork_errno;
            return -1;
        }

        server.stat_total_forks++;
        server.stat_fork_time = ustime()-start;
        server.stat_fork_rate = (double) zmalloc_used_memory() * 1000000 / server.stat_fork_time / (1024*1024*1024); /* GB per second. */
        latencyAddSampleIfNeeded("fork",server.stat_fork_time/1000);

        /* The child_pid and child_type are only for mutually exclusive children.
         * other child types should handle and store their pid's in dedicated variables.
         *
         * Today, we allows CHILD_TYPE_LDB to run in parallel with the other fork types:
         * - it isn't used for production, so it will not make the server be less efficient
         * - used for debugging, and we don't want to block it from running while other
         *   forks are running (like RDB and AOF) */
        /**child_pid 和 child_type 仅适用于互斥的孩子。其他子类型应处理并将其 pid 存储在专用变量中。
         * 今天，我们允许 CHILD_TYPE_LDB 与其他 fork 类型并行运行：
         *   - 它不用于生产，因此不会降低服务器效率
         *   - 用于调试，我们不想阻止它运行当其他分叉正在运行时（如 RDB 和 AOF）*/
        if (isMutuallyExclusiveChildType(purpose)) {
            server.child_pid = childpid;
            server.child_type = purpose;
            server.stat_current_cow_peak = 0;
            server.stat_current_cow_bytes = 0;
            server.stat_current_cow_updated = 0;
            server.stat_current_save_keys_processed = 0;
            server.stat_module_progress = 0;
            server.stat_current_save_keys_total = dbTotalServerKeyCount();
        }

        updateDictResizePolicy();
        moduleFireServerEvent(REDISMODULE_EVENT_FORK_CHILD,
                              REDISMODULE_SUBEVENT_FORK_CHILD_BORN,
                              NULL);
    }
    return childpid;
}

void sendChildCowInfo(childInfoType info_type, char *pname) {
    sendChildInfoGeneric(info_type, 0, -1, pname);
}

void sendChildInfo(childInfoType info_type, size_t keys, char *pname) {
    sendChildInfoGeneric(info_type, keys, -1, pname);
}

/* Try to release pages back to the OS directly (bypassing the allocator),
 * in an effort to decrease CoW during fork. For small allocations, we can't
 * release any full page, so in an effort to avoid getting the size of the
 * allocation from the allocator (malloc_size) when we already know it's small,
 * we check the size_hint. If the size is not already known, passing a size_hint
 * of 0 will lead the checking the real size of the allocation.
 * Also please note that the size may be not accurate, so in order to make this
 * solution effective, the judgement for releasing memory pages should not be
 * too strict. */
/**尝试将页面直接释放回操作系统（绕过分配器），以减少分叉期间的 CoW。
 * 对于小的分配，我们不能释放任何完整的页面，所以为了避免在我们已经知道
 * 它很小的情况下从分配器 (malloc_size) 获取分配的大小，我们检查 size_hint。
 * 如果大小未知，传递 0 的 size_hint 将导致检查分配的实际大小。
 * 另外请注意，大小可能不准确，所以为了使这个方案有效，释放内存页的判断不要太严格。*/
void dismissMemory(void* ptr, size_t size_hint) {
    if (ptr == NULL) return;

    /* madvise(MADV_DONTNEED) can not release pages if the size of memory
     * is too small, we try to release only for the memory which the size
     * is more than half of page size. */
    /**如果内存太小，madvise(MADV_DONTNEED) 无法释放页面，我们尝试只释放超过页面大小一半的内存。*/
    if (size_hint && size_hint <= server.page_size/2) return;

    zmadvise_dontneed(ptr);
}

/* Dismiss big chunks of memory inside a client structure, see dismissMemory() */
/**消除客户端结构中的大块内存，请参阅dismissMemory()*/
void dismissClientMemory(client *c) {
    /* Dismiss client query buffer and static reply buffer. */
    /**关闭客户端查询缓冲区和静态回复缓冲区。*/
    dismissMemory(c->buf, c->buf_usable_size);
    dismissSds(c->querybuf);
    /* Dismiss argv array only if we estimate it contains a big buffer. */
    /**仅当我们估计它包含一个大缓冲区时才关闭 argv 数组。*/
    if (c->argc && c->argv_len_sum/c->argc >= server.page_size) {
        for (int i = 0; i < c->argc; i++) {
            dismissObject(c->argv[i], 0);
        }
    }
    if (c->argc) dismissMemory(c->argv, c->argc*sizeof(robj*));

    /* Dismiss the reply array only if the average buffer size is bigger
     * than a page. */
    /**仅当平均缓冲区大小大于一页时才关闭回复数组。*/
    if (listLength(c->reply) &&
        c->reply_bytes/listLength(c->reply) >= server.page_size)
    {
        listIter li;
        listNode *ln;
        listRewind(c->reply, &li);
        while ((ln = listNext(&li))) {
            clientReplyBlock *bulk = listNodeValue(ln);
            /* Default bulk size is 16k, actually it has extra data, maybe it
             * occupies 20k according to jemalloc bin size if using jemalloc. */
            /**默认的bulk size是16k，实际上它有额外的数据，如果使用jemalloc，根据jemalloc bin size可能会占用20k。*/
            if (bulk) dismissMemory(bulk, bulk->size);
        }
    }
}

/* In the child process, we don't need some buffers anymore, and these are
 * likely to change in the parent when there's heavy write traffic.
 * We dismiss them right away, to avoid CoW.
 * see dismissMemeory(). */
/**在子进程中，我们不再需要一些缓冲区，当写入流量很大时，这些缓冲区可能会在父进程中发生变化。
 * 我们立即解雇他们，以避免出现 CoW。见dismissMemeory()。*/
void dismissMemoryInChild(void) {
    /* madvise(MADV_DONTNEED) may not work if Transparent Huge Pages is enabled. */
    /**如果启用了透明大页面，madvise(MADV_DONTNEED) 可能不起作用。*/
    if (server.thp_enabled) return;

    /* Currently we use zmadvise_dontneed only when we use jemalloc with Linux.
     * so we avoid these pointless loops when they're not going to do anything. */
    /**目前，我们仅在将 jemalloc 与 Linux 一起使用时才使用 zmadvise_dontneed。
     * 因此，当它们不打算做任何事情时，我们会避免这些无意义的循环。*/
#if defined(USE_JEMALLOC) && defined(__linux__)
    listIter li;
    listNode *ln;

    /* Dismiss replication buffer. We don't need to separately dismiss replication
     * backlog and replica' output buffer, because they just reference the global
     * replication buffer but don't cost real memory. */
    /**关闭复制缓冲区。我们不需要单独关闭复制积压和副本的输出缓冲区，因为它们只是引用全局复制缓冲区，但不占用实际内存。*/
    listRewind(server.repl_buffer_blocks, &li);
    while((ln = listNext(&li))) {
        replBufBlock *o = listNodeValue(ln);
        dismissMemory(o, o->size);
    }

    /* Dismiss all clients memory. 关闭所有客户端内存。*/
    listRewind(server.clients, &li);
    while((ln = listNext(&li))) {
        client *c = listNodeValue(ln);
        dismissClientMemory(c);
    }
#endif
}

void memtest(size_t megabytes, int passes);

/* Returns 1 if there is --sentinel among the arguments or if
 * executable name contains "redis-sentinel". */
/**如果参数中有 --sentinel 或可执行文件名称包含“redis-sentinel”，则返回 1。*/
int checkForSentinelMode(int argc, char **argv, char *exec_name) {
    if (strstr(exec_name,"redis-sentinel") != NULL) return 1;

    for (int j = 1; j < argc; j++)
        if (!strcmp(argv[j],"--sentinel")) return 1;
    return 0;
}

/* Function called at startup to load RDB or AOF file in memory. */
/**启动时调用的函数，用于在内存中加载 RDB 或 AOF 文件。*/
void loadDataFromDisk(void) {
    long long start = ustime();
    if (server.aof_state == AOF_ON) {
        int ret = loadAppendOnlyFiles(server.aof_manifest);
        if (ret == AOF_FAILED || ret == AOF_OPEN_ERR)
            exit(1);
        if (ret != AOF_NOT_EXIST)
            serverLog(LL_NOTICE, "DB loaded from append only file: %.3f seconds", (float)(ustime()-start)/1000000);
    } else {
        rdbSaveInfo rsi = RDB_SAVE_INFO_INIT;
        errno = 0; /* Prevent a stale value from affecting error checking 防止陈旧的值影响错误检查*/
        int rdb_flags = RDBFLAGS_NONE;
        if (iAmMaster()) {
            /* Master may delete expired keys when loading, we should
             * propagate expire to replication backlog. */
            /**Master 在加载时可能会删除过期的密钥，我们应该将过期传播到复制积压。*/
            createReplicationBacklog();
            rdb_flags |= RDBFLAGS_FEED_REPL;
        }
        if (rdbLoad(server.rdb_filename,&rsi,rdb_flags) == C_OK) {
            serverLog(LL_NOTICE,"DB loaded from disk: %.3f seconds",
                (float)(ustime()-start)/1000000);

            /* Restore the replication ID / offset from the RDB file. */
            /**从 RDB 文件恢复复制 ID 偏移量。*/
            if (rsi.repl_id_is_set &&
                rsi.repl_offset != -1 &&
                /* Note that older implementations may save a repl_stream_db
                 * of -1 inside the RDB file in a wrong way, see more
                 * information in function rdbPopulateSaveInfo. */
                /**请注意，较旧的实现可能会以错误的方式在 RDB 文件中保存 -1 的 repl_stream_db，
                 * 请参阅函数 rdbPopulateSaveInfo 中的更多信息。*/
                rsi.repl_stream_db != -1)
            {
                if (!iAmMaster()) {
                    memcpy(server.replid,rsi.repl_id,sizeof(server.replid));
                    server.master_repl_offset = rsi.repl_offset;
                    /* If this is a replica, create a cached master from this
                     * information, in order to allow partial resynchronizations
                     * with masters. */
                    /**如果这是一个副本，请根据此信息创建一个缓存的主控，以允许与主控进行部分重新同步。*/
                    replicationCacheMasterUsingMyself();
                    selectDb(server.cached_master,rsi.repl_stream_db);
                } else {
                    /* If this is a master, we can save the replication info
                     * as secondary ID and offset, in order to allow replicas
                     * to partial resynchronizations with masters. */
                    /**如果这是主服务器，我们可以将复制信息保存为辅助 ID 和偏移量，以允许副本与主服务器进行部分重新同步。*/
                    memcpy(server.replid2,rsi.repl_id,sizeof(server.replid));
                    server.second_replid_offset = rsi.repl_offset+1;
                    /* Rebase master_repl_offset from rsi.repl_offset. */
                    /**从 rsi.repl_offset 重新设置 master_repl_offset。*/
                    server.master_repl_offset += rsi.repl_offset;
                    serverAssert(server.repl_backlog);
                    server.repl_backlog->offset = server.master_repl_offset -
                              server.repl_backlog->histlen + 1;
                    rebaseReplicationBuffer(rsi.repl_offset);
                    server.repl_no_slaves_since = time(NULL);
                }
            }
        } else if (errno != ENOENT) {
            serverLog(LL_WARNING,"Fatal error loading the DB: %s. Exiting.",strerror(errno));
            exit(1);
        }

        /* We always create replication backlog if server is a master, we need
         * it because we put DELs in it when loading expired keys in RDB, but
         * if RDB doesn't have replication info or there is no rdb, it is not
         * possible to support partial resynchronization, to avoid extra memory
         * of replication backlog, we drop it.
         * 如果服务器是主服务器，我们总是会创建复制积压，我们需要它，因为我们在加载 RDB 中的过期键时将 DEL 放入其中，
         * 但是如果 RDB 没有复制信息或没有 rdb，则无法支持部分重新同步，为了避免复制积压的额外内存，我们将其删除。*/
        if (server.master_repl_offset == 0 && server.repl_backlog)
            freeReplicationBacklog();
    }
}

void redisOutOfMemoryHandler(size_t allocation_size) {
    serverLog(LL_WARNING,"Out Of Memory allocating %zu bytes!",
        allocation_size);
    serverPanic("Redis aborting for OUT OF MEMORY. Allocating %zu bytes!",
        allocation_size);
}

/* Callback for sdstemplate on proc-title-template. See redis.conf for
 * supported variables.
 */
//在 proc-title-template 上回调 sdstemplate。有关支持的变量，请参见 redis.conf。
static sds redisProcTitleGetVariable(const sds varname, void *arg)
{
    if (!strcmp(varname, "title")) {
        return sdsnew(arg);
    } else if (!strcmp(varname, "listen-addr")) {
        if (server.port || server.tls_port)
            return sdscatprintf(sdsempty(), "%s:%u",
                                server.bindaddr_count ? server.bindaddr[0] : "*",
                                server.port ? server.port : server.tls_port);
        else
            return sdscatprintf(sdsempty(), "unixsocket:%s", server.unixsocket);
    } else if (!strcmp(varname, "server-mode")) {
        if (server.cluster_enabled) return sdsnew("[cluster]");
        else if (server.sentinel_mode) return sdsnew("[sentinel]");
        else return sdsempty();
    } else if (!strcmp(varname, "config-file")) {
        return sdsnew(server.configfile ? server.configfile : "-");
    } else if (!strcmp(varname, "port")) {
        return sdscatprintf(sdsempty(), "%u", server.port);
    } else if (!strcmp(varname, "tls-port")) {
        return sdscatprintf(sdsempty(), "%u", server.tls_port);
    } else if (!strcmp(varname, "unixsocket")) {
        return sdsnew(server.unixsocket);
    } else
        return NULL;    /* Unknown variable name */
}

/* Expand the specified proc-title-template string and return a newly
 * allocated sds, or NULL. */
//展开指定的 proc-title-template 字符串并返回一个新分配的 sds，或 NULL。
static sds expandProcTitleTemplate(const char *template, const char *title) {
    sds res = sdstemplate(template, redisProcTitleGetVariable, (void *) title);
    if (!res)
        return NULL;
    return sdstrim(res, " ");
}
/* Validate the specified template, returns 1 if valid or 0 otherwise. */
//验证指定的模板，如果有效则返回 1，否则返回 0。
int validateProcTitleTemplate(const char *template) {
    int ok = 1;
    sds res = expandProcTitleTemplate(template, "");
    if (!res)
        return 0;
    if (sdslen(res) == 0) ok = 0;
    sdsfree(res);
    return ok;
}

int redisSetProcTitle(char *title) {
#ifdef USE_SETPROCTITLE
    if (!title) title = server.exec_argv[0];
    sds proc_title = expandProcTitleTemplate(server.proc_title_template, title);
    if (!proc_title) return C_ERR;  /* Not likely, proc_title_template is validated
 *                                      不太可能，proc_title_template 已验证*/

    setproctitle("%s", proc_title);
    sdsfree(proc_title);
#else
    UNUSED(title);
#endif

    return C_OK;
}

void redisSetCpuAffinity(const char *cpulist) {
#ifdef USE_SETCPUAFFINITY
    setcpuaffinity(cpulist);
#else
    UNUSED(cpulist);
#endif
}

/* Send a notify message to systemd. Returns sd_notify return code which is
 * a positive number on success. */
//向 systemd 发送通知消息。返回 sd_notify 返回码，成功时为正数。
int redisCommunicateSystemd(const char *sd_notify_msg) {
#ifdef HAVE_LIBSYSTEMD
    int ret = sd_notify(0, sd_notify_msg);

    if (ret == 0)
        serverLog(LL_WARNING, "systemd supervision error: NOTIFY_SOCKET not found!");
    else if (ret < 0)
        serverLog(LL_WARNING, "systemd supervision error: sd_notify: %d", ret);
    return ret;
#else
    UNUSED(sd_notify_msg);
    return 0;
#endif
}

/* Attempt to set up upstart supervision. Returns 1 if successful. */
//尝试设置暴发户监管。如果成功则返回 1。
static int redisSupervisedUpstart(void) {
    const char *upstart_job = getenv("UPSTART_JOB");

    if (!upstart_job) {
        serverLog(LL_WARNING,
                "upstart supervision requested, but UPSTART_JOB not found!");
        return 0;
    }

    serverLog(LL_NOTICE, "supervised by upstart, will stop to signal readiness.");
    raise(SIGSTOP);
    unsetenv("UPSTART_JOB");
    return 1;
}

/* Attempt to set up systemd supervision. Returns 1 if successful. */
//尝试设置 systemd 监督。如果成功则返回 1。
static int redisSupervisedSystemd(void) {
#ifndef HAVE_LIBSYSTEMD
    serverLog(LL_WARNING,
            "systemd supervision requested or auto-detected, but Redis is compiled without libsystemd support!");
    return 0;
#else
    if (redisCommunicateSystemd("STATUS=Redis is loading...\n") <= 0)
        return 0;
    serverLog(LL_NOTICE,
        "Supervised by systemd. Please make sure you set appropriate values for TimeoutStartSec and TimeoutStopSec in your service unit.");
    return 1;
#endif
}

int redisIsSupervised(int mode) {
    int ret = 0;

    if (mode == SUPERVISED_AUTODETECT) {
        if (getenv("UPSTART_JOB")) {
            serverLog(LL_VERBOSE, "Upstart supervision detected.");
            mode = SUPERVISED_UPSTART;
        } else if (getenv("NOTIFY_SOCKET")) {
            serverLog(LL_VERBOSE, "Systemd supervision detected.");
            mode = SUPERVISED_SYSTEMD;
        }
    }

    switch (mode) {
        case SUPERVISED_UPSTART:
            ret = redisSupervisedUpstart();
            break;
        case SUPERVISED_SYSTEMD:
            ret = redisSupervisedSystemd();
            break;
        default:
            break;
    }

    if (ret)
        server.supervised_mode = mode;

    return ret;
}

int iAmMaster(void) {
    return ((!server.cluster_enabled && server.masterhost == NULL) ||
            (server.cluster_enabled && nodeIsMaster(server.cluster->myself)));
}

#ifdef REDIS_TEST
#include "testhelp.h"

int __failed_tests = 0;
int __test_num = 0;

/* The flags are the following:
* --accurate:     Runs tests with more iterations.
* --large-memory: Enables tests that consume more than 100mb. */
typedef int redisTestProc(int argc, char **argv, int flags);
struct redisTest {
    char *name;
    redisTestProc *proc;
    int failed;
} redisTests[] = {
    {"ziplist", ziplistTest},
    {"quicklist", quicklistTest},
    {"intset", intsetTest},
    {"zipmap", zipmapTest},
    {"sha1test", sha1Test},
    {"util", utilTest},
    {"endianconv", endianconvTest},
    {"crc64", crc64Test},
    {"zmalloc", zmalloc_test},
    {"sds", sdsTest},
    {"dict", dictTest},
    {"listpack", listpackTest}
};
redisTestProc *getTestProcByName(const char *name) {
    int numtests = sizeof(redisTests)/sizeof(struct redisTest);
    for (int j = 0; j < numtests; j++) {
        if (!strcasecmp(name,redisTests[j].name)) {
            return redisTests[j].proc;
        }
    }
    return NULL;
}
#endif

int main(int argc, char **argv) {

    struct timeval tv;
    int j;
    char config_from_stdin = 0;

#ifdef REDIS_TEST
    if (argc >= 3 && !strcasecmp(argv[1], "test")) {
        int flags = 0;
        for (j = 3; j < argc; j++) {
            char *arg = argv[j];
            if (!strcasecmp(arg, "--accurate")) flags |= REDIS_TEST_ACCURATE;
            else if (!strcasecmp(arg, "--large-memory")) flags |= REDIS_TEST_LARGE_MEMORY;
        }

        if (!strcasecmp(argv[2], "all")) {
            int numtests = sizeof(redisTests)/sizeof(struct redisTest);
            for (j = 0; j < numtests; j++) {
                redisTests[j].failed = (redisTests[j].proc(argc,argv,flags) != 0);
            }

            /* Report tests result */
            int failed_num = 0;
            for (j = 0; j < numtests; j++) {
                if (redisTests[j].failed) {
                    failed_num++;
                    printf("[failed] Test - %s\n", redisTests[j].name);
                } else {
                    printf("[ok] Test - %s\n", redisTests[j].name);
                }
            }

            printf("%d tests, %d passed, %d failed\n", numtests,
                   numtests-failed_num, failed_num);

            return failed_num == 0 ? 0 : 1;
        } else {
            redisTestProc *proc = getTestProcByName(argv[2]);
            if (!proc) return -1; /* test not found */
            return proc(argc,argv,flags);
        }

        return 0;
    }
#endif

    /* We need to initialize our libraries, and the server configuration. */
    //我们需要初始化我们的库和服务器配置。
#ifdef INIT_SETPROCTITLE_REPLACEMENT
    spt_init(argc, argv);
#endif
    setlocale(LC_COLLATE,"");
    tzset(); /* Populates 'timezone' global. */
    zmalloc_set_oom_handler(redisOutOfMemoryHandler);

    /* To achieve entropy, in case of containers, their time() and getpid() can
     * be the same. But value of tv_usec is fast enough to make the difference
     * 为了实现熵，在容器的情况下，它们的 time() 和 getpid() 可以相同。但是 tv_usec 的值足够快，足以产生影响*/
    gettimeofday(&tv,NULL);
    srand(time(NULL)^getpid()^tv.tv_usec);
    srandom(time(NULL)^getpid()^tv.tv_usec);
    init_genrand64(((long long) tv.tv_sec * 1000000 + tv.tv_usec) ^ getpid());
    crc64_init();

    /* Store umask value. Because umask(2) only offers a set-and-get API we have
     * to reset it and restore it back. We do this early to avoid a potential
     * race condition with threads that could be creating files or directories.
     */
    /**
     * 存储 umask 值。因为 umask(2) 只提供了一个 set-and-get API，我们必须重置它并恢复它。
     * 我们尽早这样做是为了避免可能正在创建文件或目录的线程的潜在竞争条件。
     * */
    umask(server.umask = umask(0777));

    uint8_t hashseed[16];
    getRandomBytes(hashseed,sizeof(hashseed));
    dictSetHashFunctionSeed(hashseed);

    char *exec_name = strrchr(argv[0], '/');
    if (exec_name == NULL) exec_name = argv[0];
    server.sentinel_mode = checkForSentinelMode(argc,argv, exec_name);
    initServerConfig();
    ACLInit(); /* The ACL subsystem must be initialized ASAP because the
                  basic networking code and client creation depends on it.
                  ACL 子系统必须尽快初始化，因为基本网络代码和客户端创建依赖于它。*/
    moduleInitModulesSystem();
    tlsInit();

    /* Store the executable path and arguments in a safe place in order
     * to be able to restart the server later. */
    //将可执行路径和参数存储在安全的地方，以便以后能够重新启动服务器。
    server.executable = getAbsolutePath(argv[0]);
    server.exec_argv = zmalloc(sizeof(char*)*(argc+1));
    server.exec_argv[argc] = NULL;
    for (j = 0; j < argc; j++) server.exec_argv[j] = zstrdup(argv[j]);

    /* We need to init sentinel right now as parsing the configuration file
     * in sentinel mode will have the effect of populating the sentinel
     * data structures with master nodes to monitor.
     * 我们现在需要初始化哨兵，因为在哨兵模式下解析配置文件将具有用主节点填充哨兵数据结构以进行监控的效果。*/
    if (server.sentinel_mode) {
        initSentinelConfig();
        initSentinel();
    }

    /* Check if we need to start in redis-check-rdb/aof mode. We just execute
     * the program main. However the program is part of the Redis executable
     * so that we can easily execute an RDB check on loading errors.
     * 检查我们是否需要以 redis-check-rdbaof 模式启动。我们只执行程序 main。
     * 然而，该程序是 Redis 可执行文件的一部分，因此我们可以轻松地对加载错误执行 RDB 检查。*/
    if (strstr(exec_name,"redis-check-rdb") != NULL)
        redis_check_rdb_main(argc,argv,NULL);
    else if (strstr(exec_name,"redis-check-aof") != NULL)
        redis_check_aof_main(argc,argv);

    if (argc >= 2) {
        j = 1; /* First option to parse in argv[] */
        sds options = sdsempty();

        /* Handle special options --help and --version */
        if (strcmp(argv[1], "-v") == 0 ||
            strcmp(argv[1], "--version") == 0) version();
        if (strcmp(argv[1], "--help") == 0 ||
            strcmp(argv[1], "-h") == 0) usage();
        if (strcmp(argv[1], "--test-memory") == 0) {
            if (argc == 3) {
                memtest(atoi(argv[2]),50);
                exit(0);
            } else {
                fprintf(stderr,"Please specify the amount of memory to test in megabytes.\n");
                fprintf(stderr,"Example: ./redis-server --test-memory 4096\n\n");
                exit(1);
            }
        } if (strcmp(argv[1], "--check-system") == 0) {
            exit(syscheck() ? 0 : 1);
        }
        /* Parse command line options
         * Precedence wise, File, stdin, explicit options -- last config is the one that matters.
         *
         * First argument is the config file name? */
        /**
         * 解析命令行选项优先级、文件、标准输入、显式选项——最后一个配置是最重要的。第一个参数是配置文件名？
         * */
        if (argv[1][0] != '-') {
            /* Replace the config file in server.exec_argv with its absolute path. */
            //将 server.exec_argv 中的配置文件替换为其绝对路径。
            server.configfile = getAbsolutePath(argv[1]);
            zfree(server.exec_argv[1]);
            server.exec_argv[1] = zstrdup(server.configfile);
            j = 2; // Skip this arg when parsing options 解析选项时跳过此参数
        }
        sds *argv_tmp;
        int argc_tmp;
        int handled_last_config_arg = 1;
        while(j < argc) {
            /* Either first or last argument - Should we read config from stdin? */
            //第一个或最后一个参数 - 我们应该从标准输入读取配置吗？
            if (argv[j][0] == '-' && argv[j][1] == '\0' && (j == 1 || j == argc-1)) {
                config_from_stdin = 1;
            }
            /* All the other options are parsed and conceptually appended to the
             * configuration file. For instance --port 6380 will generate the
             * string "port 6380\n" to be parsed after the actual config file
             * and stdin input are parsed (if they exist).
             * Only consider that if the last config has at least one argument.
             * 所有其他选项都被解析并在概念上附加到配置文件中。
             * 例如 --port 6380 将生成字符串“port 6380\n”在实际配置文件和标准输入被解析（如果它们存在）之后被解析。
             * 仅考虑如果最后一个配置至少有一个参数。*/
            else if (handled_last_config_arg && argv[j][0] == '-' && argv[j][1] == '-') {
                /* Option name */
                if (sdslen(options)) options = sdscat(options,"\n");
                /* argv[j]+2 for removing the preceding `--` */
                //argv[j]+2 用于删除前面的 `--`
                options = sdscat(options,argv[j]+2);
                options = sdscat(options," ");

                argv_tmp = sdssplitargs(argv[j], &argc_tmp);
                if (argc_tmp == 1) {
                    /* Means that we only have one option name, like --port or "--port " */
                    //意味着我们只有一个选项名称，例如 --port 或 "--port "
                    handled_last_config_arg = 0;

                    if ((j != argc-1) && argv[j+1][0] == '-' && argv[j+1][1] == '-' &&
                        !strcasecmp(argv[j], "--save"))
                    {
                        /* Special case: handle some things like `--save --config value`.
                         * In this case, if next argument starts with `--`, we will reset
                         * handled_last_config_arg flag and append an empty "" config value
                         * to the options, so it will become `--save "" --config value`.
                         * We are doing it to be compatible with pre 7.0 behavior (which we
                         * break it in #10660, 7.0.1), since there might be users who generate
                         * a command line from an array and when it's empty that's what they produce.
                         * 特殊情况：处理一些诸如 `--save --config value` 之类的事情。
                         * 在这种情况下，如果下一个参数以`--`开头，我们将重置handled_last_config_arg标志
                         * 并将一个空的“”配置值附加到选项中，因此它将成为`--save“”--config值`。
                         * 我们这样做是为了与 7.0 之前的行为（我们在 10660、7.0.1 中打破它）兼容，
                         * 因为可能会有用户从数组生成命令行，而当它为空时，这就是他们生成的内容。*/
                        options = sdscat(options, "\"\"");
                        handled_last_config_arg = 1;
                    }
                    else if ((j == argc-1) && !strcasecmp(argv[j], "--save")) {
                        /* Special case: when empty save is the last argument.
                         * In this case, we append an empty "" config value to the options,
                         * so it will become `--save ""` and will follow the same reset thing.
                         * 特殊情况：当空保存是最后一个参数时。
                         * 在这种情况下，我们将一个空的 "" 配置值附加到选项中，因此它将变为 `--save ""`
                         * 并且将遵循相同的重置操作。*/
                        options = sdscat(options, "\"\"");
                    }
                } else {
                    /* Means that we are passing both config name and it's value in the same arg,
                     * like "--port 6380", so we need to reset handled_last_config_arg flag. */
                    //意味着我们在同一个 arg 中传递了配置名称和它的值，例如“--port 6380”，所以我们需要重置handled_last_config_arg 标志。
                    handled_last_config_arg = 1;
                }
                sdsfreesplitres(argv_tmp, argc_tmp);
            } else {
                /* Option argument */
                options = sdscatrepr(options,argv[j],strlen(argv[j]));
                options = sdscat(options," ");
                handled_last_config_arg = 1;
            }
            j++;
        }

        loadServerConfig(server.configfile, config_from_stdin, options);
        if (server.sentinel_mode) loadSentinelConfigFromQueue();
        sdsfree(options);
    }
    if (server.sentinel_mode) sentinelCheckConfigFile();
    server.supervised = redisIsSupervised(server.supervised_mode);
    int background = server.daemonize && !server.supervised;
    if (background) daemonize();

    serverLog(LL_WARNING, "oO0OoO0OoO0Oo Redis is starting oO0OoO0OoO0Oo");
    serverLog(LL_WARNING,
        "Redis version=%s, bits=%d, commit=%s, modified=%d, pid=%d, just started",
            REDIS_VERSION,
            (sizeof(long) == 8) ? 64 : 32,
            redisGitSHA1(),
            strtol(redisGitDirty(),NULL,10) > 0,
            (int)getpid());

    if (argc == 1) {
        serverLog(LL_WARNING, "Warning: no config file specified, using the default config. In order to specify a config file use %s /path/to/redis.conf", argv[0]);
    } else {
        serverLog(LL_WARNING, "Configuration loaded");
    }

    initServer();
    if (background || server.pidfile) createPidFile();
    if (server.set_proc_title) redisSetProcTitle(NULL);
    redisAsciiArt();
    checkTcpBacklogSettings();

    if (!server.sentinel_mode) {
        /* Things not needed when running in Sentinel mode. */
        serverLog(LL_WARNING,"Server initialized");
    #ifdef __linux__
        linuxMemoryWarnings();
        sds err_msg = NULL;
        if (checkXenClocksource(&err_msg) < 0) {
            serverLog(LL_WARNING, "WARNING %s", err_msg);
            sdsfree(err_msg);
        }
    #if defined (__arm64__)
        int ret;
        if ((ret = checkLinuxMadvFreeForkBug(&err_msg)) <= 0) {
            if (ret < 0) {
                serverLog(LL_WARNING, "WARNING %s", err_msg);
                sdsfree(err_msg);
            } else
                serverLog(LL_WARNING, "Failed to test the kernel for a bug that could lead to data corruption during background save. "
                                      "Your system could be affected, please report this error.");
            if (!checkIgnoreWarning("ARM64-COW-BUG")) {
                serverLog(LL_WARNING,"Redis will now exit to prevent data corruption. "
                                     "Note that it is possible to suppress this warning by setting the following config: ignore-warnings ARM64-COW-BUG");
                exit(1);
            }
        }
    #endif /* __arm64__ */
    #endif /* __linux__ */
        moduleInitModulesSystemLast();
        moduleLoadFromQueue();
        ACLLoadUsersAtStartup();
        InitServerLast();
        aofLoadManifestFromDisk();
        loadDataFromDisk();
        aofOpenIfNeededOnServerStart();
        aofDelHistoryFiles();
        if (server.cluster_enabled) {
            if (verifyClusterConfigWithData() == C_ERR) {
                serverLog(LL_WARNING,
                    "You can't have keys in a DB different than DB 0 when in "
                    "Cluster mode. Exiting.");
                exit(1);
            }
        }
        if (server.ipfd.count > 0 || server.tlsfd.count > 0)
            serverLog(LL_NOTICE,"Ready to accept connections");
        if (server.sofd > 0)
            serverLog(LL_NOTICE,"The server is now ready to accept connections at %s", server.unixsocket);
        if (server.supervised_mode == SUPERVISED_SYSTEMD) {
            if (!server.masterhost) {
                redisCommunicateSystemd("STATUS=Ready to accept connections\n");
            } else {
                redisCommunicateSystemd("STATUS=Ready to accept connections in read-only mode. Waiting for MASTER <-> REPLICA sync\n");
            }
            redisCommunicateSystemd("READY=1\n");
        }
    } else {
        ACLLoadUsersAtStartup();
        InitServerLast();
        sentinelIsRunning();
        if (server.supervised_mode == SUPERVISED_SYSTEMD) {
            redisCommunicateSystemd("STATUS=Ready to accept connections\n");
            redisCommunicateSystemd("READY=1\n");
        }
    }

    /* Warning the user about suspicious maxmemory setting. */
    //警告用户有关可疑的最大内存设置。
    if (server.maxmemory > 0 && server.maxmemory < 1024*1024) {
        serverLog(LL_WARNING,"WARNING: You specified a maxmemory value that is less than 1MB (current value is %llu bytes). Are you sure this is what you really want?", server.maxmemory);
    }

    redisSetCpuAffinity(server.server_cpulist);
    setOOMScoreAdj(-1);

    aeMain(server.el);
    aeDeleteEventLoop(server.el);
    return 0;
}

/* The End */
