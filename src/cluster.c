/* Redis Cluster implementation.
 *
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
#include "endianconv.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <math.h>

/* A global reference to myself is handy to make code more clear.
 * Myself always points to server.cluster->myself, that is, the clusterNode
 * that represents this node. */
//对我自己的全局引用有助于使代码更清晰。 myself总是指向server.cluster->myself，也就是代表这个节点的clusterNode。
clusterNode *myself = NULL;

clusterNode *createClusterNode(char *nodename, int flags);
void clusterAddNode(clusterNode *node);
void clusterAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void clusterReadHandler(connection *conn);
void clusterSendPing(clusterLink *link, int type);
void clusterSendFail(char *nodename);
void clusterSendFailoverAuthIfNeeded(clusterNode *node, clusterMsg *request);
void clusterUpdateState(void);
int clusterNodeGetSlotBit(clusterNode *n, int slot);
sds clusterGenNodesDescription(int filter, int use_pport);
list *clusterGetNodesServingMySlots(clusterNode *node);
int clusterNodeAddSlave(clusterNode *master, clusterNode *slave);
int clusterAddSlot(clusterNode *n, int slot);
int clusterDelSlot(int slot);
int clusterDelNodeSlots(clusterNode *node);
int clusterNodeSetSlotBit(clusterNode *n, int slot);
void clusterSetMaster(clusterNode *n);
void clusterHandleSlaveFailover(void);
void clusterHandleSlaveMigration(int max_slaves);
int bitmapTestBit(unsigned char *bitmap, int pos);
void clusterDoBeforeSleep(int flags);
void clusterSendUpdate(clusterLink *link, clusterNode *node);
void resetManualFailover(void);
void clusterCloseAllSlots(void);
void clusterSetNodeAsMaster(clusterNode *n);
void clusterDelNode(clusterNode *delnode);
sds representClusterNodeFlags(sds ci, uint16_t flags);
sds representSlotInfo(sds ci, uint16_t *slot_info_pairs, int slot_info_pairs_count);
void clusterFreeNodesSlotsInfo(clusterNode *n);
uint64_t clusterGetMaxEpoch(void);
int clusterBumpConfigEpochWithoutConsensus(void);
void moduleCallClusterReceivers(const char *sender_id, uint64_t module_id, uint8_t type, const unsigned char *payload, uint32_t len);
const char *clusterGetMessageTypeString(int type);
void removeChannelsInSlot(unsigned int slot);
unsigned int countKeysInSlot(unsigned int hashslot);
unsigned int countChannelsInSlot(unsigned int hashslot);
unsigned int delKeysInSlot(unsigned int hashslot);

/* Links to the next and previous entries for keys in the same slot are stored
 * in the dict entry metadata. See Slot to Key API below. */
//指向同一槽中键的下一个和上一个条目的链接存储在 dict 条目元数据中。请参阅下面的 Slot to Key API。
#define dictEntryNextInSlot(de) \
    (((clusterDictEntryMetadata *)dictMetadata(de))->next)
#define dictEntryPrevInSlot(de) \
    (((clusterDictEntryMetadata *)dictMetadata(de))->prev)

#define RCVBUF_INIT_LEN 1024
#define RCVBUF_MAX_PREALLOC (1<<20) /* 1MB */

/* Cluster nodes hash table, mapping nodes addresses 1.2.3.4:6379 to
 * clusterNode structures. */
//集群节点哈希表，将节点地址 1.2.3.4:6379 映射到 clusterNode 结构。
dictType clusterNodesDictType = {
        dictSdsHash,                /* hash function */
        NULL,                       /* key dup */
        NULL,                       /* val dup */
        dictSdsKeyCompare,          /* key compare */
        dictSdsDestructor,          /* key destructor */
        NULL,                       /* val destructor */
        NULL                        /* allow to expand */
};

/* Cluster re-addition blacklist. This maps node IDs to the time
 * we can re-add this node. The goal is to avoid reading a removed
 * node for some time. */
//集群重新添加黑名单。这会将节点 ID 映射到我们可以重新添加该节点的时间。目标是避免在一段时间内读取已删除的节点。
dictType clusterNodesBlackListDictType = {
        dictSdsCaseHash,            /* hash function */
        NULL,                       /* key dup */
        NULL,                       /* val dup */
        dictSdsKeyCaseCompare,      /* key compare */
        dictSdsDestructor,          /* key destructor */
        NULL,                       /* val destructor */
        NULL                        /* allow to expand */
};

/* -----------------------------------------------------------------------------
 * Initialization
 * -------------------------------------------------------------------------- */

/* Load the cluster config from 'filename'.
 *
 * If the file does not exist or is zero-length (this may happen because
 * when we lock the nodes.conf file, we create a zero-length one for the
 * sake of locking if it does not already exist), C_ERR is returned.
 * If the configuration was loaded from the file, C_OK is returned. */
/**
 * 从“文件名”加载集群配置。如果文件不存在或长度为零（这可能是因为当我们锁定nodes.conf 文件时，
 * 如果它不存在，我们会创建一个长度为零的文件以进行锁定），则返回C_ERR。如果配置是从文件中加载的，则返回 C_OK。
 * */
int clusterLoadConfig(char *filename) {
    FILE *fp = fopen(filename,"r");
    struct stat sb;
    char *line;
    int maxline, j;

    if (fp == NULL) {
        if (errno == ENOENT) {
            return C_ERR;
        } else {
            serverLog(LL_WARNING,
                "Loading the cluster node config from %s: %s",
                filename, strerror(errno));
            exit(1);
        }
    }

    if (redis_fstat(fileno(fp),&sb) == -1) {
        serverLog(LL_WARNING,
            "Unable to obtain the cluster node config file stat %s: %s",
            filename, strerror(errno));
        exit(1);
    }
    /* Check if the file is zero-length: if so return C_ERR to signal
     * we have to write the config. */
    //检查文件是否为零长度：如果是，则返回 C_ERR 以表示我们必须编写配置。
    if (sb.st_size == 0) {
        fclose(fp);
        return C_ERR;
    }

    /* Parse the file. Note that single lines of the cluster config file can
     * be really long as they include all the hash slots of the node.
     * This means in the worst possible case, half of the Redis slots will be
     * present in a single line, possibly in importing or migrating state, so
     * together with the node ID of the sender/receiver.
     *
     * To simplify we allocate 1024+CLUSTER_SLOTS*128 bytes per line. */
    /**
     * 解析文件。请注意，集群配置文件的单行可以非常长，因为它们包含节点的所有哈希槽。
     * 这意味着在最坏的情况下，一半的 Redis 插槽将出现在一行中，可能处于导入或迁移状态，因此连同发送方的节点 ID。
     * 为了简化，我们为每行分配 1024+CLUSTER_SLOTS128 个字节。
     * */
    maxline = 1024+CLUSTER_SLOTS*128;
    line = zmalloc(maxline);
    while(fgets(line,maxline,fp) != NULL) {
        int argc;
        sds *argv;
        clusterNode *n, *master;
        char *p, *s;

        /* Skip blank lines, they can be created either by users manually
         * editing nodes.conf or by the config writing process if stopped
         * before the truncate() call. */
        //跳过空行，它们可以由用户手动编辑 nodes.conf 或由配置编写过程创建，如果在 truncate() 调用之前停止。
        if (line[0] == '\n' || line[0] == '\0') continue;

        /* Split the line into arguments for processing. */
        //将行拆分为参数进行处理。
        argv = sdssplitargs(line,&argc);
        if (argv == NULL) goto fmterr;

        /* Handle the special "vars" line. Don't pretend it is the last
         * line even if it actually is when generated by Redis. */
        //处理特殊的“vars”行。不要假装它是最后一行，即使它实际上是由 Redis 生成的。
        if (strcasecmp(argv[0],"vars") == 0) {
            if (!(argc % 2)) goto fmterr;
            for (j = 1; j < argc; j += 2) {
                if (strcasecmp(argv[j],"currentEpoch") == 0) {
                    server.cluster->currentEpoch =
                            strtoull(argv[j+1],NULL,10);
                } else if (strcasecmp(argv[j],"lastVoteEpoch") == 0) {
                    server.cluster->lastVoteEpoch =
                            strtoull(argv[j+1],NULL,10);
                } else {
                    serverLog(LL_WARNING,
                        "Skipping unknown cluster config variable '%s'",
                        argv[j]);
                }
            }
            sdsfreesplitres(argv,argc);
            continue;
        }

        /* Regular config lines have at least eight fields */
        //常规配置行至少有八个字段
        if (argc < 8) {
            sdsfreesplitres(argv,argc);
            goto fmterr;
        }

        /* Create this node if it does not exist */
        //如果该节点不存在则创建它
        if (verifyClusterNodeId(argv[0], sdslen(argv[0])) == C_ERR) {
            sdsfreesplitres(argv, argc);
            goto fmterr;
        }
        n = clusterLookupNode(argv[0], sdslen(argv[0]));
        if (!n) {
            n = createClusterNode(argv[0],0);
            clusterAddNode(n);
        }
        /* Format for the node address information: 
         * ip:port[@cport][,hostname] */

        /* Hostname is an optional argument that defines the endpoint
         * that can be reported to clients instead of IP. */
        /**
         * 节点地址信息的格式：ip:port[@cport][,hostname] 主机名是一个可选参数，
         * 它定义了可以报告给客户端而不是 IP 的端点。
         * */
        char *hostname = strchr(argv[1], ',');
        if (hostname) {
            *hostname = '\0';
            hostname++;
            n->hostname = sdscpy(n->hostname, hostname);
        } else if (sdslen(n->hostname) != 0) {
            sdsclear(n->hostname);
        }

        /* Address and port */
        if ((p = strrchr(argv[1],':')) == NULL) {
            sdsfreesplitres(argv,argc);
            goto fmterr;
        }
        *p = '\0';
        memcpy(n->ip,argv[1],strlen(argv[1])+1);
        char *port = p+1;
        char *busp = strchr(port,'@');
        if (busp) {
            *busp = '\0';
            busp++;
        }
        n->port = atoi(port);
        /* In older versions of nodes.conf the "@busport" part is missing.
         * In this case we set it to the default offset of 10000 from the
         * base port. */
        //在较早版本的nodes.conf 中，“@busport”部分缺失。在这种情况下，我们将其设置为与基本端口的默认偏移量 10000。
        n->cport = busp ? atoi(busp) : n->port + CLUSTER_PORT_INCR;

        /* The plaintext port for client in a TLS cluster (n->pport) is not
         * stored in nodes.conf. It is received later over the bus protocol.
         * TLS 集群中客户端的明文端口 (n->pport) 不存储在 nodes.conf 中。稍后通过总线协议接收它。*/

        /* Parse flags */
        p = s = argv[2];
        while(p) {
            p = strchr(s,',');
            if (p) *p = '\0';
            if (!strcasecmp(s,"myself")) {
                serverAssert(server.cluster->myself == NULL);
                myself = server.cluster->myself = n;
                n->flags |= CLUSTER_NODE_MYSELF;
            } else if (!strcasecmp(s,"master")) {
                n->flags |= CLUSTER_NODE_MASTER;
            } else if (!strcasecmp(s,"slave")) {
                n->flags |= CLUSTER_NODE_SLAVE;
            } else if (!strcasecmp(s,"fail?")) {
                n->flags |= CLUSTER_NODE_PFAIL;
            } else if (!strcasecmp(s,"fail")) {
                n->flags |= CLUSTER_NODE_FAIL;
                n->fail_time = mstime();
            } else if (!strcasecmp(s,"handshake")) {
                n->flags |= CLUSTER_NODE_HANDSHAKE;
            } else if (!strcasecmp(s,"noaddr")) {
                n->flags |= CLUSTER_NODE_NOADDR;
            } else if (!strcasecmp(s,"nofailover")) {
                n->flags |= CLUSTER_NODE_NOFAILOVER;
            } else if (!strcasecmp(s,"noflags")) {
                /* nothing to do */
            } else {
                serverPanic("Unknown flag in redis cluster config file");
            }
            if (p) s = p+1;
        }

        /* Get master if any. Set the master and populate master's
         * slave list. */
        //有的话找师傅。设置master并填充master的slave列表。
        if (argv[3][0] != '-') {
            if (verifyClusterNodeId(argv[3], sdslen(argv[3])) == C_ERR) {
                sdsfreesplitres(argv, argc);
                goto fmterr;
            }
            master = clusterLookupNode(argv[3], sdslen(argv[3]));
            if (!master) {
                master = createClusterNode(argv[3],0);
                clusterAddNode(master);
            }
            n->slaveof = master;
            clusterNodeAddSlave(master,n);
        }

        /* Set ping sent / pong received timestamps */
        //设置 ping 发送 pong 接收时间戳
        if (atoi(argv[4])) n->ping_sent = mstime();
        if (atoi(argv[5])) n->pong_received = mstime();

        /* Set configEpoch for this node.
         * If the node is a replica, set its config epoch to 0.
         * If it's a primary, load the config epoch from the configuration file. */
        //为此节点设置 configEpoch。如果节点是副本，则将其配置纪元设置为 0。如果是主节点，则从配置文件加载配置纪元。
        n->configEpoch = (nodeIsSlave(n) && n->slaveof) ? 0 : strtoull(argv[6],NULL,10);

        /* Populate hash slots served by this instance. */
        //填充此实例提供的哈希槽。
        for (j = 8; j < argc; j++) {
            int start, stop;

            if (argv[j][0] == '[') {
                /* Here we handle migrating / importing slots */
                //这里我们处理迁移导入槽
                int slot;
                char direction;
                clusterNode *cn;

                p = strchr(argv[j],'-');
                serverAssert(p != NULL);
                *p = '\0';
                direction = p[1]; /* Either '>' or '<' */
                slot = atoi(argv[j]+1);
                if (slot < 0 || slot >= CLUSTER_SLOTS) {
                    sdsfreesplitres(argv,argc);
                    goto fmterr;
                }
                p += 3;

                char *pr = strchr(p, ']');
                size_t node_len = pr - p;
                if (pr == NULL || verifyClusterNodeId(p, node_len) == C_ERR) {
                    sdsfreesplitres(argv, argc);
                    goto fmterr;
                }
                cn = clusterLookupNode(p, CLUSTER_NAMELEN);
                if (!cn) {
                    cn = createClusterNode(p,0);
                    clusterAddNode(cn);
                }
                if (direction == '>') {
                    server.cluster->migrating_slots_to[slot] = cn;
                } else {
                    server.cluster->importing_slots_from[slot] = cn;
                }
                continue;
            } else if ((p = strchr(argv[j],'-')) != NULL) {
                *p = '\0';
                start = atoi(argv[j]);
                stop = atoi(p+1);
            } else {
                start = stop = atoi(argv[j]);
            }
            if (start < 0 || start >= CLUSTER_SLOTS ||
                stop < 0 || stop >= CLUSTER_SLOTS)
            {
                sdsfreesplitres(argv,argc);
                goto fmterr;
            }
            while(start <= stop) clusterAddSlot(n, start++);
        }

        sdsfreesplitres(argv,argc);
    }
    /* Config sanity check */
    if (server.cluster->myself == NULL) goto fmterr;

    zfree(line);
    fclose(fp);

    serverLog(LL_NOTICE,"Node configuration loaded, I'm %.40s", myself->name);

    /* Something that should never happen: currentEpoch smaller than
     * the max epoch found in the nodes configuration. However we handle this
     * as some form of protection against manual editing of critical files. */
    /**
     * 永远不应该发生的事情：currentEpoch 小于在节点配置中找到的最大 epoch。
     * 但是，我们将其作为某种形式的保护来防止手动编辑关键文件。
     * */
    if (clusterGetMaxEpoch() > server.cluster->currentEpoch) {
        server.cluster->currentEpoch = clusterGetMaxEpoch();
    }
    return C_OK;

fmterr:
    serverLog(LL_WARNING,
        "Unrecoverable error: corrupted cluster config file.");
    zfree(line);
    if (fp) fclose(fp);
    exit(1);
}

/* Cluster node configuration is exactly the same as CLUSTER NODES output.
 *
 * This function writes the node config and returns 0, on error -1
 * is returned.
 *
 * Note: we need to write the file in an atomic way from the point of view
 * of the POSIX filesystem semantics, so that if the server is stopped
 * or crashes during the write, we'll end with either the old file or the
 * new one. Since we have the full payload to write available we can use
 * a single write to write the whole file. If the pre-existing file was
 * bigger we pad our payload with newlines that are anyway ignored and truncate
 * the file afterward. */
/**
 * 集群节点配置与 CLUSTER NODES 输出完全相同。此函数写入节点配置并返回 0，错误时返回 -1。
 * 注意：从 POSIX 文件系统语义的角度来看，我们需要以原子方式写入文件，这样如果服务器在写入过程中停止或崩溃，
 * 我们将以旧文件或新文件结束。由于我们有完整的有效负载可供写入，我们可以使用单次写入来写入整个文件。
 * 如果预先存在的文件更大，我们用换行符填充我们的有效负载，这些换行符无论如何都会被忽略并在之后截断文件。
 * */
int clusterSaveConfig(int do_fsync) {
    sds ci;
    size_t content_size;
    struct stat sb;
    int fd;

    server.cluster->todo_before_sleep &= ~CLUSTER_TODO_SAVE_CONFIG;

    /* Get the nodes description and concatenate our "vars" directive to
     * save currentEpoch and lastVoteEpoch. */
    //获取节点描述并连接我们的“vars”指令以保存 currentEpoch 和 lastVoteEpoch。
    ci = clusterGenNodesDescription(CLUSTER_NODE_HANDSHAKE, 0);
    ci = sdscatprintf(ci,"vars currentEpoch %llu lastVoteEpoch %llu\n",
        (unsigned long long) server.cluster->currentEpoch,
        (unsigned long long) server.cluster->lastVoteEpoch);
    content_size = sdslen(ci);

    if ((fd = open(server.cluster_configfile,O_WRONLY|O_CREAT,0644))
        == -1) goto err;

    if (redis_fstat(fd,&sb) == -1) goto err;

    /* Pad the new payload if the existing file length is greater. */
    //如果现有文件长度更大，则填充新的有效负载。
    if (sb.st_size > (off_t)content_size) {
        ci = sdsgrowzero(ci,sb.st_size);
        memset(ci+content_size,'\n',sb.st_size-content_size);
    }
    
    if (write(fd,ci,sdslen(ci)) != (ssize_t)sdslen(ci)) goto err;
    if (do_fsync) {
        server.cluster->todo_before_sleep &= ~CLUSTER_TODO_FSYNC_CONFIG;
        if (fsync(fd) == -1) goto err;
    }

    /* Truncate the file if needed to remove the final \n padding that
     * is just garbage. */
    //如果需要，请截断文件以删除只是垃圾的最终 \n 填充。
    if (content_size != sdslen(ci) && ftruncate(fd,content_size) == -1) {
        /* ftruncate() failing is not a critical error.
         * ftruncate() 失败不是严重错误。*/
    }
    close(fd);
    sdsfree(ci);
    return 0;

err:
    if (fd != -1) close(fd);
    sdsfree(ci);
    return -1;
}

void clusterSaveConfigOrDie(int do_fsync) {
    if (clusterSaveConfig(do_fsync) == -1) {
        serverLog(LL_WARNING,"Fatal: can't update cluster config file.");
        exit(1);
    }
}

/* Lock the cluster config using flock(), and retain the file descriptor used to
 * acquire the lock so that the file will be locked as long as the process is up.
 *
 * This works because we always update nodes.conf with a new version
 * in-place, reopening the file, and writing to it in place (later adjusting
 * the length with ftruncate()).
 *
 * On success C_OK is returned, otherwise an error is logged and
 * the function returns C_ERR to signal a lock was not acquired. */
/**
 * 使用flock()锁定集群配置，并保留用于获取锁定的文件描述符，以便只要进程启动，文件就会被锁定。
 * 这是可行的，因为我们总是使用新版本就地更新nodes.conf，重新打开文件并就地写入（稍后使用ftruncate() 调整长度）。
 * 成功时返回 C_OK，否则记录错误并且函数返回 C_ERR 以表示未获取锁。
 * */
int clusterLockConfig(char *filename) {
/* flock() does not exist on Solaris
 * and a fcntl-based solution won't help, as we constantly re-open that file,
 * which will release _all_ locks anyway
 */
/**
 * Solaris 上不存在flock() 并且基于 fcntl 的解决方案也无济于事，因为我们不断地重新打开该文件，这无论如何都会释放_all_ 锁
 * */
#if !defined(__sun)
    /* To lock it, we need to open the file in a way it is created if
     * it does not exist, otherwise there is a race condition with other
     * processes. */
    //要锁定它，如果文件不存在，我们需要以创建它的方式打开文件，否则会出现与其他进程的竞争条件。
    int fd = open(filename,O_WRONLY|O_CREAT|O_CLOEXEC,0644);
    if (fd == -1) {
        serverLog(LL_WARNING,
            "Can't open %s in order to acquire a lock: %s",
            filename, strerror(errno));
        return C_ERR;
    }

    if (flock(fd,LOCK_EX|LOCK_NB) == -1) {
        if (errno == EWOULDBLOCK) {
            serverLog(LL_WARNING,
                 "Sorry, the cluster configuration file %s is already used "
                 "by a different Redis Cluster node. Please make sure that "
                 "different nodes use different cluster configuration "
                 "files.", filename);
        } else {
            serverLog(LL_WARNING,
                "Impossible to lock %s: %s", filename, strerror(errno));
        }
        close(fd);
        return C_ERR;
    }
    /* Lock acquired: leak the 'fd' by not closing it until shutdown time, so that
     * we'll retain the lock to the file as long as the process exists.
     *
     * After fork, the child process will get the fd opened by the parent process,
     * we need save `fd` to `cluster_config_file_lock_fd`, so that in redisFork(),
     * it will be closed in the child process.
     * If it is not closed, when the main process is killed -9, but the child process
     * (redis-aof-rewrite) is still alive, the fd(lock) will still be held by the
     * child process, and the main process will fail to get lock, means fail to start. */
    /**
     * 获得锁：通过在关闭时间之前不关闭它来泄漏“fd”，这样只要进程存在，我们就会保留对文件的锁。
     * fork后子进程会拿到父进程打开的fd，我们需要将fd保存到cluster_config_file_lock_fd中，
     * 这样在redisFork()中会在子进程中关闭。如果不关闭，当主进程被杀-9，但子进程（redis-aof-rewrite）还活着时，
     * fd(lock)仍然会被子进程持有，主进程会失败获得锁，意味着无法启动。
     * */
    server.cluster_config_file_lock_fd = fd;
#else
    UNUSED(filename);
#endif /* __sun */

    return C_OK;
}

/* Derives our ports to be announced in the cluster bus. */
//派生要在集群总线中公布的端口。
void deriveAnnouncedPorts(int *announced_port, int *announced_pport,
                          int *announced_cport) {
    int port = server.tls_cluster ? server.tls_port : server.port;
    /* Default announced ports. 默认公布的端口。*/
    *announced_port = port;
    *announced_pport = server.tls_cluster ? server.port : 0;
    *announced_cport = server.cluster_port ? server.cluster_port : port + CLUSTER_PORT_INCR;
    
    /* Config overriding announced ports. 配置覆盖已宣布的端口。*/
    if (server.tls_cluster && server.cluster_announce_tls_port) {
        *announced_port = server.cluster_announce_tls_port;
        *announced_pport = server.cluster_announce_port;
    } else if (server.cluster_announce_port) {
        *announced_port = server.cluster_announce_port;
    }
    if (server.cluster_announce_bus_port) {
        *announced_cport = server.cluster_announce_bus_port;
    }
}

/* Some flags (currently just the NOFAILOVER flag) may need to be updated
 * in the "myself" node based on the current configuration of the node,
 * that may change at runtime via CONFIG SET. This function changes the
 * set of flags in myself->flags accordingly. */
/**
 * 一些标志（目前只是 NOFAILOVER 标志）可能需要根据节点的当前配置在“我自己”节点中更新，这可能会在运行时通过 CONFIG SET 更改。
 * 这个函数相应地改变了我自己->标志中的标志集。
 * */
void clusterUpdateMyselfFlags(void) {
    if (!myself) return;
    int oldflags = myself->flags;
    int nofailover = server.cluster_slave_no_failover ?
                     CLUSTER_NODE_NOFAILOVER : 0;
    myself->flags &= ~CLUSTER_NODE_NOFAILOVER;
    myself->flags |= nofailover;
    if (myself->flags != oldflags) {
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                             CLUSTER_TODO_UPDATE_STATE);
    }
}


/* We want to take myself->ip in sync with the cluster-announce-ip option.
* The option can be set at runtime via CONFIG SET. */
//我们希望将自己的->ip 与 cluster-announce-ip 选项同步。该选项可以在运行时通过 CONFIG SET 进行设置。
void clusterUpdateMyselfIp(void) {
    if (!myself) return;
    static char *prev_ip = NULL;
    char *curr_ip = server.cluster_announce_ip;
    int changed = 0;

    if (prev_ip == NULL && curr_ip != NULL) changed = 1;
    else if (prev_ip != NULL && curr_ip == NULL) changed = 1;
    else if (prev_ip && curr_ip && strcmp(prev_ip,curr_ip)) changed = 1;

    if (changed) {
        if (prev_ip) zfree(prev_ip);
        prev_ip = curr_ip;

        if (curr_ip) {
            /* We always take a copy of the previous IP address, by
            * duplicating the string. This way later we can check if
            * the address really changed. */
            //我们总是通过复制字符串来复制以前的 IP 地址。这样以后我们可以检查地址是否真的改变了。
            prev_ip = zstrdup(prev_ip);
            strncpy(myself->ip,server.cluster_announce_ip,NET_IP_STR_LEN-1);
            myself->ip[NET_IP_STR_LEN-1] = '\0';
        } else {
            myself->ip[0] = '\0'; /* Force autodetection. 强制自动检测。*/
        }
    }
}

/* Update the hostname for the specified node with the provided C string. */
//使用提供的 C 字符串更新指定节点的主机名。
static void updateAnnouncedHostname(clusterNode *node, char *new) {
    /* Previous and new hostname are the same, no need to update. */
    //旧主机名和新主机名相同，无需更新。
    if (new && !strcmp(new, node->hostname)) {
        return;
    }

    if (new) {
        node->hostname = sdscpy(node->hostname, new);
    } else if (sdslen(node->hostname) != 0) {
        sdsclear(node->hostname);
    }
}

/* Update my hostname based on server configuration values */
//根据服务器配置值更新我的主机名
void clusterUpdateMyselfHostname(void) {
    if (!myself) return;
    updateAnnouncedHostname(myself, server.cluster_announce_hostname);
}

void clusterInit(void) {
    int saveconf = 0;

    server.cluster = zmalloc(sizeof(clusterState));
    server.cluster->myself = NULL;
    server.cluster->currentEpoch = 0;
    server.cluster->state = CLUSTER_FAIL;
    server.cluster->size = 1;
    server.cluster->todo_before_sleep = 0;
    server.cluster->nodes = dictCreate(&clusterNodesDictType);
    server.cluster->nodes_black_list =
        dictCreate(&clusterNodesBlackListDictType);
    server.cluster->failover_auth_time = 0;
    server.cluster->failover_auth_count = 0;
    server.cluster->failover_auth_rank = 0;
    server.cluster->failover_auth_epoch = 0;
    server.cluster->cant_failover_reason = CLUSTER_CANT_FAILOVER_NONE;
    server.cluster->lastVoteEpoch = 0;

    /* Initialize stats */
    for (int i = 0; i < CLUSTERMSG_TYPE_COUNT; i++) {
        server.cluster->stats_bus_messages_sent[i] = 0;
        server.cluster->stats_bus_messages_received[i] = 0;
    }
    server.cluster->stats_pfail_nodes = 0;
    server.cluster->stat_cluster_links_buffer_limit_exceeded = 0;

    memset(server.cluster->slots,0, sizeof(server.cluster->slots));
    clusterCloseAllSlots();

    /* Lock the cluster config file to make sure every node uses
     * its own nodes.conf. */
    //锁定集群配置文件以确保每个节点都使用自己的nodes.conf。
    server.cluster_config_file_lock_fd = -1;
    if (clusterLockConfig(server.cluster_configfile) == C_ERR)
        exit(1);

    /* Load or create a new nodes configuration. */
    //加载或创建新的节点配置。
    if (clusterLoadConfig(server.cluster_configfile) == C_ERR) {
        /* No configuration found. We will just use the random name provided
         * by the createClusterNode() function. */
        //未找到配置。我们将只使用 createClusterNode() 函数提供的随机名称。
        myself = server.cluster->myself =
            createClusterNode(NULL,CLUSTER_NODE_MYSELF|CLUSTER_NODE_MASTER);
        serverLog(LL_NOTICE,"No cluster configuration found, I'm %.40s",
            myself->name);
        clusterAddNode(myself);
        saveconf = 1;
    }
    if (saveconf) clusterSaveConfigOrDie(1);

    /* We need a listening TCP port for our cluster messaging needs. */
    //我们需要一个侦听 TCP 端口来满足集群消息传递的需求。
    server.cfd.count = 0;

    /* Port sanity check II
     * The other handshake port check is triggered too late to stop
     * us from trying to use a too-high cluster port number. */
    //端口完整性检查 II 另一个握手端口检查触发得太晚，无法阻止我们尝试使用过高的集群端口号。
    int port = server.tls_cluster ? server.tls_port : server.port;
    if (!server.cluster_port && port > (65535-CLUSTER_PORT_INCR)) {
        serverLog(LL_WARNING, "Redis port number too high. "
                   "Cluster communication port is 10,000 port "
                   "numbers higher than your Redis port. "
                   "Your Redis port number must be 55535 or less.");
        exit(1);
    }
    if (!server.bindaddr_count) {
        serverLog(LL_WARNING, "No bind address is configured, but it is required for the Cluster bus.");
        exit(1);
    }
    int cport = server.cluster_port ? server.cluster_port : port + CLUSTER_PORT_INCR;
    if (listenToPort(cport, &server.cfd) == C_ERR ) {
        /* Note: the following log text is matched by the test suite. */
        //注意：以下日志文本由测试套件匹配。
        serverLog(LL_WARNING, "Failed listening on port %u (cluster), aborting.", cport);
        exit(1);
    }
    
    if (createSocketAcceptHandler(&server.cfd, clusterAcceptHandler) != C_OK) {
        serverPanic("Unrecoverable error creating Redis Cluster socket accept handler.");
    }

    /* Initialize data for the Slot to key API. */
    //初始化 Slot to key API 的数据。
    slotToKeyInit(server.db);

    /* The slots -> channels map is a radix tree. Initialize it here. */
    //slot -> channels 映射是一个基数树。在这里初始化它。
    server.cluster->slots_to_channels = raxNew();

    /* Set myself->port/cport/pport to my listening ports, we'll just need to
     * discover the IP address via MEET messages. */
    //将 self->portcportpport 设置为我的监听端口，我们只需要通过 MEET 消息发现 IP 地址。
    deriveAnnouncedPorts(&myself->port, &myself->pport, &myself->cport);

    server.cluster->mf_end = 0;
    server.cluster->mf_slave = NULL;
    resetManualFailover();
    clusterUpdateMyselfFlags();
    clusterUpdateMyselfIp();
    clusterUpdateMyselfHostname();
}

/* Reset a node performing a soft or hard reset:
 *
 * 1) All other nodes are forgotten.
 * 2) All the assigned / open slots are released.
 * 3) If the node is a slave, it turns into a master.
 * 4) Only for hard reset: a new Node ID is generated.
 * 5) Only for hard reset: currentEpoch and configEpoch are set to 0.
 * 6) The new configuration is saved and the cluster state updated.
 * 7) If the node was a slave, the whole data set is flushed away. */
/**
 * 重置执行软或硬重置的节点：
 *   1) 忘记所有其他节点。
 *   2) 释放所有分配的空位。
 *   3) 如果节点是从节点，则变为主节点。
 *   4) 仅用于硬重置：生成一个新的节点 ID。
 *   5) 仅用于硬重置：currentEpoch 和 configEpoch 设置为 0。
 *   6) 保存新配置并更新集群状态。
 *   7) 如果节点是从节点，则将整个数据集刷新掉。
 * */
void clusterReset(int hard) {
    dictIterator *di;
    dictEntry *de;
    int j;

    /* Turn into master. */
    if (nodeIsSlave(myself)) {
        clusterSetNodeAsMaster(myself);
        replicationUnsetMaster();
        emptyData(-1,EMPTYDB_NO_FLAGS,NULL);
    }

    /* Close slots, reset manual failover state. */
    //关闭插槽，重置手动故障转移状态。
    clusterCloseAllSlots();
    resetManualFailover();

    /* Unassign all the slots. 取消分配所有插槽。*/
    for (j = 0; j < CLUSTER_SLOTS; j++) clusterDelSlot(j);

    /* Forget all the nodes, but myself.忘记所有节点，但我自己。 */
    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        if (node == myself) continue;
        clusterDelNode(node);
    }
    dictReleaseIterator(di);

    /* Hard reset only: set epochs to 0, change node ID. */
    //仅硬重置：将 epochs 设置为 0，更改节点 ID。
    if (hard) {
        sds oldname;

        server.cluster->currentEpoch = 0;
        server.cluster->lastVoteEpoch = 0;
        myself->configEpoch = 0;
        serverLog(LL_WARNING, "configEpoch set to 0 via CLUSTER RESET HARD");

        /* To change the Node ID we need to remove the old name from the
         * nodes table, change the ID, and re-add back with new name. */
        //要更改节点 ID，我们需要从节点表中删除旧名称，更改 ID，然后重新添加新名称。
        oldname = sdsnewlen(myself->name, CLUSTER_NAMELEN);
        dictDelete(server.cluster->nodes,oldname);
        sdsfree(oldname);
        getRandomHexChars(myself->name, CLUSTER_NAMELEN);
        clusterAddNode(myself);
        serverLog(LL_NOTICE,"Node hard reset, now I'm %.40s", myself->name);
    }

    /* Make sure to persist the new config and update the state. */
    //确保保留新配置并更新状态。
    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                         CLUSTER_TODO_UPDATE_STATE|
                         CLUSTER_TODO_FSYNC_CONFIG);
}

/* -----------------------------------------------------------------------------
 * CLUSTER communication link
 * -------------------------------------------------------------------------- */

clusterLink *createClusterLink(clusterNode *node) {
    clusterLink *link = zmalloc(sizeof(*link));
    link->ctime = mstime();
    link->sndbuf = sdsempty();
    link->rcvbuf = zmalloc(link->rcvbuf_alloc = RCVBUF_INIT_LEN);
    link->rcvbuf_len = 0;
    link->conn = NULL;
    link->node = node;
    /* Related node can only possibly be known at link creation time if this is an outbound link */
    //如果这是出站链接，则相关节点只能在链接创建时知道
    link->inbound = (node == NULL);
    if (!link->inbound) {
        node->link = link;
    }
    return link;
}

/* Free a cluster link, but does not free the associated node of course.
 * This function will just make sure that the original node associated
 * with this link will have the 'link' field set to NULL. */
//释放一个集群链接，但当然不会释放关联的节点。此函数将确保与此链接关联的原始节点将“链接”字段设置为 NULL。
void freeClusterLink(clusterLink *link) {
    if (link->conn) {
        connClose(link->conn);
        link->conn = NULL;
    }
    sdsfree(link->sndbuf);
    zfree(link->rcvbuf);
    if (link->node) {
        if (link->node->link == link) {
            serverAssert(!link->inbound);
            link->node->link = NULL;
        } else if (link->node->inbound_link == link) {
            serverAssert(link->inbound);
            link->node->inbound_link = NULL;
        }
    }
    zfree(link);
}

void setClusterNodeToInboundClusterLink(clusterNode *node, clusterLink *link) {
    serverAssert(!link->node);
    serverAssert(link->inbound);
    if (node->inbound_link) {
        /* A peer may disconnect and then reconnect with us, and it's not guaranteed that
         * we would always process the disconnection of the existing inbound link before
         * accepting a new existing inbound link. Therefore, it's possible to have more than
         * one inbound link from the same node at the same time. */
        /**
         * 对等点可能会断开连接然后与我们重新连接，并且不能保证我们总是在接受新的现有入站链接之前处理现有入站链接的断开连接。
         * 因此，同一节点可能同时拥有多个入站链接。
         * */
        serverLog(LL_DEBUG, "Replacing inbound link fd %d from node %.40s with fd %d",
                node->inbound_link->conn->fd, node->name, link->conn->fd);
    }
    node->inbound_link = link;
    link->node = node;
}

static void clusterConnAcceptHandler(connection *conn) {
    clusterLink *link;

    if (connGetState(conn) != CONN_STATE_CONNECTED) {
        serverLog(LL_VERBOSE,
                "Error accepting cluster node connection: %s", connGetLastError(conn));
        connClose(conn);
        return;
    }

    /* Create a link object we use to handle the connection.
     * It gets passed to the readable handler when data is available.
     * Initially the link->node pointer is set to NULL as we don't know
     * which node is, but the right node is references once we know the
     * node identity. */
    /**
     * 创建一个我们用来处理连接的链接对象。当数据可用时，它被传递给可读处理程序。
     * 最初，link->node 指针设置为 NULL，因为我们不知道哪个节点是，但是一旦我们知道节点身份，正确的节点就是引用。
     * */
    link = createClusterLink(NULL);
    link->conn = conn;
    connSetPrivateData(conn, link);

    /* Register read handler */
    connSetReadHandler(conn, clusterReadHandler);
}

#define MAX_CLUSTER_ACCEPTS_PER_CALL 1000
void clusterAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cport, cfd;
    int max = MAX_CLUSTER_ACCEPTS_PER_CALL;
    char cip[NET_IP_STR_LEN];
    UNUSED(el);
    UNUSED(mask);
    UNUSED(privdata);

    /* If the server is starting up, don't accept cluster connections:
     * UPDATE messages may interact with the database content. */
    //如果服务器正在启动，请不要接受集群连接：UPDATE 消息可能与数据库内容交互。
    if (server.masterhost == NULL && server.loading) return;

    while(max--) {
        cfd = anetTcpAccept(server.neterr, fd, cip, sizeof(cip), &cport);
        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK)
                serverLog(LL_VERBOSE,
                    "Error accepting cluster node: %s", server.neterr);
            return;
        }

        connection *conn = server.tls_cluster ?
            connCreateAcceptedTLS(cfd, TLS_CLIENT_AUTH_YES) : connCreateAcceptedSocket(cfd);

        /* Make sure connection is not in an error state */
        //确保连接未处于错误状态
        if (connGetState(conn) != CONN_STATE_ACCEPTING) {
            serverLog(LL_VERBOSE,
                "Error creating an accepting connection for cluster node: %s",
                    connGetLastError(conn));
            connClose(conn);
            return;
        }
        connEnableTcpNoDelay(conn);
        connKeepAlive(conn,server.cluster_node_timeout * 2);

        /* Use non-blocking I/O for cluster messages. */
        //对集群消息使用非阻塞 IO。
        serverLog(LL_VERBOSE,"Accepting cluster node connection from %s:%d", cip, cport);

        /* Accept the connection now.  connAccept() may call our handler directly
         * or schedule it for later depending on connection implementation.
         */
        //立即接受连接。 connAccept() 可以直接调用我们的处理程序，也可以根据连接实现将其安排在以后使用。
        if (connAccept(conn, clusterConnAcceptHandler) == C_ERR) {
            if (connGetState(conn) == CONN_STATE_ERROR)
                serverLog(LL_VERBOSE,
                        "Error accepting cluster node connection: %s",
                        connGetLastError(conn));
            connClose(conn);
            return;
        }
    }
}

/* Return the approximated number of sockets we are using in order to
 * take the cluster bus connections. */
//返回我们使用的套接字的近似数量，以便进行集群总线连接。
unsigned long getClusterConnectionsCount(void) {
    /* We decrement the number of nodes by one, since there is the
     * "myself" node too in the list. Each node uses two file descriptors,
     * one incoming and one outgoing, thus the multiplication by 2. */
    //我们将节点数减一，因为列表中也有“我自己”节点。每个节点使用两个文件描述符，一个传入和一个传出，因此乘以 2。
    return server.cluster_enabled ?
           ((dictSize(server.cluster->nodes)-1)*2) : 0;
}

/* -----------------------------------------------------------------------------
 * Key space handling 密钥空间处理
 * -------------------------------------------------------------------------- */

/* We have 16384 hash slots. The hash slot of a given key is obtained
 * as the least significant 14 bits of the crc16 of the key.
 *
 * However if the key contains the {...} pattern, only the part between
 * { and } is hashed. This may be useful in the future to force certain
 * keys to be in the same node (assuming no resharding is in progress). */
/**
 * 我们有 16384 个哈希槽。给定密钥的哈希槽作为密钥的 crc16 的最低有效 14 位获得。
 * 但是，如果键包含 {...} 模式，则只有 { 和 } 之间的部分被散列。
 * 这可能在将来强制某些键在同一个节点中有用（假设没有重新分片正在进行）。
 * */
unsigned int keyHashSlot(char *key, int keylen) {
    int s, e; /* start-end indexes of { and } */

    for (s = 0; s < keylen; s++)
        if (key[s] == '{') break;

    /* No '{' ? Hash the whole key. This is the base case. */
    //不 '{' ？散列整个密钥。这是基本情况。
    if (s == keylen) return crc16(key,keylen) & 0x3FFF;

    /* '{' found? Check if we have the corresponding '}'. */
    //'{' 成立？检查我们是否有相应的'}'。
    for (e = s+1; e < keylen; e++)
        if (key[e] == '}') break;

    /* No '}' or nothing between {} ? Hash the whole key. */
    //没有 '}' 或 {} 之间什么都没有？散列整个密钥。
    if (e == keylen || e == s+1) return crc16(key,keylen) & 0x3FFF;

    /* If we are here there is both a { and a } on its right. Hash
     * what is in the middle between { and }. */
    //如果我们在这里，它的右边有一个 { 和一个 }。散列 { 和 } 中间的内容。
    return crc16(key+s+1,e-s-1) & 0x3FFF;
}

/* -----------------------------------------------------------------------------
 * CLUSTER node API
 * -------------------------------------------------------------------------- */

/* Create a new cluster node, with the specified flags.
 * If "nodename" is NULL this is considered a first handshake and a random
 * node name is assigned to this node (it will be fixed later when we'll
 * receive the first pong).
 *
 * The node is created and returned to the user, but it is not automatically
 * added to the nodes hash table. */
/**
 * 使用指定的标志创建一个新的集群节点。如果“nodename”为 NULL，则认为这是第一次握手，
 * 并为该节点分配一个随机节点名称（稍后我们将收到第一个 pong 时将修复它）。
 * 节点被创建并返回给用户，但它不会自动添加到节点哈希表中。
 * */
clusterNode *createClusterNode(char *nodename, int flags) {
    clusterNode *node = zmalloc(sizeof(*node));

    if (nodename)
        memcpy(node->name, nodename, CLUSTER_NAMELEN);
    else
        getRandomHexChars(node->name, CLUSTER_NAMELEN);
    node->ctime = mstime();
    node->configEpoch = 0;
    node->flags = flags;
    memset(node->slots,0,sizeof(node->slots));
    node->slot_info_pairs = NULL;
    node->slot_info_pairs_count = 0;
    node->numslots = 0;
    node->numslaves = 0;
    node->slaves = NULL;
    node->slaveof = NULL;
    node->last_in_ping_gossip = 0;
    node->ping_sent = node->pong_received = 0;
    node->data_received = 0;
    node->fail_time = 0;
    node->link = NULL;
    node->inbound_link = NULL;
    memset(node->ip,0,sizeof(node->ip));
    node->hostname = sdsempty();
    node->port = 0;
    node->cport = 0;
    node->pport = 0;
    node->fail_reports = listCreate();
    node->voted_time = 0;
    node->orphaned_time = 0;
    node->repl_offset_time = 0;
    node->repl_offset = 0;
    listSetFreeMethod(node->fail_reports,zfree);
    return node;
}

/* This function is called every time we get a failure report from a node.
 * The side effect is to populate the fail_reports list (or to update
 * the timestamp of an existing report).
 *
 * 'failing' is the node that is in failure state according to the
 * 'sender' node.
 *
 * The function returns 0 if it just updates a timestamp of an existing
 * failure report from the same sender. 1 is returned if a new failure
 * report is created. */
/**
 * 每次我们从节点收到故障报告时都会调用此函数。副作用是填充 fail_reports 列表（或更新现有报告的时间戳）。
 * “失败”是根据“发送者”节点处于失败状态的节点。如果它只是更新来自同一发送者的现有故障报告的时间戳，则该函数返回 0。
 * 如果创建了新的失败报告，则返回 1。
 * */
int clusterNodeAddFailureReport(clusterNode *failing, clusterNode *sender) {
    list *l = failing->fail_reports;
    listNode *ln;
    listIter li;
    clusterNodeFailReport *fr;

    /* If a failure report from the same sender already exists, just update
     * the timestamp. */
    //如果来自同一发件人的失败报告已经存在，只需更新时间戳即可。
    listRewind(l,&li);
    while ((ln = listNext(&li)) != NULL) {
        fr = ln->value;
        if (fr->node == sender) {
            fr->time = mstime();
            return 0;
        }
    }

    /* Otherwise create a new report. */
    //否则创建一个新报告。
    fr = zmalloc(sizeof(*fr));
    fr->node = sender;
    fr->time = mstime();
    listAddNodeTail(l,fr);
    return 1;
}

/* Remove failure reports that are too old, where too old means reasonably
 * older than the global node timeout. Note that anyway for a node to be
 * flagged as FAIL we need to have a local PFAIL state that is at least
 * older than the global node timeout, so we don't just trust the number
 * of failure reports from other nodes. */
/**
 * 删除太旧的故障报告，其中太旧意味着比全局节点超时合理地旧。
 * 请注意，无论如何，要将节点标记为 FAIL，我们需要有一个本地 PFAIL 状态，该状态至少早于全局节点超时，
 * 因此我们不只信任来自其他节点的故障报告的数量。
 * */
void clusterNodeCleanupFailureReports(clusterNode *node) {
    list *l = node->fail_reports;
    listNode *ln;
    listIter li;
    clusterNodeFailReport *fr;
    mstime_t maxtime = server.cluster_node_timeout *
                     CLUSTER_FAIL_REPORT_VALIDITY_MULT;
    mstime_t now = mstime();

    listRewind(l,&li);
    while ((ln = listNext(&li)) != NULL) {
        fr = ln->value;
        if (now - fr->time > maxtime) listDelNode(l,ln);
    }
}

/* Remove the failing report for 'node' if it was previously considered
 * failing by 'sender'. This function is called when a node informs us via
 * gossip that a node is OK from its point of view (no FAIL or PFAIL flags).
 *
 * Note that this function is called relatively often as it gets called even
 * when there are no nodes failing, and is O(N), however when the cluster is
 * fine the failure reports list is empty so the function runs in constant
 * time.
 *
 * The function returns 1 if the failure report was found and removed.
 * Otherwise 0 is returned. */
/**
 * 如果“发件人”之前认为它失败，则删除“节点”的失败报告。
 * 当一个节点通过 gossip 通知我们一个节点从它的角度来看是好的（没有 FAIL 或 PFAIL 标志）时，这个函数被调用。
 * 请注意，此函数被调用相对频繁，因为即使没有节点发生故障，它也会被调用，并且是 O(N)，
 * 但是当集群正常时，故障报告列表为空，因此该函数在恒定时间内运行。
 * 如果发现并删除了失败报告，则该函数返回 1。否则返回 0。
 * */
int clusterNodeDelFailureReport(clusterNode *node, clusterNode *sender) {
    list *l = node->fail_reports;
    listNode *ln;
    listIter li;
    clusterNodeFailReport *fr;

    /* Search for a failure report from this sender. */
    //搜索来自该发件人的失败报告。
    listRewind(l,&li);
    while ((ln = listNext(&li)) != NULL) {
        fr = ln->value;
        if (fr->node == sender) break;
    }
    if (!ln) return 0; /* No failure report from this sender. 此发件人没有失败报告。*/

    /* Remove the failure report. 删除故障报告。*/
    listDelNode(l,ln);
    clusterNodeCleanupFailureReports(node);
    return 1;
}

/* Return the number of external nodes that believe 'node' is failing,
 * not including this node, that may have a PFAIL or FAIL state for this
 * node as well. */
//返回认为“节点”失败的外部节点的数量，不包括该节点，该节点也可能具有该节点的 PFAIL 或 FAIL 状态。
int clusterNodeFailureReportsCount(clusterNode *node) {
    clusterNodeCleanupFailureReports(node);
    return listLength(node->fail_reports);
}

int clusterNodeRemoveSlave(clusterNode *master, clusterNode *slave) {
    int j;

    for (j = 0; j < master->numslaves; j++) {
        if (master->slaves[j] == slave) {
            if ((j+1) < master->numslaves) {
                int remaining_slaves = (master->numslaves - j) - 1;
                memmove(master->slaves+j,master->slaves+(j+1),
                        (sizeof(*master->slaves) * remaining_slaves));
            }
            master->numslaves--;
            if (master->numslaves == 0)
                master->flags &= ~CLUSTER_NODE_MIGRATE_TO;
            return C_OK;
        }
    }
    return C_ERR;
}

int clusterNodeAddSlave(clusterNode *master, clusterNode *slave) {
    int j;

    /* If it's already a slave, don't add it again. */
    //如果已经是slave，就不要再添加了。
    for (j = 0; j < master->numslaves; j++)
        if (master->slaves[j] == slave) return C_ERR;
    master->slaves = zrealloc(master->slaves,
        sizeof(clusterNode*)*(master->numslaves+1));
    master->slaves[master->numslaves] = slave;
    master->numslaves++;
    master->flags |= CLUSTER_NODE_MIGRATE_TO;
    return C_OK;
}

int clusterCountNonFailingSlaves(clusterNode *n) {
    int j, okslaves = 0;

    for (j = 0; j < n->numslaves; j++)
        if (!nodeFailed(n->slaves[j])) okslaves++;
    return okslaves;
}

/* Low level cleanup of the node structure. Only called by clusterDelNode(). */
//节点结构的低级清理。仅由 clusterDelNode() 调用。
void freeClusterNode(clusterNode *n) {
    sds nodename;
    int j;

    /* If the node has associated slaves, we have to set
     * all the slaves->slaveof fields to NULL (unknown). */
    //如果节点有关联的 slave，我们必须将所有 slaves->slaveof 字段设置为 NULL（未知）。
    for (j = 0; j < n->numslaves; j++)
        n->slaves[j]->slaveof = NULL;

    /* Remove this node from the list of slaves of its master. */
    //从其主节点的从节点列表中删除该节点。
    if (nodeIsSlave(n) && n->slaveof) clusterNodeRemoveSlave(n->slaveof,n);

    /* Unlink from the set of nodes. */
    //从节点集取消链接。
    nodename = sdsnewlen(n->name, CLUSTER_NAMELEN);
    serverAssert(dictDelete(server.cluster->nodes,nodename) == DICT_OK);
    sdsfree(nodename);
    sdsfree(n->hostname);

    /* Release links and associated data structures. */
    //释放链接和相关的数据结构。
    if (n->link) freeClusterLink(n->link);
    if (n->inbound_link) freeClusterLink(n->inbound_link);
    listRelease(n->fail_reports);
    zfree(n->slaves);
    zfree(n);
}

/* Add a node to the nodes hash table */
//将节点添加到节点哈希表
void clusterAddNode(clusterNode *node) {
    int retval;

    retval = dictAdd(server.cluster->nodes,
            sdsnewlen(node->name,CLUSTER_NAMELEN), node);
    serverAssert(retval == DICT_OK);
}

/* Remove a node from the cluster. The function performs the high level
 * cleanup, calling freeClusterNode() for the low level cleanup.
 * Here we do the following:
 *
 * 1) Mark all the slots handled by it as unassigned.
 * 2) Remove all the failure reports sent by this node and referenced by
 *    other nodes.
 * 3) Free the node with freeClusterNode() that will in turn remove it
 *    from the hash table and from the list of slaves of its master, if
 *    it is a slave node.
 */
/**
 * 从集群中删除一个节点。该函数执行高级清理，调用 freeClusterNode() 进行低级清理。在这里，我们执行以下操作：
 *   1) 将其处理的所有插槽标记为未分配。
 *   2) 删除该节点发送的所有被其他节点引用的失败报告。
 *   3) 使用 freeClusterNode() 释放节点，如果它是从节点，它将依次从哈希表和其主节点的从列表中删除它。
 * */
void clusterDelNode(clusterNode *delnode) {
    int j;
    dictIterator *di;
    dictEntry *de;

    /* 1) Mark slots as unassigned. 将插槽标记为未分配。*/
    for (j = 0; j < CLUSTER_SLOTS; j++) {
        if (server.cluster->importing_slots_from[j] == delnode)
            server.cluster->importing_slots_from[j] = NULL;
        if (server.cluster->migrating_slots_to[j] == delnode)
            server.cluster->migrating_slots_to[j] = NULL;
        if (server.cluster->slots[j] == delnode)
            clusterDelSlot(j);
    }

    /* 2) Remove failure reports. 删除失败报告。 */
    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        if (node == delnode) continue;
        clusterNodeDelFailureReport(node,delnode);
    }
    dictReleaseIterator(di);

    /* 3) Free the node, unlinking it from the cluster. 释放节点，将其与集群取消链接。*/
    freeClusterNode(delnode);
}

/* Cluster node sanity check. Returns C_OK if the node id
 * is valid an C_ERR otherwise. */
//集群节点健全性检查。如果节点 id 有效，则返回 C_OK，否则返回 C_ERR。
int verifyClusterNodeId(const char *name, int length) {
    if (length != CLUSTER_NAMELEN) return C_ERR;
    for (int i = 0; i < length; i++) {
        if (name[i] >= 'a' && name[i] <= 'z') continue;
        if (name[i] >= '0' && name[i] <= '9') continue;
        return C_ERR;
    }
    return C_OK;
}

/* Node lookup by name 按名称查找节点*/
clusterNode *clusterLookupNode(const char *name, int length) {
    if (verifyClusterNodeId(name, length) != C_OK) return NULL;
    sds s = sdsnewlen(name, length);
    dictEntry *de = dictFind(server.cluster->nodes, s);
    sdsfree(s);
    if (de == NULL) return NULL;
    return dictGetVal(de);
}

/* Get all the nodes serving the same slots as the given node. */
//获取与给定节点服务相同插槽的所有节点。
list *clusterGetNodesServingMySlots(clusterNode *node) {
    list *nodes_for_slot = listCreate();
    clusterNode *my_primary = nodeIsMaster(node) ? node : node->slaveof;

    /* This function is only valid for fully connected nodes, so
     * they should have a known primary. */
    //这个函数只对全连接节点有效，所以它们应该有一个已知的主节点。
    serverAssert(my_primary);
    listAddNodeTail(nodes_for_slot, my_primary);
    for (int i=0; i < my_primary->numslaves; i++) {
        listAddNodeTail(nodes_for_slot, my_primary->slaves[i]);
    }
    return nodes_for_slot;
}

/* This is only used after the handshake. When we connect a given IP/PORT
 * as a result of CLUSTER MEET we don't have the node name yet, so we
 * pick a random one, and will fix it when we receive the PONG request using
 * this function. */
/**
 * 这仅在握手后使用。当我们连接一个给定的 IPPORT 作为 CLUSTER MEET 的结果时，我们还没有节点名称，
 * 所以我们选择一个随机的，并在我们使用这个函数收到 PONG 请求时修复它。
 * */
void clusterRenameNode(clusterNode *node, char *newname) {
    int retval;
    sds s = sdsnewlen(node->name, CLUSTER_NAMELEN);

    serverLog(LL_DEBUG,"Renaming node %.40s into %.40s",
        node->name, newname);
    retval = dictDelete(server.cluster->nodes, s);
    sdsfree(s);
    serverAssert(retval == DICT_OK);
    memcpy(node->name, newname, CLUSTER_NAMELEN);
    clusterAddNode(node);
}

/* -----------------------------------------------------------------------------
 * CLUSTER config epoch handling   CLUSTER 配置时期处理
 * -------------------------------------------------------------------------- */

/* Return the greatest configEpoch found in the cluster, or the current
 * epoch if greater than any node configEpoch. */
//返回集群中找到的最大 configEpoch，如果大于任何节点 configEpoch，则返回当前 epoch。
uint64_t clusterGetMaxEpoch(void) {
    uint64_t max = 0;
    dictIterator *di;
    dictEntry *de;

    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        if (node->configEpoch > max) max = node->configEpoch;
    }
    dictReleaseIterator(di);
    if (max < server.cluster->currentEpoch) max = server.cluster->currentEpoch;
    return max;
}

/* If this node epoch is zero or is not already the greatest across the
 * cluster (from the POV of the local configuration), this function will:
 *
 * 1) Generate a new config epoch, incrementing the current epoch.
 * 2) Assign the new epoch to this node, WITHOUT any consensus.
 * 3) Persist the configuration on disk before sending packets with the
 *    new configuration.
 *
 * If the new config epoch is generated and assigned, C_OK is returned,
 * otherwise C_ERR is returned (since the node has already the greatest
 * configuration around) and no operation is performed.
 *
 * Important note: this function violates the principle that config epochs
 * should be generated with consensus and should be unique across the cluster.
 * However Redis Cluster uses this auto-generated new config epochs in two
 * cases:
 *
 * 1) When slots are closed after importing. Otherwise resharding would be
 *    too expensive.
 * 2) When CLUSTER FAILOVER is called with options that force a slave to
 *    failover its master even if there is not master majority able to
 *    create a new configuration epoch.
 *
 * Redis Cluster will not explode using this function, even in the case of
 * a collision between this node and another node, generating the same
 * configuration epoch unilaterally, because the config epoch conflict
 * resolution algorithm will eventually move colliding nodes to different
 * config epochs. However using this function may violate the "last failover
 * wins" rule, so should only be used with care. */
/**
 * 如果此节点 epoch 为零或在整个集群中还不是最大的（从本地配置的 POV），此函数将：
 *   1）生成一个新的配置 epoch，增加当前 epoch。
 *   2) 将新纪元分配给该节点，无需任何共识。
 *   3) 在使用新配置发送数据包之前将配置保存在磁盘上。
 * 如果生成并分配了新的 config epoch，则返回 C_OK，否则返回 C_ERR（因为节点周围已经有最大的配置）并且不执行任何操作。
 * 重要提示：此功能违反了配置 epoch 应由共识生成且在集群中应唯一的原则。
 * 然而，Redis 集群在两种情况下使用这个自动生成的新配置时期：
 *   1）当槽在导入后关闭时。否则重新分片将太昂贵。
 *   2) 当 CLUSTER FAILOVER 调用选项时，即使没有能够创建新配置时期的主节点多数，也强制从节点对其主节点进行故障转移。
 * Redis Cluster使用这个功能不会爆炸，即使在这个节点和另一个节点发生冲突的情况下，单方面生成相同的配置epoch，
 * 因为config epoch冲突解决算法最终会将冲突的节点移动到不同的config epoch。
 * 但是，使用此功能可能会违反“最后一次故障转移获胜”规则，因此应谨慎使用。
 * */
int clusterBumpConfigEpochWithoutConsensus(void) {
    uint64_t maxEpoch = clusterGetMaxEpoch();

    if (myself->configEpoch == 0 ||
        myself->configEpoch != maxEpoch)
    {
        server.cluster->currentEpoch++;
        myself->configEpoch = server.cluster->currentEpoch;
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                             CLUSTER_TODO_FSYNC_CONFIG);
        serverLog(LL_WARNING,
            "New configEpoch set to %llu",
            (unsigned long long) myself->configEpoch);
        return C_OK;
    } else {
        return C_ERR;
    }
}

/* This function is called when this node is a master, and we receive from
 * another master a configuration epoch that is equal to our configuration
 * epoch.
 *
 * BACKGROUND
 *
 * It is not possible that different slaves get the same config
 * epoch during a failover election, because the slaves need to get voted
 * by a majority. However when we perform a manual resharding of the cluster
 * the node will assign a configuration epoch to itself without to ask
 * for agreement. Usually resharding happens when the cluster is working well
 * and is supervised by the sysadmin, however it is possible for a failover
 * to happen exactly while the node we are resharding a slot to assigns itself
 * a new configuration epoch, but before it is able to propagate it.
 *
 * So technically it is possible in this condition that two nodes end with
 * the same configuration epoch.
 *
 * Another possibility is that there are bugs in the implementation causing
 * this to happen.
 *
 * Moreover when a new cluster is created, all the nodes start with the same
 * configEpoch. This collision resolution code allows nodes to automatically
 * end with a different configEpoch at startup automatically.
 *
 * In all the cases, we want a mechanism that resolves this issue automatically
 * as a safeguard. The same configuration epoch for masters serving different
 * set of slots is not harmful, but it is if the nodes end serving the same
 * slots for some reason (manual errors or software bugs) without a proper
 * failover procedure.
 *
 * In general we want a system that eventually always ends with different
 * masters having different configuration epochs whatever happened, since
 * nothing is worse than a split-brain condition in a distributed system.
 *
 * BEHAVIOR
 *
 * When this function gets called, what happens is that if this node
 * has the lexicographically smaller Node ID compared to the other node
 * with the conflicting epoch (the 'sender' node), it will assign itself
 * the greatest configuration epoch currently detected among nodes plus 1.
 *
 * This means that even if there are multiple nodes colliding, the node
 * with the greatest Node ID never moves forward, so eventually all the nodes
 * end with a different configuration epoch.
 */
/**
 * 当该节点是主节点时调用此函数，并且我们从另一个主节点接收到与我们的配置纪元相等的配置纪元。
 * 背景技术在故障转移选举期间，不同的slave不可能获得相同的config epoch，因为slave需要获得多数票。
 * 但是，当我们对集群执行手动重新分片时，节点将为其自身分配一个配置时期，而无需征求同意。
 * 通常在集群运行良好并由系统管理员监督时进行重新分片，
 * 但是故障转移可能恰好在我们重新分片插槽的节点为自己分配一个新的配置纪元时发生，但在它能够传播它之前.
 * 因此从技术上讲，在这种情况下，两个节点可能以相同的配置时期结束。
 * 另一种可能性是实现中存在导致这种情况发生的错误。此外，当创建新集群时，所有节点都以相同的 configEpoch 开始。
 * 此冲突解决代码允许节点在启动时自动以不同的 configEpoch 结束。
 * 在所有情况下，我们都需要一种能够自动解决此问题的机制作为保障。
 * 服务于不同插槽集的主节点的相同配置时期是无害的，但如果节点由于某种原因（手动错误或软件错误）而没有适当的
 * 故障转移过程而终止服务相同的插槽。一般来说，我们想要一个系统，无论发生什么，
 * 最终总是以具有不同配置时期的不同主节点结束，因为没有什么比分布式系统中的脑裂情况更糟糕的了。
 * 行为当这个函数被调用时，发生的情况是，如果这个节点的节点 ID 比另一个节点（“发送者”节点）的字典顺序更小，
 * 它会为自己分配当前在节点中检测到的最大配置 epoch 加上1.
 * 这意味着即使有多个节点发生冲突，具有最大 Node ID 的节点永远不会向前移动，因此最终所有节点都以不同的配置 epoch 结束。
 * */
void clusterHandleConfigEpochCollision(clusterNode *sender) {
    /* Prerequisites: nodes have the same configEpoch and are both masters. */
    //先决条件：节点具有相同的 configEpoch 并且都是 master。
    if (sender->configEpoch != myself->configEpoch ||
        !nodeIsMaster(sender) || !nodeIsMaster(myself)) return;
    /* Don't act if the colliding node has a smaller Node ID. */
    //如果冲突节点的节点 ID 较小，则不要采取行动。
    if (memcmp(sender->name,myself->name,CLUSTER_NAMELEN) <= 0) return;
    /* Get the next ID available at the best of this node knowledge. */
    //充分利用此节点知识获取下一个可用 ID。
    server.cluster->currentEpoch++;
    myself->configEpoch = server.cluster->currentEpoch;
    clusterSaveConfigOrDie(1);
    serverLog(LL_VERBOSE,
        "WARNING: configEpoch collision with node %.40s."
        " configEpoch set to %llu",
        sender->name,
        (unsigned long long) myself->configEpoch);
}

/* -----------------------------------------------------------------------------
 * CLUSTER nodes blacklist
 *
 * The nodes blacklist is just a way to ensure that a given node with a given
 * Node ID is not re-added before some time elapsed (this time is specified
 * in seconds in CLUSTER_BLACKLIST_TTL).
 *
 * This is useful when we want to remove a node from the cluster completely:
 * when CLUSTER FORGET is called, it also puts the node into the blacklist so
 * that even if we receive gossip messages from other nodes that still remember
 * about the node we want to remove, we don't re-add it before some time.
 *
 * Currently the CLUSTER_BLACKLIST_TTL is set to 1 minute, this means
 * that redis-cli has 60 seconds to send CLUSTER FORGET messages to nodes
 * in the cluster without dealing with the problem of other nodes re-adding
 * back the node to nodes we already sent the FORGET command to.
 *
 * The data structure used is a hash table with an sds string representing
 * the node ID as key, and the time when it is ok to re-add the node as
 * value.
 *
 * CLUSTER 节点黑名单
 * 节点黑名单只是一种确保具有给定节点 ID 的给定节点在经过一段时间之前不会重新添加的一种方法
 * （此时间在 CLUSTER_BLACKLIST_TTL 中以秒为单位指定）。
 * 当我们想从集群中完全删除一个节点时，这很有用：当调用 CLUSTER FORGET 时，它也会将该节点放入黑名单，
 * 这样即使我们从其他节点收到八卦消息，仍然记得我们要删除的节点，我们不会在一段时间之前重新添加它。
 * 目前 CLUSTER_BLACKLIST_TTL 设置为 1 分钟，这意味着 redis-cli 有 60 秒的时间
 * 向集群中的节点发送 CLUSTER FORGET 消息，而无需处理其他节点将节点重新添加回我们已经发送过 FORGET 命令的节点的问题至。
 * 使用的数据结构是一个哈希表，其中一个sds字符串表示节点ID作为key，可以重新添加节点的时间作为value。
 * -------------------------------------------------------------------------- */

#define CLUSTER_BLACKLIST_TTL 60      /* 1 minute. */


/* Before of the addNode() or Exists() operations we always remove expired
 * entries from the black list. This is an O(N) operation but it is not a
 * problem since add / exists operations are called very infrequently and
 * the hash table is supposed to contain very little elements at max.
 * However without the cleanup during long uptime and with some automated
 * node add/removal procedures, entries could accumulate. */
/**
 * 在 addNode() 或 Exists() 操作之前，我们总是从黑名单中删除过期条目。
 * 这是一个 O(N) 操作，但这不是问题，因为 add exists 操作很少被调用，并且哈希表应该最多包含非常少的元素。
 * 但是，如果没有在长时间正常运行期间进行清理并使用一些自动节点添加删除程序，条目可能会累积。
 * */
void clusterBlacklistCleanup(void) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetSafeIterator(server.cluster->nodes_black_list);
    while((de = dictNext(di)) != NULL) {
        int64_t expire = dictGetUnsignedIntegerVal(de);

        if (expire < server.unixtime)
            dictDelete(server.cluster->nodes_black_list,dictGetKey(de));
    }
    dictReleaseIterator(di);
}

/* Cleanup the blacklist and add a new node ID to the black list. */
//清理黑名单并将新的节点 ID 添加到黑名单中。
void clusterBlacklistAddNode(clusterNode *node) {
    dictEntry *de;
    sds id = sdsnewlen(node->name,CLUSTER_NAMELEN);

    clusterBlacklistCleanup();
    if (dictAdd(server.cluster->nodes_black_list,id,NULL) == DICT_OK) {
        /* If the key was added, duplicate the sds string representation of
         * the key for the next lookup. We'll free it at the end. */
        //如果添加了密钥，请复制密钥的 sds 字符串表示形式以进行下一次查找。我们将在最后释放它。
        id = sdsdup(id);
    }
    de = dictFind(server.cluster->nodes_black_list,id);
    dictSetUnsignedIntegerVal(de,time(NULL)+CLUSTER_BLACKLIST_TTL);
    sdsfree(id);
}

/* Return non-zero if the specified node ID exists in the blacklist.
 * You don't need to pass an sds string here, any pointer to 40 bytes
 * will work. */
//如果指定的节点 ID 存在于黑名单中，则返回非零值。您不需要在此处传递 sds 字符串，任何指向 40 字节的指针都可以使用。
int clusterBlacklistExists(char *nodeid) {
    sds id = sdsnewlen(nodeid,CLUSTER_NAMELEN);
    int retval;

    clusterBlacklistCleanup();
    retval = dictFind(server.cluster->nodes_black_list,id) != NULL;
    sdsfree(id);
    return retval;
}

/* -----------------------------------------------------------------------------
 * CLUSTER messages exchange - PING/PONG and gossip
 * CLUSTER 消息交换 - PING/PONG and gossip
 * -------------------------------------------------------------------------- */

/* This function checks if a given node should be marked as FAIL.
 * It happens if the following conditions are met:
 *
 * 1) We received enough failure reports from other master nodes via gossip.
 *    Enough means that the majority of the masters signaled the node is
 *    down recently.
 * 2) We believe this node is in PFAIL state.
 *
 * If a failure is detected we also inform the whole cluster about this
 * event trying to force every other node to set the FAIL flag for the node.
 *
 * Note that the form of agreement used here is weak, as we collect the majority
 * of masters state during some time, and even if we force agreement by
 * propagating the FAIL message, because of partitions we may not reach every
 * node. However:
 *
 * 1) Either we reach the majority and eventually the FAIL state will propagate
 *    to all the cluster.
 * 2) Or there is no majority so no slave promotion will be authorized and the
 *    FAIL flag will be cleared after some time.
 */
/**
 * 此函数检查给定节点是否应标记为 FAIL。如果满足以下条件，就会发生这种情况：
 *   1）我们通过 gossip 从其他主节点收到足够多的故障报告。足够意味着大多数主节点最近发出节点已关闭的信号。
 *   2) 我们认为该节点处于 PFAIL 状态。
 * 如果检测到故障，我们还会通知整个集群该事件试图强制每个其他节点为该节点设置 FAIL 标志。
 * 请注意，这里使用的协议形式很弱，因为我们在一段时间内收集了大多数 master 状态，
 * 即使我们通过传播 FAIL 消息来强制协议，由于分区，我们可能无法到达每个节点。然而：
 *   1）要么我们达到多数，最终失败状态将传播到所有集群。
 *   2) 或者没有多数，所以不会授权从站升级，一段时间后FAIL标志将被清除。
 * */
void markNodeAsFailingIfNeeded(clusterNode *node) {
    int failures;
    int needed_quorum = (server.cluster->size / 2) + 1;

    if (!nodeTimedOut(node)) return; /* We can reach it. 我们可以达到它。*/
    if (nodeFailed(node)) return; /* Already FAILing. 已经失败了。*/

    failures = clusterNodeFailureReportsCount(node);
    /* Also count myself as a voter if I'm a master. 如果我是master，也将自己视为选民。*/
    if (nodeIsMaster(myself)) failures++;
    if (failures < needed_quorum) return; /* No weak agreement from masters. 没有来自主人的弱同意。*/

    serverLog(LL_NOTICE,
        "Marking node %.40s as failing (quorum reached).", node->name);

    /* Mark the node as failing. 将节点标记为失败。*/
    node->flags &= ~CLUSTER_NODE_PFAIL;
    node->flags |= CLUSTER_NODE_FAIL;
    node->fail_time = mstime();

    /* Broadcast the failing node name to everybody, forcing all the other
     * reachable nodes to flag the node as FAIL.
     * We do that even if this node is a replica and not a master: anyway
     * the failing state is triggered collecting failure reports from masters,
     * so here the replica is only helping propagating this status. */
    /**
     * 向所有人广播失败的节点名称，强制所有其他可达节点将该节点标记为 FAIL。
     * 即使该节点是副本而不是主节点，我们也会这样做：无论如何都会触发失败状态，从主节点收集失败报告，
     * 所以这里副本只是帮助传播此状态。
     * */
    clusterSendFail(node->name);
    clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
}

/* This function is called only if a node is marked as FAIL, but we are able
 * to reach it again. It checks if there are the conditions to undo the FAIL
 * state. */
//仅当节点被标记为 FAIL 时才调用此函数，但我们能够再次访问它。它检查是否存在撤消 FAIL 状态的条件。
void clearNodeFailureIfNeeded(clusterNode *node) {
    mstime_t now = mstime();

    serverAssert(nodeFailed(node));

    /* For slaves we always clear the FAIL flag if we can contact the
     * node again. */
    //对于从节点，如果我们可以再次联系节点，我们总是清除 FAIL 标志。
    if (nodeIsSlave(node) || node->numslots == 0) {
        serverLog(LL_NOTICE,
            "Clear FAIL state for node %.40s: %s is reachable again.",
                node->name,
                nodeIsSlave(node) ? "replica" : "master without slots");
        node->flags &= ~CLUSTER_NODE_FAIL;
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
    }

    /* If it is a master and...
     * 1) The FAIL state is old enough.
     * 2) It is yet serving slots from our point of view (not failed over).
     * Apparently no one is going to fix these slots, clear the FAIL flag. */
    /**
     * 如果它是主控并且...
     *   1) FAIL 状态已经足够老了。
     *   2) 从我们的角度来看，它仍在服务插槽（没有故障转移）。
     * 显然没有人会修复这些插槽，清除 FAIL 标志。
     * */
    if (nodeIsMaster(node) && node->numslots > 0 &&
        (now - node->fail_time) >
        (server.cluster_node_timeout * CLUSTER_FAIL_UNDO_TIME_MULT))
    {
        serverLog(LL_NOTICE,
            "Clear FAIL state for node %.40s: is reachable again and nobody is serving its slots after some time.",
                node->name);
        node->flags &= ~CLUSTER_NODE_FAIL;
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
    }
}

/* Return true if we already have a node in HANDSHAKE state matching the
 * specified ip address and port number. This function is used in order to
 * avoid adding a new handshake node for the same address multiple times. */
/**
 * 如果我们已经有一个处于 HANDSHAKE 状态的节点匹配指定的 IP 地址和端口号，则返回 true。
 * 使用此功能是为了避免为同一地址多次添加新的握手节点。
 * */
int clusterHandshakeInProgress(char *ip, int port, int cport) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        if (!nodeInHandshake(node)) continue;
        if (!strcasecmp(node->ip,ip) &&
            node->port == port &&
            node->cport == cport) break;
    }
    dictReleaseIterator(di);
    return de != NULL;
}

/* Start a handshake with the specified address if there is not one
 * already in progress. Returns non-zero if the handshake was actually
 * started. On error zero is returned and errno is set to one of the
 * following values:
 *
 * EAGAIN - There is already a handshake in progress for this address.
 * EINVAL - IP or port are not valid. */
/**
 * 如果没有正在进行的握手，则使用指定的地址开始握手。如果实际开始握手，则返回非零值。
 * 出错时返回零并将 errno 设置为以下值之一： EAGAIN - 该地址已经在进行握手。 EINVAL - IP 或端口无效。
 * */
int clusterStartHandshake(char *ip, int port, int cport) {
    clusterNode *n;
    char norm_ip[NET_IP_STR_LEN];
    struct sockaddr_storage sa;

    /* IP sanity check IP 健全性检查*/
    if (inet_pton(AF_INET,ip,
            &(((struct sockaddr_in *)&sa)->sin_addr)))
    {
        sa.ss_family = AF_INET;
    } else if (inet_pton(AF_INET6,ip,
            &(((struct sockaddr_in6 *)&sa)->sin6_addr)))
    {
        sa.ss_family = AF_INET6;
    } else {
        errno = EINVAL;
        return 0;
    }

    /* Port sanity check */
    if (port <= 0 || port > 65535 || cport <= 0 || cport > 65535) {
        errno = EINVAL;
        return 0;
    }

    /* Set norm_ip as the normalized string representation of the node
     * IP address. */
    //将 norm_ip 设置为节点 IP 地址的规范化字符串表示。
    memset(norm_ip,0,NET_IP_STR_LEN);
    if (sa.ss_family == AF_INET)
        inet_ntop(AF_INET,
            (void*)&(((struct sockaddr_in *)&sa)->sin_addr),
            norm_ip,NET_IP_STR_LEN);
    else
        inet_ntop(AF_INET6,
            (void*)&(((struct sockaddr_in6 *)&sa)->sin6_addr),
            norm_ip,NET_IP_STR_LEN);

    if (clusterHandshakeInProgress(norm_ip,port,cport)) {
        errno = EAGAIN;
        return 0;
    }

    /* Add the node with a random address (NULL as first argument to
     * createClusterNode()). Everything will be fixed during the
     * handshake. */
    //添加具有随机地址的节点（NULL 作为 createClusterNode() 的第一个参数）。握手期间一切都将得到解决。
    n = createClusterNode(NULL,CLUSTER_NODE_HANDSHAKE|CLUSTER_NODE_MEET);
    memcpy(n->ip,norm_ip,sizeof(n->ip));
    n->port = port;
    n->cport = cport;
    clusterAddNode(n);
    return 1;
}

/* Process the gossip section of PING or PONG packets.
 * Note that this function assumes that the packet is already sanity-checked
 * by the caller, not in the content of the gossip section, but in the
 * length. */
//处理 PING 或 PONG 数据包的 gossip 部分。请注意，此函数假定调用者已经对数据包进行了完整性检查，
// 而不是在 gossip 部分的内容中，而是在长度中。
void clusterProcessGossipSection(clusterMsg *hdr, clusterLink *link) {
    uint16_t count = ntohs(hdr->count);
    clusterMsgDataGossip *g = (clusterMsgDataGossip*) hdr->data.ping.gossip;
    clusterNode *sender = link->node ? link->node : clusterLookupNode(hdr->sender, CLUSTER_NAMELEN);

    while(count--) {
        uint16_t flags = ntohs(g->flags);
        clusterNode *node;
        sds ci;

        if (server.verbosity == LL_DEBUG) {
            ci = representClusterNodeFlags(sdsempty(), flags);
            serverLog(LL_DEBUG,"GOSSIP %.40s %s:%d@%d %s",
                g->nodename,
                g->ip,
                ntohs(g->port),
                ntohs(g->cport),
                ci);
            sdsfree(ci);
        }

        /* Update our state accordingly to the gossip sections */
        //根据八卦部分更新我们的状态
        node = clusterLookupNode(g->nodename, CLUSTER_NAMELEN);
        if (node) {
            /* We already know this node.
               Handle failure reports, only when the sender is a master. */
            //我们已经知道这个节点。仅当发件人是主服务器时才处理失败报告。
            if (sender && nodeIsMaster(sender) && node != myself) {
                if (flags & (CLUSTER_NODE_FAIL|CLUSTER_NODE_PFAIL)) {
                    if (clusterNodeAddFailureReport(node,sender)) {
                        serverLog(LL_VERBOSE,
                            "Node %.40s reported node %.40s as not reachable.",
                            sender->name, node->name);
                    }
                    markNodeAsFailingIfNeeded(node);
                } else {
                    if (clusterNodeDelFailureReport(node,sender)) {
                        serverLog(LL_VERBOSE,
                            "Node %.40s reported node %.40s is back online.",
                            sender->name, node->name);
                    }
                }
            }

            /* If from our POV the node is up (no failure flags are set),
             * we have no pending ping for the node, nor we have failure
             * reports for this node, update the last pong time with the
             * one we see from the other nodes. */
            /**
             * 如果从我们的 POV 来看，节点已启动（未设置故障标志），我们没有该节点的待处理 ping，
             * 也没有该节点的故障报告，请使用我们从其他节点看到的更新最后一个 pong 时间。
             * */
            if (!(flags & (CLUSTER_NODE_FAIL|CLUSTER_NODE_PFAIL)) &&
                node->ping_sent == 0 &&
                clusterNodeFailureReportsCount(node) == 0)
            {
                mstime_t pongtime = ntohl(g->pong_received);
                pongtime *= 1000; /* Convert back to milliseconds. */

                /* Replace the pong time with the received one only if
                 * it's greater than our view but is not in the future
                 * (with 500 milliseconds tolerance) from the POV of our
                 * clock. */
                //仅当它大于我们的视图但不在我们时钟的 POV 的未来（具有 500 毫秒容差）时，才用收到的时间替换乒乓球时间。
                if (pongtime <= (server.mstime+500) &&
                    pongtime > node->pong_received)
                {
                    node->pong_received = pongtime;
                }
            }

            /* If we already know this node, but it is not reachable, and
             * we see a different address in the gossip section of a node that
             * can talk with this other node, update the address, disconnect
             * the old link if any, so that we'll attempt to connect with the
             * new address. */
            /**
             * 如果我们已经知道这个节点，但它无法访问，并且我们在一个节点的 gossip 部分看到了一个不同的地址，
             * 可以与这个其他节点通信，更新地址，如果有的话，断开旧链接，这样我们就可以尝试连接新地址。
             * */
            if (node->flags & (CLUSTER_NODE_FAIL|CLUSTER_NODE_PFAIL) &&
                !(flags & CLUSTER_NODE_NOADDR) &&
                !(flags & (CLUSTER_NODE_FAIL|CLUSTER_NODE_PFAIL)) &&
                (strcasecmp(node->ip,g->ip) ||
                 node->port != ntohs(g->port) ||
                 node->cport != ntohs(g->cport)))
            {
                if (node->link) freeClusterLink(node->link);
                memcpy(node->ip,g->ip,NET_IP_STR_LEN);
                node->port = ntohs(g->port);
                node->pport = ntohs(g->pport);
                node->cport = ntohs(g->cport);
                node->flags &= ~CLUSTER_NODE_NOADDR;
            }
        } else {
            /* If it's not in NOADDR state and we don't have it, we
             * add it to our trusted dict with exact nodeid and flag.
             * Note that we cannot simply start a handshake against
             * this IP/PORT pairs, since IP/PORT can be reused already,
             * otherwise we risk joining another cluster.
             *
             * Note that we require that the sender of this gossip message
             * is a well known node in our cluster, otherwise we risk
             * joining another cluster. */
            /**
             * 如果它不在 NOADDR 状态并且我们没有它，我们将它添加到我们信任的字典中，并带有确切的 nodeid 和标志。
             * 请注意，我们不能简单地针对这个 IPPORT 对开始握手，因为 IPPORT 已经可以重用，否则我们可能会加入另一个集群。
             * 请注意，我们要求此八卦消息的发送者是我们集群中的知名节点，否则我们可能会加入另一个集群。
             * */
            if (sender &&
                !(flags & CLUSTER_NODE_NOADDR) &&
                !clusterBlacklistExists(g->nodename))
            {
                clusterNode *node;
                node = createClusterNode(g->nodename, flags);
                memcpy(node->ip,g->ip,NET_IP_STR_LEN);
                node->port = ntohs(g->port);
                node->pport = ntohs(g->pport);
                node->cport = ntohs(g->cport);
                clusterAddNode(node);
            }
        }

        /* Next node */
        g++;
    }
}

/* IP -> string conversion. 'buf' is supposed to at least be 46 bytes.
 * If 'announced_ip' length is non-zero, it is used instead of extracting
 * the IP from the socket peer address. */
/**
 * IP -> 字符串转换。 'buf' 应该至少为 46 个字节。如果“announced_ip”长度不为零，则使用它而不是从套接字对等地址中提取 IP。
 * */
void nodeIp2String(char *buf, clusterLink *link, char *announced_ip) {
    if (announced_ip[0] != '\0') {
        memcpy(buf,announced_ip,NET_IP_STR_LEN);
        buf[NET_IP_STR_LEN-1] = '\0'; /* We are not sure the input is sane. */
    } else {
        connPeerToString(link->conn, buf, NET_IP_STR_LEN, NULL);
    }
}

/* Update the node address to the IP address that can be extracted
 * from link->fd, or if hdr->myip is non empty, to the address the node
 * is announcing us. The port is taken from the packet header as well.
 *
 * If the address or port changed, disconnect the node link so that we'll
 * connect again to the new address.
 *
 * If the ip/port pair are already correct no operation is performed at
 * all.
 *
 * The function returns 0 if the node address is still the same,
 * otherwise 1 is returned. */
/**
 * 将节点地址更新为可以从 link->fd 中提取的 IP 地址，或者如果 hdr->myip 不为空，则更新为节点通知我们的地址。
 * 端口也取自数据包标头。如果地址或端口发生更改，请断开节点链接，以便我们再次连接到新地址。
 * 如果 ipport 对已经正确，则根本不执行任何操作。如果节点地址仍然相同，该函数返回 0，否则返回 1。
 * */
int nodeUpdateAddressIfNeeded(clusterNode *node, clusterLink *link,
                              clusterMsg *hdr)
{
    char ip[NET_IP_STR_LEN] = {0};
    int port = ntohs(hdr->port);
    int pport = ntohs(hdr->pport);
    int cport = ntohs(hdr->cport);

    /* We don't proceed if the link is the same as the sender link, as this
     * function is designed to see if the node link is consistent with the
     * symmetric link that is used to receive PINGs from the node.
     *
     * As a side effect this function never frees the passed 'link', so
     * it is safe to call during packet processing. */
    /**
     * 如果链路与发送方链路相同，我们不继续，因为此函数旨在查看节点链路是否与用于从节点接收 PING 的对称链路一致。
     * 作为副作用，此函数永远不会释放传递的“链接”，因此在数据包处理期间调用它是安全的。
     * */
    if (link == node->link) return 0;

    nodeIp2String(ip,link,hdr->myip);
    if (node->port == port && node->cport == cport && node->pport == pport &&
        strcmp(ip,node->ip) == 0) return 0;

    /* IP / port is different, update it. */
    memcpy(node->ip,ip,sizeof(ip));
    node->port = port;
    node->pport = pport;
    node->cport = cport;
    if (node->link) freeClusterLink(node->link);
    node->flags &= ~CLUSTER_NODE_NOADDR;
    serverLog(LL_WARNING,"Address updated for node %.40s, now %s:%d",
        node->name, node->ip, node->port);

    /* Check if this is our master and we have to change the
     * replication target as well. */
    //检查这是否是我们的主服务器，我们也必须更改复制目标。
    if (nodeIsSlave(myself) && myself->slaveof == node)
        replicationSetMaster(node->ip, node->port);
    return 1;
}

/* Reconfigure the specified node 'n' as a master. This function is called when
 * a node that we believed to be a slave is now acting as master in order to
 * update the state of the node. */
//将指定的节点“n”重新配置为主节点。当我们认为是从节点的节点现在充当主节点以更新节点的状态时，将调用此函数。
void clusterSetNodeAsMaster(clusterNode *n) {
    if (nodeIsMaster(n)) return;

    if (n->slaveof) {
        clusterNodeRemoveSlave(n->slaveof,n);
        if (n != myself) n->flags |= CLUSTER_NODE_MIGRATE_TO;
    }
    n->flags &= ~CLUSTER_NODE_SLAVE;
    n->flags |= CLUSTER_NODE_MASTER;
    n->slaveof = NULL;

    /* Update config and state. */
    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                         CLUSTER_TODO_UPDATE_STATE);
}

/* This function is called when we receive a master configuration via a
 * PING, PONG or UPDATE packet. What we receive is a node, a configEpoch of the
 * node, and the set of slots claimed under this configEpoch.
 *
 * What we do is to rebind the slots with newer configuration compared to our
 * local configuration, and if needed, we turn ourself into a replica of the
 * node (see the function comments for more info).
 *
 * The 'sender' is the node for which we received a configuration update.
 * Sometimes it is not actually the "Sender" of the information, like in the
 * case we receive the info via an UPDATE packet. */
/**
 * 当我们通过 PING、PONG 或 UPDATE 数据包收到主配置时调用此函数。
 * 我们收到的是一个节点，一个节点的 configEpoch，以及在这个 configEpoch 下声明的一组槽。
 * 我们所做的是与本地配置相比，使用更新的配置重新绑定插槽，如果需要，
 * 我们将自己变成节点的副本（有关更多信息，请参阅函数注释）。 “发送者”是我们收到配置更新的节点。
 * 有时它实际上不是信息的“发送者”，例如我们通过 UPDATE 数据包接收信息的情况。
 * */
void clusterUpdateSlotsConfigWith(clusterNode *sender, uint64_t senderConfigEpoch, unsigned char *slots) {
    int j;
    clusterNode *curmaster = NULL, *newmaster = NULL;
    /* The dirty slots list is a list of slots for which we lose the ownership
     * while having still keys inside. This usually happens after a failover
     * or after a manual cluster reconfiguration operated by the admin.
     *
     * If the update message is not able to demote a master to slave (in this
     * case we'll resync with the master updating the whole key space), we
     * need to delete all the keys in the slots we lost ownership. */
    /**
     * 脏槽列表是我们失去所有权但内部仍然有密钥的槽的列表。这通常发生在故障转移之后或管理员手动重新配置集群之后。
     * 如果更新消息无法将 master 降级为 slave（在这种情况下，我们将与 master 重新同步更新整个密钥空间），
     * 我们需要删除我们失去所有权的插槽中的所有密钥。
     * */
    uint16_t dirty_slots[CLUSTER_SLOTS];
    int dirty_slots_count = 0;

    /* We should detect if sender is new master of our shard.
     * We will know it if all our slots were migrated to sender, and sender
     * has no slots except ours */
    //我们应该检测发件人是否是我们分片的新主人。如果我们所有的 slot 都迁移到了 sender，
    // 我们就会知道，并且 sender 除了我们的 slot 之外没有 slot
    int sender_slots = 0;
    int migrated_our_slots = 0;

    /* Here we set curmaster to this node or the node this node
     * replicates to if it's a slave. In the for loop we are
     * interested to check if slots are taken away from curmaster. */
    //在这里，我们将 curmaster 设置为该节点或该节点复制到的节点（如果它是从属节点）。
    // 在 for 循环中，我们有兴趣检查插槽是否从 curmaster 中移除。
    curmaster = nodeIsMaster(myself) ? myself : myself->slaveof;

    if (sender == myself) {
        serverLog(LL_WARNING,"Discarding UPDATE message about myself.");
        return;
    }

    for (j = 0; j < CLUSTER_SLOTS; j++) {
        if (bitmapTestBit(slots,j)) {
            sender_slots++;

            /* The slot is already bound to the sender of this message. */
            //该插槽已绑定到此消息的发送者。
            if (server.cluster->slots[j] == sender) continue;

            /* The slot is in importing state, it should be modified only
             * manually via redis-cli (example: a resharding is in progress
             * and the migrating side slot was already closed and is advertising
             * a new config. We still want the slot to be closed manually). */
            /**
             * 插槽处于导入状态，只能通过 redis-cli 手动修改（例如：正在重新分片，迁移侧插槽已关闭并正在通告新配置。
             * 我们仍然希望手动关闭插槽） .
             * */
            if (server.cluster->importing_slots_from[j]) continue;

            /* We rebind the slot to the new node claiming it if:
             * 1) The slot was unassigned or the new node claims it with a
             *    greater configEpoch.
             * 2) We are not currently importing the slot. */
            /**
             * 如果出现以下情况，我们会将插槽重新绑定到声明它的新节点：
             * 1) 插槽未分配或新节点使用更大的 configEpoch 声明它。
             * 2）我们目前没有导入插槽。
             * */
            if (server.cluster->slots[j] == NULL ||
                server.cluster->slots[j]->configEpoch < senderConfigEpoch)
            {
                /* Was this slot mine, and still contains keys? Mark it as
                 * a dirty slot. */
                //这个插槽是我的，并且仍然包含密钥？将其标记为脏插槽。
                if (server.cluster->slots[j] == myself &&
                    countKeysInSlot(j) &&
                    sender != myself)
                {
                    dirty_slots[dirty_slots_count] = j;
                    dirty_slots_count++;
                }

                if (server.cluster->slots[j] == curmaster) {
                    newmaster = sender;
                    migrated_our_slots++;
                }
                clusterDelSlot(j);
                clusterAddSlot(sender,j);
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                     CLUSTER_TODO_UPDATE_STATE|
                                     CLUSTER_TODO_FSYNC_CONFIG);
            }
        }
    }

    /* After updating the slots configuration, don't do any actual change
     * in the state of the server if a module disabled Redis Cluster
     * keys redirections. */
    //更新插槽配置后，如果模块禁用了 Redis 集群键重定向，则不要对服务器状态进行任何实际更改。
    if (server.cluster_module_flags & CLUSTER_MODULE_FLAG_NO_REDIRECTION)
        return;

    /* If at least one slot was reassigned from a node to another node
     * with a greater configEpoch, it is possible that:
     * 1) We are a master left without slots. This means that we were
     *    failed over and we should turn into a replica of the new
     *    master.
     * 2) We are a slave and our master is left without slots. We need
     *    to replicate to the new slots owner. */
    /**
     * 如果至少一个 slot 被从一个节点重新分配到另一个具有更大 configEpoch 的节点，则可能：
     *   1）我们是一个没有 slot 的 master。这意味着我们失败了，我们应该变成新主人的副本。
     *   2）我们是奴隶，我们的主人没有插槽。我们需要复制到新的插槽所有者。
     * */
    if (newmaster && curmaster->numslots == 0 &&
            (server.cluster_allow_replica_migration ||
             sender_slots == migrated_our_slots)) {
        serverLog(LL_WARNING,
            "Configuration change detected. Reconfiguring myself "
            "as a replica of %.40s", sender->name);
        clusterSetMaster(sender);
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                             CLUSTER_TODO_UPDATE_STATE|
                             CLUSTER_TODO_FSYNC_CONFIG);
    } else if (myself->slaveof && myself->slaveof->slaveof) {
        /* Safeguard against sub-replicas. A replica's master can turn itself
         * into a replica if its last slot is removed. If no other node takes
         * over the slot, there is nothing else to trigger replica migration. */
        /**
         * 防止子副本。如果删除了最后一个 slot，则副本的 master 可以将自己变成副本。
         * 如果没有其他节点接管该槽，则没有其他任何东西可以触发副本迁移。
         * */
        serverLog(LL_WARNING,
                  "I'm a sub-replica! Reconfiguring myself as a replica of grandmaster %.40s",
                  myself->slaveof->slaveof->name);
        clusterSetMaster(myself->slaveof->slaveof);
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                             CLUSTER_TODO_UPDATE_STATE|
                             CLUSTER_TODO_FSYNC_CONFIG);
    } else if (dirty_slots_count) {
        /* If we are here, we received an update message which removed
         * ownership for certain slots we still have keys about, but still
         * we are serving some slots, so this master node was not demoted to
         * a slave.
         *
         * In order to maintain a consistent state between keys and slots
         * we need to remove all the keys from the slots we lost. */
        /**
         * 如果我们在这里，我们会收到一条更新消息，该消息删除了我们仍然拥有密钥的某些插槽的所有权，
         * 但我们仍在为一些插槽提供服务，因此该主节点没有降级为从节点。为了保持键和槽之间的一致状态，
         * 我们需要从丢失的槽中删除所有键。
         * */
        for (j = 0; j < dirty_slots_count; j++)
            delKeysInSlot(dirty_slots[j]);
    }
}

/* Cluster ping extensions.
 *
 * The ping/pong/meet messages support arbitrary extensions to add additional
 * metadata to the messages that are sent between the various nodes in the
 * cluster. The extensions take the form:
 * [ Header length + type (8 bytes) ] 
 * [ Extension information (Arbitrary length, but must be 8 byte padded) ]
 */
/**
 * 集群 ping 扩展。 pingpongmeet 消息支持任意扩展，以向集群中各个节点之间发送的消息添加额外的元数据。
 * 扩展的形式为：
 * [ Header length + type (8 bytes) ]
 * [ Extension information (Arbitrary length, but must be 8 byte padding) ]
 * */


/* Returns the length of a given extension */
//返回给定扩展的长度
static uint32_t getPingExtLength(clusterMsgPingExt *ext) {
    return ntohl(ext->length);
}

/* Returns the initial position of ping extensions. May return an invalid
 * address if there are no ping extensions. */
//返回 ping 扩展的初始位置。如果没有 ping 扩展，可能会返回无效地址。
static clusterMsgPingExt *getInitialPingExt(clusterMsg *hdr, uint16_t count) {
    clusterMsgPingExt *initial = (clusterMsgPingExt*) &(hdr->data.ping.gossip[count]);
    return initial;
} 

/* Given a current ping extension, returns the start of the next extension. May return
 * an invalid address if there are no further ping extensions. */
//给定当前的 ping 扩展，返回下一个扩展的开始。如果没有进一步的 ping 扩展，可能会返回无效地址。
static clusterMsgPingExt *getNextPingExt(clusterMsgPingExt *ext) {
    clusterMsgPingExt *next = (clusterMsgPingExt *) (((char *) ext) + getPingExtLength(ext));
    return next;
}

/* Returns the exact size needed to store the hostname. The returned value
 * will be 8 byte padded. */
//返回存储主机名所需的确切大小。返回值将填充 8 个字节。
int getHostnamePingExtSize() {
    /* If hostname is not set, we don't send this extension */
    //如果未设置主机名，我们不会发送此扩展
    if (sdslen(myself->hostname) == 0) return 0;

    int totlen = sizeof(clusterMsgPingExt) + EIGHT_BYTE_ALIGN(sdslen(myself->hostname) + 1);
    return totlen;
}

/* Write the hostname ping extension at the start of the cursor. This function
 * will update the cursor to point to the end of the written extension and
 * will return the amount of bytes written. */
/**
 * 在光标的开头写入主机名 ping 扩展名。此函数将更新光标以指向写入扩展的末尾，并返回写入的字节数。
 * */
int writeHostnamePingExt(clusterMsgPingExt **cursor) {
    /* If hostname is not set, we don't send this extension */
    //如果未设置主机名，我们不会发送此扩展
    if (sdslen(myself->hostname) == 0) return 0;

    /* Add the hostname information at the extension cursor */
    //在扩展光标处添加主机名信息
    clusterMsgPingExtHostname *ext = &(*cursor)->ext[0].hostname;
    memcpy(ext->hostname, myself->hostname, sdslen(myself->hostname));
    uint32_t extension_size = getHostnamePingExtSize();

    /* Move the write cursor 移动写光标*/
    (*cursor)->type = htons(CLUSTERMSG_EXT_TYPE_HOSTNAME);
    (*cursor)->length = htonl(extension_size);
    /* Make sure the string is NULL terminated by adding 1 */
    //通过添加 1 确保字符串为 NULL 终止
    *cursor = (clusterMsgPingExt *) (ext->hostname + EIGHT_BYTE_ALIGN(sdslen(myself->hostname) + 1));
    return extension_size;
}

/* We previously validated the extensions, so this function just needs to
 * handle the extensions. */
//我们之前验证了扩展，所以这个函数只需要处理扩展。
void clusterProcessPingExtensions(clusterMsg *hdr, clusterLink *link) {
    clusterNode *sender = link->node ? link->node : clusterLookupNode(hdr->sender, CLUSTER_NAMELEN);
    char *ext_hostname = NULL;
    uint16_t extensions = ntohs(hdr->extensions);
    /* Loop through all the extensions and process them */
    //循环遍历所有扩展并处理它们
    clusterMsgPingExt *ext = getInitialPingExt(hdr, ntohs(hdr->count));
    while (extensions--) {
        uint16_t type = ntohs(ext->type);
        if (type == CLUSTERMSG_EXT_TYPE_HOSTNAME) {
            clusterMsgPingExtHostname *hostname_ext = (clusterMsgPingExtHostname *) &(ext->ext[0].hostname);
            ext_hostname = hostname_ext->hostname;
        } else {
            /* Unknown type, we will ignore it but log what happened. */
            //未知类型，我们将忽略它但记录发生的事情。
            serverLog(LL_WARNING, "Received unknown extension type %d", type);
        }

        /* We know this will be valid since we validated it ahead of time */
        //我们知道这将是有效的，因为我们提前验证了它
        ext = getNextPingExt(ext);
    }
    /* If the node did not send us a hostname extension, assume
     * they don't have an announced hostname. Otherwise, we'll
     * set it now. */
    //如果节点没有向我们发送主机名扩展，假设他们没有宣布的主机名。否则，我们现在就设置它。
    updateAnnouncedHostname(sender, ext_hostname);
}

static clusterNode *getNodeFromLinkAndMsg(clusterLink *link, clusterMsg *hdr) {
    clusterNode *sender;
    if (link->node && !nodeInHandshake(link->node)) {
        /* If the link has an associated node, use that so that we don't have to look it
         * up every time, except when the node is still in handshake, the node still has
         * a random name thus not truly "known". */
        //如果链接有一个关联节点，使用它这样我们就不必每次都查找它，除非节点仍在握手中，
        // 节点仍然有一个随机名称，因此不是真正的“已知”。
        sender = link->node;
    } else {
        /* Otherwise, fetch sender based on the message */
        //否则，根据消息获取发件人
        sender = clusterLookupNode(hdr->sender, CLUSTER_NAMELEN);
        /* We know the sender node but haven't associate it with the link. This must
         * be an inbound link because only for inbound links we didn't know which node
         * to associate when they were created. */
        /**
         * 我们知道发送者节点，但尚未将其与链接相关联。这必须是一个入站链接，因为只有入站链接在创建时我们不知道要关联哪个节点。
         * */
        if (sender && !link->node) {
            setClusterNodeToInboundClusterLink(sender, link);
        }
    }
    return sender;
}

/* When this function is called, there is a packet to process starting
 * at link->rcvbuf. Releasing the buffer is up to the caller, so this
 * function should just handle the higher level stuff of processing the
 * packet, modifying the cluster state if needed.
 *
 * The function returns 1 if the link is still valid after the packet
 * was processed, otherwise 0 if the link was freed since the packet
 * processing lead to some inconsistency error (for instance a PONG
 * received from the wrong sender ID). */
/**
 * 当这个函数被调用时，有一个从link->rcvbuf 开始的数据包要处理。
 * 释放缓冲区由调用者决定，所以这个函数应该只处理处理数据包的更高级别的东西，如果需要的话修改集群状态。
 * 如果在处理数据包后链接仍然有效，则该函数返回 1，否则如果由于数据包处理导致某些不一致错误
 * （例如从错误的发送者 ID 接收到的 PONG）而释放链接，则该函数返回 0。
 * */
int clusterProcessPacket(clusterLink *link) {
    clusterMsg *hdr = (clusterMsg*) link->rcvbuf;
    uint32_t totlen = ntohl(hdr->totlen);
    uint16_t type = ntohs(hdr->type);
    mstime_t now = mstime();

    if (type < CLUSTERMSG_TYPE_COUNT)
        server.cluster->stats_bus_messages_received[type]++;
    serverLog(LL_DEBUG,"--- Processing packet of type %s, %lu bytes",
        clusterGetMessageTypeString(type), (unsigned long) totlen);

    /* Perform sanity checks */
    if (totlen < 16) return 1; /* At least signature, version, totlen, count.至少签名，版本，totlen，计数。 */
    if (totlen > link->rcvbuf_len) return 1;

    if (ntohs(hdr->ver) != CLUSTER_PROTO_VER) {
        /* Can't handle messages of different versions. */
        //无法处理不同版本的消息。
        return 1;
    }

    if (type == server.cluster_drop_packet_filter) {
        serverLog(LL_WARNING, "Dropping packet that matches debug drop filter");
        return 1;
    }

    uint16_t flags = ntohs(hdr->flags);
    uint16_t extensions = ntohs(hdr->extensions);
    uint64_t senderCurrentEpoch = 0, senderConfigEpoch = 0;
    uint32_t explen; /* expected length of this packet */
    clusterNode *sender;

    if (type == CLUSTERMSG_TYPE_PING || type == CLUSTERMSG_TYPE_PONG ||
        type == CLUSTERMSG_TYPE_MEET)
    {
        uint16_t count = ntohs(hdr->count);

        explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
        explen += (sizeof(clusterMsgDataGossip)*count);

        /* If there is extension data, which doesn't have a fixed length,
         * loop through them and validate the length of it now. */
        //如果存在没有固定长度的扩展数据，则循环它们并立即验证它的长度。
        if (hdr->mflags[0] & CLUSTERMSG_FLAG0_EXT_DATA) {
            clusterMsgPingExt *ext = getInitialPingExt(hdr, count);
            while (extensions--) {
                uint16_t extlen = getPingExtLength(ext);
                if (extlen % 8 != 0) {
                    serverLog(LL_WARNING, "Received a %s packet without proper padding (%d bytes)", 
                        clusterGetMessageTypeString(type), (int) extlen);
                    return 1;
                }
                if ((totlen - explen) < extlen) {
                    serverLog(LL_WARNING, "Received invalid %s packet with extension data that exceeds "
                        "total packet length (%lld)", clusterGetMessageTypeString(type),
                        (unsigned long long) totlen);
                    return 1;
                }
                explen += extlen;
                ext = getNextPingExt(ext);
            }
        }
    } else if (type == CLUSTERMSG_TYPE_FAIL) {
        explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
        explen += sizeof(clusterMsgDataFail);
    } else if (type == CLUSTERMSG_TYPE_PUBLISH || type == CLUSTERMSG_TYPE_PUBLISHSHARD) {
        explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
        explen += sizeof(clusterMsgDataPublish) -
                8 +
                ntohl(hdr->data.publish.msg.channel_len) +
                ntohl(hdr->data.publish.msg.message_len);
    } else if (type == CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST ||
               type == CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK ||
               type == CLUSTERMSG_TYPE_MFSTART)
    {
        explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    } else if (type == CLUSTERMSG_TYPE_UPDATE) {
        explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
        explen += sizeof(clusterMsgDataUpdate);
    } else if (type == CLUSTERMSG_TYPE_MODULE) {
        explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
        explen += sizeof(clusterMsgModule) -
                3 + ntohl(hdr->data.module.msg.len);
    } else {
        /* We don't know this type of packet, so we assume it's well formed. */
        //我们不知道这种类型的数据包，所以我们假设它格式正确。
        explen = totlen;
    }

    if (totlen != explen) {
        serverLog(LL_WARNING, "Received invalid %s packet of length %lld but expected length %lld", 
            clusterGetMessageTypeString(type), (unsigned long long) totlen, (unsigned long long) explen);
        return 1;
    } 

    sender = getNodeFromLinkAndMsg(link, hdr);

    /* Update the last time we saw any data from this node. We
     * use this in order to avoid detecting a timeout from a node that
     * is just sending a lot of data in the cluster bus, for instance
     * because of Pub/Sub. */
    /**
     * 更新我们上次看到来自该节点的任何数据的时间。我们使用它是为了避免检测到节点超时，
     * 该节点刚刚在集群总线中发送大量数据，例如由于 PubSub。
     * */
    if (sender) sender->data_received = now;

    if (sender && !nodeInHandshake(sender)) {
        /* Update our currentEpoch if we see a newer epoch in the cluster. */
        //如果我们在集群中看到更新的 epoch，请更新我们的 currentEpoch。
        senderCurrentEpoch = ntohu64(hdr->currentEpoch);
        senderConfigEpoch = ntohu64(hdr->configEpoch);
        if (senderCurrentEpoch > server.cluster->currentEpoch)
            server.cluster->currentEpoch = senderCurrentEpoch;
        /* Update the sender configEpoch if it is publishing a newer one. */
        //如果要发布较新的发送者 configEpoch，请更新它。
        if (senderConfigEpoch > sender->configEpoch) {
            sender->configEpoch = senderConfigEpoch;
            clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                 CLUSTER_TODO_FSYNC_CONFIG);
        }
        /* Update the replication offset info for this node. */
        //更新此节点的复制偏移信息。
        sender->repl_offset = ntohu64(hdr->offset);
        sender->repl_offset_time = now;
        /* If we are a slave performing a manual failover and our master
         * sent its offset while already paused, populate the MF state. */
        //如果我们是执行手动故障转移的从站，并且我们的主站在已暂停时发送了偏移量，则填充 MF 状态。
        if (server.cluster->mf_end &&
            nodeIsSlave(myself) &&
            myself->slaveof == sender &&
            hdr->mflags[0] & CLUSTERMSG_FLAG0_PAUSED &&
            server.cluster->mf_master_offset == -1)
        {
            server.cluster->mf_master_offset = sender->repl_offset;
            clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_MANUALFAILOVER);
            serverLog(LL_WARNING,
                "Received replication offset for paused "
                "master manual failover: %lld",
                server.cluster->mf_master_offset);
        }
    }

    /* Initial processing of PING and MEET requests replying with a PONG. */
    //PING 和 MEET 请求的初始处理以 PONG 回复。
    if (type == CLUSTERMSG_TYPE_PING || type == CLUSTERMSG_TYPE_MEET) {
        /* We use incoming MEET messages in order to set the address
         * for 'myself', since only other cluster nodes will send us
         * MEET messages on handshakes, when the cluster joins, or
         * later if we changed address, and those nodes will use our
         * official address to connect to us. So by obtaining this address
         * from the socket is a simple way to discover / update our own
         * address in the cluster without it being hardcoded in the config.
         *
         * However if we don't have an address at all, we update the address
         * even with a normal PING packet. If it's wrong it will be fixed
         * by MEET later. */
        /**
         * 我们使用传入的 MEET 消息来设置“我自己”的地址，因为只有其他集群节点会在握手时向我们发送 MEET 消息，
         * 当集群加入时，或者稍后如果我们更改地址，这些节点将使用我们的官方地址来连接到我们。
         * 因此，通过从套接字获取此地址是一种在集群中发现更新我们自己的地址的简单方法，而无需在配置中对其进行硬编码。
         * 但是，如果我们根本没有地址，即使使用正常的 PING 数据包，我们也会更新地址。如有错误，MEET 稍后会修复。
         * */
        if ((type == CLUSTERMSG_TYPE_MEET || myself->ip[0] == '\0') &&
            server.cluster_announce_ip == NULL)
        {
            char ip[NET_IP_STR_LEN];

            if (connSockName(link->conn,ip,sizeof(ip),NULL) != -1 &&
                strcmp(ip,myself->ip))
            {
                memcpy(myself->ip,ip,NET_IP_STR_LEN);
                serverLog(LL_WARNING,"IP address for this node updated to %s",
                    myself->ip);
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
            }
        }

        /* Add this node if it is new for us and the msg type is MEET.
         * In this stage we don't try to add the node with the right
         * flags, slaveof pointer, and so forth, as this details will be
         * resolved when we'll receive PONGs from the node. */
        /**
         * 如果它对我们来说是新的并且消息类型是 MEET，请添加此节点。
         * 在这个阶段，我们不会尝试使用正确的标志、slaveof 指针等添加节点，因为当我们从节点接收 PONG 时，这些细节将得到解决。
         * */
        if (!sender && type == CLUSTERMSG_TYPE_MEET) {
            clusterNode *node;

            node = createClusterNode(NULL,CLUSTER_NODE_HANDSHAKE);
            nodeIp2String(node->ip,link,hdr->myip);
            node->port = ntohs(hdr->port);
            node->pport = ntohs(hdr->pport);
            node->cport = ntohs(hdr->cport);
            clusterAddNode(node);
            clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
        }

        /* If this is a MEET packet from an unknown node, we still process
         * the gossip section here since we have to trust the sender because
         * of the message type. */
        //如果这是来自未知节点的 MEET 数据包，我们仍然在此处处理 gossip 部分，因为由于消息类型，我们必须信任发送者。
        if (!sender && type == CLUSTERMSG_TYPE_MEET)
            clusterProcessGossipSection(hdr,link);

        /* Anyway reply with a PONG */
        clusterSendPing(link,CLUSTERMSG_TYPE_PONG);
    }

    /* PING, PONG, MEET: process config information. */
    if (type == CLUSTERMSG_TYPE_PING || type == CLUSTERMSG_TYPE_PONG ||
        type == CLUSTERMSG_TYPE_MEET)
    {
        serverLog(LL_DEBUG,"%s packet received: %.40s",
            clusterGetMessageTypeString(type),
            link->node ? link->node->name : "NULL");
        if (!link->inbound) {
            if (nodeInHandshake(link->node)) {
                /* If we already have this node, try to change the
                 * IP/port of the node with the new one. */
                //如果我们已经有了这个节点，尝试用新的节点更改节点的 IPport。
                if (sender) {
                    serverLog(LL_VERBOSE,
                        "Handshake: we already know node %.40s, "
                        "updating the address if needed.", sender->name);
                    if (nodeUpdateAddressIfNeeded(sender,link,hdr))
                    {
                        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                             CLUSTER_TODO_UPDATE_STATE);
                    }
                    /* Free this node as we already have it. This will
                     * cause the link to be freed as well. */
                    //释放这个节点，因为我们已经有了它。这也将导致链接被释放。
                    clusterDelNode(link->node);
                    return 0;
                }

                /* First thing to do is replacing the random name with the
                 * right node name if this was a handshake stage. */
                //如果这是握手阶段，首先要做的是用正确的节点名称替换随机名称。
                clusterRenameNode(link->node, hdr->sender);
                serverLog(LL_DEBUG,"Handshake with node %.40s completed.",
                    link->node->name);
                link->node->flags &= ~CLUSTER_NODE_HANDSHAKE;
                link->node->flags |= flags&(CLUSTER_NODE_MASTER|CLUSTER_NODE_SLAVE);
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
            } else if (memcmp(link->node->name,hdr->sender,
                        CLUSTER_NAMELEN) != 0)
            {
                /* If the reply has a non matching node ID we
                 * disconnect this node and set it as not having an associated
                 * address. */
                //如果回复的节点 ID 不匹配，我们断开该节点并将其设置为没有关联地址。
                serverLog(LL_DEBUG,"PONG contains mismatching sender ID. About node %.40s added %d ms ago, having flags %d",
                    link->node->name,
                    (int)(now-(link->node->ctime)),
                    link->node->flags);
                link->node->flags |= CLUSTER_NODE_NOADDR;
                link->node->ip[0] = '\0';
                link->node->port = 0;
                link->node->pport = 0;
                link->node->cport = 0;
                freeClusterLink(link);
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
                return 0;
            }
        }

        /* Copy the CLUSTER_NODE_NOFAILOVER flag from what the sender
         * announced. This is a dynamic flag that we receive from the
         * sender, and the latest status must be trusted. We need it to
         * be propagated because the slave ranking used to understand the
         * delay of each slave in the voting process, needs to know
         * what are the instances really competing. */
        /**
         * 从发件人宣布的内容中复制 CLUSTER_NODE_NOFAILOVER 标志。
         * 这是我们从发件人那里收到的动态标志，必须信任最新状态。我们需要传播它，
         * 因为从属排名用于了解每个从属在投票过程中的延迟，需要知道哪些实例真正竞争。
         * */
        if (sender) {
            int nofailover = flags & CLUSTER_NODE_NOFAILOVER;
            sender->flags &= ~CLUSTER_NODE_NOFAILOVER;
            sender->flags |= nofailover;
        }

        /* Update the node address if it changed. */
        //如果更改了节点地址，请更新它。
        if (sender && type == CLUSTERMSG_TYPE_PING &&
            !nodeInHandshake(sender) &&
            nodeUpdateAddressIfNeeded(sender,link,hdr))
        {
            clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                 CLUSTER_TODO_UPDATE_STATE);
        }

        /* Update our info about the node */
        //更新我们关于节点的信息
        if (!link->inbound && type == CLUSTERMSG_TYPE_PONG) {
            link->node->pong_received = now;
            link->node->ping_sent = 0;

            /* The PFAIL condition can be reversed without external
             * help if it is momentary (that is, if it does not
             * turn into a FAIL state).
             *
             * The FAIL condition is also reversible under specific
             * conditions detected by clearNodeFailureIfNeeded(). */
            /**
             * 如果 PFAIL 条件是暂时的（即，如果它没有变成 FAIL 状态），则无需外部帮助即可逆转 PFAIL 条件。
             * 在 clearNodeFailureIfNeeded() 检测到的特定条件下，FAIL 条件也是可逆的。
             * */
            if (nodeTimedOut(link->node)) {
                link->node->flags &= ~CLUSTER_NODE_PFAIL;
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                     CLUSTER_TODO_UPDATE_STATE);
            } else if (nodeFailed(link->node)) {
                clearNodeFailureIfNeeded(link->node);
            }
        }

        /* Check for role switch: slave -> master or master -> slave. */
        //检查角色切换：slave -> master 或 master -> slave。
        if (sender) {
            if (!memcmp(hdr->slaveof,CLUSTER_NODE_NULL_NAME,
                sizeof(hdr->slaveof)))
            {
                /* Node is a master. */
                clusterSetNodeAsMaster(sender);
            } else {
                /* Node is a slave. */
                clusterNode *master = clusterLookupNode(hdr->slaveof, CLUSTER_NAMELEN);

                if (nodeIsMaster(sender)) {
                    /* Master turned into a slave! Reconfigure the node. */
                    //主人变成了奴隶！重新配置节点。
                    clusterDelNodeSlots(sender);
                    sender->flags &= ~(CLUSTER_NODE_MASTER|
                                       CLUSTER_NODE_MIGRATE_TO);
                    sender->flags |= CLUSTER_NODE_SLAVE;

                    /* Update config and state. */
                    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                         CLUSTER_TODO_UPDATE_STATE);
                }

                /* Master node changed for this slave? */
                //为这个从站更改了主节点？
                if (master && sender->slaveof != master) {
                    if (sender->slaveof)
                        clusterNodeRemoveSlave(sender->slaveof,sender);
                    clusterNodeAddSlave(master,sender);
                    sender->slaveof = master;

                    /* Update config. */
                    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
                }
            }
        }

        /* Update our info about served slots.
         *
         * Note: this MUST happen after we update the master/slave state
         * so that CLUSTER_NODE_MASTER flag will be set. */

        /* Many checks are only needed if the set of served slots this
         * instance claims is different compared to the set of slots we have
         * for it. Check this ASAP to avoid other computational expansive
         * checks later. */
        /**
         * 更新我们关于服务插槽的信息。注意：这必须在我们更新主从状态之后发生，以便设置 CLUSTER_NODE_MASTER 标志。
         * 仅当此实例声称的服务插槽集与我们为其拥有的插槽集不同时，才需要进行许多检查。
         * 尽快检查此项以避免以后进行其他计算扩展检查。
         * */
        clusterNode *sender_master = NULL; /* Sender or its master if slave. 发送者或它的master，如果是slave。*/
        int dirty_slots = 0; /* Sender claimed slots don't match my view? 发件人声称的插槽与我的观点不符？*/

        if (sender) {
            sender_master = nodeIsMaster(sender) ? sender : sender->slaveof;
            if (sender_master) {
                dirty_slots = memcmp(sender_master->slots,
                        hdr->myslots,sizeof(hdr->myslots)) != 0;
            }
        }

        /* 1) If the sender of the message is a master, and we detected that
         *    the set of slots it claims changed, scan the slots to see if we
         *    need to update our configuration. */
        /**
         * 1) 如果消息的发送者是 master，并且我们检测到它声称的 slot 集发生了变化，
         *    请扫描 slot 以查看我们是否需要更新我们的配置。
         * */
        if (sender && nodeIsMaster(sender) && dirty_slots)
            clusterUpdateSlotsConfigWith(sender,senderConfigEpoch,hdr->myslots);

        /* 2) We also check for the reverse condition, that is, the sender
         *    claims to serve slots we know are served by a master with a
         *    greater configEpoch. If this happens we inform the sender.
         *
         * This is useful because sometimes after a partition heals, a
         * reappearing master may be the last one to claim a given set of
         * hash slots, but with a configuration that other instances know to
         * be deprecated. Example:
         *
         * A and B are master and slave for slots 1,2,3.
         * A is partitioned away, B gets promoted.
         * B is partitioned away, and A returns available.
         *
         * Usually B would PING A publishing its set of served slots and its
         * configEpoch, but because of the partition B can't inform A of the
         * new configuration, so other nodes that have an updated table must
         * do it. In this way A will stop to act as a master (or can try to
         * failover if there are the conditions to win the election). */
        /**
         * 2）我们还检查了相反的情况，即发送者声称为我们知道由具有更大 configEpoch 的主服务器提供服务的插槽提供服务。
         * 如果发生这种情况，我们会通知发件人。这很有用，因为有时在分区愈合后，
         * 重新出现的 master 可能是最后一个声明给定哈希槽集的 master，但其他实例知道不推荐使用的配置。
         * 示例：
         *   A 和 B 是插槽 1、2、3 的主从。
         *   A被分割，B被提升。
         *   B 被分区，A 返回可用。
         * 通常 B 会 PING A 发布其服务槽集及其 configEpoch，但由于分区 B 无法通知 A 新配置，
         * 因此具有更新表的其他节点必须这样做。
         * 这样A就停止做master了(or can try to failover if there are the conditions to win the election).
         * */
        if (sender && dirty_slots) {
            int j;

            for (j = 0; j < CLUSTER_SLOTS; j++) {
                if (bitmapTestBit(hdr->myslots,j)) {
                    if (server.cluster->slots[j] == sender ||
                        server.cluster->slots[j] == NULL) continue;
                    if (server.cluster->slots[j]->configEpoch >
                        senderConfigEpoch)
                    {
                        serverLog(LL_VERBOSE,
                            "Node %.40s has old slots configuration, sending "
                            "an UPDATE message about %.40s",
                                sender->name, server.cluster->slots[j]->name);
                        clusterSendUpdate(sender->link,
                            server.cluster->slots[j]);

                        /* TODO: instead of exiting the loop send every other
                         * UPDATE packet for other nodes that are the new owner
                         * of sender's slots. */
                        //作为发送方插槽的新所有者的其他节点的 UPDATE 数据包。
                        break;
                    }
                }
            }
        }

        /* If our config epoch collides with the sender's try to fix
         * the problem. */
        //如果我们的 config epoch 与发送者的尝试解决问题发生冲突。
        if (sender &&
            nodeIsMaster(myself) && nodeIsMaster(sender) &&
            senderConfigEpoch == myself->configEpoch)
        {
            clusterHandleConfigEpochCollision(sender);
        }

        /* Get info from the gossip section */
        //从八卦部分获取信息
        if (sender) {
            clusterProcessGossipSection(hdr,link);
            clusterProcessPingExtensions(hdr,link);
        }
    } else if (type == CLUSTERMSG_TYPE_FAIL) {
        clusterNode *failing;

        if (sender) {
            failing = clusterLookupNode(hdr->data.fail.about.nodename, CLUSTER_NAMELEN);
            if (failing &&
                !(failing->flags & (CLUSTER_NODE_FAIL|CLUSTER_NODE_MYSELF)))
            {
                serverLog(LL_NOTICE,
                    "FAIL message received from %.40s about %.40s",
                    hdr->sender, hdr->data.fail.about.nodename);
                failing->flags |= CLUSTER_NODE_FAIL;
                failing->fail_time = now;
                failing->flags &= ~CLUSTER_NODE_PFAIL;
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                     CLUSTER_TODO_UPDATE_STATE);
            }
        } else {
            serverLog(LL_NOTICE,
                "Ignoring FAIL message from unknown node %.40s about %.40s",
                hdr->sender, hdr->data.fail.about.nodename);
        }
    } else if (type == CLUSTERMSG_TYPE_PUBLISH || type == CLUSTERMSG_TYPE_PUBLISHSHARD) {
        if (!sender) return 1;  /* We don't know that node. */

        robj *channel, *message;
        uint32_t channel_len, message_len;

        /* Don't bother creating useless objects if there are no
         * Pub/Sub subscribers. */
        //如果没有 Pub/Sub 订阅者，请不要费心创建无用的对象。
        if ((type == CLUSTERMSG_TYPE_PUBLISH
            && serverPubsubSubscriptionCount() > 0)
        || (type == CLUSTERMSG_TYPE_PUBLISHSHARD
            && serverPubsubShardSubscriptionCount() > 0))
        {
            channel_len = ntohl(hdr->data.publish.msg.channel_len);
            message_len = ntohl(hdr->data.publish.msg.message_len);
            channel = createStringObject(
                        (char*)hdr->data.publish.msg.bulk_data,channel_len);
            message = createStringObject(
                        (char*)hdr->data.publish.msg.bulk_data+channel_len,
                        message_len);
            pubsubPublishMessage(channel, message, type == CLUSTERMSG_TYPE_PUBLISHSHARD);
            decrRefCount(channel);
            decrRefCount(message);
        }
    } else if (type == CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST) {
        if (!sender) return 1;  /* We don't know that node. 我们不知道那个节点。*/
        clusterSendFailoverAuthIfNeeded(sender,hdr);
    } else if (type == CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK) {
        if (!sender) return 1;  /* We don't know that node. */
        /* We consider this vote only if the sender is a master serving
         * a non zero number of slots, and its currentEpoch is greater or
         * equal to epoch where this node started the election. */
        /**
         * 只有当发送者是服务于非零数量槽的主节点，并且其 currentEpoch 大于或等于
         * 该节点开始选举的 epoch 时，我们才会考虑此投票。
         * */
        if (nodeIsMaster(sender) && sender->numslots > 0 &&
            senderCurrentEpoch >= server.cluster->failover_auth_epoch)
        {
            server.cluster->failover_auth_count++;
            /* Maybe we reached a quorum here, set a flag to make sure
             * we check ASAP. */
            //也许我们在这里达到了法定人数，设置一个标志以确保我们尽快检查。
            clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_FAILOVER);
        }
    } else if (type == CLUSTERMSG_TYPE_MFSTART) {
        /* This message is acceptable only if I'm a master and the sender
         * is one of my slaves. */
        //仅当我是master并且发件人是我的slave之一时，此消息才可接受。
        if (!sender || sender->slaveof != myself) return 1;
        /* Manual failover requested from slaves. Initialize the state
         * accordingly. */
        //从服务器请求手动故障转移。相应地初始化状态。
        resetManualFailover();
        server.cluster->mf_end = now + CLUSTER_MF_TIMEOUT;
        server.cluster->mf_slave = sender;
        pauseClients(PAUSE_DURING_FAILOVER,
                     now + (CLUSTER_MF_TIMEOUT * CLUSTER_MF_PAUSE_MULT),
                     CLIENT_PAUSE_WRITE);
        serverLog(LL_WARNING,"Manual failover requested by replica %.40s.",
            sender->name);
        /* We need to send a ping message to the replica, as it would carry
         * `server.cluster->mf_master_offset`, which means the master paused clients
         * at offset `server.cluster->mf_master_offset`, so that the replica would
         * know that it is safe to set its `server.cluster->mf_can_start` to 1 so as
         * to complete failover as quickly as possible. */
        /**
         * 我们需要向副本发送一个 ping 消息，因为它会携带 `server.cluster->mf_master_offset`，
         * 这意味着 master 在偏移量 `server.cluster->mf_master_offset` 处暂停了客户端，
         * 以便副本知道它是安全地将其 `server.cluster->mf_can_start` 设置为 1，以便尽快完成故障转移。
         * */
        clusterSendPing(link, CLUSTERMSG_TYPE_PING);
    } else if (type == CLUSTERMSG_TYPE_UPDATE) {
        clusterNode *n; /* The node the update is about. 更新所涉及的节点。*/
        uint64_t reportedConfigEpoch =
                    ntohu64(hdr->data.update.nodecfg.configEpoch);

        if (!sender) return 1;  /* We don't know the sender. 我们不知道发件人。*/
        n = clusterLookupNode(hdr->data.update.nodecfg.nodename, CLUSTER_NAMELEN);
        if (!n) return 1;   /* We don't know the reported node. 我们不知道报告的节点。*/
        if (n->configEpoch >= reportedConfigEpoch) return 1; /* Nothing new. */

        /* If in our current config the node is a slave, set it as a master. */
        //如果在我们当前的配置中节点是从属节点，则将其设置为主节点。
        if (nodeIsSlave(n)) clusterSetNodeAsMaster(n);

        /* Update the node's configEpoch. 更新节点的 configEpoch。*/
        n->configEpoch = reportedConfigEpoch;
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                             CLUSTER_TODO_FSYNC_CONFIG);

        /* Check the bitmap of served slots and update our
         * config accordingly. */
        //检查服务插槽的位图并相应地更新我们的配置。
        clusterUpdateSlotsConfigWith(n,reportedConfigEpoch,
            hdr->data.update.nodecfg.slots);
    } else if (type == CLUSTERMSG_TYPE_MODULE) {
        if (!sender) return 1;  /* Protect the module from unknown nodes. 保护模块免受未知节点的影响。*/
        /* We need to route this message back to the right module subscribed
         * for the right message type. */
        //我们需要将此消息路由回订阅正确消息类型的正确模块。
        uint64_t module_id = hdr->data.module.msg.module_id; /* Endian-safe ID */
        uint32_t len = ntohl(hdr->data.module.msg.len);
        uint8_t type = hdr->data.module.msg.type;
        unsigned char *payload = hdr->data.module.msg.bulk_data;
        moduleCallClusterReceivers(sender->name,module_id,type,payload,len);
    } else {
        serverLog(LL_WARNING,"Received unknown packet type: %d", type);
    }
    return 1;
}

/* This function is called when we detect the link with this node is lost.
   We set the node as no longer connected. The Cluster Cron will detect
   this connection and will try to get it connected again.

   Instead if the node is a temporary node used to accept a query, we
   completely free the node on error. */
/**
 * 当我们检测到与该节点的链接丢失时调用此函数。我们将节点设置为不再连接。
 * Cluster Cron 将检测到此连接并尝试再次连接。相反，如果节点是用于接受查询的临时节点，我们会在出错时完全释放节点。
 * */
void handleLinkIOError(clusterLink *link) {
    freeClusterLink(link);
}

/* Send data. This is handled using a trivial send buffer that gets
 * consumed by write(). We don't try to optimize this for speed too much
 * as this is a very low traffic channel. */
//发送数据。这是使用 write() 消耗的普通发送缓冲区来处理的。我们不会尝试过多地优化此速度，因为这是一个非常低流量的通道。
void clusterWriteHandler(connection *conn) {
    clusterLink *link = connGetPrivateData(conn);
    ssize_t nwritten;

    nwritten = connWrite(conn, link->sndbuf, sdslen(link->sndbuf));
    if (nwritten <= 0) {
        serverLog(LL_DEBUG,"I/O error writing to node link: %s",
            (nwritten == -1) ? connGetLastError(conn) : "short write");
        handleLinkIOError(link);
        return;
    }
    sdsrange(link->sndbuf,nwritten,-1);
    if (sdslen(link->sndbuf) == 0)
        connSetWriteHandler(link->conn, NULL);
}

/* A connect handler that gets called when a connection to another node
 * gets established.
 */
//建立与另一个节点的连接时调用的连接处理程序。
void clusterLinkConnectHandler(connection *conn) {
    clusterLink *link = connGetPrivateData(conn);
    clusterNode *node = link->node;

    /* Check if connection succeeded */
    //检查连接是否成功
    if (connGetState(conn) != CONN_STATE_CONNECTED) {
        serverLog(LL_VERBOSE, "Connection with Node %.40s at %s:%d failed: %s",
                node->name, node->ip, node->cport,
                connGetLastError(conn));
        freeClusterLink(link);
        return;
    }

    /* Register a read handler from now on */
    //从现在开始注册一个读取处理程序
    connSetReadHandler(conn, clusterReadHandler);

    /* Queue a PING in the new connection ASAP: this is crucial
     * to avoid false positives in failure detection.
     *
     * If the node is flagged as MEET, we send a MEET message instead
     * of a PING one, to force the receiver to add us in its node
     * table. */
    /**
     * 尽快在新连接中排队 PING：这对于避免故障检测中的误报至关重要。
     * 如果节点被标记为 MEET，我们发送 MEET 消息而不是 PING 消息，以强制接收者将我们添加到其节点表中。
     * */
    mstime_t old_ping_sent = node->ping_sent;
    clusterSendPing(link, node->flags & CLUSTER_NODE_MEET ?
            CLUSTERMSG_TYPE_MEET : CLUSTERMSG_TYPE_PING);
    if (old_ping_sent) {
        /* If there was an active ping before the link was
         * disconnected, we want to restore the ping time, otherwise
         * replaced by the clusterSendPing() call. */
        //如果在链接断开之前有一个活动的 ping，我们想要恢复 ping 时间，否则由 clusterSendPing() 调用代替。
        node->ping_sent = old_ping_sent;
    }
    /* We can clear the flag after the first packet is sent.
     * If we'll never receive a PONG, we'll never send new packets
     * to this node. Instead after the PONG is received and we
     * are no longer in meet/handshake status, we want to send
     * normal PING packets. */
    /**
     * 我们可以在发送第一个数据包后清除该标志。如果我们永远不会收到 PONG，我们就永远不会向该节点发送新数据包。
     * 相反，在收到 PONG 并且我们不再处于 meethandshake 状态后，我们希望发送正常的 PING 数据包。
     * */
    node->flags &= ~CLUSTER_NODE_MEET;

    serverLog(LL_DEBUG,"Connecting with Node %.40s at %s:%d",
            node->name, node->ip, node->cport);
}

/* Read data. Try to read the first field of the header first to check the
 * full length of the packet. When a whole packet is in memory this function
 * will call the function to process the packet. And so forth. */
//读取数据。尝试首先读取包头的第一个字段以检查数据包的全长。当整个数据包在内存中时，此函数将调用该函数来处理数据包。等等。
void clusterReadHandler(connection *conn) {
    clusterMsg buf[1];
    ssize_t nread;
    clusterMsg *hdr;
    clusterLink *link = connGetPrivateData(conn);
    unsigned int readlen, rcvbuflen;

    while(1) { /* Read as long as there is data to read. 只要有数据要读取，就读取。*/
        rcvbuflen = link->rcvbuf_len;
        if (rcvbuflen < 8) {
            /* First, obtain the first 8 bytes to get the full message
             * length. */
            //首先，获取前 8 个字节以获得完整的消息长度。
            readlen = 8 - rcvbuflen;
        } else {
            /* Finally read the full message. */
            //最后阅读全文。
            hdr = (clusterMsg*) link->rcvbuf;
            if (rcvbuflen == 8) {
                /* Perform some sanity check on the message signature
                 * and length. */
                //对消息签名和长度执行一些完整性检查。
                if (memcmp(hdr->sig,"RCmb",4) != 0 ||
                    ntohl(hdr->totlen) < CLUSTERMSG_MIN_LEN)
                {
                    serverLog(LL_WARNING,
                        "Bad message length or signature received "
                        "from Cluster bus.");
                    handleLinkIOError(link);
                    return;
                }
            }
            readlen = ntohl(hdr->totlen) - rcvbuflen;
            if (readlen > sizeof(buf)) readlen = sizeof(buf);
        }

        nread = connRead(conn,buf,readlen);
        if (nread == -1 && (connGetState(conn) == CONN_STATE_CONNECTED)) return; /* No more data ready. */

        if (nread <= 0) {
            /* I/O error... */
            serverLog(LL_DEBUG,"I/O error reading from node link: %s",
                (nread == 0) ? "connection closed" : connGetLastError(conn));
            handleLinkIOError(link);
            return;
        } else {
            /* Read data and recast the pointer to the new buffer. */
            //读取数据并重铸指向新缓冲区的指针。
            size_t unused = link->rcvbuf_alloc - link->rcvbuf_len;
            if ((size_t)nread > unused) {
                size_t required = link->rcvbuf_len + nread;
                /* If less than 1mb, grow to twice the needed size, if larger grow by 1mb. */
                //如果小于 1mb，则增长到所需大小的两倍，如果更大，则增长 1mb。
                link->rcvbuf_alloc = required < RCVBUF_MAX_PREALLOC ? required * 2: required + RCVBUF_MAX_PREALLOC;
                link->rcvbuf = zrealloc(link->rcvbuf, link->rcvbuf_alloc);
            }
            memcpy(link->rcvbuf + link->rcvbuf_len, buf, nread);
            link->rcvbuf_len += nread;
            hdr = (clusterMsg*) link->rcvbuf;
            rcvbuflen += nread;
        }

        /* Total length obtained? Process this packet. */
        //获得的总长度？处理这个数据包。
        if (rcvbuflen >= 8 && rcvbuflen == ntohl(hdr->totlen)) {
            if (clusterProcessPacket(link)) {
                if (link->rcvbuf_alloc > RCVBUF_INIT_LEN) {
                    zfree(link->rcvbuf);
                    link->rcvbuf = zmalloc(link->rcvbuf_alloc = RCVBUF_INIT_LEN);
                }
                link->rcvbuf_len = 0;
            } else {
                return; /* Link no longer valid.链接不再有效。 */
            }
        }
    }
}

/* Put stuff into the send buffer.
 *
 * It is guaranteed that this function will never have as a side effect
 * the link to be invalidated, so it is safe to call this function
 * from event handlers that will do stuff with the same link later. */
/**
 * 将东西放入发送缓冲区。保证此函数永远不会对链接失效产生副作用，因此可以安全地从事件处理程序调用此函数，
 * 这些事件处理程序稍后将使用相同的链接进行处理。
 * */
void clusterSendMessage(clusterLink *link, unsigned char *msg, size_t msglen) {
    if (sdslen(link->sndbuf) == 0 && msglen != 0)
        connSetWriteHandlerWithBarrier(link->conn, clusterWriteHandler, 1);

    link->sndbuf = sdscatlen(link->sndbuf, msg, msglen);

    /* Populate sent messages stats. 填充已发送消息的统计信息。*/
    clusterMsg *hdr = (clusterMsg*) msg;
    uint16_t type = ntohs(hdr->type);
    if (type < CLUSTERMSG_TYPE_COUNT)
        server.cluster->stats_bus_messages_sent[type]++;
}

/* Send a message to all the nodes that are part of the cluster having
 * a connected link.
 *
 * It is guaranteed that this function will never have as a side effect
 * some node->link to be invalidated, so it is safe to call this function
 * from event handlers that will do stuff with node links later. */
/**
 * 向集群中具有连接链路的所有节点发送消息。保证这个函数永远不会有一些节点->链接无效的副作用，
 * 所以从事件处理程序调用这个函数是安全的，这些事件处理程序稍后会处理节点链接。
 * */
void clusterBroadcastMessage(void *buf, size_t len) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        if (!node->link) continue;
        if (node->flags & (CLUSTER_NODE_MYSELF|CLUSTER_NODE_HANDSHAKE))
            continue;
        clusterSendMessage(node->link,buf,len);
    }
    dictReleaseIterator(di);
}

/* Build the message header. hdr must point to a buffer at least
 * sizeof(clusterMsg) in bytes. */
//构建消息头。 hdr 必须指向至少 sizeof(clusterMsg) 字节的缓冲区。
void clusterBuildMessageHdr(clusterMsg *hdr, int type) {
    int totlen = 0;
    uint64_t offset;
    clusterNode *master;

    /* If this node is a master, we send its slots bitmap and configEpoch.
     * If this node is a slave we send the master's information instead (the
     * node is flagged as slave so the receiver knows that it is NOT really
     * in charge for this slots. */
    /**
     * 如果这个节点是主节点，我们发送它的槽位图和 configEpoch。如果该节点是从节点，
     * 我们将发送主节点的信息（该节点被标记为从节点，因此接收器知道它并不真正负责该插槽。
     * */
    master = (nodeIsSlave(myself) && myself->slaveof) ?
              myself->slaveof : myself;

    memset(hdr,0,sizeof(*hdr));
    hdr->ver = htons(CLUSTER_PROTO_VER);
    hdr->sig[0] = 'R';
    hdr->sig[1] = 'C';
    hdr->sig[2] = 'm';
    hdr->sig[3] = 'b';
    hdr->type = htons(type);
    memcpy(hdr->sender,myself->name,CLUSTER_NAMELEN);

    /* If cluster-announce-ip option is enabled, force the receivers of our
     * packets to use the specified address for this node. Otherwise if the
     * first byte is zero, they'll do auto discovery. */
    //如果启用了 cluster-announce-ip 选项，则强制我们的数据包的接收者使用此节点的指定地址。
    // 否则，如果第一个字节为零，他们将进行自动发现。
    memset(hdr->myip,0,NET_IP_STR_LEN);
    if (server.cluster_announce_ip) {
        strncpy(hdr->myip,server.cluster_announce_ip,NET_IP_STR_LEN-1);
        hdr->myip[NET_IP_STR_LEN-1] = '\0';
    }

    /* Handle cluster-announce-[tls-|bus-]port. */
    int announced_port, announced_pport, announced_cport;
    deriveAnnouncedPorts(&announced_port, &announced_pport, &announced_cport);

    memcpy(hdr->myslots,master->slots,sizeof(hdr->myslots));
    memset(hdr->slaveof,0,CLUSTER_NAMELEN);
    if (myself->slaveof != NULL)
        memcpy(hdr->slaveof,myself->slaveof->name, CLUSTER_NAMELEN);
    hdr->port = htons(announced_port);
    hdr->pport = htons(announced_pport);
    hdr->cport = htons(announced_cport);
    hdr->flags = htons(myself->flags);
    hdr->state = server.cluster->state;

    /* Set the currentEpoch and configEpochs. 设置 currentEpoch 和 configEpochs。*/
    hdr->currentEpoch = htonu64(server.cluster->currentEpoch);
    hdr->configEpoch = htonu64(master->configEpoch);

    /* Set the replication offset. 设置复制偏移量。 */
    if (nodeIsSlave(myself))
        offset = replicationGetSlaveOffset();
    else
        offset = server.master_repl_offset;
    hdr->offset = htonu64(offset);

    /* Set the message flags. */
    if (nodeIsMaster(myself) && server.cluster->mf_end)
        hdr->mflags[0] |= CLUSTERMSG_FLAG0_PAUSED;

    /* Compute the message length for certain messages. For other messages
     * this is up to the caller.
     * 计算某些消息的消息长度。对于其他消息，这取决于呼叫者。*/
    if (type == CLUSTERMSG_TYPE_FAIL) {
        totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
        totlen += sizeof(clusterMsgDataFail);
    } else if (type == CLUSTERMSG_TYPE_UPDATE) {
        totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
        totlen += sizeof(clusterMsgDataUpdate);
    }
    hdr->totlen = htonl(totlen);
    /* For PING, PONG, MEET and other variable length messages fixing the
     * totlen field is up to the caller.
     * 对于 PING、PONG、MEET 和其他可变长度消息，固定 totlen 字段由调用者决定。*/
}

/* Set the i-th entry of the gossip section in the message pointed by 'hdr'
 * to the info of the specified node 'n'. */
//将'hdr'指向的消息中gossip部分的第i个条目设置为指定节点'n'的信息。
void clusterSetGossipEntry(clusterMsg *hdr, int i, clusterNode *n) {
    clusterMsgDataGossip *gossip;
    gossip = &(hdr->data.ping.gossip[i]);
    memcpy(gossip->nodename,n->name,CLUSTER_NAMELEN);
    gossip->ping_sent = htonl(n->ping_sent/1000);
    gossip->pong_received = htonl(n->pong_received/1000);
    memcpy(gossip->ip,n->ip,sizeof(n->ip));
    gossip->port = htons(n->port);
    gossip->cport = htons(n->cport);
    gossip->flags = htons(n->flags);
    gossip->pport = htons(n->pport);
    gossip->notused1 = 0;
}

/* Send a PING or PONG packet to the specified node, making sure to add enough
 * gossip information. */
//向指定节点发送 PING 或 PONG 数据包，确保添加足够的 gossip 信息。
void clusterSendPing(clusterLink *link, int type) {
    static unsigned long long cluster_pings_sent = 0;
    cluster_pings_sent++;
    unsigned char *buf;
    clusterMsg *hdr;
    int gossipcount = 0; /* Number of gossip sections added so far. 到目前为止添加的八卦部分的数量。*/
    int wanted; /* Number of gossip sections we want to append if possible. 如果可能，我们要附加的八卦部分的数量。*/
    int estlen; /* Upper bound on estimated packet length 估计数据包长度的上限*/
    /* freshnodes is the max number of nodes we can hope to append at all:
     * nodes available minus two (ourself and the node we are sending the
     * message to). However practically there may be less valid nodes since
     * nodes in handshake state, disconnected, are not considered. */
    /**
     * freshnodes 是我们希望追加的最大节点数：可用节点减去 2（我们自己和我们向其发送消息的节点）。
     * 然而实际上可能存在较少有效节点，因为不考虑处于握手状态、断开连接的节点。
     * */
    int freshnodes = dictSize(server.cluster->nodes)-2;

    /* How many gossip sections we want to add? 1/10 of the number of nodes
     * and anyway at least 3. Why 1/10?
     *
     * If we have N masters, with N/10 entries, and we consider that in
     * node_timeout we exchange with each other node at least 4 packets
     * (we ping in the worst case in node_timeout/2 time, and we also
     * receive two pings from the host), we have a total of 8 packets
     * in the node_timeout*2 failure reports validity time. So we have
     * that, for a single PFAIL node, we can expect to receive the following
     * number of failure reports (in the specified window of time):
     *
     * PROB * GOSSIP_ENTRIES_PER_PACKET * TOTAL_PACKETS:
     *
     * PROB = probability of being featured in a single gossip entry,
     *        which is 1 / NUM_OF_NODES.
     * ENTRIES = 10.
     * TOTAL_PACKETS = 2 * 4 * NUM_OF_MASTERS.
     *
     * If we assume we have just masters (so num of nodes and num of masters
     * is the same), with 1/10 we always get over the majority, and specifically
     * 80% of the number of nodes, to account for many masters failing at the
     * same time.
     *
     * Since we have non-voting slaves that lower the probability of an entry
     * to feature our node, we set the number of entries per packet as
     * 10% of the total nodes we have. */
    /**
     * 我们要添加多少个八卦部分？ 110 个节点，至少 3 个。为什么是 110？如果我们有 N 个 master，有 N10 个条目，
     * 并且我们认为在 node_timeout 中我们与其他节点至少交换了 4 个数据包（我们在 node_timeout2 时间内 ping 最坏的情况，
     * 并且我们还收到来自主机的两个 ping），我们有node_timeout2中一共8个包失败报告有效时间。
     * 因此，对于单个 PFAIL 节点，我们可以预期会收到以下数量的故障报告（在指定的时间窗口内）：
     * PROB GOSSIP_ENTRIES_PER_PACKET TOTAL_PACKETS：
     * PROB = 在单个 gossip 条目中出现的概率，即 1 NUM_OF_NODES。
     * ENTRIES = 10。
     * TOTAL_PACKETS = 2 4 NUM_OF_MASTERS。
     * 如果我们假设我们只有主节点（因此节点数和主节点数相同），110 我们总是超过大多数，特别是节点数的 80%，
     * 以解释许多主节点同时失败.由于我们有无投票的从属节点，这会降低条目出现在我们节点上的概率，
     * 因此我们将每个数据包的条目数设置为我们拥有的节点总数的 10%。
     * */
    wanted = floor(dictSize(server.cluster->nodes)/10);
    if (wanted < 3) wanted = 3;
    if (wanted > freshnodes) wanted = freshnodes;

    /* Include all the nodes in PFAIL state, so that failure reports are
     * faster to propagate to go from PFAIL to FAIL state. */
    //包括所有处于 PFAIL 状态的节点，以便更快地传播故障报告以从 PFAIL 状态变为 FAIL 状态。
    int pfail_wanted = server.cluster->stats_pfail_nodes;

    /* Compute the maximum estlen to allocate our buffer. We'll fix the estlen
     * later according to the number of gossip sections we really were able
     * to put inside the packet. */
    //计算最大 estlen 以分配我们的缓冲区。稍后我们将根据我们真正能够放入数据包中的八卦部分的数量来修复 estlen。
    estlen = sizeof(clusterMsg) - sizeof(union clusterMsgData);
    estlen += (sizeof(clusterMsgDataGossip)*(wanted + pfail_wanted));
    estlen += getHostnamePingExtSize();

    /* Note: clusterBuildMessageHdr() expects the buffer to be always at least
     * sizeof(clusterMsg) or more. */
    //注意： clusterBuildMessageHdr() 期望缓冲区始终至少为 sizeof(clusterMsg) 或更大。
    if (estlen < (int)sizeof(clusterMsg)) estlen = sizeof(clusterMsg);
    buf = zcalloc(estlen);
    hdr = (clusterMsg*) buf;

    /* Populate the header. 填充标题。 */
    if (!link->inbound && type == CLUSTERMSG_TYPE_PING)
        link->node->ping_sent = mstime();
    clusterBuildMessageHdr(hdr,type);

    /* Populate the gossip fields  填充八卦字段*/
    int maxiterations = wanted*3;
    while(freshnodes > 0 && gossipcount < wanted && maxiterations--) {
        dictEntry *de = dictGetRandomKey(server.cluster->nodes);
        clusterNode *this = dictGetVal(de);

        /* Don't include this node: the whole packet header is about us
         * already, so we just gossip about other nodes. */
        //不要包括这个节点：整个数据包头已经是关于我们的，所以我们只是八卦其他节点。
        if (this == myself) continue;

        /* PFAIL nodes will be added later. */
        //稍后将添加 PFAIL 节点。
        if (this->flags & CLUSTER_NODE_PFAIL) continue;

        /* In the gossip section don't include:
         * 1) Nodes in HANDSHAKE state.
         * 3) Nodes with the NOADDR flag set.
         * 4) Disconnected nodes if they don't have configured slots.
         */
        /**
         * 在 gossip 部分不包括：
         *  1) 处于 HANDSHAKE 状态的节点。
         *  3) 设置了 NOADDR 标志的节点。
         *  4) 如果节点没有配置插槽，则断开连接。
         * */
        if (this->flags & (CLUSTER_NODE_HANDSHAKE|CLUSTER_NODE_NOADDR) ||
            (this->link == NULL && this->numslots == 0))
        {
            freshnodes--; /* Technically not correct, but saves CPU.技术上不正确，但可以节省 CPU。 */
            continue;
        }

        /* Do not add a node we already have. */
        //不要添加我们已有的节点。
        if (this->last_in_ping_gossip == cluster_pings_sent) continue;

        /* Add it */
        clusterSetGossipEntry(hdr,gossipcount,this);
        this->last_in_ping_gossip = cluster_pings_sent;
        freshnodes--;
        gossipcount++;
    }

    /* If there are PFAIL nodes, add them at the end. */
    //如果有 PFAIL 节点，则将它们添加到末尾。
    if (pfail_wanted) {
        dictIterator *di;
        dictEntry *de;

        di = dictGetSafeIterator(server.cluster->nodes);
        while((de = dictNext(di)) != NULL && pfail_wanted > 0) {
            clusterNode *node = dictGetVal(de);
            if (node->flags & CLUSTER_NODE_HANDSHAKE) continue;
            if (node->flags & CLUSTER_NODE_NOADDR) continue;
            if (!(node->flags & CLUSTER_NODE_PFAIL)) continue;
            clusterSetGossipEntry(hdr,gossipcount,node);
            gossipcount++;
            /* We take the count of the slots we allocated, since the
             * PFAIL stats may not match perfectly with the current number
             * of PFAIL nodes.
             * 我们计算我们分配的插槽数，因为 PFAIL 统计数据可能与当前 PFAIL 节点的数量不完全匹配。*/
            pfail_wanted--;
        }
        dictReleaseIterator(di);
    }

    
    int totlen = 0;
    int extensions = 0;
    /* Set the initial extension position 设置初始扩展位置*/
    clusterMsgPingExt *cursor = getInitialPingExt(hdr, gossipcount);
    /* Add in the extensions */
    if (sdslen(myself->hostname) != 0) {
        hdr->mflags[0] |= CLUSTERMSG_FLAG0_EXT_DATA;
        totlen += writeHostnamePingExt(&cursor);
        extensions++;
    }

    /* Compute the actual total length and send! */
    //计算实际总长度并发送！
    totlen += sizeof(clusterMsg)-sizeof(union clusterMsgData);
    totlen += (sizeof(clusterMsgDataGossip)*gossipcount);
    hdr->count = htons(gossipcount);
    hdr->extensions = htons(extensions);
    hdr->totlen = htonl(totlen);
    clusterSendMessage(link,buf,totlen);
    zfree(buf);
}

/* Send a PONG packet to every connected node that's not in handshake state
 * and for which we have a valid link.
 *
 * In Redis Cluster pongs are not used just for failure detection, but also
 * to carry important configuration information. So broadcasting a pong is
 * useful when something changes in the configuration and we want to make
 * the cluster aware ASAP (for instance after a slave promotion).
 *
 * The 'target' argument specifies the receiving instances using the
 * defines below:
 *
 * CLUSTER_BROADCAST_ALL -> All known instances.
 * CLUSTER_BROADCAST_LOCAL_SLAVES -> All slaves in my master-slaves ring.
 */
/**
 * 向每个未处于握手状态且我们拥有有效链接的连接节点发送一个 PONG 数据包。
 * 在 Redis Cluster 中，pong 不仅用于故障检测，还用于携带重要的配置信息。
 * 因此，当配置发生更改并且我们希望尽快让集群感知（例如在从属升级之后）时，广播 pong 很有用。
 * 'target' 参数使用以下定义指定接收实例：
 *   CLUSTER_BROADCAST_ALL -> 所有已知实例。
 *   CLUSTER_BROADCAST_LOCAL_SLAVES -> 我的主从环中的所有从站。
 * */
#define CLUSTER_BROADCAST_ALL 0
#define CLUSTER_BROADCAST_LOCAL_SLAVES 1
void clusterBroadcastPong(int target) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        if (!node->link) continue;
        if (node == myself || nodeInHandshake(node)) continue;
        if (target == CLUSTER_BROADCAST_LOCAL_SLAVES) {
            int local_slave =
                nodeIsSlave(node) && node->slaveof &&
                (node->slaveof == myself || node->slaveof == myself->slaveof);
            if (!local_slave) continue;
        }
        clusterSendPing(node->link,CLUSTERMSG_TYPE_PONG);
    }
    dictReleaseIterator(di);
}

/* Send a PUBLISH message.
 *
 * If link is NULL, then the message is broadcasted to the whole cluster.
 *
 * Sanitizer suppression: In clusterMsgDataPublish, sizeof(bulk_data) is 8.
 * As all the struct is used as a buffer, when more than 8 bytes are copied into
 * the 'bulk_data', sanitizer generates an out-of-bounds error which is a false
 * positive in this context. */
/**
 * 发送 PUBLISH 消息。如果 link 为 NULL，则将消息广播到整个集群。 Sanitizer 抑制：
 * 在 clusterMsgDataPublish 中，sizeof(bulk_data) 为 8。
 * 由于所有 struct 都用作缓冲区，当将超过 8 个字节复制到“bulk_data”中时，sanitizer 会生成越界错误，这是误报在这种情况下。
 * */
REDIS_NO_SANITIZE("bounds")
void clusterSendPublish(clusterLink *link, robj *channel, robj *message, uint16_t type) {
    unsigned char *payload;
    clusterMsg buf[1];
    clusterMsg *hdr = (clusterMsg*) buf;
    uint32_t totlen;
    uint32_t channel_len, message_len;

    channel = getDecodedObject(channel);
    message = getDecodedObject(message);
    channel_len = sdslen(channel->ptr);
    message_len = sdslen(message->ptr);

    clusterBuildMessageHdr(hdr,type);
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    totlen += sizeof(clusterMsgDataPublish) - 8 + channel_len + message_len;

    hdr->data.publish.msg.channel_len = htonl(channel_len);
    hdr->data.publish.msg.message_len = htonl(message_len);
    hdr->totlen = htonl(totlen);

    /* Try to use the local buffer if possible */
    //如果可能，尝试使用本地缓冲区
    if (totlen < sizeof(buf)) {
        payload = (unsigned char*)buf;
    } else {
        payload = zmalloc(totlen);
        memcpy(payload,hdr,sizeof(*hdr));
        hdr = (clusterMsg*) payload;
    }
    memcpy(hdr->data.publish.msg.bulk_data,channel->ptr,sdslen(channel->ptr));
    memcpy(hdr->data.publish.msg.bulk_data+sdslen(channel->ptr),
        message->ptr,sdslen(message->ptr));

    if (link)
        clusterSendMessage(link,payload,totlen);
    else
        clusterBroadcastMessage(payload,totlen);

    decrRefCount(channel);
    decrRefCount(message);
    if (payload != (unsigned char*)buf) zfree(payload);
}

/* Send a FAIL message to all the nodes we are able to contact.
 * The FAIL message is sent when we detect that a node is failing
 * (CLUSTER_NODE_PFAIL) and we also receive a gossip confirmation of this:
 * we switch the node state to CLUSTER_NODE_FAIL and ask all the other
 * nodes to do the same ASAP. */
/**
 * 向我们能够联系的所有节点发送 FAIL 消息。
 * 当我们检测到一个节点出现故障（CLUSTER_NODE_PFAIL）时会发送 FAIL 消息，并且我们还会收到对此的八卦确认：
 * 我们将节点状态切换为 CLUSTER_NODE_FAIL 并要求所有其他节点尽快执行相同的操作。
 * */
void clusterSendFail(char *nodename) {
    clusterMsg buf[1];
    clusterMsg *hdr = (clusterMsg*) buf;

    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_FAIL);
    memcpy(hdr->data.fail.about.nodename,nodename,CLUSTER_NAMELEN);
    clusterBroadcastMessage(buf,ntohl(hdr->totlen));
}

/* Send an UPDATE message to the specified link carrying the specified 'node'
 * slots configuration. The node name, slots bitmap, and configEpoch info
 * are included. */
//向携带指定“节点”插槽配置的指定链接发送 UPDATE 消息。包括节点名称、插槽位图和 configEpoch 信息。
void clusterSendUpdate(clusterLink *link, clusterNode *node) {
    clusterMsg buf[1];
    clusterMsg *hdr = (clusterMsg*) buf;

    if (link == NULL) return;
    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_UPDATE);
    memcpy(hdr->data.update.nodecfg.nodename,node->name,CLUSTER_NAMELEN);
    hdr->data.update.nodecfg.configEpoch = htonu64(node->configEpoch);
    memcpy(hdr->data.update.nodecfg.slots,node->slots,sizeof(node->slots));
    clusterSendMessage(link,(unsigned char*)buf,ntohl(hdr->totlen));
}

/* Send a MODULE message.
 *
 * If link is NULL, then the message is broadcasted to the whole cluster. */
//发送 MODULE 消息。如果 link 为 NULL，则将消息广播到整个集群。
void clusterSendModule(clusterLink *link, uint64_t module_id, uint8_t type,
                       const char *payload, uint32_t len) {
    unsigned char *heapbuf;
    clusterMsg buf[1];
    clusterMsg *hdr = (clusterMsg*) buf;
    uint32_t totlen;

    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_MODULE);
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    totlen += sizeof(clusterMsgModule) - 3 + len;

    hdr->data.module.msg.module_id = module_id; /* Already endian adjusted. 已经调整了字节序。*/
    hdr->data.module.msg.type = type;
    hdr->data.module.msg.len = htonl(len);
    hdr->totlen = htonl(totlen);

    /* Try to use the local buffer if possible */
    //如果可能，尝试使用本地缓冲区
    if (totlen < sizeof(buf)) {
        heapbuf = (unsigned char*)buf;
    } else {
        heapbuf = zmalloc(totlen);
        memcpy(heapbuf,hdr,sizeof(*hdr));
        hdr = (clusterMsg*) heapbuf;
    }
    memcpy(hdr->data.module.msg.bulk_data,payload,len);

    if (link)
        clusterSendMessage(link,heapbuf,totlen);
    else
        clusterBroadcastMessage(heapbuf,totlen);

    if (heapbuf != (unsigned char*)buf) zfree(heapbuf);
}

/* This function gets a cluster node ID string as target, the same way the nodes
 * addresses are represented in the modules side, resolves the node, and sends
 * the message. If the target is NULL the message is broadcasted.
 *
 * The function returns C_OK if the target is valid, otherwise C_ERR is
 * returned. */
//此函数获取集群节点 ID 字符串作为目标，与在模块端表示节点地址的方式相同，解析节点并发送消息。
// 如果目标为 NULL，则广播消息。如果目标有效，该函数返回 C_OK，否则返回 C_ERR。
int clusterSendModuleMessageToTarget(const char *target, uint64_t module_id, uint8_t type, const char *payload, uint32_t len) {
    clusterNode *node = NULL;

    if (target != NULL) {
        node = clusterLookupNode(target, strlen(target));
        if (node == NULL || node->link == NULL) return C_ERR;
    }

    clusterSendModule(target ? node->link : NULL,
                      module_id, type, payload, len);
    return C_OK;
}

/* -----------------------------------------------------------------------------
 * CLUSTER Pub/Sub support
 *
 * If `sharded` is 0:
 * For now we do very little, just propagating [S]PUBLISH messages across the whole
 * cluster. In the future we'll try to get smarter and avoiding propagating those
 * messages to hosts without receives for a given channel.
 * Otherwise:
 * Publish this message across the slot (primary/replica).
 * 如果 `sharded` 为 0：现在我们做的很少，只是在整个集群中传播 [S]PUBLISH 消息。
 * 将来，我们将尝试变得更聪明，并避免将这些消息传播给没有给定频道接收的主机。否则：跨槽（主副本）发布此消息。
 * -------------------------------------------------------------------------- */
void clusterPropagatePublish(robj *channel, robj *message, int sharded) {
    if (!sharded) {
        clusterSendPublish(NULL, channel, message, CLUSTERMSG_TYPE_PUBLISH);
        return;
    }

    list *nodes_for_slot = clusterGetNodesServingMySlots(server.cluster->myself);
    if (listLength(nodes_for_slot) != 0) {
        listIter li;
        listNode *ln;
        listRewind(nodes_for_slot, &li);
        while((ln = listNext(&li))) {
            clusterNode *node = listNodeValue(ln);
            if (node != myself) {
                clusterSendPublish(node->link, channel, message, CLUSTERMSG_TYPE_PUBLISHSHARD);
            }
        }
    }
    listRelease(nodes_for_slot);
}

/* -----------------------------------------------------------------------------
 * SLAVE node specific functions
 * -------------------------------------------------------------------------- */

/* This function sends a FAILOVER_AUTH_REQUEST message to every node in order to
 * see if there is the quorum for this slave instance to failover its failing
 * master.
 *
 * Note that we send the failover request to everybody, master and slave nodes,
 * but only the masters are supposed to reply to our query. */
/**
 * 此函数向每个节点发送一个 FAILOVER_AUTH_REQUEST 消息，以查看此从属实例是否有法定人数来故障转移其失败的主节点。
 * 请注意，我们将故障转移请求发送给每个人，主节点和从节点，但只有主节点应该回复我们的查询。
 * */
void clusterRequestFailoverAuth(void) {
    clusterMsg buf[1];
    clusterMsg *hdr = (clusterMsg*) buf;
    uint32_t totlen;

    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST);
    /* If this is a manual failover, set the CLUSTERMSG_FLAG0_FORCEACK bit
     * in the header to communicate the nodes receiving the message that
     * they should authorized the failover even if the master is working. */
    /**
     * 如果这是手动故障转移，请在标头中设置 CLUSTERMSG_FLAG0_FORCEACK 位以与接收消息的节点通信，
     * 即使主服务器正在工作，它们也应该授权故障转移。
     * */
    if (server.cluster->mf_end) hdr->mflags[0] |= CLUSTERMSG_FLAG0_FORCEACK;
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    hdr->totlen = htonl(totlen);
    clusterBroadcastMessage(buf,totlen);
}

/* Send a FAILOVER_AUTH_ACK message to the specified node. */
//向指定节点发送 FAILOVER_AUTH_ACK 消息。
void clusterSendFailoverAuth(clusterNode *node) {
    clusterMsg buf[1];
    clusterMsg *hdr = (clusterMsg*) buf;
    uint32_t totlen;

    if (!node->link) return;
    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK);
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    hdr->totlen = htonl(totlen);
    clusterSendMessage(node->link,(unsigned char*)buf,totlen);
}

/* Send a MFSTART message to the specified node. */
//向指定节点发送 MFSTART 消息。
void clusterSendMFStart(clusterNode *node) {
    clusterMsg buf[1];
    clusterMsg *hdr = (clusterMsg*) buf;
    uint32_t totlen;

    if (!node->link) return;
    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_MFSTART);
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    hdr->totlen = htonl(totlen);
    clusterSendMessage(node->link,(unsigned char*)buf,totlen);
}

/* Vote for the node asking for our vote if there are the conditions. */
//如果有条件，请投票给要求我们投票的节点。
void clusterSendFailoverAuthIfNeeded(clusterNode *node, clusterMsg *request) {
    clusterNode *master = node->slaveof;
    uint64_t requestCurrentEpoch = ntohu64(request->currentEpoch);
    uint64_t requestConfigEpoch = ntohu64(request->configEpoch);
    unsigned char *claimed_slots = request->myslots;
    int force_ack = request->mflags[0] & CLUSTERMSG_FLAG0_FORCEACK;
    int j;

    /* IF we are not a master serving at least 1 slot, we don't have the
     * right to vote, as the cluster size in Redis Cluster is the number
     * of masters serving at least one slot, and quorum is the cluster
     * size + 1 */
    /**
     * 如果我们不是至少服务 1 个 slot 的 master，我们没有投票权，
     * 因为 Redis Cluster 中的集群大小是服务至少一个 slot 的 master 数量，quorum 是集群大小 + 1
     * */
    if (nodeIsSlave(myself) || myself->numslots == 0) return;

    /* Request epoch must be >= our currentEpoch.
     * Note that it is impossible for it to actually be greater since
     * our currentEpoch was updated as a side effect of receiving this
     * request, if the request epoch was greater. */
    /**
     * 请求纪元必须 >= 我们的当前纪元。请注意，它实际上不可能更大，因为如果请求 epoch 更大，
     * 我们的 currentEpoch 会作为接收此请求的副作用而更新。
     * */
    if (requestCurrentEpoch < server.cluster->currentEpoch) {
        serverLog(LL_WARNING,
            "Failover auth denied to %.40s: reqEpoch (%llu) < curEpoch(%llu)",
            node->name,
            (unsigned long long) requestCurrentEpoch,
            (unsigned long long) server.cluster->currentEpoch);
        return;
    }

    /* I already voted for this epoch? Return ASAP. */
    //我已经为这个时代投票了？尽快返回。
    if (server.cluster->lastVoteEpoch == server.cluster->currentEpoch) {
        serverLog(LL_WARNING,
                "Failover auth denied to %.40s: already voted for epoch %llu",
                node->name,
                (unsigned long long) server.cluster->currentEpoch);
        return;
    }

    /* Node must be a slave and its master down.
     * The master can be non failing if the request is flagged
     * with CLUSTERMSG_FLAG0_FORCEACK (manual failover). */
    //节点必须是从属节点，并且它的主节点已关闭。如果请求被标记为 CLUSTERMSG_FLAG0_FORCEACK（手动故障转移），
    // 则主节点可以是非失败的。
    if (nodeIsMaster(node) || master == NULL ||
        (!nodeFailed(master) && !force_ack))
    {
        if (nodeIsMaster(node)) {
            serverLog(LL_WARNING,
                    "Failover auth denied to %.40s: it is a master node",
                    node->name);
        } else if (master == NULL) {
            serverLog(LL_WARNING,
                    "Failover auth denied to %.40s: I don't know its master",
                    node->name);
        } else if (!nodeFailed(master)) {
            serverLog(LL_WARNING,
                    "Failover auth denied to %.40s: its master is up",
                    node->name);
        }
        return;
    }

    /* We did not voted for a slave about this master for two
     * times the node timeout. This is not strictly needed for correctness
     * of the algorithm but makes the base case more linear. */
    //在节点超时的两倍内，我们没有为这个主节点投票给从节点。这并不是算法正确性所必需的，但会使基本情况更加线性。
    if (mstime() - node->slaveof->voted_time < server.cluster_node_timeout * 2)
    {
        serverLog(LL_WARNING,
                "Failover auth denied to %.40s: "
                "can't vote about this master before %lld milliseconds",
                node->name,
                (long long) ((server.cluster_node_timeout*2)-
                             (mstime() - node->slaveof->voted_time)));
        return;
    }

    /* The slave requesting the vote must have a configEpoch for the claimed
     * slots that is >= the one of the masters currently serving the same
     * slots in the current configuration. */
    //请求投票的从站必须具有声明的插槽的 configEpoch，该插槽 >= 当前服务于当前配置中相同插槽的主机之一。
    for (j = 0; j < CLUSTER_SLOTS; j++) {
        if (bitmapTestBit(claimed_slots, j) == 0) continue;
        if (server.cluster->slots[j] == NULL ||
            server.cluster->slots[j]->configEpoch <= requestConfigEpoch)
        {
            continue;
        }
        /* If we reached this point we found a slot that in our current slots
         * is served by a master with a greater configEpoch than the one claimed
         * by the slave requesting our vote. Refuse to vote for this slave. */
        /**
         * 如果我们达到这一点，我们会发现在我们当前的槽中的一个槽由一个master提供服务，
         * 该主站的 configEpoch 比请求我们投票的从站声称的要大。拒绝投票给这个slave。
         * */
        serverLog(LL_WARNING,
                "Failover auth denied to %.40s: "
                "slot %d epoch (%llu) > reqEpoch (%llu)",
                node->name, j,
                (unsigned long long) server.cluster->slots[j]->configEpoch,
                (unsigned long long) requestConfigEpoch);
        return;
    }

    /* We can vote for this slave. */
    server.cluster->lastVoteEpoch = server.cluster->currentEpoch;
    node->slaveof->voted_time = mstime();
    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|CLUSTER_TODO_FSYNC_CONFIG);
    clusterSendFailoverAuth(node);
    serverLog(LL_WARNING, "Failover auth granted to %.40s for epoch %llu",
        node->name, (unsigned long long) server.cluster->currentEpoch);
}

/* This function returns the "rank" of this instance, a slave, in the context
 * of its master-slaves ring. The rank of the slave is given by the number of
 * other slaves for the same master that have a better replication offset
 * compared to the local one (better means, greater, so they claim more data).
 *
 * A slave with rank 0 is the one with the greatest (most up to date)
 * replication offset, and so forth. Note that because how the rank is computed
 * multiple slaves may have the same rank, in case they have the same offset.
 *
 * The slave rank is used to add a delay to start an election in order to
 * get voted and replace a failing master. Slaves with better replication
 * offsets are more likely to win. */
/**
 * 这个函数返回这个实例的“等级”，一个从属，在它的主从环的上下文中。
 * 从站的等级由同一主站的其他从站数量给出，这些从站与本地副本相比具有更好的复制偏移量（更好的手段，更大，
 * 因此他们要求更多的数据）。等级为 0 的从站是具有最大（最新）复制偏移量的从站，依此类推。
 * 请注意，由于如何计算排名，多个从属服务器可能具有相同的排名，以防它们具有相同的偏移量。
 * slave rank 用于添加延迟以开始选举，以便获得投票并替换失败的 master。具有更好复制偏移量的从站更有可能获胜。
 * */
int clusterGetSlaveRank(void) {
    long long myoffset;
    int j, rank = 0;
    clusterNode *master;

    serverAssert(nodeIsSlave(myself));
    master = myself->slaveof;
    if (master == NULL) return 0; /* Never called by slaves without master. 从来没有被没有master的slave召唤过。*/

    myoffset = replicationGetSlaveOffset();
    for (j = 0; j < master->numslaves; j++)
        if (master->slaves[j] != myself &&
            !nodeCantFailover(master->slaves[j]) &&
            master->slaves[j]->repl_offset > myoffset) rank++;
    return rank;
}

/* This function is called by clusterHandleSlaveFailover() in order to
 * let the slave log why it is not able to failover. Sometimes there are
 * not the conditions, but since the failover function is called again and
 * again, we can't log the same things continuously.
 *
 * This function works by logging only if a given set of conditions are
 * true:
 *
 * 1) The reason for which the failover can't be initiated changed.
 *    The reasons also include a NONE reason we reset the state to
 *    when the slave finds that its master is fine (no FAIL flag).
 * 2) Also, the log is emitted again if the master is still down and
 *    the reason for not failing over is still the same, but more than
 *    CLUSTER_CANT_FAILOVER_RELOG_PERIOD seconds elapsed.
 * 3) Finally, the function only logs if the slave is down for more than
 *    five seconds + NODE_TIMEOUT. This way nothing is logged when a
 *    failover starts in a reasonable time.
 *
 * The function is called with the reason why the slave can't failover
 * which is one of the integer macros CLUSTER_CANT_FAILOVER_*.
 *
 * The function is guaranteed to be called only if 'myself' is a slave. */
/**
 * 这个函数由 clusterHandleSlaveFailover() 调用，以便让从属记录为什么它不能进行故障转移。
 * 有时没有条件，但是由于一次又一次地调用故障转移功能，我们无法连续记录相同的事情。
 * 此功能仅在给定的一组条件为真时通过记录起作用：
 *   1) 无法启动故障转移的原因已更改。原因还包括一个 NONE 原因，
 *      我们将状态重置为从属设备发现其主设备正常（没有 FAIL 标志）。
 *   2) 此外，如果主服务器仍然关闭并且未故障转移的原因仍然相同，但超过 CLUSTER_CANT_FAILOVER_RELOG_PERIOD 秒，
 *      则会再次发出日志。
 *   3) 最后，该函数仅在从机宕机时间超过 5 秒 + NODE_TIMEOUT 时才会记录。
 *      这样，当故障转移在合理的时间内开始时，不会记录任何内容。
 * 调用该函数的原因是从站无法故障转移，这是整数宏 CLUSTER_CANT_FAILOVER_ 之一。
 * 只有当"myself"是slave时，才能保证调用该函数。
 * */
void clusterLogCantFailover(int reason) {
    char *msg;
    static time_t lastlog_time = 0;
    mstime_t nolog_fail_time = server.cluster_node_timeout + 5000;

    /* Don't log if we have the same reason for some time. */
    //如果一段时间以来我们有相同的原因，请不要登录。
    if (reason == server.cluster->cant_failover_reason &&
        time(NULL)-lastlog_time < CLUSTER_CANT_FAILOVER_RELOG_PERIOD)
        return;

    server.cluster->cant_failover_reason = reason;

    /* We also don't emit any log if the master failed no long ago, the
     * goal of this function is to log slaves in a stalled condition for
     * a long time.
     * 如果 master 不久前发生故障，我们也不会发出任何日志，此功能的目标是记录 slave 长时间处于停滞状态。*/
    if (myself->slaveof &&
        nodeFailed(myself->slaveof) &&
        (mstime() - myself->slaveof->fail_time) < nolog_fail_time) return;

    switch(reason) {
    case CLUSTER_CANT_FAILOVER_DATA_AGE:
        msg = "Disconnected from master for longer than allowed. "
              "Please check the 'cluster-replica-validity-factor' configuration "
              "option.";
        break;
    case CLUSTER_CANT_FAILOVER_WAITING_DELAY:
        msg = "Waiting the delay before I can start a new failover.";
        break;
    case CLUSTER_CANT_FAILOVER_EXPIRED:
        msg = "Failover attempt expired.";
        break;
    case CLUSTER_CANT_FAILOVER_WAITING_VOTES:
        msg = "Waiting for votes, but majority still not reached.";
        break;
    default:
        msg = "Unknown reason code.";
        break;
    }
    lastlog_time = time(NULL);
    serverLog(LL_WARNING,"Currently unable to failover: %s", msg);
}

/* This function implements the final part of automatic and manual failovers,
 * where the slave grabs its master's hash slots, and propagates the new
 * configuration.
 *
 * Note that it's up to the caller to be sure that the node got a new
 * configuration epoch already. */
/**
 * 该函数实现了自动和手动故障转移的最后一部分，从服务器获取其主服务器的哈希槽，并传播新配置。
 * 请注意，由调用者确保节点已经获得了新的配置时期。
 * */
void clusterFailoverReplaceYourMaster(void) {
    int j;
    clusterNode *oldmaster = myself->slaveof;

    if (nodeIsMaster(myself) || oldmaster == NULL) return;

    /* 1) Turn this node into a master.把这个节点变成master。*/
    clusterSetNodeAsMaster(myself);
    replicationUnsetMaster();

    /* 2) Claim all the slots assigned to our master.索取分配给我们的msater的所有插槽 */
    for (j = 0; j < CLUSTER_SLOTS; j++) {
        if (clusterNodeGetSlotBit(oldmaster,j)) {
            clusterDelSlot(j);
            clusterAddSlot(myself,j);
        }
    }

    /* 3) Update state and save config. 更新状态并保存配置。*/
    clusterUpdateState();
    clusterSaveConfigOrDie(1);

    /* 4) Pong all the other nodes so that they can update the state
     *    accordingly and detect that we switched to master role.
     *    Pong 所有其他节点，以便他们可以相应地更新状态并检测我们切换到主角色。*/
    clusterBroadcastPong(CLUSTER_BROADCAST_ALL);

    /* 5) If there was a manual failover in progress, clear the state.
     *    如果正在进行手动故障转移，请清除状态。*/
    resetManualFailover();
}

/* This function is called if we are a slave node and our master serving
 * a non-zero amount of hash slots is in FAIL state.
 *
 * The goal of this function is:
 * 1) To check if we are able to perform a failover, is our data updated?
 * 2) Try to get elected by masters.
 * 3) Perform the failover informing all the other nodes.
 */
/**
 * 如果我们是从节点并且我们的主节点服务非零数量的哈希槽处于 FAIL 状态，则调用此函数。
 * 这个函数的目标是：
 *   1）检查我们是否能够执行故障转移，我们的数据是否更新了？
 *   2）尝试被主人选举。
 *   3) 执行故障转移通知所有其他节点。
 * */
void clusterHandleSlaveFailover(void) {
    mstime_t data_age;
    mstime_t auth_age = mstime() - server.cluster->failover_auth_time;
    int needed_quorum = (server.cluster->size / 2) + 1;
    int manual_failover = server.cluster->mf_end != 0 &&
                          server.cluster->mf_can_start;
    mstime_t auth_timeout, auth_retry_time;

    server.cluster->todo_before_sleep &= ~CLUSTER_TODO_HANDLE_FAILOVER;

    /* Compute the failover timeout (the max time we have to send votes
     * and wait for replies), and the failover retry time (the time to wait
     * before trying to get voted again).
     *
     * Timeout is MAX(NODE_TIMEOUT*2,2000) milliseconds.
     * Retry is two times the Timeout.
     */
    /**
     * 计算故障转移超时（我们必须发送投票和等待回复的最长时间）和故障转移重试时间（在尝试再次投票之前等待的时间）。
     * 超时为 MAX(NODE_TIMEOUT2,2000) 毫秒。重试是超时的两倍。
     * */
    auth_timeout = server.cluster_node_timeout*2;
    if (auth_timeout < 2000) auth_timeout = 2000;
    auth_retry_time = auth_timeout*2;

    /* Pre conditions to run the function, that must be met both in case
     * of an automatic or manual failover:
     * 1) We are a slave.
     * 2) Our master is flagged as FAIL, or this is a manual failover.
     * 3) We don't have the no failover configuration set, and this is
     *    not a manual failover.
     * 4) It is serving slots. */
    /**
     * 运行该功能的先决条件，在自动或手动故障转移的情况下都必须满足：
     *   1）我们是slave。
     *   2) 我们的 master 被标记为 FAIL，或者这是手动故障转移。
     *   3) 我们没有设置无故障转移配置，这不是手动故障转移。
     *   4）它正在服务插槽。
     * */
    if (nodeIsMaster(myself) ||
        myself->slaveof == NULL ||
        (!nodeFailed(myself->slaveof) && !manual_failover) ||
        (server.cluster_slave_no_failover && !manual_failover) ||
        myself->slaveof->numslots == 0)
    {
        /* There are no reasons to failover, so we set the reason why we
         * are returning without failing over to NONE. */
        //没有故障转移的原因，因此我们将不进行故障转移而返回的原因设置为 NONE。
        server.cluster->cant_failover_reason = CLUSTER_CANT_FAILOVER_NONE;
        return;
    }

    /* Set data_age to the number of milliseconds we are disconnected from
     * the master. */
    //将 data_age 设置为我们与主服务器断开连接的毫秒数。
    if (server.repl_state == REPL_STATE_CONNECTED) {
        data_age = (mstime_t)(server.unixtime - server.master->lastinteraction)
                   * 1000;
    } else {
        data_age = (mstime_t)(server.unixtime - server.repl_down_since) * 1000;
    }

    /* Remove the node timeout from the data age as it is fine that we are
     * disconnected from our master at least for the time it was down to be
     * flagged as FAIL, that's the baseline. */
    /**
     * 从数据时代中删除节点超时，因为至少在它被标记为 FAIL 时我们与我们的主服务器断开连接是可以的，这是基线。
     * */
    if (data_age > server.cluster_node_timeout)
        data_age -= server.cluster_node_timeout;

    /* Check if our data is recent enough according to the slave validity
     * factor configured by the user.
     *
     * Check bypassed for manual failovers. */
    //根据用户配置的从站有效性因子检查我们的数据是否足够新。检查绕过手动故障转移。
    if (server.cluster_slave_validity_factor &&
        data_age >
        (((mstime_t)server.repl_ping_slave_period * 1000) +
         (server.cluster_node_timeout * server.cluster_slave_validity_factor)))
    {
        if (!manual_failover) {
            clusterLogCantFailover(CLUSTER_CANT_FAILOVER_DATA_AGE);
            return;
        }
    }

    /* If the previous failover attempt timeout and the retry time has
     * elapsed, we can setup a new one. */
    //如果之前的故障转移尝试超时并且重试时间已经过去，我们可以设置一个新的。
    if (auth_age > auth_retry_time) {
        server.cluster->failover_auth_time = mstime() +
            500 + /* Fixed delay of 500 milliseconds, let FAIL msg propagate. 固定延迟 500 毫秒，让 FAIL msg 传播。*/
            random() % 500; /* Random delay between 0 and 500 milliseconds. 0 到 500 毫秒之间的随机延迟。*/
        server.cluster->failover_auth_count = 0;
        server.cluster->failover_auth_sent = 0;
        server.cluster->failover_auth_rank = clusterGetSlaveRank();
        /* We add another delay that is proportional to the slave rank.
         * Specifically 1 second * rank. This way slaves that have a probably
         * less updated replication offset, are penalized. */
        /**
         * 我们添加了另一个与从属等级成比例的延迟。特别是1个二等奖。
         * 这样，可能具有较少更新的复制偏移量的从属服务器会受到惩罚。
         * */
        server.cluster->failover_auth_time +=
            server.cluster->failover_auth_rank * 1000;
        /* However if this is a manual failover, no delay is needed. */
        //但是，如果这是手动故障转移，则不需要延迟。
        if (server.cluster->mf_end) {
            server.cluster->failover_auth_time = mstime();
            server.cluster->failover_auth_rank = 0;
	    clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_FAILOVER);
        }
        serverLog(LL_WARNING,
            "Start of election delayed for %lld milliseconds "
            "(rank #%d, offset %lld).",
            server.cluster->failover_auth_time - mstime(),
            server.cluster->failover_auth_rank,
            replicationGetSlaveOffset());
        /* Now that we have a scheduled election, broadcast our offset
         * to all the other slaves so that they'll updated their offsets
         * if our offset is better. */
        /**
         * 现在我们有一个预定的选举，将我们的偏移量广播给所有其他从站，这样如果我们的偏移量更好，他们就会更新他们的偏移量。
         * */
        clusterBroadcastPong(CLUSTER_BROADCAST_LOCAL_SLAVES);
        return;
    }

    /* It is possible that we received more updated offsets from other
     * slaves for the same master since we computed our election delay.
     * Update the delay if our rank changed.
     *
     * Not performed if this is a manual failover. */
    /**
     * 由于我们计算了选举延迟，因此我们可能从同一主服务器的其他从服务器收到了更多更新的偏移量。
     * 如果我们的排名发生变化，请更新延迟。如果这是手动故障转移，则不执行。
     * */
    if (server.cluster->failover_auth_sent == 0 &&
        server.cluster->mf_end == 0)
    {
        int newrank = clusterGetSlaveRank();
        if (newrank > server.cluster->failover_auth_rank) {
            long long added_delay =
                (newrank - server.cluster->failover_auth_rank) * 1000;
            server.cluster->failover_auth_time += added_delay;
            server.cluster->failover_auth_rank = newrank;
            serverLog(LL_WARNING,
                "Replica rank updated to #%d, added %lld milliseconds of delay.",
                newrank, added_delay);
        }
    }

    /* Return ASAP if we can't still start the election. */
    //如果我们仍然无法开始选举，请尽快返回。
    if (mstime() < server.cluster->failover_auth_time) {
        clusterLogCantFailover(CLUSTER_CANT_FAILOVER_WAITING_DELAY);
        return;
    }

    /* Return ASAP if the election is too old to be valid. */
    //如果选举太旧而无效，请尽快返回。
    if (auth_age > auth_timeout) {
        clusterLogCantFailover(CLUSTER_CANT_FAILOVER_EXPIRED);
        return;
    }

    /* Ask for votes if needed. 如果需要，请投票。*/
    if (server.cluster->failover_auth_sent == 0) {
        server.cluster->currentEpoch++;
        server.cluster->failover_auth_epoch = server.cluster->currentEpoch;
        serverLog(LL_WARNING,"Starting a failover election for epoch %llu.",
            (unsigned long long) server.cluster->currentEpoch);
        clusterRequestFailoverAuth();
        server.cluster->failover_auth_sent = 1;
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                             CLUSTER_TODO_UPDATE_STATE|
                             CLUSTER_TODO_FSYNC_CONFIG);
        return; /* Wait for replies. 等待回复。*/
    }

    /* Check if we reached the quorum. 检查我们是否达到了法定人数。*/
    if (server.cluster->failover_auth_count >= needed_quorum) {
        /* We have the quorum, we can finally failover the master.
         * 我们有法定人数，我们终于可以故障转移主服务器了。*/

        serverLog(LL_WARNING,
            "Failover election won: I'm the new master.");

        /* Update my configEpoch to the epoch of the election. */
        //将我的 configEpoch 更新为选举的时代。
        if (myself->configEpoch < server.cluster->failover_auth_epoch) {
            myself->configEpoch = server.cluster->failover_auth_epoch;
            serverLog(LL_WARNING,
                "configEpoch set to %llu after successful failover",
                (unsigned long long) myself->configEpoch);
        }

        /* Take responsibility for the cluster slots. 负责集群槽。*/
        clusterFailoverReplaceYourMaster();
    } else {
        clusterLogCantFailover(CLUSTER_CANT_FAILOVER_WAITING_VOTES);
    }
}

/* -----------------------------------------------------------------------------
 * CLUSTER slave migration
 *
 * Slave migration is the process that allows a slave of a master that is
 * already covered by at least another slave, to "migrate" to a master that
 * is orphaned, that is, left with no working slaves.
 * ------------------------------------------------------------------------- */

/* This function is responsible to decide if this replica should be migrated
 * to a different (orphaned) master. It is called by the clusterCron() function
 * only if:
 *
 * 1) We are a slave node.
 * 2) It was detected that there is at least one orphaned master in
 *    the cluster.
 * 3) We are a slave of one of the masters with the greatest number of
 *    slaves.
 *
 * This checks are performed by the caller since it requires to iterate
 * the nodes anyway, so we spend time into clusterHandleSlaveMigration()
 * if definitely needed.
 *
 * The function is called with a pre-computed max_slaves, that is the max
 * number of working (not in FAIL state) slaves for a single master.
 *
 * Additional conditions for migration are examined inside the function.
 */
/**
 * -------------------------------------------------- ---------------------------
 * CLUSTER 从属迁移
 *
 * 从属迁移是允许一个主控的一个从属的过程，该主控已经被至少另一个覆盖奴隶，“迁移”到一个孤立的主人，也就是说，没有工作的奴隶。
 * -------------------------------------------------- -----------------------
 * 这个函数负责决定是否应该将这个副本迁移到不同的（孤立的）master。
 * 只有在以下情况下才由 clusterCron() 函数调用它：
 *   1) 我们是从节点。
 *   2) 检测到集群中至少有一个孤立的master。
 *   3）我们是拥有最多奴隶的主人之一的奴隶。此检查由调用者执行，因为它无论如何都需要迭代节点，
 * 因此如果确实需要，我们会花时间进入 clusterHandleSlaveMigration()。
 * 该函数使用预先计算的 max_slaves 调用，即单个主设备的最大工作（非 FAIL 状态）从属设备数量。
 * 在函数内部检查迁移的附加条件。
 * */
void clusterHandleSlaveMigration(int max_slaves) {
    int j, okslaves = 0;
    clusterNode *mymaster = myself->slaveof, *target = NULL, *candidate = NULL;
    dictIterator *di;
    dictEntry *de;

    /* Step 1: Don't migrate if the cluster state is not ok. */
    //第 1 步：如果集群状态不正常，请不要迁移。
    if (server.cluster->state != CLUSTER_OK) return;

    /* Step 2: Don't migrate if my master will not be left with at least
     *         'migration-barrier' slaves after my migration. */
    //第 2 步：如果我的主人在我迁移后不会留下至少“迁移障碍”奴隶，请不要迁移。
    if (mymaster == NULL) return;
    for (j = 0; j < mymaster->numslaves; j++)
        if (!nodeFailed(mymaster->slaves[j]) &&
            !nodeTimedOut(mymaster->slaves[j])) okslaves++;
    if (okslaves <= server.cluster_migration_barrier) return;

    /* Step 3: Identify a candidate for migration, and check if among the
     * masters with the greatest number of ok slaves, I'm the one with the
     * smallest node ID (the "candidate slave").
     *
     * Note: this means that eventually a replica migration will occur
     * since slaves that are reachable again always have their FAIL flag
     * cleared, so eventually there must be a candidate.
     * There is a possible race condition causing multiple
     * slaves to migrate at the same time, but this is unlikely to
     * happen and relatively harmless when it does. */
    /**
     * 第 3 步：确定迁移的候选者，并检查是否在拥有最多 ok slave 的 master 中，我是节点 ID 最小的那个（“候选 slave”）。
     * 注意：这意味着最终将发生副本迁移，因为再次可访问的从属服务器总是清除其 FAIL 标志，因此最终必须有一个候选者。
     * 可能存在导致多个从属服务器同时迁移的竞争条件，但这不太可能发生并且当它发生时相对无害。
     * */
    candidate = myself;
    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        int okslaves = 0, is_orphaned = 1;

        /* We want to migrate only if this master is working, orphaned, and
         * used to have slaves or if failed over a master that had slaves
         * (MIGRATE_TO flag). This way we only migrate to instances that were
         * supposed to have replicas. */
        /**
         * 仅当此 master 正在工作、孤立并且曾经拥有 slave 或者如果故障转移具有 slave 的 master（MIGRATE_TO 标志）时，
         * 我们才希望迁移。这样我们只迁移到应该有副本的实例。
         * */
        if (nodeIsSlave(node) || nodeFailed(node)) is_orphaned = 0;
        if (!(node->flags & CLUSTER_NODE_MIGRATE_TO)) is_orphaned = 0;

        /* Check number of working slaves. 检查工作奴隶的数量。*/
        if (nodeIsMaster(node)) okslaves = clusterCountNonFailingSlaves(node);
        if (okslaves > 0) is_orphaned = 0;

        if (is_orphaned) {
            if (!target && node->numslots > 0) target = node;

            /* Track the starting time of the orphaned condition for this
             * master. 跟踪此 master 孤立条件的开始时间。*/
            if (!node->orphaned_time) node->orphaned_time = mstime();
        } else {
            node->orphaned_time = 0;
        }

        /* Check if I'm the slave candidate for the migration: attached
         * to a master with the maximum number of slaves and with the smallest
         * node ID. 检查我是否是迁移的从属候选：连接到具有最大从属数量和最小节点 ID 的主控。*/
        if (okslaves == max_slaves) {
            for (j = 0; j < node->numslaves; j++) {
                if (memcmp(node->slaves[j]->name,
                           candidate->name,
                           CLUSTER_NAMELEN) < 0)
                {
                    candidate = node->slaves[j];
                }
            }
        }
    }
    dictReleaseIterator(di);

    /* Step 4: perform the migration if there is a target, and if I'm the
     * candidate, but only if the master is continuously orphaned for a
     * couple of seconds, so that during failovers, we give some time to
     * the natural slaves of this instance to advertise their switch from
     * the old master to the new one.
     * 第 4 步：如果有目标，并且如果我是候选者，则执行迁移，但前提是 master 连续孤立几秒钟，以便在故障转移期间，
     * 我们给这个实例的自然奴隶一些时间宣传他们从旧主人转向新主人的转变。*/
    if (target && candidate == myself &&
        (mstime()-target->orphaned_time) > CLUSTER_SLAVE_MIGRATION_DELAY &&
       !(server.cluster_module_flags & CLUSTER_MODULE_FLAG_NO_FAILOVER))
    {
        serverLog(LL_WARNING,"Migrating to orphaned master %.40s",
            target->name);
        clusterSetMaster(target);
    }
}

/* -----------------------------------------------------------------------------
 * CLUSTER manual failover
 *
 * This are the important steps performed by slaves during a manual failover:
 * 1) User send CLUSTER FAILOVER command. The failover state is initialized
 *    setting mf_end to the millisecond unix time at which we'll abort the
 *    attempt.
 * 2) Slave sends a MFSTART message to the master requesting to pause clients
 *    for two times the manual failover timeout CLUSTER_MF_TIMEOUT.
 *    When master is paused for manual failover, it also starts to flag
 *    packets with CLUSTERMSG_FLAG0_PAUSED.
 * 3) Slave waits for master to send its replication offset flagged as PAUSED.
 * 4) If slave received the offset from the master, and its offset matches,
 *    mf_can_start is set to 1, and clusterHandleSlaveFailover() will perform
 *    the failover as usually, with the difference that the vote request
 *    will be modified to force masters to vote for a slave that has a
 *    working master.
 *
 * From the point of view of the master things are simpler: when a
 * PAUSE_CLIENTS packet is received the master sets mf_end as well and
 * the sender in mf_slave. During the time limit for the manual failover
 * the master will just send PINGs more often to this slave, flagged with
 * the PAUSED flag, so that the slave will set mf_master_offset when receiving
 * a packet from the master with this flag set.
 *
 * The goal of the manual failover is to perform a fast failover without
 * data loss due to the asynchronous master-slave replication.
 *
 * CLUSTER 手动故障转移
 *
 * 这是从服务器在手动故障转移期间执行的重要步骤：
 *   1) 用户发送 CLUSTER FAILOVER 命令。故障转移状态已初始化，将 mf_end 设置为我们将中止尝试的毫秒 unix 时间。
 *   2) 从站向主站发送一条 MFSTART 消息，请求在手动故障转移超时 CLUSTER_MF_TIMEOUT 的两倍内暂停客户端。
 *      当 master 暂停手动故障转移时，它也开始使用 CLUSTERMSG_FLAG0_PAUSED 标记数据包。
 *   3) Slave 等待 master 发送它的复制偏移量，标记为 PAUSED。
 *   4) 如果slave收到了master的offset，并且它的offset匹配，mf_can_start设置为1，
 *      clusterHandleSlaveFailover()会像往常一样执行failover，不同的是投票请求会被修改为强制master投票给
 *      一个有一个工作主人的奴隶。
 * 从主设备的角度来看，事情更简单：当收到 PAUSE_CLIENTS 数据包时，
 * 主设备也设置 mf_end，发送方设置为 mf_slave。在手动故障转移的时间限制期间，
 * 主服务器将更频繁地向该从服务器发送 PING，并使用 PAUSED 标志进行标记，
 *  以便从服务器在接收到设置了此标志的主服务器的数据包时设置 mf_master_offset。
 *  手动故障转移的目标是在不因异步主从复制而丢失数据的情况下执行快速故障转移。
 * -------------------------------------------------------------------------- */

/* Reset the manual failover state. This works for both masters and slaves
 * as all the state about manual failover is cleared.
 *
 * The function can be used both to initialize the manual failover state at
 * startup or to abort a manual failover in progress. */
/**
 * 重置手动故障转移状态。这适用于主服务器和从服务器，因为有关手动故障转移的所有状态都已清除。
 * 该函数既可用于在启动时初始化手动故障转移状态，也可用于中止正在进行的手动故障转移。
 * */
void resetManualFailover(void) {
    if (server.cluster->mf_slave) {
        /* We were a master failing over, so we paused clients. Regardless
         * of the outcome we unpause now to allow traffic again. */
        //我们是故障转移的大师，所以我们暂停了客户端。无论结果如何，我们现在都会取消暂停以再次允许流量。
        unpauseClients(PAUSE_DURING_FAILOVER);
    }
    server.cluster->mf_end = 0; /* No manual failover in progress. 没有正在进行的手动故障转移。*/
    server.cluster->mf_can_start = 0;
    server.cluster->mf_slave = NULL;
    server.cluster->mf_master_offset = -1;
}

/* If a manual failover timed out, abort it. 如果手动故障转移超时，请中止它。*/
void manualFailoverCheckTimeout(void) {
    if (server.cluster->mf_end && server.cluster->mf_end < mstime()) {
        serverLog(LL_WARNING,"Manual failover timed out.");
        resetManualFailover();
    }
}

/* This function is called from the cluster cron function in order to go
 * forward with a manual failover state machine. */
//如果手动故障转移超时，请中止它。
void clusterHandleManualFailover(void) {
    /* Return ASAP if no manual failover is in progress. */
    //如果没有进行手动故障转移，请尽快返回。
    if (server.cluster->mf_end == 0) return;

    /* If mf_can_start is non-zero, the failover was already triggered so the
     * next steps are performed by clusterHandleSlaveFailover(). */
    //如果 mf_can_start 不为零，则故障转移已被触发，因此后续步骤由 clusterHandleSlaveFailover() 执行。
    if (server.cluster->mf_can_start) return;

    if (server.cluster->mf_master_offset == -1) return; /* Wait for offset... */

    if (server.cluster->mf_master_offset == replicationGetSlaveOffset()) {
        /* Our replication offset matches the master replication offset
         * announced after clients were paused. We can start the failover. */
        //我们的复制偏移量与客户端暂停后宣布的主复制偏移量相匹配。我们可以启动故障转移。
        server.cluster->mf_can_start = 1;
        serverLog(LL_WARNING,
            "All master replication stream processed, "
            "manual failover can start.");
        clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_FAILOVER);
        return;
    }
    clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_MANUALFAILOVER);
}

/* -----------------------------------------------------------------------------
 * CLUSTER cron job 集群 cron 作业
 * -------------------------------------------------------------------------- */

/* Check if the node is disconnected and re-establish the connection.
 * Also update a few stats while we are here, that can be used to make
 * better decisions in other part of the code.
 * 检查节点是否断开并重新建立连接。当我们在这里时，还要更新一些统计数据，这可用于在代码的其他部分做出更好的决策。*/
static int clusterNodeCronHandleReconnect(clusterNode *node, mstime_t handshake_timeout, mstime_t now) {
    /* Not interested in reconnecting the link with myself or nodes
     * for which we have no address. */
    //对重新连接与我自己或我们没有地址的节点的链接不感兴趣。
    if (node->flags & (CLUSTER_NODE_MYSELF|CLUSTER_NODE_NOADDR)) return 1;

    if (node->flags & CLUSTER_NODE_PFAIL)
        server.cluster->stats_pfail_nodes++;

    /* A Node in HANDSHAKE state has a limited lifespan equal to the
     * configured node timeout. */
    //处于 HANDSHAKE 状态的节点的生命周期有限，等于配置的节点超时。
    if (nodeInHandshake(node) && now - node->ctime > handshake_timeout) {
        clusterDelNode(node);
        return 1;
    }

    if (node->link == NULL) {
        clusterLink *link = createClusterLink(node);
        link->conn = server.tls_cluster ? connCreateTLS() : connCreateSocket();
        connSetPrivateData(link->conn, link);
        if (connConnect(link->conn, node->ip, node->cport, server.bind_source_addr,
                    clusterLinkConnectHandler) == -1) {
            /* We got a synchronous error from connect before
             * clusterSendPing() had a chance to be called.
             * If node->ping_sent is zero, failure detection can't work,
             * so we claim we actually sent a ping now (that will
             * be really sent as soon as the link is obtained). */
            /**
             * 在 clusterSendPing() 有机会被调用之前，我们从 connect 收到了一个同步错误。
             * 如果 node->ping_sent 为零，则无法进行故障检测，因此我们声称我们现在实际上发送了
             * 一个 ping（一旦获得链接就会真正发送）。
             * */
            if (node->ping_sent == 0) node->ping_sent = mstime();
            serverLog(LL_DEBUG, "Unable to connect to "
                "Cluster Node [%s]:%d -> %s", node->ip,
                node->cport, server.neterr);

            freeClusterLink(link);
            return 0;
        }
    }
    return 0;
}

static void resizeClusterLinkBuffer(clusterLink *link) {
     /* If unused space is a lot bigger than the used portion of the buffer then free up unused space.
      * We use a factor of 4 because of the greediness of sdsMakeRoomFor (used by sdscatlen). */
     //如果未使用的空间比缓冲区的已用部分大很多，则释放未使用的空间。
     // 由于 sdsMakeRoomFor（由 sdscatlen 使用）的贪婪性，我们使用了 4 因子。
    if (link != NULL && sdsavail(link->sndbuf) / 4 > sdslen(link->sndbuf)) {
        link->sndbuf = sdsRemoveFreeSpace(link->sndbuf);
    }
}

/* Resize the send buffer of a node if it is wasting
 * enough space. */
//如果节点浪费了足够的空间，请调整其发送缓冲区的大小。
static void clusterNodeCronResizeBuffers(clusterNode *node) {
    resizeClusterLinkBuffer(node->link);
    resizeClusterLinkBuffer(node->inbound_link);
}

static void freeClusterLinkOnBufferLimitReached(clusterLink *link) {
    if (link == NULL || server.cluster_link_sendbuf_limit_bytes == 0) {
        return;
    }
    unsigned long long mem_link = sdsalloc(link->sndbuf);
    if (mem_link > server.cluster_link_sendbuf_limit_bytes) {
        serverLog(LL_WARNING, "Freeing cluster link(%s node %.40s, used memory: %llu) due to "
                "exceeding send buffer memory limit.", link->inbound ? "from" : "to",
                link->node ? link->node->name : "", mem_link);
        freeClusterLink(link);
        server.cluster->stat_cluster_links_buffer_limit_exceeded++;
    }
}

/* Free outbound link to a node if its send buffer size exceeded limit. */
//如果节点的发送缓冲区大小超过限制，则释放到节点的出站链接。
static void clusterNodeCronFreeLinkOnBufferLimitReached(clusterNode *node) {
    freeClusterLinkOnBufferLimitReached(node->link);
    freeClusterLinkOnBufferLimitReached(node->inbound_link);
}

static size_t getClusterLinkMemUsage(clusterLink *link) {
    if (link != NULL) {
        return sizeof(clusterLink) + sdsalloc(link->sndbuf) + link->rcvbuf_alloc;
    } else {
        return 0;
    }
}

/* Update memory usage statistics of all current cluster links */
//更新所有当前集群链接的内存使用统计
static void clusterNodeCronUpdateClusterLinksMemUsage(clusterNode *node) {
    server.stat_cluster_links_memory += getClusterLinkMemUsage(node->link);
    server.stat_cluster_links_memory += getClusterLinkMemUsage(node->inbound_link);
}

/* This is executed 10 times every second */
//每秒执行 10 次
void clusterCron(void) {
    dictIterator *di;
    dictEntry *de;
    int update_state = 0;
    int orphaned_masters; /* How many masters there are without ok slaves. 有多少master没有好的slave。*/
    int max_slaves; /* Max number of ok slaves for a single master. 单个 master 的最大 ok slave 数。*/
    int this_slaves; /* Number of ok slaves for our master (if we are slave). 我们的master的正常slave的数量（如果我们是slave）。*/
    mstime_t min_pong = 0, now = mstime();
    clusterNode *min_pong_node = NULL;
    static unsigned long long iteration = 0;
    mstime_t handshake_timeout;

    iteration++; /* Number of times this function was called so far. 到目前为止调用此函数的次数。*/

    clusterUpdateMyselfHostname();

    /* The handshake timeout is the time after which a handshake node that was
     * not turned into a normal node is removed from the nodes. Usually it is
     * just the NODE_TIMEOUT value, but when NODE_TIMEOUT is too small we use
     * the value of 1 second. */
    /**
     * 握手超时是没有变成正常节点的握手节点从节点中移除的时间。
     * 通常它只是 NODE_TIMEOUT 的值，但是当 NODE_TIMEOUT 太小时我们使用 1 秒的值。
     * */
    handshake_timeout = server.cluster_node_timeout;
    if (handshake_timeout < 1000) handshake_timeout = 1000;

    /* Clear so clusterNodeCronHandleReconnect can count the number of nodes in PFAIL. */
    //清除以便 clusterNodeCronHandleReconnect 可以计算 PFAIL 中的节点数。
    server.cluster->stats_pfail_nodes = 0;
    /* Clear so clusterNodeCronUpdateClusterLinksMemUsage can count the current memory usage of all cluster links. */
    //清除以便 clusterNodeCronUpdateClusterLinksMemUsage 可以统计所有集群链接的当前内存使用情况。
    server.stat_cluster_links_memory = 0;
    /* Run through some of the operations we want to do on each cluster node. */
    //运行我们想要在每个集群节点上执行的一些操作。
    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        /* The sequence goes:
         * 1. We try to shrink link buffers if possible.
         * 2. We free the links whose buffers are still oversized after possible shrinking.
         * 3. We update the latest memory usage of cluster links.
         * 4. We immediately attempt reconnecting after freeing links.
         */
        /**
         * 顺序如下：
         *   1. 如果可能，我们会尝试缩小链接缓冲区。
         *   2. 我们释放缓冲区在可能收缩后仍然过大的链接。
         *   3.我们更新了集群链接的最新内存使用情况。
         *   4.我们在释放链接后立即尝试重新连接。
         * */
        clusterNodeCronResizeBuffers(node);
        clusterNodeCronFreeLinkOnBufferLimitReached(node);
        clusterNodeCronUpdateClusterLinksMemUsage(node);
        /* The protocol is that function(s) below return non-zero if the node was
         * terminated.
         */
        //协议是如果节点被终止，下面的函数返回非零。
        if(clusterNodeCronHandleReconnect(node, handshake_timeout, now)) continue;
    }
    dictReleaseIterator(di); 

    /* Ping some random node 1 time every 10 iterations, so that we usually ping
     * one random node every second. */
    //每 10 次迭代 ping 某个随机节点 1 次，所以我们通常每秒 ping 一个随机节点。
    if (!(iteration % 10)) {
        int j;

        /* Check a few random nodes and ping the one with the oldest
         * pong_received time. */
        //检查几个随机节点并 ping 具有最早 pong_received 时间的节点。
        for (j = 0; j < 5; j++) {
            de = dictGetRandomKey(server.cluster->nodes);
            clusterNode *this = dictGetVal(de);

            /* Don't ping nodes disconnected or with a ping currently active. */
            //不要 ping 已断开连接或 ping 当前处于活动状态的节点。
            if (this->link == NULL || this->ping_sent != 0) continue;
            if (this->flags & (CLUSTER_NODE_MYSELF|CLUSTER_NODE_HANDSHAKE))
                continue;
            if (min_pong_node == NULL || min_pong > this->pong_received) {
                min_pong_node = this;
                min_pong = this->pong_received;
            }
        }
        if (min_pong_node) {
            serverLog(LL_DEBUG,"Pinging node %.40s", min_pong_node->name);
            clusterSendPing(min_pong_node->link, CLUSTERMSG_TYPE_PING);
        }
    }

    /* Iterate nodes to check if we need to flag something as failing.
     * This loop is also responsible to:
     * 1) Check if there are orphaned masters (masters without non failing
     *    slaves).
     * 2) Count the max number of non failing slaves for a single master.
     * 3) Count the number of slaves for our master, if we are a slave. */
    /**
     * 迭代节点以检查我们是否需要将某些内容标记为失败。该循环还负责：
     *   1) 检查是否有孤立的主控（没有非故障从属的主控）。
     *   2）计算单个主设备的最大非故障从设备数量。
     *   3）如果我们是奴隶，请为我们的主人计算奴隶的数量。
     * */
    orphaned_masters = 0;
    max_slaves = 0;
    this_slaves = 0;
    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        now = mstime(); /* Use an updated time at every iteration. 在每次迭代中使用更新的时间。*/

        if (node->flags &
            (CLUSTER_NODE_MYSELF|CLUSTER_NODE_NOADDR|CLUSTER_NODE_HANDSHAKE))
                continue;

        /* Orphaned master check, useful only if the current instance
         * is a slave that may migrate to another master. */
        //孤立主检查，仅当当前实例是可能迁移到另一个主实例的从属时才有用。
        if (nodeIsSlave(myself) && nodeIsMaster(node) && !nodeFailed(node)) {
            int okslaves = clusterCountNonFailingSlaves(node);

            /* A master is orphaned if it is serving a non-zero number of
             * slots, have no working slaves, but used to have at least one
             * slave, or failed over a master that used to have slaves. */
            /**
             * 如果一个主服务器服务于非零数量的槽，没有工作的从服务器，但曾经有至少一个从服务器，
             * 或者故障转移过去有从服务器的主服务器，则它是孤立的。
             * */
            if (okslaves == 0 && node->numslots > 0 &&
                node->flags & CLUSTER_NODE_MIGRATE_TO)
            {
                orphaned_masters++;
            }
            if (okslaves > max_slaves) max_slaves = okslaves;
            if (myself->slaveof == node)
                this_slaves = okslaves;
        }

        /* If we are not receiving any data for more than half the cluster
         * timeout, reconnect the link: maybe there is a connection
         * issue even if the node is alive. */
        //如果集群超时一半以上没有收到任何数据，请重新连接链接：即使节点处于活动状态，也可能存在连接问题。
        mstime_t ping_delay = now - node->ping_sent;
        mstime_t data_delay = now - node->data_received;
        if (node->link && /* is connected */
            now - node->link->ctime >
            server.cluster_node_timeout && /* was not already reconnected 尚未重新连接*/
            node->ping_sent && /* we already sent a ping 我们已经发送了一个 ping*/
            /* and we are waiting for the pong more than timeout/2 */
            //我们等待乒乓球的时间超过了 timeout2
            ping_delay > server.cluster_node_timeout/2 &&
            /* and in such interval we are not seeing any traffic at all. */
            //在这样的时间间隔内，我们根本看不到任何流量。
            data_delay > server.cluster_node_timeout/2)
        {
            /* Disconnect the link, it will be reconnected automatically. */
            //断开链接，它会自动重新连接。
            freeClusterLink(node->link);
        }

        /* If we have currently no active ping in this instance, and the
         * received PONG is older than half the cluster timeout, send
         * a new ping now, to ensure all the nodes are pinged without
         * a too big delay. */
        /**
         * 如果我们目前在这个实例中没有活动的 ping，并且收到的 PONG 超过集群超时的一半，
         * 现在发送一个新的 ping，以确保所有节点都被 ping 通，没有太大的延迟。
         * */
        if (node->link &&
            node->ping_sent == 0 &&
            (now - node->pong_received) > server.cluster_node_timeout/2)
        {
            clusterSendPing(node->link, CLUSTERMSG_TYPE_PING);
            continue;
        }

        /* If we are a master and one of the slaves requested a manual
         * failover, ping it continuously. */
        //如果我们是主服务器并且其中一个从服务器请求手动故障转移，请连续 ping 它。
        if (server.cluster->mf_end &&
            nodeIsMaster(myself) &&
            server.cluster->mf_slave == node &&
            node->link)
        {
            clusterSendPing(node->link, CLUSTERMSG_TYPE_PING);
            continue;
        }

        /* Check only if we have an active ping for this instance. */
        //仅检查我们是否有此实例的活动 ping。
        if (node->ping_sent == 0) continue;

        /* Check if this node looks unreachable.
         * Note that if we already received the PONG, then node->ping_sent
         * is zero, so can't reach this code at all, so we don't risk of
         * checking for a PONG delay if we didn't sent the PING.
         *
         * We also consider every incoming data as proof of liveness, since
         * our cluster bus link is also used for data: under heavy data
         * load pong delays are possible. */
        /**
         * 检查此节点是否看起来无法访问。请注意，如果我们已经收到 PONG，则 node->ping_sent 为零，
         * 因此根本无法访问此代码，因此如果我们没有发送 PING，我们不会冒险检查 PONG 延迟。
         * 我们还将每个传入的数据视为活跃度的证明，因为我们的集群总线链路也用于数据：在大量数据负载下，pong 延迟是可能的。
         * */
        mstime_t node_delay = (ping_delay < data_delay) ? ping_delay :
                                                          data_delay;

        if (node_delay > server.cluster_node_timeout) {
            /* Timeout reached. Set the node as possibly failing if it is
             * not already in this state. */
            //超时。如果节点尚未处于此状态，则将其设置为可能失败。
            if (!(node->flags & (CLUSTER_NODE_PFAIL|CLUSTER_NODE_FAIL))) {
                serverLog(LL_DEBUG,"*** NODE %.40s possibly failing",
                    node->name);
                node->flags |= CLUSTER_NODE_PFAIL;
                update_state = 1;
            }
        }
    }
    dictReleaseIterator(di);

    /* If we are a slave node but the replication is still turned off,
     * enable it if we know the address of our master and it appears to
     * be up. */
    //如果我们是从节点但复制仍然关闭，如果我们知道主节点的地址并且它似乎已启动，则启用它。
    if (nodeIsSlave(myself) &&
        server.masterhost == NULL &&
        myself->slaveof &&
        nodeHasAddr(myself->slaveof))
    {
        replicationSetMaster(myself->slaveof->ip, myself->slaveof->port);
    }

    /* Abort a manual failover if the timeout is reached. */
    //如果达到超时，则中止手动故障转移。
    manualFailoverCheckTimeout();

    if (nodeIsSlave(myself)) {
        clusterHandleManualFailover();
        if (!(server.cluster_module_flags & CLUSTER_MODULE_FLAG_NO_FAILOVER))
            clusterHandleSlaveFailover();
        /* If there are orphaned slaves, and we are a slave among the masters
         * with the max number of non-failing slaves, consider migrating to
         * the orphaned masters. Note that it does not make sense to try
         * a migration if there is no master with at least *two* working
         * slaves. */
        /**
         * 如果有孤立的 slave，并且我们是 master 中最大数量的 non-failing slave，请考虑迁移到孤立的 master。
         * 请注意，如果没有具有至少两个工作从属的主服务器，则尝试迁移是没有意义的。
         * */
        if (orphaned_masters && max_slaves >= 2 && this_slaves == max_slaves &&
		server.cluster_allow_replica_migration)
            clusterHandleSlaveMigration(max_slaves);
    }

    if (update_state || server.cluster->state == CLUSTER_FAIL)
        clusterUpdateState();
}

/* This function is called before the event handler returns to sleep for
 * events. It is useful to perform operations that must be done ASAP in
 * reaction to events fired but that are not safe to perform inside event
 * handlers, or to perform potentially expansive tasks that we need to do
 * a single time before replying to clients. */
/**
 * 在事件处理程序返回到事件睡眠之前调用此函数。
 * 执行必须尽快完成以响应触发的事件但在事件处理程序内部执行不安全的操作是有用的，
 * 或者执行我们需要在回复客户端之前执行一次的潜在扩展任务。
 * */
void clusterBeforeSleep(void) {
    int flags = server.cluster->todo_before_sleep;

    /* Reset our flags (not strictly needed since every single function
     * called for flags set should be able to clear its flag). */
    //重置我们的标志（不是严格需要，因为调用标志集的每个函数都应该能够清除它的标志）。
    server.cluster->todo_before_sleep = 0;

    if (flags & CLUSTER_TODO_HANDLE_MANUALFAILOVER) {
        /* Handle manual failover as soon as possible so that won't have a 100ms
         * as it was handled only in clusterCron */
        //尽快处理手动故障转移，这样不会有 100 毫秒，因为它仅在 clusterCron 中处理
        if(nodeIsSlave(myself)) {
            clusterHandleManualFailover();
            if (!(server.cluster_module_flags & CLUSTER_MODULE_FLAG_NO_FAILOVER))
                clusterHandleSlaveFailover();
        }
    } else if (flags & CLUSTER_TODO_HANDLE_FAILOVER) {
        /* Handle failover, this is needed when it is likely that there is already
         * the quorum from masters in order to react fast. */
        //处理故障转移，当可能已经有来自 master 的仲裁以快速做出反应时，这是需要的。
        clusterHandleSlaveFailover();
    }

    /* Update the cluster state. */
    if (flags & CLUSTER_TODO_UPDATE_STATE)
        clusterUpdateState();

    /* Save the config, possibly using fsync. 保存配置，可能使用 fsync。*/
    if (flags & CLUSTER_TODO_SAVE_CONFIG) {
        int fsync = flags & CLUSTER_TODO_FSYNC_CONFIG;
        clusterSaveConfigOrDie(fsync);
    }
}

void clusterDoBeforeSleep(int flags) {
    server.cluster->todo_before_sleep |= flags;
}

/* -----------------------------------------------------------------------------
 * Slots management
 * -------------------------------------------------------------------------- */

/* Test bit 'pos' in a generic bitmap. Return 1 if the bit is set,
 * otherwise 0. */
//在通用位图中测试位“pos”。如果该位已设置，则返回 1，否则返回 0。
int bitmapTestBit(unsigned char *bitmap, int pos) {
    off_t byte = pos/8;
    int bit = pos&7;
    return (bitmap[byte] & (1<<bit)) != 0;
}

/* Set the bit at position 'pos' in a bitmap. */
//在位图中的位置 'pos' 处设置位。
void bitmapSetBit(unsigned char *bitmap, int pos) {
    off_t byte = pos/8;
    int bit = pos&7;
    bitmap[byte] |= 1<<bit;
}

/* Clear the bit at position 'pos' in a bitmap. */
//清除位图中位置 'pos' 的位。
void bitmapClearBit(unsigned char *bitmap, int pos) {
    off_t byte = pos/8;
    int bit = pos&7;
    bitmap[byte] &= ~(1<<bit);
}

/* Return non-zero if there is at least one master with slaves in the cluster.
 * Otherwise zero is returned. Used by clusterNodeSetSlotBit() to set the
 * MIGRATE_TO flag the when a master gets the first slot. */
/**
 * 如果集群中至少有一个具有从属的主服务器，则返回非零值。否则返回零。
 * 由 clusterNodeSetSlotBit() 使用来设置 MIGRATE_TO 标志，当主设备获得第一个插槽时。
 * */
int clusterMastersHaveSlaves(void) {
    dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
    dictEntry *de;
    int slaves = 0;
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        if (nodeIsSlave(node)) continue;
        slaves += node->numslaves;
    }
    dictReleaseIterator(di);
    return slaves != 0;
}

/* Set the slot bit and return the old value. */
//设置槽位并返回旧值。
int clusterNodeSetSlotBit(clusterNode *n, int slot) {
    int old = bitmapTestBit(n->slots,slot);
    bitmapSetBit(n->slots,slot);
    if (!old) {
        n->numslots++;
        /* When a master gets its first slot, even if it has no slaves,
         * it gets flagged with MIGRATE_TO, that is, the master is a valid
         * target for replicas migration, if and only if at least one of
         * the other masters has slaves right now.
         *
         * Normally masters are valid targets of replica migration if:
         * 1. The used to have slaves (but no longer have).
         * 2. They are slaves failing over a master that used to have slaves.
         *
         * However new masters with slots assigned are considered valid
         * migration targets if the rest of the cluster is not a slave-less.
         *
         * 当 master 获得第一个 slot 时，即使它没有 slave，它也会被标记为 MIGRATE_TO，
         * 也就是说，master 是副本迁移的有效目标，当且仅当至少一个其他 master 现在有 slave。
         * 通常，如果存在以下情况，主服务器是副本迁移的有效目标：
         *   1. 曾经有从服务器（但不再有）。
         *   2. 他们是从曾经有奴隶的主人失败的奴隶。
         * 但是，如果集群的其余部分不是无从节点，则分配有插槽的新主节点被认为是有效的迁移目标。
         *
         * See https://github.com/redis/redis/issues/3043 for more info. */
        if (n->numslots == 1 && clusterMastersHaveSlaves())
            n->flags |= CLUSTER_NODE_MIGRATE_TO;
    }
    return old;
}

/* Clear the slot bit and return the old value. */
//清除槽位并返回旧值。
int clusterNodeClearSlotBit(clusterNode *n, int slot) {
    int old = bitmapTestBit(n->slots,slot);
    bitmapClearBit(n->slots,slot);
    if (old) n->numslots--;
    return old;
}

/* Return the slot bit from the cluster node structure. */
//从集群节点结构返回槽位。
int clusterNodeGetSlotBit(clusterNode *n, int slot) {
    return bitmapTestBit(n->slots,slot);
}

/* Add the specified slot to the list of slots that node 'n' will
 * serve. Return C_OK if the operation ended with success.
 * If the slot is already assigned to another instance this is considered
 * an error and C_ERR is returned. */
/**
 * 将指定的槽添加到节点“n”将服务的槽列表中。如果操作以成功结束，则返回 C_OK。
 * 如果插槽已分配给另一个实例，则认为这是一个错误并返回 C_ERR。
 * */
int clusterAddSlot(clusterNode *n, int slot) {
    if (server.cluster->slots[slot]) return C_ERR;
    clusterNodeSetSlotBit(n,slot);
    server.cluster->slots[slot] = n;
    return C_OK;
}

/* Delete the specified slot marking it as unassigned.
 * Returns C_OK if the slot was assigned, otherwise if the slot was
 * already unassigned C_ERR is returned. */
//删除将其标记为未分配的指定插槽。如果插槽已分配，则返回 C_OK，否则如果插槽已未分配，则返回 C_ERR。
int clusterDelSlot(int slot) {
    clusterNode *n = server.cluster->slots[slot];

    if (!n) return C_ERR;

    /* Cleanup the channels in master/replica as part of slot deletion. */
    //作为槽删除的一部分，清理 masterreplica 中的通道。
    list *nodes_for_slot = clusterGetNodesServingMySlots(n);
    listNode *ln = listSearchKey(nodes_for_slot, myself);
    if (ln != NULL) {
        removeChannelsInSlot(slot);
    }
    listRelease(nodes_for_slot);
    serverAssert(clusterNodeClearSlotBit(n,slot) == 1);
    server.cluster->slots[slot] = NULL;
    return C_OK;
}

/* Delete all the slots associated with the specified node.
 * The number of deleted slots is returned. */
//删除与指定节点关联的所有槽。返回已删除的槽数。
int clusterDelNodeSlots(clusterNode *node) {
    int deleted = 0, j;

    for (j = 0; j < CLUSTER_SLOTS; j++) {
        if (clusterNodeGetSlotBit(node,j)) {
            clusterDelSlot(j);
            deleted++;
        }
    }
    return deleted;
}

/* Clear the migrating / importing state for all the slots.
 * This is useful at initialization and when turning a master into slave. */
//清除所有槽的迁移导入状态。这在初始化和将主机变为从机时很有用。
void clusterCloseAllSlots(void) {
    memset(server.cluster->migrating_slots_to,0,
        sizeof(server.cluster->migrating_slots_to));
    memset(server.cluster->importing_slots_from,0,
        sizeof(server.cluster->importing_slots_from));
}

/* -----------------------------------------------------------------------------
 * Cluster state evaluation function 集群状态评估函数
 * -------------------------------------------------------------------------- */

/* The following are defines that are only used in the evaluation function
 * and are based on heuristics. Actually the main point about the rejoin and
 * writable delay is that they should be a few orders of magnitude larger
 * than the network latency. */
/**
 * 以下是仅在评估函数中使用并基于启发式的定义。实际上，关于重新加入和可写延迟的要点是它们应该比网络延迟大几个数量级。
 * */
#define CLUSTER_MAX_REJOIN_DELAY 5000
#define CLUSTER_MIN_REJOIN_DELAY 500
#define CLUSTER_WRITABLE_DELAY 2000

void clusterUpdateState(void) {
    int j, new_state;
    int reachable_masters = 0;
    static mstime_t among_minority_time;
    static mstime_t first_call_time = 0;

    server.cluster->todo_before_sleep &= ~CLUSTER_TODO_UPDATE_STATE;

    /* If this is a master node, wait some time before turning the state
     * into OK, since it is not a good idea to rejoin the cluster as a writable
     * master, after a reboot, without giving the cluster a chance to
     * reconfigure this node. Note that the delay is calculated starting from
     * the first call to this function and not since the server start, in order
     * to not count the DB loading time. */
    /**
     * 如果这是一个主节点，请等待一段时间，然后再将状态变为 OK，因为在重新启动后，
     * 作为可写主节点重新加入集群并不是一个好主意，而不给集群重新配置该节点的机会。
     * 请注意，延迟是从第一次调用此函数开始计算的，而不是从服务器启动开始计算，以便不计算数据库加载时间。
     * */
    if (first_call_time == 0) first_call_time = mstime();
    if (nodeIsMaster(myself) &&
        server.cluster->state == CLUSTER_FAIL &&
        mstime() - first_call_time < CLUSTER_WRITABLE_DELAY) return;

    /* Start assuming the state is OK. We'll turn it into FAIL if there
     * are the right conditions. */
    //开始假设状态是好的。如果条件合适，我们会将其变为 FAIL。
    new_state = CLUSTER_OK;

    /* Check if all the slots are covered. */
    //检查是否所有插槽都被覆盖。
    if (server.cluster_require_full_coverage) {
        for (j = 0; j < CLUSTER_SLOTS; j++) {
            if (server.cluster->slots[j] == NULL ||
                server.cluster->slots[j]->flags & (CLUSTER_NODE_FAIL))
            {
                new_state = CLUSTER_FAIL;
                break;
            }
        }
    }

    /* Compute the cluster size, that is the number of master nodes
     * serving at least a single slot.
     *
     * At the same time count the number of reachable masters having
     * at least one slot. */
    //计算集群大小，即服务至少一个插槽的主节点的数量。同时计算具有至少一个插槽的可达主设备的数量。
    {
        dictIterator *di;
        dictEntry *de;

        server.cluster->size = 0;
        di = dictGetSafeIterator(server.cluster->nodes);
        while((de = dictNext(di)) != NULL) {
            clusterNode *node = dictGetVal(de);

            if (nodeIsMaster(node) && node->numslots) {
                server.cluster->size++;
                if ((node->flags & (CLUSTER_NODE_FAIL|CLUSTER_NODE_PFAIL)) == 0)
                    reachable_masters++;
            }
        }
        dictReleaseIterator(di);
    }

    /* If we are in a minority partition, change the cluster state
     * to FAIL. */
    //如果我们处于少数分区中，请将集群状态更改为 FAIL。
    {
        int needed_quorum = (server.cluster->size / 2) + 1;

        if (reachable_masters < needed_quorum) {
            new_state = CLUSTER_FAIL;
            among_minority_time = mstime();
        }
    }

    /* Log a state change */
    if (new_state != server.cluster->state) {
        mstime_t rejoin_delay = server.cluster_node_timeout;

        /* If the instance is a master and was partitioned away with the
         * minority, don't let it accept queries for some time after the
         * partition heals, to make sure there is enough time to receive
         * a configuration update. */
        /**
         * 如果实例是主实例并且与少数实例分区，则在分区愈合后的一段时间内不要让它接受查询，以确保有足够的时间接收配置更新。
         * */
        if (rejoin_delay > CLUSTER_MAX_REJOIN_DELAY)
            rejoin_delay = CLUSTER_MAX_REJOIN_DELAY;
        if (rejoin_delay < CLUSTER_MIN_REJOIN_DELAY)
            rejoin_delay = CLUSTER_MIN_REJOIN_DELAY;

        if (new_state == CLUSTER_OK &&
            nodeIsMaster(myself) &&
            mstime() - among_minority_time < rejoin_delay)
        {
            return;
        }

        /* Change the state and log the event. 更改状态并记录事件。*/
        serverLog(LL_WARNING,"Cluster state changed: %s",
            new_state == CLUSTER_OK ? "ok" : "fail");
        server.cluster->state = new_state;
    }
}

/* This function is called after the node startup in order to verify that data
 * loaded from disk is in agreement with the cluster configuration:
 *
 * 1) If we find keys about hash slots we have no responsibility for, the
 *    following happens:
 *    A) If no other node is in charge according to the current cluster
 *       configuration, we add these slots to our node.
 *    B) If according to our config other nodes are already in charge for
 *       this slots, we set the slots as IMPORTING from our point of view
 *       in order to justify we have those slots, and in order to make
 *       redis-cli aware of the issue, so that it can try to fix it.
 * 2) If we find data in a DB different than DB0 we return C_ERR to
 *    signal the caller it should quit the server with an error message
 *    or take other actions.
 *
 * The function always returns C_OK even if it will try to correct
 * the error described in "1". However if data is found in DB different
 * from DB0, C_ERR is returned.
 *
 * The function also uses the logging facility in order to warn the user
 * about desynchronizations between the data we have in memory and the
 * cluster configuration. */
/**
 * 该函数在节点启动后调用，以验证从磁盘加载的数据是否与集群配置一致：
 *   1）如果我们找到与我们无关的哈希槽的键，则会发生以下情况：
 *     A）如果没有其他节点根据当前集群配置负责，我们将这些插槽添加到我们的节点。
 *     B）如果根据我们的配置，其他节点已经负责这个插槽，我们将插槽设置为 IMPORTING 从我们的角度来看，
 *     以证明我们拥有这些插槽是合理的，并且为了让 redis-cli 意识到这个问题，以便它可以尝试修复它。
 *   2) 如果我们在不同于 DB0 的 DB 中发现数据，我们返回 C_ERR 以通知调用者它应该退出服务器并显示错误消息或采取其他措施。
 * 该函数总是返回 C_OK，即使它会尝试纠正“1”中描述的错误。但是，如果在不同于 DB0 的 DB 中找到数据，则返回 C_ERR。
 * 该函数还使用日志记录工具来警告用户我们在内存中的数据与集群配置之间的不同步。
 * */
int verifyClusterConfigWithData(void) {
    int j;
    int update_config = 0;

    /* Return ASAP if a module disabled cluster redirections. In that case
     * every master can store keys about every possible hash slot. */
    //如果模块禁用了集群重定向，请尽快返回。在这种情况下，每个 master 都可以存储有关每个可能的哈希槽的密钥。
    if (server.cluster_module_flags & CLUSTER_MODULE_FLAG_NO_REDIRECTION)
        return C_OK;

    /* If this node is a slave, don't perform the check at all as we
     * completely depend on the replication stream. */
    //如果此节点是从节点，则根本不执行检查，因为我们完全依赖于复制流。
    if (nodeIsSlave(myself)) return C_OK;

    /* Make sure we only have keys in DB0. */
    //确保我们只有 DB0 中的密钥。
    for (j = 1; j < server.dbnum; j++) {
        if (dictSize(server.db[j].dict)) return C_ERR;
    }

    /* Check that all the slots we see populated memory have a corresponding
     * entry in the cluster table. Otherwise fix the table. */
    //检查我们看到填充内存的所有插槽是否在集群表中都有相应的条目。否则修复表。
    for (j = 0; j < CLUSTER_SLOTS; j++) {
        if (!countKeysInSlot(j)) continue; /* No keys in this slot. */
        /* Check if we are assigned to this slot or if we are importing it.
         * In both cases check the next slot as the configuration makes
         * sense. */
        //检查我们是否被分配到这个插槽或者我们是否正在导入它。在这两种情况下，检查下一个插槽，因为配置是有意义的。
        if (server.cluster->slots[j] == myself ||
            server.cluster->importing_slots_from[j] != NULL) continue;

        /* If we are here data and cluster config don't agree, and we have
         * slot 'j' populated even if we are not importing it, nor we are
         * assigned to this slot. Fix this condition.
         * 如果我们在这里数据和集群配置不同意，并且即使我们没有导入它，我们也填充了插槽“j”，
         * 我们也没有被分配到这个插槽。修复这种情况。*/

        update_config++;
        /* Case A: slot is unassigned. Take responsibility for it. */
        //情况 A：插槽未分配。对它负责。
        if (server.cluster->slots[j] == NULL) {
            serverLog(LL_WARNING, "I have keys for unassigned slot %d. "
                                    "Taking responsibility for it.",j);
            clusterAddSlot(myself,j);
        } else {
            serverLog(LL_WARNING, "I have keys for slot %d, but the slot is "
                                    "assigned to another node. "
                                    "Setting it to importing state.",j);
            server.cluster->importing_slots_from[j] = server.cluster->slots[j];
        }
    }
    if (update_config) clusterSaveConfigOrDie(1);
    return C_OK;
}

/* -----------------------------------------------------------------------------
 * SLAVE nodes handling
 * -------------------------------------------------------------------------- */

/* Set the specified node 'n' as master for this node.
 * If this node is currently a master, it is turned into a slave. */
//将指定的节点“n”设置为该节点的主节点。如果此节点当前是主节点，则将其变为从节点。
void clusterSetMaster(clusterNode *n) {
    serverAssert(n != myself);
    serverAssert(myself->numslots == 0);

    if (nodeIsMaster(myself)) {
        myself->flags &= ~(CLUSTER_NODE_MASTER|CLUSTER_NODE_MIGRATE_TO);
        myself->flags |= CLUSTER_NODE_SLAVE;
        clusterCloseAllSlots();
    } else {
        if (myself->slaveof)
            clusterNodeRemoveSlave(myself->slaveof,myself);
    }
    myself->slaveof = n;
    clusterNodeAddSlave(n,myself);
    replicationSetMaster(n->ip, n->port);
    resetManualFailover();
}

/* -----------------------------------------------------------------------------
 * Nodes to string representation functions. 字符串表示函数的节点。
 * -------------------------------------------------------------------------- */

struct redisNodeFlags {
    uint16_t flag;
    char *name;
};

static struct redisNodeFlags redisNodeFlagsTable[] = {
    {CLUSTER_NODE_MYSELF,       "myself,"},
    {CLUSTER_NODE_MASTER,       "master,"},
    {CLUSTER_NODE_SLAVE,        "slave,"},
    {CLUSTER_NODE_PFAIL,        "fail?,"},
    {CLUSTER_NODE_FAIL,         "fail,"},
    {CLUSTER_NODE_HANDSHAKE,    "handshake,"},
    {CLUSTER_NODE_NOADDR,       "noaddr,"},
    {CLUSTER_NODE_NOFAILOVER,   "nofailover,"}
};

/* Concatenate the comma separated list of node flags to the given SDS
 * string 'ci'. */
//将节点标志的逗号分隔列表连接到给定的 SDS 字符串“ci”。
sds representClusterNodeFlags(sds ci, uint16_t flags) {
    size_t orig_len = sdslen(ci);
    int i, size = sizeof(redisNodeFlagsTable)/sizeof(struct redisNodeFlags);
    for (i = 0; i < size; i++) {
        struct redisNodeFlags *nodeflag = redisNodeFlagsTable + i;
        if (flags & nodeflag->flag) ci = sdscat(ci, nodeflag->name);
    }
    /* If no flag was added, add the "noflags" special flag. */
    //如果没有添加标志，请添加“nflags”特殊标志。
    if (sdslen(ci) == orig_len) ci = sdscat(ci,"noflags,");
    sdsIncrLen(ci,-1); /* Remove trailing comma. 删除尾随逗号。*/
    return ci;
}

/* Concatenate the slot ownership information to the given SDS string 'ci'.
 * If the slot ownership is in a contiguous block, it's represented as start-end pair,
 * else each slot is added separately. */
/**
 * 将槽所有权信息连接到给定的 SDS 字符串“ci”。如果槽的所有权在一个连续的块中，它表示为起始对，否则每个槽是单独添加的。
 * */
sds representSlotInfo(sds ci, uint16_t *slot_info_pairs, int slot_info_pairs_count) {
    for (int i = 0; i< slot_info_pairs_count; i+=2) {
        unsigned long start = slot_info_pairs[i];
        unsigned long end = slot_info_pairs[i+1];
        if (start == end) {
            ci = sdscatfmt(ci, " %i", start);
        } else {
            ci = sdscatfmt(ci, " %i-%i", start, end);
        }
    }
    return ci;
}

/* Generate a csv-alike representation of the specified cluster node.
 * See clusterGenNodesDescription() top comment for more information.
 *
 * The function returns the string representation as an SDS string. */
/**
 * 生成指定集群节点的类似 csv 的表示。有关更多信息，请参阅 clusterGenNodesDescription() 顶部评论。
 * 该函数将字符串表示形式返回为 SDS 字符串。
 * */
sds clusterGenNodeDescription(clusterNode *node, int use_pport) {
    int j, start;
    sds ci;
    int port = use_pport && node->pport ? node->pport : node->port;

    /* Node coordinates 节点坐标*/
    ci = sdscatlen(sdsempty(),node->name,CLUSTER_NAMELEN);
    if (sdslen(node->hostname) != 0) {
        ci = sdscatfmt(ci," %s:%i@%i,%s ",
            node->ip,
            port,
            node->cport,
            node->hostname);
    } else {
        ci = sdscatfmt(ci," %s:%i@%i ",
            node->ip,
            port,
            node->cport);
    }

    /* Flags */
    ci = representClusterNodeFlags(ci, node->flags);

    /* Slave of... or just "-" */
    ci = sdscatlen(ci," ",1);
    if (node->slaveof)
        ci = sdscatlen(ci,node->slaveof->name,CLUSTER_NAMELEN);
    else
        ci = sdscatlen(ci,"-",1);

    unsigned long long nodeEpoch = node->configEpoch;
    if (nodeIsSlave(node) && node->slaveof) {
        nodeEpoch = node->slaveof->configEpoch;
    }
    /* Latency from the POV of this node, config epoch, link status */
    //来自该节点的 POV 的延迟、配置 epoch、链接状态
    ci = sdscatfmt(ci," %I %I %U %s",
        (long long) node->ping_sent,
        (long long) node->pong_received,
        nodeEpoch,
        (node->link || node->flags & CLUSTER_NODE_MYSELF) ?
                    "connected" : "disconnected");

    /* Slots served by this instance. If we already have slots info,
     * append it directly, otherwise, generate slots only if it has. */
    //此实例服务的插槽。如果我们已经有插槽信息，则直接附加它，否则，仅在有时才生成插槽。
    if (node->slot_info_pairs) {
        ci = representSlotInfo(ci, node->slot_info_pairs, node->slot_info_pairs_count);
    } else if (node->numslots > 0) {
        start = -1;
        for (j = 0; j < CLUSTER_SLOTS; j++) {
            int bit;

            if ((bit = clusterNodeGetSlotBit(node,j)) != 0) {
                if (start == -1) start = j;
            }
            if (start != -1 && (!bit || j == CLUSTER_SLOTS-1)) {
                if (bit && j == CLUSTER_SLOTS-1) j++;

                if (start == j-1) {
                    ci = sdscatfmt(ci," %i",start);
                } else {
                    ci = sdscatfmt(ci," %i-%i",start,j-1);
                }
                start = -1;
            }
        }
    }

    /* Just for MYSELF node we also dump info about slots that
     * we are migrating to other instances or importing from other
     * instances. */
    //仅对于 MYSELF 节点，我们还转储有关我们正在迁移到其他实例或从其他实例导入的插槽的信息。
    if (node->flags & CLUSTER_NODE_MYSELF) {
        for (j = 0; j < CLUSTER_SLOTS; j++) {
            if (server.cluster->migrating_slots_to[j]) {
                ci = sdscatprintf(ci," [%d->-%.40s]",j,
                    server.cluster->migrating_slots_to[j]->name);
            } else if (server.cluster->importing_slots_from[j]) {
                ci = sdscatprintf(ci," [%d-<-%.40s]",j,
                    server.cluster->importing_slots_from[j]->name);
            }
        }
    }
    return ci;
}

/* Generate the slot topology for all nodes and store the string representation
 * in the slots_info struct on the node. This is used to improve the efficiency
 * of clusterGenNodesDescription() because it removes looping of the slot space
 * for generating the slot info for each node individually. */
/**
 * 为所有节点生成槽拓扑并将字符串表示形式存储在节点上的 slot_info 结构中。
 * 这用于提高 clusterGenNodesDescription() 的效率，因为它消除了用于为每个节点单独生成槽信息的槽空间循环。
 * */
void clusterGenNodesSlotsInfo(int filter) {
    clusterNode *n = NULL;
    int start = -1;

    for (int i = 0; i <= CLUSTER_SLOTS; i++) {
        /* Find start node and slot id. 查找起始节点和槽 id。*/
        if (n == NULL) {
            if (i == CLUSTER_SLOTS) break;
            n = server.cluster->slots[i];
            start = i;
            continue;
        }

        /* Generate slots info when occur different node with start
         * or end of slot. */
        //当出现具有插槽开始或结束的不同节点时生成插槽信息。
        if (i == CLUSTER_SLOTS || n != server.cluster->slots[i]) {
            if (!(n->flags & filter)) {
                if (!n->slot_info_pairs) {
                    n->slot_info_pairs = zmalloc(2 * n->numslots * sizeof(uint16_t));
                }
                serverAssert((n->slot_info_pairs_count + 1) < (2 * n->numslots));
                n->slot_info_pairs[n->slot_info_pairs_count++] = start;
                n->slot_info_pairs[n->slot_info_pairs_count++] = i-1;
            }
            if (i == CLUSTER_SLOTS) break;
            n = server.cluster->slots[i];
            start = i;
        }
    }
}

void clusterFreeNodesSlotsInfo(clusterNode *n) {
    zfree(n->slot_info_pairs);
    n->slot_info_pairs = NULL;
    n->slot_info_pairs_count = 0;
}

/* Generate a csv-alike representation of the nodes we are aware of,
 * including the "myself" node, and return an SDS string containing the
 * representation (it is up to the caller to free it).
 *
 * All the nodes matching at least one of the node flags specified in
 * "filter" are excluded from the output, so using zero as a filter will
 * include all the known nodes in the representation, including nodes in
 * the HANDSHAKE state.
 *
 * Setting use_pport to 1 in a TLS cluster makes the result contain the
 * plaintext client port rather then the TLS client port of each node.
 *
 * The representation obtained using this function is used for the output
 * of the CLUSTER NODES function, and as format for the cluster
 * configuration file (nodes.conf) for a given node. */
/**
 * 生成我们知道的节点的类似 csv 的表示，包括“我自己”节点，并返回包含表示的 SDS 字符串（由调用者释放它）。
 * 与“filter”中指定的至少一个节点标志匹配的所有节点都从输出中排除，因此使用零作为过滤器将包括表示中的所有已知节点，
 * 包括处于 HANDSHAKE 状态的节点。在 TLS 集群中将 use_pport 设置为 1 会使结果包含明文客户端端口，
 * 而不是每个节点的 TLS 客户端端口。使用此函数获得的表示用于 CLUSTER NODES 函数的输出，
 * 并作为给定节点的集群配置文件 (nodes.conf) 的格式。
 * */
sds clusterGenNodesDescription(int filter, int use_pport) {
    sds ci = sdsempty(), ni;
    dictIterator *di;
    dictEntry *de;

    /* Generate all nodes slots info firstly. */
    //首先生成所有节点插槽信息。
    clusterGenNodesSlotsInfo(filter);

    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        if (node->flags & filter) continue;
        ni = clusterGenNodeDescription(node, use_pport);
        ci = sdscatsds(ci,ni);
        sdsfree(ni);
        ci = sdscatlen(ci,"\n",1);

        /* Release slots info. */
        clusterFreeNodesSlotsInfo(node);
    }
    dictReleaseIterator(di);
    return ci;
}

/* Add to the output buffer of the given client the description of the given cluster link.
 * The description is a map with each entry being an attribute of the link. */
//将给定集群链接的描述添加到给定客户端的输出缓冲区。描述是一个映射，每个条目都是链接的一个属性。
void addReplyClusterLinkDescription(client *c, clusterLink *link) {
    addReplyMapLen(c, 6);

    addReplyBulkCString(c, "direction");
    addReplyBulkCString(c, link->inbound ? "from" : "to");

    /* addReplyClusterLinkDescription is only called for links that have been
     * associated with nodes. The association is always bi-directional, so
     * in addReplyClusterLinkDescription, link->node should never be NULL. */
    /**
     * 仅对已与节点关联的链接调用 addReplyClusterLinkDescription。
     * 关联始终是双向的，因此在 addReplyClusterLinkDescription 中，link->node 永远不应为 NULL。
     * */
    serverAssert(link->node);
    sds node_name = sdsnewlen(link->node->name, CLUSTER_NAMELEN);
    addReplyBulkCString(c, "node");
    addReplyBulkCString(c, node_name);
    sdsfree(node_name);

    addReplyBulkCString(c, "create-time");
    addReplyLongLong(c, link->ctime);

    char events[3], *p;
    p = events;
    if (link->conn) {
        if (connHasReadHandler(link->conn)) *p++ = 'r';
        if (connHasWriteHandler(link->conn)) *p++ = 'w';
    }
    *p = '\0';
    addReplyBulkCString(c, "events");
    addReplyBulkCString(c, events);

    addReplyBulkCString(c, "send-buffer-allocated");
    addReplyLongLong(c, sdsalloc(link->sndbuf));

    addReplyBulkCString(c, "send-buffer-used");
    addReplyLongLong(c, sdslen(link->sndbuf));
}

/* Add to the output buffer of the given client an array of cluster link descriptions,
 * with array entry being a description of a single current cluster link. */
//将集群链接描述数组添加到给定客户端的输出缓冲区，数组条目是单个当前集群链接的描述。
void addReplyClusterLinksDescription(client *c) {
    dictIterator *di;
    dictEntry *de;
    void *arraylen_ptr = NULL;
    int num_links = 0;

    arraylen_ptr = addReplyDeferredLen(c);

    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        if (node->link) {
            num_links++;
            addReplyClusterLinkDescription(c, node->link);
        }
        if (node->inbound_link) {
            num_links++;
            addReplyClusterLinkDescription(c, node->inbound_link);
        }
    }
    dictReleaseIterator(di);

    setDeferredArrayLen(c, arraylen_ptr, num_links);
}

/* -----------------------------------------------------------------------------
 * CLUSTER command
 * -------------------------------------------------------------------------- */

const char *getPreferredEndpoint(clusterNode *n) {
    switch(server.cluster_preferred_endpoint_type) {
    case CLUSTER_ENDPOINT_TYPE_IP: return n->ip;
    case CLUSTER_ENDPOINT_TYPE_HOSTNAME: return (sdslen(n->hostname) != 0) ? n->hostname : "?";
    case CLUSTER_ENDPOINT_TYPE_UNKNOWN_ENDPOINT: return "";
    }
    return "unknown";
}

const char *clusterGetMessageTypeString(int type) {
    switch(type) {
    case CLUSTERMSG_TYPE_PING: return "ping";
    case CLUSTERMSG_TYPE_PONG: return "pong";
    case CLUSTERMSG_TYPE_MEET: return "meet";
    case CLUSTERMSG_TYPE_FAIL: return "fail";
    case CLUSTERMSG_TYPE_PUBLISH: return "publish";
    case CLUSTERMSG_TYPE_PUBLISHSHARD: return "publishshard";
    case CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST: return "auth-req";
    case CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK: return "auth-ack";
    case CLUSTERMSG_TYPE_UPDATE: return "update";
    case CLUSTERMSG_TYPE_MFSTART: return "mfstart";
    case CLUSTERMSG_TYPE_MODULE: return "module";
    }
    return "unknown";
}

int getSlotOrReply(client *c, robj *o) {
    long long slot;

    if (getLongLongFromObject(o,&slot) != C_OK ||
        slot < 0 || slot >= CLUSTER_SLOTS)
    {
        addReplyError(c,"Invalid or out of range slot");
        return -1;
    }
    return (int) slot;
}

/* Returns an indication if the replica node is fully available
 * and should be listed in CLUSTER SLOTS response.
 * Returns 1 for available nodes, 0 for nodes that have 
 * not finished their initial sync, in failed state, or are 
 * otherwise considered not available to serve read commands. */
/**
 * 如果副本节点完全可用并且应在 CLUSTER SLOTS 响应中列出，则返回指示。
 * 返回 1 表示可用节点，返回 0 表示尚未完成初始同步、处于失败状态或被认为不可用于提供读取命令的节点。
 * */
static int isReplicaAvailable(clusterNode *node) {
    if (nodeFailed(node)) {
        return 0;
    }
    long long repl_offset = node->repl_offset;
    if (node->flags & CLUSTER_NODE_MYSELF) {
        /* Nodes do not update their own information
         * in the cluster node list. */
        //节点不会在集群节点列表中更新自己的信息。
        repl_offset = replicationGetSlaveOffset();
    }
    return (repl_offset != 0);
}

int checkSlotAssignmentsOrReply(client *c, unsigned char *slots, int del, int start_slot, int end_slot) {
    int slot;
    for (slot = start_slot; slot <= end_slot; slot++) {
        if (del && server.cluster->slots[slot] == NULL) {
            addReplyErrorFormat(c,"Slot %d is already unassigned", slot);
            return C_ERR;
        } else if (!del && server.cluster->slots[slot]) {
            addReplyErrorFormat(c,"Slot %d is already busy", slot);
            return C_ERR;
        }
        if (slots[slot]++ == 1) {
            addReplyErrorFormat(c,"Slot %d specified multiple times",(int)slot);
            return C_ERR;
        }
    }
    return C_OK;
}

void clusterUpdateSlots(client *c, unsigned char *slots, int del) {
    int j;
    for (j = 0; j < CLUSTER_SLOTS; j++) {
        if (slots[j]) {
            int retval;
                
            /* If this slot was set as importing we can clear this
             * state as now we are the real owner of the slot. */
            //如果此插槽设置为正在导入，我们可以清除此状态，因为现在我们是插槽的真正所有者。
            if (server.cluster->importing_slots_from[j])
                server.cluster->importing_slots_from[j] = NULL;

            retval = del ? clusterDelSlot(j) :
                           clusterAddSlot(myself,j);
            serverAssertWithInfo(c,NULL,retval == C_OK);
        }
    }
}

void addNodeToNodeReply(client *c, clusterNode *node) {
    addReplyArrayLen(c, 4);
    if (server.cluster_preferred_endpoint_type == CLUSTER_ENDPOINT_TYPE_IP) {
        addReplyBulkCString(c, node->ip);
    } else if (server.cluster_preferred_endpoint_type == CLUSTER_ENDPOINT_TYPE_HOSTNAME) {
        addReplyBulkCString(c, sdslen(node->hostname) != 0 ? node->hostname : "?");
    } else if (server.cluster_preferred_endpoint_type == CLUSTER_ENDPOINT_TYPE_UNKNOWN_ENDPOINT) {
        addReplyNull(c);
    } else {
        serverPanic("Unrecognized preferred endpoint type");
    }
    
    /* Report non-TLS ports to non-TLS client in TLS cluster if available. */
    //如果可用，将非 TLS 端口报告给 TLS 集群中的非 TLS 客户端。
    int use_pport = (server.tls_cluster &&
                     c->conn && connGetType(c->conn) != CONN_TYPE_TLS);
    addReplyLongLong(c, use_pport && node->pport ? node->pport : node->port);
    addReplyBulkCBuffer(c, node->name, CLUSTER_NAMELEN);

    /* Add the additional endpoint information, this is all the known networking information
     * that is not the preferred endpoint. */
    //添加额外的端点信息，这是所有已知的不是首选端点的网络信息。
    void *deflen = addReplyDeferredLen(c);
    int length = 0;
    if (server.cluster_preferred_endpoint_type != CLUSTER_ENDPOINT_TYPE_IP) {
        addReplyBulkCString(c, "ip");
        addReplyBulkCString(c, node->ip);
        length++;
    }
    if (server.cluster_preferred_endpoint_type != CLUSTER_ENDPOINT_TYPE_HOSTNAME
        && sdslen(node->hostname) != 0)
    {
        addReplyBulkCString(c, "hostname");
        addReplyBulkCString(c, node->hostname);
        length++;
    }
    setDeferredMapLen(c, deflen, length);
}

void addNodeReplyForClusterSlot(client *c, clusterNode *node, int start_slot, int end_slot) {
    int i, nested_elements = 3; /* slots (2) + master addr (1) */
    void *nested_replylen = addReplyDeferredLen(c);
    addReplyLongLong(c, start_slot);
    addReplyLongLong(c, end_slot);
    addNodeToNodeReply(c, node);
    
    /* Remaining nodes in reply are replicas for slot range */
    //回复的剩余节点是插槽范围的副本
    for (i = 0; i < node->numslaves; i++) {
        /* This loop is copy/pasted from clusterGenNodeDescription()
         * with modifications for per-slot node aggregation. */
        //此循环从 clusterGenNodeDescription() 复制粘贴，并针对每个插槽节点聚合进行了修改。
        if (!isReplicaAvailable(node->slaves[i])) continue;
        addNodeToNodeReply(c, node->slaves[i]);
        nested_elements++;
    }
    setDeferredArrayLen(c, nested_replylen, nested_elements);
}

/* Add detailed information of a node to the output buffer of the given client. */
//将节点的详细信息添加到给定客户端的输出缓冲区。
void addNodeDetailsToShardReply(client *c, clusterNode *node) {
    int reply_count = 0;
    void *node_replylen = addReplyDeferredLen(c);
    addReplyBulkCString(c, "id");
    addReplyBulkCBuffer(c, node->name, CLUSTER_NAMELEN);
    reply_count++;

    /* We use server.tls_cluster as a proxy for whether or not
     * the remote port is the tls port or not */
    //我们使用 server.tls_cluster 作为远程端口是否为 tls 端口的代理
    int plaintext_port = server.tls_cluster ? node->pport : node->port;
    int tls_port = server.tls_cluster ? node->port : 0;
    if (plaintext_port) {
        addReplyBulkCString(c, "port");
        addReplyLongLong(c, plaintext_port);
        reply_count++;
    }

    if (tls_port) {
        addReplyBulkCString(c, "tls-port");
        addReplyLongLong(c, tls_port);
        reply_count++;
    }

    addReplyBulkCString(c, "ip");
    addReplyBulkCString(c, node->ip);
    reply_count++;

    addReplyBulkCString(c, "endpoint");
    addReplyBulkCString(c, getPreferredEndpoint(node));
    reply_count++;

    if (node->hostname) {
        addReplyBulkCString(c, "hostname");
        addReplyBulkCString(c, node->hostname);
        reply_count++;
    }

    long long node_offset;
    if (node->flags & CLUSTER_NODE_MYSELF) {
        node_offset = nodeIsSlave(node) ? replicationGetSlaveOffset() : server.master_repl_offset;
    } else {
        node_offset = node->repl_offset;
    }

    addReplyBulkCString(c, "role");
    addReplyBulkCString(c, nodeIsSlave(node) ? "replica" : "master");
    reply_count++;

    addReplyBulkCString(c, "replication-offset");
    addReplyLongLong(c, node_offset);
    reply_count++;

    addReplyBulkCString(c, "health");
    const char *health_msg = NULL;
    if (nodeFailed(node)) {
        health_msg = "fail";
    } else if (nodeIsSlave(node) && node_offset == 0) {
        health_msg = "loading";
    } else {
        health_msg = "online";
    }
    addReplyBulkCString(c, health_msg);
    reply_count++;

    setDeferredMapLen(c, node_replylen, reply_count);
}

/* Add the shard reply of a single shard based off the given primary node. */
//根据给定的主节点添加单个分片的分片回复。
void addShardReplyForClusterShards(client *c, clusterNode *node, uint16_t *slot_info_pairs, int slot_pairs_count) {
    addReplyMapLen(c, 2);
    addReplyBulkCString(c, "slots");
    if (slot_info_pairs) {
        serverAssert((slot_pairs_count % 2) == 0);
        addReplyArrayLen(c, slot_pairs_count);
        for (int i = 0; i < slot_pairs_count; i++)
            addReplyLongLong(c, (unsigned long)slot_info_pairs[i]);
    } else {
        /* If no slot info pair is provided, the node owns no slots */
        //如果没有提供槽信息对，则节点不拥有槽
        addReplyArrayLen(c, 0);
    }

    addReplyBulkCString(c, "nodes");
    list *nodes_for_slot = clusterGetNodesServingMySlots(node);
    /* At least the provided node should be serving its slots */
    //至少提供的节点应该为其插槽提供服务
    serverAssert(nodes_for_slot);
    addReplyArrayLen(c, listLength(nodes_for_slot));
    if (listLength(nodes_for_slot) != 0) {
        listIter li;
        listNode *ln;
        listRewind(nodes_for_slot, &li);
        while ((ln = listNext(&li))) {
            clusterNode *node = listNodeValue(ln);
            addNodeDetailsToShardReply(c, node);
        }
        listRelease(nodes_for_slot);
    }
}

/* Add to the output buffer of the given client, an array of slot (start, end)
 * pair owned by the shard, also the primary and set of replica(s) along with
 * information about each node. */
//添加到给定客户端的输出缓冲区，分片拥有的插槽（开始，结束）对数组，以及主副本和副本集以及有关每个节点的信息。
void clusterReplyShards(client *c) {
    void *shard_replylen = addReplyDeferredLen(c);
    int shard_count = 0;
    /* This call will add slot_info_pairs to all nodes */
    //此调用会将 slot_info_pairs 添加到所有节点
    clusterGenNodesSlotsInfo(0);
    dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
    dictEntry *de;
    /* Iterate over all the available nodes in the cluster, for each primary
     * node return generate the cluster shards response. if the primary node
     * doesn't own any slot, cluster shard response contains the node related
     * information and an empty slots array. */
    /**
     * 迭代集群中的所有可用节点，为每个主节点返回生成集群分片响应。
     * 如果主节点不拥有任何槽，则集群分片响应包含节点相关信息和一个空槽数组。
     * */
    while((de = dictNext(di)) != NULL) {
        clusterNode *n = dictGetVal(de);
        if (!nodeIsMaster(n)) {
            /* You can force a replica to own slots, even though it'll get reverted,
             * so freeing the slot pair here just in case. */
            //您可以强制副本拥有插槽，即使它会被还原，因此在此处释放插槽对以防万一。
            clusterFreeNodesSlotsInfo(n);
            continue;
        }
        shard_count++;
        /* n->slot_info_pairs is set to NULL when the the node owns no slots. */
        //当节点不拥有槽时，n->slot_info_pairs 设置为 NULL。
        addShardReplyForClusterShards(c, n, n->slot_info_pairs, n->slot_info_pairs_count);
        clusterFreeNodesSlotsInfo(n);
    }
    dictReleaseIterator(di);
    setDeferredArrayLen(c, shard_replylen, shard_count);
}

void clusterReplyMultiBulkSlots(client * c) {
    /* Format: 1) 1) start slot
     *            2) end slot
     *            3) 1) master IP
     *               2) master port
     *               3) node ID
     *            4) 1) replica IP
     *               2) replica port
     *               3) node ID
     *           ... continued until done
     */
    clusterNode *n = NULL;
    int num_masters = 0, start = -1;
    void *slot_replylen = addReplyDeferredLen(c);

    for (int i = 0; i <= CLUSTER_SLOTS; i++) {
        /* Find start node and slot id. 查找起始节点和槽 id。*/
        if (n == NULL) {
            if (i == CLUSTER_SLOTS) break;
            n = server.cluster->slots[i];
            start = i;
            continue;
        }

        /* Add cluster slots info when occur different node with start
         * or end of slot. */
        //当出现具有插槽开始或结束的不同节点时添加集群插槽信息。
        if (i == CLUSTER_SLOTS || n != server.cluster->slots[i]) {
            addNodeReplyForClusterSlot(c, n, start, i-1);
            num_masters++;
            if (i == CLUSTER_SLOTS) break;
            n = server.cluster->slots[i];
            start = i;
        }
    }
    setDeferredArrayLen(c, slot_replylen, num_masters);
}

void clusterCommand(client *c) {
    if (server.cluster_enabled == 0) {
        addReplyError(c,"This instance has cluster support disabled");
        return;
    }

    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"help")) {
        const char *help[] = {
"ADDSLOTS <slot> [<slot> ...]",
"    Assign slots to current node.",
"ADDSLOTSRANGE <start slot> <end slot> [<start slot> <end slot> ...]",
"    Assign slots which are between <start-slot> and <end-slot> to current node.",
"BUMPEPOCH",
"    Advance the cluster config epoch.",
"COUNT-FAILURE-REPORTS <node-id>",
"    Return number of failure reports for <node-id>.",
"COUNTKEYSINSLOT <slot>",
"    Return the number of keys in <slot>.",
"DELSLOTS <slot> [<slot> ...]",
"    Delete slots information from current node.",
"DELSLOTSRANGE <start slot> <end slot> [<start slot> <end slot> ...]",
"    Delete slots information which are between <start-slot> and <end-slot> from current node.",
"FAILOVER [FORCE|TAKEOVER]",
"    Promote current replica node to being a master.",
"FORGET <node-id>",
"    Remove a node from the cluster.",
"GETKEYSINSLOT <slot> <count>",
"    Return key names stored by current node in a slot.",
"FLUSHSLOTS",
"    Delete current node own slots information.",
"INFO",
"    Return information about the cluster.",
"KEYSLOT <key>",
"    Return the hash slot for <key>.",
"MEET <ip> <port> [<bus-port>]",
"    Connect nodes into a working cluster.",
"MYID",
"    Return the node id.",
"NODES",
"    Return cluster configuration seen by node. Output format:",
"    <id> <ip:port> <flags> <master> <pings> <pongs> <epoch> <link> <slot> ...",
"REPLICATE <node-id>",
"    Configure current node as replica to <node-id>.",
"RESET [HARD|SOFT]",
"    Reset current node (default: soft).",
"SET-CONFIG-EPOCH <epoch>",
"    Set config epoch of current node.",
"SETSLOT <slot> (IMPORTING <node-id>|MIGRATING <node-id>|STABLE|NODE <node-id>)",
"    Set slot state.",
"REPLICAS <node-id>",
"    Return <node-id> replicas.",
"SAVECONFIG",
"    Force saving cluster configuration on disk.",
"SLOTS",
"    Return information about slots range mappings. Each range is made of:",
"    start, end, master and replicas IP addresses, ports and ids",
"SHARDS",
"    Return information about slot range mappings and the nodes associated with them.",
"LINKS",
"    Return information about all network links between this node and its peers.",
"    Output format is an array where each array element is a map containing attributes of a link",
NULL
        };
        addReplyHelp(c, help);
    } else if (!strcasecmp(c->argv[1]->ptr,"meet") && (c->argc == 4 || c->argc == 5)) {
        /* CLUSTER MEET <ip> <port> [cport] */
        long long port, cport;

        if (getLongLongFromObject(c->argv[3], &port) != C_OK) {
            addReplyErrorFormat(c,"Invalid TCP base port specified: %s",
                                (char*)c->argv[3]->ptr);
            return;
        }

        if (c->argc == 5) {
            if (getLongLongFromObject(c->argv[4], &cport) != C_OK) {
                addReplyErrorFormat(c,"Invalid TCP bus port specified: %s",
                                    (char*)c->argv[4]->ptr);
                return;
            }
        } else {
            cport = port + CLUSTER_PORT_INCR;
        }

        if (clusterStartHandshake(c->argv[2]->ptr,port,cport) == 0 &&
            errno == EINVAL)
        {
            addReplyErrorFormat(c,"Invalid node address specified: %s:%s",
                            (char*)c->argv[2]->ptr, (char*)c->argv[3]->ptr);
        } else {
            addReply(c,shared.ok);
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"nodes") && c->argc == 2) {
        /* CLUSTER NODES */
        /* Report plaintext ports, only if cluster is TLS but client is known to
         * be non-TLS). */
        //集群节点报告明文端口，仅当集群是 TLS 但已知客户端是非 TLS 时）。
        int use_pport = (server.tls_cluster &&
                        c->conn && connGetType(c->conn) != CONN_TYPE_TLS);
        sds nodes = clusterGenNodesDescription(0, use_pport);
        addReplyVerbatim(c,nodes,sdslen(nodes),"txt");
        sdsfree(nodes);
    } else if (!strcasecmp(c->argv[1]->ptr,"myid") && c->argc == 2) {
        /* CLUSTER MYID */
        addReplyBulkCBuffer(c,myself->name, CLUSTER_NAMELEN);
    } else if (!strcasecmp(c->argv[1]->ptr,"slots") && c->argc == 2) {
        /* CLUSTER SLOTS */
        clusterReplyMultiBulkSlots(c);
    } else if (!strcasecmp(c->argv[1]->ptr,"shards") && c->argc == 2) {
        /* CLUSTER SHARDS */
        clusterReplyShards(c);
    } else if (!strcasecmp(c->argv[1]->ptr,"flushslots") && c->argc == 2) {
        /* CLUSTER FLUSHSLOTS */
        if (dictSize(server.db[0].dict) != 0) {
            addReplyError(c,"DB must be empty to perform CLUSTER FLUSHSLOTS.");
            return;
        }
        clusterDelNodeSlots(myself);
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
        addReply(c,shared.ok);
    } else if ((!strcasecmp(c->argv[1]->ptr,"addslots") ||
               !strcasecmp(c->argv[1]->ptr,"delslots")) && c->argc >= 3)
    {
        /* CLUSTER ADDSLOTS <slot> [slot] ... */
        /* CLUSTER DELSLOTS <slot> [slot] ... */
        int j, slot;
        unsigned char *slots = zmalloc(CLUSTER_SLOTS);
        int del = !strcasecmp(c->argv[1]->ptr,"delslots");

        memset(slots,0,CLUSTER_SLOTS);
        /* Check that all the arguments are parseable.*/
        for (j = 2; j < c->argc; j++) {
            if ((slot = getSlotOrReply(c,c->argv[j])) == C_ERR) {
                zfree(slots);
                return;
            }
        }
        /* Check that the slots are not already busy. */
        //检查插槽是否已忙。
        for (j = 2; j < c->argc; j++) {
            slot = getSlotOrReply(c,c->argv[j]);
            if (checkSlotAssignmentsOrReply(c, slots, del, slot, slot) == C_ERR) {
                zfree(slots);
                return;
            }
        }
        clusterUpdateSlots(c, slots, del);    
        zfree(slots);
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
        addReply(c,shared.ok);
    } else if ((!strcasecmp(c->argv[1]->ptr,"addslotsrange") ||
               !strcasecmp(c->argv[1]->ptr,"delslotsrange")) && c->argc >= 4) {
        if (c->argc % 2 == 1) {
            addReplyErrorArity(c);
            return;
        }
        /* CLUSTER ADDSLOTSRANGE <start slot> <end slot> [<start slot> <end slot> ...] */
        /* CLUSTER DELSLOTSRANGE <start slot> <end slot> [<start slot> <end slot> ...] */
        int j, startslot, endslot;
        unsigned char *slots = zmalloc(CLUSTER_SLOTS);
        int del = !strcasecmp(c->argv[1]->ptr,"delslotsrange");

        memset(slots,0,CLUSTER_SLOTS);
        /* Check that all the arguments are parseable and that all the
         * slots are not already busy. */
        //检查所有参数是否都是可解析的，并且所有插槽都不是已经忙了。
        for (j = 2; j < c->argc; j += 2) {
            if ((startslot = getSlotOrReply(c,c->argv[j])) == C_ERR) {
                zfree(slots);
                return;
            }
            if ((endslot = getSlotOrReply(c,c->argv[j+1])) == C_ERR) {
                zfree(slots);
                return;
            }
            if (startslot > endslot) {
                addReplyErrorFormat(c,"start slot number %d is greater than end slot number %d", startslot, endslot);
                zfree(slots);
                return;
            }

            if (checkSlotAssignmentsOrReply(c, slots, del, startslot, endslot) == C_ERR) {
                zfree(slots);
                return;
            }
        }
        clusterUpdateSlots(c, slots, del);
        zfree(slots);
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"setslot") && c->argc >= 4) {
        /* SETSLOT 10 MIGRATING <node ID> */
        /* SETSLOT 10 IMPORTING <node ID> */
        /* SETSLOT 10 STABLE */
        /* SETSLOT 10 NODE <node ID> */
        int slot;
        clusterNode *n;

        if (nodeIsSlave(myself)) {
            addReplyError(c,"Please use SETSLOT only with masters.");
            return;
        }

        if ((slot = getSlotOrReply(c,c->argv[2])) == -1) return;

        if (!strcasecmp(c->argv[3]->ptr,"migrating") && c->argc == 5) {
            if (server.cluster->slots[slot] != myself) {
                addReplyErrorFormat(c,"I'm not the owner of hash slot %u",slot);
                return;
            }
            n = clusterLookupNode(c->argv[4]->ptr, sdslen(c->argv[4]->ptr));
            if (n == NULL) {
                addReplyErrorFormat(c,"I don't know about node %s",
                    (char*)c->argv[4]->ptr);
                return;
            }
            if (nodeIsSlave(n)) {
                addReplyError(c,"Target node is not a master");
                return;
            }
            server.cluster->migrating_slots_to[slot] = n;
        } else if (!strcasecmp(c->argv[3]->ptr,"importing") && c->argc == 5) {
            if (server.cluster->slots[slot] == myself) {
                addReplyErrorFormat(c,
                    "I'm already the owner of hash slot %u",slot);
                return;
            }
            n = clusterLookupNode(c->argv[4]->ptr, sdslen(c->argv[4]->ptr));
            if (n == NULL) {
                addReplyErrorFormat(c,"I don't know about node %s",
                    (char*)c->argv[4]->ptr);
                return;
            }
            if (nodeIsSlave(n)) {
                addReplyError(c,"Target node is not a master");
                return;
            }
            server.cluster->importing_slots_from[slot] = n;
        } else if (!strcasecmp(c->argv[3]->ptr,"stable") && c->argc == 4) {
            /* CLUSTER SETSLOT <SLOT> STABLE */
            server.cluster->importing_slots_from[slot] = NULL;
            server.cluster->migrating_slots_to[slot] = NULL;
        } else if (!strcasecmp(c->argv[3]->ptr,"node") && c->argc == 5) {
            /* CLUSTER SETSLOT <SLOT> NODE <NODE ID> */
            n = clusterLookupNode(c->argv[4]->ptr, sdslen(c->argv[4]->ptr));
            if (!n) {
                addReplyErrorFormat(c,"Unknown node %s",
                    (char*)c->argv[4]->ptr);
                return;
            }
            if (nodeIsSlave(n)) {
                addReplyError(c,"Target node is not a master");
                return;
            }
            /* If this hash slot was served by 'myself' before to switch
             * make sure there are no longer local keys for this hash slot. */
            //如果此哈希槽在切换之前由“myself”提供服务，请确保此哈希槽不再有本地密钥。
            if (server.cluster->slots[slot] == myself && n != myself) {
                if (countKeysInSlot(slot) != 0) {
                    addReplyErrorFormat(c,
                        "Can't assign hashslot %d to a different node "
                        "while I still hold keys for this hash slot.", slot);
                    return;
                }
            }
            /* If this slot is in migrating status but we have no keys
             * for it assigning the slot to another node will clear
             * the migrating status. */
            //如果此插槽处于迁移状态，但我们没有为其分配插槽的密钥，则将插槽分配给另一个节点将清除迁移状态。
            if (countKeysInSlot(slot) == 0 &&
                server.cluster->migrating_slots_to[slot])
                server.cluster->migrating_slots_to[slot] = NULL;

            int slot_was_mine = server.cluster->slots[slot] == myself;
            clusterDelSlot(slot);
            clusterAddSlot(n,slot);

            /* If we are a master left without slots, we should turn into a
             * replica of the new master. */
            //如果我们是一个没有槽的master，我们应该变成新master的副本。
            if (slot_was_mine &&
                n != myself &&
                myself->numslots == 0 &&
                server.cluster_allow_replica_migration)
            {
                serverLog(LL_WARNING,
                          "Configuration change detected. Reconfiguring myself "
                          "as a replica of %.40s", n->name);
                clusterSetMaster(n);
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG |
                                     CLUSTER_TODO_UPDATE_STATE |
                                     CLUSTER_TODO_FSYNC_CONFIG);
            }

            /* If this node was importing this slot, assigning the slot to
             * itself also clears the importing status. */
            //如果此节点正在导入此插槽，则将插槽分配给自身也会清除导入状态。
            if (n == myself &&
                server.cluster->importing_slots_from[slot])
            {
                /* This slot was manually migrated, set this node configEpoch
                 * to a new epoch so that the new version can be propagated
                 * by the cluster.
                 *
                 * Note that if this ever results in a collision with another
                 * node getting the same configEpoch, for example because a
                 * failover happens at the same time we close the slot, the
                 * configEpoch collision resolution will fix it assigning
                 * a different epoch to each node. */
                /**
                 * 这个 slot 是手动迁移的，将这个节点 configEpoch 设置为一个新的 epoch，这样新版本就可以被集群传播。
                 * 请注意，如果这导致与获得相同 configEpoch 的另一个节点发生冲突，
                 * 例如因为在我们关闭插槽的同时发生故障转移，则 configEpoch 冲突解决方案将修复它，
                 * 为每个节点分配不同的 epoch。
                 * */
                if (clusterBumpConfigEpochWithoutConsensus() == C_OK) {
                    serverLog(LL_WARNING,
                        "configEpoch updated after importing slot %d", slot);
                }
                server.cluster->importing_slots_from[slot] = NULL;
                /* After importing this slot, let the other nodes know as
                 * soon as possible. */
                //导入此槽后，尽快让其他节点知道。
                clusterBroadcastPong(CLUSTER_BROADCAST_ALL);
            }
        } else {
            addReplyError(c,
                "Invalid CLUSTER SETSLOT action or number of arguments. Try CLUSTER HELP");
            return;
        }
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|CLUSTER_TODO_UPDATE_STATE);
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"bumpepoch") && c->argc == 2) {
        /* CLUSTER BUMPEPOCH */
        int retval = clusterBumpConfigEpochWithoutConsensus();
        sds reply = sdscatprintf(sdsempty(),"+%s %llu\r\n",
                (retval == C_OK) ? "BUMPED" : "STILL",
                (unsigned long long) myself->configEpoch);
        addReplySds(c,reply);
    } else if (!strcasecmp(c->argv[1]->ptr,"info") && c->argc == 2) {
        /* CLUSTER INFO */
        char *statestr[] = {"ok","fail"};
        int slots_assigned = 0, slots_ok = 0, slots_pfail = 0, slots_fail = 0;
        uint64_t myepoch;
        int j;

        for (j = 0; j < CLUSTER_SLOTS; j++) {
            clusterNode *n = server.cluster->slots[j];

            if (n == NULL) continue;
            slots_assigned++;
            if (nodeFailed(n)) {
                slots_fail++;
            } else if (nodeTimedOut(n)) {
                slots_pfail++;
            } else {
                slots_ok++;
            }
        }

        myepoch = (nodeIsSlave(myself) && myself->slaveof) ?
                  myself->slaveof->configEpoch : myself->configEpoch;

        sds info = sdscatprintf(sdsempty(),
            "cluster_state:%s\r\n"
            "cluster_slots_assigned:%d\r\n"
            "cluster_slots_ok:%d\r\n"
            "cluster_slots_pfail:%d\r\n"
            "cluster_slots_fail:%d\r\n"
            "cluster_known_nodes:%lu\r\n"
            "cluster_size:%d\r\n"
            "cluster_current_epoch:%llu\r\n"
            "cluster_my_epoch:%llu\r\n"
            , statestr[server.cluster->state],
            slots_assigned,
            slots_ok,
            slots_pfail,
            slots_fail,
            dictSize(server.cluster->nodes),
            server.cluster->size,
            (unsigned long long) server.cluster->currentEpoch,
            (unsigned long long) myepoch
        );

        /* Show stats about messages sent and received. */
        //显示有关发送和接收的消息的统计信息。
        long long tot_msg_sent = 0;
        long long tot_msg_received = 0;

        for (int i = 0; i < CLUSTERMSG_TYPE_COUNT; i++) {
            if (server.cluster->stats_bus_messages_sent[i] == 0) continue;
            tot_msg_sent += server.cluster->stats_bus_messages_sent[i];
            info = sdscatprintf(info,
                "cluster_stats_messages_%s_sent:%lld\r\n",
                clusterGetMessageTypeString(i),
                server.cluster->stats_bus_messages_sent[i]);
        }
        info = sdscatprintf(info,
            "cluster_stats_messages_sent:%lld\r\n", tot_msg_sent);

        for (int i = 0; i < CLUSTERMSG_TYPE_COUNT; i++) {
            if (server.cluster->stats_bus_messages_received[i] == 0) continue;
            tot_msg_received += server.cluster->stats_bus_messages_received[i];
            info = sdscatprintf(info,
                "cluster_stats_messages_%s_received:%lld\r\n",
                clusterGetMessageTypeString(i),
                server.cluster->stats_bus_messages_received[i]);
        }
        info = sdscatprintf(info,
            "cluster_stats_messages_received:%lld\r\n", tot_msg_received);

        info = sdscatprintf(info,
            "total_cluster_links_buffer_limit_exceeded:%llu\r\n",
            server.cluster->stat_cluster_links_buffer_limit_exceeded);

        /* Produce the reply protocol. 生成回复协议。*/
        addReplyVerbatim(c,info,sdslen(info),"txt");
        sdsfree(info);
    } else if (!strcasecmp(c->argv[1]->ptr,"saveconfig") && c->argc == 2) {
        int retval = clusterSaveConfig(1);

        if (retval == 0)
            addReply(c,shared.ok);
        else
            addReplyErrorFormat(c,"error saving the cluster node config: %s",
                strerror(errno));
    } else if (!strcasecmp(c->argv[1]->ptr,"keyslot") && c->argc == 3) {
        /* CLUSTER KEYSLOT <key> */
        sds key = c->argv[2]->ptr;

        addReplyLongLong(c,keyHashSlot(key,sdslen(key)));
    } else if (!strcasecmp(c->argv[1]->ptr,"countkeysinslot") && c->argc == 3) {
        /* CLUSTER COUNTKEYSINSLOT <slot> */
        long long slot;

        if (getLongLongFromObjectOrReply(c,c->argv[2],&slot,NULL) != C_OK)
            return;
        if (slot < 0 || slot >= CLUSTER_SLOTS) {
            addReplyError(c,"Invalid slot");
            return;
        }
        addReplyLongLong(c,countKeysInSlot(slot));
    } else if (!strcasecmp(c->argv[1]->ptr,"getkeysinslot") && c->argc == 4) {
        /* CLUSTER GETKEYSINSLOT <slot> <count> */
        long long maxkeys, slot;

        if (getLongLongFromObjectOrReply(c,c->argv[2],&slot,NULL) != C_OK)
            return;
        if (getLongLongFromObjectOrReply(c,c->argv[3],&maxkeys,NULL)
            != C_OK)
            return;
        if (slot < 0 || slot >= CLUSTER_SLOTS || maxkeys < 0) {
            addReplyError(c,"Invalid slot or number of keys");
            return;
        }

        unsigned int keys_in_slot = countKeysInSlot(slot);
        unsigned int numkeys = maxkeys > keys_in_slot ? keys_in_slot : maxkeys;
        addReplyArrayLen(c,numkeys);
        dictEntry *de = (*server.db->slots_to_keys).by_slot[slot].head;
        for (unsigned int j = 0; j < numkeys; j++) {
            serverAssert(de != NULL);
            sds sdskey = dictGetKey(de);
            addReplyBulkCBuffer(c, sdskey, sdslen(sdskey));
            de = dictEntryNextInSlot(de);
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"forget") && c->argc == 3) {
        /* CLUSTER FORGET <NODE ID> */
        clusterNode *n = clusterLookupNode(c->argv[2]->ptr, sdslen(c->argv[2]->ptr));
        if (!n) {
            addReplyErrorFormat(c,"Unknown node %s", (char*)c->argv[2]->ptr);
            return;
        } else if (n == myself) {
            addReplyError(c,"I tried hard but I can't forget myself...");
            return;
        } else if (nodeIsSlave(myself) && myself->slaveof == n) {
            addReplyError(c,"Can't forget my master!");
            return;
        }
        clusterBlacklistAddNode(n);
        clusterDelNode(n);
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|
                             CLUSTER_TODO_SAVE_CONFIG);
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"replicate") && c->argc == 3) {
        /* CLUSTER REPLICATE <NODE ID> */
        /* Lookup the specified node in our table. 在我们的表中查找指定的节点。*/
        clusterNode *n = clusterLookupNode(c->argv[2]->ptr, sdslen(c->argv[2]->ptr));
        if (!n) {
            addReplyErrorFormat(c,"Unknown node %s", (char*)c->argv[2]->ptr);
            return;
        }

        /* I can't replicate myself. 我无法复制自己。*/
        if (n == myself) {
            addReplyError(c,"Can't replicate myself");
            return;
        }

        /* Can't replicate a slave. 无法复制slave。*/
        if (nodeIsSlave(n)) {
            addReplyError(c,"I can only replicate a master, not a replica.");
            return;
        }

        /* If the instance is currently a master, it should have no assigned
         * slots nor keys to accept to replicate some other node.
         * Slaves can switch to another master without issues. */
        //如果该实例当前是一个主节点，它应该没有分配的槽或键来接受复制其他节点。从站可以毫无问题地切换到另一个主站。
        if (nodeIsMaster(myself) &&
            (myself->numslots != 0 || dictSize(server.db[0].dict) != 0)) {
            addReplyError(c,
                "To set a master the node must be empty and "
                "without assigned slots.");
            return;
        }

        /* Set the master. */
        clusterSetMaster(n);
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
        addReply(c,shared.ok);
    } else if ((!strcasecmp(c->argv[1]->ptr,"slaves") ||
                !strcasecmp(c->argv[1]->ptr,"replicas")) && c->argc == 3) {
        /* CLUSTER SLAVES <NODE ID> */
        clusterNode *n = clusterLookupNode(c->argv[2]->ptr, sdslen(c->argv[2]->ptr));
        int j;

        /* Lookup the specified node in our table. 在我们的表中查找指定的节点。*/
        if (!n) {
            addReplyErrorFormat(c,"Unknown node %s", (char*)c->argv[2]->ptr);
            return;
        }

        if (nodeIsSlave(n)) {
            addReplyError(c,"The specified node is not a master");
            return;
        }

        /* Use plaintext port if cluster is TLS but client is non-TLS. */
        //如果集群是 TLS 但客户端是非 TLS，则使用明文端口。
        int use_pport = (server.tls_cluster &&
                         c->conn && connGetType(c->conn) != CONN_TYPE_TLS);
        addReplyArrayLen(c,n->numslaves);
        for (j = 0; j < n->numslaves; j++) {
            sds ni = clusterGenNodeDescription(n->slaves[j], use_pport);
            addReplyBulkCString(c,ni);
            sdsfree(ni);
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"count-failure-reports") &&
               c->argc == 3)
    {
        /* CLUSTER COUNT-FAILURE-REPORTS <NODE ID> */
        clusterNode *n = clusterLookupNode(c->argv[2]->ptr, sdslen(c->argv[2]->ptr));

        if (!n) {
            addReplyErrorFormat(c,"Unknown node %s", (char*)c->argv[2]->ptr);
            return;
        } else {
            addReplyLongLong(c,clusterNodeFailureReportsCount(n));
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"failover") &&
               (c->argc == 2 || c->argc == 3))
    {
        /* CLUSTER FAILOVER [FORCE|TAKEOVER] */
        int force = 0, takeover = 0;

        if (c->argc == 3) {
            if (!strcasecmp(c->argv[2]->ptr,"force")) {
                force = 1;
            } else if (!strcasecmp(c->argv[2]->ptr,"takeover")) {
                takeover = 1;
                force = 1; /* Takeover also implies force. 接管也意味着力量。*/
            } else {
                addReplyErrorObject(c,shared.syntaxerr);
                return;
            }
        }

        /* Check preconditions. 检查前提条件。*/
        if (nodeIsMaster(myself)) {
            addReplyError(c,"You should send CLUSTER FAILOVER to a replica");
            return;
        } else if (myself->slaveof == NULL) {
            addReplyError(c,"I'm a replica but my master is unknown to me");
            return;
        } else if (!force &&
                   (nodeFailed(myself->slaveof) ||
                    myself->slaveof->link == NULL))
        {
            addReplyError(c,"Master is down or failed, "
                            "please use CLUSTER FAILOVER FORCE");
            return;
        }
        resetManualFailover();
        server.cluster->mf_end = mstime() + CLUSTER_MF_TIMEOUT;

        if (takeover) {
            /* A takeover does not perform any initial check. It just
             * generates a new configuration epoch for this node without
             * consensus, claims the master's slots, and broadcast the new
             * configuration. */
            //接管不会执行任何初始检查。它只是在没有共识的情况下为该节点生成一个新的配置纪元，声明主节点的插槽，并广播新的配置。
            serverLog(LL_WARNING,"Taking over the master (user request).");
            clusterBumpConfigEpochWithoutConsensus();
            clusterFailoverReplaceYourMaster();
        } else if (force) {
            /* If this is a forced failover, we don't need to talk with our
             * master to agree about the offset. We just failover taking over
             * it without coordination. */
            //如果这是一个强制故障转移，我们不需要与我们的主人讨论就偏移量达成一致。我们只是在没有协调的情况下故障转移接管它。
            serverLog(LL_WARNING,"Forced failover user request accepted.");
            server.cluster->mf_can_start = 1;
        } else {
            serverLog(LL_WARNING,"Manual failover user request accepted.");
            clusterSendMFStart(myself->slaveof);
        }
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"set-config-epoch") && c->argc == 3)
    {
        /* CLUSTER SET-CONFIG-EPOCH <epoch>
         *
         * The user is allowed to set the config epoch only when a node is
         * totally fresh: no config epoch, no other known node, and so forth.
         * This happens at cluster creation time to start with a cluster where
         * every node has a different node ID, without to rely on the conflicts
         * resolution system which is too slow when a big cluster is created.
         *
         * CLUSTER SET-CONFIG-EPOCH <epoch>
         * 仅当节点完全新鲜时，用户才被允许设置配置纪元：没有配置纪元，没有其他已知节点，等等。
         * 这发生在集群创建时，从每个节点都有不同节点 ID 的集群开始，而不依赖于在创建大集群时太慢的冲突解决系统。
         * */
        long long epoch;

        if (getLongLongFromObjectOrReply(c,c->argv[2],&epoch,NULL) != C_OK)
            return;

        if (epoch < 0) {
            addReplyErrorFormat(c,"Invalid config epoch specified: %lld",epoch);
        } else if (dictSize(server.cluster->nodes) > 1) {
            addReplyError(c,"The user can assign a config epoch only when the "
                            "node does not know any other node.");
        } else if (myself->configEpoch != 0) {
            addReplyError(c,"Node config epoch is already non-zero");
        } else {
            myself->configEpoch = epoch;
            serverLog(LL_WARNING,
                "configEpoch set to %llu via CLUSTER SET-CONFIG-EPOCH",
                (unsigned long long) myself->configEpoch);

            if (server.cluster->currentEpoch < (uint64_t)epoch)
                server.cluster->currentEpoch = epoch;
            /* No need to fsync the config here since in the unlucky event
             * of a failure to persist the config, the conflict resolution code
             * will assign a unique config to this node. */
            //无需在此处同步配置，因为如果不幸无法持久化配置，冲突解决代码将为该节点分配唯一的配置。
            clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|
                                 CLUSTER_TODO_SAVE_CONFIG);
            addReply(c,shared.ok);
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"reset") &&
               (c->argc == 2 || c->argc == 3))
    {
        /* CLUSTER RESET [SOFT|HARD] */
        int hard = 0;

        /* Parse soft/hard argument. Default is soft. */
        //解析软硬参数。默认为软。
        if (c->argc == 3) {
            if (!strcasecmp(c->argv[2]->ptr,"hard")) {
                hard = 1;
            } else if (!strcasecmp(c->argv[2]->ptr,"soft")) {
                hard = 0;
            } else {
                addReplyErrorObject(c,shared.syntaxerr);
                return;
            }
        }

        /* Slaves can be reset while containing data, but not master nodes
         * that must be empty. */
        //从站可以在包含数据时重置，但不能重置必须为空的主节点。
        if (nodeIsMaster(myself) && dictSize(c->db->dict) != 0) {
            addReplyError(c,"CLUSTER RESET can't be called with "
                            "master nodes containing keys");
            return;
        }
        clusterReset(hard);
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"links") && c->argc == 2) {
        /* CLUSTER LINKS */
        addReplyClusterLinksDescription(c);
    } else {
        addReplySubcommandSyntaxError(c);
        return;
    }
}

void removeChannelsInSlot(unsigned int slot) {
    unsigned int channelcount = countChannelsInSlot(slot);
    if (channelcount == 0) return;

    /* Retrieve all the channels for the slot. */
    //检索插槽的所有通道。
    robj **channels = zmalloc(sizeof(robj*)*channelcount);
    raxIterator iter;
    int j = 0;
    unsigned char indexed[2];

    indexed[0] = (slot >> 8) & 0xff;
    indexed[1] = slot & 0xff;
    raxStart(&iter,server.cluster->slots_to_channels);
    raxSeek(&iter,">=",indexed,2);
    while(raxNext(&iter)) {
        if (iter.key[0] != indexed[0] || iter.key[1] != indexed[1]) break;
        channels[j++] = createStringObject((char*)iter.key + 2, iter.key_len - 2);
    }
    raxStop(&iter);

    pubsubUnsubscribeShardChannels(channels, channelcount);
    zfree(channels);
}

/* -----------------------------------------------------------------------------
 * DUMP, RESTORE and MIGRATE commands
 * -------------------------------------------------------------------------- */

/* Generates a DUMP-format representation of the object 'o', adding it to the
 * io stream pointed by 'rio'. This function can't fail. */
//生成对象 'o' 的 DUMP 格式表示，将其添加到 'rio' 指向的 io 流中。此功能不能失败。
void createDumpPayload(rio *payload, robj *o, robj *key, int dbid) {
    unsigned char buf[2];
    uint64_t crc;

    /* Serialize the object in an RDB-like format. It consist of an object type
     * byte followed by the serialized object. This is understood by RESTORE. */
    //以类似 RDB 的格式序列化对象。它由一个对象类型字节后跟序列化对象组成。 RESTORE 可以理解这一点。
    rioInitWithBuffer(payload,sdsempty());
    serverAssert(rdbSaveObjectType(payload,o));
    serverAssert(rdbSaveObject(payload,o,key,dbid));

    /* Write the footer, this is how it looks like:
     * 写下页脚，它是这样的：
     * ----------------+---------------------+---------------+
     * ... RDB payload | 2 bytes RDB version | 8 bytes CRC64 |
     * ----------------+---------------------+---------------+
     * RDB version and CRC are both in little endian.
     * RDB 版本和 CRC 都是 little endian。
     */

    /* RDB version */
    buf[0] = RDB_VERSION & 0xff;
    buf[1] = (RDB_VERSION >> 8) & 0xff;
    payload->io.buffer.ptr = sdscatlen(payload->io.buffer.ptr,buf,2);

    /* CRC64 */
    crc = crc64(0,(unsigned char*)payload->io.buffer.ptr,
                sdslen(payload->io.buffer.ptr));
    memrev64ifbe(&crc);
    payload->io.buffer.ptr = sdscatlen(payload->io.buffer.ptr,&crc,8);
}

/* Verify that the RDB version of the dump payload matches the one of this Redis
 * instance and that the checksum is ok.
 * If the DUMP payload looks valid C_OK is returned, otherwise C_ERR
 * is returned. If rdbver_ptr is not NULL, its populated with the value read
 * from the input buffer. */
/**
 * 验证转储负载的 RDB 版本是否与此 Redis 实例之一匹配，并且校验和是否正常。如果 DUMP 有效负载看起来有效，
 * 则返回 C_OK，否则返回 C_ERR。如果 rdbver_ptr 不为 NULL，则使用从输入缓冲区读取的值填充它。
 * */
int verifyDumpPayload(unsigned char *p, size_t len, uint16_t *rdbver_ptr) {
    unsigned char *footer;
    uint16_t rdbver;
    uint64_t crc;

    /* At least 2 bytes of RDB version and 8 of CRC64 should be present. */
    //至少应存在 2 个字节的 RDB 版本和 8 个 CRC64。
    if (len < 10) return C_ERR;
    footer = p+(len-10);

    /* Set and verify RDB version. */
    rdbver = (footer[1] << 8) | footer[0];
    if (rdbver_ptr) {
        *rdbver_ptr = rdbver;
    }
    if (rdbver > RDB_VERSION) return C_ERR;

    if (server.skip_checksum_validation)
        return C_OK;

    /* Verify CRC64 */
    crc = crc64(0,p,len-8);
    memrev64ifbe(&crc);
    return (memcmp(&crc,footer+2,8) == 0) ? C_OK : C_ERR;
}

/* DUMP keyname
 * DUMP is actually not used by Redis Cluster but it is the obvious
 * complement of RESTORE and can be useful for different applications. */
//DUMP keyname DUMP 实际上并没有被 Redis Cluster 使用，但它是 RESTORE 的明显补充，可以用于不同的应用程序。
void dumpCommand(client *c) {
    robj *o;
    rio payload;

    /* Check if the key is here. 检查钥匙是否在这里。*/
    if ((o = lookupKeyRead(c->db,c->argv[1])) == NULL) {
        addReplyNull(c);
        return;
    }

    /* Create the DUMP encoded representation. 创建 DUMP 编码表示。*/
    createDumpPayload(&payload,o,c->argv[1],c->db->id);

    /* Transfer to the client 转给客户*/
    addReplyBulkSds(c,payload.io.buffer.ptr);
    return;
}

/* RESTORE key ttl serialized-value [REPLACE] [ABSTTL] [IDLETIME seconds] [FREQ frequency] */
void restoreCommand(client *c) {
    long long ttl, lfu_freq = -1, lru_idle = -1, lru_clock = -1;
    rio payload;
    int j, type, replace = 0, absttl = 0;
    robj *obj;

    /* Parse additional options 解析附加选项*/
    for (j = 4; j < c->argc; j++) {
        int additional = c->argc-j-1;
        if (!strcasecmp(c->argv[j]->ptr,"replace")) {
            replace = 1;
        } else if (!strcasecmp(c->argv[j]->ptr,"absttl")) {
            absttl = 1;
        } else if (!strcasecmp(c->argv[j]->ptr,"idletime") && additional >= 1 &&
                   lfu_freq == -1)
        {
            if (getLongLongFromObjectOrReply(c,c->argv[j+1],&lru_idle,NULL)
                    != C_OK) return;
            if (lru_idle < 0) {
                addReplyError(c,"Invalid IDLETIME value, must be >= 0");
                return;
            }
            lru_clock = LRU_CLOCK();
            j++; /* Consume additional arg. 消耗额外的 arg。*/
        } else if (!strcasecmp(c->argv[j]->ptr,"freq") && additional >= 1 &&
                   lru_idle == -1)
        {
            if (getLongLongFromObjectOrReply(c,c->argv[j+1],&lfu_freq,NULL)
                    != C_OK) return;
            if (lfu_freq < 0 || lfu_freq > 255) {
                addReplyError(c,"Invalid FREQ value, must be >= 0 and <= 255");
                return;
            }
            j++; /* Consume additional arg. */
        } else {
            addReplyErrorObject(c,shared.syntaxerr);
            return;
        }
    }

    /* Make sure this key does not already exist here... */
    //确保此处不存在此密钥...
    robj *key = c->argv[1];
    if (!replace && lookupKeyWrite(c->db,key) != NULL) {
        addReplyErrorObject(c,shared.busykeyerr);
        return;
    }

    /* Check if the TTL value makes sense */
    //检查 TTL 值是否有意义
    if (getLongLongFromObjectOrReply(c,c->argv[2],&ttl,NULL) != C_OK) {
        return;
    } else if (ttl < 0) {
        addReplyError(c,"Invalid TTL value, must be >= 0");
        return;
    }

    /* Verify RDB version and data checksum. */
    //验证 RDB 版本和数据校验和。
    if (verifyDumpPayload(c->argv[3]->ptr,sdslen(c->argv[3]->ptr),NULL) == C_ERR)
    {
        addReplyError(c,"DUMP payload version or checksum are wrong");
        return;
    }

    rioInitWithBuffer(&payload,c->argv[3]->ptr);
    if (((type = rdbLoadObjectType(&payload)) == -1) ||
        ((obj = rdbLoadObject(type,&payload,key->ptr,c->db->id,NULL)) == NULL))
    {
        addReplyError(c,"Bad data format");
        return;
    }

    /* Remove the old key if needed. 如果需要，请移除旧密钥。*/
    int deleted = 0;
    if (replace)
        deleted = dbDelete(c->db,key);

    if (ttl && !absttl) ttl+=mstime();
    if (ttl && checkAlreadyExpired(ttl)) {
        if (deleted) {
            rewriteClientCommandVector(c,2,shared.del,key);
            signalModifiedKey(c,c->db,key);
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",key,c->db->id);
            server.dirty++;
        }
        decrRefCount(obj);
        addReply(c, shared.ok);
        return;
    }

    /* Create the key and set the TTL if any */
    //创建密钥并设置 TTL（如果有）
    dbAdd(c->db,key,obj);
    if (ttl) {
        setExpire(c,c->db,key,ttl);
        if (!absttl) {
            /* Propagate TTL as absolute timestamp */
            //将 TTL 传播为绝对时间戳
            robj *ttl_obj = createStringObjectFromLongLong(ttl);
            rewriteClientCommandArgument(c,2,ttl_obj);
            decrRefCount(ttl_obj);
            rewriteClientCommandArgument(c,c->argc,shared.absttl);
        }
    }
    objectSetLRUOrLFU(obj,lfu_freq,lru_idle,lru_clock,1000);
    signalModifiedKey(c,c->db,key);
    notifyKeyspaceEvent(NOTIFY_GENERIC,"restore",key,c->db->id);
    addReply(c,shared.ok);
    server.dirty++;
}

/* MIGRATE socket cache implementation.
 *
 * We take a map between host:ip and a TCP socket that we used to connect
 * to this instance in recent time.
 * This sockets are closed when the max number we cache is reached, and also
 * in serverCron() when they are around for more than a few seconds. */
/**
 * MIGRATE 套接字缓存实现。我们在 host:ip 和我们最近用来连接到这个实例的 TCP 套接字之间进行映射。
 * 当达到我们缓存的最大数量时，此套接字将关闭，并且当它们存在超过几秒钟时，也会在 serverCron() 中关闭。
 * */
#define MIGRATE_SOCKET_CACHE_ITEMS 64 /* max num of items in the cache. 缓存中的最大项目数。*/
#define MIGRATE_SOCKET_CACHE_TTL 10 /* close cached sockets after 10 sec. 10 秒后关闭缓存的套接字。*/

typedef struct migrateCachedSocket {
    connection *conn;
    long last_dbid;
    time_t last_use_time;
} migrateCachedSocket;

/* Return a migrateCachedSocket containing a TCP socket connected with the
 * target instance, possibly returning a cached one.
 *
 * This function is responsible of sending errors to the client if a
 * connection can't be established. In this case -1 is returned.
 * Otherwise on success the socket is returned, and the caller should not
 * attempt to free it after usage.
 *
 * If the caller detects an error while using the socket, migrateCloseSocket()
 * should be called so that the connection will be created from scratch
 * the next time. */
/**
 * 返回包含与目标实例连接的 TCP 套接字的 migrateCachedSocket，可能返回一个缓存的。
 * 如果无法建立连接，此函数负责向客户端发送错误。在这种情况下，返回 -1。否则成功返回套接字，调用者不应尝试在使用后释放它。
 * 如果调用者在使用套接字时检测到错误，则应调用 migrateCloseSocket() 以便下次从头开始创建连接。
 * */
migrateCachedSocket* migrateGetSocket(client *c, robj *host, robj *port, long timeout) {
    connection *conn;
    sds name = sdsempty();
    migrateCachedSocket *cs;

    /* Check if we have an already cached socket for this ip:port pair. */
    //检查我们是否已经为这个 ip:port 对缓存了一个套接字。
    name = sdscatlen(name,host->ptr,sdslen(host->ptr));
    name = sdscatlen(name,":",1);
    name = sdscatlen(name,port->ptr,sdslen(port->ptr));
    cs = dictFetchValue(server.migrate_cached_sockets,name);
    if (cs) {
        sdsfree(name);
        cs->last_use_time = server.unixtime;
        return cs;
    }

    /* No cached socket, create one. */
    if (dictSize(server.migrate_cached_sockets) == MIGRATE_SOCKET_CACHE_ITEMS) {
        /* Too many items, drop one at random. 物品太多，随便丢一个。*/
        dictEntry *de = dictGetRandomKey(server.migrate_cached_sockets);
        cs = dictGetVal(de);
        connClose(cs->conn);
        zfree(cs);
        dictDelete(server.migrate_cached_sockets,dictGetKey(de));
    }

    /* Create the socket */
    conn = server.tls_cluster ? connCreateTLS() : connCreateSocket();
    if (connBlockingConnect(conn, host->ptr, atoi(port->ptr), timeout)
            != C_OK) {
        addReplyError(c,"-IOERR error or timeout connecting to the client");
        connClose(conn);
        sdsfree(name);
        return NULL;
    }
    connEnableTcpNoDelay(conn);

    /* Add to the cache and return it to the caller. */
    //添加到缓存并将其返回给调用者。
    cs = zmalloc(sizeof(*cs));
    cs->conn = conn;

    cs->last_dbid = -1;
    cs->last_use_time = server.unixtime;
    dictAdd(server.migrate_cached_sockets,name,cs);
    return cs;
}

/* Free a migrate cached connection. */
//释放一个迁移缓存的连接。
void migrateCloseSocket(robj *host, robj *port) {
    sds name = sdsempty();
    migrateCachedSocket *cs;

    name = sdscatlen(name,host->ptr,sdslen(host->ptr));
    name = sdscatlen(name,":",1);
    name = sdscatlen(name,port->ptr,sdslen(port->ptr));
    cs = dictFetchValue(server.migrate_cached_sockets,name);
    if (!cs) {
        sdsfree(name);
        return;
    }

    connClose(cs->conn);
    zfree(cs);
    dictDelete(server.migrate_cached_sockets,name);
    sdsfree(name);
}

void migrateCloseTimedoutSockets(void) {
    dictIterator *di = dictGetSafeIterator(server.migrate_cached_sockets);
    dictEntry *de;

    while((de = dictNext(di)) != NULL) {
        migrateCachedSocket *cs = dictGetVal(de);

        if ((server.unixtime - cs->last_use_time) > MIGRATE_SOCKET_CACHE_TTL) {
            connClose(cs->conn);
            zfree(cs);
            dictDelete(server.migrate_cached_sockets,dictGetKey(de));
        }
    }
    dictReleaseIterator(di);
}

/* MIGRATE host port key dbid timeout [COPY | REPLACE | AUTH password |
 *         AUTH2 username password]
 *
 * On in the multiple keys form:
 * 在多键形式中：
 *
 * MIGRATE host port "" dbid timeout [COPY | REPLACE | AUTH password |
 *         AUTH2 username password] KEYS key1 key2 ... keyN */
void migrateCommand(client *c) {
    migrateCachedSocket *cs;
    int copy = 0, replace = 0, j;
    char *username = NULL;
    char *password = NULL;
    long timeout;
    long dbid;
    robj **ov = NULL; /* Objects to migrate. 要迁移的对象。 */
    robj **kv = NULL; /* Key names. */
    robj **newargv = NULL; /* Used to rewrite the command as DEL ... keys ...  用于将命令改写为 DEL ... keys ...*/
    rio cmd, payload;
    int may_retry = 1;
    int write_error = 0;
    int argv_rewritten = 0;

    /* To support the KEYS option we need the following additional state. */
    //为了支持 KEYS 选项，我们需要以下附加状态。
    int first_key = 3; /* Argument index of the first key. 第一个键的参数索引。*/
    int num_keys = 1;  /* By default only migrate the 'key' argument. 默认情况下，仅迁移 'key' 参数。*/

    /* Parse additional options 解析附加选项*/
    for (j = 6; j < c->argc; j++) {
        int moreargs = (c->argc-1) - j;
        if (!strcasecmp(c->argv[j]->ptr,"copy")) {
            copy = 1;
        } else if (!strcasecmp(c->argv[j]->ptr,"replace")) {
            replace = 1;
        } else if (!strcasecmp(c->argv[j]->ptr,"auth")) {
            if (!moreargs) {
                addReplyErrorObject(c,shared.syntaxerr);
                return;
            }
            j++;
            password = c->argv[j]->ptr;
            redactClientCommandArgument(c,j);
        } else if (!strcasecmp(c->argv[j]->ptr,"auth2")) {
            if (moreargs < 2) {
                addReplyErrorObject(c,shared.syntaxerr);
                return;
            }
            username = c->argv[++j]->ptr;
            redactClientCommandArgument(c,j);
            password = c->argv[++j]->ptr;
            redactClientCommandArgument(c,j);
        } else if (!strcasecmp(c->argv[j]->ptr,"keys")) {
            if (sdslen(c->argv[3]->ptr) != 0) {
                addReplyError(c,
                    "When using MIGRATE KEYS option, the key argument"
                    " must be set to the empty string");
                return;
            }
            first_key = j+1;
            num_keys = c->argc - j - 1;
            break; /* All the remaining args are keys. 所有剩余的参数都是键。*/
        } else {
            addReplyErrorObject(c,shared.syntaxerr);
            return;
        }
    }

    /* Sanity check */
    if (getLongFromObjectOrReply(c,c->argv[5],&timeout,NULL) != C_OK ||
        getLongFromObjectOrReply(c,c->argv[4],&dbid,NULL) != C_OK)
    {
        return;
    }
    if (timeout <= 0) timeout = 1000;

    /* Check if the keys are here. If at least one key is to migrate, do it
     * otherwise if all the keys are missing reply with "NOKEY" to signal
     * the caller there was nothing to migrate. We don't return an error in
     * this case, since often this is due to a normal condition like the key
     * expiring in the meantime. */
    /**
     * 检查钥匙是否在这里。如果至少要迁移一个键，则如果所有键都丢失，则使用“NOKEY”回复以向调用者发出没有要迁移的信号，
     * 否则请执行此操作。在这种情况下，我们不会返回错误，因为这通常是由于密钥在此期间到期等正常情况造成的。
     * */
    ov = zrealloc(ov,sizeof(robj*)*num_keys);
    kv = zrealloc(kv,sizeof(robj*)*num_keys);
    int oi = 0;

    for (j = 0; j < num_keys; j++) {
        if ((ov[oi] = lookupKeyRead(c->db,c->argv[first_key+j])) != NULL) {
            kv[oi] = c->argv[first_key+j];
            oi++;
        }
    }
    num_keys = oi;
    if (num_keys == 0) {
        zfree(ov); zfree(kv);
        addReplySds(c,sdsnew("+NOKEY\r\n"));
        return;
    }

try_again:
    write_error = 0;

    /* Connect */
    cs = migrateGetSocket(c,c->argv[1],c->argv[2],timeout);
    if (cs == NULL) {
        zfree(ov); zfree(kv);
        return; /* error sent to the client by migrateGetSocket()  migrateGetSocket() 发送给客户端的错误*/
    }

    rioInitWithBuffer(&cmd,sdsempty());

    /* Authentication */
    if (password) {
        int arity = username ? 3 : 2;
        serverAssertWithInfo(c,NULL,rioWriteBulkCount(&cmd,'*',arity));
        serverAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,"AUTH",4));
        if (username) {
            serverAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,username,
                                 sdslen(username)));
        }
        serverAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,password,
            sdslen(password)));
    }

    /* Send the SELECT command if the current DB is not already selected. */
    //如果尚未选择当前 DB，则发送 SELECT 命令。
    int select = cs->last_dbid != dbid; /* Should we emit SELECT? 我们应该发出 SELECT 吗？*/
    if (select) {
        serverAssertWithInfo(c,NULL,rioWriteBulkCount(&cmd,'*',2));
        serverAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,"SELECT",6));
        serverAssertWithInfo(c,NULL,rioWriteBulkLongLong(&cmd,dbid));
    }

    int non_expired = 0; /* Number of keys that we'll find non expired.
                            Note that serializing large keys may take some time
                            so certain keys that were found non expired by the
                            lookupKey() function, may be expired later.
                            我们将发现未过期的密钥数。请注意，序列化大键可能需要一些时间，
                            因此 lookupKey() 函数发现未过期的某些键可能稍后会过期。*/

    /* Create RESTORE payload and generate the protocol to call the command. */
    //创建 RESTORE 有效负载并生成协议以调用该命令。
    for (j = 0; j < num_keys; j++) {
        long long ttl = 0;
        long long expireat = getExpire(c->db,kv[j]);

        if (expireat != -1) {
            ttl = expireat-mstime();
            if (ttl < 0) {
                continue;
            }
            if (ttl < 1) ttl = 1;
        }

        /* Relocate valid (non expired) keys and values into the array in successive
         * positions to remove holes created by the keys that were present
         * in the first lookup but are now expired after the second lookup. */
        //将有效（未过期）键和值重新定位到数组中的连续位置，以删除由第一次查找中存在但现在在第二次查找后过期的键创建的空洞。
        ov[non_expired] = ov[j];
        kv[non_expired++] = kv[j];

        serverAssertWithInfo(c,NULL,
            rioWriteBulkCount(&cmd,'*',replace ? 5 : 4));

        if (server.cluster_enabled)
            serverAssertWithInfo(c,NULL,
                rioWriteBulkString(&cmd,"RESTORE-ASKING",14));
        else
            serverAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,"RESTORE",7));
        serverAssertWithInfo(c,NULL,sdsEncodedObject(kv[j]));
        serverAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,kv[j]->ptr,
                sdslen(kv[j]->ptr)));
        serverAssertWithInfo(c,NULL,rioWriteBulkLongLong(&cmd,ttl));

        /* Emit the payload argument, that is the serialized object using
         * the DUMP format. */
        //发出有效载荷参数，即使用 DUMP 格式的序列化对象。
        createDumpPayload(&payload,ov[j],kv[j],dbid);
        serverAssertWithInfo(c,NULL,
            rioWriteBulkString(&cmd,payload.io.buffer.ptr,
                               sdslen(payload.io.buffer.ptr)));
        sdsfree(payload.io.buffer.ptr);

        /* Add the REPLACE option to the RESTORE command if it was specified
         * as a MIGRATE option. */
        //如果将 REPLACE 选项指定为 MIGRATE 选项，请将其添加到 RESTORE 命令。
        if (replace)
            serverAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,"REPLACE",7));
    }

    /* Fix the actual number of keys we are migrating. */
    //修复我们正在迁移的实际密钥数量。
    num_keys = non_expired;

    /* Transfer the query to the other node in 64K chunks. */
    //以 64K 块将查询传输到另一个节点。
    errno = 0;
    {
        sds buf = cmd.io.buffer.ptr;
        size_t pos = 0, towrite;
        int nwritten = 0;

        while ((towrite = sdslen(buf)-pos) > 0) {
            towrite = (towrite > (64*1024) ? (64*1024) : towrite);
            nwritten = connSyncWrite(cs->conn,buf+pos,towrite,timeout);
            if (nwritten != (signed)towrite) {
                write_error = 1;
                goto socket_err;
            }
            pos += nwritten;
        }
    }

    char buf0[1024]; /* Auth reply. */
    char buf1[1024]; /* Select reply. */
    char buf2[1024]; /* Restore reply. */

    /* Read the AUTH reply if needed. 如果需要，请阅读 AUTH 回复。 */
    if (password && connSyncReadLine(cs->conn, buf0, sizeof(buf0), timeout) <= 0)
        goto socket_err;

    /* Read the SELECT reply if needed. 如果需要，请阅读 SELECT 回复。*/
    if (select && connSyncReadLine(cs->conn, buf1, sizeof(buf1), timeout) <= 0)
        goto socket_err;

    /* Read the RESTORE replies. 阅读 RESTORE 回复。*/
    int error_from_target = 0;
    int socket_error = 0;
    int del_idx = 1; /* Index of the key argument for the replicated DEL op. 复制的 DEL 操作的关键参数的索引。*/

    /* Allocate the new argument vector that will replace the current command,
     * to propagate the MIGRATE as a DEL command (if no COPY option was given).
     * We allocate num_keys+1 because the additional argument is for "DEL"
     * command name itself. */
    /**
     * 分配将替换当前命令的新参数向量，以将 MIGRATE 作为 DEL 命令传播（如果未给出 COPY 选项）。
     * 我们分配 num_keys+1 是因为附加参数用于“DEL”命令名称本身。
     * */
    if (!copy) newargv = zmalloc(sizeof(robj*)*(num_keys+1));

    for (j = 0; j < num_keys; j++) {
        if (connSyncReadLine(cs->conn, buf2, sizeof(buf2), timeout) <= 0) {
            socket_error = 1;
            break;
        }
        if ((password && buf0[0] == '-') ||
            (select && buf1[0] == '-') ||
            buf2[0] == '-')
        {
            /* On error assume that last_dbid is no longer valid. */
            //出错时假定 last_dbid 不再有效。
            if (!error_from_target) {
                cs->last_dbid = -1;
                char *errbuf;
                if (password && buf0[0] == '-') errbuf = buf0;
                else if (select && buf1[0] == '-') errbuf = buf1;
                else errbuf = buf2;

                error_from_target = 1;
                addReplyErrorFormat(c,"Target instance replied with error: %s",
                    errbuf+1);
            }
        } else {
            if (!copy) {
                /* No COPY option: remove the local key, signal the change. */
                //无 COPY 选项：删除本地密钥，发出更改信号。
                dbDelete(c->db,kv[j]);
                signalModifiedKey(c,c->db,kv[j]);
                notifyKeyspaceEvent(NOTIFY_GENERIC,"del",kv[j],c->db->id);
                server.dirty++;

                /* Populate the argument vector to replace the old one. */
                //填充参数向量以替换旧的。
                newargv[del_idx++] = kv[j];
                incrRefCount(kv[j]);
            }
        }
    }

    /* On socket error, if we want to retry, do it now before rewriting the
     * command vector. We only retry if we are sure nothing was processed
     * and we failed to read the first reply (j == 0 test). */
    /**
     * 在套接字错误时，如果我们想重试，请在重写命令向量之前立即执行。
     * 只有当我们确定没有处理任何内容并且我们未能读取第一个回复（j == 0 测试）时，我们才会重试。
     * */
    if (!error_from_target && socket_error && j == 0 && may_retry &&
        errno != ETIMEDOUT)
    {
        goto socket_err; /* A retry is guaranteed because of tested conditions. 由于测试条件，保证重试。*/
    }

    /* On socket errors, close the migration socket now that we still have
     * the original host/port in the ARGV. Later the original command may be
     * rewritten to DEL and will be too later. */
    //如果出现套接字错误，请关闭迁移套接字，因为我们仍然在 ARGV 中拥有原始主机端口。稍后原始命令可能会被重写为 DEL 并且太晚了。
    if (socket_error) migrateCloseSocket(c->argv[1],c->argv[2]);

    if (!copy) {
        /* Translate MIGRATE as DEL for replication/AOF. Note that we do
         * this only for the keys for which we received an acknowledgement
         * from the receiving Redis server, by using the del_idx index. */
        //将 MIGRATE 翻译为 DEL 以进行复制 AOF。请注意，我们仅对从接收 Redis 服务器收到确认的键执行此操作，使用 del_idx 索引。
        if (del_idx > 1) {
            newargv[0] = createStringObject("DEL",3);
            /* Note that the following call takes ownership of newargv. */
            //请注意，以下调用获取 newargv 的所有权。
            replaceClientCommandVector(c,del_idx,newargv);
            argv_rewritten = 1;
        } else {
            /* No key transfer acknowledged, no need to rewrite as DEL. */
            //没有确认密钥传输，无需重写为 DEL。
            zfree(newargv);
        }
        newargv = NULL; /* Make it safe to call zfree() on it in the future. 确保将来在其上调用 zfree() 是安全的。*/
    }

    /* If we are here and a socket error happened, we don't want to retry.
     * Just signal the problem to the client, but only do it if we did not
     * already queue a different error reported by the destination server. */
    /**
     * 如果我们在这里并且发生了套接字错误，我们不想重试。
     * 只需向客户端发出问题信号，但只有在我们尚未将目标服务器报告的不同错误排队时才这样做。
     * */
    if (!error_from_target && socket_error) {
        may_retry = 0;
        goto socket_err;
    }

    if (!error_from_target) {
        /* Success! Update the last_dbid in migrateCachedSocket, so that we can
         * avoid SELECT the next time if the target DB is the same. Reply +OK.
         *
         * Note: If we reached this point, even if socket_error is true
         * still the SELECT command succeeded (otherwise the code jumps to
         * socket_err label. */
        /**
         * 成功！更新 migrateCachedSocket 中的 last_dbid ，这样我们下次在目标 DB 相同的情况下就可以避免 SELECT 了。
         * 回复+OK。注意：如果我们达到了这一点，即使 socket_error 为真，SELECT 命令仍然成功
         * （否则代码会跳转到 socket_err 标签。
         * */
        cs->last_dbid = dbid;
        addReply(c,shared.ok);
    } else {
        /* On error we already sent it in the for loop above, and set
         * the currently selected socket to -1 to force SELECT the next time.
         * 错误时我们已经在上面的 for 循环中发送了它，并将当前选择的套接字设置为 -1 以强制 SELECT 下一次。*/
    }

    sdsfree(cmd.io.buffer.ptr);
    zfree(ov); zfree(kv); zfree(newargv);
    return;

/* On socket errors we try to close the cached socket and try again.
 * It is very common for the cached socket to get closed, if just reopening
 * it works it's a shame to notify the error to the caller. */
/**
 * 在套接字错误时，我们尝试关闭缓存的套接字并重试。
 * 缓存的套接字关闭是很常见的，如果只是重新打开它就可以工作，将错误通知给调用者是一种耻辱。
 * */
socket_err:
    /* Cleanup we want to perform in both the retry and no retry case.
     * Note: Closing the migrate socket will also force SELECT next time. */
    //我们希望在重试和不重试情况下执行清理。注意：关闭 migrate 套接字也会在下一次强制 SELECT。
    sdsfree(cmd.io.buffer.ptr);

    /* If the command was rewritten as DEL and there was a socket error,
     * we already closed the socket earlier. While migrateCloseSocket()
     * is idempotent, the host/port arguments are now gone, so don't do it
     * again. */
    /**
     * 如果命令被重写为 DEL 并且有一个套接字错误，我们之前已经关闭了套接字。
     * 虽然 migrateCloseSocket() 是幂等的，但 hostport 参数现在已经消失了，所以不要再这样做了。
     * */
    if (!argv_rewritten) migrateCloseSocket(c->argv[1],c->argv[2]);
    zfree(newargv);
    newargv = NULL; /* This will get reallocated on retry.  这将在重试时重新分配。*/

    /* Retry only if it's not a timeout and we never attempted a retry
     * (or the code jumping here did not set may_retry to zero). */
    //仅当它不是超时并且我们从未尝试过重试时才重试（或者此处跳转的代码没有将 may_retry 设置为零）。
    if (errno != ETIMEDOUT && may_retry) {
        may_retry = 0;
        goto try_again;
    }

    /* Cleanup we want to do if no retry is attempted. */
    //如果不尝试重试，我们想要进行清理。
    zfree(ov); zfree(kv);
    addReplyErrorSds(c, sdscatprintf(sdsempty(),
                                  "-IOERR error or timeout %s to target instance",
                                  write_error ? "writing" : "reading"));
    return;
}

/* -----------------------------------------------------------------------------
 * Cluster functions related to serving / redirecting clients 与服务重定向客户端相关的集群功能
 * -------------------------------------------------------------------------- */

/* The ASKING command is required after a -ASK redirection.
 * The client should issue ASKING before to actually send the command to
 * the target instance. See the Redis Cluster specification for more
 * information. */
/**
 * -ASK 重定向后需要 ASKING 命令。客户端应该在将命令实际发送到目标实例之前发出 ASKING。
 * 有关更多信息，请参阅 Redis 集群规范。
 * */
void askingCommand(client *c) {
    if (server.cluster_enabled == 0) {
        addReplyError(c,"This instance has cluster support disabled");
        return;
    }
    c->flags |= CLIENT_ASKING;
    addReply(c,shared.ok);
}

/* The READONLY command is used by clients to enter the read-only mode.
 * In this mode slaves will not redirect clients as long as clients access
 * with read-only commands to keys that are served by the slave's master. */
/**
 * 客户端使用 READONLY 命令进入只读模式。在这种模式下，
 * 只要客户端使用只读命令访问从属主控提供的密钥，从属就不会重定向客户端。
 * */
void readonlyCommand(client *c) {
    if (server.cluster_enabled == 0) {
        addReplyError(c,"This instance has cluster support disabled");
        return;
    }
    c->flags |= CLIENT_READONLY;
    addReply(c,shared.ok);
}

/* The READWRITE command just clears the READONLY command state. */
//READWRITE 命令只是清除 READONLY 命令状态。
void readwriteCommand(client *c) {
    if (server.cluster_enabled == 0) {
        addReplyError(c,"This instance has cluster support disabled");
        return;
    }
    c->flags &= ~CLIENT_READONLY;
    addReply(c,shared.ok);
}

/* Return the pointer to the cluster node that is able to serve the command.
 * For the function to succeed the command should only target either:
 *
 * 1) A single key (even multiple times like LPOPRPUSH mylist mylist).
 * 2) Multiple keys in the same hash slot, while the slot is stable (no
 *    resharding in progress).
 *
 * On success the function returns the node that is able to serve the request.
 * If the node is not 'myself' a redirection must be performed. The kind of
 * redirection is specified setting the integer passed by reference
 * 'error_code', which will be set to CLUSTER_REDIR_ASK or
 * CLUSTER_REDIR_MOVED.
 *
 * When the node is 'myself' 'error_code' is set to CLUSTER_REDIR_NONE.
 *
 * If the command fails NULL is returned, and the reason of the failure is
 * provided via 'error_code', which will be set to:
 *
 * CLUSTER_REDIR_CROSS_SLOT if the request contains multiple keys that
 * don't belong to the same hash slot.
 *
 * CLUSTER_REDIR_UNSTABLE if the request contains multiple keys
 * belonging to the same slot, but the slot is not stable (in migration or
 * importing state, likely because a resharding is in progress).
 *
 * CLUSTER_REDIR_DOWN_UNBOUND if the request addresses a slot which is
 * not bound to any node. In this case the cluster global state should be
 * already "down" but it is fragile to rely on the update of the global state,
 * so we also handle it here.
 *
 * CLUSTER_REDIR_DOWN_STATE and CLUSTER_REDIR_DOWN_RO_STATE if the cluster is
 * down but the user attempts to execute a command that addresses one or more keys. */
/**
 * 返回指向能够为命令提供服务的集群节点的指针。要使函数成功，该命令应仅针对以下任一目标：
 *   1) 单个键（甚至多次，如 LPOPRPUSH mylist mylist）。
 *   2) 同一个哈希槽中有多个键，而槽是稳定的（没有进行重新分片）。
 * 成功时，该函数返回能够为请求提供服务的节点。如果节点不是“我自己”，则必须执行重定向。
 * 重定向的类型指定设置通过引用'error_code'传递的整数，它将设置为CLUSTER_REDIR_ASK或CLUSTER_REDIR_MOVED。
 * 当节点是“我自己”时，“error_code”设置为 CLUSTER_REDIR_NONE。如果命令失败返回NULL，
 * 失败的原因通过'error_code'提供，如果请求包含多个不属于同一个哈希槽的键，则设置为：CLUSTER_REDIR_CROSS_SLOT。
 * CLUSTER_REDIR_UNSTABLE 如果请求包含属于同一槽的多个键，但槽不稳定（处于迁移或导入状态，可能是因为正在进行重新分片）。
 * CLUSTER_REDIR_DOWN_UNBOUND 如果请求地址未绑定到任何节点的插槽。
 * 在这种情况下集群全局状态应该已经“down”了，但是依赖全局状态的更新是脆弱的，所以我们也在这里处理。
 * CLUSTER_REDIR_DOWN_STATE 和 CLUSTER_REDIR_DOWN_RO_STATE 如果集群已关闭但用户尝试执行处理一个或多个键的命令。
 * */
clusterNode *getNodeByQuery(client *c, struct redisCommand *cmd, robj **argv, int argc, int *hashslot, int *error_code) {
    clusterNode *n = NULL;
    robj *firstkey = NULL;
    int multiple_keys = 0;
    multiState *ms, _ms;
    multiCmd mc;
    int i, slot = 0, migrating_slot = 0, importing_slot = 0, missing_keys = 0,
        existing_keys = 0;

    /* Allow any key to be set if a module disabled cluster redirections. */
    //如果模块禁用集群重定向，则允许设置任何键。
    if (server.cluster_module_flags & CLUSTER_MODULE_FLAG_NO_REDIRECTION)
        return myself;

    /* Set error code optimistically for the base case. 为基本情况乐观地设置错误代码。*/
    if (error_code) *error_code = CLUSTER_REDIR_NONE;

    /* Modules can turn off Redis Cluster redirection: this is useful
     * when writing a module that implements a completely different
     * distributed system.
     * 模块可以关闭 Redis 集群重定向：这在编写实现完全不同的分布式系统的模块时很有用。*/

    /* We handle all the cases as if they were EXEC commands, so we have
     * a common code path for everything */
    //我们处理所有情况，就好像它们是 EXEC 命令一样，所以我们对所有事情都有一个共同的代码路径
    if (cmd->proc == execCommand) {
        /* If CLIENT_MULTI flag is not set EXEC is just going to return an
         * error. */
        //如果未设置 CLIENT_MULTI 标志，则 EXEC 只会返回错误。
        if (!(c->flags & CLIENT_MULTI)) return myself;
        ms = &c->mstate;
    } else {
        /* In order to have a single codepath create a fake Multi State
         * structure if the client is not in MULTI/EXEC state, this way
         * we have a single codepath below. */
        /**
         * 如果客户端不处于 MULTIEXEC 状态，为了让单个代码路径创建一个假的多状态结构，这样我们在下面有一个单一的代码路径。
         * */
        ms = &_ms;
        _ms.commands = &mc;
        _ms.count = 1;
        mc.argv = argv;
        mc.argc = argc;
        mc.cmd = cmd;
    }

    int is_pubsubshard = cmd->proc == ssubscribeCommand ||
            cmd->proc == sunsubscribeCommand ||
            cmd->proc == spublishCommand;

    /* Check that all the keys are in the same hash slot, and obtain this
     * slot and the node associated. */
    //检查所有的key是否在同一个hash slot中，并获取这个slot和关联的节点。
    for (i = 0; i < ms->count; i++) {
        struct redisCommand *mcmd;
        robj **margv;
        int margc, numkeys, j;
        keyReference *keyindex;

        mcmd = ms->commands[i].cmd;
        margc = ms->commands[i].argc;
        margv = ms->commands[i].argv;

        getKeysResult result = GETKEYS_RESULT_INIT;
        numkeys = getKeysFromCommand(mcmd,margv,margc,&result);
        keyindex = result.keys;

        for (j = 0; j < numkeys; j++) {
            robj *thiskey = margv[keyindex[j].pos];
            int thisslot = keyHashSlot((char*)thiskey->ptr,
                                       sdslen(thiskey->ptr));

            if (firstkey == NULL) {
                /* This is the first key we see. Check what is the slot
                 * and node. */
                //这是我们看到的第一个键。检查什么是槽和节点。
                firstkey = thiskey;
                slot = thisslot;
                n = server.cluster->slots[slot];

                /* Error: If a slot is not served, we are in "cluster down"
                 * state. However the state is yet to be updated, so this was
                 * not trapped earlier in processCommand(). Report the same
                 * error to the client. */
                /**
                 * 错误：如果未提供插槽，则我们处于“集群关闭”状态。但是状态尚未更新，
                 * 因此在 processCommand() 中并没有更早地捕获它。向客户端报告相同的错误。
                 * */
                if (n == NULL) {
                    getKeysFreeResult(&result);
                    if (error_code)
                        *error_code = CLUSTER_REDIR_DOWN_UNBOUND;
                    return NULL;
                }

                /* If we are migrating or importing this slot, we need to check
                 * if we have all the keys in the request (the only way we
                 * can safely serve the request, otherwise we return a TRYAGAIN
                 * error). To do so we set the importing/migrating state and
                 * increment a counter for every missing key. */
                /**
                 * 如果我们正在迁移或导入此插槽，我们需要检查我们是否拥有请求中的所有密钥
                 * （我们可以安全地为请求提供服务的唯一方法，否则我们会返回 TRYAGAIN 错误）。
                 * 为此，我们设置 importingmigrating 状态并为每个丢失的键增加一个计数器。
                 * */
                if (n == myself &&
                    server.cluster->migrating_slots_to[slot] != NULL)
                {
                    migrating_slot = 1;
                } else if (server.cluster->importing_slots_from[slot] != NULL) {
                    importing_slot = 1;
                }
            } else {
                /* If it is not the first key/channel, make sure it is exactly
                 * the same key/channel as the first we saw. */
                //如果它不是第一个 keychannel，请确保它与我们看到的第一个 keychannel 完全相同。
                if (!equalStringObjects(firstkey,thiskey)) {
                    if (slot != thisslot) {
                        /* Error: multiple keys from different slots. */
                        //错误：来自不同插槽的多个键。
                        getKeysFreeResult(&result);
                        if (error_code)
                            *error_code = CLUSTER_REDIR_CROSS_SLOT;
                        return NULL;
                    } else {
                        /* Flag this request as one with multiple different
                         * keys/channels. */
                        //将此请求标记为具有多个不同密钥通道的请求。
                        multiple_keys = 1;
                    }
                }
            }

            /* Migrating / Importing slot? Count keys we don't have.
             * If it is pubsubshard command, it isn't required to check
             * the channel being present or not in the node during the
             * slot migration, the channel will be served from the source
             * node until the migration completes with CLUSTER SETSLOT <slot>
             * NODE <node-id>. */
            /**
             * 迁移导入槽？数我们没有的钥匙。如果是 pubsubshard 命令，则在 slot 迁移期间不需要检查节点中是否存在通道，
             * 该通道将从源节点提供服务，直到迁移完成 CLUSTER SETSLOT <slot> NODE <node-身份证>。
             * */
            int flags = LOOKUP_NOTOUCH | LOOKUP_NOSTATS | LOOKUP_NONOTIFY;
            if ((migrating_slot || importing_slot) && !is_pubsubshard)
            {
                if (lookupKeyReadWithFlags(&server.db[0], thiskey, flags) == NULL) missing_keys++;
                else existing_keys++;
            }
        }
        getKeysFreeResult(&result);
    }

    /* No key at all in command? then we can serve the request
     * without redirections or errors in all the cases. */
    //根本没有钥匙在指挥？那么我们就可以在所有情况下为请求提供服务而不会出现重定向或错误。
    if (n == NULL) return myself;

    /* Cluster is globally down but we got keys? We only serve the request
     * if it is a read command and when allow_reads_when_down is enabled. */
    //集群全局关闭，但我们有密钥？我们仅在它是读取命令并且启用 allow_reads_when_down 时才提供请求。
    if (server.cluster->state != CLUSTER_OK) {
        if (is_pubsubshard) {
            if (!server.cluster_allow_pubsubshard_when_down) {
                if (error_code) *error_code = CLUSTER_REDIR_DOWN_STATE;
                return NULL;
            }
        } else if (!server.cluster_allow_reads_when_down) {
            /* The cluster is configured to block commands when the
             * cluster is down. */
            //集群配置为在集群关闭时阻止命令。
            if (error_code) *error_code = CLUSTER_REDIR_DOWN_STATE;
            return NULL;
        } else if (cmd->flags & CMD_WRITE) {
            /* The cluster is configured to allow read only commands */
            //集群配置为允许只读命令
            if (error_code) *error_code = CLUSTER_REDIR_DOWN_RO_STATE;
            return NULL;
        } else {
            /* Fall through and allow the command to be executed:
             * this happens when server.cluster_allow_reads_when_down is
             * true and the command is not a write command */
            /**
             * 失败并允许执行命令：当 server.cluster_allow_reads_when_down 为真且命令不是写入命令时会发生这种情况
             * */
        }
    }

    /* Return the hashslot by reference.通过引用返回哈希槽。 */
    if (hashslot) *hashslot = slot;

    /* MIGRATE always works in the context of the local node if the slot
     * is open (migrating or importing state). We need to be able to freely
     * move keys among instances in this case. */
    /**
     * 如果插槽打开（迁移或导入状态），MIGRATE 始终在本地节点的上下文中工作。
     * 在这种情况下，我们需要能够在实例之间自由移动键。
     * */
    if ((migrating_slot || importing_slot) && cmd->proc == migrateCommand)
        return myself;

    /* If we don't have all the keys and we are migrating the slot, send
     * an ASK redirection or TRYAGAIN. */
    //如果我们没有所有密钥并且我们正在迁移插槽，请发送 ASK 重定向或 TRYAGAIN。
    if (migrating_slot && missing_keys) {
        /* If we have keys but we don't have all keys, we return TRYAGAIN */
        //如果我们有密钥但我们没有所有密钥，我们返回 TRYAGAIN
        if (existing_keys) {
            if (error_code) *error_code = CLUSTER_REDIR_UNSTABLE;
            return NULL;
        } else {
            if (error_code) *error_code = CLUSTER_REDIR_ASK;
            return server.cluster->migrating_slots_to[slot];
        }
    }

    /* If we are receiving the slot, and the client correctly flagged the
     * request as "ASKING", we can serve the request. However if the request
     * involves multiple keys and we don't have them all, the only option is
     * to send a TRYAGAIN error. */
    /**
     * 如果我们正在接收槽，并且客户端正确地将请求标记为“ASKING”，我们可以为请求提供服务。
     * 但是，如果请求涉及多个密钥并且我们没有全部，则唯一的选择是发送 TRYAGAIN 错误。
     * */
    if (importing_slot &&
        (c->flags & CLIENT_ASKING || cmd->flags & CMD_ASKING))
    {
        if (multiple_keys && missing_keys) {
            if (error_code) *error_code = CLUSTER_REDIR_UNSTABLE;
            return NULL;
        } else {
            return myself;
        }
    }

    /* Handle the read-only client case reading from a slave: if this
     * node is a slave and the request is about a hash slot our master
     * is serving, we can reply without redirection. */
    //处理从从属节点读取的只读客户端案例：如果该节点是从属节点并且请求是关于我们的主节点正在服务的哈希槽，
    // 我们可以在不重定向的情况下进行回复。
    int is_write_command = (c->cmd->flags & CMD_WRITE) ||
                           (c->cmd->proc == execCommand && (c->mstate.cmd_flags & CMD_WRITE));
    if (((c->flags & CLIENT_READONLY) || is_pubsubshard) &&
        !is_write_command &&
        nodeIsSlave(myself) &&
        myself->slaveof == n)
    {
        return myself;
    }

    /* Base case: just return the right node. However if this node is not
     * myself, set error_code to MOVED since we need to issue a redirection. */
    //基本情况：只返回正确的节点。但是，如果该节点不是我自己，请将 error_code 设置为 MOVED，因为我们需要发出重定向。
    if (n != myself && error_code) *error_code = CLUSTER_REDIR_MOVED;
    return n;
}

/* Send the client the right redirection code, according to error_code
 * that should be set to one of CLUSTER_REDIR_* macros.
 *
 * If CLUSTER_REDIR_ASK or CLUSTER_REDIR_MOVED error codes
 * are used, then the node 'n' should not be NULL, but should be the
 * node we want to mention in the redirection. Moreover hashslot should
 * be set to the hash slot that caused the redirection. */
/**
 * 根据应该设置为 CLUSTER_REDIR_ 宏之一的 error_code，向客户端发送正确的重定向代码。
 * 如果使用了 CLUSTER_REDIR_ASK 或 CLUSTER_REDIR_MOVED 错误代码，那么节点 'n' 不应为 NULL，
 * 而应该是我们要在重定向中提及的节点。此外，hashslot 应该设置为导致重定向的哈希槽。
 * */
void clusterRedirectClient(client *c, clusterNode *n, int hashslot, int error_code) {
    if (error_code == CLUSTER_REDIR_CROSS_SLOT) {
        addReplyError(c,"-CROSSSLOT Keys in request don't hash to the same slot");
    } else if (error_code == CLUSTER_REDIR_UNSTABLE) {
        /* The request spawns multiple keys in the same slot,
         * but the slot is not "stable" currently as there is
         * a migration or import in progress. */
        //该请求在同一个槽中生成多个键，但该槽当前不是“稳定的”，因为正在进行迁移或导入。
        addReplyError(c,"-TRYAGAIN Multiple keys request during rehashing of slot");
    } else if (error_code == CLUSTER_REDIR_DOWN_STATE) {
        addReplyError(c,"-CLUSTERDOWN The cluster is down");
    } else if (error_code == CLUSTER_REDIR_DOWN_RO_STATE) {
        addReplyError(c,"-CLUSTERDOWN The cluster is down and only accepts read commands");
    } else if (error_code == CLUSTER_REDIR_DOWN_UNBOUND) {
        addReplyError(c,"-CLUSTERDOWN Hash slot not served");
    } else if (error_code == CLUSTER_REDIR_MOVED ||
               error_code == CLUSTER_REDIR_ASK)
    {
        /* Redirect to IP:port. Include plaintext port if cluster is TLS but
         * client is non-TLS. */
        //重定向到 IP：端口。如果集群是 TLS 但客户端是非 TLS，则包括明文端口。
        int use_pport = (server.tls_cluster &&
                        c->conn && connGetType(c->conn) != CONN_TYPE_TLS);
        int port = use_pport && n->pport ? n->pport : n->port;
        addReplyErrorSds(c,sdscatprintf(sdsempty(),
            "-%s %d %s:%d",
            (error_code == CLUSTER_REDIR_ASK) ? "ASK" : "MOVED",
            hashslot, getPreferredEndpoint(n), port));
    } else {
        serverPanic("getNodeByQuery() unknown error.");
    }
}

/* This function is called by the function processing clients incrementally
 * to detect timeouts, in order to handle the following case:
 *
 * 1) A client blocks with BLPOP or similar blocking operation.
 * 2) The master migrates the hash slot elsewhere or turns into a slave.
 * 3) The client may remain blocked forever (or up to the max timeout time)
 *    waiting for a key change that will never happen.
 *
 * If the client is found to be blocked into a hash slot this node no
 * longer handles, the client is sent a redirection error, and the function
 * returns 1. Otherwise 0 is returned and no operation is performed. */
/**
 * 该函数由增量处理客户端的函数调用以检测超时，以处理以下情况：
 *   1) 客户端使用 BLPOP 或类似的阻塞操作阻塞。
 *   2）master将hash slot迁移到别处或变成slave。
 *   3) 客户端可能会永远保持阻塞状态（或直到最大超时时间），等待永远不会发生的密钥更改。
 * 如果发现客户端被阻塞到该节点不再处理的哈希槽中，则向客户端发送重定向错误，并且该函数返回1。
 * 否则返回0并且不执行任何操作。
 * */
int clusterRedirectBlockedClientIfNeeded(client *c) {
    if (c->flags & CLIENT_BLOCKED &&
        (c->btype == BLOCKED_LIST ||
         c->btype == BLOCKED_ZSET ||
         c->btype == BLOCKED_STREAM ||
         c->btype == BLOCKED_MODULE))
    {
        dictEntry *de;
        dictIterator *di;

        /* If the cluster is down, unblock the client with the right error.
         * If the cluster is configured to allow reads on cluster down, we
         * still want to emit this error since a write will be required
         * to unblock them which may never come.  */
        /**
         * 如果集群已关闭，请使用正确的错误解除对客户端的阻止。
         * 如果集群配置为允许在集群关闭时进行读取，我们仍然希望发出此错误，
         * 因为需要写入来解除阻塞，而这可能永远不会发生。
         * */
        if (server.cluster->state == CLUSTER_FAIL) {
            clusterRedirectClient(c,NULL,0,CLUSTER_REDIR_DOWN_STATE);
            return 1;
        }

        /* If the client is blocked on module, but ont on a specific key,
         * don't unblock it (except for the CLSUTER_FAIL case above). */
        //如果客户端在模块上被阻止，但不是在特定键上，请不要取消阻止它（除了上面的 CLSUTER_FAIL 情况）。
        if (c->btype == BLOCKED_MODULE && !moduleClientIsBlockedOnKeys(c))
            return 0;

        /* All keys must belong to the same slot, so check first key only. */
        //所有的键必须属于同一个槽，所以只检查第一个键。
        di = dictGetIterator(c->bpop.keys);
        if ((de = dictNext(di)) != NULL) {
            robj *key = dictGetKey(de);
            int slot = keyHashSlot((char*)key->ptr, sdslen(key->ptr));
            clusterNode *node = server.cluster->slots[slot];

            /* if the client is read-only and attempting to access key that our
             * replica can handle, allow it. */
            //如果客户端是只读的并试图访问我们的副本可以处理的密钥，请允许它。
            if ((c->flags & CLIENT_READONLY) &&
                !(c->lastcmd->flags & CMD_WRITE) &&
                nodeIsSlave(myself) && myself->slaveof == node)
            {
                node = myself;
            }

            /* We send an error and unblock the client if:
             * 1) The slot is unassigned, emitting a cluster down error.
             * 2) The slot is not handled by this node, nor being imported. */
            /**
             * 如果出现以下情况，我们将发送错误并解除对客户端的阻塞：
             *   1) 插槽未分配，发出集群关闭错误。
             *   2) slot 不是由这个节点处理的，也不是被导入的。
             * */
            if (node != myself &&
                server.cluster->importing_slots_from[slot] == NULL)
            {
                if (node == NULL) {
                    clusterRedirectClient(c,NULL,0,
                        CLUSTER_REDIR_DOWN_UNBOUND);
                } else {
                    clusterRedirectClient(c,node,slot,
                        CLUSTER_REDIR_MOVED);
                }
                dictReleaseIterator(di);
                return 1;
            }
        }
        dictReleaseIterator(di);
    }
    return 0;
}

/* Slot to Key API. This is used by Redis Cluster in order to obtain in
 * a fast way a key that belongs to a specified hash slot. This is useful
 * while rehashing the cluster and in other conditions when we need to
 * understand if we have keys for a given hash slot.
 * 插槽到密钥 API。 Redis Cluster 使用它来快速获取属于指定哈希槽的密钥。
 * 当我们需要了解我们是否有给定哈希槽的键时，这在重新散列集群和其他情况下很有用。*/

void slotToKeyAddEntry(dictEntry *entry, redisDb *db) {
    sds key = entry->key;
    unsigned int hashslot = keyHashSlot(key, sdslen(key));
    slotToKeys *slot_to_keys = &(*db->slots_to_keys).by_slot[hashslot];
    slot_to_keys->count++;

    /* Insert entry before the first element in the list. */
    //在列表中的第一个元素之前插入条目。
    dictEntry *first = slot_to_keys->head;
    dictEntryNextInSlot(entry) = first;
    if (first != NULL) {
        serverAssert(dictEntryPrevInSlot(first) == NULL);
        dictEntryPrevInSlot(first) = entry;
    }
    serverAssert(dictEntryPrevInSlot(entry) == NULL);
    slot_to_keys->head = entry;
}

void slotToKeyDelEntry(dictEntry *entry, redisDb *db) {
    sds key = entry->key;
    unsigned int hashslot = keyHashSlot(key, sdslen(key));
    slotToKeys *slot_to_keys = &(*db->slots_to_keys).by_slot[hashslot];
    slot_to_keys->count--;

    /* Connect previous and next entries to each other. */
    //将上一个和下一个条目相互连接。
    dictEntry *next = dictEntryNextInSlot(entry);
    dictEntry *prev = dictEntryPrevInSlot(entry);
    if (next != NULL) {
        dictEntryPrevInSlot(next) = prev;
    }
    if (prev != NULL) {
        dictEntryNextInSlot(prev) = next;
    } else {
        /* The removed entry was the first in the list. */
        //删除的条目是列表中的第一个。
        serverAssert(slot_to_keys->head == entry);
        slot_to_keys->head = next;
    }
}

/* Updates neighbour entries when an entry has been replaced (e.g. reallocated
 * during active defrag). */
//当条目被替换时更新邻居条目（例如，在活动碎片整理期间重新分配）。
void slotToKeyReplaceEntry(dictEntry *entry, redisDb *db) {
    dictEntry *next = dictEntryNextInSlot(entry);
    dictEntry *prev = dictEntryPrevInSlot(entry);
    if (next != NULL) {
        dictEntryPrevInSlot(next) = entry;
    }
    if (prev != NULL) {
        dictEntryNextInSlot(prev) = entry;
    } else {
        /* The replaced entry was the first in the list. */
        //被替换的条目是列表中的第一个。
        sds key = entry->key;
        unsigned int hashslot = keyHashSlot(key, sdslen(key));
        slotToKeys *slot_to_keys = &(*db->slots_to_keys).by_slot[hashslot];
        slot_to_keys->head = entry;
    }
}

/* Initialize slots-keys map of given db. */
//初始化给定数据库的槽键映射。
void slotToKeyInit(redisDb *db) {
    db->slots_to_keys = zcalloc(sizeof(clusterSlotToKeyMapping));
}

/* Empty slots-keys map of given db. */
//给定数据库的空槽键映射。
void slotToKeyFlush(redisDb *db) {
    memset(db->slots_to_keys, 0,
        sizeof(clusterSlotToKeyMapping));
}

/* Free slots-keys map of given db. */
//给定数据库的空闲槽键映射。
void slotToKeyDestroy(redisDb *db) {
    zfree(db->slots_to_keys);
    db->slots_to_keys = NULL;
}

/* Remove all the keys in the specified hash slot.
 * The number of removed items is returned. */
//删除指定哈希槽中的所有键。返回删除的项目数。
unsigned int delKeysInSlot(unsigned int hashslot) {
    unsigned int j = 0;
    dictEntry *de = (*server.db->slots_to_keys).by_slot[hashslot].head;
    while (de != NULL) {
        sds sdskey = dictGetKey(de);
        de = dictEntryNextInSlot(de);
        robj *key = createStringObject(sdskey, sdslen(sdskey));
        dbDelete(&server.db[0], key);
        decrRefCount(key);
        j++;
    }
    return j;
}

unsigned int countKeysInSlot(unsigned int hashslot) {
    return (*server.db->slots_to_keys).by_slot[hashslot].count;
}

/* -----------------------------------------------------------------------------
 * Operation(s) on channel rax tree. 通道 rax 树上的操作。
 * -------------------------------------------------------------------------- */

void slotToChannelUpdate(sds channel, int add) {
    size_t keylen = sdslen(channel);
    unsigned int hashslot = keyHashSlot(channel,keylen);
    unsigned char buf[64];
    unsigned char *indexed = buf;

    if (keylen+2 > 64) indexed = zmalloc(keylen+2);
    indexed[0] = (hashslot >> 8) & 0xff;
    indexed[1] = hashslot & 0xff;
    memcpy(indexed+2,channel,keylen);
    if (add) {
        raxInsert(server.cluster->slots_to_channels,indexed,keylen+2,NULL,NULL);
    } else {
        raxRemove(server.cluster->slots_to_channels,indexed,keylen+2,NULL);
    }
    if (indexed != buf) zfree(indexed);
}

void slotToChannelAdd(sds channel) {
    slotToChannelUpdate(channel,1);
}

void slotToChannelDel(sds channel) {
    slotToChannelUpdate(channel,0);
}

/* Get the count of the channels for a given slot. */
//获取给定插槽的通道数。
unsigned int countChannelsInSlot(unsigned int hashslot) {
    raxIterator iter;
    int j = 0;
    unsigned char indexed[2];

    indexed[0] = (hashslot >> 8) & 0xff;
    indexed[1] = hashslot & 0xff;
    raxStart(&iter,server.cluster->slots_to_channels);
    raxSeek(&iter,">=",indexed,2);
    while(raxNext(&iter)) {
        if (iter.key[0] != indexed[0] || iter.key[1] != indexed[1]) break;
        j++;
    }
    raxStop(&iter);
    return j;
}
