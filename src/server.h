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

#ifndef __REDIS_H
#define __REDIS_H

#include "fmacros.h"
#include "config.h"
#include "solarisfixes.h"
#include "rio.h"
#include "atomicvar.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <syslog.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <lua.h>
#include <signal.h>
#include "hdr_histogram.h"

#ifdef HAVE_LIBSYSTEMD
#include <systemd/sd-daemon.h>
#endif

#ifndef static_assert
#define static_assert(expr, lit) extern char __static_assert_failure[(expr) ? 1:-1]
#endif

typedef long long mstime_t; /* millisecond time type.毫秒时间类型。 */
typedef long long ustime_t; /* microsecond time type. 微秒时间类型。*/

#include "ae.h"      /* Event driven programming library */
#include "sds.h"     /* Dynamic safe strings */
#include "dict.h"    /* Hash tables */
#include "adlist.h"  /* Linked lists */
#include "zmalloc.h" /* total memory usage aware version of malloc/free */
#include "anet.h"    /* Networking the easy way */
#include "intset.h"  /* Compact integer set structure */
#include "version.h" /* Version macro */
#include "util.h"    /* Misc functions useful in many places */
#include "latency.h" /* Latency monitor API */
#include "sparkline.h" /* ASCII graphs API */
#include "quicklist.h"  /* Lists are encoded as linked lists of
                           N-elements flat arrays */
#include "rax.h"     /* Radix tree */
#include "connection.h" /* Connection abstraction */

#define REDISMODULE_CORE 1
#include "redismodule.h"    /* Redis modules API defines. */

/* Following includes allow test functions to be called from Redis main() */
//以下包括允许从 Redis main() 调用测试函数
#include "zipmap.h"
#include "ziplist.h" /* Compact list data structure */
#include "sha1.h"
#include "endianconv.h"
#include "crc64.h"

/* min/max */
#undef min
#undef max
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))

/* Error codes */
#define C_OK                    0
#define C_ERR                   -1

/* Static server configuration */
#define CONFIG_DEFAULT_HZ        10             /* Time interrupt calls/sec. 时间中断调用秒。*/
#define CONFIG_MIN_HZ            1
#define CONFIG_MAX_HZ            500
#define MAX_CLIENTS_PER_CLOCK_TICK 200          /* HZ is adapted based on that. HZ 在此基础上进行了改编。 */
#define CRON_DBS_PER_CALL 16
#define NET_MAX_WRITES_PER_EVENT (1024*64)
#define PROTO_SHARED_SELECT_CMDS 10
#define OBJ_SHARED_INTEGERS 10000
#define OBJ_SHARED_BULKHDR_LEN 32
#define OBJ_SHARED_HDR_STRLEN(_len_) (((_len_) < 10) ? 4 : 5) /* see shared.mbulkhdr etc. 请参阅 shared.mbulkhdr 等。*/
#define LOG_MAX_LEN    1024 /* Default maximum length of syslog messages.系统日志消息的默认最大长度。*/
#define AOF_REWRITE_ITEMS_PER_CMD 64
#define AOF_ANNOTATION_LINE_MAX_LEN 1024
#define CONFIG_RUN_ID_SIZE 40
#define RDB_EOF_MARK_SIZE 40
#define CONFIG_REPL_BACKLOG_MIN_SIZE (1024*16)          /* 16k */
#define CONFIG_BGSAVE_RETRY_DELAY 5 /* Wait a few secs before trying again. 等待几秒钟后再重试。*/
#define CONFIG_DEFAULT_PID_FILE "/var/run/redis.pid"
#define CONFIG_DEFAULT_BINDADDR_COUNT 2
#define CONFIG_DEFAULT_BINDADDR { "*", "-::*" }
#define NET_HOST_STR_LEN 256 /* Longest valid hostname 最长有效主机名*/
#define NET_IP_STR_LEN 46 /* INET6_ADDRSTRLEN is 46, but we need to be sure. INET6_ADDRSTRLEN 是 46，但我们需要确定*/
#define NET_ADDR_STR_LEN (NET_IP_STR_LEN+32) /* Must be enough for ip:port. ip:port 必须够用*/
#define NET_HOST_PORT_STR_LEN (NET_HOST_STR_LEN+32) /* Must be enough for hostname:port. 主机名：端口必须足够*/
#define CONFIG_BINDADDR_MAX 16
#define CONFIG_MIN_RESERVED_FDS 32
#define CONFIG_DEFAULT_PROC_TITLE_TEMPLATE "{title} {listen-addr} {server-mode}"

/* Bucket sizes for client eviction pools. Each bucket stores clients with
 * memory usage of up to twice the size of the bucket below it. */
//客户端驱逐池的桶大小。每个存储桶存储的客户端的内存使用量最多是其下方存储桶大小的两倍。
#define CLIENT_MEM_USAGE_BUCKET_MIN_LOG 15 /* Bucket sizes start at up to 32KB (2^15)存储桶大小最高为 32KB (2^15) */
#define CLIENT_MEM_USAGE_BUCKET_MAX_LOG 33 /* Bucket for largest clients: sizes above 4GB (2^32) 最大客户的存储桶：大小超过 4GB (2^32)*/
#define CLIENT_MEM_USAGE_BUCKETS (1+CLIENT_MEM_USAGE_BUCKET_MAX_LOG-CLIENT_MEM_USAGE_BUCKET_MIN_LOG)

#define ACTIVE_EXPIRE_CYCLE_SLOW 0
#define ACTIVE_EXPIRE_CYCLE_FAST 1

/* Children process will exit with this status code to signal that the
 * process terminated without an error: this is useful in order to kill
 * a saving child (RDB or AOF one), without triggering in the parent the
 * write protection that is normally turned on on write errors.
 * Usually children that are terminated with SIGUSR1 will exit with this
 * special code. */
/**子进程将退出并带有此状态码以表示进程终止而没有错误：
 * 这对于杀死正在保存的子进程（RDB 或 AOF 之一）很有用，而不会在父进程中触发通常在写入时打开的写保护错误。
 * 通常，以 SIGUSR1 终止的子进程将使用此特殊代码退出。*/
#define SERVER_CHILD_NOERROR_RETVAL    255

/* Reading copy-on-write info is sometimes expensive and may slow down child
 * processes that report it continuously. We measure the cost of obtaining it
 * and hold back additional reading based on this factor. */
/**读取写时复制信息有时会很昂贵，并且可能会减慢连续报告它的子进程。
 * 我们衡量获得它的成本，并根据这个因素阻止额外的阅读。*/
#define CHILD_COW_DUTY_CYCLE           100

/* Instantaneous metrics tracking. 即时指标跟踪。*/
#define STATS_METRIC_SAMPLES 16     /* Number of samples per metric. 每个指标的样本数。*/
#define STATS_METRIC_COMMAND 0      /* Number of commands executed. 执行的命令数。*/
#define STATS_METRIC_NET_INPUT 1    /* Bytes read to network. 读取到网络的字节。*/
#define STATS_METRIC_NET_OUTPUT 2   /* Bytes written to network.写入网络的字节。 */
#define STATS_METRIC_NET_INPUT_REPLICATION 3   /* Bytes read to network during replication. 复制期间读取到网络的字节数。*/
#define STATS_METRIC_NET_OUTPUT_REPLICATION 4   /* Bytes written to network during replication. */
#define STATS_METRIC_COUNT 5

/* Protocol and I/O related defines 协议和 IO 相关定义*/
#define PROTO_IOBUF_LEN         (1024*16)  /* Generic I/O buffer size 通用 IO 缓冲区大小*/
#define PROTO_REPLY_CHUNK_BYTES (16*1024) /* 16k output buffer 16k 输出缓冲器 */
#define PROTO_INLINE_MAX_SIZE   (1024*64) /* Max size of inline reads 内联读取的最大大小*/
#define PROTO_MBULK_BIG_ARG     (1024*32)
#define PROTO_RESIZE_THRESHOLD  (1024*32) /* Threshold for determining whether to resize query buffer
 *                                            判断是否调整查询缓冲区大小的阈值*/
#define PROTO_REPLY_MIN_BYTES   (1024) /* the lower limit on reply buffer size 回复缓冲区大小的下限*/
#define REDIS_AUTOSYNC_BYTES (1024*1024*4) /* Sync file every 4MB. 每 4MB 同步文件。 */

#define REPLY_BUFFER_DEFAULT_PEAK_RESET_TIME 5000 /* 5 seconds */

/* When configuring the server eventloop, we setup it so that the total number
 * of file descriptors we can handle are server.maxclients + RESERVED_FDS +
 * a few more to stay safe. Since RESERVED_FDS defaults to 32, we add 96
 * in order to make sure of not over provisioning more than 128 fds. */
/**在配置服务器事件循环时，我们对其进行设置，以便我们可以处理的文件描述符总数为
 * server.maxclients + RESERVED_FDS + 更多以保持安全。
 * 由于 RESERVED_FDS 默认为 32，我们添加 96 以确保不会过度配置超过 128 个 fd。*/
#define CONFIG_FDSET_INCR (CONFIG_MIN_RESERVED_FDS+96)

/* OOM Score Adjustment classes. OOM 分数调整分类。*/
#define CONFIG_OOM_MASTER 0
#define CONFIG_OOM_REPLICA 1
#define CONFIG_OOM_BGCHILD 2
#define CONFIG_OOM_COUNT 3

extern int configOOMScoreAdjValuesDefaults[CONFIG_OOM_COUNT];

/* Hash table parameters 哈希表参数*/
#define HASHTABLE_MIN_FILL        10      /* Minimal hash table fill 10% 最小哈希表填充 10% */
#define HASHTABLE_MAX_LOAD_FACTOR 1.618   /* Maximum hash table load factor. 最大哈希表加载因子。*/

/* Command flags. Please check the definition of struct redisCommand in this file
 * for more information about the meaning of every flag. */
//命令标志。请检查此文件中 struct redisCommand 的定义，以获取有关每个标志含义的更多信息。
#define CMD_WRITE (1ULL<<0)
#define CMD_READONLY (1ULL<<1)
#define CMD_DENYOOM (1ULL<<2)
#define CMD_MODULE (1ULL<<3)           /* Command exported by module. 模块导出的命令。*/
#define CMD_ADMIN (1ULL<<4)
#define CMD_PUBSUB (1ULL<<5)
#define CMD_NOSCRIPT (1ULL<<6)
#define CMD_BLOCKING (1ULL<<8)       /* Has potential to block.有阻挡的潜力。 */
#define CMD_LOADING (1ULL<<9)
#define CMD_STALE (1ULL<<10)
#define CMD_SKIP_MONITOR (1ULL<<11)
#define CMD_SKIP_SLOWLOG (1ULL<<12)
#define CMD_ASKING (1ULL<<13)
#define CMD_FAST (1ULL<<14)
#define CMD_NO_AUTH (1ULL<<15)
#define CMD_MAY_REPLICATE (1ULL<<16)
#define CMD_SENTINEL (1ULL<<17)
#define CMD_ONLY_SENTINEL (1ULL<<18)
#define CMD_NO_MANDATORY_KEYS (1ULL<<19)
#define CMD_PROTECTED (1ULL<<20)
#define CMD_MODULE_GETKEYS (1ULL<<21)  /* Use the modules getkeys interface.使用模块 getkeys 接口。 */
#define CMD_MODULE_NO_CLUSTER (1ULL<<22) /* Deny on Redis Cluster.在 Redis 集群上拒绝。 */
#define CMD_NO_ASYNC_LOADING (1ULL<<23)
#define CMD_NO_MULTI (1ULL<<24)
#define CMD_MOVABLE_KEYS (1ULL<<25) /* The legacy range spec doesn't cover all keys.
                                     * Populated by populateCommandLegacyRangeSpec.
                                     * 旧版范围规范并未涵盖所有键。
                                     * 由 populateCommandLegacyRangeSpec 填充。*/
#define CMD_ALLOW_BUSY ((1ULL<<26))
#define CMD_MODULE_GETCHANNELS (1ULL<<27)  /* Use the modules getchannels interface.
                                            使用模块 getchannels 接口。*/

/* Command flags that describe ACLs categories. 描述 ACL 类别的命令标志。*/
#define ACL_CATEGORY_KEYSPACE (1ULL<<0)
#define ACL_CATEGORY_READ (1ULL<<1)
#define ACL_CATEGORY_WRITE (1ULL<<2)
#define ACL_CATEGORY_SET (1ULL<<3)
#define ACL_CATEGORY_SORTEDSET (1ULL<<4)
#define ACL_CATEGORY_LIST (1ULL<<5)
#define ACL_CATEGORY_HASH (1ULL<<6)
#define ACL_CATEGORY_STRING (1ULL<<7)
#define ACL_CATEGORY_BITMAP (1ULL<<8)
#define ACL_CATEGORY_HYPERLOGLOG (1ULL<<9)
#define ACL_CATEGORY_GEO (1ULL<<10)
#define ACL_CATEGORY_STREAM (1ULL<<11)
#define ACL_CATEGORY_PUBSUB (1ULL<<12)
#define ACL_CATEGORY_ADMIN (1ULL<<13)
#define ACL_CATEGORY_FAST (1ULL<<14)
#define ACL_CATEGORY_SLOW (1ULL<<15)
#define ACL_CATEGORY_BLOCKING (1ULL<<16)
#define ACL_CATEGORY_DANGEROUS (1ULL<<17)
#define ACL_CATEGORY_CONNECTION (1ULL<<18)
#define ACL_CATEGORY_TRANSACTION (1ULL<<19)
#define ACL_CATEGORY_SCRIPTING (1ULL<<20)

/* Key-spec flags *
 * -------------- */
/* The following refer what the command actually does with the value or metadata
 * of the key, and not necessarily the user data or how it affects it.
 * Each key-spec may must have exactly one of these. Any operation that's not
 * distinctly deletion, overwrite or read-only would be marked as RW. */
/**Key-spec flags --------------
 * 以下是指命令对键的值或我所做的实际操作，而不一定是用户数据或它如何影响它。
 * 每个关键规范可能必须恰好具有其中之一。任何明显删除、覆盖或只读的操作都将被标记为 RW。*/
#define CMD_KEY_RO (1ULL<<0)     /* Read-Only - Reads the value of the key, but
                                  * doesn't necessarily returns it.只读 - 读取 k 的值不一定会返回它。 */
#define CMD_KEY_RW (1ULL<<1)     /* Read-Write - Modifies the data stored in the
                                  * value of the key or its metadata. 读写 - 修改密钥的数据存储值或其元数据。*/
#define CMD_KEY_OW (1ULL<<2)     /* Overwrite - Overwrites the data stored in
                                  * the value of the key. 覆盖 - 覆盖键的值。*/
#define CMD_KEY_RM (1ULL<<3)     /* Deletes the key. 删除密钥。*/
/* The following refer to user data inside the value of the key, not the metadata
 * like LRU, type, cardinality. It refers to the logical operation on the user's
 * data (actual input strings / TTL), being used / returned / copied / changed,
 * It doesn't refer to modification or returning of metadata (like type, count,
 * presence of data). Any write that's not INSERT or DELETE, would be an UPDATE.
 * Each key-spec may have one of the writes with or without access, or none: */
/**以下是指键值内的用户数据，而不是 LRU、类型、基数之类的元数据。
 * 它是指对用户数据（实际输入字符串TTL）的逻辑操作，被使用返回复制更改，
 * 它不是指修改或返回元数据（如类型，计数，数据存在）。
 * 任何不是 INSERT 或 DELETE 的写入都将是 UPDATE。
 * 每个 key-spec 可能有一个有或没有访问权限的写入，或者没有：*/
#define CMD_KEY_ACCESS (1ULL<<4) /* Returns, copies or uses the user data from
                                  * the value of the key.返回、复制或使用键的值。 */
#define CMD_KEY_UPDATE (1ULL<<5) /* Updates data to the value, new value may
                                  * depend on the old value. 将数据更新为值，n 取决于旧值。*/
#define CMD_KEY_INSERT (1ULL<<6) /* Adds data to the value with no chance of
                                  * modification or deletion of existing data.
                                  * 将数据添加到值中，而不会修改或删除现有数据。*/
#define CMD_KEY_DELETE (1ULL<<7) /* Explicitly deletes some content
                                  * from the value of the key. 从键的值中显式删除一些内容。*/
/* Other flags: */
#define CMD_KEY_NOT_KEY (1ULL<<8)     /* A 'fake' key that should be routed
                                       * like a key in cluster mode but is 
                                       * excluded from other key checks.
                                       * 一个“假”密钥，应该像集群模式中的密钥一样被路由，
                                       * 但被排除在其他密钥检查之外。*/
#define CMD_KEY_INCOMPLETE (1ULL<<9)  /* Means that the keyspec might not point
                                       * out to all keys it should cover
                                       * 意味着 keyspec 可能不会针对它应该覆盖的所有键*/
#define CMD_KEY_VARIABLE_FLAGS (1ULL<<10)  /* Means that some keys might have
                                            * different flags depending on arguments
                                            * 意味着某些键可能具有不同的标志，具体取决于参数*/

/* Key flags for when access type is unknown 访问类型未知时的关键标志*/
#define CMD_KEY_FULL_ACCESS (CMD_KEY_RW | CMD_KEY_ACCESS | CMD_KEY_UPDATE)

/* Channel flags share the same flag space as the key flags */
//通道标志与键标志共享相同的标志空间
#define CMD_CHANNEL_PATTERN (1ULL<<11)     /* The argument is a channel pattern 参数是通道模式*/
#define CMD_CHANNEL_SUBSCRIBE (1ULL<<12)   /* The command subscribes to channels 该命令订阅频道*/
#define CMD_CHANNEL_UNSUBSCRIBE (1ULL<<13) /* The command unsubscribes to channels 该命令取消订阅频道*/
#define CMD_CHANNEL_PUBLISH (1ULL<<14)     /* The command publishes to channels.该命令发布到频道。 */

/* AOF states AOF 状态*/
#define AOF_OFF 0             /* AOF is off */
#define AOF_ON 1              /* AOF is on */
#define AOF_WAIT_REWRITE 2    /* AOF waits rewrite to start appending AOF 等待重写开始追加*/

/* AOF return values for loadAppendOnlyFiles() and loadSingleAppendOnlyFile() */
//loadAppendOnlyFiles() 和 loadSingleAppendOnlyFile() 的 AOF 返回值
#define AOF_OK 0
#define AOF_NOT_EXIST 1
#define AOF_EMPTY 2
#define AOF_OPEN_ERR 3
#define AOF_FAILED 4
#define AOF_TRUNCATED 5

/* Command doc flags */
#define CMD_DOC_NONE 0
#define CMD_DOC_DEPRECATED (1<<0) /* Command is deprecated 命令已弃用*/
#define CMD_DOC_SYSCMD (1<<1) /* System (internal) command 系统（内部）命令 */

/* Client flags */
#define CLIENT_SLAVE (1<<0)   /* This client is a replica 此客户端是副本*/
#define CLIENT_MASTER (1<<1)  /* This client is a master */
#define CLIENT_MONITOR (1<<2) /* This client is a slave monitor, see MONITOR */
#define CLIENT_MULTI (1<<3)   /* This client is in a MULTI context 此客户端处于 MULTI 上下文中*/
#define CLIENT_BLOCKED (1<<4) /* The client is waiting in a blocking operation 客户端正在等待阻塞操作*/
#define CLIENT_DIRTY_CAS (1<<5) /* Watched keys modified. EXEC will fail.监视键已修改。执行将失败。 */
#define CLIENT_CLOSE_AFTER_REPLY (1<<6) /* Close after writing entire reply.写完整个回复后关闭。 */
#define CLIENT_UNBLOCKED (1<<7) /* This client was unblocked and is stored in
                                  server.unblocked_clients 此客户端已解锁服务器。unblocked_clients*/
#define CLIENT_SCRIPT (1<<8) /* This is a non connected client used by Lua 这是 Lua 使用的非连接客户端*/
#define CLIENT_ASKING (1<<9)     /* Client issued the ASKING command 客户端发出 ASKING 命令*/
#define CLIENT_CLOSE_ASAP (1<<10)/* Close this client ASAP尽快关闭此客户端 */
#define CLIENT_UNIX_SOCKET (1<<11) /* Client connected via Unix domain socket 通过 Unix 域套接字连接的客户端*/
#define CLIENT_DIRTY_EXEC (1<<12)  /* EXEC will fail for errors while queueing 排队时 EXEC 将因错误而失败*/
#define CLIENT_MASTER_FORCE_REPLY (1<<13)  /* Queue replies even if is master 队列回复即使是master*/
#define CLIENT_FORCE_AOF (1<<14)   /* Force AOF propagation of current cmd. 强制当前 cmd 的 AOF 传播。*/
#define CLIENT_FORCE_REPL (1<<15)  /* Force replication of current cmd. 强制复制当前 cmd。*/
#define CLIENT_PRE_PSYNC (1<<16)   /* Instance don't understand PSYNC.实例不理解 PSYNC。 */
#define CLIENT_READONLY (1<<17)    /* Cluster client is in read-only state.集群客户端处于只读状态。 */
#define CLIENT_PUBSUB (1<<18)      /* Client is in Pub/Sub mode. 客户端处于 PubSub 模式。*/
#define CLIENT_PREVENT_AOF_PROP (1<<19)  /* Don't propagate to AOF. 不要传播到 AOF。*/
#define CLIENT_PREVENT_REPL_PROP (1<<20)  /* Don't propagate to slaves. 不要传播给slaves*/
#define CLIENT_PREVENT_PROP (CLIENT_PREVENT_AOF_PROP|CLIENT_PREVENT_REPL_PROP)
#define CLIENT_PENDING_WRITE (1<<21) /* Client has output to send but a write
                               handler is yet not installed. 客户端有要发送的输出，但尚未安装 w 处理程序。*/
#define CLIENT_REPLY_OFF (1<<22)   /* Don't send replies to client. 不要向客户发送回复。*/
#define CLIENT_REPLY_SKIP_NEXT (1<<23)  /* Set CLIENT_REPLY_SKIP for next cmd 为下一个 cmd 设置 CLIENT_REPLY_SKIP*/
#define CLIENT_REPLY_SKIP (1<<24)  /* Don't send just this reply. 不要只发送这个回复。*/
#define CLIENT_LUA_DEBUG (1<<25)  /* Run EVAL in debug mode. 在调试模式下运行 EVAL。*/
#define CLIENT_LUA_DEBUG_SYNC (1<<26)  /* EVAL debugging without fork() 没有 fork() 的 EVAL 调试*/
#define CLIENT_MODULE (1<<27) /* Non connected client used by some module.某些模块使用的未连接客户端。 */
#define CLIENT_PROTECTED (1<<28) /* Client should not be freed for now. 客户端现在不应该被释放。*/
/* #define CLIENT_... (1<<29) currently unused, feel free to use in the future */
//定义 CLIENT_... (1<<29) 当前未使用，以后可以随意使用
#define CLIENT_PENDING_COMMAND (1<<30) /* Indicates the client has a fully
                                        * parsed command ready for execution.
                                        * 指示客户端已准备好执行完全解析的命令。*/
#define CLIENT_TRACKING (1ULL<<31) /* Client enabled keys tracking in order to
                                   perform client side caching.
                                   启用客户端的密钥跟踪执行客户端缓存。*/
#define CLIENT_TRACKING_BROKEN_REDIR (1ULL<<32) /* Target client is invalid. 目标客户端无效。*/
#define CLIENT_TRACKING_BCAST (1ULL<<33) /* Tracking in BCAST mode. 在 BCAST 模式下跟踪。*/
#define CLIENT_TRACKING_OPTIN (1ULL<<34)  /* Tracking in opt-in mode.在选择加入模式下进行跟踪。 */
#define CLIENT_TRACKING_OPTOUT (1ULL<<35) /* Tracking in opt-out mode.以退出模式跟踪。 */
#define CLIENT_TRACKING_CACHING (1ULL<<36) /* CACHING yes/no was given,
                                              depending on optin/optout mode.
                                              根据 optinoptout 模式，给出了 CACHING yesno。*/
#define CLIENT_TRACKING_NOLOOP (1ULL<<37) /* Don't send invalidation messages
                                             about writes performed by myself.
                                             不要发送关于我自己执行的写入的无效消息。*/
#define CLIENT_IN_TO_TABLE (1ULL<<38) /* This client is in the timeout table.此客户端在超时表中。 */
#define CLIENT_PROTOCOL_ERROR (1ULL<<39) /* Protocol error chatting with it.与它聊天的协议错误。 */
#define CLIENT_CLOSE_AFTER_COMMAND (1ULL<<40) /* Close after executing commands
                                               * and writing entire reply.执行命令并写入整个回复后关闭。 */
#define CLIENT_DENY_BLOCKING (1ULL<<41) /* Indicate that the client should not be blocked.
                                           currently, turned on inside MULTI, Lua, RM_Call,
                                           and AOF client
                                           指示不应阻止客户端。目前，在 MULTI、Lua、RM_Call、和 AOF 客户端*/
#define CLIENT_REPL_RDBONLY (1ULL<<42) /* This client is a replica that only wants
                                          RDB without replication buffer.
                                          这个客户端是一个只有 RDB 没有复制缓冲区的副本。*/
#define CLIENT_NO_EVICT (1ULL<<43) /* This client is protected against client
                                      memory eviction. 这个客户端是保护内存驱逐。*/

/* Client block type (btype field in client structure)
 * if CLIENT_BLOCKED flag is set. */
//客户端块类型（客户端结构中的 btype 字段） 如果设置了 CLIENT_BLOCKED 标志。
#define BLOCKED_NONE 0    /* Not blocked, no CLIENT_BLOCKED flag set.未阻塞，未设置 CLIENT_BLOCKED 标志。 */
#define BLOCKED_LIST 1    /* BLPOP & co. */
#define BLOCKED_WAIT 2    /* WAIT for synchronous replication.等待同步复制。 */
#define BLOCKED_MODULE 3  /* Blocked by a loadable module. 被可加载模块阻止。*/
#define BLOCKED_STREAM 4  /* XREAD. */
#define BLOCKED_ZSET 5    /* BZPOP et al. */
#define BLOCKED_POSTPONE 6 /* Blocked by processCommand, re-try processing later. 被 processCommand 阻塞，稍后重试处理。*/
#define BLOCKED_SHUTDOWN 7 /* SHUTDOWN. */
#define BLOCKED_NUM 8      /* Number of blocked states. 阻塞状态的数量。*/

/* Client request types */
#define PROTO_REQ_INLINE 1
#define PROTO_REQ_MULTIBULK 2

/* Client classes for client limits, currently used only for
 * the max-client-output-buffer limit implementation. */
//客户端限制的客户端类，当前仅使用 max-client-output-buffer 限制实现。
#define CLIENT_TYPE_NORMAL 0 /* Normal req-reply clients + MONITORs 正常的请求回复客户端 + MONITORs*/
#define CLIENT_TYPE_SLAVE 1  /* Slaves. */
#define CLIENT_TYPE_PUBSUB 2 /* Clients subscribed to PubSub channels. 订阅 PubSub 频道的客户。*/
#define CLIENT_TYPE_MASTER 3 /* Master. */
#define CLIENT_TYPE_COUNT 4  /* Total number of client types.客户端类型的总数。 */
#define CLIENT_TYPE_OBUF_COUNT 3 /* Number of clients to expose to output
                                    buffer configuration. Just the first
                                    three: normal, slave, pubsub.
                                    公开给输出缓冲区配置的客户端数量。
                                    只是前三个：normal, slave, pubsub*/

/* Slave replication state. Used in server.repl_state for slaves to remember
 * what to do next. */
//从属复制状态。在 server.repl_state 中用于从站记住下一步该做什么。
typedef enum {
    REPL_STATE_NONE = 0,            /* No active replication 没有主动复制*/
    REPL_STATE_CONNECT,             /* Must connect to master 必须连接到master*/
    REPL_STATE_CONNECTING,          /* Connecting to master */
    /* --- Handshake states, must be ordered --- */
    REPL_STATE_RECEIVE_PING_REPLY,  /* Wait for PING reply */
    REPL_STATE_SEND_HANDSHAKE,      /* Send handshake sequence to master */
    REPL_STATE_RECEIVE_AUTH_REPLY,  /* Wait for AUTH reply */
    REPL_STATE_RECEIVE_PORT_REPLY,  /* Wait for REPLCONF reply */
    REPL_STATE_RECEIVE_IP_REPLY,    /* Wait for REPLCONF reply */
    REPL_STATE_RECEIVE_CAPA_REPLY,  /* Wait for REPLCONF reply */
    REPL_STATE_SEND_PSYNC,          /* Send PSYNC */
    REPL_STATE_RECEIVE_PSYNC_REPLY, /* Wait for PSYNC reply */
    /* --- End of handshake states --- */
    REPL_STATE_TRANSFER,        /* Receiving .rdb from master */
    REPL_STATE_CONNECTED,       /* Connected to master */
} repl_state;

/* The state of an in progress coordinated failover */
//正在进行的协调故障转移的状态
typedef enum {
    NO_FAILOVER = 0,        /* No failover in progress 没有正在进行的故障转移*/
    FAILOVER_WAIT_FOR_SYNC, /* Waiting for target replica to catch up 等待目标副本赶上*/
    FAILOVER_IN_PROGRESS    /* Waiting for target replica to accept
                             * PSYNC FAILOVER request. 等待目标副本接受 PSYNC FAILOVER 请求。*/
} failover_state;

/* State of slaves from the POV of the master. Used in client->replstate.
 * In SEND_BULK and ONLINE state the slave receives new updates
 * in its output queue. In the WAIT_BGSAVE states instead the server is waiting
 * to start the next background saving in order to send updates to it. */
/**来自master的 POV 的slaves状态。在客户端->replstate 中使用。
 * 在 SEND_BULK 和 ONLINE 状态下，slave 在其输出队列中接收新的更新。
 * 相反，在 WAIT_BGSAVE 状态下，服务器正在等待开始下一次后台保存，以便向其发送更新。*/
#define SLAVE_STATE_WAIT_BGSAVE_START 6 /* We need to produce a new RDB file. 我们需要生成一个新的 RDB 文件。*/
#define SLAVE_STATE_WAIT_BGSAVE_END 7 /* Waiting RDB file creation to finish. 等待 RDB 文件创建完成。*/
#define SLAVE_STATE_SEND_BULK 8 /* Sending RDB file to slave.将 RDB 文件发送到从站。 */
#define SLAVE_STATE_ONLINE 9 /* RDB file transmitted, sending just updates. 传输 RDB 文件，仅发送更新。*/

/* Slave capabilities. 从属能力。*/
#define SLAVE_CAPA_NONE 0
#define SLAVE_CAPA_EOF (1<<0)    /* Can parse the RDB EOF streaming format.可以解析 RDB EOF 流格式。 */
#define SLAVE_CAPA_PSYNC2 (1<<1) /* Supports PSYNC2 protocol. */

/* Slave requirements */
#define SLAVE_REQ_NONE 0
#define SLAVE_REQ_RDB_EXCLUDE_DATA (1 << 0)      /* Exclude data from RDB */
#define SLAVE_REQ_RDB_EXCLUDE_FUNCTIONS (1 << 1) /* Exclude functions from RDB */
/* Mask of all bits in the slave requirements bitfield that represent non-standard (filtered) RDB requirements */
//从站要求位域中所有位的掩码，这些位表示非标准（过滤的）RDB 要求
#define SLAVE_REQ_RDB_MASK (SLAVE_REQ_RDB_EXCLUDE_DATA | SLAVE_REQ_RDB_EXCLUDE_FUNCTIONS)

/* Synchronous read timeout - slave side */
//同步读取超时 - 从端
#define CONFIG_REPL_SYNCIO_TIMEOUT 5

/* The default number of replication backlog blocks to trim per call. */
//每次调用要修剪的复制积压块的默认数量。
#define REPL_BACKLOG_TRIM_BLOCKS_PER_CALL 64

/* In order to quickly find the requested offset for PSYNC requests,
 * we index some nodes in the replication buffer linked list into a rax. */
//为了快速找到 PSYNC 请求的请求偏移量，我们将复制缓冲区链表中的一些节点索引到一个 rax 中。
#define REPL_BACKLOG_INDEX_PER_BLOCKS 64

/* List related stuff 列出相关的东西*/
#define LIST_HEAD 0
#define LIST_TAIL 1
#define ZSET_MIN 0
#define ZSET_MAX 1

/* Sort operations */
#define SORT_OP_GET 0

/* Log levels */
#define LL_DEBUG 0
#define LL_VERBOSE 1
#define LL_NOTICE 2
#define LL_WARNING 3
#define LL_RAW (1<<10) /* Modifier to log without timestamp 不带时间戳记录的修饰符*/

/* Supervision options 监督选项*/
#define SUPERVISED_NONE 0
#define SUPERVISED_AUTODETECT 1
#define SUPERVISED_SYSTEMD 2
#define SUPERVISED_UPSTART 3

/* Anti-warning macro... */
#define UNUSED(V) ((void) V)

#define ZSKIPLIST_MAXLEVEL 32 /* Should be enough for 2^64 elements */
#define ZSKIPLIST_P 0.25      /* Skiplist P = 1/4 */

/* Append only defines */
#define AOF_FSYNC_NO 0
#define AOF_FSYNC_ALWAYS 1
#define AOF_FSYNC_EVERYSEC 2

/* Replication diskless load defines 复制无盘负载定义*/
#define REPL_DISKLESS_LOAD_DISABLED 0
#define REPL_DISKLESS_LOAD_WHEN_DB_EMPTY 1
#define REPL_DISKLESS_LOAD_SWAPDB 2

/* TLS Client Authentication */
#define TLS_CLIENT_AUTH_NO 0
#define TLS_CLIENT_AUTH_YES 1
#define TLS_CLIENT_AUTH_OPTIONAL 2

/* Sanitize dump payload 清理转储有效负载*/
#define SANITIZE_DUMP_NO 0
#define SANITIZE_DUMP_YES 1
#define SANITIZE_DUMP_CLIENTS 2

/* Enable protected config/command */
#define PROTECTED_ACTION_ALLOWED_NO 0
#define PROTECTED_ACTION_ALLOWED_YES 1
#define PROTECTED_ACTION_ALLOWED_LOCAL 2

/* Sets operations codes */
#define SET_OP_UNION 0
#define SET_OP_DIFF 1
#define SET_OP_INTER 2

/* oom-score-adj defines */
#define OOM_SCORE_ADJ_NO 0
#define OOM_SCORE_RELATIVE 1
#define OOM_SCORE_ADJ_ABSOLUTE 2

/* Redis maxmemory strategies. Instead of using just incremental number
 * for this defines, we use a set of flags so that testing for certain
 * properties common to multiple policies is faster. */
/**Redis 最大内存策略。我们使用一组标志而不是仅使用增量数字来定义，以便更快地测试多个策略共有的某些属性。*/
#define MAXMEMORY_FLAG_LRU (1<<0)
#define MAXMEMORY_FLAG_LFU (1<<1)
#define MAXMEMORY_FLAG_ALLKEYS (1<<2)
#define MAXMEMORY_FLAG_NO_SHARED_INTEGERS \
    (MAXMEMORY_FLAG_LRU|MAXMEMORY_FLAG_LFU)

#define MAXMEMORY_VOLATILE_LRU ((0<<8)|MAXMEMORY_FLAG_LRU)
#define MAXMEMORY_VOLATILE_LFU ((1<<8)|MAXMEMORY_FLAG_LFU)
#define MAXMEMORY_VOLATILE_TTL (2<<8)
#define MAXMEMORY_VOLATILE_RANDOM (3<<8)
#define MAXMEMORY_ALLKEYS_LRU ((4<<8)|MAXMEMORY_FLAG_LRU|MAXMEMORY_FLAG_ALLKEYS)
#define MAXMEMORY_ALLKEYS_LFU ((5<<8)|MAXMEMORY_FLAG_LFU|MAXMEMORY_FLAG_ALLKEYS)
#define MAXMEMORY_ALLKEYS_RANDOM ((6<<8)|MAXMEMORY_FLAG_ALLKEYS)
#define MAXMEMORY_NO_EVICTION (7<<8)

/* Units */
#define UNIT_SECONDS 0
#define UNIT_MILLISECONDS 1

/* SHUTDOWN flags */
#define SHUTDOWN_NOFLAGS 0      /* No flags. */
#define SHUTDOWN_SAVE 1         /* Force SAVE on SHUTDOWN even if no save
                                   points are configured.
                                   即使没有配置保存点，也要在 SHUTDOWN 时强制保存。*/
#define SHUTDOWN_NOSAVE 2       /* Don't SAVE on SHUTDOWN. */
#define SHUTDOWN_NOW 4          /* Don't wait for replicas to catch up. 不要等待复制品赶上来。*/
#define SHUTDOWN_FORCE 8        /* Don't let errors prevent shutdown. 不要让错误阻止关机。*/

/* Command call flags, see call() function */
//命令调用标志，见 call() 函数
#define CMD_CALL_NONE 0
#define CMD_CALL_SLOWLOG (1<<0)
#define CMD_CALL_STATS (1<<1)
#define CMD_CALL_PROPAGATE_AOF (1<<2)
#define CMD_CALL_PROPAGATE_REPL (1<<3)
#define CMD_CALL_PROPAGATE (CMD_CALL_PROPAGATE_AOF|CMD_CALL_PROPAGATE_REPL)
#define CMD_CALL_FULL (CMD_CALL_SLOWLOG | CMD_CALL_STATS | CMD_CALL_PROPAGATE)
#define CMD_CALL_FROM_MODULE (1<<4)  /* From RM_Call */

/* Command propagation flags, see propagateNow() function */
//命令传播标志，参见propagateNow() 函数
#define PROPAGATE_NONE 0
#define PROPAGATE_AOF 1
#define PROPAGATE_REPL 2

/* Client pause types, larger types are more restrictive
 * pause types than smaller pause types. */
//客户端暂停类型，较大类型的暂停类型比较小的暂停类型更具限制性。
typedef enum {
    CLIENT_PAUSE_OFF = 0, /* Pause no commands */
    CLIENT_PAUSE_WRITE,   /* Pause write commands */
    CLIENT_PAUSE_ALL      /* Pause all commands */
} pause_type;

/* Client pause purposes. Each purpose has its own end time and pause type. */
//客户端暂停目的。每个目的都有自己的结束时间和暂停类型。
typedef enum {
    PAUSE_BY_CLIENT_COMMAND = 0,
    PAUSE_DURING_SHUTDOWN,
    PAUSE_DURING_FAILOVER,
    NUM_PAUSE_PURPOSES /* This value is the number of purposes above. 这个值就是上面的目的数。*/
} pause_purpose;

typedef struct {
    pause_type type;
    mstime_t end;
} pause_event;

/* Ways that a clusters endpoint can be described */
//可以描述集群端点的方式
typedef enum {
    CLUSTER_ENDPOINT_TYPE_IP = 0,          /* Show IP address */
    CLUSTER_ENDPOINT_TYPE_HOSTNAME,        /* Show hostname */
    CLUSTER_ENDPOINT_TYPE_UNKNOWN_ENDPOINT /* Show NULL or empty */
} cluster_endpoint_type;

/* RDB active child save type. */
#define RDB_CHILD_TYPE_NONE 0
#define RDB_CHILD_TYPE_DISK 1     /* RDB is written to disk. */
#define RDB_CHILD_TYPE_SOCKET 2   /* RDB is written to slave socket. */

/* Keyspace changes notification classes. Every class is associated with a
 * character for configuration purposes. */
//键空间更改通知类。出于配置目的，每个类都与一个字符相关联。
#define NOTIFY_KEYSPACE (1<<0)    /* K */
#define NOTIFY_KEYEVENT (1<<1)    /* E */
#define NOTIFY_GENERIC (1<<2)     /* g */
#define NOTIFY_STRING (1<<3)      /* $ */
#define NOTIFY_LIST (1<<4)        /* l */
#define NOTIFY_SET (1<<5)         /* s */
#define NOTIFY_HASH (1<<6)        /* h */
#define NOTIFY_ZSET (1<<7)        /* z */
#define NOTIFY_EXPIRED (1<<8)     /* x */
#define NOTIFY_EVICTED (1<<9)     /* e */
#define NOTIFY_STREAM (1<<10)     /* t */
#define NOTIFY_KEY_MISS (1<<11)   /* m (Note: This one is excluded from NOTIFY_ALL on purpose)
 *                                     注意：这一项被故意排除在 NOTIFY_ALL 之外）*/
#define NOTIFY_LOADED (1<<12)     /* module only key space notification, indicate a key loaded from rdb
 *                                   仅模块键空间通知，表示从 rdb 加载的键*/
#define NOTIFY_MODULE (1<<13)     /* d, module key space notification */
#define NOTIFY_NEW (1<<14)        /* n, new key notification */
#define NOTIFY_ALL (NOTIFY_GENERIC | NOTIFY_STRING | NOTIFY_LIST | NOTIFY_SET | NOTIFY_HASH | NOTIFY_ZSET | NOTIFY_EXPIRED | NOTIFY_EVICTED | NOTIFY_STREAM | NOTIFY_MODULE) /* A flag */

/* Using the following macro you can run code inside serverCron() with the
 * specified period, specified in milliseconds.
 * The actual resolution depends on server.hz. */
/**使用以下宏，您可以在 serverCron() 中以指定的时间段（以毫秒为单位）运行代码。实际分辨率取决于 server.hz。*/
#define run_with_period(_ms_) if ((_ms_ <= 1000/server.hz) || !(server.cronloops%((_ms_)/(1000/server.hz))))

/* We can print the stacktrace, so our assert is defined this way: */
//我们可以打印堆栈跟踪，所以我们的断言是这样定义的：
#define serverAssertWithInfo(_c,_o,_e) ((_e)?(void)0 : (_serverAssertWithInfo(_c,_o,#_e,__FILE__,__LINE__),redis_unreachable()))
#define serverAssert(_e) ((_e)?(void)0 : (_serverAssert(#_e,__FILE__,__LINE__),redis_unreachable()))
#define serverPanic(...) _serverPanic(__FILE__,__LINE__,__VA_ARGS__),redis_unreachable()

/* latency histogram per command init settings 每个命令初始化设置的延迟直方图*/
#define LATENCY_HISTOGRAM_MIN_VALUE 1L        /* >= 1 nanosec */
#define LATENCY_HISTOGRAM_MAX_VALUE 1000000000L  /* <= 1 secs */
#define LATENCY_HISTOGRAM_PRECISION 2  /* Maintain a value precision of 2 significant digits across LATENCY_HISTOGRAM_MIN_VALUE and LATENCY_HISTOGRAM_MAX_VALUE range.
                                        * Value quantization within the range will thus be no larger than 1/100th (or 1%) of any value.
                                        * The total size per histogram should sit around 40 KiB Bytes.
                                        * 在 LATENCY_HISTOGRAM_MIN_VALUE 和 LATENCY_HISTOGRAM_MAX_VALUE 范围内保持 2 位有效数字的值精度。
                                        * 因此，该范围内的值量化将不大于任何值的 1100（或 1%）。
                                        * 每个直方图的总大小应约为 40 KiB 字节。*/

/* Busy module flags, see busy_module_yield_flags 繁忙的模块标志，见busy_module_yield_flags*/
#define BUSY_MODULE_YIELD_NONE (0)
#define BUSY_MODULE_YIELD_EVENTS (1<<0)
#define BUSY_MODULE_YIELD_CLIENTS (1<<1)

/*-----------------------------------------------------------------------------
 * Data types
 *----------------------------------------------------------------------------*/

/* A redis object, that is a type able to hold a string / list / set */

/* The actual Redis Object */
#define OBJ_STRING 0    /* String object. */
#define OBJ_LIST 1      /* List object. */
#define OBJ_SET 2       /* Set object. */
#define OBJ_ZSET 3      /* Sorted set object. */
#define OBJ_HASH 4      /* Hash object. */

/* The "module" object type is a special one that signals that the object
 * is one directly managed by a Redis module. In this case the value points
 * to a moduleValue struct, which contains the object value (which is only
 * handled by the module itself) and the RedisModuleType struct which lists
 * function pointers in order to serialize, deserialize, AOF-rewrite and
 * free the object.
 *
 * Inside the RDB file, module types are encoded as OBJ_MODULE followed
 * by a 64 bit module type ID, which has a 54 bits module-specific signature
 * in order to dispatch the loading to the right module, plus a 10 bits
 * encoding version. */
/**"module"对象类型是一种特殊的对象类型，它表明该对象是由 Redis 模块直接管理的对象。
 * 在这种情况下，值指向一个 moduleValue 结构，该结构包含对象值（仅由模块本身处理）和 RedisModuleType 结构，
 * 该结构列出了函数指针，以便序列化、反序列化、AOF 重写和释放对象。
 * 在 RDB 文件中，模块类型编码为 OBJ_MODULE 后跟 64 位模块类型 ID，它具有 54 位模块特定签名，
 * 以便将加载分派到正确的模块，加上 10 位编码版本。*/
#define OBJ_MODULE 5    /* Module object. */
#define OBJ_STREAM 6    /* Stream object. */

/* Extract encver / signature from a module type ID. */
//从模块类型 ID 中提取封装签名。
#define REDISMODULE_TYPE_ENCVER_BITS 10
#define REDISMODULE_TYPE_ENCVER_MASK ((1<<REDISMODULE_TYPE_ENCVER_BITS)-1)
#define REDISMODULE_TYPE_ENCVER(id) (id & REDISMODULE_TYPE_ENCVER_MASK)
#define REDISMODULE_TYPE_SIGN(id) ((id & ~((uint64_t)REDISMODULE_TYPE_ENCVER_MASK)) >>REDISMODULE_TYPE_ENCVER_BITS)

/* Bit flags for moduleTypeAuxSaveFunc */
#define REDISMODULE_AUX_BEFORE_RDB (1<<0)
#define REDISMODULE_AUX_AFTER_RDB (1<<1)

struct RedisModule;
struct RedisModuleIO;
struct RedisModuleDigest;
struct RedisModuleCtx;
struct moduleLoadQueueEntry;
struct redisObject;
struct RedisModuleDefragCtx;
struct RedisModuleInfoCtx;
struct RedisModuleKeyOptCtx;
struct RedisModuleCommand;

/* Each module type implementation should export a set of methods in order
 * to serialize and deserialize the value in the RDB file, rewrite the AOF
 * log, create the digest for "DEBUG DIGEST", and free the value when a key
 * is deleted. */
/**每个模块类型实现都应该导出一组方法，以便序列化和反序列化 RDB 文件中的值，重写 AOF 日志，
 * 为“DEBUG DIGEST”创建摘要，并在删除键时释放值。*/
typedef void *(*moduleTypeLoadFunc)(struct RedisModuleIO *io, int encver);
typedef void (*moduleTypeSaveFunc)(struct RedisModuleIO *io, void *value);
typedef int (*moduleTypeAuxLoadFunc)(struct RedisModuleIO *rdb, int encver, int when);
typedef void (*moduleTypeAuxSaveFunc)(struct RedisModuleIO *rdb, int when);
typedef void (*moduleTypeRewriteFunc)(struct RedisModuleIO *io, struct redisObject *key, void *value);
typedef void (*moduleTypeDigestFunc)(struct RedisModuleDigest *digest, void *value);
typedef size_t (*moduleTypeMemUsageFunc)(const void *value);
typedef void (*moduleTypeFreeFunc)(void *value);
typedef size_t (*moduleTypeFreeEffortFunc)(struct redisObject *key, const void *value);
typedef void (*moduleTypeUnlinkFunc)(struct redisObject *key, void *value);
typedef void *(*moduleTypeCopyFunc)(struct redisObject *fromkey, struct redisObject *tokey, const void *value);
typedef int (*moduleTypeDefragFunc)(struct RedisModuleDefragCtx *ctx, struct redisObject *key, void **value);
typedef void (*RedisModuleInfoFunc)(struct RedisModuleInfoCtx *ctx, int for_crash_report);
typedef void (*RedisModuleDefragFunc)(struct RedisModuleDefragCtx *ctx);
typedef size_t (*moduleTypeMemUsageFunc2)(struct RedisModuleKeyOptCtx *ctx, const void *value, size_t sample_size);
typedef void (*moduleTypeFreeFunc2)(struct RedisModuleKeyOptCtx *ctx, void *value);
typedef size_t (*moduleTypeFreeEffortFunc2)(struct RedisModuleKeyOptCtx *ctx, const void *value);
typedef void (*moduleTypeUnlinkFunc2)(struct RedisModuleKeyOptCtx *ctx, void *value);
typedef void *(*moduleTypeCopyFunc2)(struct RedisModuleKeyOptCtx *ctx, const void *value);

/* This callback type is called by moduleNotifyUserChanged() every time
 * a user authenticated via the module API is associated with a different
 * user or gets disconnected. This needs to be exposed since you can't cast
 * a function pointer to (void *). */
/**每次通过模块 API 进行身份验证的用户与不同的用户关联或断开连接时，都会调用此回调类型。
 * 这需要公开，因为您不能将函数指针强制转换为 (void )。*/
typedef void (*RedisModuleUserChangedFunc) (uint64_t client_id, void *privdata);


/* The module type, which is referenced in each value of a given type, defines
 * the methods and links to the module exporting the type. */
//在给定类型的每个值中引用的模块类型定义了导出该类型的模块的方法和链接。
typedef struct RedisModuleType {
    uint64_t id; /* Higher 54 bits of type ID + 10 lower bits of encoding ver. 类型 ID 的高 54 位 + 编码版本的低 10 位。*/
    struct RedisModule *module;
    moduleTypeLoadFunc rdb_load;
    moduleTypeSaveFunc rdb_save;
    moduleTypeRewriteFunc aof_rewrite;
    moduleTypeMemUsageFunc mem_usage;
    moduleTypeDigestFunc digest;
    moduleTypeFreeFunc free;
    moduleTypeFreeEffortFunc free_effort;
    moduleTypeUnlinkFunc unlink;
    moduleTypeCopyFunc copy;
    moduleTypeDefragFunc defrag;
    moduleTypeAuxLoadFunc aux_load;
    moduleTypeAuxSaveFunc aux_save;
    moduleTypeMemUsageFunc2 mem_usage2;
    moduleTypeFreeEffortFunc2 free_effort2;
    moduleTypeUnlinkFunc2 unlink2;
    moduleTypeCopyFunc2 copy2;
    int aux_save_triggers;
    char name[10]; /* 9 bytes name + null term. Charset: A-Z a-z 0-9 _- */
} moduleType;

/* In Redis objects 'robj' structures of type OBJ_MODULE, the value pointer
 * is set to the following structure, referencing the moduleType structure
 * in order to work with the value, and at the same time providing a raw
 * pointer to the value, as created by the module commands operating with
 * the module type.
 *
 * So for example in order to free such a value, it is possible to use
 * the following code:
 *
 *  if (robj->type == OBJ_MODULE) {
 *      moduleValue *mt = robj->ptr;
 *      mt->type->free(mt->value);
 *      zfree(mt); // We need to release this in-the-middle struct as well.
 *  }
 */
/**
 * 在 OBJ_MODULE 类型的 Redis 对象“robj”结构中，值指针设置为以下结构，引用 moduleType 结构以使用值，
 * 同时提供指向值的原始指针，由使用模块类型操作的模块命令。
 * 因此，例如为了释放这样的值，可以使用以下代码：
 * if (robj->type == OBJ_MODULE) {
 *      moduleValue *mt = robj->ptr;
 *      mt->type->free(mt->value);
 *      zfree(mt); // 我们也需要释放这个中间结构。
 *  }
 * */
typedef struct moduleValue {
    moduleType *type;
    void *value;
} moduleValue;

/* This structure represents a module inside the system. */
//该结构代表系统内部的一个模块。
struct RedisModule {
    void *handle;   /* Module dlopen() handle. */
    char *name;     /* Module name. */
    int ver;        /* Module version. We use just progressive integers. 模块版本。我们只使用渐进整数。*/
    int apiver;     /* Module API version as requested during initialization. 初始化期间请求的模块 API 版本。*/
    list *types;    /* Module data types. */
    list *usedby;   /* List of modules using APIs from this one. 使用此 API 的模块列表。*/
    list *using;    /* List of modules we use some APIs of. 我们使用一些 API 的模块列表。*/
    list *filters;  /* List of filters the module has registered. 模块已注册的过滤器列表。*/
    list *module_configs; /* List of configurations the module has registered  模块已注册的配置列表*/
    int configs_initialized; /* Have the module configurations been initialized? 模块配置是否已初始化？*/
    int in_call;    /* RM_Call() nesting level */
    int in_hook;    /* Hooks callback nesting level for this module (0 or 1).挂钩此模块的回调嵌套级别（0 或 1）。 */
    int options;    /* Module options and capabilities. 模块选项和功能。*/
    int blocked_clients;         /* Count of RedisModuleBlockedClient in this module. 此模块中 RedisModuleBlockedClient 的计数。*/
    RedisModuleInfoFunc info_cb; /* Callback for module to add INFO fields. 回调模块以添加 INFO 字段。*/
    RedisModuleDefragFunc defrag_cb;    /* Callback for global data defrag. 全局数据碎片整理的回调。*/
    struct moduleLoadQueueEntry *loadmod; /* Module load arguments for config rewrite. 用于配置重写的模块加载参数。*/
};
typedef struct RedisModule RedisModule;

/* This is a wrapper for the 'rio' streams used inside rdb.c in Redis, so that
 * the user does not have to take the total count of the written bytes nor
 * to care about error conditions. */
//这是 Redis 中 rdb.c 中使用的 'rio' 流的包装器，因此用户不必计算写入字节的总数，也不必关心错误情况。
typedef struct RedisModuleIO {
    size_t bytes;       /* Bytes read / written so far. 到目前为止读取写入的字节数。*/
    rio *rio;           /* Rio stream. */
    moduleType *type;   /* Module type doing the operation. 执行操作的模块类型。*/
    int error;          /* True if error condition happened. 如果发生错误情况，则为真。*/
    int ver;            /* Module serialization version: 1 (old),
                         * 2 (current version with opcodes annotation).
                         * 模块序列化版本：1（旧），2（带有操作码注释的当前版本）。*/
    struct RedisModuleCtx *ctx; /* Optional context, see RM_GetContextFromIO() 可选上下文，参见 RM_GetContextFromIO()*/
    struct redisObject *key;    /* Optional name of key processed 已处理密钥的可选名称*/
    int dbid;            /* The dbid of the key being processed, -1 when unknown.正在处理的密钥的 dbid，未知时为 -1。 */
} RedisModuleIO;       

/* Macro to initialize an IO context. Note that the 'ver' field is populated
 * inside rdb.c according to the version of the value to load. */
//用于初始化 IO 上下文的宏。请注意，“ver”字段根据要加载的值的版本填充在 rdb.c 中。
#define moduleInitIOContext(iovar,mtype,rioptr,keyptr,db) do { \
    iovar.rio = rioptr; \
    iovar.type = mtype; \
    iovar.bytes = 0; \
    iovar.error = 0; \
    iovar.ver = 0; \
    iovar.key = keyptr; \
    iovar.dbid = db; \
    iovar.ctx = NULL; \
} while(0)

/* This is a structure used to export DEBUG DIGEST capabilities to Redis
 * modules. We want to capture both the ordered and unordered elements of
 * a data structure, so that a digest can be created in a way that correctly
 * reflects the values. See the DEBUG DIGEST command implementation for more
 * background. */
/**这是一种用于将 DEBUG DIGEST 功能导出到 Redis 模块的结构。
 * 我们希望捕获数据结构的有序和无序元素，以便可以以正确反映值的方式创建摘要。
 * 有关更多背景信息，请参阅 DEBUG DIGEST 命令实现。*/
typedef struct RedisModuleDigest {
    unsigned char o[20];    /* Ordered elements. 有序元素。*/
    unsigned char x[20];    /* Xored elements. 异或元素。*/
    struct redisObject *key; /* Optional name of key processed 已处理密钥的可选名称*/
    int dbid;                /* The dbid of the key being processed 正在处理的密钥的 dbid*/
} RedisModuleDigest;

/* Just start with a digest composed of all zero bytes.只需从由所有零字节组成的摘要开始。 */
#define moduleInitDigestContext(mdvar) do { \
    memset(mdvar.o,0,sizeof(mdvar.o)); \
    memset(mdvar.x,0,sizeof(mdvar.x)); \
} while(0)

/* Objects encoding. Some kind of objects like Strings and Hashes can be
 * internally represented in multiple ways. The 'encoding' field of the object
 * is set to one of this fields for this object. */
/**对象编码。某些类型的对象（如字符串和哈希）可以在内部以多种方式表示。对象的“编码”字段设置为此对象的此字段之一。*/
#define OBJ_ENCODING_RAW 0     /* Raw representation 原始表示*/
#define OBJ_ENCODING_INT 1     /* Encoded as integer 编码为整数*/
#define OBJ_ENCODING_HT 2      /* Encoded as hash table 编码为哈希表*/
#define OBJ_ENCODING_ZIPMAP 3  /* No longer used: old hash encoding. */
#define OBJ_ENCODING_LINKEDLIST 4 /* No longer used: old list encoding. */
#define OBJ_ENCODING_ZIPLIST 5 /* No longer used: old list/hash/zset encoding. */
#define OBJ_ENCODING_INTSET 6  /* Encoded as intset 编码为 intset*/
#define OBJ_ENCODING_SKIPLIST 7  /* Encoded as skiplist 编码为跳过列表*/
#define OBJ_ENCODING_EMBSTR 8  /* Embedded sds string encoding 嵌入式 sds 字符串编码*/
#define OBJ_ENCODING_QUICKLIST 9 /* Encoded as linked list of listpacks 编码为列表包的链表*/
#define OBJ_ENCODING_STREAM 10 /* Encoded as a radix tree of listpacks 编码为列表包的基数树*/
#define OBJ_ENCODING_LISTPACK 11 /* Encoded as a listpack 编码为列表包*/

#define LRU_BITS 24
#define LRU_CLOCK_MAX ((1<<LRU_BITS)-1) /* Max value of obj->lru obj->lru 的最大值*/
#define LRU_CLOCK_RESOLUTION 1000 /* LRU clock resolution in ms LRU 时钟分辨率（以 ms 为单位）*/

#define OBJ_SHARED_REFCOUNT INT_MAX     /* Global object never destroyed. 全局对象永远不会被破坏。*/
#define OBJ_STATIC_REFCOUNT (INT_MAX-1) /* Object allocated in the stack. 在堆栈中分配的对象。*/
#define OBJ_FIRST_SPECIAL_REFCOUNT OBJ_STATIC_REFCOUNT
typedef struct redisObject {
    unsigned type:4;
    unsigned encoding:4;
    unsigned lru:LRU_BITS; /* LRU time (relative to global lru_clock) or
                            * LFU data (least significant 8 bits frequency
                            * and most significant 16 bits access time).
                            * LRU 时间（相对于全局 lru_clock）或 LFU 数据（最低有效 8 位频率和最高有效 16 位访问时间）。*/
    int refcount;
    void *ptr;
} robj;

/* The a string name for an object's type as listed above
 * Native types are checked against the OBJ_STRING, OBJ_LIST, OBJ_* defines,
 * and Module types have their registered name returned. */
/**上面列出的对象类型的字符串名称根据 OBJ_STRING、OBJ_LIST、OBJ_*定义检查本机类型，并返回模块类型的注册名称。*/
char *getObjectTypeName(robj*);

/* Macro used to initialize a Redis object allocated on the stack.
 * Note that this macro is taken near the structure definition to make sure
 * we'll update it when the structure is changed, to avoid bugs like
 * bug #85 introduced exactly in this way. */
/**用于初始化分配在堆栈上的 Redis 对象的宏。
 * 请注意，这个宏是在结构定义附近使用的，以确保我们在结构更改时更新它，
 * 以避免像错误 85 这样的错误正是以这种方式引入的。*/
#define initStaticStringObject(_var,_ptr) do { \
    _var.refcount = OBJ_STATIC_REFCOUNT; \
    _var.type = OBJ_STRING; \
    _var.encoding = OBJ_ENCODING_RAW; \
    _var.ptr = _ptr; \
} while(0)

struct evictionPoolEntry; /* Defined in evict.c */

/* This structure is used in order to represent the output buffer of a client,
 * which is actually a linked list of blocks like that, that is: client->reply. */
//这个结构用来表示一个客户端的输出缓冲区，实际上就是一个这样的块的链表，即：client->reply。
typedef struct clientReplyBlock {
    size_t size, used;
    char buf[];
} clientReplyBlock;

/* Replication buffer blocks is the list of replBufBlock.
 *
 * +--------------+       +--------------+       +--------------+
 * | refcount = 1 |  ...  | refcount = 0 |  ...  | refcount = 2 |
 * +--------------+       +--------------+       +--------------+
 *      |                                            /       \
 *      |                                           /         \
 *      |                                          /           \
 *  Repl Backlog                               Replia_A      Replia_B
 * 
 * Each replica or replication backlog increments only the refcount of the
 * 'ref_repl_buf_node' which it points to. So when replica walks to the next
 * node, it should first increase the next node's refcount, and when we trim
 * the replication buffer nodes, we remove node always from the head node which
 * refcount is 0. If the refcount of the head node is not 0, we must stop
 * trimming and never iterate the next node. */

/* Similar with 'clientReplyBlock', it is used for shared buffers between
 * all replica clients and replication backlog. */
/**
 * 复制缓冲区块是 replBufBlock 的列表。
 *  * +--------------+       +--------------+       +--------------+
 * | refcount = 1 |  ...  | refcount = 0 |  ...  | refcount = 2 |
 * +--------------+       +--------------+       +--------------+
 *      |                                            /       \
 *      |                                           /         \
 *      |                                          /           \
 *  Repl Backlog                               Replia_A      Replia_B
每个副本或复制积压仅增加它指向的“ref_repl_buf_node”的引用计数。
 所以replica走到下一个节点的时候，应该先增加下一个节点的refcount，当我们修剪replication buffer节点时，
 总是从头节点中移除refcount为0的节点。如果头节点的refcount不为0 ，我们必须停止修剪并且永远不要迭代下一个节点。
 与“clientReplyBlock”类似，它用于所有副本客户端和复制积压之间的共享缓冲区。*/
typedef struct replBufBlock {
    int refcount;           /* Number of replicas or repl backlog using. 使用的副本数或 repl backlog。*/
    long long id;           /* The unique incremental number. 唯一的增量编号。*/
    long long repl_offset;  /* Start replication offset of the block.开始块的复制偏移量。 */
    size_t size, used;
    char buf[];
} replBufBlock;

/* Opaque type for the Slot to Key API. Slot to Key API 的不透明类型。*/
typedef struct clusterSlotToKeyMapping clusterSlotToKeyMapping;

/* Redis database representation. There are multiple databases identified
 * by integers from 0 (the default database) up to the max configured
 * database. The database number is the 'id' field in the structure. */
/**
 * Redis 数据库表示。有多个数据库由从 0（默认数据库）到最大配置数据库的整数标识。数据库编号是结构中的“id”字段。
 * */
typedef struct redisDb {
    dict *dict;                 /* The keyspace for this DB 此数据库的键空间*/
    dict *expires;              /* Timeout of keys with a timeout set 设置了超时的键超时*/
    dict *blocking_keys;        /* Keys with clients waiting for data (BLPOP) 客户端等待数据的键 (BLPOP)*/
    dict *ready_keys;           /* Blocked keys that received a PUSH 收到 PUSH 的阻塞键*/
    dict *watched_keys;         /* WATCHED keys for MULTI/EXEC CAS MULTIEXEC CAS 的 WATCHED 键*/
    int id;                     /* Database ID */
    long long avg_ttl;          /* Average TTL, just for stats 平均 TTL，仅用于统计*/
    unsigned long expires_cursor; /* Cursor of the active expire cycle. 活动过期周期的光标。*/
    list *defrag_later;         /* List of key names to attempt to defrag
 *                                 one by one, gradually. 要逐步进行碎片整理的键名列表。*/
    clusterSlotToKeyMapping *slots_to_keys; /* Array of slots to keys.
 *                                             Only used in cluster mode (db 0). 到键的插槽数组。仅用于集群模式 (db 0)。*/
} redisDb;

/* forward declaration for functions ctx 函数 ctx 的前向声明*/
typedef struct functionsLibCtx functionsLibCtx;

/* Holding object that need to be populated during
 * rdb loading. On loading end it is possible to decide
 * whether not to set those objects on their rightful place.
 * For example: dbarray need to be set as main database on
 *              successful loading and dropped on failure. */
/**在 rdb 加载期间需要填充的持有对象。在加载结束时，可以决定是否不将这些对象设置在其应有的位置。例如：
 * dbarray 需要在成功加载时设置为主数据库，在失败时删除。*/
typedef struct rdbLoadingCtx {
    redisDb* dbarray;
    functionsLibCtx* functions_lib_ctx;
}rdbLoadingCtx;

/* Client MULTI/EXEC state */
typedef struct multiCmd {
    robj **argv;
    int argv_len;
    int argc;
    struct redisCommand *cmd;
} multiCmd;

typedef struct multiState {
    multiCmd *commands;     /* Array of MULTI commands MULTI 命令数组*/
    int count;              /* Total number of MULTI commands MULTI 命令总数*/
    int cmd_flags;          /* The accumulated command flags OR-ed together.
                               So if at least a command has a given flag, it
                               will be set in this field.
                               累积的命令标志 OR-ed 在一起。所以如果至少一个命令有一个给定的标志，它将被设置在这个字段中。*/
    int cmd_inv_flags;      /* Same as cmd_flags, OR-ing the ~flags. so that it
                               is possible to know if all the commands have a
                               certain flag.
                               与 cmd_flags 相同，对 ~flags 进行 OR-ing。这样就可以知道是否所有的命令都有一个特定的标志。*/
    size_t argv_len_sums;    /* mem used by all commands arguments 所有命令参数使用的内存*/
    int alloc_count;         /* total number of multiCmd struct memory reserved. 保留的 multiCmd 结构内存总数。*/
} multiState;

/* This structure holds the blocking operation state for a client.
 * The fields used depend on client->btype. */
//此结构保存客户端的阻塞操作状态。使用的字段取决于 client->btype。
typedef struct blockingState {
    /* Generic fields. */
    long count;             /* Elements to pop if count was specified (BLMPOP/BZMPOP), -1 otherwise.
 *                              如果指定了计数 (BLMPOPBZMPOP)，则要弹出的元素，否则为 -1。*/
    mstime_t timeout;       /* Blocking operation timeout. If UNIX current time
                             * is > timeout then the operation timed out.
                             * 阻塞操作超时。如果 UNIX 当前时间 > 超时，则操作超时。*/

    /* BLOCKED_LIST, BLOCKED_ZSET and BLOCKED_STREAM */
    dict *keys;             /* The keys we are waiting to terminate a blocking
                             * operation such as BLPOP or XREAD. Or NULL.
                             * 我们等待终止阻塞操作的键，例如 BLPOP 或 XREAD。或为空。*/
    robj *target;           /* The key that should receive the element,
                             * for BLMOVE. 应该接收元素的键，对于 BLMOVE。*/
    struct blockPos {
        int wherefrom;      /* Where to pop from 从哪里弹出*/
        int whereto;        /* Where to push to 推到哪里*/
    } blockpos;              /* The positions in the src/dst lists/zsets
                             * where we want to pop/push an element
                             * for BLPOP, BRPOP, BLMOVE and BZMPOP.
                             * src/dst 中的位置列出了我们想要弹出 BLPOP、BRPOP、BLMOVE 和 BZMPOP 元素的位置。*/

    /* BLOCK_STREAM */
    size_t xread_count;     /* XREAD COUNT option. */
    robj *xread_group;      /* XREADGROUP group name. */
    robj *xread_consumer;   /* XREADGROUP consumer name. */
    int xread_group_noack;

    /* BLOCKED_WAIT */
    int numreplicas;        /* Number of replicas we are waiting for ACK. 我们正在等待 ACK 的副本数。*/
    long long reploffset;   /* Replication offset to reach. 要达到的复制偏移量。*/

    /* BLOCKED_MODULE */
    void *module_blocked_handle; /* RedisModuleBlockedClient structure.
                                    which is opaque for the Redis core, only
                                    handled in module.c.
                                    RedisModuleBlockedClient 结构。这对于 Redis 核心是不透明的，仅在 module.c 中处理。*/
} blockingState;

/* The following structure represents a node in the server.ready_keys list,
 * where we accumulate all the keys that had clients blocked with a blocking
 * operation such as B[LR]POP, but received new data in the context of the
 * last executed command.
 *
 * After the execution of every command or script, we run this list to check
 * if as a result we should serve data to clients blocked, unblocking them.
 * Note that server.ready_keys will not have duplicates as there dictionary
 * also called ready_keys in every structure representing a Redis database,
 * where we make sure to remember if a given key was already added in the
 * server.ready_keys list. */
/**下面的结构表示 server.ready_keys 列表中的一个节点，我们在其中累积所有让客户端被阻塞操作（例如 B[LR]POP）阻塞，
 * 但在最后执行的命令的上下文中接收到新数据的键。
 * 在执行每个命令或脚本之后，我们运行这个列表来检查我们是否应该将数据提供给被阻塞的客户端，解除阻塞。
 * 请注意，server.ready_keys 不会有重复项，因为在代表 Redis 数据库的每个结构中，
 * 字典也称为 ready_keys，我们确保记住给定键是否已添加到 server.ready_keys 列表中。*/
typedef struct readyList {
    redisDb *db;
    robj *key;
} readyList;

/* This structure represents a Redis user. This is useful for ACLs, the
 * user is associated to the connection after the connection is authenticated.
 * If there is no associated user, the connection uses the default user. */
/**这个结构代表一个 Redis 用户。这对 ACL 很有用，用户在连接通过身份验证后与连接相关联。
 * 如果没有关联用户，则连接使用默认用户。*/
#define USER_COMMAND_BITS_COUNT 1024    /* The total number of command bits
                                           in the user structure. The last valid
                                           command ID we can set in the user
                                           is USER_COMMAND_BITS_COUNT-1.
                                           用户结构中的命令位总数。我们可以在用户中设置的最后一个有效命令 ID
                                           是 USER_COMMAND_BITS_COUNT-1。*/
#define USER_FLAG_ENABLED (1<<0)        /* The user is active. */
#define USER_FLAG_DISABLED (1<<1)       /* The user is disabled. */
#define USER_FLAG_NOPASS (1<<2)         /* The user requires no password, any
                                           provided password will work. For the
                                           default user, this also means that
                                           no AUTH is needed, and every
                                           connection is immediately
                                           authenticated.
                                           用户不需要密码，任何提供的密码都可以使用。对于默认用户，
                                           这也意味着不需要 AUTH，每个连接都会立即进行身份验证。*/
#define USER_FLAG_SANITIZE_PAYLOAD (1<<3)       /* The user require a deep RESTORE
                                                 * payload sanitization. 用户需要深度 RESTORE 有效负载清理。*/
#define USER_FLAG_SANITIZE_PAYLOAD_SKIP (1<<4)  /* The user should skip the
                                                 * deep sanitization of RESTORE
                                                 * payload. 用户应该跳过对 RESTORE 有效负载的深度清理。*/

#define SELECTOR_FLAG_ROOT (1<<0)           /* This is the root user permission
                                             * selector. 这是根用户权限选择器。*/
#define SELECTOR_FLAG_ALLKEYS (1<<1)        /* The user can mention any key.用户可以提及任何键。 */
#define SELECTOR_FLAG_ALLCOMMANDS (1<<2)    /* The user can run all commands. 用户可以运行所有命令。*/
#define SELECTOR_FLAG_ALLCHANNELS (1<<3)    /* The user can mention any Pub/Sub
                                               channel. 用户可以提及任何 PubSub 频道。*/

typedef struct {
    sds name;       /* The username as an SDS string. SDS 字符串形式的用户名。*/
    uint32_t flags; /* See USER_FLAG_* */
    list *passwords; /* A list of SDS valid passwords for this user. 此用户的 SDS 有效密码列表。*/
    list *selectors; /* A list of selectors this user validates commands
                        against. This list will always contain at least
                        one selector for backwards compatibility.
                        此用户验证命令所依据的选择器列表。该列表将始终包含至少一个选择器以实现向后兼容性。*/
} user;

/* With multiplexing we need to take per-client state.
 * Clients are taken in a linked list.
 * 通过多路复用，我们需要获取每个客户端的状态。客户被带入一个链表。*/

#define CLIENT_ID_AOF (UINT64_MAX) /* Reserved ID for the AOF client. If you
                                      need more reserved IDs use UINT64_MAX-1,
                                      -2, ... and so forth.
                                      AOF 客户端的保留 ID。如果您需要更多保留 ID，请使用 UINT64_MAX-1、-2、... 等等。*/

/* Replication backlog is not separate memory, it just is one consumer of
 * the global replication buffer. This structure records the reference of
 * replication buffers. Since the replication buffer block list may be very long,
 * it would cost much time to search replication offset on partial resync, so
 * we use one rax tree to index some blocks every REPL_BACKLOG_INDEX_PER_BLOCKS
 * to make searching offset from replication buffer blocks list faster. */
/**复制积压不是单独的内存，它只是全局复制缓冲区的一个消费者。该结构记录了复制缓冲区的引用。
 * 由于复制缓冲区块列表可能很长，在部分重新同步时搜索复制偏移量会花费大量时间，
 * 因此我们使用一棵 rax 树索引每个 REPL_BACKLOG_INDEX_PER_BLOCKS 的一些块，以更快地从复制缓冲区块列表中搜索偏移量。*/
typedef struct replBacklog {
    listNode *ref_repl_buf_node; /* Referenced node of replication buffer blocks,
                                  * see the definition of replBufBlock.
                                  * 复制缓冲块的引用节点，见replBufBlock的定义。*/
    size_t unindexed_count;      /* The count from last creating index block. 上次创建索引块的计数。*/
    rax *blocks_index;           /* The index of recorded blocks of replication
                                  * buffer for quickly searching replication
                                  * offset on partial resynchronization.
                                  * 复制缓冲区记录块的索引，用于在部分重新同步时快速搜索复制偏移量。*/
    long long histlen;           /* Backlog actual data length 积压实际数据长度*/
    long long offset;            /* Replication "master offset" of first
                                  * byte in the replication backlog buffer.
                                  * 复制积压缓冲区中第一个字节的复制“主偏移”。*/
} replBacklog;

typedef struct {
    list *clients;
    size_t mem_usage_sum;
} clientMemUsageBucket;

typedef struct client {
    uint64_t id;            /* Client incremental unique ID. 客户端增量唯一 ID。*/
    uint64_t flags;         /* Client flags: CLIENT_* macros. 客户端标志：CLIENT_ 宏。*/
    connection *conn;
    int resp;               /* RESP protocol version. Can be 2 or 3.  RESP 协议版本。可以是 2 或 3。*/
    redisDb *db;            /* Pointer to currently SELECTed DB. 指向当前选择的数据库的指针。*/
    robj *name;             /* As set by CLIENT SETNAME. 由 CLIENT SETNAME 设置。*/
    sds querybuf;           /* Buffer we use to accumulate client queries. 我们用来累积客户端查询的缓冲区。*/
    size_t qb_pos;          /* The position we have read in querybuf. 我们在querybuf中读到的位置。*/
    size_t querybuf_peak;   /* Recent (100ms or more) peak of querybuf size. querybuf 大小的最近（100 毫秒或更多）峰值。*/
    int argc;               /* Num of arguments of current command. 当前命令的参数数量。*/
    robj **argv;            /* Arguments of current command. 当前命令的参数。*/
    int argv_len;           /* Size of argv array (may be more than argc) argv 数组的大小（可能大于 argc）*/
    int original_argc;      /* Num of arguments of original command if arguments were rewritten. 如果参数被重写，原始命令的参数数量。*/
    robj **original_argv;   /* Arguments of original command if arguments were rewritten. 如果参数被重写，则为原始命令的参数。*/
    size_t argv_len_sum;    /* Sum of lengths of objects in argv list. argv 列表中对象的长度总和。*/
    struct redisCommand *cmd, *lastcmd;  /* Last command executed. 最后执行的命令。*/
    struct redisCommand *realcmd; /* The original command that was executed by the client,
                                     Used to update error stats in case the c->cmd was modified
                                     during the command invocation (like on GEOADD for example).
                                     客户端执行的原始命令，用于更新错误统计信息，
                                     以防在命令调用期间修改了 c->cmd（例如在 GEOADD 上）。*/
    user *user;             /* User associated with this connection. If the
                               user is set to NULL the connection can do
                               anything (admin).
                               与此连接关联的用户。如果用户设置为 NULL，则连接可以做任何事情（管理员）。*/
    int reqtype;            /* Request protocol type: PROTO_REQ_* 请求协议类型：PROTO_REQ_ */
    int multibulklen;       /* Number of multi bulk arguments left to read. 剩下要读取的多批量参数的数量。*/
    long bulklen;           /* Length of bulk argument in multi bulk request. 多批量请求中的批量参数长度。*/
    list *reply;            /* List of reply objects to send to the client. 要发送给客户端的回复对象列表。*/
    unsigned long long reply_bytes; /* Tot bytes of objects in reply list. 回复列表中对象的总字节数。*/
    list *deferred_reply_errors;    /* Used for module thread safe contexts. 用于模块线程安全上下文。*/
    size_t sentlen;         /* Amount of bytes already sent in the current
                               buffer or object being sent.
                               当前缓冲区或正在发送的对象中已发送的字节数。*/
    time_t ctime;           /* Client creation time. 客户端创建时间。*/
    long duration;          /* Current command duration. Used for measuring latency of blocking/non-blocking cmds
 *                             当前命令持续时间。用于测量blockingnon-blocking cmds的延迟*/
    int slot;               /* The slot the client is executing against. Set to -1 if no slot is being used
 *                             客户端正在执行的插槽。如果没有使用插槽，则设置为 -1*/
    time_t lastinteraction; /* Time of the last interaction, used for timeout 最后一次交互的时间，用于超时 */
    time_t obuf_soft_limit_reached_time;
    int authenticated;      /* Needed when the default user requires auth. 当默认用户需要身份验证时需要。*/
    int replstate;          /* Replication state if this is a slave. 如果这是从属设备，则为复制状态。*/
    int repl_start_cmd_stream_on_ack; /* Install slave write handler on first ACK. 在第一个 ACK 上安装从写处理程序。*/
    int repldbfd;           /* Replication DB file descriptor. 复制数据库文件描述符。*/
    off_t repldboff;        /* Replication DB file offset. 复制数据库文件偏移量。*/
    off_t repldbsize;       /* Replication DB file size. 复制数据库文件大小。*/
    sds replpreamble;       /* Replication DB preamble. 复制数据库序言。*/
    long long read_reploff; /* Read replication offset if this is a master. 如果这是主服务器，则读取复制偏移量。*/
    long long reploff;      /* Applied replication offset if this is a master. 如果这是主服务器，则应用复制偏移量。*/
    long long repl_applied; /* Applied replication data count in querybuf, if this is a replica. querybuf 中应用的复制数据计数，如果这是一个副本。*/
    long long repl_ack_off; /* Replication ack offset, if this is a slave. 复制确认偏移量，如果这是slave。*/
    long long repl_ack_time;/* Replication ack time, if this is a slave. 复制确认时间，如果这是slave。*/
    long long repl_last_partial_write; /* The last time the server did a partial write from the RDB child pipe to this replica  服务器上一次从 RDB 子管道向这个副本进行部分写入*/
    long long psync_initial_offset; /* FULLRESYNC reply offset other slaves
                                       copying this slave output buffer
                                       should use.    FULLRESYNC 复制此从属输出缓冲区的其他从属应使用的回复偏移量。*/
    char replid[CONFIG_RUN_ID_SIZE+1]; /* Master replication ID (if master). master复制 ID（如果是master）。*/
    int slave_listening_port; /* As configured with: REPLCONF listening-port 配置为：REPLCONF 监听端口*/
    char *slave_addr;       /* Optionally given by REPLCONF ip-address 可选地由 REPLCONF ip-address 给出*/
    int slave_capa;         /* Slave capabilities: SLAVE_CAPA_* bitwise OR. slave能力：SLAVE_CAPA_按位或。*/
    int slave_req;          /* Slave requirements: SLAVE_REQ_*  slave要求：SLAVE_REQ_*/
    multiState mstate;      /* MULTI/EXEC state */
    int btype;              /* Type of blocking op if CLIENT_BLOCKED. CLIENT_BLOCKED 的阻塞操作类型。*/
    blockingState bpop;     /* blocking state */
    long long woff;         /* Last write global replication offset. 最后写入全局复制偏移量。*/
    list *watched_keys;     /* Keys WATCHED for MULTI/EXEC CAS */
    dict *pubsub_channels;  /* channels a client is interested in (SUBSCRIBE) 客户感兴趣的频道（订阅）*/
    list *pubsub_patterns;  /* patterns a client is interested in (SUBSCRIBE) 客户感兴趣的模式（订阅）*/
    dict *pubsubshard_channels;  /* shard level channels a client is interested in (SSUBSCRIBE) 客户感兴趣的分片级频道（订阅）*/
    sds peerid;             /* Cached peer ID. */
    sds sockname;           /* Cached connection target address. */
    listNode *client_list_node; /* list node in client list 在客户端列表中列出节点*/
    listNode *postponed_list_node; /* list node within the postponed list 延迟列表中的列表节点*/
    listNode *pending_read_list_node; /* list node in clients pending read list 在客户端挂起读取列表中列出节点*/
    RedisModuleUserChangedFunc auth_callback; /* Module callback to execute
                                               * when the authenticated user
                                               * changes. 当经过身份验证的用户更改时执行的模块回调。*/
    void *auth_callback_privdata; /* Private data that is passed when the auth
                                   * changed callback is executed. Opaque for
                                   * Redis Core. 执行 auth changed 回调时传递的私有数据。 Redis 核心不透明。*/
    void *auth_module;      /* The module that owns the callback, which is used
                             * to disconnect the client if the module is
                             * unloaded for cleanup. Opaque for Redis Core.
                             * 拥有回调的模块，用于在卸载模块进行清理时断开客户端。 Redis 核心不透明。*/

    /* If this client is in tracking mode and this field is non zero,
     * invalidation messages for keys fetched by this client will be send to
     * the specified client ID. */
    /**如果此客户端处于跟踪模式并且此字段不为零，则此客户端获取的密钥的无效消息将发送到指定的客户端 ID。*/
    uint64_t client_tracking_redirection;
    rax *client_tracking_prefixes; /* A dictionary of prefixes we are already
                                      subscribed to in BCAST mode, in the
                                      context of client side caching.
                                      在客户端缓存的上下文中，我们已经在 BCAST 模式下订阅了前缀字典。*/

    /* In updateClientMemUsage() we track the memory usage of
     * each client and add it to the sum of all the clients of a given type,
     * however we need to remember what was the old contribution of each
     * client, and in which category the client was, in order to remove it
     * before adding it the new value. */
    /**在 updateClientMemUsage() 中，我们跟踪每个客户端的内存使用情况并将其添加到给定类型的所有客户端的总和中，
     * 但是我们需要记住每个客户端的旧贡献是什么，以及客户端属于哪个类别，在在添加新值之前将其删除。*/
    size_t last_memory_usage;
    int last_memory_type;

    listNode *mem_usage_bucket_node;
    clientMemUsageBucket *mem_usage_bucket;

    listNode *ref_repl_buf_node; /* Referenced node of replication buffer blocks,
                                  * see the definition of replBufBlock.
                                  * 复制缓冲块的引用节点，见replBufBlock的定义。*/
    size_t ref_block_pos;        /* Access position of referenced buffer block,
                                  * i.e. the next offset to send.
                                  * 引用缓冲区块的访问位置，即要发送的下一个偏移量。*/

    /* Response buffer */
    size_t buf_peak; /* Peak used size of buffer in last 5 sec interval. 过去 5 秒间隔内使用的缓冲区大小峰值。*/
    mstime_t buf_peak_last_reset_time; /* keeps the last time the buffer peak value was reset 保留上次重置缓冲区峰值的时间*/
    int bufpos;
    size_t buf_usable_size; /* Usable size of buffer. 缓冲区的可用大小。*/
    char *buf;
} client;

struct saveparam {
    time_t seconds;
    int changes;
};

struct moduleLoadQueueEntry {
    sds path;
    int argc;
    robj **argv;
};

struct sentinelLoadQueueEntry {
    int argc;
    sds *argv;
    int linenum;
    sds line;
};

struct sentinelConfig {
    list *pre_monitor_cfg;
    list *monitor_cfg;
    list *post_monitor_cfg;
};

struct sharedObjectsStruct {
    robj *crlf, *ok, *err, *emptybulk, *czero, *cone, *pong, *space,
    *queued, *null[4], *nullarray[4], *emptymap[4], *emptyset[4],
    *emptyarray, *wrongtypeerr, *nokeyerr, *syntaxerr, *sameobjecterr,
    *outofrangeerr, *noscripterr, *loadingerr,
    *slowevalerr, *slowscripterr, *slowmoduleerr, *bgsaveerr,
    *masterdownerr, *roslaveerr, *execaborterr, *noautherr, *noreplicaserr,
    *busykeyerr, *oomerr, *plus, *messagebulk, *pmessagebulk, *subscribebulk,
    *unsubscribebulk, *psubscribebulk, *punsubscribebulk, *del, *unlink,
    *rpop, *lpop, *lpush, *rpoplpush, *lmove, *blmove, *zpopmin, *zpopmax,
    *emptyscan, *multi, *exec, *left, *right, *hset, *srem, *xgroup, *xclaim,  
    *script, *replconf, *eval, *persist, *set, *pexpireat, *pexpire, 
    *time, *pxat, *absttl, *retrycount, *force, *justid, *entriesread,
    *lastid, *ping, *setid, *keepttl, *load, *createconsumer,
    *getack, *special_asterick, *special_equals, *default_username, *redacted,
    *ssubscribebulk,*sunsubscribebulk, *smessagebulk,
    *select[PROTO_SHARED_SELECT_CMDS],
    *integers[OBJ_SHARED_INTEGERS],
    *mbulkhdr[OBJ_SHARED_BULKHDR_LEN], /* "*<value>\r\n" */
    *bulkhdr[OBJ_SHARED_BULKHDR_LEN],  /* "$<value>\r\n" */
    *maphdr[OBJ_SHARED_BULKHDR_LEN],   /* "%<value>\r\n" */
    *sethdr[OBJ_SHARED_BULKHDR_LEN];   /* "~<value>\r\n" */
    sds minstring, maxstring;
};

/* ZSETs use a specialized version of Skiplists */
//ZSET 使用专用版本的 Skiplist
typedef struct zskiplistNode {
    sds ele;
    double score;
    struct zskiplistNode *backward;
    struct zskiplistLevel {
        struct zskiplistNode *forward;
        unsigned long span;
    } level[];
} zskiplistNode;

typedef struct zskiplist {
    struct zskiplistNode *header, *tail;
    unsigned long length;
    int level;
} zskiplist;

typedef struct zset {
    dict *dict;
    zskiplist *zsl;
} zset;

typedef struct clientBufferLimitsConfig {
    unsigned long long hard_limit_bytes;
    unsigned long long soft_limit_bytes;
    time_t soft_limit_seconds;
} clientBufferLimitsConfig;

extern clientBufferLimitsConfig clientBufferLimitsDefaults[CLIENT_TYPE_OBUF_COUNT];

/* The redisOp structure defines a Redis Operation, that is an instance of
 * a command with an argument vector, database ID, propagation target
 * (PROPAGATE_*), and command pointer.
 *
 * Currently only used to additionally propagate more commands to AOF/Replication
 * after the propagation of the executed command. */
/**redisOp 结构定义了一个 Redis 操作，它是一个带有参数向量、数据库 ID、传播目标 (PROPAGATE_) 和命令指针的命令实例。
 *
 * 目前只用于在执行的命令传播之后，额外传播更多的命令到 AOFReplication。*/
typedef struct redisOp {
    robj **argv;
    int argc, dbid, target;
} redisOp;

/* Defines an array of Redis operations. There is an API to add to this
 * structure in an easy way.
 * 定义一个 Redis 操作数组。有一个 API 可以简单地添加到这个结构中。
 *
 * redisOpArrayInit();
 * redisOpArrayAppend();
 * redisOpArrayFree();
 */
typedef struct redisOpArray {
    redisOp *ops;
    int numops;
    int capacity;
} redisOpArray;

/* This structure is returned by the getMemoryOverheadData() function in
 * order to return memory overhead information. */
//此结构由 getMemoryOverheadData() 函数返回，以返回内存开销信息。
struct redisMemOverhead {
    size_t peak_allocated;
    size_t total_allocated;
    size_t startup_allocated;
    size_t repl_backlog;
    size_t clients_slaves;
    size_t clients_normal;
    size_t cluster_links;
    size_t aof_buffer;
    size_t lua_caches;
    size_t functions_caches;
    size_t overhead_total;
    size_t dataset;
    size_t total_keys;
    size_t bytes_per_key;
    float dataset_perc;
    float peak_perc;
    float total_frag;
    ssize_t total_frag_bytes;
    float allocator_frag;
    ssize_t allocator_frag_bytes;
    float allocator_rss;
    ssize_t allocator_rss_bytes;
    float rss_extra;
    size_t rss_extra_bytes;
    size_t num_dbs;
    struct {
        size_t dbid;
        size_t overhead_ht_main;
        size_t overhead_ht_expires;
        size_t overhead_ht_slot_to_keys;
    } *db;
};

/* Replication error behavior determines the replica behavior
 * when it receives an error over the replication stream. In
 * either case the error is logged. */
//复制错误行为决定了副本在通过复制流接收到错误时的行为。在任何一种情况下，都会记录错误。
typedef enum {
    PROPAGATION_ERR_BEHAVIOR_IGNORE = 0,
    PROPAGATION_ERR_BEHAVIOR_PANIC,
    PROPAGATION_ERR_BEHAVIOR_PANIC_ON_REPLICAS
} replicationErrorBehavior;

/* This structure can be optionally passed to RDB save/load functions in
 * order to implement additional functionalities, by storing and loading
 * metadata to the RDB file.
 *
 * For example, to use select a DB at load time, useful in
 * replication in order to make sure that chained slaves (slaves of slaves)
 * select the correct DB and are able to accept the stream coming from the
 * top-level master. */
/**通过将元数据存储和加载到 RDB 文件，可以选择将此结构传递给 RDB 保存加载函数，以实现其他功能。
 *
 * 例如，在加载时使用 select a DB，这在复制中很有用，以确保链接的从属（从属的从属）选择正确的 DB 并能够接受来自顶级主控的流。*/
typedef struct rdbSaveInfo {
    /* Used saving and loading. 使用保存和加载。*/
    int repl_stream_db;  /* DB to select in server.master client. 要在 server.master 客户端中选择的数据库。*/

    /* Used only loading. 仅用于加载。*/
    int repl_id_is_set;  /* True if repl_id field is set. 如果设置了 repl_id 字段，则为真。*/
    char repl_id[CONFIG_RUN_ID_SIZE+1];     /* Replication ID. 复制 ID。*/
    long long repl_offset;                  /* Replication offset. */
} rdbSaveInfo;

#define RDB_SAVE_INFO_INIT {-1,0,"0000000000000000000000000000000000000000",-1}

struct malloc_stats {
    size_t zmalloc_used;
    size_t process_rss;
    size_t allocator_allocated;
    size_t allocator_active;
    size_t allocator_resident;
};

typedef struct socketFds {
    int fd[CONFIG_BINDADDR_MAX];
    int count;
} socketFds;

/*-----------------------------------------------------------------------------
 * TLS Context Configuration
 *----------------------------------------------------------------------------*/

typedef struct redisTLSContextConfig {
    char *cert_file;                /* Server side and optionally client side cert file name 服务器端和可选的客户端证书文件名*/
    char *key_file;                 /* Private key filename for cert_file cert_file 的私钥文件名*/
    char *key_file_pass;            /* Optional password for key_file key_file 的可选密码*/
    char *client_cert_file;         /* Certificate to use as a client; if none, use cert_file 用作客户端的证书；如果没有，请使用 cert_file*/
    char *client_key_file;          /* Private key filename for client_cert_file       client_cert_file 的私钥文件名*/
    char *client_key_file_pass;     /* Optional password for client_key_file client_key_file 的可选密码*/
    char *dh_params_file;
    char *ca_cert_file;
    char *ca_cert_dir;
    char *protocols;
    char *ciphers;
    char *ciphersuites;
    int prefer_server_ciphers;
    int session_caching;
    int session_cache_size;
    int session_cache_timeout;
} redisTLSContextConfig;

/*-----------------------------------------------------------------------------
 * AOF manifest definition
 *----------------------------------------------------------------------------*/
typedef enum {
    AOF_FILE_TYPE_BASE  = 'b', /* BASE file */
    AOF_FILE_TYPE_HIST  = 'h', /* HISTORY file */
    AOF_FILE_TYPE_INCR  = 'i', /* INCR file */
} aof_file_type;

typedef struct {
    sds           file_name;  /* file name */
    long long     file_seq;   /* file sequence */
    aof_file_type file_type;  /* file type */
} aofInfo;

typedef struct {
    aofInfo     *base_aof_info;       /* BASE file information. NULL if there is no BASE file. 基本文件信息。如果没有 BASE 文件，则为 NULL。*/
    list        *incr_aof_list;       /* INCR AOFs list. We may have multiple INCR AOF when rewrite fails. INCR AOF 列表。当重写失败时，我们可能有多个 INCR AOF。*/
    list        *history_aof_list;    /* HISTORY AOF list. When the AOFRW success, The aofInfo contained in
                                         `base_aof_info` and `incr_aof_list` will be moved to this list. We
                                         will delete these AOF files when AOFRW finish.
                                         历史 AOF 列表。当 AOFRW 成功时，`base_aof_info` 和 `incr_aof_list` 中包含的 aofInfo 将被移动到这个列表中。
                                         当 AOFRW 完成时，我们将删除这些 AOF 文件。*/
    long long   curr_base_file_seq;   /* The sequence number used by the current BASE file. 当前 BASE 文件使用的序列号。*/
    long long   curr_incr_file_seq;   /* The sequence number used by the current INCR file. 当前 INCR 文件使用的序列号。*/
    int         dirty;                /* 1 Indicates that the aofManifest in the memory is inconsistent with
                                         disk, we need to persist it immediately.
                                         1 表示内存中的aofManifest与磁盘不一致，我们需要立即持久化。*/
} aofManifest;

/*-----------------------------------------------------------------------------
 * Global server state
 *----------------------------------------------------------------------------*/

/* AIX defines hz to __hz, we don't use this define and in order to allow
 * Redis build on AIX we need to undef it. */
//AIX 将 hz 定义为 __hz，我们不使用此定义，为了允许 Redis 在 AIX 上构建，我们需要取消定义它。
#ifdef _AIX
#undef hz
#endif

#define CHILD_TYPE_NONE 0
#define CHILD_TYPE_RDB 1
#define CHILD_TYPE_AOF 2
#define CHILD_TYPE_LDB 3
#define CHILD_TYPE_MODULE 4

typedef enum childInfoType {
    CHILD_INFO_TYPE_CURRENT_INFO,
    CHILD_INFO_TYPE_AOF_COW_SIZE,
    CHILD_INFO_TYPE_RDB_COW_SIZE,
    CHILD_INFO_TYPE_MODULE_COW_SIZE
} childInfoType;

struct redisServer {
    /* General */
    pid_t pid;                  /* Main process pid. */
    pthread_t main_thread_id;         /* Main thread id */
    char *configfile;           /* Absolute config file path, or NULL */
    char *executable;           /* Absolute executable file path. */
    char **exec_argv;           /* Executable argv vector (copy). */
    int dynamic_hz;             /* Change hz value depending on # of clients. */
    int config_hz;              /* Configured HZ value. May be different than
                                   the actual 'hz' field value if dynamic-hz
                                   is enabled. */
    mode_t umask;               /* The umask value of the process on startup */
    int hz;                     /* serverCron() calls frequency in hertz */
    int in_fork_child;          /* indication that this is a fork child */
    redisDb *db;
    dict *commands;             /* Command table */
    dict *orig_commands;        /* Command table before command renaming. */
    aeEventLoop *el;
    rax *errors;                /* Errors table */
    redisAtomic unsigned int lruclock; /* Clock for LRU eviction */
    volatile sig_atomic_t shutdown_asap; /* Shutdown ordered by signal handler. */
    mstime_t shutdown_mstime;   /* Timestamp to limit graceful shutdown. */
    int last_sig_received;      /* Indicates the last SIGNAL received, if any (e.g., SIGINT or SIGTERM). */
    int shutdown_flags;         /* Flags passed to prepareForShutdown(). */
    int activerehashing;        /* Incremental rehash in serverCron() */
    int active_defrag_running;  /* Active defragmentation running (holds current scan aggressiveness) */
    char *pidfile;              /* PID file path */
    int arch_bits;              /* 32 or 64 depending on sizeof(long) */
    int cronloops;              /* Number of times the cron function run */
    char runid[CONFIG_RUN_ID_SIZE+1];  /* ID always different at every exec. */
    int sentinel_mode;          /* True if this instance is a Sentinel. */
    size_t initial_memory_usage; /* Bytes used after initialization. */
    int always_show_logo;       /* Show logo even for non-stdout logging. */
    int in_exec;                /* Are we inside EXEC? */
    int busy_module_yield_flags;         /* Are we inside a busy module? (triggered by RM_Yield). see BUSY_MODULE_YIELD_ flags. */
    const char *busy_module_yield_reply; /* When non-null, we are inside RM_Yield. */
    int core_propagates;        /* Is the core (in oppose to the module subsystem) is in charge of calling propagatePendingCommands? */
    int propagate_no_multi;     /* True if propagatePendingCommands should avoid wrapping command in MULTI/EXEC */
    int module_ctx_nesting;     /* moduleCreateContext() nesting level */
    char *ignore_warnings;      /* Config: warnings that should be ignored. */
    int client_pause_in_transaction; /* Was a client pause executed during this Exec? */
    int thp_enabled;                 /* If true, THP is enabled. */
    size_t page_size;                /* The page size of OS. */
    /* Modules */
    dict *moduleapi;            /* Exported core APIs dictionary for modules. */
    dict *sharedapi;            /* Like moduleapi but containing the APIs that
                                   modules share with each other. */
    dict *module_configs_queue; /* Dict that stores module configurations from .conf file until after modules are loaded during startup or arguments to loadex. */
    list *loadmodule_queue;     /* List of modules to load at startup. */
    int module_pipe[2];         /* Pipe used to awake the event loop by module threads. */
    pid_t child_pid;            /* PID of current child */
    int child_type;             /* Type of current child */
    /* Networking */
    int port;                   /* TCP listening port */
    int tls_port;               /* TLS listening port */
    int tcp_backlog;            /* TCP listen() backlog */
    char *bindaddr[CONFIG_BINDADDR_MAX]; /* Addresses we should bind to */
    int bindaddr_count;         /* Number of addresses in server.bindaddr[] */
    char *bind_source_addr;     /* Source address to bind on for outgoing connections */
    char *unixsocket;           /* UNIX socket path */
    unsigned int unixsocketperm; /* UNIX socket permission (see mode_t) */
    socketFds ipfd;             /* TCP socket file descriptors */
    socketFds tlsfd;            /* TLS socket file descriptors */
    int sofd;                   /* Unix socket file descriptor */
    uint32_t socket_mark_id;    /* ID for listen socket marking */
    socketFds cfd;              /* Cluster bus listening socket */
    list *clients;              /* List of active clients */
    list *clients_to_close;     /* Clients to close asynchronously */
    list *clients_pending_write; /* There is to write or install handler. */
    list *clients_pending_read;  /* Client has pending read socket buffers. */
    list *slaves, *monitors;    /* List of slaves and MONITORs */
    client *current_client;     /* Current client executing the command. */

    /* Stuff for client mem eviction */
    clientMemUsageBucket client_mem_usage_buckets[CLIENT_MEM_USAGE_BUCKETS];

    rax *clients_timeout_table; /* Radix tree for blocked clients timeouts. */
    long fixed_time_expire;     /* If > 0, expire keys against server.mstime. */
    int in_nested_call;         /* If > 0, in a nested call of a call */
    rax *clients_index;         /* Active clients dictionary by client ID. */
    pause_type client_pause_type;      /* True if clients are currently paused */
    list *postponed_clients;       /* List of postponed clients */
    mstime_t client_pause_end_time;    /* Time when we undo clients_paused */
    pause_event *client_pause_per_purpose[NUM_PAUSE_PURPOSES];
    char neterr[ANET_ERR_LEN];   /* Error buffer for anet.c */
    dict *migrate_cached_sockets;/* MIGRATE cached sockets */
    redisAtomic uint64_t next_client_id; /* Next client unique ID. Incremental. */
    int protected_mode;         /* Don't accept external connections. */
    int io_threads_num;         /* Number of IO threads to use. */
    int io_threads_do_reads;    /* Read and parse from IO threads? */
    int io_threads_active;      /* Is IO threads currently active? */
    long long events_processed_while_blocked; /* processEventsWhileBlocked() */
    int enable_protected_configs;    /* Enable the modification of protected configs, see PROTECTED_ACTION_ALLOWED_* */
    int enable_debug_cmd;            /* Enable DEBUG commands, see PROTECTED_ACTION_ALLOWED_* */
    int enable_module_cmd;           /* Enable MODULE commands, see PROTECTED_ACTION_ALLOWED_* */

    /* RDB / AOF loading information */
    volatile sig_atomic_t loading; /* We are loading data from disk if true */
    volatile sig_atomic_t async_loading; /* We are loading data without blocking the db being served */
    off_t loading_total_bytes;
    off_t loading_rdb_used_mem;
    off_t loading_loaded_bytes;
    time_t loading_start_time;
    off_t loading_process_events_interval_bytes;
    /* Fields used only for stats */
    time_t stat_starttime;          /* Server start time */
    long long stat_numcommands;     /* Number of processed commands */
    long long stat_numconnections;  /* Number of connections received */
    long long stat_expiredkeys;     /* Number of expired keys */
    double stat_expired_stale_perc; /* Percentage of keys probably expired */
    long long stat_expired_time_cap_reached_count; /* Early expire cycle stops.*/
    long long stat_expire_cycle_time_used; /* Cumulative microseconds used. */
    long long stat_evictedkeys;     /* Number of evicted keys (maxmemory) */
    long long stat_evictedclients;  /* Number of evicted clients */
    long long stat_total_eviction_exceeded_time;  /* Total time over the memory limit, unit us */
    monotime stat_last_eviction_exceeded_time;  /* Timestamp of current eviction start, unit us */
    long long stat_keyspace_hits;   /* Number of successful lookups of keys */
    long long stat_keyspace_misses; /* Number of failed lookups of keys */
    long long stat_active_defrag_hits;      /* number of allocations moved */
    long long stat_active_defrag_misses;    /* number of allocations scanned but not moved */
    long long stat_active_defrag_key_hits;  /* number of keys with moved allocations */
    long long stat_active_defrag_key_misses;/* number of keys scanned and not moved */
    long long stat_active_defrag_scanned;   /* number of dictEntries scanned */
    long long stat_total_active_defrag_time; /* Total time memory fragmentation over the limit, unit us */
    monotime stat_last_active_defrag_time; /* Timestamp of current active defrag start */
    size_t stat_peak_memory;        /* Max used memory record */
    long long stat_aof_rewrites;    /* number of aof file rewrites performed */
    long long stat_aofrw_consecutive_failures; /* The number of consecutive failures of aofrw */
    long long stat_rdb_saves;       /* number of rdb saves performed */
    long long stat_fork_time;       /* Time needed to perform latest fork() */
    double stat_fork_rate;          /* Fork rate in GB/sec. */
    long long stat_total_forks;     /* Total count of fork. */
    long long stat_rejected_conn;   /* Clients rejected because of maxclients */
    long long stat_sync_full;       /* Number of full resyncs with slaves. */
    long long stat_sync_partial_ok; /* Number of accepted PSYNC requests. */
    long long stat_sync_partial_err;/* Number of unaccepted PSYNC requests. */
    list *slowlog;                  /* SLOWLOG list of commands */
    long long slowlog_entry_id;     /* SLOWLOG current entry ID */
    long long slowlog_log_slower_than; /* SLOWLOG time limit (to get logged) */
    unsigned long slowlog_max_len;     /* SLOWLOG max number of items logged */
    struct malloc_stats cron_malloc_stats; /* sampled in serverCron(). */
    redisAtomic long long stat_net_input_bytes; /* Bytes read from network. */
    redisAtomic long long stat_net_output_bytes; /* Bytes written to network. */
    redisAtomic long long stat_net_repl_input_bytes; /* Bytes read during replication, added to stat_net_input_bytes in 'info'. */
    redisAtomic long long stat_net_repl_output_bytes; /* Bytes written during replication, added to stat_net_output_bytes in 'info'. */
    size_t stat_current_cow_peak;   /* Peak size of copy on write bytes. */
    size_t stat_current_cow_bytes;  /* Copy on write bytes while child is active. */
    monotime stat_current_cow_updated;  /* Last update time of stat_current_cow_bytes */
    size_t stat_current_save_keys_processed;  /* Processed keys while child is active. */
    size_t stat_current_save_keys_total;  /* Number of keys when child started. */
    size_t stat_rdb_cow_bytes;      /* Copy on write bytes during RDB saving. */
    size_t stat_aof_cow_bytes;      /* Copy on write bytes during AOF rewrite. */
    size_t stat_module_cow_bytes;   /* Copy on write bytes during module fork. */
    double stat_module_progress;   /* Module save progress. */
    size_t stat_clients_type_memory[CLIENT_TYPE_COUNT];/* Mem usage by type */
    size_t stat_cluster_links_memory; /* Mem usage by cluster links */
    long long stat_unexpected_error_replies; /* Number of unexpected (aof-loading, replica to master, etc.) error replies */
    long long stat_total_error_replies; /* Total number of issued error replies ( command + rejected errors ) */
    long long stat_dump_payload_sanitizations; /* Number deep dump payloads integrity validations. */
    long long stat_io_reads_processed; /* Number of read events processed by IO / Main threads */
    long long stat_io_writes_processed; /* Number of write events processed by IO / Main threads */
    redisAtomic long long stat_total_reads_processed; /* Total number of read events processed */
    redisAtomic long long stat_total_writes_processed; /* Total number of write events processed */
    /* The following two are used to track instantaneous metrics, like
     * number of operations per second, network traffic. */
    struct {
        long long last_sample_time; /* Timestamp of last sample in ms */
        long long last_sample_count;/* Count in last sample */
        long long samples[STATS_METRIC_SAMPLES];
        int idx;
    } inst_metric[STATS_METRIC_COUNT];
    long long stat_reply_buffer_shrinks; /* Total number of output buffer shrinks */
    long long stat_reply_buffer_expands; /* Total number of output buffer expands */

    /* Configuration */
    int verbosity;                  /* Loglevel in redis.conf */
    int maxidletime;                /* Client timeout in seconds */
    int tcpkeepalive;               /* Set SO_KEEPALIVE if non-zero. */
    int active_expire_enabled;      /* Can be disabled for testing purposes. */
    int active_expire_effort;       /* From 1 (default) to 10, active effort. */
    int active_defrag_enabled;
    int sanitize_dump_payload;      /* Enables deep sanitization for ziplist and listpack in RDB and RESTORE. */
    int skip_checksum_validation;   /* Disable checksum validation for RDB and RESTORE payload. */
    int jemalloc_bg_thread;         /* Enable jemalloc background thread */
    size_t active_defrag_ignore_bytes; /* minimum amount of fragmentation waste to start active defrag */
    int active_defrag_threshold_lower; /* minimum percentage of fragmentation to start active defrag */
    int active_defrag_threshold_upper; /* maximum percentage of fragmentation at which we use maximum effort */
    int active_defrag_cycle_min;       /* minimal effort for defrag in CPU percentage */
    int active_defrag_cycle_max;       /* maximal effort for defrag in CPU percentage */
    unsigned long active_defrag_max_scan_fields; /* maximum number of fields of set/hash/zset/list to process from within the main dict scan */
    size_t client_max_querybuf_len; /* Limit for client query buffer length */
    int dbnum;                      /* Total number of configured DBs */
    int supervised;                 /* 1 if supervised, 0 otherwise. */
    int supervised_mode;            /* See SUPERVISED_* */
    int daemonize;                  /* True if running as a daemon */
    int set_proc_title;             /* True if change proc title */
    char *proc_title_template;      /* Process title template format */
    clientBufferLimitsConfig client_obuf_limits[CLIENT_TYPE_OBUF_COUNT];
    int pause_cron;                 /* Don't run cron tasks (debug) */
    int latency_tracking_enabled;   /* 1 if extended latency tracking is enabled, 0 otherwise. */
    double *latency_tracking_info_percentiles; /* Extended latency tracking info output percentile list configuration. */
    int latency_tracking_info_percentiles_len;
    /* AOF persistence */
    int aof_enabled;                /* AOF configuration */
    int aof_state;                  /* AOF_(ON|OFF|WAIT_REWRITE) */
    int aof_fsync;                  /* Kind of fsync() policy */
    char *aof_filename;             /* Basename of the AOF file and manifest file */
    char *aof_dirname;              /* Name of the AOF directory */
    int aof_no_fsync_on_rewrite;    /* Don't fsync if a rewrite is in prog. */
    int aof_rewrite_perc;           /* Rewrite AOF if % growth is > M and... */
    off_t aof_rewrite_min_size;     /* the AOF file is at least N bytes. */
    off_t aof_rewrite_base_size;    /* AOF size on latest startup or rewrite. */
    off_t aof_current_size;         /* AOF current size (Including BASE + INCRs). */
    off_t aof_last_incr_size;       /* The size of the latest incr AOF. */
    off_t aof_fsync_offset;         /* AOF offset which is already synced to disk. */
    int aof_flush_sleep;            /* Micros to sleep before flush. (used by tests) */
    int aof_rewrite_scheduled;      /* Rewrite once BGSAVE terminates. */
    sds aof_buf;      /* AOF buffer, written before entering the event loop */
    int aof_fd;       /* File descriptor of currently selected AOF file */
    int aof_selected_db; /* Currently selected DB in AOF */
    time_t aof_flush_postponed_start; /* UNIX time of postponed AOF flush */
    time_t aof_last_fsync;            /* UNIX time of last fsync() */
    time_t aof_rewrite_time_last;   /* Time used by last AOF rewrite run. */
    time_t aof_rewrite_time_start;  /* Current AOF rewrite start time. */
    time_t aof_cur_timestamp;       /* Current record timestamp in AOF */
    int aof_timestamp_enabled;      /* Enable record timestamp in AOF */
    int aof_lastbgrewrite_status;   /* C_OK or C_ERR */
    unsigned long aof_delayed_fsync;  /* delayed AOF fsync() counter */
    int aof_rewrite_incremental_fsync;/* fsync incrementally while aof rewriting? */
    int rdb_save_incremental_fsync;   /* fsync incrementally while rdb saving? */
    int aof_last_write_status;      /* C_OK or C_ERR */
    int aof_last_write_errno;       /* Valid if aof write/fsync status is ERR */
    int aof_load_truncated;         /* Don't stop on unexpected AOF EOF. */
    int aof_use_rdb_preamble;       /* Specify base AOF to use RDB encoding on AOF rewrites. */
    redisAtomic int aof_bio_fsync_status; /* Status of AOF fsync in bio job. */
    redisAtomic int aof_bio_fsync_errno;  /* Errno of AOF fsync in bio job. */
    aofManifest *aof_manifest;       /* Used to track AOFs. */
    int aof_disable_auto_gc;         /* If disable automatically deleting HISTORY type AOFs?
                                        default no. (for testings). */

    /* RDB persistence */
    long long dirty;                /* Changes to DB from the last save 上次保存后对 DB 的更改*/
    long long dirty_before_bgsave;  /* Used to restore dirty on failed BGSAVE 用于在失败的 BGSAVE 上恢复脏*/
    long long rdb_last_load_keys_expired;  /* number of expired keys when loading RDB 加载 RDB 时过期键的数量*/
    long long rdb_last_load_keys_loaded;   /* number of loaded keys when loading RDB 加载 RDB 时加载的键数*/
    struct saveparam *saveparams;   /* Save points array for RDB */
    int saveparamslen;              /* Number of saving points 保存点数*/
    char *rdb_filename;             /* Name of RDB file */
    int rdb_compression;            /* Use compression in RDB? */
    int rdb_checksum;               /* Use RDB checksum? */
    int rdb_del_sync_files;         /* Remove RDB files used only for SYNC if
                                       the instance does not use persistence.
                                       如果实例不使用持久性，则删除仅用于 SYNC 的 RDB 文件。*/
    time_t lastsave;                /* Unix time of last successful save 上次成功保存的 Unix 时间*/
    time_t lastbgsave_try;          /* Unix time of last attempted bgsave 上次尝试 bgsave 的 Unix 时间*/
    time_t rdb_save_time_last;      /* Time used by last RDB save run. 上次 RDB 保存运行所用的时间。*/
    time_t rdb_save_time_start;     /* Current RDB save start time.当前 RDB 保存开始时间。 */
    int rdb_bgsave_scheduled;       /* BGSAVE when possible if true. 如果为真，则尽可能 BGSAVE。*/
    int rdb_child_type;             /* Type of save by active child. 活动孩子的保存类型。*/
    int lastbgsave_status;          /* C_OK or C_ERR */
    int stop_writes_on_bgsave_err;  /* Don't allow writes if can't BGSAVE 如果不能 BGSAVE，则不允许写入*/
    int rdb_pipe_read;              /* RDB pipe used to transfer the rdb data 用于传输 rdb 数据的 RDB 管道*/
                                    /* to the parent process in diskless repl. 到无盘repl中的父进程。*/
    int rdb_child_exit_pipe;        /* Used by the diskless parent allow child exit. 由无盘父级使用，允许子级退出。*/
    connection **rdb_pipe_conns;    /* Connections which are currently the 当前的连接*/
    int rdb_pipe_numconns;          /* target of diskless rdb fork child. 无盘 rdb fork child 的目标。*/
    int rdb_pipe_numconns_writing;  /* Number of rdb conns with pending writes. 具有挂起写入的 rdb conns 数。*/
    char *rdb_pipe_buff;            /* In diskless replication, this buffer holds data 在无盘复制中，此缓冲区保存数据*/
    int rdb_pipe_bufflen;           /* that was read from the rdb pipe. 这是从 rdb 管道中读取的。*/
    int rdb_key_save_delay;         /* Delay in microseconds between keys while
                                     * writing the RDB. (for testings). negative
                                     * value means fractions of microseconds (on average).
                                     * 写入 RDB 时键之间的延迟（以微秒为单位）。 （用于测试）。负值表示微秒的分数（平均）。*/

    int key_load_delay;             /* Delay in microseconds between keys while
                                     * loading aof or rdb. (for testings). negative
                                     * value means fractions of microseconds (on average).
                                     * 加载 aof 和 rdb 时键之间的延迟（以微秒为单位）。 （用于检测）。
                                     * 负值表示微秒的分数（平均）。*/
    /* Pipe and data structures for child -> parent info sharing. */
    //child -> parent 信息共享的管道和数据结构。
    int child_info_pipe[2];         /* Pipe used to write the child_info_data. 用于写入 child_info_data 的管道。*/
    int child_info_nread;           /* Num of bytes of the last read from pipe 上次从管道读取的字节数*/
    /* Propagation of commands in AOF / replication */
    //AOF 复制中的命令传播
    redisOpArray also_propagate;    /* Additional command to propagate. 要传播的附加命令。*/
    int replication_allowed;        /* Are we allowed to replicate? 我们可以复制吗？*/
    /* Logging */
    char *logfile;                  /* Path of log file 日志文件路径*/
    int syslog_enabled;             /* Is syslog enabled? 系统日志是否启用？*/
    char *syslog_ident;             /* Syslog ident 系统日志标识*/
    int syslog_facility;            /* Syslog facility 系统日志工具*/
    int crashlog_enabled;           /* Enable signal handler for crashlog.
                                     * disable for clean core dumps.
                                     * 为崩溃日志启用信号处理程序。禁用干净的核心转储。*/
    int memcheck_enabled;           /* Enable memory check on crash. 启用崩溃时的内存检查。*/
    int use_exit_on_panic;          /* Use exit() on panic and assert rather than
                                     * abort(). useful for Valgrind.
                                     * 在 panic 和 assert 时使用 exit() 而不是 abort()。对 Valgrind 有用。*/
    /* Shutdown */
    int shutdown_timeout;           /* Graceful shutdown time limit in seconds.以秒为单位的正常关机时间限制。 */
    int shutdown_on_sigint;         /* Shutdown flags configured for SIGINT. */
    int shutdown_on_sigterm;        /* Shutdown flags configured for SIGTERM. 为 SIGTERM 配置的关闭标志。*/

    /* Replication (master) */
    char replid[CONFIG_RUN_ID_SIZE+1];  /* My current replication ID. 我当前的复制 ID。*/
    char replid2[CONFIG_RUN_ID_SIZE+1]; /* replid inherited from master 从 master 继承的 replid*/
    long long master_repl_offset;   /* My current replication offset 我当前的复制偏移量*/
    long long second_replid_offset; /* Accept offsets up to this for replid2. 接受 replid2 的偏移量。*/
    int slaveseldb;                 /* Last SELECTed DB in replication output 复制输出中的最后一个 SELECTed DB*/
    int repl_ping_slave_period;     /* Master pings the slave every N seconds 主服务器每 N 秒 ping 一次从服务器*/
    replBacklog *repl_backlog;      /* Replication backlog for partial syncs 部分同步的复制积压*/
    long long repl_backlog_size;    /* Backlog circular buffer size 积压循环缓冲区大小*/
    time_t repl_backlog_time_limit; /* Time without slaves after the backlog
                                       gets released. 积压工作被释放后没有slave的时间。*/
    time_t repl_no_slaves_since;    /* We have no slaves since that time.
                                       Only valid if server.slaves len is 0.
                                       从那时起，我们就没有slave了。仅当 server.slaves len 为 0 时才有效。*/
    int repl_min_slaves_to_write;   /* Min number of slaves to write. 要写入的slave的最小数量。*/
    int repl_min_slaves_max_lag;    /* Max lag of <count> slaves to write. <count> 个slave的最大延迟写入。*/
    int repl_good_slaves_count;     /* Number of slaves with lag <= max_lag. 滞后 <= max_lag 的从站数量。*/
    int repl_diskless_sync;         /* Master send RDB to slaves sockets directly. Master 将 RDB 直接发送到 slave 套接字。*/
    int repl_diskless_load;         /* Slave parse RDB directly from the socket.
                                     * see REPL_DISKLESS_LOAD_* enum
                                     * Slave 直接从套接字解析 RDB。见 REPL_DISKLESS_LOAD_ 枚举*/
    int repl_diskless_sync_delay;   /* Delay to start a diskless repl BGSAVE. 延迟启动无盘 repl BGSAVE。*/
    int repl_diskless_sync_max_replicas;/* Max replicas for diskless repl BGSAVE
                                         * delay (start sooner if they all connect).
                                         * 无盘 repl BGSAVE 延迟的最大副本数（如果它们都连接，则启动更快）。*/
    size_t repl_buffer_mem;         /* The memory of replication buffer. 复制缓冲区的内存。*/
    list *repl_buffer_blocks;       /* Replication buffers blocks list
                                     * (serving replica clients and repl backlog)
                                     * 复制缓冲区块列表（服务副本客户端和 repl backlog）*/
    /* Replication (slave) */
    char *masteruser;               /* AUTH with this user and masterauth with master 使用此用户进行身份验证，使用 master 进行 masterauth*/
    sds masterauth;                 /* AUTH with this password with master 使用此密码与 master 进行身份验证*/
    char *masterhost;               /* Hostname of master */
    int masterport;                 /* Port of master */
    int repl_timeout;               /* Timeout after N seconds of master idle 主机空闲 N 秒后超时*/
    client *master;     /* Client that is master for this slave 作为该从属设备的主设备的客户端*/
    client *cached_master; /* Cached master to be reused for PSYNC. 缓存的 master 将被重用于 PSYNC。*/
    int repl_syncio_timeout; /* Timeout for synchronous I/O calls 同步 IO 调用超时*/
    int repl_state;          /* Replication status if the instance is a slave 如果实例是slave实例，则复制状态*/
    off_t repl_transfer_size; /* Size of RDB to read from master during sync. 同步期间要从 master 读取的 RDB 的大小。*/
    off_t repl_transfer_read; /* Amount of RDB read from master during sync. 同步期间从 master 读取的 RDB 量。*/
    off_t repl_transfer_last_fsync_off; /* Offset when we fsync-ed last time. 我们上次 fsync 时的偏移量。*/
    connection *repl_transfer_s;     /* Slave -> Master SYNC connection */
    int repl_transfer_fd;    /* Slave -> Master SYNC temp file descriptor */
    char *repl_transfer_tmpfile; /* Slave-> master SYNC temp file name    slave-> master SYNC 临时文件名*/
    time_t repl_transfer_lastio; /* Unix time of the latest read, for timeout 最后一次读取的 Unix 时间，用于超时*/
    int repl_serve_stale_data; /* Serve stale data when link is down? 链接断开时提供陈旧数据？*/
    int repl_slave_ro;          /* Slave is read only?  slave是只读的？*/
    int repl_slave_ignore_maxmemory;    /* If true slaves do not evict.如果真正的slave不驱逐。 */
    time_t repl_down_since; /* Unix time at which link with master went down 与 master 的链接断开的 Unix 时间*/
    int repl_disable_tcp_nodelay;   /* Disable TCP_NODELAY after SYNC? SYNC 后禁用 TCP_NODELAY？*/
    int slave_priority;             /* Reported in INFO and used by Sentinel. 在 INFO 中报告并由 Sentinel 使用。*/
    int replica_announced;          /* If true, replica is announced by Sentinel 如果为真，副本由哨兵宣布*/
    int slave_announce_port;        /* Give the master this listening port. */
    char *slave_announce_ip;        /* Give the master this ip address. 给master这个IP地址。*/
    int propagation_error_behavior; /* Configures the behavior of the replica
                                     * when it receives an error on the replication stream
                                     * 配置副本在复制流上收到错误时的行为*/
    int repl_ignore_disk_write_error;   /* Configures whether replicas panic when unable to
                                         * persist writes to AOF.
                                         * 配置在无法持久写入 AOF 时副本是否恐慌。*/
    /* The following two fields is where we store master PSYNC replid/offset
     * while the PSYNC is in progress. At the end we'll copy the fields into
     * the server->master client structure. */
    //以下两个字段是我们在 PSYNC 进行时存储主 PSYNC replidoffset 的地方。最后，我们将这些字段复制到服务器-> 主客户端结构中。
    char master_replid[CONFIG_RUN_ID_SIZE+1];  /* Master PSYNC runid. */
    long long master_initial_offset;           /* Master PSYNC offset. */
    int repl_slave_lazy_flush;          /* Lazy FLUSHALL before loading DB? 加载数据库之前延迟刷新？*/
    /* Synchronous replication.同步复制。 */
    list *clients_waiting_acks;         /* Clients waiting in WAIT command. 客户端在等待命令中等待。*/
    int get_ack_from_slaves;            /* If true we send REPLCONF GETACK. 如果为真，我们发送 REPLCONF GETACK。*/
    /* Limits */
    unsigned int maxclients;            /* Max number of simultaneous clients 最大同时客户端数*/
    unsigned long long maxmemory;   /* Max number of memory bytes to use 要使用的最大内存字节数*/
    ssize_t maxmemory_clients;       /* Memory limit for total client buffers 总客户端缓冲区的内存限制*/
    int maxmemory_policy;           /* Policy for key eviction 密钥驱逐政策*/
    int maxmemory_samples;          /* Precision of random sampling 随机抽样精度*/
    int maxmemory_eviction_tenacity;/* Aggressiveness of eviction processing 驱逐处理的积极性*/
    int lfu_log_factor;             /* LFU logarithmic counter factor. LFU 对数计数器因子。*/
    int lfu_decay_time;             /* LFU counter decay factor.LFU 计数器衰减因子。 */
    long long proto_max_bulk_len;   /* Protocol bulk length maximum size. 协议批量长度最大大小。*/
    int oom_score_adj_values[CONFIG_OOM_COUNT];   /* Linux oom_score_adj configuration   Linux oom_score_adj 配置*/
    int oom_score_adj;                            /* If true, oom_score_adj is managed 如果为 true，则管理 oom_score_adj*/
    int disable_thp;                              /* If true, disable THP by syscall 如果为真，则通过系统调用禁用 THP*/
    /* Blocked clients 被阻止的客户*/
    unsigned int blocked_clients;   /* # of clients executing a blocking cmd.执行阻塞 cmd 的客户端。*/
    unsigned int blocked_clients_by_type[BLOCKED_NUM];
    list *unblocked_clients; /* list of clients to unblock before next loop 在下一个循环之前解除阻塞的客户端列表*/
    list *ready_keys;        /* List of readyList structures for BLPOP & co.   BLPOP & co 的 readyList 结构列表*/
    /* Client side caching. 客户端缓存。*/
    unsigned int tracking_clients;  /* # of clients with tracking enabled.启用跟踪的客户。*/
    size_t tracking_table_max_keys; /* Max number of keys in tracking table. 跟踪表中的最大键数。*/
    list *tracking_pending_keys; /* tracking invalidation keys pending to flush 跟踪待刷新的失效键 */
    /* Sort parameters - qsort_r() is only available under BSD so we
     * have to take this state global, in order to pass it to sortCompare() */
    //排序参数 - qsort_r() 仅在 BSD 下可用，因此我们必须将此状态全局化，以便将其传递给 sortCompare()
    int sort_desc;
    int sort_alpha;
    int sort_bypattern;
    int sort_store;
    /* Zip structure config, see redis.conf for more information  */
    //Zip 结构配置，更多信息见 redis.conf
    size_t hash_max_listpack_entries;
    size_t hash_max_listpack_value;
    size_t set_max_intset_entries;
    size_t zset_max_listpack_entries;
    size_t zset_max_listpack_value;
    size_t hll_sparse_max_bytes;
    size_t stream_node_max_bytes;
    long long stream_node_max_entries;
    /* List parameters */
    int list_max_listpack_size;
    int list_compress_depth;
    /* time cache */
    redisAtomic time_t unixtime; /* Unix time sampled every cron cycle. Unix 时间对每个 cron 周期进行采样。*/
    time_t timezone;            /* Cached timezone. As set by tzset(). 缓存时区。由 tzset() 设置。*/
    int daylight_active;        /* Currently in daylight saving time. 目前处于夏令时。*/
    mstime_t mstime;            /* 'unixtime' in milliseconds. */
    ustime_t ustime;            /* 'unixtime' in microseconds. */
    size_t blocking_op_nesting; /* Nesting level of blocking operation, used to reset blocked_last_cron.
 *                                 阻塞操作的嵌套级别，用于重置blocked_last_cron。*/
    long long blocked_last_cron; /* Indicate the mstime of the last time we did cron jobs from a blocking operation
 *                                  指示我们上次从阻塞操作中执行 cron 作业的 mstime*/
    /* Pubsub */
    dict *pubsub_channels;  /* Map channels to list of subscribed clients 将频道映射到订阅客户端列表*/
    dict *pubsub_patterns;  /* A dict of pubsub_patterns   pubsub_patterns 的字典*/
    int notify_keyspace_events; /* Events to propagate via Pub/Sub. This is an
                                   xor of NOTIFY_... flags. 通过 PubSub 传播的事件。这是 NOTIFY_... 标志的异或。*/
    dict *pubsubshard_channels;  /* Map shard channels to list of subscribed clients 将分片通道映射到订阅客户端列表*/
    /* Cluster */
    int cluster_enabled;      /* Is cluster enabled? */
    int cluster_port;         /* Set the cluster port for a node. 设置节点的集群端口。*/
    mstime_t cluster_node_timeout; /* Cluster node timeout. */
    char *cluster_configfile; /* Cluster auto-generated config file name. 集群自动生成的配置文件名。*/
    struct clusterState *cluster;  /* State of the cluster */
    int cluster_migration_barrier; /* Cluster replicas migration barrier. 集群副本迁移障碍。*/
    int cluster_allow_replica_migration; /* Automatic replica migrations to orphaned masters and
 *                                          from empty masters 自动副本迁移到孤立的 master 和空的 master*/
    int cluster_slave_validity_factor; /* Slave max data age for failover. 故障转移的从站最大数据年龄。*/
    int cluster_require_full_coverage; /* If true, put the cluster down if
                                          there is at least an uncovered slot.
                                          如果为真，如果至少有一个未覆盖的插槽，则将集群放下。*/
    int cluster_slave_no_failover;  /* Prevent slave from starting a failover
                                       if the master is in failure state.
                                       如果主服务器处于故障状态，则防止从服务器启动故障转移。*/
    char *cluster_announce_ip;  /* IP address to announce on cluster bus. */
    char *cluster_announce_hostname;  /* hostname to announce on cluster bus. */
    int cluster_preferred_endpoint_type; /* Use the announced hostname when available.可用时使用宣布的主机名。 */
    int cluster_announce_port;     /* base port to announce on cluster bus. */
    int cluster_announce_tls_port; /* TLS port to announce on cluster bus. */
    int cluster_announce_bus_port; /* bus port to announce on cluster bus. 在集群总线上宣布的总线端口。*/
    int cluster_module_flags;      /* Set of flags that Redis modules are able
                                      to set in order to suppress certain
                                      native Redis Cluster features. Check the
                                      REDISMODULE_CLUSTER_FLAG_*.
                                      Redis 模块可以设置的一组标志，以抑制某些本机 Redis 集群功能。
                                      检查 REDISMODULE_CLUSTER_FLAG_。*/
    int cluster_allow_reads_when_down; /* Are reads allowed when the cluster
                                        is down? 集群关闭时是否允许读取？*/
    int cluster_config_file_lock_fd;   /* cluster config fd, will be flock    cluster config fd，将是flock*/
    unsigned long long cluster_link_sendbuf_limit_bytes;  /* Memory usage limit on individual link send buffers
 *                                                           单个链接发送缓冲区的内存使用限制*/
    int cluster_drop_packet_filter; /* Debug config that allows tactically
                                   * dropping packets of a specific type
                                   * 调试配置，允许战术性地丢弃特定类型的数据包*/
    /* Scripting */
    client *script_caller;       /* The client running script right now, or NULL 客户端正在运行的脚本，或 NULL*/
    mstime_t busy_reply_threshold;  /* Script / module timeout in milliseconds 脚本/模块超时（以毫秒为单位）*/
    int pre_command_oom_state;         /* OOM before command (script?) was started 启动命令（脚本？）之前的 OOM*/
    int script_disable_deny_script;    /* Allow running commands marked "no-script" inside a script.
 *                                        允许在脚本中运行标记为“no-script”的命令。*/
    /* Lazy free */
    int lazyfree_lazy_eviction;
    int lazyfree_lazy_expire;
    int lazyfree_lazy_server_del;
    int lazyfree_lazy_user_del;
    int lazyfree_lazy_user_flush;
    /* Latency monitor 延迟监视器*/
    long long latency_monitor_threshold;
    dict *latency_events;
    /* ACLs */
    char *acl_filename;           /* ACL Users file. NULL if not configured. ACL 用户文件。如果未配置，则为 NULL。*/
    unsigned long acllog_max_len; /* Maximum length of the ACL LOG list. ACL LOG 列表的最大长度。*/
    sds requirepass;              /* Remember the cleartext password set with
                                     the old "requirepass" directive for
                                     backward compatibility with Redis <= 5.
                                     记住使用旧的“requirepass”指令设置的明文密码，以便向后兼容 Redis <= 5。*/
    int acl_pubsub_default;      /* Default ACL pub/sub channels flag 默认 ACL 发布订阅通道标志*/
    /* Assert & bug reporting */
    int watchdog_period;  /* Software watchdog period in ms. 0 = off 软件看门狗周期，以 ms 为单位。 0 = 关闭*/
    /* System hardware info */
    size_t system_memory_size;  /* Total memory in system as reported by OS 操作系统报告的系统总内存*/
    /* TLS Configuration */
    int tls_cluster;
    int tls_replication;
    int tls_auth_clients;
    redisTLSContextConfig tls_ctx_config;
    /* cpu affinity */
    char *server_cpulist; /* cpu affinity list of redis server main/io thread. redis 服务器 mainio 线程的 cpu 亲和性列表。*/
    char *bio_cpulist; /* cpu affinity list of bio thread. 生物线程的 cpu 亲和性列表。*/
    char *aof_rewrite_cpulist; /* cpu affinity list of aof rewrite process. aof 重写进程的 cpu 亲和性列表。*/
    char *bgsave_cpulist; /* cpu affinity list of bgsave process.  bgsave 进程的 cpu 亲和性列表。*/
    /* Sentinel config */
    struct sentinelConfig *sentinel_config; /* sentinel config to load at startup time. 在启动时加载的哨兵配置。*/
    /* Coordinate failover info 协调故障转移信息*/
    mstime_t failover_end_time; /* Deadline for failover command. 故障转移命令的截止日期。*/
    int force_failover; /* If true then failover will be forced at the
                         * deadline, otherwise failover is aborted.
                         * 如果为 true，则将在截止日期前强制进行故障转移，否则将中止故障转移。*/
    char *target_replica_host; /* Failover target host. If null during a
                                * failover then any replica can be used.
                                * 故障转移目标主机。如果在故障转移期间为 null，则可以使用任何副本。*/
    int target_replica_port; /* Failover target port */
    int failover_state; /* Failover state */
    int cluster_allow_pubsubshard_when_down; /* Is pubsubshard allowed when the cluster
                                                is down, doesn't affect pubsub global.
                                                集群关闭时是否允许 pubsubshard，不影响 pubsub 全局。*/
    long reply_buffer_peak_reset_time; /* The amount of time (in milliseconds) to wait between
 *                                        reply buffer peak resets  在回复缓冲区峰值重置之间等待的时间量（以毫秒为单位） */
    int reply_buffer_resizing_enabled; /* Is reply buffer resizing enabled (1 by default) 是否启用回复缓冲区调整大小（默认为 1）*/
};

#define MAX_KEYS_BUFFER 256

typedef struct {
    int pos; /* The position of the key within the client array 客户端数组中键的位置*/
    int flags; /* The flags associated with the key access, see
                  CMD_KEY_* for more information
                  与密钥访问关联的标志，请参阅 CMD_KEY_ 了解更多信息*/
} keyReference;

/* A result structure for the various getkeys function calls. It lists the
 * keys as indices to the provided argv. This functionality is also re-used
 * for returning channel information.
 */
//各种 getkeys 函数调用的结果结构。它将键列为提供的 argv 的索引。此功能也可重新用于返回频道信息。
typedef struct {
    keyReference keysbuf[MAX_KEYS_BUFFER];/* Pre-allocated buffer, to save heap allocations 预分配缓冲区，以节省堆分配*/
    keyReference *keys;                  /* Key indices array, points to keysbuf or heap 键索引数组，指向键缓冲区或堆*/
    int numkeys;                        /* Number of key indices return 返回的关键索引数量*/
    int size;                           /* Available array size 可用数组大小*/
} getKeysResult;
#define GETKEYS_RESULT_INIT { {{0}}, NULL, 0, MAX_KEYS_BUFFER }

/* Key specs definitions.
 *
 * Brief: This is a scheme that tries to describe the location
 * of key arguments better than the old [first,last,step] scheme
 * which is limited and doesn't fit many commands.
 *
 * There are two steps:
 * 1. begin_search (BS): in which index should we start searching for keys?
 * 2. find_keys (FK): relative to the output of BS, how can we will which args are keys?
 *
 * There are two types of BS:
 * 1. index: key args start at a constant index
 * 2. keyword: key args start just after a specific keyword
 *
 * There are two kinds of FK:
 * 1. range: keys end at a specific index (or relative to the last argument)
 * 2. keynum: there's an arg that contains the number of key args somewhere before the keys themselves
 */
/**
 * 关键规格定义。
 * 简介：这个方案试图比旧的 [first,last,step] 方案更好地描述关键参数的位置，
 * 旧的 [first,last,step] 方案是有限的并且不适合许多命令。
 * 有两个步骤：
 *   1. begin_search (BS)：我们应该从哪个索引开始搜索键？
 *   2. find_keys(FK)：相对于BS的输出，我们怎么知道哪些args是key？
 * BS 有两种类型：
 *   1. index：key args 从一个常量索引开始
 *   2.keyword：key args 在特定关键字之后开始
 * FK 有两种：
 *   1. range：key 在特定索引（或相对到最后一个参数）
 *   2. keynum：有一个 arg 包含键本身之前某处的键 args 的数量
 * */

/* Must be synced with generate-command-code.py */
//必须与 generate-command-code.py 同步
typedef enum {
    KSPEC_BS_INVALID = 0, /* Must be 0 */
    KSPEC_BS_UNKNOWN,
    KSPEC_BS_INDEX,
    KSPEC_BS_KEYWORD
} kspec_bs_type;

/* Must be synced with generate-command-code.py */
//必须与 generate-command-code.py 同步
typedef enum {
    KSPEC_FK_INVALID = 0, /* Must be 0 */
    KSPEC_FK_UNKNOWN,
    KSPEC_FK_RANGE,
    KSPEC_FK_KEYNUM
} kspec_fk_type;

typedef struct {
    /* Declarative data */
    const char *notes;
    uint64_t flags;
    kspec_bs_type begin_search_type;
    union {
        struct {
            /* The index from which we start the search for keys */
            //我们开始搜索键的索引
            int pos;
        } index;
        struct {
            /* The keyword that indicates the beginning of key args */
            //表示 key args 开头的关键字
            const char *keyword;
            /* An index in argv from which to start searching.
             * Can be negative, which means start search from the end, in reverse
             * (Example: -2 means to start in reverse from the penultimate arg) */
             //argv 中的索引，从中开始搜索。可以为负数，表示从末尾开始搜索，反向（示例：-2 表示从倒数第二个 arg 开始反向）
            int startfrom;
        } keyword;
    } bs;
    kspec_fk_type find_keys_type;
    union {
        /* NOTE: Indices in this struct are relative to the result of the begin_search step!
         * These are: range.lastkey, keynum.keynumidx, keynum.firstkey */
        //注意：此结构中的索引与 begin_search 步骤的结果相关！它们是：range.lastkey、keynum.keynumidx、keynum.firstkey
        struct {
            /* Index of the last key.
             * Can be negative, in which case it's not relative. -1 indicating till the last argument,
             * -2 one before the last and so on. */
            //最后一个键的索引。可以是负数，在这种情况下它不是相对的。
            // -1 表示直到最后一个参数，-2 在最后一个参数之前，依此类推。
            int lastkey;
            /* How many args should we skip after finding a key, in order to find the next one. */
            //找到一个键后我们应该跳过多少个参数，以便找到下一个。
            int keystep;
            /* If lastkey is -1, we use limit to stop the search by a factor. 0 and 1 mean no limit.
             * 2 means 1/2 of the remaining args, 3 means 1/3, and so on. */
            //如果 lastkey 为 -1，我们使用 limit 来停止搜索。 0 和 1 表示没有限制。
            // 2 表示剩余 args 中的 12 个，3 表示 13 个，依此类推。
            int limit;
        } range;
        struct {
            /* Index of the argument containing the number of keys to come */
            //包含要到来的键数的参数索引
            int keynumidx;
            /* Index of the fist key (Usually it's just after keynumidx, in
             * which case it should be set to keynumidx+1). */
            //第一个键的索引（通常在keynumidx之后，在这种情况下应该设置为keynumidx+1）。
            int firstkey;
            /* How many args should we skip after finding a key, in order to find the next one. */
            //找到一个键后我们应该跳过多少个参数，以便找到下一个。
            int keystep;
        } keynum;
    } fk;
} keySpec;

/* Number of static key specs 静态键规格数*/
#define STATIC_KEY_SPECS_NUM 4

/* Must be synced with ARG_TYPE_STR and generate-command-code.py */
//必须与 ARG_TYPE_STR 和 generate-command-code.py 同步
typedef enum {
    ARG_TYPE_STRING,
    ARG_TYPE_INTEGER,
    ARG_TYPE_DOUBLE,
    ARG_TYPE_KEY, /* A string, but represents a keyname 一个字符串，但代表一个键名*/
    ARG_TYPE_PATTERN,
    ARG_TYPE_UNIX_TIME,
    ARG_TYPE_PURE_TOKEN,
    ARG_TYPE_ONEOF, /* Has subargs 有子参数*/
    ARG_TYPE_BLOCK /* Has subargs */
} redisCommandArgType;

#define CMD_ARG_NONE            (0)
#define CMD_ARG_OPTIONAL        (1<<0)
#define CMD_ARG_MULTIPLE        (1<<1)
#define CMD_ARG_MULTIPLE_TOKEN  (1<<2)

typedef struct redisCommandArg {
    const char *name;
    redisCommandArgType type;
    int key_spec_index;
    const char *token;
    const char *summary;
    const char *since;
    int flags;
    const char *deprecated_since;
    struct redisCommandArg *subargs;
    /* runtime populated data 运行时填充数据*/
    int num_args;
} redisCommandArg;

/* Must be synced with RESP2_TYPE_STR and generate-command-code.py */
//必须与 RESP2_TYPE_STR 和 generate-command-code.py 同步
typedef enum {
    RESP2_SIMPLE_STRING,
    RESP2_ERROR,
    RESP2_INTEGER,
    RESP2_BULK_STRING,
    RESP2_NULL_BULK_STRING,
    RESP2_ARRAY,
    RESP2_NULL_ARRAY,
} redisCommandRESP2Type;

/* Must be synced with RESP3_TYPE_STR and generate-command-code.py */
//必须与 RESP3_TYPE_STR 和 generate-command-code.py 同步
typedef enum {
    RESP3_SIMPLE_STRING,
    RESP3_ERROR,
    RESP3_INTEGER,
    RESP3_DOUBLE,
    RESP3_BULK_STRING,
    RESP3_ARRAY,
    RESP3_MAP,
    RESP3_SET,
    RESP3_BOOL,
    RESP3_NULL,
} redisCommandRESP3Type;

typedef struct {
    const char *since;
    const char *changes;
} commandHistory;

/* Must be synced with COMMAND_GROUP_STR and generate-command-code.py */
//必须与 COMMAND_GROUP_STR 和 generate-command-code.py 同步
typedef enum {
    COMMAND_GROUP_GENERIC,
    COMMAND_GROUP_STRING,
    COMMAND_GROUP_LIST,
    COMMAND_GROUP_SET,
    COMMAND_GROUP_SORTED_SET,
    COMMAND_GROUP_HASH,
    COMMAND_GROUP_PUBSUB,
    COMMAND_GROUP_TRANSACTIONS,
    COMMAND_GROUP_CONNECTION,
    COMMAND_GROUP_SERVER,
    COMMAND_GROUP_SCRIPTING,
    COMMAND_GROUP_HYPERLOGLOG,
    COMMAND_GROUP_CLUSTER,
    COMMAND_GROUP_SENTINEL,
    COMMAND_GROUP_GEO,
    COMMAND_GROUP_STREAM,
    COMMAND_GROUP_BITMAP,
    COMMAND_GROUP_MODULE,
} redisCommandGroup;

typedef void redisCommandProc(client *c);
typedef int redisGetKeysProc(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);

/* Redis command structure.
 *
 * Note that the command table is in commands.c and it is auto-generated.
 *
 * This is the meaning of the flags:
 *
 * CMD_WRITE:       Write command (may modify the key space).
 *
 * CMD_READONLY:    Commands just reading from keys without changing the content.
 *                  Note that commands that don't read from the keyspace such as
 *                  TIME, SELECT, INFO, administrative commands, and connection
 *                  or transaction related commands (multi, exec, discard, ...)
 *                  are not flagged as read-only commands, since they affect the
 *                  server or the connection in other ways.
 *
 * CMD_DENYOOM:     May increase memory usage once called. Don't allow if out
 *                  of memory.
 *
 * CMD_ADMIN:       Administrative command, like SAVE or SHUTDOWN.
 *
 * CMD_PUBSUB:      Pub/Sub related command.
 *
 * CMD_NOSCRIPT:    Command not allowed in scripts.
 *
 * CMD_BLOCKING:    The command has the potential to block the client.
 *
 * CMD_LOADING:     Allow the command while loading the database.
 *
 * CMD_NO_ASYNC_LOADING: Deny during async loading (when a replica uses diskless
 *                       sync swapdb, and allows access to the old dataset)
 *
 * CMD_STALE:       Allow the command while a slave has stale data but is not
 *                  allowed to serve this data. Normally no command is accepted
 *                  in this condition but just a few.
 *
 * CMD_SKIP_MONITOR:  Do not automatically propagate the command on MONITOR.
 *
 * CMD_SKIP_SLOWLOG:  Do not automatically propagate the command to the slowlog.
 *
 * CMD_ASKING:      Perform an implicit ASKING for this command, so the
 *                  command will be accepted in cluster mode if the slot is marked
 *                  as 'importing'.
 *
 * CMD_FAST:        Fast command: O(1) or O(log(N)) command that should never
 *                  delay its execution as long as the kernel scheduler is giving
 *                  us time. Note that commands that may trigger a DEL as a side
 *                  effect (like SET) are not fast commands.
 *
 * CMD_NO_AUTH:     Command doesn't require authentication
 *
 * CMD_MAY_REPLICATE:   Command may produce replication traffic, but should be
 *                      allowed under circumstances where write commands are disallowed.
 *                      Examples include PUBLISH, which replicates pubsub messages,and
 *                      EVAL, which may execute write commands, which are replicated,
 *                      or may just execute read commands. A command can not be marked
 *                      both CMD_WRITE and CMD_MAY_REPLICATE
 *
 * CMD_SENTINEL:    This command is present in sentinel mode.
 *
 * CMD_ONLY_SENTINEL: This command is present only when in sentinel mode.
 *                    And should be removed from redis.
 *
 * CMD_NO_MANDATORY_KEYS: This key arguments for this command are optional.
 *
 * CMD_NO_MULTI: The command is not allowed inside a transaction
 *
 * The following additional flags are only used in order to put commands
 * in a specific ACL category. Commands can have multiple ACL categories.
 * See redis.conf for the exact meaning of each.
 *
 * @keyspace, @read, @write, @set, @sortedset, @list, @hash, @string, @bitmap,
 * @hyperloglog, @stream, @admin, @fast, @slow, @pubsub, @blocking, @dangerous,
 * @connection, @transaction, @scripting, @geo.
 *
 * Note that:
 *
 * 1) The read-only flag implies the @read ACL category.
 * 2) The write flag implies the @write ACL category.
 * 3) The fast flag implies the @fast ACL category.
 * 4) The admin flag implies the @admin and @dangerous ACL category.
 * 5) The pub-sub flag implies the @pubsub ACL category.
 * 6) The lack of fast flag implies the @slow ACL category.
 * 7) The non obvious "keyspace" category includes the commands
 *    that interact with keys without having anything to do with
 *    specific data structures, such as: DEL, RENAME, MOVE, SELECT,
 *    TYPE, EXPIRE*, PEXPIRE*, TTL, PTTL, ...
 */
/**
 * Redis 命令结构。请注意，命令表在 commands.c 中并且是自动生成的。
 * 这是标志的含义：
 * CMD_WRITE：写命令（可能修改键空间）。
 * CMD_READONLY：仅从键读取而不更改内容的命令。
 *   请注意，不从键空间读取的命令，例如 TIME、SELECT、INFO、管理命令
 *   以及与连接或事务相关的命令（multi、exec、discard...）不会被标记为只读命令，因为它们以其他方式影响服务器或连接。
 * CMD_DENYOOM：调用后可能会增加内存使用量。如果内存不足，请不要允许。
 * CMD_ADMIN：管理命令，如 SAVE 或 SHUTDOWN。
 * CMD_PUBSUB：PubSub 相关命令。
 * CMD_NOSCRIPT：脚本中不允许的命令。
 * CMD_BLOCKING：该命令有可能阻塞客户端。
 * CMD_LOADING：在加载数据库时允许该命令。
 * CMD_NO_ASYNC_LOADING：在异步加载期间拒绝（当副本使用无盘同步 swapdb，并允许访问旧数据集时）
 *   通常在这种情况下不接受任何命令，而只接受一些命令。
 * CMD_SKIP_MONITOR：不要在 MONITOR 上自动传播命令。
 * CMD_SKIP_SLOWLOG：不要自动将命令传播到慢日志。
 * CMD_ASKING：对这个命令执行一个隐式的 ASKING，所以如果槽被标记为“importing”，这个命令将在集群模式下被接受。
 * CMD_FAST：快速命令：O(1) 或 O(log(N)) 命令，只要内核调度程序给我们时间，就不会延迟其执行。
 *   请注意，可能触发 DEL 作为副作用的命令（如 SET）不是快速命令。
 * CMD_NO_AUTH：命令不需要身份验证
 * CMD_MAY_REPLICATE：命令可能会产生复制流量，但在不允许写入命令的情况下应该允许。
 *   示例包括 PUBLISH，它复制 pubsub 消息，以及 EVAL，它可以执行写入命令，这些命令被复制，或者可能只执行读取命令。
 *   不能同时标记 CMD_WRITE 和 CMD_MAY_REPLICATE CMD_SENTINEL 的命令：此命令存在于哨兵模式中。
 * CMD_ONLY_SENTINEL：此命令仅在哨兵模式下存在。并且应该从redis中删除。
 * CMD_NO_MANDATORY_KEYS：此命令的关键参数是可选的。
 * CMD_NO_MULTI：事务中不允许该命令 以下附加标志仅用于将命令放入特定 ACL 类别。命令可以有多个 ACL 类别。
 *   有关每个的确切含义，请参见 redis.conf。
 *   @keyspace、@read、@write、@set、@sortedset、@list、@hash、@string、@bitmap、
 *   @hyperloglog、@stream、@admin、@fast、@slow、@pubsub、@blocking、@dangerous 、
 *   @connection、@transaction、@scripting、@geo。
 *   注意：
 *   1) 只读标志暗示@read ACL 类别。
 *   2) write 标志暗示@write ACL 类别。
 *   3) fast 标志暗示@fast ACL 类别。
 *   4) admin 标志暗示@admin 和@dangerous ACL 类别。
 *   5) pub-sub 标志暗示@pubsub ACL 类别。
 *   6) 缺少快速标志意味着@slow ACL 类别。
 *   7）不明显的“键空间”类别包括与键交互而与特定数据结构无关的命令，例如：
 *     DEL，RENAME，MOVE，SELECT，TYPE，EXPIRE，PEXPIRE，TTL，PTTL，...
 * */
struct redisCommand {
    /* Declarative data 声明性数据*/
    const char *declared_name; /* A string representing the command declared_name.
                                * It is a const char * for native commands and SDS for module commands.
                                * 表示命令声明名称的字符串。它是原生命令的 const char 和模块命令的 SDS。*/
    const char *summary; /* Summary of the command (optional). 命令摘要（可选）。*/
    const char *complexity; /* Complexity description (optional). 复杂性描述（可选）。*/
    const char *since; /* Debut version of the command (optional). 命令的首次亮相版本（可选）。*/
    int doc_flags; /* Flags for documentation (see CMD_DOC_*). 文档标志（参见 CMD_DOC_）。*/
    const char *replaced_by; /* In case the command is deprecated, this is the successor command.
 *                              如果不推荐使用该命令，则这是后续命令。*/
    const char *deprecated_since; /* In case the command is deprecated, when did it happen?
 *                                   如果该命令被弃用，它是什么时候发生的？*/
    redisCommandGroup group; /* Command group 命令组*/
    commandHistory *history; /* History of the command 命令的历史 */
    const char **tips; /* An array of strings that are meant to be tips
 *                     for clients/proxies regarding this command
 *                     一个字符串数组，旨在作为有关此命令的客户端代理的提示*/
    redisCommandProc *proc; /* Command implementation 命令实现*/
    int arity; /* Number of arguments, it is possible to use -N to say >= N  参数个数，可以用 -N 表示 >= N*/
    uint64_t flags; /* Command flags, see CMD_*. 命令标志，见 CMD_。*/
    uint64_t acl_categories; /* ACl categories, see ACL_CATEGORY_*. ACl 类别，请参阅 ACL_CATEGORY_。*/
    keySpec key_specs_static[STATIC_KEY_SPECS_NUM]; /* Key specs. See keySpec key规格。见 keySpec*/
    /* Use a function to determine keys arguments in a command line.
     * Used for Redis Cluster redirect (may be NULL)
     * 使用函数来确定命令行中的键参数。用于 Redis 集群重定向（可能为 NULL）*/
    redisGetKeysProc *getkeys_proc;
    /* Array of subcommands (may be NULL) 子命令数组（可能为 NULL）*/
    struct redisCommand *subcommands;
    /* Array of arguments (may be NULL) 参数数组（可能为 NULL）*/
    struct redisCommandArg *args;

    /* Runtime populated data 运行时填充数据*/
    long long microseconds, calls, rejected_calls, failed_calls;
    int id;     /* Command ID. This is a progressive ID starting from 0 that
                   is assigned at runtime, and is used in order to check
                   ACLs. A connection is able to execute a given command if
                   the user associated to the connection has this command
                   bit set in the bitmap of allowed commands.
                   命令标识。这是一个从 0 开始的渐进式 ID，在运行时分配，
                   用于检查 ACL。如果与连接关联的用户在允许命令的位图中设置了该命令位，
                   则该连接能够执行给定命令。*/
    sds fullname; /* A SDS string representing the command fullname.
 *                   表示命令全名的 SDS 字符串。*/
    struct hdr_histogram* latency_histogram; /*points to the command latency command histogram (unit of time nanosecond)
 *                                             指向命令延迟命令直方图（时间单位纳秒）*/
    keySpec *key_specs;
    keySpec legacy_range_key_spec; /* The legacy (first,last,step) key spec is
                                     * still maintained (if applicable) so that
                                     * we can still support the reply format of
                                     * COMMAND INFO and COMMAND GETKEYS
                                     * 遗留的 (first,last,step) 密钥规范仍然保留（如果适用），
                                     * 因此我们仍然可以支持 COMMAND INFO 和 COMMAND GETKEYS
                                     * 的回复格式*/
    int num_args;
    int num_history;
    int num_tips;
    int key_specs_num;
    int key_specs_max;
    dict *subcommands_dict; /* A dictionary that holds the subcommands, the key is the subcommand sds name
                             * (not the fullname), and the value is the redisCommand structure pointer.
                             * 保存子命令的字典，键是子命令 sds 名称（不是全名），值是 redisCommand 结构指针。*/
    struct redisCommand *parent;
    struct RedisModuleCommand *module_cmd; /* A pointer to the module command data (NULL if native command)
 *                                         指向模块命令数据的指针（如果是本机命令，则为 NULL）*/
};

struct redisError {
    long long count;
};

struct redisFunctionSym {
    char *name;
    unsigned long pointer;
};

typedef struct _redisSortObject {
    robj *obj;
    union {
        double score;
        robj *cmpobj;
    } u;
} redisSortObject;

typedef struct _redisSortOperation {
    int type;
    robj *pattern;
} redisSortOperation;

/* Structure to hold list iteration abstraction. 保存列表迭代抽象的结构。*/
typedef struct {
    robj *subject;
    unsigned char encoding;
    unsigned char direction; /* Iteration direction 迭代方向 */
    quicklistIter *iter;
} listTypeIterator;

/* Structure for an entry while iterating over a list. */
//迭代列表时条目的结构。
typedef struct {
    listTypeIterator *li;
    quicklistEntry entry; /* Entry in quicklist 快速列表中的条目*/
} listTypeEntry;

/* Structure to hold set iteration abstraction. */
//保存集合迭代抽象的结构。
typedef struct {
    robj *subject;
    int encoding;
    int ii; /* intset iterator 整数迭代器*/
    dictIterator *di;
} setTypeIterator;

/* Structure to hold hash iteration abstraction. Note that iteration over
 * hashes involves both fields and values. Because it is possible that
 * not both are required, store pointers in the iterator to avoid
 * unnecessary memory allocation for fields/values. */
/**
 * 保存哈希迭代抽象的结构。请注意，对哈希的迭代涉及字段和值。
 * 因为可能并非两者都需要，所以将指针存储在迭代器中以避免为字段值分配不必要的内存。
 * */
typedef struct {
    robj *subject;
    int encoding;

    unsigned char *fptr, *vptr;

    dictIterator *di;
    dictEntry *de;
} hashTypeIterator;

#include "stream.h"  /* Stream data type header file.流数据类型头文件。 */

#define OBJ_HASH_KEY 1
#define OBJ_HASH_VALUE 2

#define IO_THREADS_OP_IDLE 0
#define IO_THREADS_OP_READ 1
#define IO_THREADS_OP_WRITE 2
extern int io_threads_op;

/*-----------------------------------------------------------------------------
 * Extern declarations
 *----------------------------------------------------------------------------*/

extern struct redisServer server;
extern struct sharedObjectsStruct shared;
extern dictType objectKeyPointerValueDictType;
extern dictType objectKeyHeapPointerValueDictType;
extern dictType setDictType;
extern dictType BenchmarkDictType;
extern dictType zsetDictType;
extern dictType dbDictType;
extern double R_Zero, R_PosInf, R_NegInf, R_Nan;
extern dictType hashDictType;
extern dictType stringSetDictType;
extern dictType externalStringType;
extern dictType sdsHashDictType;
extern dictType dbExpiresDictType;
extern dictType modulesDictType;
extern dictType sdsReplyDictType;
extern dict *modules;

/*-----------------------------------------------------------------------------
 * Functions prototypes
 *----------------------------------------------------------------------------*/

/* Command metadata */
void populateCommandLegacyRangeSpec(struct redisCommand *c);
int populateArgsStructure(struct redisCommandArg *args);

/* Modules */
void moduleInitModulesSystem(void);
void moduleInitModulesSystemLast(void);
void modulesCron(void);
int moduleLoad(const char *path, void **argv, int argc, int is_loadex);
int moduleUnload(sds name);
void moduleLoadFromQueue(void);
int moduleGetCommandKeysViaAPI(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
int moduleGetCommandChannelsViaAPI(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
moduleType *moduleTypeLookupModuleByID(uint64_t id);
void moduleTypeNameByID(char *name, uint64_t moduleid);
const char *moduleTypeModuleName(moduleType *mt);
const char *moduleNameFromCommand(struct redisCommand *cmd);
void moduleFreeContext(struct RedisModuleCtx *ctx);
void unblockClientFromModule(client *c);
void moduleHandleBlockedClients(void);
void moduleBlockedClientTimedOut(client *c);
void modulePipeReadable(aeEventLoop *el, int fd, void *privdata, int mask);
size_t moduleCount(void);
void moduleAcquireGIL(void);
int moduleTryAcquireGIL(void);
void moduleReleaseGIL(void);
void moduleNotifyKeyspaceEvent(int type, const char *event, robj *key, int dbid);
void moduleCallCommandFilters(client *c);
void ModuleForkDoneHandler(int exitcode, int bysignal);
int TerminateModuleForkChild(int child_pid, int wait);
ssize_t rdbSaveModulesAux(rio *rdb, int when);
int moduleAllDatatypesHandleErrors();
int moduleAllModulesHandleReplAsyncLoad();
sds modulesCollectInfo(sds info, dict *sections_dict, int for_crash_report, int sections);
void moduleFireServerEvent(uint64_t eid, int subid, void *data);
void processModuleLoadingProgressEvent(int is_aof);
int moduleTryServeClientBlockedOnKey(client *c, robj *key);
void moduleUnblockClient(client *c);
int moduleBlockedClientMayTimeout(client *c);
int moduleClientIsBlockedOnKeys(client *c);
void moduleNotifyUserChanged(client *c);
void moduleNotifyKeyUnlink(robj *key, robj *val, int dbid);
size_t moduleGetFreeEffort(robj *key, robj *val, int dbid);
size_t moduleGetMemUsage(robj *key, robj *val, size_t sample_size, int dbid);
robj *moduleTypeDupOrReply(client *c, robj *fromkey, robj *tokey, int todb, robj *value);
int moduleDefragValue(robj *key, robj *obj, long *defragged, int dbid);
int moduleLateDefrag(robj *key, robj *value, unsigned long *cursor, long long endtime, long long *defragged, int dbid);
long moduleDefragGlobals(void);
void *moduleGetHandleByName(char *modulename);
int moduleIsModuleCommand(void *module_handle, struct redisCommand *cmd);

/* Utils */
long long ustime(void);
long long mstime(void);
void getRandomHexChars(char *p, size_t len);
void getRandomBytes(unsigned char *p, size_t len);
uint64_t crc64(uint64_t crc, const unsigned char *s, uint64_t l);
void exitFromChild(int retcode);
long long redisPopcount(void *s, long count);
int redisSetProcTitle(char *title);
int validateProcTitleTemplate(const char *template);
int redisCommunicateSystemd(const char *sd_notify_msg);
void redisSetCpuAffinity(const char *cpulist);

/* afterErrorReply flags  afterErrorReply 标志*/
#define ERR_REPLY_FLAG_NO_STATS_UPDATE (1ULL<<0) /* Indicating that we should not update
                                                    error stats after sending error reply
                                                    表示我们不应该在发送错误回复后更新错误统计信息*/
/* networking.c -- Networking and Client related operations */
//network.c -- 网络和客户端相关操作
client *createClient(connection *conn);
void freeClient(client *c);
void freeClientAsync(client *c);
void logInvalidUseAndFreeClientAsync(client *c, const char *fmt, ...);
int beforeNextClient(client *c);
void clearClientConnectionState(client *c);
void resetClient(client *c);
void freeClientOriginalArgv(client *c);
void freeClientArgv(client *c);
void sendReplyToClient(connection *conn);
void *addReplyDeferredLen(client *c);
void setDeferredArrayLen(client *c, void *node, long length);
void setDeferredMapLen(client *c, void *node, long length);
void setDeferredSetLen(client *c, void *node, long length);
void setDeferredAttributeLen(client *c, void *node, long length);
void setDeferredPushLen(client *c, void *node, long length);
int processInputBuffer(client *c);
void acceptTcpHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void acceptTLSHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void acceptUnixHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void readQueryFromClient(connection *conn);
int prepareClientToWrite(client *c);
void addReplyNull(client *c);
void addReplyNullArray(client *c);
void addReplyBool(client *c, int b);
void addReplyVerbatim(client *c, const char *s, size_t len, const char *ext);
void addReplyProto(client *c, const char *s, size_t len);
void AddReplyFromClient(client *c, client *src);
void addReplyBulk(client *c, robj *obj);
void addReplyBulkCString(client *c, const char *s);
void addReplyBulkCBuffer(client *c, const void *p, size_t len);
void addReplyBulkLongLong(client *c, long long ll);
void addReply(client *c, robj *obj);
void addReplySds(client *c, sds s);
void addReplyBulkSds(client *c, sds s);
void setDeferredReplyBulkSds(client *c, void *node, sds s);
void addReplyErrorObject(client *c, robj *err);
void addReplyOrErrorObject(client *c, robj *reply);
void afterErrorReply(client *c, const char *s, size_t len, int flags);
void addReplyErrorSdsEx(client *c, sds err, int flags);
void addReplyErrorSds(client *c, sds err);
void addReplyError(client *c, const char *err);
void addReplyErrorArity(client *c);
void addReplyErrorExpireTime(client *c);
void addReplyStatus(client *c, const char *status);
void addReplyDouble(client *c, double d);
void addReplyLongLongWithPrefix(client *c, long long ll, char prefix);
void addReplyBigNum(client *c, const char* num, size_t len);
void addReplyHumanLongDouble(client *c, long double d);
void addReplyLongLong(client *c, long long ll);
void addReplyArrayLen(client *c, long length);
void addReplyMapLen(client *c, long length);
void addReplySetLen(client *c, long length);
void addReplyAttributeLen(client *c, long length);
void addReplyPushLen(client *c, long length);
void addReplyHelp(client *c, const char **help);
void addReplySubcommandSyntaxError(client *c);
void addReplyLoadedModules(client *c);
void copyReplicaOutputBuffer(client *dst, client *src);
void addListRangeReply(client *c, robj *o, long start, long end, int reverse);
void deferredAfterErrorReply(client *c, list *errors);
size_t sdsZmallocSize(sds s);
size_t getStringObjectSdsUsedMemory(robj *o);
void freeClientReplyValue(void *o);
void *dupClientReplyValue(void *o);
char *getClientPeerId(client *client);
char *getClientSockName(client *client);
sds catClientInfoString(sds s, client *client);
sds getAllClientsInfoString(int type);
int clientSetName(client *c, robj *name);
void rewriteClientCommandVector(client *c, int argc, ...);
void rewriteClientCommandArgument(client *c, int i, robj *newval);
void replaceClientCommandVector(client *c, int argc, robj **argv);
void redactClientCommandArgument(client *c, int argc);
size_t getClientOutputBufferMemoryUsage(client *c);
size_t getClientMemoryUsage(client *c, size_t *output_buffer_mem_usage);
int freeClientsInAsyncFreeQueue(void);
int closeClientOnOutputBufferLimitReached(client *c, int async);
int getClientType(client *c);
int getClientTypeByName(char *name);
char *getClientTypeName(int class);
void flushSlavesOutputBuffers(void);
void disconnectSlaves(void);
void evictClients(void);
int listenToPort(int port, socketFds *fds);
void pauseClients(pause_purpose purpose, mstime_t end, pause_type type);
void unpauseClients(pause_purpose purpose);
int areClientsPaused(void);
int checkClientPauseTimeoutAndReturnIfPaused(void);
void unblockPostponedClients();
void processEventsWhileBlocked(void);
void whileBlockedCron();
void blockingOperationStarts();
void blockingOperationEnds();
int handleClientsWithPendingWrites(void);
int handleClientsWithPendingWritesUsingThreads(void);
int handleClientsWithPendingReadsUsingThreads(void);
int stopThreadedIOIfNeeded(void);
int clientHasPendingReplies(client *c);
int islocalClient(client *c);
int updateClientMemUsage(client *c);
void updateClientMemUsageBucket(client *c);
void unlinkClient(client *c);
int writeToClient(client *c, int handler_installed);
void linkClient(client *c);
void protectClient(client *c);
void unprotectClient(client *c);
void initThreadedIO(void);
client *lookupClientByID(uint64_t id);
int authRequired(client *c);
void putClientInPendingWriteQueue(client *c);

#ifdef __GNUC__
void addReplyErrorFormatEx(client *c, int flags, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void addReplyErrorFormat(client *c, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void addReplyStatusFormat(client *c, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
void addReplyErrorFormatEx(client *c, int flags, const char *fmt, ...);
void addReplyErrorFormat(client *c, const char *fmt, ...);
void addReplyStatusFormat(client *c, const char *fmt, ...);
#endif

/* Client side caching (tracking mode) 客户端缓存（跟踪模式）*/
void enableTracking(client *c, uint64_t redirect_to, uint64_t options, robj **prefix, size_t numprefix);
void disableTracking(client *c);
void trackingRememberKeys(client *c);
void trackingInvalidateKey(client *c, robj *keyobj, int bcast);
void trackingScheduleKeyInvalidation(uint64_t client_id, robj *keyobj);
void trackingHandlePendingKeyInvalidations(void);
void trackingInvalidateKeysOnFlush(int async);
void freeTrackingRadixTree(rax *rt);
void freeTrackingRadixTreeAsync(rax *rt);
void trackingLimitUsedSlots(void);
uint64_t trackingGetTotalItems(void);
uint64_t trackingGetTotalKeys(void);
uint64_t trackingGetTotalPrefixes(void);
void trackingBroadcastInvalidationMessages(void);
int checkPrefixCollisionsOrReply(client *c, robj **prefix, size_t numprefix);

/* List data type 列表数据类型*/
void listTypePush(robj *subject, robj *value, int where);
robj *listTypePop(robj *subject, int where);
unsigned long listTypeLength(const robj *subject);
listTypeIterator *listTypeInitIterator(robj *subject, long index, unsigned char direction);
void listTypeReleaseIterator(listTypeIterator *li);
void listTypeSetIteratorDirection(listTypeIterator *li, unsigned char direction);
int listTypeNext(listTypeIterator *li, listTypeEntry *entry);
robj *listTypeGet(listTypeEntry *entry);
void listTypeInsert(listTypeEntry *entry, robj *value, int where);
void listTypeReplace(listTypeEntry *entry, robj *value);
int listTypeEqual(listTypeEntry *entry, robj *o);
void listTypeDelete(listTypeIterator *iter, listTypeEntry *entry);
robj *listTypeDup(robj *o);
int listTypeDelRange(robj *o, long start, long stop);
void unblockClientWaitingData(client *c);
void popGenericCommand(client *c, int where);
void listElementsRemoved(client *c, robj *key, int where, robj *o, long count, int *deleted);

/* MULTI/EXEC/WATCH... */
void unwatchAllKeys(client *c);
void initClientMultiState(client *c);
void freeClientMultiState(client *c);
void queueMultiCommand(client *c, uint64_t cmd_flags);
size_t multiStateMemOverhead(client *c);
void touchWatchedKey(redisDb *db, robj *key);
int isWatchedKeyExpired(client *c);
void touchAllWatchedKeysInDb(redisDb *emptied, redisDb *replaced_with);
void discardTransaction(client *c);
void flagTransaction(client *c);
void execCommandAbort(client *c, sds error);

/* Redis object implementation Redis 对象实现*/
void decrRefCount(robj *o);
void decrRefCountVoid(void *o);
void incrRefCount(robj *o);
robj *makeObjectShared(robj *o);
void freeStringObject(robj *o);
void freeListObject(robj *o);
void freeSetObject(robj *o);
void freeZsetObject(robj *o);
void freeHashObject(robj *o);
void dismissObject(robj *o, size_t dump_size);
robj *createObject(int type, void *ptr);
robj *createStringObject(const char *ptr, size_t len);
robj *createRawStringObject(const char *ptr, size_t len);
robj *createEmbeddedStringObject(const char *ptr, size_t len);
robj *tryCreateRawStringObject(const char *ptr, size_t len);
robj *tryCreateStringObject(const char *ptr, size_t len);
robj *dupStringObject(const robj *o);
int isSdsRepresentableAsLongLong(sds s, long long *llval);
int isObjectRepresentableAsLongLong(robj *o, long long *llongval);
robj *tryObjectEncoding(robj *o);
robj *getDecodedObject(robj *o);
size_t stringObjectLen(robj *o);
robj *createStringObjectFromLongLong(long long value);
robj *createStringObjectFromLongLongForValue(long long value);
robj *createStringObjectFromLongDouble(long double value, int humanfriendly);
robj *createQuicklistObject(void);
robj *createSetObject(void);
robj *createIntsetObject(void);
robj *createHashObject(void);
robj *createZsetObject(void);
robj *createZsetListpackObject(void);
robj *createStreamObject(void);
robj *createModuleObject(moduleType *mt, void *value);
int getLongFromObjectOrReply(client *c, robj *o, long *target, const char *msg);
int getPositiveLongFromObjectOrReply(client *c, robj *o, long *target, const char *msg);
int getRangeLongFromObjectOrReply(client *c, robj *o, long min, long max, long *target, const char *msg);
int checkType(client *c, robj *o, int type);
int getLongLongFromObjectOrReply(client *c, robj *o, long long *target, const char *msg);
int getDoubleFromObjectOrReply(client *c, robj *o, double *target, const char *msg);
int getDoubleFromObject(const robj *o, double *target);
int getLongLongFromObject(robj *o, long long *target);
int getLongDoubleFromObject(robj *o, long double *target);
int getLongDoubleFromObjectOrReply(client *c, robj *o, long double *target, const char *msg);
int getIntFromObjectOrReply(client *c, robj *o, int *target, const char *msg);
char *strEncoding(int encoding);
int compareStringObjects(robj *a, robj *b);
int collateStringObjects(robj *a, robj *b);
int equalStringObjects(robj *a, robj *b);
unsigned long long estimateObjectIdleTime(robj *o);
void trimStringObjectIfNeeded(robj *o);
#define sdsEncodedObject(objptr) (objptr->encoding == OBJ_ENCODING_RAW || objptr->encoding == OBJ_ENCODING_EMBSTR)

/* Synchronous I/O with timeout 带超时的同步 IO*/
ssize_t syncWrite(int fd, char *ptr, ssize_t size, long long timeout);
ssize_t syncRead(int fd, char *ptr, ssize_t size, long long timeout);
ssize_t syncReadLine(int fd, char *ptr, ssize_t size, long long timeout);

/* Replication 复制*/
void replicationFeedSlaves(list *slaves, int dictid, robj **argv, int argc);
void replicationFeedStreamFromMasterStream(char *buf, size_t buflen);
void resetReplicationBuffer(void);
void feedReplicationBuffer(char *buf, size_t len);
void freeReplicaReferencedReplBuffer(client *replica);
void replicationFeedMonitors(client *c, list *monitors, int dictid, robj **argv, int argc);
void updateSlavesWaitingBgsave(int bgsaveerr, int type);
void replicationCron(void);
void replicationStartPendingFork(void);
void replicationHandleMasterDisconnection(void);
void replicationCacheMaster(client *c);
void resizeReplicationBacklog();
void replicationSetMaster(char *ip, int port);
void replicationUnsetMaster(void);
void refreshGoodSlavesCount(void);
int checkGoodReplicasStatus(void);
void processClientsWaitingReplicas(void);
void unblockClientWaitingReplicas(client *c);
int replicationCountAcksByOffset(long long offset);
void replicationSendNewlineToMaster(void);
long long replicationGetSlaveOffset(void);
char *replicationGetSlaveName(client *c);
long long getPsyncInitialOffset(void);
int replicationSetupSlaveForFullResync(client *slave, long long offset);
void changeReplicationId(void);
void clearReplicationId2(void);
void createReplicationBacklog(void);
void freeReplicationBacklog(void);
void replicationCacheMasterUsingMyself(void);
void feedReplicationBacklog(void *ptr, size_t len);
void incrementalTrimReplicationBacklog(size_t blocks);
int canFeedReplicaReplBuffer(client *replica);
void rebaseReplicationBuffer(long long base_repl_offset);
void showLatestBacklog(void);
void rdbPipeReadHandler(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask);
void rdbPipeWriteHandlerConnRemoved(struct connection *conn);
void clearFailoverState(void);
void updateFailoverStatus(void);
void abortFailover(const char *err);
const char *getFailoverStateString();

/* Generic persistence functions 通用持久性函数*/
void startLoadingFile(size_t size, char* filename, int rdbflags);
void startLoading(size_t size, int rdbflags, int async);
void loadingAbsProgress(off_t pos);
void loadingIncrProgress(off_t size);
void stopLoading(int success);
void updateLoadingFileName(char* filename);
void startSaving(int rdbflags);
void stopSaving(int success);
int allPersistenceDisabled(void);

#define DISK_ERROR_TYPE_AOF 1       /* Don't accept writes: AOF errors. 不接受写入：AOF 错误。*/
#define DISK_ERROR_TYPE_RDB 2       /* Don't accept writes: RDB errors.不接受写入：RDB 错误。 */
#define DISK_ERROR_TYPE_NONE 0      /* No problems, we can accept writes. 没问题，我们可以接受写。 */
int writeCommandsDeniedByDiskError(void);
sds writeCommandsGetDiskErrorMessage(int);

/* RDB persistence RDB 持久性*/
#include "rdb.h"
void killRDBChild(void);
int bg_unlink(const char *filename);

/* AOF persistence AOF 持久性*/
void flushAppendOnlyFile(int force);
void feedAppendOnlyFile(int dictid, robj **argv, int argc);
void aofRemoveTempFile(pid_t childpid);
int rewriteAppendOnlyFileBackground(void);
int loadAppendOnlyFiles(aofManifest *am);
void stopAppendOnly(void);
int startAppendOnly(void);
void backgroundRewriteDoneHandler(int exitcode, int bysignal);
ssize_t aofReadDiffFromParent(void);
void killAppendOnlyChild(void);
void restartAOFAfterSYNC();
void aofLoadManifestFromDisk(void);
void aofOpenIfNeededOnServerStart(void);
void aofManifestFree(aofManifest *am);
int aofDelHistoryFiles(void);
int aofRewriteLimited(void);

/* Child info */
void openChildInfoPipe(void);
void closeChildInfoPipe(void);
void sendChildInfoGeneric(childInfoType info_type, size_t keys, double progress, char *pname);
void sendChildCowInfo(childInfoType info_type, char *pname);
void sendChildInfo(childInfoType info_type, size_t keys, char *pname);
void receiveChildInfo(void);

/* Fork helpers */
int redisFork(int purpose);
int hasActiveChildProcess();
void resetChildState();
int isMutuallyExclusiveChildType(int type);

/* acl.c -- Authentication related prototypes. acl.c -- 身份验证相关的原型。*/
extern rax *Users;
extern user *DefaultUser;
void ACLInit(void);
/* Return values for ACLCheckAllPerm(). ACLCheckAllPerm() 的返回值。*/
#define ACL_OK 0
#define ACL_DENIED_CMD 1
#define ACL_DENIED_KEY 2
#define ACL_DENIED_AUTH 3 /* Only used for ACL LOG entries. 仅用于 ACL LOG 条目。*/
#define ACL_DENIED_CHANNEL 4 /* Only used for pub/sub commands 仅用于 pubsub 命令*/

/* Context values for addACLLogEntry(). addACLLogEntry() 的上下文值。*/
#define ACL_LOG_CTX_TOPLEVEL 0
#define ACL_LOG_CTX_LUA 1
#define ACL_LOG_CTX_MULTI 2
#define ACL_LOG_CTX_MODULE 3

/* ACL key permission types ACL 密钥权限类型 */
#define ACL_READ_PERMISSION (1<<0)
#define ACL_WRITE_PERMISSION (1<<1)
#define ACL_ALL_PERMISSION (ACL_READ_PERMISSION|ACL_WRITE_PERMISSION)

int ACLCheckUserCredentials(robj *username, robj *password);
int ACLAuthenticateUser(client *c, robj *username, robj *password);
unsigned long ACLGetCommandID(sds cmdname);
void ACLClearCommandID(void);
user *ACLGetUserByName(const char *name, size_t namelen);
int ACLUserCheckKeyPerm(user *u, const char *key, int keylen, int flags);
int ACLUserCheckChannelPerm(user *u, sds channel, int literal);
int ACLCheckAllUserCommandPerm(user *u, struct redisCommand *cmd, robj **argv, int argc, int *idxptr);
int ACLUserCheckCmdWithUnrestrictedKeyAccess(user *u, struct redisCommand *cmd, robj **argv, int argc, int flags);
int ACLCheckAllPerm(client *c, int *idxptr);
int ACLSetUser(user *u, const char *op, ssize_t oplen);
uint64_t ACLGetCommandCategoryFlagByName(const char *name);
int ACLAppendUserForLoading(sds *argv, int argc, int *argc_err);
const char *ACLSetUserStringError(void);
int ACLLoadConfiguredUsers(void);
sds ACLDescribeUser(user *u);
void ACLLoadUsersAtStartup(void);
void addReplyCommandCategories(client *c, struct redisCommand *cmd);
user *ACLCreateUnlinkedUser();
void ACLFreeUserAndKillClients(user *u);
void addACLLogEntry(client *c, int reason, int context, int argpos, sds username, sds object);
const char* getAclErrorMessage(int acl_res);
void ACLUpdateDefaultUserPassword(sds password);

/* Sorted sets data type */

/* Input flags. */
#define ZADD_IN_NONE 0
#define ZADD_IN_INCR (1<<0)    /* Increment the score instead of setting it. 增加分数而不是设置它。*/
#define ZADD_IN_NX (1<<1)      /* Don't touch elements not already existing.不要触摸不存在的元素。 */
#define ZADD_IN_XX (1<<2)      /* Only touch elements already existing.只有触摸元素已经存在。 */
#define ZADD_IN_GT (1<<3)      /* Only update existing when new scores are higher. 仅在新分数较高时更新现有的。*/
#define ZADD_IN_LT (1<<4)      /* Only update existing when new scores are lower.仅在新分数较低时更新现有的。 */

/* Output flags. */
#define ZADD_OUT_NOP (1<<0)     /* Operation not performed because of conditionals.由于条件，操作未执行。*/
#define ZADD_OUT_NAN (1<<1)     /* Only touch elements already existing. 只有触摸元素已经存在。*/
#define ZADD_OUT_ADDED (1<<2)   /* The element was new and was added.该元素是新的并已添加。 */
#define ZADD_OUT_UPDATED (1<<3) /* The element already existed, score updated.该元素已存在，分数已更新。     */


/* Struct to hold an inclusive/exclusive range spec by score comparison. */
//Struct 通过分数比较来保存包含独占范围规范。
typedef struct {
    double min, max;
    int minex, maxex; /* are min or max exclusive? 最小或最大是专有的吗？*/
} zrangespec;

/* Struct to hold an inclusive/exclusive range spec by lexicographic comparison. */
//通过字典比较来保持包含独占范围规范的结构。
typedef struct {
    sds min, max;     /* May be set to shared.(minstring|maxstring) 可以设置为共享。(minstring|maxstring)*/
    int minex, maxex; /* are min or max exclusive? 最小或最大是专有的吗？*/
} zlexrangespec;

/* flags for incrCommandFailedCalls incrCommandFailedCalls 的标志*/
#define ERROR_COMMAND_REJECTED (1<<0) /* Indicate to update the command rejected stats
 *                                        指示更新命令拒绝的统计信息*/
#define ERROR_COMMAND_FAILED (1<<1) /* Indicate to update the command failed stats
 *                                     指示更新命令失败的统计信息*/

zskiplist *zslCreate(void);
void zslFree(zskiplist *zsl);
zskiplistNode *zslInsert(zskiplist *zsl, double score, sds ele);
unsigned char *zzlInsert(unsigned char *zl, sds ele, double score);
int zslDelete(zskiplist *zsl, double score, sds ele, zskiplistNode **node);
zskiplistNode *zslFirstInRange(zskiplist *zsl, zrangespec *range);
zskiplistNode *zslLastInRange(zskiplist *zsl, zrangespec *range);
double zzlGetScore(unsigned char *sptr);
void zzlNext(unsigned char *zl, unsigned char **eptr, unsigned char **sptr);
void zzlPrev(unsigned char *zl, unsigned char **eptr, unsigned char **sptr);
unsigned char *zzlFirstInRange(unsigned char *zl, zrangespec *range);
unsigned char *zzlLastInRange(unsigned char *zl, zrangespec *range);
unsigned long zsetLength(const robj *zobj);
void zsetConvert(robj *zobj, int encoding);
void zsetConvertToListpackIfNeeded(robj *zobj, size_t maxelelen, size_t totelelen);
int zsetScore(robj *zobj, sds member, double *score);
unsigned long zslGetRank(zskiplist *zsl, double score, sds o);
int zsetAdd(robj *zobj, double score, sds ele, int in_flags, int *out_flags, double *newscore);
long zsetRank(robj *zobj, sds ele, int reverse);
int zsetDel(robj *zobj, sds ele);
robj *zsetDup(robj *o);
void genericZpopCommand(client *c, robj **keyv, int keyc, int where, int emitkey, long count, int use_nested_array, int reply_nil_when_empty, int *deleted);
sds lpGetObject(unsigned char *sptr);
int zslValueGteMin(double value, zrangespec *spec);
int zslValueLteMax(double value, zrangespec *spec);
void zslFreeLexRange(zlexrangespec *spec);
int zslParseLexRange(robj *min, robj *max, zlexrangespec *spec);
unsigned char *zzlFirstInLexRange(unsigned char *zl, zlexrangespec *range);
unsigned char *zzlLastInLexRange(unsigned char *zl, zlexrangespec *range);
zskiplistNode *zslFirstInLexRange(zskiplist *zsl, zlexrangespec *range);
zskiplistNode *zslLastInLexRange(zskiplist *zsl, zlexrangespec *range);
int zzlLexValueGteMin(unsigned char *p, zlexrangespec *spec);
int zzlLexValueLteMax(unsigned char *p, zlexrangespec *spec);
int zslLexValueGteMin(sds value, zlexrangespec *spec);
int zslLexValueLteMax(sds value, zlexrangespec *spec);

/* Core functions */
int getMaxmemoryState(size_t *total, size_t *logical, size_t *tofree, float *level);
size_t freeMemoryGetNotCountedMemory();
int overMaxmemoryAfterAlloc(size_t moremem);
int processCommand(client *c);
int processPendingCommandAndInputBuffer(client *c);
void setupSignalHandlers(void);
void removeSignalHandlers(void);
int createSocketAcceptHandler(socketFds *sfd, aeFileProc *accept_handler);
int changeListenPort(int port, socketFds *sfd, aeFileProc *accept_handler);
int changeBindAddr(void);
struct redisCommand *lookupSubcommand(struct redisCommand *container, sds sub_name);
struct redisCommand *lookupCommand(robj **argv, int argc);
struct redisCommand *lookupCommandBySdsLogic(dict *commands, sds s);
struct redisCommand *lookupCommandBySds(sds s);
struct redisCommand *lookupCommandByCStringLogic(dict *commands, const char *s);
struct redisCommand *lookupCommandByCString(const char *s);
struct redisCommand *lookupCommandOrOriginal(robj **argv, int argc);
int commandCheckExistence(client *c, sds *err);
int commandCheckArity(client *c, sds *err);
void startCommandExecution();
int incrCommandStatsOnError(struct redisCommand *cmd, int flags);
void call(client *c, int flags);
void alsoPropagate(int dbid, robj **argv, int argc, int target);
void propagatePendingCommands();
void redisOpArrayInit(redisOpArray *oa);
void redisOpArrayFree(redisOpArray *oa);
void forceCommandPropagation(client *c, int flags);
void preventCommandPropagation(client *c);
void preventCommandAOF(client *c);
void preventCommandReplication(client *c);
void slowlogPushCurrentCommand(client *c, struct redisCommand *cmd, ustime_t duration);
void updateCommandLatencyHistogram(struct hdr_histogram** latency_histogram, int64_t duration_hist);
int prepareForShutdown(int flags);
void replyToClientsBlockedOnShutdown(void);
int abortShutdown(void);
void afterCommand(client *c);
int mustObeyClient(client *c);
#ifdef __GNUC__
void _serverLog(int level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
void _serverLog(int level, const char *fmt, ...);
#endif
void serverLogRaw(int level, const char *msg);
void serverLogFromHandler(int level, const char *msg);
void usage(void);
void updateDictResizePolicy(void);
int htNeedsResize(dict *dict);
void populateCommandTable(void);
void resetCommandTableStats(dict* commands);
void resetErrorTableStats(void);
void adjustOpenFilesLimit(void);
void incrementErrorCount(const char *fullerr, size_t namelen);
void closeListeningSockets(int unlink_unix_socket);
void updateCachedTime(int update_daylight_info);
void resetServerStats(void);
void activeDefragCycle(void);
unsigned int getLRUClock(void);
unsigned int LRU_CLOCK(void);
const char *evictPolicyToString(void);
struct redisMemOverhead *getMemoryOverheadData(void);
void freeMemoryOverheadData(struct redisMemOverhead *mh);
void checkChildrenDone(void);
int setOOMScoreAdj(int process_class);
void rejectCommandFormat(client *c, const char *fmt, ...);
void *activeDefragAlloc(void *ptr);
robj *activeDefragStringOb(robj* ob, long *defragged);
void dismissSds(sds s);
void dismissMemory(void* ptr, size_t size_hint);
void dismissMemoryInChild(void);

#define RESTART_SERVER_NONE 0
#define RESTART_SERVER_GRACEFULLY (1<<0)     /* Do proper shutdown. 正确关机。*/
#define RESTART_SERVER_CONFIG_REWRITE (1<<1) /* CONFIG REWRITE before restart.重新启动前配置重写。*/
int restartServer(int flags, mstime_t delay);

/* Set data type */
robj *setTypeCreate(sds value);
int setTypeAdd(robj *subject, sds value);
int setTypeRemove(robj *subject, sds value);
int setTypeIsMember(robj *subject, sds value);
setTypeIterator *setTypeInitIterator(robj *subject);
void setTypeReleaseIterator(setTypeIterator *si);
int setTypeNext(setTypeIterator *si, sds *sdsele, int64_t *llele);
sds setTypeNextObject(setTypeIterator *si);
int setTypeRandomElement(robj *setobj, sds *sdsele, int64_t *llele);
unsigned long setTypeRandomElements(robj *set, unsigned long count, robj *aux_set);
unsigned long setTypeSize(const robj *subject);
void setTypeConvert(robj *subject, int enc);
robj *setTypeDup(robj *o);

/* Hash data type */
#define HASH_SET_TAKE_FIELD (1<<0)
#define HASH_SET_TAKE_VALUE (1<<1)
#define HASH_SET_COPY 0

void hashTypeConvert(robj *o, int enc);
void hashTypeTryConversion(robj *subject, robj **argv, int start, int end);
int hashTypeExists(robj *o, sds key);
int hashTypeDelete(robj *o, sds key);
unsigned long hashTypeLength(const robj *o);
hashTypeIterator *hashTypeInitIterator(robj *subject);
void hashTypeReleaseIterator(hashTypeIterator *hi);
int hashTypeNext(hashTypeIterator *hi);
void hashTypeCurrentFromListpack(hashTypeIterator *hi, int what,
                                 unsigned char **vstr,
                                 unsigned int *vlen,
                                 long long *vll);
sds hashTypeCurrentFromHashTable(hashTypeIterator *hi, int what);
void hashTypeCurrentObject(hashTypeIterator *hi, int what, unsigned char **vstr, unsigned int *vlen, long long *vll);
sds hashTypeCurrentObjectNewSds(hashTypeIterator *hi, int what);
robj *hashTypeLookupWriteOrCreate(client *c, robj *key);
robj *hashTypeGetValueObject(robj *o, sds field);
int hashTypeSet(robj *o, sds field, sds value, int flags);
robj *hashTypeDup(robj *o);

/* Pub / Sub */
int pubsubUnsubscribeAllChannels(client *c, int notify);
int pubsubUnsubscribeShardAllChannels(client *c, int notify);
void pubsubUnsubscribeShardChannels(robj **channels, unsigned int count);
int pubsubUnsubscribeAllPatterns(client *c, int notify);
int pubsubPublishMessage(robj *channel, robj *message, int sharded);
int pubsubPublishMessageAndPropagateToCluster(robj *channel, robj *message, int sharded);
void addReplyPubsubMessage(client *c, robj *channel, robj *msg, robj *message_bulk);
int serverPubsubSubscriptionCount();
int serverPubsubShardSubscriptionCount();
size_t pubsubMemOverhead(client *c);

/* Keyspace events notification 键空间事件通知*/
void notifyKeyspaceEvent(int type, char *event, robj *key, int dbid);
int keyspaceEventsStringToFlags(char *classes);
sds keyspaceEventsFlagsToString(int flags);

/* Configuration */
/* Configuration Flags */
//配置标志
#define MODIFIABLE_CONFIG 0 /* This is the implied default for a standard 
                             * config, which is mutable.
                             * 这是标准配置的隐含默认值，它是可变的。*/
#define IMMUTABLE_CONFIG (1ULL<<0) /* Can this value only be set at startup? 这个值只能在启动时设置吗？*/
#define SENSITIVE_CONFIG (1ULL<<1) /* Does this value contain sensitive information 此值是否包含敏感信息*/
#define DEBUG_CONFIG (1ULL<<2) /* Values that are useful for debugging.对调试有用的值。 */
#define MULTI_ARG_CONFIG (1ULL<<3) /* This config receives multiple arguments.此配置接收多个参数。 */
#define HIDDEN_CONFIG (1ULL<<4) /* This config is hidden in `config get <pattern>` (used for tests/debugging)
 *                                 此配置隐藏在 `config get <pattern>` 中（用于测试调试）*/
#define PROTECTED_CONFIG (1ULL<<5) /* Becomes immutable if enable-protected-configs is enabled.
 *                                   如果启用了 enable-protected-configs，则变为不可变。*/
#define DENY_LOADING_CONFIG (1ULL<<6) /* This config is forbidden during loading.加载过程中禁止此配置。 */
#define ALIAS_CONFIG (1ULL<<7) /* For configs with multiple names, this flag is set on the alias.
 *                                   对于具有多个名称的配置，此标志设置在别名上。*/
#define MODULE_CONFIG (1ULL<<8) /* This config is a module config 这个配置是一个模块配置*/
#define VOLATILE_CONFIG (1ULL<<9) /* The config is a reference to the config data and not the config data itself (ex.
                                   * a file name containing more configuration like a tls key). In this case we want
                                   * to apply the configuration change even if the new config value is the same as
                                   * the old. config 是对配置数据的引用，而不是对配置数据本身的引用
                                   * （例如，包含更多配置的文件名，如 tls 键）。
                                   * 在这种情况下，即使新配置值与旧配置值相同，我们也希望应用配置更改。*/

#define INTEGER_CONFIG 0 /* No flags means a simple integer configuration
 *                        没有标志意味着简单的整数配置*/
#define MEMORY_CONFIG (1<<0) /* Indicates if this value can be loaded as a memory value
 *                              指示此值是否可以作为内存值加载*/
#define PERCENT_CONFIG (1<<1) /* Indicates if this value can be loaded as a percent (and stored as a negative int)
 *                               指示此值是否可以加载为百分比（并存储为负整数）*/
#define OCTAL_CONFIG (1<<2) /* This value uses octal representation 该值使用八进制表示*/

/* Enum Configs contain an array of configEnum objects that match a string with an integer. */
//Enum Configs 包含一个 configEnum 对象数组，这些对象将字符串与整数匹配。
typedef struct configEnum {
    char *name;
    int val;
} configEnum;

/* Type of configuration. 配置类型。*/
typedef enum {
    BOOL_CONFIG,
    NUMERIC_CONFIG,
    STRING_CONFIG,
    SDS_CONFIG,
    ENUM_CONFIG,
    SPECIAL_CONFIG,
} configType;

void loadServerConfig(char *filename, char config_from_stdin, char *options);
void appendServerSaveParams(time_t seconds, int changes);
void resetServerSaveParams(void);
struct rewriteConfigState; /* Forward declaration to export API. 转发声明以导出 API。*/
void rewriteConfigRewriteLine(struct rewriteConfigState *state, const char *option, sds line, int force);
void rewriteConfigMarkAsProcessed(struct rewriteConfigState *state, const char *option);
int rewriteConfig(char *path, int force_write);
void initConfigValues();
void removeConfig(sds name);
sds getConfigDebugInfo();
int allowProtectedAction(int config, client *c);

/* Module Configuration */
typedef struct ModuleConfig ModuleConfig;
int performModuleConfigSetFromName(sds name, sds value, const char **err);
int performModuleConfigSetDefaultFromName(sds name, const char **err);
void addModuleBoolConfig(const char *module_name, const char *name, int flags, void *privdata, int default_val);
void addModuleStringConfig(const char *module_name, const char *name, int flags, void *privdata, sds default_val);
void addModuleEnumConfig(const char *module_name, const char *name, int flags, void *privdata, int default_val, configEnum *enum_vals);
void addModuleNumericConfig(const char *module_name, const char *name, int flags, void *privdata, long long default_val, int conf_flags, long long lower, long long upper);
void addModuleConfigApply(list *module_configs, ModuleConfig *module_config);
int moduleConfigApplyConfig(list *module_configs, const char **err, const char **err_arg_name);
int getModuleBoolConfig(ModuleConfig *module_config);
int setModuleBoolConfig(ModuleConfig *config, int val, const char **err);
sds getModuleStringConfig(ModuleConfig *module_config);
int setModuleStringConfig(ModuleConfig *config, sds strval, const char **err);
int getModuleEnumConfig(ModuleConfig *module_config);
int setModuleEnumConfig(ModuleConfig *config, int val, const char **err);
long long getModuleNumericConfig(ModuleConfig *module_config);
int setModuleNumericConfig(ModuleConfig *config, long long val, const char **err);

/* db.c -- Keyspace access API */
int removeExpire(redisDb *db, robj *key);
void deleteExpiredKeyAndPropagate(redisDb *db, robj *keyobj);
void propagateDeletion(redisDb *db, robj *key, int lazy);
int keyIsExpired(redisDb *db, robj *key);
long long getExpire(redisDb *db, robj *key);
void setExpire(client *c, redisDb *db, robj *key, long long when);
int checkAlreadyExpired(long long when);
robj *lookupKeyRead(redisDb *db, robj *key);
robj *lookupKeyWrite(redisDb *db, robj *key);
robj *lookupKeyReadOrReply(client *c, robj *key, robj *reply);
robj *lookupKeyWriteOrReply(client *c, robj *key, robj *reply);
robj *lookupKeyReadWithFlags(redisDb *db, robj *key, int flags);
robj *lookupKeyWriteWithFlags(redisDb *db, robj *key, int flags);
robj *objectCommandLookup(client *c, robj *key);
robj *objectCommandLookupOrReply(client *c, robj *key, robj *reply);
int objectSetLRUOrLFU(robj *val, long long lfu_freq, long long lru_idle,
                       long long lru_clock, int lru_multiplier);
#define LOOKUP_NONE 0
#define LOOKUP_NOTOUCH (1<<0)  /* Don't update LRU. 不要更新 LRU。*/
#define LOOKUP_NONOTIFY (1<<1) /* Don't trigger keyspace event on key misses.不要在键未命中时触发键空间事件。 */
#define LOOKUP_NOSTATS (1<<2)  /* Don't update keyspace hits/misses counters. 不要更新 keyspace hitsmisses 计数器。*/
#define LOOKUP_WRITE (1<<3)    /* Delete expired keys even in replicas.即使在副本中也可以删除过期的密钥。 */

void dbAdd(redisDb *db, robj *key, robj *val);
int dbAddRDBLoad(redisDb *db, sds key, robj *val);
void dbOverwrite(redisDb *db, robj *key, robj *val);

#define SETKEY_KEEPTTL 1
#define SETKEY_NO_SIGNAL 2
#define SETKEY_ALREADY_EXIST 4
#define SETKEY_DOESNT_EXIST 8
void setKey(client *c, redisDb *db, robj *key, robj *val, int flags);
robj *dbRandomKey(redisDb *db);
int dbSyncDelete(redisDb *db, robj *key);
int dbDelete(redisDb *db, robj *key);
robj *dbUnshareStringValue(redisDb *db, robj *key, robj *o);

#define EMPTYDB_NO_FLAGS 0      /* No flags. */
#define EMPTYDB_ASYNC (1<<0)    /* Reclaim memory in another thread. 在另一个线程中回收内存。*/
#define EMPTYDB_NOFUNCTIONS (1<<1) /* Indicate not to flush the functions. 指示不刷新函数。*/
long long emptyData(int dbnum, int flags, void(callback)(dict*));
long long emptyDbStructure(redisDb *dbarray, int dbnum, int async, void(callback)(dict*));
void flushAllDataAndResetRDB(int flags);
long long dbTotalServerKeyCount();
redisDb *initTempDb(void);
void discardTempDb(redisDb *tempDb, void(callback)(dict*));


int selectDb(client *c, int id);
void signalModifiedKey(client *c, redisDb *db, robj *key);
void signalFlushedDb(int dbid, int async);
void scanGenericCommand(client *c, robj *o, unsigned long cursor);
int parseScanCursorOrReply(client *c, robj *o, unsigned long *cursor);
int dbAsyncDelete(redisDb *db, robj *key);
void emptyDbAsync(redisDb *db);
size_t lazyfreeGetPendingObjectsCount(void);
size_t lazyfreeGetFreedObjectsCount(void);
void lazyfreeResetStats(void);
void freeObjAsync(robj *key, robj *obj, int dbid);
void freeReplicationBacklogRefMemAsync(list *blocks, rax *index);

/* API to get key arguments from commands */
//从命令中获取关键参数的 API
#define GET_KEYSPEC_DEFAULT 0
#define GET_KEYSPEC_INCLUDE_NOT_KEYS (1<<0) /* Consider 'fake' keys as keys 将“假”密钥视为密钥*/
#define GET_KEYSPEC_RETURN_PARTIAL (1<<1) /* Return all keys that can be found 返回所有可以找到的键*/

int getKeysFromCommandWithSpecs(struct redisCommand *cmd, robj **argv, int argc, int search_flags, getKeysResult *result);
keyReference *getKeysPrepareResult(getKeysResult *result, int numkeys);
int getKeysFromCommand(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
int doesCommandHaveKeys(struct redisCommand *cmd);
int getChannelsFromCommand(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
int doesCommandHaveChannelsWithFlags(struct redisCommand *cmd, int flags);
void getKeysFreeResult(getKeysResult *result);
int sintercardGetKeys(struct redisCommand *cmd,robj **argv, int argc, getKeysResult *result);
int zunionInterDiffGetKeys(struct redisCommand *cmd,robj **argv, int argc, getKeysResult *result);
int zunionInterDiffStoreGetKeys(struct redisCommand *cmd,robj **argv, int argc, getKeysResult *result);
int evalGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
int functionGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
int sortGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
int sortROGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
int migrateGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
int georadiusGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
int xreadGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
int lmpopGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
int blmpopGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
int zmpopGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
int bzmpopGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
int setGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
int bitfieldGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);

unsigned short crc16(const char *buf, int len);

/* Sentinel */
void initSentinelConfig(void);
void initSentinel(void);
void sentinelTimer(void);
const char *sentinelHandleConfiguration(char **argv, int argc);
void queueSentinelConfig(sds *argv, int argc, int linenum, sds line);
void loadSentinelConfigFromQueue(void);
void sentinelIsRunning(void);
void sentinelCheckConfigFile(void);
void sentinelCommand(client *c);
void sentinelInfoCommand(client *c);
void sentinelPublishCommand(client *c);
void sentinelRoleCommand(client *c);

/* redis-check-rdb & aof */
int redis_check_rdb(char *rdbfilename, FILE *fp);
int redis_check_rdb_main(int argc, char **argv, FILE *fp);
int redis_check_aof_main(int argc, char **argv);

/* Scripting */
void scriptingInit(int setup);
int ldbRemoveChild(pid_t pid);
void ldbKillForkedSessions(void);
int ldbPendingChildren(void);
sds luaCreateFunction(client *c, robj *body);
void luaLdbLineHook(lua_State *lua, lua_Debug *ar);
void freeLuaScriptsAsync(dict *lua_scripts);
void freeFunctionsAsync(functionsLibCtx *lib_ctx);
int ldbIsEnabled();
void ldbLog(sds entry);
void ldbLogRedisReply(char *reply);
void sha1hex(char *digest, char *script, size_t len);
unsigned long evalMemory();
dict* evalScriptsDict();
unsigned long evalScriptsMemory();
uint64_t evalGetCommandFlags(client *c, uint64_t orig_flags);
uint64_t fcallGetCommandFlags(client *c, uint64_t orig_flags);
int isInsideYieldingLongCommand();

typedef struct luaScript {
    uint64_t flags;
    robj *body;
} luaScript;

/* Blocked clients */
//被阻塞的客户
void processUnblockedClients(void);
void blockClient(client *c, int btype);
void unblockClient(client *c);
void queueClientForReprocessing(client *c);
void replyToBlockedClientTimedOut(client *c);
int getTimeoutFromObjectOrReply(client *c, robj *object, mstime_t *timeout, int unit);
void disconnectAllBlockedClients(void);
void handleClientsBlockedOnKeys(void);
void signalKeyAsReady(redisDb *db, robj *key, int type);
void blockForKeys(client *c, int btype, robj **keys, int numkeys, long count, mstime_t timeout, robj *target, struct blockPos *blockpos, streamID *ids);
void updateStatsOnUnblock(client *c, long blocked_us, long reply_us, int had_errors);
void scanDatabaseForDeletedStreams(redisDb *emptied, redisDb *replaced_with);

/* timeout.c -- Blocked clients timeout and connections timeout. */
//timeout.c -- 阻塞客户端超时和连接超时。
void addClientToTimeoutTable(client *c);
void removeClientFromTimeoutTable(client *c);
void handleBlockedClientsTimeout(void);
int clientsCronHandleTimeout(client *c, mstime_t now_ms);

/* expire.c -- Handling of expired keys expire.c -- 过期密钥的处理*/
void activeExpireCycle(int type);
void expireSlaveKeys(void);
void rememberSlaveKeyWithExpire(redisDb *db, robj *key);
void flushSlaveKeysWithExpireList(void);
size_t getSlaveKeyWithExpireCount(void);

/* evict.c -- maxmemory handling and LRU eviction. */
//evict.c -- 最大内存处理和 LRU 驱逐。
void evictionPoolAlloc(void);
#define LFU_INIT_VAL 5
unsigned long LFUGetTimeInMinutes(void);
uint8_t LFULogIncr(uint8_t value);
unsigned long LFUDecrAndReturn(robj *o);
#define EVICT_OK 0
#define EVICT_RUNNING 1
#define EVICT_FAIL 2
int performEvictions(void);
void startEvictionTimeProc(void);

/* Keys hashing / comparison functions for dict.c hash tables. */
//dict.c 哈希表的键哈希比较函数。
uint64_t dictSdsHash(const void *key);
uint64_t dictSdsCaseHash(const void *key);
int dictSdsKeyCompare(dict *d, const void *key1, const void *key2);
int dictSdsKeyCaseCompare(dict *d, const void *key1, const void *key2);
void dictSdsDestructor(dict *d, void *val);
void *dictSdsDup(dict *d, const void *key);

/* Git SHA1 */
char *redisGitSHA1(void);
char *redisGitDirty(void);
uint64_t redisBuildId(void);
char *redisBuildIdString(void);

/* Commands prototypes */
void authCommand(client *c);
void pingCommand(client *c);
void echoCommand(client *c);
void commandCommand(client *c);
void commandCountCommand(client *c);
void commandListCommand(client *c);
void commandInfoCommand(client *c);
void commandGetKeysCommand(client *c);
void commandGetKeysAndFlagsCommand(client *c);
void commandHelpCommand(client *c);
void commandDocsCommand(client *c);
void setCommand(client *c);
void setnxCommand(client *c);
void setexCommand(client *c);
void psetexCommand(client *c);
void getCommand(client *c);
void getexCommand(client *c);
void getdelCommand(client *c);
void delCommand(client *c);
void unlinkCommand(client *c);
void existsCommand(client *c);
void setbitCommand(client *c);
void getbitCommand(client *c);
void bitfieldCommand(client *c);
void bitfieldroCommand(client *c);
void setrangeCommand(client *c);
void getrangeCommand(client *c);
void incrCommand(client *c);
void decrCommand(client *c);
void incrbyCommand(client *c);
void decrbyCommand(client *c);
void incrbyfloatCommand(client *c);
void selectCommand(client *c);
void swapdbCommand(client *c);
void randomkeyCommand(client *c);
void keysCommand(client *c);
void scanCommand(client *c);
void dbsizeCommand(client *c);
void lastsaveCommand(client *c);
void saveCommand(client *c);
void bgsaveCommand(client *c);
void bgrewriteaofCommand(client *c);
void shutdownCommand(client *c);
void slowlogCommand(client *c);
void moveCommand(client *c);
void copyCommand(client *c);
void renameCommand(client *c);
void renamenxCommand(client *c);
void lpushCommand(client *c);
void rpushCommand(client *c);
void lpushxCommand(client *c);
void rpushxCommand(client *c);
void linsertCommand(client *c);
void lpopCommand(client *c);
void rpopCommand(client *c);
void lmpopCommand(client *c);
void llenCommand(client *c);
void lindexCommand(client *c);
void lrangeCommand(client *c);
void ltrimCommand(client *c);
void typeCommand(client *c);
void lsetCommand(client *c);
void saddCommand(client *c);
void sremCommand(client *c);
void smoveCommand(client *c);
void sismemberCommand(client *c);
void smismemberCommand(client *c);
void scardCommand(client *c);
void spopCommand(client *c);
void srandmemberCommand(client *c);
void sinterCommand(client *c);
void sinterCardCommand(client *c);
void sinterstoreCommand(client *c);
void sunionCommand(client *c);
void sunionstoreCommand(client *c);
void sdiffCommand(client *c);
void sdiffstoreCommand(client *c);
void sscanCommand(client *c);
void syncCommand(client *c);
void flushdbCommand(client *c);
void flushallCommand(client *c);
void sortCommand(client *c);
void sortroCommand(client *c);
void lremCommand(client *c);
void lposCommand(client *c);
void rpoplpushCommand(client *c);
void lmoveCommand(client *c);
void infoCommand(client *c);
void mgetCommand(client *c);
void monitorCommand(client *c);
void expireCommand(client *c);
void expireatCommand(client *c);
void pexpireCommand(client *c);
void pexpireatCommand(client *c);
void getsetCommand(client *c);
void ttlCommand(client *c);
void touchCommand(client *c);
void pttlCommand(client *c);
void expiretimeCommand(client *c);
void pexpiretimeCommand(client *c);
void persistCommand(client *c);
void replicaofCommand(client *c);
void roleCommand(client *c);
void debugCommand(client *c);
void msetCommand(client *c);
void msetnxCommand(client *c);
void zaddCommand(client *c);
void zincrbyCommand(client *c);
void zrangeCommand(client *c);
void zrangebyscoreCommand(client *c);
void zrevrangebyscoreCommand(client *c);
void zrangebylexCommand(client *c);
void zrevrangebylexCommand(client *c);
void zcountCommand(client *c);
void zlexcountCommand(client *c);
void zrevrangeCommand(client *c);
void zcardCommand(client *c);
void zremCommand(client *c);
void zscoreCommand(client *c);
void zmscoreCommand(client *c);
void zremrangebyscoreCommand(client *c);
void zremrangebylexCommand(client *c);
void zpopminCommand(client *c);
void zpopmaxCommand(client *c);
void zmpopCommand(client *c);
void bzpopminCommand(client *c);
void bzpopmaxCommand(client *c);
void bzmpopCommand(client *c);
void zrandmemberCommand(client *c);
void multiCommand(client *c);
void execCommand(client *c);
void discardCommand(client *c);
void blpopCommand(client *c);
void brpopCommand(client *c);
void blmpopCommand(client *c);
void brpoplpushCommand(client *c);
void blmoveCommand(client *c);
void appendCommand(client *c);
void strlenCommand(client *c);
void zrankCommand(client *c);
void zrevrankCommand(client *c);
void hsetCommand(client *c);
void hsetnxCommand(client *c);
void hgetCommand(client *c);
void hmgetCommand(client *c);
void hdelCommand(client *c);
void hlenCommand(client *c);
void hstrlenCommand(client *c);
void zremrangebyrankCommand(client *c);
void zunionstoreCommand(client *c);
void zinterstoreCommand(client *c);
void zdiffstoreCommand(client *c);
void zunionCommand(client *c);
void zinterCommand(client *c);
void zinterCardCommand(client *c);
void zrangestoreCommand(client *c);
void zdiffCommand(client *c);
void zscanCommand(client *c);
void hkeysCommand(client *c);
void hvalsCommand(client *c);
void hgetallCommand(client *c);
void hexistsCommand(client *c);
void hscanCommand(client *c);
void hrandfieldCommand(client *c);
void configSetCommand(client *c);
void configGetCommand(client *c);
void configResetStatCommand(client *c);
void configRewriteCommand(client *c);
void configHelpCommand(client *c);
void hincrbyCommand(client *c);
void hincrbyfloatCommand(client *c);
void subscribeCommand(client *c);
void unsubscribeCommand(client *c);
void psubscribeCommand(client *c);
void punsubscribeCommand(client *c);
void publishCommand(client *c);
void pubsubCommand(client *c);
void spublishCommand(client *c);
void ssubscribeCommand(client *c);
void sunsubscribeCommand(client *c);
void watchCommand(client *c);
void unwatchCommand(client *c);
void clusterCommand(client *c);
void restoreCommand(client *c);
void migrateCommand(client *c);
void askingCommand(client *c);
void readonlyCommand(client *c);
void readwriteCommand(client *c);
int verifyDumpPayload(unsigned char *p, size_t len, uint16_t *rdbver_ptr);
void dumpCommand(client *c);
void objectCommand(client *c);
void memoryCommand(client *c);
void clientCommand(client *c);
void helloCommand(client *c);
void evalCommand(client *c);
void evalRoCommand(client *c);
void evalShaCommand(client *c);
void evalShaRoCommand(client *c);
void scriptCommand(client *c);
void fcallCommand(client *c);
void fcallroCommand(client *c);
void functionLoadCommand(client *c);
void functionDeleteCommand(client *c);
void functionKillCommand(client *c);
void functionStatsCommand(client *c);
void functionListCommand(client *c);
void functionHelpCommand(client *c);
void functionFlushCommand(client *c);
void functionRestoreCommand(client *c);
void functionDumpCommand(client *c);
void timeCommand(client *c);
void bitopCommand(client *c);
void bitcountCommand(client *c);
void bitposCommand(client *c);
void replconfCommand(client *c);
void waitCommand(client *c);
void georadiusbymemberCommand(client *c);
void georadiusbymemberroCommand(client *c);
void georadiusCommand(client *c);
void georadiusroCommand(client *c);
void geoaddCommand(client *c);
void geohashCommand(client *c);
void geoposCommand(client *c);
void geodistCommand(client *c);
void geosearchCommand(client *c);
void geosearchstoreCommand(client *c);
void pfselftestCommand(client *c);
void pfaddCommand(client *c);
void pfcountCommand(client *c);
void pfmergeCommand(client *c);
void pfdebugCommand(client *c);
void latencyCommand(client *c);
void moduleCommand(client *c);
void securityWarningCommand(client *c);
void xaddCommand(client *c);
void xrangeCommand(client *c);
void xrevrangeCommand(client *c);
void xlenCommand(client *c);
void xreadCommand(client *c);
void xgroupCommand(client *c);
void xsetidCommand(client *c);
void xackCommand(client *c);
void xpendingCommand(client *c);
void xclaimCommand(client *c);
void xautoclaimCommand(client *c);
void xinfoCommand(client *c);
void xdelCommand(client *c);
void xtrimCommand(client *c);
void lolwutCommand(client *c);
void aclCommand(client *c);
void lcsCommand(client *c);
void quitCommand(client *c);
void resetCommand(client *c);
void failoverCommand(client *c);

#if defined(__GNUC__)
void *calloc(size_t count, size_t size) __attribute__ ((deprecated));
void free(void *ptr) __attribute__ ((deprecated));
void *malloc(size_t size) __attribute__ ((deprecated));
void *realloc(void *ptr, size_t size) __attribute__ ((deprecated));
#endif

/* Debugging stuff */
void _serverAssertWithInfo(const client *c, const robj *o, const char *estr, const char *file, int line);
void _serverAssert(const char *estr, const char *file, int line);
#ifdef __GNUC__
void _serverPanic(const char *file, int line, const char *msg, ...)
    __attribute__ ((format (printf, 3, 4)));
#else
void _serverPanic(const char *file, int line, const char *msg, ...);
#endif
void serverLogObjectDebugInfo(const robj *o);
void sigsegvHandler(int sig, siginfo_t *info, void *secret);
const char *getSafeInfoString(const char *s, size_t len, char **tmp);
dict *genInfoSectionDict(robj **argv, int argc, char **defaults, int *out_all, int *out_everything);
void releaseInfoSectionDict(dict *sec);
sds genRedisInfoString(dict *section_dict, int all_sections, int everything);
sds genModulesInfoString(sds info);
void applyWatchdogPeriod();
void watchdogScheduleSignal(int period);
void serverLogHexDump(int level, char *descr, void *value, size_t len);
int memtest_preserving_test(unsigned long *m, size_t bytes, int passes);
void mixDigest(unsigned char *digest, const void *ptr, size_t len);
void xorDigest(unsigned char *digest, const void *ptr, size_t len);
sds catSubCommandFullname(const char *parent_name, const char *sub_name);
void commandAddSubcommand(struct redisCommand *parent, struct redisCommand *subcommand, const char *declared_name);
void debugDelay(int usec);
void killIOThreads(void);
void killThreads(void);
void makeThreadKillable(void);
void swapMainDbWithTempDb(redisDb *tempDb);

/* Use macro for checking log level to avoid evaluating arguments in cases log
 * should be ignored due to low level. */
//使用宏检查日志级别，以避免在日志由于低级别而被忽略的情况下评估参数。
#define serverLog(level, ...) do {\
        if (((level)&0xff) < server.verbosity) break;\
        _serverLog(level, __VA_ARGS__);\
    } while(0)

/* TLS stuff */
void tlsInit(void);
void tlsCleanup(void);
int tlsConfigure(redisTLSContextConfig *ctx_config);
int isTlsConfigured(void);

#define redisDebug(fmt, ...) \
    printf("DEBUG %s:%d > " fmt "\n", __FILE__, __LINE__, __VA_ARGS__)
#define redisDebugMark() \
    printf("-- MARK %s:%d --\n", __FILE__, __LINE__)

int iAmMaster(void);

#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)

#endif
