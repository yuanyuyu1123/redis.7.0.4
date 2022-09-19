/* Asynchronous replication implementation.
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
#include "bio.h"
#include "functions.h"

#include <memory.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>

void replicationDiscardCachedMaster(void);
void replicationResurrectCachedMaster(connection *conn);
void replicationSendAck(void);
void replicaPutOnline(client *slave);
void replicaStartCommandStream(client *slave);
int cancelReplicationHandshake(int reconnect);

/* We take a global flag to remember if this instance generated an RDB
 * because of replication, so that we can remove the RDB file in case
 * the instance is configured to have no persistence. */
//我们使用一个全局标志来记住此实例是否因为复制而生成了 RDB，以便我们可以删除 RDB 文件以防实例配置为没有持久性。
int RDBGeneratedByReplication = 0;

/* --------------------------- Utility functions ---------------------------- */

/* Return the pointer to a string representing the slave ip:listening_port
 * pair. Mostly useful for logging, since we want to log a slave using its
 * IP address and its listening port which is more clear for the user, for
 * example: "Closing connection with replica 10.1.2.3:6380". */
/**返回指向表示从属 ip:listening_port 对的字符串的指针。
 * 主要用于日志记录，因为我们想使用其 IP 地址和侦听端口记录从站，这对用户来说更清楚，
 * 例如：“关闭与副本 10.1.2.3:6380 的连接”。*/
char *replicationGetSlaveName(client *c) {
    static char buf[NET_HOST_PORT_STR_LEN];
    char ip[NET_IP_STR_LEN];

    ip[0] = '\0';
    buf[0] = '\0';
    if (c->slave_addr ||
        connPeerToString(c->conn,ip,sizeof(ip),NULL) != -1)
    {
        char *addr = c->slave_addr ? c->slave_addr : ip;
        if (c->slave_listening_port)
            anetFormatAddr(buf,sizeof(buf),addr,c->slave_listening_port);
        else
            snprintf(buf,sizeof(buf),"%s:<unknown-replica-port>",addr);
    } else {
        snprintf(buf,sizeof(buf),"client id #%llu",
            (unsigned long long) c->id);
    }
    return buf;
}

/* Plain unlink() can block for quite some time in order to actually apply
 * the file deletion to the filesystem. This call removes the file in a
 * background thread instead. We actually just do close() in the thread,
 * by using the fact that if there is another instance of the same file open,
 * the foreground unlink() will only remove the fs name, and deleting the
 * file's storage space will only happen once the last reference is lost. */
/**为了将文件删除实际应用到文件系统，普通的 unlink() 可以阻塞相当长的一段时间。此调用改为在后台线程中删除文件。
 * 我们实际上只是在线程中做 close()，通过使用这样一个事实，即如果打开了同一个文件的另一个实例，
 * 前台 unlink() 只会删除 fs 名称，并且删除文件的存储空间只会发生一次最后参考丢失。*/
int bg_unlink(const char *filename) {
    int fd = open(filename,O_RDONLY|O_NONBLOCK);
    if (fd == -1) {
        /* Can't open the file? Fall back to unlinking in the main thread. */
        /**无法打开文件？回退到在主线程中取消链接。*/
        return unlink(filename);
    } else {
        /* The following unlink() removes the name but doesn't free the
         * file contents because a process still has it open. */
        /**以下 unlink() 删除名称但不释放文件内容，因为进程仍然打开它。*/
        int retval = unlink(filename);
        if (retval == -1) {
            /* If we got an unlink error, we just return it, closing the
             * new reference we have to the file. */
            /**如果我们得到一个 unlink 错误，我们只是返回它，关闭我们对文件的新引用。*/
            int old_errno = errno;
            close(fd);  /* This would overwrite our errno. So we saved it. 这将覆盖我们的 errno。所以我们保存了它。*/
            errno = old_errno;
            return -1;
        }
        bioCreateCloseJob(fd);
        return 0; /* Success. */
    }
}

/* ---------------------------------- MASTER -------------------------------- */

void createReplicationBacklog(void) {
    serverAssert(server.repl_backlog == NULL);
    server.repl_backlog = zmalloc(sizeof(replBacklog));
    server.repl_backlog->ref_repl_buf_node = NULL;
    server.repl_backlog->unindexed_count = 0;
    server.repl_backlog->blocks_index = raxNew();
    server.repl_backlog->histlen = 0;
    /* We don't have any data inside our buffer, but virtually the first
     * byte we have is the next byte that will be generated for the
     * replication stream. */
    //我们的缓冲区中没有任何数据，但实际上我们拥有的第一个字节是为复制流生成的下一个字节。
    server.repl_backlog->offset = server.master_repl_offset+1;
}

/* This function is called when the user modifies the replication backlog
 * size at runtime. It is up to the function to resize the buffer and setup it
 * so that it contains the same data as the previous one (possibly less data,
 * but the most recent bytes, or the same data and more free space in case the
 * buffer is enlarged). */
/**当用户在运行时修改复制积压大小时调用此函数。由函数来调整缓冲区的大小并对其进行设置，
 * 使其包含与前一个相同的数据（可能更少的数据，但最新的字节，或者相同的数据和更多的可用空间，以防缓冲区扩大）*/
void resizeReplicationBacklog(void) {
    if (server.repl_backlog_size < CONFIG_REPL_BACKLOG_MIN_SIZE)
        server.repl_backlog_size = CONFIG_REPL_BACKLOG_MIN_SIZE;
    if (server.repl_backlog)
        incrementalTrimReplicationBacklog(REPL_BACKLOG_TRIM_BLOCKS_PER_CALL);
}

void freeReplicationBacklog(void) {
    serverAssert(listLength(server.slaves) == 0);
    if (server.repl_backlog == NULL) return;

    /* Decrease the start buffer node reference count. 减少起始缓冲区节点引用计数。*/
    if (server.repl_backlog->ref_repl_buf_node) {
        replBufBlock *o = listNodeValue(
            server.repl_backlog->ref_repl_buf_node);
        serverAssert(o->refcount == 1); /* Last reference. */
        o->refcount--;
    }

    /* Replication buffer blocks are completely released when we free the
     * backlog, since the backlog is released only when there are no replicas
     * and the backlog keeps the last reference of all blocks. */
    /**当我们释放积压时，复制缓冲区块被完全释放，因为只有在没有副本并且积压保留所有块的最后引用时才释放积压。*/
    freeReplicationBacklogRefMemAsync(server.repl_buffer_blocks,
                            server.repl_backlog->blocks_index);
    resetReplicationBuffer();
    zfree(server.repl_backlog);
    server.repl_backlog = NULL;
}

/* To make search offset from replication buffer blocks quickly
 * when replicas ask partial resynchronization, we create one index
 * block every REPL_BACKLOG_INDEX_PER_BLOCKS blocks. */
/**为了在副本请求部分重新同步时快速从复制缓冲区块中搜索偏移量，我们每个 REPL_BACKLOG_INDEX_PER_BLOCKS 块创建一个索引块。*/
void createReplicationBacklogIndex(listNode *ln) {
    server.repl_backlog->unindexed_count++;
    if (server.repl_backlog->unindexed_count >= REPL_BACKLOG_INDEX_PER_BLOCKS) {
        replBufBlock *o = listNodeValue(ln);
        uint64_t encoded_offset = htonu64(o->repl_offset);
        raxInsert(server.repl_backlog->blocks_index,
                  (unsigned char*)&encoded_offset, sizeof(uint64_t),
                  ln, NULL);
        server.repl_backlog->unindexed_count = 0;
    }
}

/* Rebase replication buffer blocks' offset since the initial
 * setting offset starts from 0 when master restart. */
//重新设置复制缓冲区块的偏移量，因为初始设置偏移量在主重启时从 0 开始。
void rebaseReplicationBuffer(long long base_repl_offset) {
    raxFree(server.repl_backlog->blocks_index);
    server.repl_backlog->blocks_index = raxNew();
    server.repl_backlog->unindexed_count = 0;

    listIter li;
    listNode *ln;
    listRewind(server.repl_buffer_blocks, &li);
    while ((ln = listNext(&li))) {
        replBufBlock *o = listNodeValue(ln);
        o->repl_offset += base_repl_offset;
        createReplicationBacklogIndex(ln);
    }
}

void resetReplicationBuffer(void) {
    server.repl_buffer_mem = 0;
    server.repl_buffer_blocks = listCreate();
    listSetFreeMethod(server.repl_buffer_blocks, (void (*)(void*))zfree);
}

int canFeedReplicaReplBuffer(client *replica) {
    /* Don't feed replicas that only want the RDB. */
    /**不要提供只需要 RDB 的副本。*/
    if (replica->flags & CLIENT_REPL_RDBONLY) return 0;

    /* Don't feed replicas that are still waiting for BGSAVE to start. */
    /**不要提供仍在等待 BGSAVE 启动的副本。*/
    if (replica->replstate == SLAVE_STATE_WAIT_BGSAVE_START) return 0;

    return 1;
}

/* Similar with 'prepareClientToWrite', note that we must call this function
 * before feeding replication stream into global replication buffer, since
 * clientHasPendingReplies in prepareClientToWrite will access the global
 * replication buffer to make judgements. */
/**与 'prepareClientToWrite' 类似，注意我们必须在将复制流馈送到全局复制缓冲区之前调用此函数，
 * 因为 prepareClientToWrite 中的 clientHasPendingReplies 将访问全局复制缓冲区进行判断。*/
int prepareReplicasToWrite(void) {
    listIter li;
    listNode *ln;
    int prepared = 0;

    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;
        if (!canFeedReplicaReplBuffer(slave)) continue;
        if (prepareClientToWrite(slave) == C_ERR) continue;
        prepared++;
    }

    return prepared;
}

/* Wrapper for feedReplicationBuffer() that takes Redis string objects
 * as input. */
//feedReplicationBuffer() 的包装器，它将 Redis 字符串对象作为输入。
void feedReplicationBufferWithObject(robj *o) {
    char llstr[LONG_STR_SIZE];
    void *p;
    size_t len;

    if (o->encoding == OBJ_ENCODING_INT) {
        len = ll2string(llstr,sizeof(llstr),(long)o->ptr);
        p = llstr;
    } else {
        len = sdslen(o->ptr);
        p = o->ptr;
    }
    feedReplicationBuffer(p,len);
}

/* Generally, we only have one replication buffer block to trim when replication
 * backlog size exceeds our setting and no replica reference it. But if replica
 * clients disconnect, we need to free many replication buffer blocks that are
 * referenced. It would cost much time if there are a lots blocks to free, that
 * will freeze server, so we trim replication backlog incrementally. */
/**通常，当复制积压大小超过我们的设置并且没有副本引用它时，我们只有一个复制缓冲区块需要修剪。
 * 但是如果副本客户端断开连接，我们需要释放许多引用的复制缓冲区块。
 * 如果有很多块要释放会花费很多时间，这会冻结服务器，所以我们逐步减少复制积压。*/
void incrementalTrimReplicationBacklog(size_t max_blocks) {
    serverAssert(server.repl_backlog != NULL);

    size_t trimmed_blocks = 0;
    while (server.repl_backlog->histlen > server.repl_backlog_size &&
           trimmed_blocks < max_blocks)
    {
        /* We never trim backlog to less than one block. */
        //我们从不将积压工作减少到少于一个块。
        if (listLength(server.repl_buffer_blocks) <= 1) break;

        /* Replicas increment the refcount of the first replication buffer block
         * they refer to, in that case, we don't trim the backlog even if
         * backlog_histlen exceeds backlog_size. This implicitly makes backlog
         * bigger than our setting, but makes the master accept partial resync as
         * much as possible. So that backlog must be the last reference of
         * replication buffer blocks. */
        /**副本会增加它们引用的第一个复制缓冲区块的引用计数，在这种情况下，即使 backlog_histlen 超过 backlog_size，
         * 我们也不会修剪 backlog。这隐含地使 backlog 大于我们的设置，但使 master 尽可能地接受部分重新同步。
         * 因此，积压必须是复制缓冲区块的最后引用。*/
        listNode *first = listFirst(server.repl_buffer_blocks);
        serverAssert(first == server.repl_backlog->ref_repl_buf_node);
        replBufBlock *fo = listNodeValue(first);
        if (fo->refcount != 1) break;

        /* We don't try trim backlog if backlog valid size will be lessen than
         * setting backlog size once we release the first repl buffer block. */
        //如果我们释放第一个 repl 缓冲区块后积压有效大小将小于设置积压大小，我们不会尝试修剪积压。
        if (server.repl_backlog->histlen - (long long)fo->size <=
            server.repl_backlog_size) break;

        /* Decr refcount and release the first block later. */
        //Decr refcount 并稍后释放第一个块。
        fo->refcount--;
        trimmed_blocks++;
        server.repl_backlog->histlen -= fo->size;

        /* Go to use next replication buffer block node. */
        /**转到使用下一个复制缓冲区块节点。*/
        listNode *next = listNextNode(first);
        server.repl_backlog->ref_repl_buf_node = next;
        serverAssert(server.repl_backlog->ref_repl_buf_node != NULL);
        /* Incr reference count to keep the new head node. */
        /**增加引用计数以保留新的头节点。*/
        ((replBufBlock *)listNodeValue(next))->refcount++;

        /* Remove the node in recorded blocks. */
        /**删除记录块中的节点。*/
        uint64_t encoded_offset = htonu64(fo->repl_offset);
        raxRemove(server.repl_backlog->blocks_index,
            (unsigned char*)&encoded_offset, sizeof(uint64_t), NULL);

        /* Delete the first node from global replication buffer. */
        /**从全局复制缓冲区中删除第一个节点。*/
        serverAssert(fo->refcount == 0 && fo->used == fo->size);
        server.repl_buffer_mem -= (fo->size +
            sizeof(listNode) + sizeof(replBufBlock));
        listDelNode(server.repl_buffer_blocks, first);
    }

    /* Set the offset of the first byte we have in the backlog. */
    /**设置我们在 backlog 中的第一个字节的偏移量。*/
    server.repl_backlog->offset = server.master_repl_offset -
                              server.repl_backlog->histlen + 1;
}

/* Free replication buffer blocks that are referenced by this client. */
//此客户端引用的空闲复制缓冲区块。
void freeReplicaReferencedReplBuffer(client *replica) {
    if (replica->ref_repl_buf_node != NULL) {
        /* Decrease the start buffer node reference count. */
        replBufBlock *o = listNodeValue(replica->ref_repl_buf_node);
        serverAssert(o->refcount > 0);
        o->refcount--;
        incrementalTrimReplicationBacklog(REPL_BACKLOG_TRIM_BLOCKS_PER_CALL);
    }
    replica->ref_repl_buf_node = NULL;
    replica->ref_block_pos = 0;
}

/* Append bytes into the global replication buffer list, replication backlog and
 * all replica clients use replication buffers collectively, this function replace
 * 'addReply*', 'feedReplicationBacklog' for replicas and replication backlog,
 * First we add buffer into global replication buffer block list, and then
 * update replica / replication-backlog referenced node and block position. */
/**将字节追加到全局复制缓冲区列表中，复制积压和所有副本客户端共同使用复制缓冲区，
 * 该函数替换'addReply'，'feedReplicationBacklog'为副本和复制积压，首先我们将缓冲区添加到全局复制缓冲区块列表中，
 * 然后更新副本复制积压引用的节点和块位置。*/
void feedReplicationBuffer(char *s, size_t len) {
    static long long repl_block_id = 0;

    if (server.repl_backlog == NULL) return;
    server.master_repl_offset += len;
    server.repl_backlog->histlen += len;

    size_t start_pos = 0; /* The position of referenced block to start sending. 开始发送的引用块的位置。*/
    listNode *start_node = NULL; /* Replica/backlog starts referenced node. Replica/backlog 启动引用的节点。*/
    int add_new_block = 0; /* Create new block if current block is total used. 如果当前块已全部使用，则创建新块。*/
    listNode *ln = listLast(server.repl_buffer_blocks);
    replBufBlock *tail = ln ? listNodeValue(ln) : NULL;

    /* Append to tail string when possible. 尽可能附加到尾字符串。*/
    if (tail && tail->size > tail->used) {
        start_node = listLast(server.repl_buffer_blocks);
        start_pos = tail->used;
        /* Copy the part we can fit into the tail, and leave the rest for a
         * new node */
        /**复制我们可以放入尾部的部分，并将其余部分留给新节点*/
        size_t avail = tail->size - tail->used;
        size_t copy = (avail >= len) ? len : avail;
        memcpy(tail->buf + tail->used, s, copy);
        tail->used += copy;
        s += copy;
        len -= copy;
    }
    if (len) {
        /* Create a new node, make sure it is allocated to at
         * least PROTO_REPLY_CHUNK_BYTES */
        /**创建一个新节点，确保它至少分配给 PROTO_REPLY_CHUNK_BYTES*/
        size_t usable_size;
        size_t size = (len < PROTO_REPLY_CHUNK_BYTES) ? PROTO_REPLY_CHUNK_BYTES : len;
        tail = zmalloc_usable(size + sizeof(replBufBlock), &usable_size);
        /* Take over the allocation's internal fragmentation */
        /**接管分配的内部碎片*/
        tail->size = usable_size - sizeof(replBufBlock);
        tail->used = len;
        tail->refcount = 0;
        tail->repl_offset = server.master_repl_offset - tail->used + 1;
        tail->id = repl_block_id++;
        memcpy(tail->buf, s, len);
        listAddNodeTail(server.repl_buffer_blocks, tail);
        /* We also count the list node memory into replication buffer memory. */
        /**我们还将列表节点内存计入复制缓冲内存。*/
        server.repl_buffer_mem += (usable_size + sizeof(listNode));
        add_new_block = 1;
        if (start_node == NULL) {
            start_node = listLast(server.repl_buffer_blocks);
            start_pos = 0;
        }
    }

    /* For output buffer of replicas.用于副本的输出缓冲区。 */
    listIter li;
    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;
        if (!canFeedReplicaReplBuffer(slave)) continue;

        /* Update shared replication buffer start position. 更新共享复制缓冲区的开始位置。*/
        if (slave->ref_repl_buf_node == NULL) {
            slave->ref_repl_buf_node = start_node;
            slave->ref_block_pos = start_pos;
            /* Only increase the start block reference count. 只增加起始块引用计数。*/
            ((replBufBlock *)listNodeValue(start_node))->refcount++;
        }

        /* Check output buffer limit only when add new block. 仅在添加新块时检查输出缓冲区限制。*/
        if (add_new_block) closeClientOnOutputBufferLimitReached(slave, 1);
    }

    /* For replication backlog 对于复制积压*/
    if (server.repl_backlog->ref_repl_buf_node == NULL) {
        server.repl_backlog->ref_repl_buf_node = start_node;
        /* Only increase the start block reference count. 只增加起始块引用计数。*/
        ((replBufBlock *)listNodeValue(start_node))->refcount++;

        /* Replication buffer must be empty before adding replication stream
         * into replication backlog. */
        /**在将复制流添加到复制积压中之前，复制缓冲区必须为空。*/
        serverAssert(add_new_block == 1 && start_pos == 0);
    }
    if (add_new_block) {
        createReplicationBacklogIndex(listLast(server.repl_buffer_blocks));
    }
    /* Try to trim replication backlog since replication backlog may exceed
     * our setting when we add replication stream. Note that it is important to
     * try to trim at least one node since in the common case this is where one
     * new backlog node is added and one should be removed. See also comments
     * in freeMemoryGetNotCountedMemory for details. */
    /**尝试修剪复制积压，因为当我们添加复制流时复制积压可能会超过我们的设置。
     * 请注意，尝试修剪至少一个节点很重要，因为在常见情况下，这是添加一个新的积压节点并应删除一个。
     * 有关详细信息，另请参阅 freeMemoryGetNotCountedMemory 中的注释。*/
    incrementalTrimReplicationBacklog(REPL_BACKLOG_TRIM_BLOCKS_PER_CALL);
}

/* Propagate write commands to replication stream.
 *
 * This function is used if the instance is a master: we use the commands
 * received by our clients in order to create the replication stream.
 * Instead if the instance is a replica and has sub-replicas attached, we use
 * replicationFeedStreamFromMasterStream() */
/**将写入命令传播到复制流。
 *
 * 如果实例是主实例，则使用此函数：我们使用客户端收到的命令来创建复制流。
 * 相反，如果实例是副本并且附加了子副本，我们使用 replicationFeedStreamFromMasterStream() */
void replicationFeedSlaves(list *slaves, int dictid, robj **argv, int argc) {
    int j, len;
    char llstr[LONG_STR_SIZE];

    /* In case we propagate a command that doesn't touch keys (PING, REPLCONF) we
     * pass dbid=server.slaveseldb which may be -1. */
    /**如果我们传播一个不接触键的命令（PING、REPLCONF），我们会传递 dbid=server.slaveseldb，它可能是 -1。*/
    serverAssert(dictid == -1 || (dictid >= 0 && dictid < server.dbnum));

    /* If the instance is not a top level master, return ASAP: we'll just proxy
     * the stream of data we receive from our master instead, in order to
     * propagate *identical* replication stream. In this way this slave can
     * advertise the same replication ID as the master (since it shares the
     * master replication history and has the same backlog and offsets). */
    /**如果实例不是顶级主控，请尽快返回：我们将只代理从主控接收到的数据流，以便传播相同的复制流。
     * 通过这种方式，这个从服务器可以通告与主服务器相同的复制 ID（因为它共享主服务器复制历史并具有相同的积压和偏移量）。*/
    if (server.masterhost != NULL) return;

    /* If there aren't slaves, and there is no backlog buffer to populate,
     * we can return ASAP. */
    /**如果没有从站，并且没有要填充的积压缓冲区，我们可以尽快返回。*/
    if (server.repl_backlog == NULL && listLength(slaves) == 0) return;

    /* We can't have slaves attached and no backlog. */
    /**我们不能附加slave并且没有积压。*/
    serverAssert(!(listLength(slaves) != 0 && server.repl_backlog == NULL));

    /* Must install write handler for all replicas first before feeding
     * replication stream. */
    /**在提供复制流之前，必须先为所有副本安装写处理程序。*/
    prepareReplicasToWrite();

    /* Send SELECT command to every slave if needed. */
    /**如果需要，向每个从站发送 SELECT 命令。*/
    if (server.slaveseldb != dictid) {
        robj *selectcmd;

        /* For a few DBs we have pre-computed SELECT command. */
        /**对于一些数据库，我们预先计算了 SELECT 命令。*/
        if (dictid >= 0 && dictid < PROTO_SHARED_SELECT_CMDS) {
            selectcmd = shared.select[dictid];
        } else {
            int dictid_len;

            dictid_len = ll2string(llstr,sizeof(llstr),dictid);
            selectcmd = createObject(OBJ_STRING,
                sdscatprintf(sdsempty(),
                "*2\r\n$6\r\nSELECT\r\n$%d\r\n%s\r\n",
                dictid_len, llstr));
        }

        feedReplicationBufferWithObject(selectcmd);

        if (dictid < 0 || dictid >= PROTO_SHARED_SELECT_CMDS)
            decrRefCount(selectcmd);

        server.slaveseldb = dictid;
    }

    /* Write the command to the replication buffer if any. */
    /**将命令写入复制缓冲区（如果有）。*/
    char aux[LONG_STR_SIZE+3];

    /* Add the multi bulk reply length. 添加多批量回复长度。*/
    aux[0] = '*';
    len = ll2string(aux+1,sizeof(aux)-1,argc);
    aux[len+1] = '\r';
    aux[len+2] = '\n';
    feedReplicationBuffer(aux,len+3);

    for (j = 0; j < argc; j++) {
        long objlen = stringObjectLen(argv[j]);

        /* We need to feed the buffer with the object as a bulk reply
         * not just as a plain string, so create the $..CRLF payload len
         * and add the final CRLF */
        /**我们需要将对象作为批量回复提供给缓冲区，而不仅仅是作为纯字符串，
         * 因此创建 ..CRLF 有效负载 len 并添加最终的 CRLF*/
        aux[0] = '$';
        len = ll2string(aux+1,sizeof(aux)-1,objlen);
        aux[len+1] = '\r';
        aux[len+2] = '\n';
        feedReplicationBuffer(aux,len+3);
        feedReplicationBufferWithObject(argv[j]);
        feedReplicationBuffer(aux+len+1,2);
    }
}

/* This is a debugging function that gets called when we detect something
 * wrong with the replication protocol: the goal is to peek into the
 * replication backlog and show a few final bytes to make simpler to
 * guess what kind of bug it could be. */
/**这是一个调试函数，当我们检测到复制协议有问题时会调用它：目标是查看复制积压并显示一些最终字节，
 * 以便更容易猜测它可能是哪种错误。*/
void showLatestBacklog(void) {
    if (server.repl_backlog == NULL) return;
    if (listLength(server.repl_buffer_blocks) == 0) return;

    size_t dumplen = 256;
    if (server.repl_backlog->histlen < (long long)dumplen)
        dumplen = server.repl_backlog->histlen;

    sds dump = sdsempty();
    listNode *node = listLast(server.repl_buffer_blocks);
    while(dumplen) {
        if (node == NULL) break;
        replBufBlock *o = listNodeValue(node);
        size_t thislen = o->used >= dumplen ? dumplen : o->used;
        sds head = sdscatrepr(sdsempty(), o->buf+o->used-thislen, thislen);
        sds tmp = sdscatsds(head, dump);
        sdsfree(dump);
        dump = tmp;
        dumplen -= thislen;
        node = listPrevNode(node);
    }

    /* Finally log such bytes: this is vital debugging info to
     * understand what happened. */
    /**最后记录这些字节：这是了解发生了什么的重要调试信息。*/
    serverLog(LL_WARNING,"Latest backlog is: '%s'", dump);
    sdsfree(dump);
}

/* This function is used in order to proxy what we receive from our master
 * to our sub-slaves. */
//此功能用于将我们从主服务器接收到的内容代理到我们的子从服务器。
#include <ctype.h>
void replicationFeedStreamFromMasterStream(char *buf, size_t buflen) {
    /* Debugging: this is handy to see the stream sent from master
     * to slaves. Disabled with if(0). */
    if (0) {
        printf("%zu:",buflen);
        for (size_t j = 0; j < buflen; j++) {
            printf("%c", isprint(buf[j]) ? buf[j] : '.');
        }
        printf("\n");
    }

    /* There must be replication backlog if having attached slaves. */
    /**如果附加了从属服务器，则必须有复制积压。*/
    if (listLength(server.slaves)) serverAssert(server.repl_backlog != NULL);
    if (server.repl_backlog) {
        /* Must install write handler for all replicas first before feeding
         * replication stream. */
        /**在提供复制流之前，必须先为所有副本安装写处理程序。*/
        prepareReplicasToWrite();
        feedReplicationBuffer(buf,buflen);
    }
}

void replicationFeedMonitors(client *c, list *monitors, int dictid, robj **argv, int argc) {
    /* Fast path to return if the monitors list is empty or the server is in loading. */
    /**如果监视器列表为空或服务器正在加载，则返回的快速路径。*/
    if (monitors == NULL || listLength(monitors) == 0 || server.loading) return;
    listNode *ln;
    listIter li;
    int j;
    sds cmdrepr = sdsnew("+");
    robj *cmdobj;
    struct timeval tv;

    gettimeofday(&tv,NULL);
    cmdrepr = sdscatprintf(cmdrepr,"%ld.%06ld ",(long)tv.tv_sec,(long)tv.tv_usec);
    if (c->flags & CLIENT_SCRIPT) {
        cmdrepr = sdscatprintf(cmdrepr,"[%d lua] ",dictid);
    } else if (c->flags & CLIENT_UNIX_SOCKET) {
        cmdrepr = sdscatprintf(cmdrepr,"[%d unix:%s] ",dictid,server.unixsocket);
    } else {
        cmdrepr = sdscatprintf(cmdrepr,"[%d %s] ",dictid,getClientPeerId(c));
    }

    for (j = 0; j < argc; j++) {
        if (argv[j]->encoding == OBJ_ENCODING_INT) {
            cmdrepr = sdscatprintf(cmdrepr, "\"%ld\"", (long)argv[j]->ptr);
        } else {
            cmdrepr = sdscatrepr(cmdrepr,(char*)argv[j]->ptr,
                        sdslen(argv[j]->ptr));
        }
        if (j != argc-1)
            cmdrepr = sdscatlen(cmdrepr," ",1);
    }
    cmdrepr = sdscatlen(cmdrepr,"\r\n",2);
    cmdobj = createObject(OBJ_STRING,cmdrepr);

    listRewind(monitors,&li);
    while((ln = listNext(&li))) {
        client *monitor = ln->value;
        addReply(monitor,cmdobj);
        updateClientMemUsage(c);
    }
    decrRefCount(cmdobj);
}

/* Feed the slave 'c' with the replication backlog starting from the
 * specified 'offset' up to the end of the backlog. */
//从指定的 'offset' 到 backlog 末尾的复制 backlog 提供给 slave 'c'。
long long addReplyReplicationBacklog(client *c, long long offset) {
    long long skip;

    serverLog(LL_DEBUG, "[PSYNC] Replica request offset: %lld", offset);

    if (server.repl_backlog->histlen == 0) {
        serverLog(LL_DEBUG, "[PSYNC] Backlog history len is zero");
        return 0;
    }

    serverLog(LL_DEBUG, "[PSYNC] Backlog size: %lld",
             server.repl_backlog_size);
    serverLog(LL_DEBUG, "[PSYNC] First byte: %lld",
             server.repl_backlog->offset);
    serverLog(LL_DEBUG, "[PSYNC] History len: %lld",
             server.repl_backlog->histlen);

    /* Compute the amount of bytes we need to discard. */
    skip = offset - server.repl_backlog->offset;
    serverLog(LL_DEBUG, "[PSYNC] Skipping: %lld", skip);

    /* Iterate recorded blocks, quickly search the approximate node. */
    listNode *node = NULL;
    if (raxSize(server.repl_backlog->blocks_index) > 0) {
        uint64_t encoded_offset = htonu64(offset);
        raxIterator ri;
        raxStart(&ri, server.repl_backlog->blocks_index);
        raxSeek(&ri, ">", (unsigned char*)&encoded_offset, sizeof(uint64_t));
        if (raxEOF(&ri)) {
            /* No found, so search from the last recorded node. */
            raxSeek(&ri, "$", NULL, 0);
            raxPrev(&ri);
            node = (listNode *)ri.data;
        } else {
            raxPrev(&ri); /* Skip the sought node. */
            /* We should search from the prev node since the offset of current
             * sought node exceeds searching offset. */
            /**我们应该从上一个节点开始搜索，因为当前搜索节点的偏移量超过了搜索偏移量。*/
            if (raxPrev(&ri))
                node = (listNode *)ri.data;
            else
                node = server.repl_backlog->ref_repl_buf_node;
        }
        raxStop(&ri);
    } else {
        /* No recorded blocks, just from the start node to search. */
        node = server.repl_backlog->ref_repl_buf_node;
    }

    /* Search the exact node. */
    while (node != NULL) {
        replBufBlock *o = listNodeValue(node);
        if (o->repl_offset + (long long)o->used >= offset) break;
        node = listNextNode(node);
    }
    serverAssert(node != NULL);

    /* Install a writer handler first.*/
    prepareClientToWrite(c);
    /* Setting output buffer of the replica. */
    replBufBlock *o = listNodeValue(node);
    o->refcount++;
    c->ref_repl_buf_node = node;
    c->ref_block_pos = offset - o->repl_offset;

    return server.repl_backlog->histlen - skip;
}

/* Return the offset to provide as reply to the PSYNC command received
 * from the slave. The returned value is only valid immediately after
 * the BGSAVE process started and before executing any other command
 * from clients. */
/**返回偏移量以作为对从从站接收到的 PSYNC 命令的回复。返回值仅在 BGSAVE 进程启动后和客户端执行任何其他命令之前立即有效。*/
long long getPsyncInitialOffset(void) {
    return server.master_repl_offset;
}

/* Send a FULLRESYNC reply in the specific case of a full resynchronization,
 * as a side effect setup the slave for a full sync in different ways:
 *
 * 1) Remember, into the slave client structure, the replication offset
 *    we sent here, so that if new slaves will later attach to the same
 *    background RDB saving process (by duplicating this client output
 *    buffer), we can get the right offset from this slave.
 * 2) Set the replication state of the slave to WAIT_BGSAVE_END so that
 *    we start accumulating differences from this point.
 * 3) Force the replication stream to re-emit a SELECT statement so
 *    the new slave incremental differences will start selecting the
 *    right database number.
 *
 * Normally this function should be called immediately after a successful
 * BGSAVE for replication was started, or when there is one already in
 * progress that we attached our slave to. */
/**在完全重新同步的特定情况下发送 FULLRESYNC 回复，作为副作用以不同的方式设置从属以进行完全同步：
 *  1）记住，在从属客户端结构中，我们在此处发送的复制偏移量，以便如果新的从属稍后
 *     将附加到相同的后台 RDB 保存进程（通过复制此客户端输出缓冲区），我们可以从该从站获得正确的偏移量。
 *  2）将slave的复制状态设置为WAIT_BGSAVE_END，这样我们就从这个点开始累积差异。
 *  3) 强制复制流重新发出 SELECT 语句，以便新的从属增量差异将开始选择正确的数据库编号。
 * 通常，这个函数应该在一个成功的 BGSAVE 复制开始后立即调用，或者当我们已经将我们的从属连接到一个正在进行中时。*/
int replicationSetupSlaveForFullResync(client *slave, long long offset) {
    char buf[128];
    int buflen;

    slave->psync_initial_offset = offset;
    slave->replstate = SLAVE_STATE_WAIT_BGSAVE_END;
    /* We are going to accumulate the incremental changes for this
     * slave as well. Set slaveseldb to -1 in order to force to re-emit
     * a SELECT statement in the replication stream. */
    /**我们还将为这个从站累积增量更改。将 slaveseldb 设置为 -1 以强制在复制流中重新发出 SELECT 语句。*/
    server.slaveseldb = -1;

    /* Don't send this reply to slaves that approached us with
     * the old SYNC command. */
    /**不要将此回复发送给使用旧 SYNC 命令接近我们的奴隶。*/
    if (!(slave->flags & CLIENT_PRE_PSYNC)) {
        buflen = snprintf(buf,sizeof(buf),"+FULLRESYNC %s %lld\r\n",
                          server.replid,offset);
        if (connWrite(slave->conn,buf,buflen) != buflen) {
            freeClientAsync(slave);
            return C_ERR;
        }
    }
    return C_OK;
}

/* This function handles the PSYNC command from the point of view of a
 * master receiving a request for partial resynchronization.
 *
 * On success return C_OK, otherwise C_ERR is returned and we proceed
 * with the usual full resync. */
/**该函数从接收部分再同步请求的主站的角度处理 PSYNC 命令。
 *
 * 成功返回 C_OK，否则返回 C_ERR，我们继续进行通常的完全重新同步。*/
int masterTryPartialResynchronization(client *c, long long psync_offset) {
    long long psync_len;
    char *master_replid = c->argv[1]->ptr;
    char buf[128];
    int buflen;

    /* Is the replication ID of this master the same advertised by the wannabe
     * slave via PSYNC? If the replication ID changed this master has a
     * different replication history, and there is no way to continue.
     * 这个主服务器的复制 ID 是否与想要的从服务器通过 PSYNC 通告的相同？
     * 如果复制 ID 更改，此 master 具有不同的复制历史，并且无法继续。
     *
     * Note that there are two potentially valid replication IDs: the ID1
     * and the ID2. The ID2 however is only valid up to a specific offset.
     * 请注意，有两个可能有效的复制 ID：ID1 和 ID2。然而，ID2 仅在特定偏移量内有效。*/
    if (strcasecmp(master_replid, server.replid) &&
        (strcasecmp(master_replid, server.replid2) ||
         psync_offset > server.second_replid_offset))
    {
        /* Replid "?" is used by slaves that want to force a full resync. */
        if (master_replid[0] != '?') {
            if (strcasecmp(master_replid, server.replid) &&
                strcasecmp(master_replid, server.replid2))
            {
                serverLog(LL_NOTICE,"Partial resynchronization not accepted: "
                    "Replication ID mismatch (Replica asked for '%s', my "
                    "replication IDs are '%s' and '%s')",
                    master_replid, server.replid, server.replid2);
            } else {
                serverLog(LL_NOTICE,"Partial resynchronization not accepted: "
                    "Requested offset for second ID was %lld, but I can reply "
                    "up to %lld", psync_offset, server.second_replid_offset);
            }
        } else {
            serverLog(LL_NOTICE,"Full resync requested by replica %s",
                replicationGetSlaveName(c));
        }
        goto need_full_resync;
    }

    /* We still have the data our slave is asking for? */
    if (!server.repl_backlog ||
        psync_offset < server.repl_backlog->offset ||
        psync_offset > (server.repl_backlog->offset + server.repl_backlog->histlen))
    {
        serverLog(LL_NOTICE,
            "Unable to partial resync with replica %s for lack of backlog (Replica request was: %lld).", replicationGetSlaveName(c), psync_offset);
        if (psync_offset > server.master_repl_offset) {
            serverLog(LL_WARNING,
                "Warning: replica %s tried to PSYNC with an offset that is greater than the master replication offset.", replicationGetSlaveName(c));
        }
        goto need_full_resync;
    }

    /* If we reached this point, we are able to perform a partial resync:
     * 1) Set client state to make it a slave.
     * 2) Inform the client we can continue with +CONTINUE
     * 3) Send the backlog data (from the offset to the end) to the slave. */
    /**如果我们达到了这一点，我们就可以执行部分重新同步：
     * 1) 设置客户端状态以使其成为从属。
     * 2) 通知客户端我们可以继续 +CONTINUE
     * 3) 将积压数据（从偏移量到末尾）发送到从机。*/
    c->flags |= CLIENT_SLAVE;
    c->replstate = SLAVE_STATE_ONLINE;
    c->repl_ack_time = server.unixtime;
    c->repl_start_cmd_stream_on_ack = 0;
    listAddNodeTail(server.slaves,c);
    /* We can't use the connection buffers since they are used to accumulate
     * new commands at this stage. But we are sure the socket send buffer is
     * empty so this write will never fail actually. */
    /**我们不能使用连接缓冲区，因为它们用于在此阶段累积新命令。
     * 但是我们确定套接字发送缓冲区是空的，所以这个写入实际上永远不会失败。*/
    if (c->slave_capa & SLAVE_CAPA_PSYNC2) {
        buflen = snprintf(buf,sizeof(buf),"+CONTINUE %s\r\n", server.replid);
    } else {
        buflen = snprintf(buf,sizeof(buf),"+CONTINUE\r\n");
    }
    if (connWrite(c->conn,buf,buflen) != buflen) {
        freeClientAsync(c);
        return C_OK;
    }
    psync_len = addReplyReplicationBacklog(c,psync_offset);
    serverLog(LL_NOTICE,
        "Partial resynchronization request from %s accepted. Sending %lld bytes of backlog starting from offset %lld.",
            replicationGetSlaveName(c),
            psync_len, psync_offset);
    /* Note that we don't need to set the selected DB at server.slaveseldb
     * to -1 to force the master to emit SELECT, since the slave already
     * has this state from the previous connection with the master. */
    /**请注意，我们不需要将 server.slaveseldb 中的选定 DB 设置为 -1 以强制主服务器发出 SELECT，
     * 因为从服务器在与主服务器的先前连接中已经具有此状态。*/

    refreshGoodSlavesCount();

    /* Fire the replica change modules event. 触发副本更改模块事件。*/
    moduleFireServerEvent(REDISMODULE_EVENT_REPLICA_CHANGE,
                          REDISMODULE_SUBEVENT_REPLICA_CHANGE_ONLINE,
                          NULL);

    return C_OK; /* The caller can return, no full resync needed. 调用者可以返回，不需要完全重新同步。*/

need_full_resync:
    /* We need a full resync for some reason... Note that we can't
     * reply to PSYNC right now if a full SYNC is needed. The reply
     * must include the master offset at the time the RDB file we transfer
     * is generated, so we need to delay the reply to that moment. */
    /**出于某种原因，我们需要完全重新同步...请注意，如果需要完全同步，我们现在无法回复 PSYNC。
     * 回复必须包含我们传输的 RDB 文件生成时的主偏移量，因此我们需要将回复延迟到那一刻。*/
    return C_ERR;
}

/* Start a BGSAVE for replication goals, which is, selecting the disk or
 * socket target depending on the configuration, and making sure that
 * the script cache is flushed before to start.
 *
 * The mincapa argument is the bitwise AND among all the slaves capabilities
 * of the slaves waiting for this BGSAVE, so represents the slave capabilities
 * all the slaves support. Can be tested via SLAVE_CAPA_* macros.
 *
 * Side effects, other than starting a BGSAVE:
 *
 * 1) Handle the slaves in WAIT_START state, by preparing them for a full
 *    sync if the BGSAVE was successfully started, or sending them an error
 *    and dropping them from the list of slaves.
 *
 * 2) Flush the Lua scripting script cache if the BGSAVE was actually
 *    started.
 *
 * Returns C_OK on success or C_ERR otherwise. */
/**
 * 为复制目标启动 BGSAVE，即根据配置选择磁盘或套接字目标，并确保在启动之前刷新脚本缓存。
 * mincapa 参数是等待此 BGSAVE 的从机的所有从机能力之间的按位与，因此表示所有从机支持的从机能力。
 * 可以通过 SLAVE_CAPA_ 宏进行测试。除了启动 BGSAVE 之外的副作用：
 *   1) 处理处于 WAIT_START 状态的从站，如果 BGSAVE 已成功启动，则通过准备它们进行完全同步，
 *      或者向它们发送错误并将它们从从站列表中删除。
 *   2) 如果 BGSAVE 实际启动了，则刷新 Lua 脚本缓存。
 * 成功时返回 C_OK，否则返回 C_ERR。
 * */
int startBgsaveForReplication(int mincapa, int req) {
    int retval;
    int socket_target = 0;
    listIter li;
    listNode *ln;

    /* We use a socket target if slave can handle the EOF marker and we're configured to do diskless syncs.
     * Note that in case we're creating a "filtered" RDB (functions-only, for example) we also force socket replication
     * to avoid overwriting the snapshot RDB file with filtered data. */
    /**如果从站可以处理 EOF 标记并且我们配置为进行无盘同步，我们将使用套接字目标。
     * 请注意，如果我们正在创建“过滤”RDB（例如，仅用于函数），我们还会强制套接字复制以避免使用过滤数据覆盖快照 RDB 文件。*/
    socket_target = (server.repl_diskless_sync || req & SLAVE_REQ_RDB_MASK) && (mincapa & SLAVE_CAPA_EOF);
    /* `SYNC` should have failed with error if we don't support socket and require a filter, assert this here */
    /**如果我们不支持套接字并且需要过滤器，则 `SYNC` 应该会失败并出现错误，请在此处断言*/
    serverAssert(socket_target || !(req & SLAVE_REQ_RDB_MASK));

    serverLog(LL_NOTICE,"Starting BGSAVE for SYNC with target: %s",
        socket_target ? "replicas sockets" : "disk");

    rdbSaveInfo rsi, *rsiptr;
    rsiptr = rdbPopulateSaveInfo(&rsi);
    /* Only do rdbSave* when rsiptr is not NULL,
     * otherwise slave will miss repl-stream-db. */
    /**仅当 rsiptr 不为 NULL 时才执行 rdbSave，否则 slave 将错过 repl-stream-db。*/
    if (rsiptr) {
        if (socket_target)
            retval = rdbSaveToSlavesSockets(req,rsiptr);
        else
            retval = rdbSaveBackground(req,server.rdb_filename,rsiptr);
    } else {
        serverLog(LL_WARNING,"BGSAVE for replication: replication information not available, can't generate the RDB file right now. Try later.");
        retval = C_ERR;
    }

    /* If we succeeded to start a BGSAVE with disk target, let's remember
     * this fact, so that we can later delete the file if needed. Note
     * that we don't set the flag to 1 if the feature is disabled, otherwise
     * it would never be cleared: the file is not deleted. This way if
     * the user enables it later with CONFIG SET, we are fine. */
    /**如果我们成功地使用磁盘目标启动了 BGSAVE，让我们记住这个事实，以便我们以后可以在需要时删除该文件。
     * 请注意，如果禁用该功能，我们不会将标志设置为 1，否则它将永远不会被清除：文件不会被删除。
     * 这样，如果用户稍后使用 CONFIG SET 启用它，我们就可以了。*/
    if (retval == C_OK && !socket_target && server.rdb_del_sync_files)
        RDBGeneratedByReplication = 1;

    /* If we failed to BGSAVE, remove the slaves waiting for a full
     * resynchronization from the list of slaves, inform them with
     * an error about what happened, close the connection ASAP. */
    /**如果 BGSAVE 失败，请从从站列表中删除等待完全重新同步的从站，告知他们发生的错误，尽快关闭连接。*/
    if (retval == C_ERR) {
        serverLog(LL_WARNING,"BGSAVE for replication failed");
        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            client *slave = ln->value;

            if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) {
                slave->replstate = REPL_STATE_NONE;
                slave->flags &= ~CLIENT_SLAVE;
                listDelNode(server.slaves,ln);
                addReplyError(slave,
                    "BGSAVE failed, replication can't continue");
                slave->flags |= CLIENT_CLOSE_AFTER_REPLY;
            }
        }
        return retval;
    }

    /* If the target is socket, rdbSaveToSlavesSockets() already setup
     * the slaves for a full resync. Otherwise for disk target do it now.*/
    /**如果目标是套接字，则 rdbSaveToSlavesSockets() 已经设置了从站以进行完全重新同步。
     * 否则，对于磁盘目标，请立即执行。*/
    if (!socket_target) {
        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            client *slave = ln->value;

            if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) {
                /* Check slave has the exact requirements */
                if (slave->slave_req != req)
                    continue;
                replicationSetupSlaveForFullResync(slave, getPsyncInitialOffset());
            }
        }
    }

    return retval;
}

/* SYNC and PSYNC command implementation. */
//SYNC 和 PSYNC 命令实现。
void syncCommand(client *c) {
    /* ignore SYNC if already slave or in monitor mode */
    if (c->flags & CLIENT_SLAVE) return;

    /* Check if this is a failover request to a replica with the same replid and
     * become a master if so. */
    if (c->argc > 3 && !strcasecmp(c->argv[0]->ptr,"psync") && 
        !strcasecmp(c->argv[3]->ptr,"failover"))
    {
        serverLog(LL_WARNING, "Failover request received for replid %s.",
            (unsigned char *)c->argv[1]->ptr);
        if (!server.masterhost) {
            addReplyError(c, "PSYNC FAILOVER can't be sent to a master.");
            return;
        }

        if (!strcasecmp(c->argv[1]->ptr,server.replid)) {
            replicationUnsetMaster();
            sds client = catClientInfoString(sdsempty(),c);
            serverLog(LL_NOTICE,
                "MASTER MODE enabled (failover request from '%s')",client);
            sdsfree(client);
        } else {
            addReplyError(c, "PSYNC FAILOVER replid must match my replid.");
            return;            
        }
    }

    /* Don't let replicas sync with us while we're failing over */
    if (server.failover_state != NO_FAILOVER) {
        addReplyError(c,"-NOMASTERLINK Can't SYNC while failing over");
        return;
    }

    /* Refuse SYNC requests if we are a slave but the link with our master
     * is not ok... */
    /**如果我们是从站，但与我们的主站的链接不正常，则拒绝 SYNC 请求...*/
    if (server.masterhost && server.repl_state != REPL_STATE_CONNECTED) {
        addReplyError(c,"-NOMASTERLINK Can't SYNC while not connected with my master");
        return;
    }

    /* SYNC can't be issued when the server has pending data to send to
     * the client about already issued commands. We need a fresh reply
     * buffer registering the differences between the BGSAVE and the current
     * dataset, so that we can copy to other slaves if needed. */
    /**当服务器有关于已发出命令的待发送数据要发送给客户端时，无法发出 SYNC。
     * 我们需要一个新的回复缓冲区来记录 BGSAVE 和当前数据集之间的差异，以便我们可以在需要时复制到其他从站。*/
    if (clientHasPendingReplies(c)) {
        addReplyError(c,"SYNC and PSYNC are invalid with pending output");
        return;
    }

    /* Fail sync if slave doesn't support EOF capability but wants a filtered RDB. This is because we force filtered
     * RDB's to be generated over a socket and not through a file to avoid conflicts with the snapshot files. Forcing
     * use of a socket is handled, if needed, in `startBgsaveForReplication`. */
    /**如果从站不支持 EOF 功能但需要过滤的 RDB，则同步失败。
     * 这是因为我们强制通过套接字而不是通过文件生成过滤的 RDB，以避免与快照文件发生冲突。
     * 如果需要，在 `startBgsaveForReplication` 中处理强制使用套接字。*/
    if (c->slave_req & SLAVE_REQ_RDB_MASK && !(c->slave_capa & SLAVE_CAPA_EOF)) {
        addReplyError(c,"Filtered replica requires EOF capability");
        return;
    }

    serverLog(LL_NOTICE,"Replica %s asks for synchronization",
        replicationGetSlaveName(c));

    /* Try a partial resynchronization if this is a PSYNC command.
     * If it fails, we continue with usual full resynchronization, however
     * when this happens replicationSetupSlaveForFullResync will replied
     * with:
     * 如果这是 PSYNC 命令，请尝试部分重新同步。如果失败，我们将继续进行通常的完全重新同步，
     * 但是当发生这种情况时，replicationSetupSlaveForFullResync 将回复：
     *
     * +FULLRESYNC <replid> <offset>
     *
     * So the slave knows the new replid and offset to try a PSYNC later
     * if the connection with the master is lost.
     * 因此，slave 知道新的 replid 和 offset，以便稍后在与 master 的连接丢失时尝试 PSYNC。*/
    if (!strcasecmp(c->argv[0]->ptr,"psync")) {
        long long psync_offset;
        if (getLongLongFromObjectOrReply(c, c->argv[2], &psync_offset, NULL) != C_OK) {
            serverLog(LL_WARNING, "Replica %s asks for synchronization but with a wrong offset",
                      replicationGetSlaveName(c));
            return;
        }

        if (masterTryPartialResynchronization(c, psync_offset) == C_OK) {
            server.stat_sync_partial_ok++;
            return; /* No full resync needed, return. 不需要完全重新同步，返回。*/
        } else {
            char *master_replid = c->argv[1]->ptr;

            /* Increment stats for failed PSYNCs, but only if the
             * replid is not "?", as this is used by slaves to force a full
             * resync on purpose when they are not able to partially
             * resync. */
            /**增加失败 PSYNC 的统计信息，但前提是 replid 不是“？”，因为当从站无法部分重新同步时，从站使用它来强制完全重新同步。*/
            if (master_replid[0] != '?') server.stat_sync_partial_err++;
        }
    } else {
        /* If a slave uses SYNC, we are dealing with an old implementation
         * of the replication protocol (like redis-cli --slave). Flag the client
         * so that we don't expect to receive REPLCONF ACK feedbacks. */
        /**如果从属使用 SYNC，我们正在处理复制协议的旧实现（如 redis-cli --slave）。
         * 标记客户端，以便我们不希望收到 REPLCONF ACK 反馈。*/
        c->flags |= CLIENT_PRE_PSYNC;
    }

    /* Full resynchronization. 完全重新同步。*/
    server.stat_sync_full++;

    /* Setup the slave as one waiting for BGSAVE to start. The following code
     * paths will change the state if we handle the slave differently. */
    /**将slave设置为等待BGSAVE启动的slave。如果我们以不同的方式处理从站，以下代码路径将改变状态。*/
    c->replstate = SLAVE_STATE_WAIT_BGSAVE_START;
    if (server.repl_disable_tcp_nodelay)
        connDisableTcpNoDelay(c->conn); /* Non critical if it fails. 如果失败则非关键。*/
    c->repldbfd = -1;
    c->flags |= CLIENT_SLAVE;
    listAddNodeTail(server.slaves,c);

    /* Create the replication backlog if needed. */
    /**如果需要，创建复制积压。*/
    if (listLength(server.slaves) == 1 && server.repl_backlog == NULL) {
        /* When we create the backlog from scratch, we always use a new
         * replication ID and clear the ID2, since there is no valid
         * past history. */
        /**当我们从头开始创建 backlog 时，我们总是使用新的复制 ID 并清除 ID2，因为没有有效的过去历史记录。*/
        changeReplicationId();
        clearReplicationId2();
        createReplicationBacklog();
        serverLog(LL_NOTICE,"Replication backlog created, my new "
                            "replication IDs are '%s' and '%s'",
                            server.replid, server.replid2);
    }

    /* CASE 1: BGSAVE is in progress, with disk target. */
    /**案例 1：BGSAVE 正在进行中，带有磁盘目标。*/
    if (server.child_type == CHILD_TYPE_RDB &&
        server.rdb_child_type == RDB_CHILD_TYPE_DISK)
    {
        /* Ok a background save is in progress. Let's check if it is a good
         * one for replication, i.e. if there is another slave that is
         * registering differences since the server forked to save. */
        /**好的，正在进行后台保存。让我们检查一下它是否适合复制，即是否有另一个从服务器正在注册差异，因为服务器分叉保存。*/
        client *slave;
        listNode *ln;
        listIter li;

        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            slave = ln->value;
            /* If the client needs a buffer of commands, we can't use
             * a replica without replication buffer. */
            /**如果客户端需要命令缓冲区，我们不能使用没有复制缓冲区的副本。*/
            if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_END &&
                (!(slave->flags & CLIENT_REPL_RDBONLY) ||
                 (c->flags & CLIENT_REPL_RDBONLY)))
                break;
        }
        /* To attach this slave, we check that it has at least all the
         * capabilities of the slave that triggered the current BGSAVE
         * and its exact requirements. */
        /**为了附加这个从站，我们检查它是否至少具有触发当前 BGSAVE 的从站的所有功能及其确切要求。*/
        if (ln && ((c->slave_capa & slave->slave_capa) == slave->slave_capa) &&
            c->slave_req == slave->slave_req) {
            /* Perfect, the server is already registering differences for
             * another slave. Set the right state, and copy the buffer.
             * We don't copy buffer if clients don't want. */
            /**完美，服务器已经在为另一个从站注册差异。设置正确的状态，并复制缓冲区。
             * 如果客户不想要，我们不会复制缓冲区。*/
            if (!(c->flags & CLIENT_REPL_RDBONLY))
                copyReplicaOutputBuffer(c,slave);
            replicationSetupSlaveForFullResync(c,slave->psync_initial_offset);
            serverLog(LL_NOTICE,"Waiting for end of BGSAVE for SYNC");
        } else {
            /* No way, we need to wait for the next BGSAVE in order to
             * register differences. */
            /**没办法，我们需要等待下一个 BGSAVE 才能注册差异。*/
            serverLog(LL_NOTICE,"Can't attach the replica to the current BGSAVE. Waiting for next BGSAVE for SYNC");
        }

    /* CASE 2: BGSAVE is in progress, with socket target. */
    /**案例 2：BGSAVE 正在进行，带有套接字目标。*/
    } else if (server.child_type == CHILD_TYPE_RDB &&
               server.rdb_child_type == RDB_CHILD_TYPE_SOCKET)
    {
        /* There is an RDB child process but it is writing directly to
         * children sockets. We need to wait for the next BGSAVE
         * in order to synchronize. */
        /**有一个 RDB 子进程，但它直接写入子套接字。我们需要等待下一个 BGSAVE 才能同步。*/
        serverLog(LL_NOTICE,"Current BGSAVE has socket target. Waiting for next BGSAVE for SYNC");

    /* CASE 3: There is no BGSAVE is in progress. */
    /**案例 3：没有正在进行的 BGSAVE。*/
    } else {
        if (server.repl_diskless_sync && (c->slave_capa & SLAVE_CAPA_EOF) &&
            server.repl_diskless_sync_delay)
        {
            /* Diskless replication RDB child is created inside
             * replicationCron() since we want to delay its start a
             * few seconds to wait for more slaves to arrive. */
            /**无盘复制 RDB 子节点是在 replicationCron() 内部创建的，
             * 因为我们希望将其启动延迟几秒钟以等待更多从属节点到达。*/
            serverLog(LL_NOTICE,"Delay next BGSAVE for diskless SYNC");
        } else {
            /* We don't have a BGSAVE in progress, let's start one. Diskless
             * or disk-based mode is determined by replica's capacity. */
            /**我们没有正在进行的 BGSAVE，让我们开始一个。无盘或基于磁盘的模式由副本的容量决定。*/
            if (!hasActiveChildProcess()) {
                startBgsaveForReplication(c->slave_capa, c->slave_req);
            } else {
                serverLog(LL_NOTICE,
                    "No BGSAVE in progress, but another BG operation is active. "
                    "BGSAVE for replication delayed");
            }
        }
    }
    return;
}

/* REPLCONF <option> <value> <option> <value> ...
 * This command is used by a replica in order to configure the replication
 * process before starting it with the SYNC command.
 * This command is also used by a master in order to get the replication
 * offset from a replica.
 *
 * Currently we support these options:
 *
 * - listening-port <port>
 * - ip-address <ip>
 * What is the listening ip and port of the Replica redis instance, so that
 * the master can accurately lists replicas and their listening ports in the
 * INFO output.
 *
 * - capa <eof|psync2>
 * What is the capabilities of this instance.
 * eof: supports EOF-style RDB transfer for diskless replication.
 * psync2: supports PSYNC v2, so understands +CONTINUE <new repl ID>.
 *
 * - ack <offset>
 * Replica informs the master the amount of replication stream that it
 * processed so far.
 *
 * - getack
 * Unlike other subcommands, this is used by master to get the replication
 * offset from a replica.
 *
 * - rdb-only <0|1>
 * Only wants RDB snapshot without replication buffer.
 *
 * - rdb-filter-only <include-filters>
 * Define "include" filters for the RDB snapshot. Currently we only support
 * a single include filter: "functions". Passing an empty string "" will
 * result in an empty RDB. */
/**REPLCONF <option> <value> <option> <value> ...
 * 此命令由副本使用，以便在使用 SYNC 命令启动复制过程之前配置复制过程。主服务器也使用此命令从副本获取复制偏移量。
 * 目前我们支持以下选项：
 *   - listener-port <port>
 *   - ip-address <ip> Replica redis实例的监听ip和端口是什么，以便master可以在INFO输出中准确列出replicas及其监听端口。
 *   - capa <eof|psync2> 这个实例的能力是什么。 eof：支持 EOF 样式的 RDB 传输以进行无盘复制。
 *     psync2：支持 PSYNC v2，所以理解 +CONTINUE <new repl ID>。
 *   - ack <offset> Replica 通知 master 到目前为止它处理的复制流的数量。
 *   - getack 与其他子命令不同，master 使用它来获取副本的复制偏移量。
 *   - rdb-only <0|1> 只需要没有复制缓冲区的 RDB 快照。
 *   - rdb-filter-only <include-filters> 为 RDB 快照定义“包含”过滤器。目前我们只支持一个包含过滤器：“functions”。
 *     传递一个空字符串 "" 将导致一个空的 RDB。*/
void replconfCommand(client *c) {
    int j;

    if ((c->argc % 2) == 0) {
        /* Number of arguments must be odd to make sure that every
         * option has a corresponding value. */
        addReplyErrorObject(c,shared.syntaxerr);
        return;
    }

    /* Process every option-value pair. */
    for (j = 1; j < c->argc; j+=2) {
        if (!strcasecmp(c->argv[j]->ptr,"listening-port")) {
            long port;

            if ((getLongFromObjectOrReply(c,c->argv[j+1],
                    &port,NULL) != C_OK))
                return;
            c->slave_listening_port = port;
        } else if (!strcasecmp(c->argv[j]->ptr,"ip-address")) {
            sds addr = c->argv[j+1]->ptr;
            if (sdslen(addr) < NET_HOST_STR_LEN) {
                if (c->slave_addr) sdsfree(c->slave_addr);
                c->slave_addr = sdsdup(addr);
            } else {
                addReplyErrorFormat(c,"REPLCONF ip-address provided by "
                    "replica instance is too long: %zd bytes", sdslen(addr));
                return;
            }
        } else if (!strcasecmp(c->argv[j]->ptr,"capa")) {
            /* Ignore capabilities not understood by this master. */
            if (!strcasecmp(c->argv[j+1]->ptr,"eof"))
                c->slave_capa |= SLAVE_CAPA_EOF;
            else if (!strcasecmp(c->argv[j+1]->ptr,"psync2"))
                c->slave_capa |= SLAVE_CAPA_PSYNC2;
        } else if (!strcasecmp(c->argv[j]->ptr,"ack")) {
            /* REPLCONF ACK is used by slave to inform the master the amount
             * of replication stream that it processed so far. It is an
             * internal only command that normal clients should never use. */
            /**从站使用 REPLCONF ACK 通知主站到目前为止它处理的复制流的数量。这是普通客户端永远不应使用的内部唯一命令。*/
            long long offset;

            if (!(c->flags & CLIENT_SLAVE)) return;
            if ((getLongLongFromObject(c->argv[j+1], &offset) != C_OK))
                return;
            if (offset > c->repl_ack_off)
                c->repl_ack_off = offset;
            c->repl_ack_time = server.unixtime;
            /* If this was a diskless replication, we need to really put
             * the slave online when the first ACK is received (which
             * confirms slave is online and ready to get more data). This
             * allows for simpler and less CPU intensive EOF detection
             * when streaming RDB files.
             * There's a chance the ACK got to us before we detected that the
             * bgsave is done (since that depends on cron ticks), so run a
             * quick check first (instead of waiting for the next ACK. */
            /**如果这是一个无盘复制，我们需要在收到第一个 ACK 时让从属真正上线（这确认从属在线并准备好获取更多数据）。
             * 这允许在流式传输 RDB 文件时进行更简单且 CPU 密集度较低的 EOF 检测。在我们检测到 bgsave 完成之前，
             * ACK 有可能已经到达我们这里（因为这取决于 cron 滴答声），所以首先运行快速检查（而不是等待下一个 ACK。*/
            if (server.child_type == CHILD_TYPE_RDB && c->replstate == SLAVE_STATE_WAIT_BGSAVE_END)
                checkChildrenDone();
            if (c->repl_start_cmd_stream_on_ack && c->replstate == SLAVE_STATE_ONLINE)
                replicaStartCommandStream(c);
            /* Note: this command does not reply anything! */
            return;
        } else if (!strcasecmp(c->argv[j]->ptr,"getack")) {
            /* REPLCONF GETACK is used in order to request an ACK ASAP
             * to the slave. */
            if (server.masterhost && server.master) replicationSendAck();
            return;
        } else if (!strcasecmp(c->argv[j]->ptr,"rdb-only")) {
           /* REPLCONF RDB-ONLY is used to identify the client only wants
            * RDB snapshot without replication buffer. */
            long rdb_only = 0;
            if (getRangeLongFromObjectOrReply(c,c->argv[j+1],
                    0,1,&rdb_only,NULL) != C_OK)
                return;
            if (rdb_only == 1) c->flags |= CLIENT_REPL_RDBONLY;
            else c->flags &= ~CLIENT_REPL_RDBONLY;
        } else if (!strcasecmp(c->argv[j]->ptr,"rdb-filter-only")) {
            /* REPLCONFG RDB-FILTER-ONLY is used to define "include" filters
             * for the RDB snapshot. Currently we only support a single
             * include filter: "functions". In the future we may want to add
             * other filters like key patterns, key types, non-volatile, module
             * aux fields, ...
             * We might want to add the complementing "RDB-FILTER-EXCLUDE" to
             * filter out certain data. */
            /**REPLCONFG RDB-FILTER-ONLY 用于为 RDB 快照定义“包含”过滤器。
             * 目前我们只支持一个包含过滤器：“functions”。将来我们可能想要添加其他过滤器，
             * 例如键模式、键类型、非易失性、模块辅助字段……我们可能想要添加补充的“RDB-FILTER-EXCLUDE”来过滤掉某些数据。*/
            int filter_count, i;
            sds *filters;
            if (!(filters = sdssplitargs(c->argv[j+1]->ptr, &filter_count))) {
                addReplyErrorFormat(c, "Missing rdb-filter-only values");
                return;
            }
            /* By default filter out all parts of the rdb */
            c->slave_req |= SLAVE_REQ_RDB_EXCLUDE_DATA;
            c->slave_req |= SLAVE_REQ_RDB_EXCLUDE_FUNCTIONS;
            for (i = 0; i < filter_count; i++) {
                if (!strcasecmp(filters[i], "functions"))
                    c->slave_req &= ~SLAVE_REQ_RDB_EXCLUDE_FUNCTIONS;
                else {
                    addReplyErrorFormat(c, "Unsupported rdb-filter-only option: %s", (char*)filters[i]);
                    sdsfreesplitres(filters, filter_count);
                    return;
                }
            }
            sdsfreesplitres(filters, filter_count);
        } else {
            addReplyErrorFormat(c,"Unrecognized REPLCONF option: %s",
                (char*)c->argv[j]->ptr);
            return;
        }
    }
    addReply(c,shared.ok);
}

/* This function puts a replica in the online state, and should be called just
 * after a replica received the RDB file for the initial synchronization.
 *
 * It does a few things:
 * 1) Put the slave in ONLINE state.
 * 2) Update the count of "good replicas".
 * 3) Trigger the module event. */
/**此函数将副本置于联机状态，并且应在副本收到 RDB 文件以进行初始同步后立即调用。它做了几件事：
 *   1) 将奴隶置于 ONLINE 状态。
 *   2）更新“好副本”的计数。
 *   3) 触发模块事件。*/
void replicaPutOnline(client *slave) {
    if (slave->flags & CLIENT_REPL_RDBONLY) {
        return;
    }

    slave->replstate = SLAVE_STATE_ONLINE;
    slave->repl_ack_time = server.unixtime; /* Prevent false timeout. */

    refreshGoodSlavesCount();
    /* Fire the replica change modules event. */
    moduleFireServerEvent(REDISMODULE_EVENT_REPLICA_CHANGE,
                          REDISMODULE_SUBEVENT_REPLICA_CHANGE_ONLINE,
                          NULL);
    serverLog(LL_NOTICE,"Synchronization with replica %s succeeded",
        replicationGetSlaveName(slave));
}

/* This function should be called just after a replica received the RDB file
 * for the initial synchronization, and we are finally ready to send the
 * incremental stream of commands.
 *
 * It does a few things:
 * 1) Close the replica's connection async if it doesn't need replication
 *    commands buffer stream, since it actually isn't a valid replica.
 * 2) Make sure the writable event is re-installed, since when calling the SYNC
 *    command we had no replies and it was disabled, and then we could
 *    accumulate output buffer data without sending it to the replica so it
 *    won't get mixed with the RDB stream. */
/**这个函数应该在副本接收到初始同步的 RDB 文件后调用，我们终于准备好发送增量命令流。它做了几件事：
 *   1）如果它不需要复制命令缓冲流，则关闭副本的异步连接，因为它实际上不是有效的副本。
 *   2) 确保重新安装可写事件，因为在调用 SYNC 命令时我们没有回复并且它被禁用，
 *      然后我们可以累积输出缓冲区数据而不将其发送到副本，因此它不会与RDB 流。*/
void replicaStartCommandStream(client *slave) {
    slave->repl_start_cmd_stream_on_ack = 0;
    if (slave->flags & CLIENT_REPL_RDBONLY) {
        serverLog(LL_NOTICE,
            "Close the connection with replica %s as RDB transfer is complete",
            replicationGetSlaveName(slave));
        freeClientAsync(slave);
        return;
    }

    putClientInPendingWriteQueue(slave);
}

/* We call this function periodically to remove an RDB file that was
 * generated because of replication, in an instance that is otherwise
 * without any persistence. We don't want instances without persistence
 * to take RDB files around, this violates certain policies in certain
 * environments. */
/**我们定期调用此函数以删除由于复制而生成的 RDB 文件，否则该实例没有任何持久性。
 * 我们不希望没有持久性的实例携带 RDB 文件，这违反了某些环境中的某些策略。*/
void removeRDBUsedToSyncReplicas(void) {
    /* If the feature is disabled, return ASAP but also clear the
     * RDBGeneratedByReplication flag in case it was set. Otherwise if the
     * feature was enabled, but gets disabled later with CONFIG SET, the
     * flag may remain set to one: then next time the feature is re-enabled
     * via CONFIG SET we have it set even if no RDB was generated
     * because of replication recently. */
    /**如果该功能被禁用，请尽快返回，但也要清除 RDBGeneratedByReplication 标志，以防它被设置。
     * 否则，如果该功能已启用，但稍后通过 CONFIG SET 禁用，则该标志可能保持设置为 1：
     * 然后下次通过 CONFIG SET 重新启用该功能时，即使最近由于复制没有生成 RDB，我们也会设置它。*/
    if (!server.rdb_del_sync_files) {
        RDBGeneratedByReplication = 0;
        return;
    }

    if (allPersistenceDisabled() && RDBGeneratedByReplication) {
        client *slave;
        listNode *ln;
        listIter li;

        int delrdb = 1;
        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            slave = ln->value;
            if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START ||
                slave->replstate == SLAVE_STATE_WAIT_BGSAVE_END ||
                slave->replstate == SLAVE_STATE_SEND_BULK)
            {
                delrdb = 0;
                break; /* No need to check the other replicas. */
            }
        }
        if (delrdb) {
            struct stat sb;
            if (lstat(server.rdb_filename,&sb) != -1) {
                RDBGeneratedByReplication = 0;
                serverLog(LL_NOTICE,
                    "Removing the RDB file used to feed replicas "
                    "in a persistence-less instance");
                bg_unlink(server.rdb_filename);
            }
        }
    }
}

void sendBulkToSlave(connection *conn) {
    client *slave = connGetPrivateData(conn);
    char buf[PROTO_IOBUF_LEN];
    ssize_t nwritten, buflen;

    /* Before sending the RDB file, we send the preamble as configured by the
     * replication process. Currently the preamble is just the bulk count of
     * the file in the form "$<length>\r\n". */
    /**在发送 RDB 文件之前，我们按照复制过程的配置发送前导码。目前，序言只是“<length>\r\n”形式的文件的批量计数。*/
    if (slave->replpreamble) {
        nwritten = connWrite(conn,slave->replpreamble,sdslen(slave->replpreamble));
        if (nwritten == -1) {
            serverLog(LL_WARNING,
                "Write error sending RDB preamble to replica: %s",
                connGetLastError(conn));
            freeClient(slave);
            return;
        }
        atomicIncr(server.stat_net_repl_output_bytes, nwritten);
        sdsrange(slave->replpreamble,nwritten,-1);
        if (sdslen(slave->replpreamble) == 0) {
            sdsfree(slave->replpreamble);
            slave->replpreamble = NULL;
            /* fall through sending data. */
        } else {
            return;
        }
    }

    /* If the preamble was already transferred, send the RDB bulk data. */
    lseek(slave->repldbfd,slave->repldboff,SEEK_SET);
    buflen = read(slave->repldbfd,buf,PROTO_IOBUF_LEN);
    if (buflen <= 0) {
        serverLog(LL_WARNING,"Read error sending DB to replica: %s",
            (buflen == 0) ? "premature EOF" : strerror(errno));
        freeClient(slave);
        return;
    }
    if ((nwritten = connWrite(conn,buf,buflen)) == -1) {
        if (connGetState(conn) != CONN_STATE_CONNECTED) {
            serverLog(LL_WARNING,"Write error sending DB to replica: %s",
                connGetLastError(conn));
            freeClient(slave);
        }
        return;
    }
    slave->repldboff += nwritten;
    atomicIncr(server.stat_net_repl_output_bytes, nwritten);
    if (slave->repldboff == slave->repldbsize) {
        close(slave->repldbfd);
        slave->repldbfd = -1;
        connSetWriteHandler(slave->conn,NULL);
        replicaPutOnline(slave);
        replicaStartCommandStream(slave);
    }
}

/* Remove one write handler from the list of connections waiting to be writable
 * during rdb pipe transfer. */
//在 rdb 管道传输期间从等待可写的连接列表中删除一个写处理程序。
void rdbPipeWriteHandlerConnRemoved(struct connection *conn) {
    if (!connHasWriteHandler(conn))
        return;
    connSetWriteHandler(conn, NULL);
    client *slave = connGetPrivateData(conn);
    slave->repl_last_partial_write = 0;
    server.rdb_pipe_numconns_writing--;
    /* if there are no more writes for now for this conn, or write error: */
    if (server.rdb_pipe_numconns_writing == 0) {
        if (aeCreateFileEvent(server.el, server.rdb_pipe_read, AE_READABLE, rdbPipeReadHandler,NULL) == AE_ERR) {
            serverPanic("Unrecoverable error creating server.rdb_pipe_read file event.");
        }
    }
}

/* Called in diskless master during transfer of data from the rdb pipe, when
 * the replica becomes writable again. */
//在从 rdb 管道传输数据期间，当副本再次变为可写时，在无盘主机中调用。
void rdbPipeWriteHandler(struct connection *conn) {
    serverAssert(server.rdb_pipe_bufflen>0);
    client *slave = connGetPrivateData(conn);
    ssize_t nwritten;
    if ((nwritten = connWrite(conn, server.rdb_pipe_buff + slave->repldboff,
                              server.rdb_pipe_bufflen - slave->repldboff)) == -1)
    {
        if (connGetState(conn) == CONN_STATE_CONNECTED)
            return; /* equivalent to EAGAIN */
        serverLog(LL_WARNING,"Write error sending DB to replica: %s",
            connGetLastError(conn));
        freeClient(slave);
        return;
    } else {
        slave->repldboff += nwritten;
        atomicIncr(server.stat_net_repl_output_bytes, nwritten);
        if (slave->repldboff < server.rdb_pipe_bufflen) {
            slave->repl_last_partial_write = server.unixtime;
            return; /* more data to write.. */
        }
    }
    rdbPipeWriteHandlerConnRemoved(conn);
}

/* Called in diskless master, when there's data to read from the child's rdb pipe */
//当有数据要从孩子的 rdb 管道中读取时，在无盘主机中调用
void rdbPipeReadHandler(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask) {
    UNUSED(mask);
    UNUSED(clientData);
    UNUSED(eventLoop);
    int i;
    if (!server.rdb_pipe_buff)
        server.rdb_pipe_buff = zmalloc(PROTO_IOBUF_LEN);
    serverAssert(server.rdb_pipe_numconns_writing==0);

    while (1) {
        server.rdb_pipe_bufflen = read(fd, server.rdb_pipe_buff, PROTO_IOBUF_LEN);
        if (server.rdb_pipe_bufflen < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            serverLog(LL_WARNING,"Diskless rdb transfer, read error sending DB to replicas: %s", strerror(errno));
            for (i=0; i < server.rdb_pipe_numconns; i++) {
                connection *conn = server.rdb_pipe_conns[i];
                if (!conn)
                    continue;
                client *slave = connGetPrivateData(conn);
                freeClient(slave);
                server.rdb_pipe_conns[i] = NULL;
            }
            killRDBChild();
            return;
        }

        if (server.rdb_pipe_bufflen == 0) {
            /* EOF - write end was closed. */
            int stillUp = 0;
            aeDeleteFileEvent(server.el, server.rdb_pipe_read, AE_READABLE);
            for (i=0; i < server.rdb_pipe_numconns; i++)
            {
                connection *conn = server.rdb_pipe_conns[i];
                if (!conn)
                    continue;
                stillUp++;
            }
            serverLog(LL_WARNING,"Diskless rdb transfer, done reading from pipe, %d replicas still up.", stillUp);
            /* Now that the replicas have finished reading, notify the child that it's safe to exit. 
             * When the server detects the child has exited, it can mark the replica as online, and
             * start streaming the replication buffers. */
            /**现在副本已完成读取，通知孩子可以安全退出。当服务器检测到子节点已退出时，它可以将副本标记为在线，
             * 并开始流式传输复制缓冲区。*/
            close(server.rdb_child_exit_pipe);
            server.rdb_child_exit_pipe = -1;
            return;
        }

        int stillAlive = 0;
        for (i=0; i < server.rdb_pipe_numconns; i++)
        {
            ssize_t nwritten;
            connection *conn = server.rdb_pipe_conns[i];
            if (!conn)
                continue;

            client *slave = connGetPrivateData(conn);
            if ((nwritten = connWrite(conn, server.rdb_pipe_buff, server.rdb_pipe_bufflen)) == -1) {
                if (connGetState(conn) != CONN_STATE_CONNECTED) {
                    serverLog(LL_WARNING,"Diskless rdb transfer, write error sending DB to replica: %s",
                        connGetLastError(conn));
                    freeClient(slave);
                    server.rdb_pipe_conns[i] = NULL;
                    continue;
                }
                /* An error and still in connected state, is equivalent to EAGAIN */
                slave->repldboff = 0;
            } else {
                /* Note: when use diskless replication, 'repldboff' is the offset
                 * of 'rdb_pipe_buff' sent rather than the offset of entire RDB. */
                slave->repldboff = nwritten;
                atomicIncr(server.stat_net_repl_output_bytes, nwritten);
            }
            /* If we were unable to write all the data to one of the replicas,
             * setup write handler (and disable pipe read handler, below) */
            if (nwritten != server.rdb_pipe_bufflen) {
                slave->repl_last_partial_write = server.unixtime;
                server.rdb_pipe_numconns_writing++;
                connSetWriteHandler(conn, rdbPipeWriteHandler);
            }
            stillAlive++;
        }

        if (stillAlive == 0) {
            serverLog(LL_WARNING,"Diskless rdb transfer, last replica dropped, killing fork child.");
            killRDBChild();
        }
        /*  Remove the pipe read handler if at least one write handler was set. */
        if (server.rdb_pipe_numconns_writing || stillAlive == 0) {
            aeDeleteFileEvent(server.el, server.rdb_pipe_read, AE_READABLE);
            break;
        }
    }
}

/* This function is called at the end of every background saving.
 *
 * The argument bgsaveerr is C_OK if the background saving succeeded
 * otherwise C_ERR is passed to the function.
 * The 'type' argument is the type of the child that terminated
 * (if it had a disk or socket target). */
/**此函数在每次后台保存结束时调用。如果后台保存成功，则参数 bgsaveerr 为 C_OK，否则将 C_ERR 传递给函数。
 * 'type' 参数是终止的孩子的类型（如果它有一个磁盘或套接字目标）。*/
void updateSlavesWaitingBgsave(int bgsaveerr, int type) {
    listNode *ln;
    listIter li;

    /* Note: there's a chance we got here from within the REPLCONF ACK command
     * so we must avoid using freeClient, otherwise we'll crash on our way up.
     * 注意：我们有可能是从 REPLCONF ACK 命令中到达这里的，所以我们必须避免使用 freeClient，否则我们会在上升过程中崩溃。*/

    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;

        if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_END) {
            struct redis_stat buf;

            if (bgsaveerr != C_OK) {
                freeClientAsync(slave);
                serverLog(LL_WARNING,"SYNC failed. BGSAVE child returned an error");
                continue;
            }

            /* If this was an RDB on disk save, we have to prepare to send
             * the RDB from disk to the slave socket. Otherwise if this was
             * already an RDB -> Slaves socket transfer, used in the case of
             * diskless replication, our work is trivial, we can just put
             * the slave online. */
            /**如果这是磁盘保存的 RDB，我们必须准备将 RDB 从磁盘发送到从套接字。
             * 否则，如果这已经是一个 RDB -> Slaves 套接字传输，用于无盘复制的情况，我们的工作是微不足道的，
             * 我们可以让从站联机。*/
            if (type == RDB_CHILD_TYPE_SOCKET) {
                serverLog(LL_NOTICE,
                    "Streamed RDB transfer with replica %s succeeded (socket). Waiting for REPLCONF ACK from slave to enable streaming",
                        replicationGetSlaveName(slave));
                /* Note: we wait for a REPLCONF ACK message from the replica in
                 * order to really put it online (install the write handler
                 * so that the accumulated data can be transferred). However
                 * we change the replication state ASAP, since our slave
                 * is technically online now.
                 *
                 * So things work like that:
                 *
                 * 1. We end transferring the RDB file via socket.
                 * 2. The replica is put ONLINE but the write handler
                 *    is not installed.
                 * 3. The replica however goes really online, and pings us
                 *    back via REPLCONF ACK commands.
                 * 4. Now we finally install the write handler, and send
                 *    the buffers accumulated so far to the replica.
                 *
                 * But why we do that? Because the replica, when we stream
                 * the RDB directly via the socket, must detect the RDB
                 * EOF (end of file), that is a special random string at the
                 * end of the RDB (for streamed RDBs we don't know the length
                 * in advance). Detecting such final EOF string is much
                 * simpler and less CPU intensive if no more data is sent
                 * after such final EOF. So we don't want to glue the end of
                 * the RDB transfer with the start of the other replication
                 * data. */
                /**
                 * 注意：我们等待来自副本的 REPLCONF ACK 消息以便真正将其上线（安装写处理程序以便可以传输累积的数据）。
                 * 但是，我们尽快更改复制状态，因为我们的从属设备现在技术上在线。所以事情是这样工作的：
                 *   1. 我们结束通过套接字传输 RDB 文件。
                 *   2. 副本处于联机状态，但未安装写入处理程序。
                 *   3. 但是副本真正在线，并通过 REPLCONF ACK 命令 ping 我们回来。
                 *   4. 现在我们终于安装了写处理程序，并将到目前为止积累的缓冲区发送到副本。
                 * 但我们为什么要这样做？因为replica，当我们直接通过socket流RDB时，必须检测RDB EOF（end of file），
                 * 即RDB末尾的特殊随机字符串（对于流RDB我们事先不知道长度）。
                 * 如果在这样的最终 EOF 之后不再发送数据，那么检测这样的最终 EOF 字符串会简单得多，并且 CPU 密集度更低。
                 * 所以我们不想将 RDB 传输的结束与其他复制数据的开始粘合在一起。
                 * */
                replicaPutOnline(slave);
                slave->repl_start_cmd_stream_on_ack = 1;
            } else {
                if ((slave->repldbfd = open(server.rdb_filename,O_RDONLY)) == -1 ||
                    redis_fstat(slave->repldbfd,&buf) == -1) {
                    freeClientAsync(slave);
                    serverLog(LL_WARNING,"SYNC failed. Can't open/stat DB after BGSAVE: %s", strerror(errno));
                    continue;
                }
                slave->repldboff = 0;
                slave->repldbsize = buf.st_size;
                slave->replstate = SLAVE_STATE_SEND_BULK;
                slave->replpreamble = sdscatprintf(sdsempty(),"$%lld\r\n",
                    (unsigned long long) slave->repldbsize);

                connSetWriteHandler(slave->conn,NULL);
                if (connSetWriteHandler(slave->conn,sendBulkToSlave) == C_ERR) {
                    freeClientAsync(slave);
                    continue;
                }
            }
        }
    }
}

/* Change the current instance replication ID with a new, random one.
 * This will prevent successful PSYNCs between this master and other
 * slaves, so the command should be called when something happens that
 * alters the current story of the dataset. */
/**使用新的随机复制 ID 更改当前实例复制 ID。这将阻止此 master 和其他 slave 之间成功的 PSYNC，
 * 因此当发生改变数据集当前故事的事情时应该调用该命令。*/
void changeReplicationId(void) {
    getRandomHexChars(server.replid,CONFIG_RUN_ID_SIZE);
    server.replid[CONFIG_RUN_ID_SIZE] = '\0';
}

/* Clear (invalidate) the secondary replication ID. This happens, for
 * example, after a full resynchronization, when we start a new replication
 * history. */
//清除（无效）辅助复制 ID。例如，在完全重新同步之后，当我们开始新的复制历史时，就会发生这种情况。
void clearReplicationId2(void) {
    memset(server.replid2,'0',sizeof(server.replid));
    server.replid2[CONFIG_RUN_ID_SIZE] = '\0';
    server.second_replid_offset = -1;
}

/* Use the current replication ID / offset as secondary replication
 * ID, and change the current one in order to start a new history.
 * This should be used when an instance is switched from slave to master
 * so that it can serve PSYNC requests performed using the master
 * replication ID. */
/**使用当前复制 ID 偏移量作为辅助复制 ID，并更改当前复制 ID 以开始新的历史记录。
 * 这应该在实例从从属切换到主控时使用，以便它可以服务使用主复制 ID 执行的 PSYNC 请求。*/
void shiftReplicationId(void) {
    memcpy(server.replid2,server.replid,sizeof(server.replid));
    /* We set the second replid offset to the master offset + 1, since
     * the slave will ask for the first byte it has not yet received, so
     * we need to add one to the offset: for example if, as a slave, we are
     * sure we have the same history as the master for 50 bytes, after we
     * are turned into a master, we can accept a PSYNC request with offset
     * 51, since the slave asking has the same history up to the 50th
     * byte, and is asking for the new bytes starting at offset 51. */
    /**我们将第二个 replid 偏移量设置为 master 偏移量 + 1，因为 slave 将要求它尚未收到的第一个字节，
     * 所以我们需要在 offset 上加一个：例如，如果作为 slave，我们确定我们与master有50个字节的相同历史，
     * 当我们变成master后，我们可以接受一个偏移量为51的PSYNC请求，因为slave请求在第50个字节之前有相同的历史，
     * 并且正在请求新的字节从偏移量 51 开始。*/
    server.second_replid_offset = server.master_repl_offset+1;
    changeReplicationId();
    serverLog(LL_WARNING,"Setting secondary replication ID to %s, valid up to offset: %lld. New replication ID is %s", server.replid2, server.second_replid_offset, server.replid);
}

/* ----------------------------------- SLAVE -------------------------------- */

/* Returns 1 if the given replication state is a handshake state,
 * 0 otherwise. */
//如果给定的复制状态是握手状态，则返回 1，否则返回 0。
int slaveIsInHandshakeState(void) {
    return server.repl_state >= REPL_STATE_RECEIVE_PING_REPLY &&
           server.repl_state <= REPL_STATE_RECEIVE_PSYNC_REPLY;
}

/* Avoid the master to detect the slave is timing out while loading the
 * RDB file in initial synchronization. We send a single newline character
 * that is valid protocol but is guaranteed to either be sent entirely or
 * not, since the byte is indivisible.
 *
 * The function is called in two contexts: while we flush the current
 * data with emptyDb(), and while we load the new data received as an
 * RDB file from the master. */
/**避免master在初始同步加载RDB文件时检测到slave超时。
 * 我们发送一个有效协议的换行符，但保证要么完全发送要么不发送，因为字节是不可分割的。
 *
 * 该函数在两种情况下调用：当我们使用 emptyDb() 刷新当前数据时，以及当我们加载从主服务器作为 RDB 文件接收的新数据时。*/
void replicationSendNewlineToMaster(void) {
    static time_t newline_sent;
    if (time(NULL) != newline_sent) {
        newline_sent = time(NULL);
        /* Pinging back in this stage is best-effort. */
        if (server.repl_transfer_s) connWrite(server.repl_transfer_s, "\n", 1);
    }
}

/* Callback used by emptyDb() while flushing away old data to load
 * the new dataset received by the master and by discardTempDb()
 * after loading succeeded or failed. */
//emptyDb() 使用回调，同时刷新旧数据以加载 master 接收到的新数据集，并在加载成功或失败后由 discardTempDb() 使用。
void replicationEmptyDbCallback(dict *d) {
    UNUSED(d);
    if (server.repl_state == REPL_STATE_TRANSFER)
        replicationSendNewlineToMaster();
}

/* Once we have a link with the master and the synchronization was
 * performed, this function materializes the master client we store
 * at server.master, starting from the specified file descriptor. */
/**一旦我们与主服务器建立了链接并执行了同步，该函数就会从指定的文件描述符开始具体化我们存储在 server.master 中的主客户端。*/
void replicationCreateMasterClient(connection *conn, int dbid) {
    server.master = createClient(conn);
    if (conn)
        connSetReadHandler(server.master->conn, readQueryFromClient);

    /**
     * Important note:
     * The CLIENT_DENY_BLOCKING flag is not, and should not, be set here.
     * For commands like BLPOP, it makes no sense to block the master
     * connection, and such blocking attempt will probably cause deadlock and
     * break the replication. We consider such a thing as a bug because
     * commands as BLPOP should never be sent on the replication link.
     * A possible use-case for blocking the replication link is if a module wants
     * to pass the execution to a background thread and unblock after the
     * execution is done. This is the reason why we allow blocking the replication
     * connection. */
    server.master->flags |= CLIENT_MASTER;

    server.master->authenticated = 1;
    server.master->reploff = server.master_initial_offset;
    server.master->read_reploff = server.master->reploff;
    server.master->user = NULL; /* This client can do everything. */
    memcpy(server.master->replid, server.master_replid,
        sizeof(server.master_replid));
    /* If master offset is set to -1, this master is old and is not
     * PSYNC capable, so we flag it accordingly. */
    if (server.master->reploff == -1)
        server.master->flags |= CLIENT_PRE_PSYNC;
    if (dbid != -1) selectDb(server.master,dbid);
}

/* This function will try to re-enable the AOF file after the
 * master-replica synchronization: if it fails after multiple attempts
 * the replica cannot be considered reliable and exists with an
 * error. */
//该函数将在主副本同步后尝试重新启用 AOF 文件：如果多次尝试失败，则副本不能被认为是可靠的并且存在错误。
void restartAOFAfterSYNC() {
    unsigned int tries, max_tries = 10;
    for (tries = 0; tries < max_tries; ++tries) {
        if (startAppendOnly() == C_OK) break;
        serverLog(LL_WARNING,
            "Failed enabling the AOF after successful master synchronization! "
            "Trying it again in one second.");
        sleep(1);
    }
    if (tries == max_tries) {
        serverLog(LL_WARNING,
            "FATAL: this replica instance finished the synchronization with "
            "its master, but the AOF can't be turned on. Exiting now.");
        exit(1);
    }
}

static int useDisklessLoad() {
    /* compute boolean decision to use diskless load */
    int enabled = server.repl_diskless_load == REPL_DISKLESS_LOAD_SWAPDB ||
           (server.repl_diskless_load == REPL_DISKLESS_LOAD_WHEN_DB_EMPTY && dbTotalServerKeyCount()==0);

    if (enabled) {
        /* Check all modules handle read errors, otherwise it's not safe to use diskless load. */
        if (!moduleAllDatatypesHandleErrors()) {
            serverLog(LL_WARNING,
                "Skipping diskless-load because there are modules that don't handle read errors.");
            enabled = 0;
        }
        /* Check all modules handle async replication, otherwise it's not safe to use diskless load. */
        else if (server.repl_diskless_load == REPL_DISKLESS_LOAD_SWAPDB && !moduleAllModulesHandleReplAsyncLoad()) {
            serverLog(LL_WARNING,
                "Skipping diskless-load because there are modules that are not aware of async replication.");
            enabled = 0;
        }
    }
    return enabled;
}

/* Helper function for readSyncBulkPayload() to initialize tempDb
 * before socket-loading the new db from master. The tempDb may be populated
 * by swapMainDbWithTempDb or freed by disklessLoadDiscardTempDb later. */
/**readSyncBulkPayload() 的帮助函数，用于在从 master 套接字加载新数据库之前初始化 tempDb。
 * tempDb 可能由 swapMainDbWithTempDb 填充或稍后由 disklessLoadDiscardTempDb 释放。*/
redisDb *disklessLoadInitTempDb(void) {
    return initTempDb();
}

/* Helper function for readSyncBulkPayload() to discard our tempDb
 * when the loading succeeded or failed. */
//readSyncBulkPayload() 的辅助函数在加载成功或失败时丢弃我们的 tempDb。
void disklessLoadDiscardTempDb(redisDb *tempDb) {
    discardTempDb(tempDb, replicationEmptyDbCallback);
}

/* If we know we got an entirely different data set from our master
 * we have no way to incrementally feed our replicas after that.
 * We want our replicas to resync with us as well, if we have any sub-replicas.
 * This is useful on readSyncBulkPayload in places where we just finished transferring db. */
/**如果我们知道我们从 master 获得了一个完全不同的数据集，那么之后我们就无法增量地提供我们的副本。
 * 如果我们有任何子副本，我们也希望我们的副本与我们重新同步。
 * 这在我们刚刚完成传输数据库的地方对 readSyncBulkPayload 很有用。*/
void replicationAttachToNewMaster() { 
    /* Replica starts to apply data from new master, we must discard the cached
     * master structure. */
    serverAssert(server.master == NULL);
    replicationDiscardCachedMaster();

    disconnectSlaves(); /* Force our replicas to resync with us as well. */
    freeReplicationBacklog(); /* Don't allow our chained replicas to PSYNC. */
}

/* Asynchronously read the SYNC payload we receive from a master */
//异步读取我们从主机接收到的 SYNC 有效负载
#define REPL_MAX_WRITTEN_BEFORE_FSYNC (1024*1024*8) /* 8 MB */
void readSyncBulkPayload(connection *conn) {
    char buf[PROTO_IOBUF_LEN];
    ssize_t nread, readlen, nwritten;
    int use_diskless_load = useDisklessLoad();
    redisDb *diskless_load_tempDb = NULL;
    functionsLibCtx* temp_functions_lib_ctx = NULL;
    int empty_db_flags = server.repl_slave_lazy_flush ? EMPTYDB_ASYNC :
                                                        EMPTYDB_NO_FLAGS;
    off_t left;

    /* Static vars used to hold the EOF mark, and the last bytes received
     * from the server: when they match, we reached the end of the transfer. */
    static char eofmark[CONFIG_RUN_ID_SIZE];
    static char lastbytes[CONFIG_RUN_ID_SIZE];
    static int usemark = 0;

    /* If repl_transfer_size == -1 we still have to read the bulk length
     * from the master reply. */
    if (server.repl_transfer_size == -1) {
        nread = connSyncReadLine(conn,buf,1024,server.repl_syncio_timeout*1000);
        if (nread == -1) {
            serverLog(LL_WARNING,
                "I/O error reading bulk count from MASTER: %s",
                strerror(errno));
            goto error;
        } else {
            /* nread here is returned by connSyncReadLine(), which calls syncReadLine() and
             * convert "\r\n" to '\0' so 1 byte is lost. */
            atomicIncr(server.stat_net_repl_input_bytes, nread+1);
        }

        if (buf[0] == '-') {
            serverLog(LL_WARNING,
                "MASTER aborted replication with an error: %s",
                buf+1);
            goto error;
        } else if (buf[0] == '\0') {
            /* At this stage just a newline works as a PING in order to take
             * the connection live. So we refresh our last interaction
             * timestamp. */
            /**在这个阶段，只有一个换行符作为一个 PING 来激活连接。所以我们刷新了我们的最后一次交互时间戳。*/
            server.repl_transfer_lastio = server.unixtime;
            return;
        } else if (buf[0] != '$') {
            serverLog(LL_WARNING,"Bad protocol from MASTER, the first byte is not '$' (we received '%s'), are you sure the host and port are right?", buf);
            goto error;
        }

        /* There are two possible forms for the bulk payload. One is the
         * usual $<count> bulk format. The other is used for diskless transfers
         * when the master does not know beforehand the size of the file to
         * transfer. In the latter case, the following format is used:
         * 批量有效负载有两种可能的形式。一种是通常的 <count> 批量格式。另一个用于无盘传输，
         * 当主机事先不知道要传输的文件大小时。在后一种情况下，使用以下格式：
         *
         * $EOF:<40 bytes delimiter>
         *
         * At the end of the file the announced delimiter is transmitted. The
         * delimiter is long and random enough that the probability of a
         * collision with the actual file content can be ignored.
         * 在文件的末尾传输宣布的分隔符。分隔符足够长且随机，以至于可以忽略与实际文件内容发生冲突的可能性。*/
        if (strncmp(buf+1,"EOF:",4) == 0 && strlen(buf+5) >= CONFIG_RUN_ID_SIZE) {
            usemark = 1;
            memcpy(eofmark,buf+5,CONFIG_RUN_ID_SIZE);
            memset(lastbytes,0,CONFIG_RUN_ID_SIZE);
            /* Set any repl_transfer_size to avoid entering this code path
             * at the next call. */
            /**设置任何 repl_transfer_size 以避免在下次调用时输入此代码路径。*/
            server.repl_transfer_size = 0;
            serverLog(LL_NOTICE,
                "MASTER <-> REPLICA sync: receiving streamed RDB from master with EOF %s",
                use_diskless_load? "to parser":"to disk");
        } else {
            usemark = 0;
            server.repl_transfer_size = strtol(buf+1,NULL,10);
            serverLog(LL_NOTICE,
                "MASTER <-> REPLICA sync: receiving %lld bytes from master %s",
                (long long) server.repl_transfer_size,
                use_diskless_load? "to parser":"to disk");
        }
        return;
    }

    if (!use_diskless_load) {
        /* Read the data from the socket, store it to a file and search
         * for the EOF. */
        if (usemark) {
            readlen = sizeof(buf);
        } else {
            left = server.repl_transfer_size - server.repl_transfer_read;
            readlen = (left < (signed)sizeof(buf)) ? left : (signed)sizeof(buf);
        }

        nread = connRead(conn,buf,readlen);
        if (nread <= 0) {
            if (connGetState(conn) == CONN_STATE_CONNECTED) {
                /* equivalent to EAGAIN */
                return;
            }
            serverLog(LL_WARNING,"I/O error trying to sync with MASTER: %s",
                (nread == -1) ? strerror(errno) : "connection lost");
            cancelReplicationHandshake(1);
            return;
        }
        atomicIncr(server.stat_net_repl_input_bytes, nread);

        /* When a mark is used, we want to detect EOF asap in order to avoid
         * writing the EOF mark into the file... */
        int eof_reached = 0;

        if (usemark) {
            /* Update the last bytes array, and check if it matches our
             * delimiter. */
            if (nread >= CONFIG_RUN_ID_SIZE) {
                memcpy(lastbytes,buf+nread-CONFIG_RUN_ID_SIZE,
                       CONFIG_RUN_ID_SIZE);
            } else {
                int rem = CONFIG_RUN_ID_SIZE-nread;
                memmove(lastbytes,lastbytes+nread,rem);
                memcpy(lastbytes+rem,buf,nread);
            }
            if (memcmp(lastbytes,eofmark,CONFIG_RUN_ID_SIZE) == 0)
                eof_reached = 1;
        }

        /* Update the last I/O time for the replication transfer (used in
         * order to detect timeouts during replication), and write what we
         * got from the socket to the dump file on disk. */
        /**更新复制传输的最后 IO 时间（用于检测复制期间的超时），并将我们从套接字获得的内容写入磁盘上的转储文件。*/
        server.repl_transfer_lastio = server.unixtime;
        if ((nwritten = write(server.repl_transfer_fd,buf,nread)) != nread) {
            serverLog(LL_WARNING,
                "Write error or short write writing to the DB dump file "
                "needed for MASTER <-> REPLICA synchronization: %s",
                (nwritten == -1) ? strerror(errno) : "short write");
            goto error;
        }
        server.repl_transfer_read += nread;

        /* Delete the last 40 bytes from the file if we reached EOF. */
        if (usemark && eof_reached) {
            if (ftruncate(server.repl_transfer_fd,
                server.repl_transfer_read - CONFIG_RUN_ID_SIZE) == -1)
            {
                serverLog(LL_WARNING,
                    "Error truncating the RDB file received from the master "
                    "for SYNC: %s", strerror(errno));
                goto error;
            }
        }

        /* Sync data on disk from time to time, otherwise at the end of the
         * transfer we may suffer a big delay as the memory buffers are copied
         * into the actual disk. */
        /**不时同步磁盘上的数据，否则在传输结束时我们可能会遇到很大的延迟，因为内存缓冲区被复制到实际磁盘中。*/
        if (server.repl_transfer_read >=
            server.repl_transfer_last_fsync_off + REPL_MAX_WRITTEN_BEFORE_FSYNC)
        {
            off_t sync_size = server.repl_transfer_read -
                              server.repl_transfer_last_fsync_off;
            rdb_fsync_range(server.repl_transfer_fd,
                server.repl_transfer_last_fsync_off, sync_size);
            server.repl_transfer_last_fsync_off += sync_size;
        }

        /* Check if the transfer is now complete */
        if (!usemark) {
            if (server.repl_transfer_read == server.repl_transfer_size)
                eof_reached = 1;
        }

        /* If the transfer is yet not complete, we need to read more, so
         * return ASAP and wait for the handler to be called again. */
        /**如果传输尚未完成，我们需要阅读更多内容，因此尽快返回并等待再次调用处理程序。*/
        if (!eof_reached) return;
    }

    /* We reach this point in one of the following cases:
     * 我们在以下情况之一中达到这一点：
     *
     * 1. The replica is using diskless replication, that is, it reads data
     *    directly from the socket to the Redis memory, without using
     *    a temporary RDB file on disk. In that case we just block and
     *    read everything from the socket.
     *    副本是使用无盘复制，即直接从socket读取数据到Redis内存，不使用磁盘上的临时RDB文件。
     *    在这种情况下，我们只是阻塞并从套接字读取所有内容。
     *
     * 2. Or when we are done reading from the socket to the RDB file, in
     *    such case we want just to read the RDB file in memory.
     *    或者当我们完成从套接字读取到 RDB 文件时，在这种情况下，我们只想读取内存中的 RDB 文件。*/

    /* We need to stop any AOF rewriting child before flushing and parsing
     * the RDB, otherwise we'll create a copy-on-write disaster. */
    /**在刷新和解析 RDB 之前，我们需要停止任何 AOF 重写子节点，否则我们将造成写时复制灾难。*/
    if (server.aof_state != AOF_OFF) stopAppendOnly();
    /* Also try to stop save RDB child before flushing and parsing the RDB:
     * 1. Ensure background save doesn't overwrite synced data after being loaded.
     * 2. Avoid copy-on-write disaster. */
    /**还尝试在刷新和解析 RDB 之前停止保存 RDB 子节点：
     * 1. 确保后台保存在加载后不会覆盖同步数据。
     * 2.避免写时复制灾难。*/
    if (server.child_type == CHILD_TYPE_RDB) {
        if (!use_diskless_load) {
            serverLog(LL_NOTICE,
                "Replica is about to load the RDB file received from the "
                "master, but there is a pending RDB child running. "
                "Killing process %ld and removing its temp file to avoid "
                "any race",
                (long) server.child_pid);
        }
        killRDBChild();
    }

    if (use_diskless_load && server.repl_diskless_load == REPL_DISKLESS_LOAD_SWAPDB) {
        /* Initialize empty tempDb dictionaries. */
        diskless_load_tempDb = disklessLoadInitTempDb();
        temp_functions_lib_ctx = functionsLibCtxCreate();

        moduleFireServerEvent(REDISMODULE_EVENT_REPL_ASYNC_LOAD,
                              REDISMODULE_SUBEVENT_REPL_ASYNC_LOAD_STARTED,
                              NULL);
    } else {
        replicationAttachToNewMaster();

        serverLog(LL_NOTICE, "MASTER <-> REPLICA sync: Flushing old data");
        emptyData(-1,empty_db_flags,replicationEmptyDbCallback);
    }

    /* Before loading the DB into memory we need to delete the readable
     * handler, otherwise it will get called recursively since
     * rdbLoad() will call the event loop to process events from time to
     * time for non blocking loading. */
    /**在将数据库加载到内存之前，我们需要删除可读处理程序，否则它将被递归调用，
     * 因为 rdbLoad() 将不时调用事件循环来处理事件以进行非阻塞加载。*/
    connSetReadHandler(conn, NULL);
    
    serverLog(LL_NOTICE, "MASTER <-> REPLICA sync: Loading DB in memory");
    rdbSaveInfo rsi = RDB_SAVE_INFO_INIT;
    if (use_diskless_load) {
        rio rdb;
        redisDb *dbarray;
        functionsLibCtx* functions_lib_ctx;
        int asyncLoading = 0;

        if (server.repl_diskless_load == REPL_DISKLESS_LOAD_SWAPDB) {
            /* Async loading means we continue serving read commands during full resync, and
             * "swap" the new db with the old db only when loading is done.
             * It is enabled only on SWAPDB diskless replication when master replication ID hasn't changed,
             * because in that state the old content of the db represents a different point in time of the same
             * data set we're currently receiving from the master. */
            /**异步加载意味着我们在完全重新同步期间继续提供读取命令，并且仅在加载完成时将新数据库与旧数据库“交换”。
             * 它仅在主复制 ID 未更改时在 SWAPDB 无盘复制上启用，因为在该状态下，
             * 数据库的旧内容代表我们当前从主服务器接收的同一数据集的不同时间点。*/
            if (memcmp(server.replid, server.master_replid, CONFIG_RUN_ID_SIZE) == 0) {
                asyncLoading = 1;
            }
            dbarray = diskless_load_tempDb;
            functions_lib_ctx = temp_functions_lib_ctx;
        } else {
            dbarray = server.db;
            functions_lib_ctx = functionsLibCtxGetCurrent();
            functionsLibCtxClear(functions_lib_ctx);
        }

        rioInitWithConn(&rdb,conn,server.repl_transfer_size);

        /* Put the socket in blocking mode to simplify RDB transfer.
         * We'll restore it when the RDB is received. */
        connBlock(conn);
        connRecvTimeout(conn, server.repl_timeout*1000);
        startLoading(server.repl_transfer_size, RDBFLAGS_REPLICATION, asyncLoading);

        int loadingFailed = 0;
        rdbLoadingCtx loadingCtx = { .dbarray = dbarray, .functions_lib_ctx = functions_lib_ctx };
        if (rdbLoadRioWithLoadingCtx(&rdb,RDBFLAGS_REPLICATION,&rsi,&loadingCtx) != C_OK) {
            /* RDB loading failed. */
            serverLog(LL_WARNING,
                      "Failed trying to load the MASTER synchronization DB "
                      "from socket: %s", strerror(errno));
            loadingFailed = 1;
        } else if (usemark) {
            /* Verify the end mark is correct. */
            if (!rioRead(&rdb, buf, CONFIG_RUN_ID_SIZE) ||
                memcmp(buf, eofmark, CONFIG_RUN_ID_SIZE) != 0)
            {
                serverLog(LL_WARNING, "Replication stream EOF marker is broken");
                loadingFailed = 1;
            }
        }

        if (loadingFailed) {
            stopLoading(0);
            cancelReplicationHandshake(1);
            rioFreeConn(&rdb, NULL);

            if (server.repl_diskless_load == REPL_DISKLESS_LOAD_SWAPDB) {
                /* Discard potentially partially loaded tempDb. */
                moduleFireServerEvent(REDISMODULE_EVENT_REPL_ASYNC_LOAD,
                                      REDISMODULE_SUBEVENT_REPL_ASYNC_LOAD_ABORTED,
                                      NULL);

                disklessLoadDiscardTempDb(diskless_load_tempDb);
                functionsLibCtxFree(temp_functions_lib_ctx);
                serverLog(LL_NOTICE, "MASTER <-> REPLICA sync: Discarding temporary DB in background");
            } else {
                /* Remove the half-loaded data in case we started with an empty replica. */
                emptyData(-1,empty_db_flags,replicationEmptyDbCallback);
            }

            /* Note that there's no point in restarting the AOF on SYNC
             * failure, it'll be restarted when sync succeeds or the replica
             * gets promoted. */
            /**请注意，在 SYNC 失败时重新启动 AOF 是没有意义的，它将在同步成功或副本被提升时重新启动。*/
            return;
        }

        /* RDB loading succeeded if we reach this point. */
        if (server.repl_diskless_load == REPL_DISKLESS_LOAD_SWAPDB) {
            /* We will soon swap main db with tempDb and replicas will start
             * to apply data from new master, we must discard the cached
             * master structure and force resync of sub-replicas. */
            /**我们很快将用 tempDb 交换主数据库，副本将开始应用来自新主数据库的数据，
             * 我们必须丢弃缓存的主数据库结构并强制重新同步子副本。*/
            replicationAttachToNewMaster();

            serverLog(LL_NOTICE, "MASTER <-> REPLICA sync: Swapping active DB with loaded DB");
            swapMainDbWithTempDb(diskless_load_tempDb);

            /* swap existing functions ctx with the temporary one */
            functionsLibCtxSwapWithCurrent(temp_functions_lib_ctx);

            moduleFireServerEvent(REDISMODULE_EVENT_REPL_ASYNC_LOAD,
                        REDISMODULE_SUBEVENT_REPL_ASYNC_LOAD_COMPLETED,
                        NULL);

            /* Delete the old db as it's useless now. */
            disklessLoadDiscardTempDb(diskless_load_tempDb);
            serverLog(LL_NOTICE, "MASTER <-> REPLICA sync: Discarding old DB in background");
        }

        /* Inform about db change, as replication was diskless and didn't cause a save. */
        server.dirty++;

        stopLoading(1);

        /* Cleanup and restore the socket to the original state to continue
         * with the normal replication. */
        /**清理并将套接字恢复到原始状态以继续正常复制。*/
        rioFreeConn(&rdb, NULL);
        connNonBlock(conn);
        connRecvTimeout(conn,0);
    } else {

        /* Make sure the new file (also used for persistence) is fully synced
         * (not covered by earlier calls to rdb_fsync_range). */
        /**确保新文件（也用于持久性）完全同步（之前对 rdb_fsync_range 的调用未涵盖）。*/
        if (fsync(server.repl_transfer_fd) == -1) {
            serverLog(LL_WARNING,
                "Failed trying to sync the temp DB to disk in "
                "MASTER <-> REPLICA synchronization: %s",
                strerror(errno));
            cancelReplicationHandshake(1);
            return;
        }

        /* Rename rdb like renaming rewrite aof asynchronously. */
        int old_rdb_fd = open(server.rdb_filename,O_RDONLY|O_NONBLOCK);
        if (rename(server.repl_transfer_tmpfile,server.rdb_filename) == -1) {
            serverLog(LL_WARNING,
                "Failed trying to rename the temp DB into %s in "
                "MASTER <-> REPLICA synchronization: %s",
                server.rdb_filename, strerror(errno));
            cancelReplicationHandshake(1);
            if (old_rdb_fd != -1) close(old_rdb_fd);
            return;
        }
        /* Close old rdb asynchronously. */
        if (old_rdb_fd != -1) bioCreateCloseJob(old_rdb_fd);

        /* Sync the directory to ensure rename is persisted */
        if (fsyncFileDir(server.rdb_filename) == -1) {
            serverLog(LL_WARNING,
                "Failed trying to sync DB directory %s in "
                "MASTER <-> REPLICA synchronization: %s",
                server.rdb_filename, strerror(errno));
            cancelReplicationHandshake(1);
            return;
        }

        if (rdbLoad(server.rdb_filename,&rsi,RDBFLAGS_REPLICATION) != C_OK) {
            serverLog(LL_WARNING,
                "Failed trying to load the MASTER synchronization "
                "DB from disk: %s", strerror(errno));
            cancelReplicationHandshake(1);
            if (server.rdb_del_sync_files && allPersistenceDisabled()) {
                serverLog(LL_NOTICE,"Removing the RDB file obtained from "
                                    "the master. This replica has persistence "
                                    "disabled");
                bg_unlink(server.rdb_filename);
            }
            /* Note that there's no point in restarting the AOF on sync failure,
               it'll be restarted when sync succeeds or replica promoted. */
            /**请注意，在同步失败时重新启动 AOF 是没有意义的，它将在同步成功或副本提升时重新启动。*/
            return;
        }

        /* Cleanup. */
        if (server.rdb_del_sync_files && allPersistenceDisabled()) {
            serverLog(LL_NOTICE,"Removing the RDB file obtained from "
                                "the master. This replica has persistence "
                                "disabled");
            bg_unlink(server.rdb_filename);
        }

        zfree(server.repl_transfer_tmpfile);
        close(server.repl_transfer_fd);
        server.repl_transfer_fd = -1;
        server.repl_transfer_tmpfile = NULL;
    }

    /* Final setup of the connected slave <- master link */
    replicationCreateMasterClient(server.repl_transfer_s,rsi.repl_stream_db);
    server.repl_state = REPL_STATE_CONNECTED;
    server.repl_down_since = 0;

    /* Fire the master link modules event. */
    moduleFireServerEvent(REDISMODULE_EVENT_MASTER_LINK_CHANGE,
                          REDISMODULE_SUBEVENT_MASTER_LINK_UP,
                          NULL);

    /* After a full resynchronization we use the replication ID and
     * offset of the master. The secondary ID / offset are cleared since
     * we are starting a new history. */
    /**完全重新同步后，我们使用主服务器的复制 ID 和偏移量。由于我们正在开始新的历史记录，因此清除了辅助 ID 偏移量。*/
    memcpy(server.replid,server.master->replid,sizeof(server.replid));
    server.master_repl_offset = server.master->reploff;
    clearReplicationId2();

    /* Let's create the replication backlog if needed. Slaves need to
     * accumulate the backlog regardless of the fact they have sub-slaves
     * or not, in order to behave correctly if they are promoted to
     * masters after a failover. */
    /**如果需要，让我们创建复制积压。从站需要累积积压，无论它们是否有子从站，以便在故障转移后升级为主站时行为正确。*/
    if (server.repl_backlog == NULL) createReplicationBacklog();
    serverLog(LL_NOTICE, "MASTER <-> REPLICA sync: Finished with success");

    if (server.supervised_mode == SUPERVISED_SYSTEMD) {
        redisCommunicateSystemd("STATUS=MASTER <-> REPLICA sync: Finished with success. Ready to accept connections in read-write mode.\n");
    }

    /* Send the initial ACK immediately to put this replica in online state. */
    if (usemark) replicationSendAck();

    /* Restart the AOF subsystem now that we finished the sync. This
     * will trigger an AOF rewrite, and when done will start appending
     * to the new file. */
    /**完成同步后，重新启动 AOF 子系统。这将触发 AOF 重写，完成后将开始追加到新文件。*/
    if (server.aof_enabled) restartAOFAfterSYNC();
    return;

error:
    cancelReplicationHandshake(1);
    return;
}

char *receiveSynchronousResponse(connection *conn) {
    char buf[256];
    /* Read the reply from the server. */
    if (connSyncReadLine(conn,buf,sizeof(buf),server.repl_syncio_timeout*1000) == -1)
    {
        serverLog(LL_WARNING, "Failed to read response from the server: %s", strerror(errno));
        return NULL;
    }
    server.repl_transfer_lastio = server.unixtime;
    return sdsnew(buf);
}

/* Send a pre-formatted multi-bulk command to the connection. */
//向连接发送预先格式化的多批量命令。
char* sendCommandRaw(connection *conn, sds cmd) {
    if (connSyncWrite(conn,cmd,sdslen(cmd),server.repl_syncio_timeout*1000) == -1) {
        return sdscatprintf(sdsempty(),"-Writing to master: %s",
                connGetLastError(conn));
    }
    return NULL;
}

/* Compose a multi-bulk command and send it to the connection.
 * Used to send AUTH and REPLCONF commands to the master before starting the
 * replication.
 *
 * Takes a list of char* arguments, terminated by a NULL argument.
 *
 * The command returns an sds string representing the result of the
 * operation. On error the first byte is a "-".
 */
/**编写多批量命令并将其发送到连接。用于在开始复制之前向主服务器发送 AUTH 和 REPLCONF 命令。
 * 采用 char 参数列表，以 NULL 参数终止。该命令返回一个表示操作结果的 sds 字符串。出错时第一个字节是“-”。*/
char *sendCommand(connection *conn, ...) {
    va_list ap;
    sds cmd = sdsempty();
    sds cmdargs = sdsempty();
    size_t argslen = 0;
    char *arg;

    /* Create the command to send to the master, we use redis binary
     * protocol to make sure correct arguments are sent. This function
     * is not safe for all binary data. */
    /**创建发送到主服务器的命令，我们使用 redis 二进制协议来确保发送正确的参数。此函数对所有二进制数据都不安全。*/
    va_start(ap,conn);
    while(1) {
        arg = va_arg(ap, char*);
        if (arg == NULL) break;
        cmdargs = sdscatprintf(cmdargs,"$%zu\r\n%s\r\n",strlen(arg),arg);
        argslen++;
    }

    cmd = sdscatprintf(cmd,"*%zu\r\n",argslen);
    cmd = sdscatsds(cmd,cmdargs);
    sdsfree(cmdargs);

    va_end(ap);
    char* err = sendCommandRaw(conn, cmd);
    sdsfree(cmd);
    if(err)
        return err;
    return NULL;
}

/* Compose a multi-bulk command and send it to the connection. 
 * Used to send AUTH and REPLCONF commands to the master before starting the
 * replication.
 *
 * argv_lens is optional, when NULL, strlen is used.
 *
 * The command returns an sds string representing the result of the
 * operation. On error the first byte is a "-".
 */
/**编写多批量命令并将其发送到连接。用于在开始复制之前向主服务器发送 AUTH 和 REPLCONF 命令。
 * argv_lens 是可选的，当为 NULL 时，使用 strlen。该命令返回一个表示操作结果的 sds 字符串。出错时第一个字节是“-”。*/
char *sendCommandArgv(connection *conn, int argc, char **argv, size_t *argv_lens) {
    sds cmd = sdsempty();
    char *arg;
    int i;

    /* Create the command to send to the master. */
    cmd = sdscatfmt(cmd,"*%i\r\n",argc);
    for (i=0; i<argc; i++) {
        int len;
        arg = argv[i];
        len = argv_lens ? argv_lens[i] : strlen(arg);
        cmd = sdscatfmt(cmd,"$%i\r\n",len);
        cmd = sdscatlen(cmd,arg,len);
        cmd = sdscatlen(cmd,"\r\n",2);
    }
    char* err = sendCommandRaw(conn, cmd);
    sdsfree(cmd);
    if (err)
        return err;
    return NULL;
}

/* Try a partial resynchronization with the master if we are about to reconnect.
 * If there is no cached master structure, at least try to issue a
 * "PSYNC ? -1" command in order to trigger a full resync using the PSYNC
 * command in order to obtain the master replid and the master replication
 * global offset.
 *
 * This function is designed to be called from syncWithMaster(), so the
 * following assumptions are made:
 *
 * 1) We pass the function an already connected socket "fd".
 * 2) This function does not close the file descriptor "fd". However in case
 *    of successful partial resynchronization, the function will reuse
 *    'fd' as file descriptor of the server.master client structure.
 *
 * The function is split in two halves: if read_reply is 0, the function
 * writes the PSYNC command on the socket, and a new function call is
 * needed, with read_reply set to 1, in order to read the reply of the
 * command. This is useful in order to support non blocking operations, so
 * that we write, return into the event loop, and read when there are data.
 *
 * When read_reply is 0 the function returns PSYNC_WRITE_ERR if there
 * was a write error, or PSYNC_WAIT_REPLY to signal we need another call
 * with read_reply set to 1. However even when read_reply is set to 1
 * the function may return PSYNC_WAIT_REPLY again to signal there were
 * insufficient data to read to complete its work. We should re-enter
 * into the event loop and wait in such a case.
 *
 * The function returns:
 *
 * PSYNC_CONTINUE: If the PSYNC command succeeded and we can continue.
 * PSYNC_FULLRESYNC: If PSYNC is supported but a full resync is needed.
 *                   In this case the master replid and global replication
 *                   offset is saved.
 * PSYNC_NOT_SUPPORTED: If the server does not understand PSYNC at all and
 *                      the caller should fall back to SYNC.
 * PSYNC_WRITE_ERROR: There was an error writing the command to the socket.
 * PSYNC_WAIT_REPLY: Call again the function with read_reply set to 1.
 * PSYNC_TRY_LATER: Master is currently in a transient error condition.
 *
 * Notable side effects:
 *
 * 1) As a side effect of the function call the function removes the readable
 *    event handler from "fd", unless the return value is PSYNC_WAIT_REPLY.
 * 2) server.master_initial_offset is set to the right value according
 *    to the master reply. This will be used to populate the 'server.master'
 *    structure replication offset.
 */
/**
 * 如果我们要重新连接，请尝试与主服务器进行部分重新同步。
 * 如果没有缓存的主结构，至少尝试发出“PSYNC ? -1”命令，以便使用 PSYNC 命令触发完全重新同步，以获得主副本和主副本全局偏移量。
 * 该函数被设计为从syncWithMaster() 调用，因此做出以下假设：
 *   1) 我们向该函数传递一个已经连接的套接字“fd”。
 *   2) 该函数不会关闭文件描述符“fd”。但是，如果部分重新同步成功，
 *      该函数将重用“fd”作为 server.master 客户端结构的文件描述符。
 * 该函数分为两部分：如果 read_reply 为 0，则该函数将 PSYNC 命令写入套接字，并且需要一个新的函数调用，
 * 并将 read_reply 设置为 1，以便读取命令的回复。这对于支持非阻塞操作很有用，以便我们写入，返回到事件循环，并在有数据时读取。
 * 当 read_reply 为 0 时，如果存在写入错误，该函数返回 PSYNC_WRITE_ERR，或者 PSYNC_WAIT_REPLY 表示我们需要另一个调用，
 * 并将 read_reply 设置为 1。然而，即使 read_reply 设置为 1，
 * 该函数也可能再次返回 PSYNC_WAIT_REPLY 以表示数据不足阅读以完成其工作。在这种情况下，我们应该重新进入事件循环并等待。
 * 函数返回：
 *   PSYNC_CONTINUE：如果 PSYNC 命令成功，我们可以继续。
 *   PSYNC_FULLRESYNC：如果支持 PSYNC 但需要完全重新同步。在这种情况下，保存了主副本和全局复制偏移量。
 *   PSYNC_NOT_SUPPORTED：如果服务器根本不理解 PSYNC，调用者应该回退到 SYNC。
 *   PSYNC_WRITE_ERROR：将命令写入套接字时出错。
 *   PSYNC_WAIT_REPLY：再次调用 read_reply 设置为 1 的函数。
 *   PSYNC_TRY_LATER：Master 当前处于瞬态错误状态。
 * 显着的副作用：
 *   1) 作为函数调用的副作用，函数从“fd”中删除可读事件处理程序，除非返回值为 PSYNC_WAIT_REPLY。
 *   2) server.master_initial_offset 根据master回复设置为正确的值。这将用于填充“server.master”结构复制偏移量。
 * */

#define PSYNC_WRITE_ERROR 0
#define PSYNC_WAIT_REPLY 1
#define PSYNC_CONTINUE 2
#define PSYNC_FULLRESYNC 3
#define PSYNC_NOT_SUPPORTED 4
#define PSYNC_TRY_LATER 5
int slaveTryPartialResynchronization(connection *conn, int read_reply) {
    char *psync_replid;
    char psync_offset[32];
    sds reply;

    /* Writing half */
    if (!read_reply) {
        /* Initially set master_initial_offset to -1 to mark the current
         * master replid and offset as not valid. Later if we'll be able to do
         * a FULL resync using the PSYNC command we'll set the offset at the
         * right value, so that this information will be propagated to the
         * client structure representing the master into server.master. */
        /**最初将 master_initial_offset 设置为 -1 以将当前的 master replid 和 offset 标记为无效。
         * 稍后，如果我们能够使用 PSYNC 命令进行 FULL 重新同步，我们会将偏移量设置为正确的值，
         * 以便此信息将传播到代表 master 的客户端结构到 server.master 中。*/
        server.master_initial_offset = -1;

        if (server.cached_master) {
            psync_replid = server.cached_master->replid;
            snprintf(psync_offset,sizeof(psync_offset),"%lld", server.cached_master->reploff+1);
            serverLog(LL_NOTICE,"Trying a partial resynchronization (request %s:%s).", psync_replid, psync_offset);
        } else {
            serverLog(LL_NOTICE,"Partial resynchronization not possible (no cached master)");
            psync_replid = "?";
            memcpy(psync_offset,"-1",3);
        }

        /* Issue the PSYNC command, if this is a master with a failover in
         * progress then send the failover argument to the replica to cause it
         * to become a master */
        if (server.failover_state == FAILOVER_IN_PROGRESS) {
            reply = sendCommand(conn,"PSYNC",psync_replid,psync_offset,"FAILOVER",NULL);
        } else {
            reply = sendCommand(conn,"PSYNC",psync_replid,psync_offset,NULL);
        }

        if (reply != NULL) {
            serverLog(LL_WARNING,"Unable to send PSYNC to master: %s",reply);
            sdsfree(reply);
            connSetReadHandler(conn, NULL);
            return PSYNC_WRITE_ERROR;
        }
        return PSYNC_WAIT_REPLY;
    }

    /* Reading half */
    reply = receiveSynchronousResponse(conn);
    /* Master did not reply to PSYNC */
    if (reply == NULL) {
        connSetReadHandler(conn, NULL);
        serverLog(LL_WARNING, "Master did not reply to PSYNC, will try later");
        return PSYNC_TRY_LATER;
    }

    if (sdslen(reply) == 0) {
        /* The master may send empty newlines after it receives PSYNC
         * and before to reply, just to keep the connection alive. */
        sdsfree(reply);
        return PSYNC_WAIT_REPLY;
    }

    connSetReadHandler(conn, NULL);

    if (!strncmp(reply,"+FULLRESYNC",11)) {
        char *replid = NULL, *offset = NULL;

        /* FULL RESYNC, parse the reply in order to extract the replid
         * and the replication offset. */
        replid = strchr(reply,' ');
        if (replid) {
            replid++;
            offset = strchr(replid,' ');
            if (offset) offset++;
        }
        if (!replid || !offset || (offset-replid-1) != CONFIG_RUN_ID_SIZE) {
            serverLog(LL_WARNING,
                "Master replied with wrong +FULLRESYNC syntax.");
            /* This is an unexpected condition, actually the +FULLRESYNC
             * reply means that the master supports PSYNC, but the reply
             * format seems wrong. To stay safe we blank the master
             * replid to make sure next PSYNCs will fail. */
            /**这是一个意外情况，实际上+FULLRESYNC回复意味着master支持PSYNC，但回复格式似乎错误。
             * 为了安全起见，我们将 master replid 置空以确保下一个 PSYNC 将失败。*/
            memset(server.master_replid,0,CONFIG_RUN_ID_SIZE+1);
        } else {
            memcpy(server.master_replid, replid, offset-replid-1);
            server.master_replid[CONFIG_RUN_ID_SIZE] = '\0';
            server.master_initial_offset = strtoll(offset,NULL,10);
            serverLog(LL_NOTICE,"Full resync from master: %s:%lld",
                server.master_replid,
                server.master_initial_offset);
        }
        sdsfree(reply);
        return PSYNC_FULLRESYNC;
    }

    if (!strncmp(reply,"+CONTINUE",9)) {
        /* Partial resync was accepted. */
        serverLog(LL_NOTICE,
            "Successful partial resynchronization with master.");

        /* Check the new replication ID advertised by the master. If it
         * changed, we need to set the new ID as primary ID, and set
         * secondary ID as the old master ID up to the current offset, so
         * that our sub-slaves will be able to PSYNC with us after a
         * disconnection. */
        /**检查主服务器通告的新复制 ID。如果它发生了变化，我们需要将新的 ID 设置为主 ID，
         * 并将辅助 ID 设置为旧的主 ID，直到当前的偏移量，这样我们的子从机就可以在断开连接后与我们进行 PSYNC。*/
        char *start = reply+10;
        char *end = reply+9;
        while(end[0] != '\r' && end[0] != '\n' && end[0] != '\0') end++;
        if (end-start == CONFIG_RUN_ID_SIZE) {
            char new[CONFIG_RUN_ID_SIZE+1];
            memcpy(new,start,CONFIG_RUN_ID_SIZE);
            new[CONFIG_RUN_ID_SIZE] = '\0';

            if (strcmp(new,server.cached_master->replid)) {
                /* Master ID changed. */
                serverLog(LL_WARNING,"Master replication ID changed to %s",new);

                /* Set the old ID as our ID2, up to the current offset+1. */
                memcpy(server.replid2,server.cached_master->replid,
                    sizeof(server.replid2));
                server.second_replid_offset = server.master_repl_offset+1;

                /* Update the cached master ID and our own primary ID to the
                 * new one. */
                memcpy(server.replid,new,sizeof(server.replid));
                memcpy(server.cached_master->replid,new,sizeof(server.replid));

                /* Disconnect all the sub-slaves: they need to be notified. */
                disconnectSlaves();
            }
        }

        /* Setup the replication to continue. */
        sdsfree(reply);
        replicationResurrectCachedMaster(conn);

        /* If this instance was restarted and we read the metadata to
         * PSYNC from the persistence file, our replication backlog could
         * be still not initialized. Create it. */
        /**如果此实例重新启动并且我们从持久性文件中将元数据读取到 PSYNC，我们的复制积压可能仍未初始化。创造它。*/
        if (server.repl_backlog == NULL) createReplicationBacklog();
        return PSYNC_CONTINUE;
    }

    /* If we reach this point we received either an error (since the master does
     * not understand PSYNC or because it is in a special state and cannot
     * serve our request), or an unexpected reply from the master.
     * 如果我们到达这一点，我们要么收到错误（因为 master 不理解 PSYNC 或者因为它处于特殊状态并且无法满足我们的请求），
     * 要么收到来自 master 的意外回复。
     *
     * Return PSYNC_NOT_SUPPORTED on errors we don't understand, otherwise
     * return PSYNC_TRY_LATER if we believe this is a transient error.
     * 如果我们不理解错误，则返回 PSYNC_NOT_SUPPORTED，否则如果我们认为这是一个暂时性错误，则返回 PSYNC_TRY_LATER。*/

    if (!strncmp(reply,"-NOMASTERLINK",13) ||
        !strncmp(reply,"-LOADING",8))
    {
        serverLog(LL_NOTICE,
            "Master is currently unable to PSYNC "
            "but should be in the future: %s", reply);
        sdsfree(reply);
        return PSYNC_TRY_LATER;
    }

    if (strncmp(reply,"-ERR",4)) {
        /* If it's not an error, log the unexpected event. */
        serverLog(LL_WARNING,
            "Unexpected reply to PSYNC from master: %s", reply);
    } else {
        serverLog(LL_NOTICE,
            "Master does not support PSYNC or is in "
            "error state (reply: %s)", reply);
    }
    sdsfree(reply);
    return PSYNC_NOT_SUPPORTED;
}

/* This handler fires when the non blocking connect was able to
 * establish a connection with the master. */
//当非阻塞连接能够与主服务器建立连接时，将触发此处理程序。
void syncWithMaster(connection *conn) {
    char tmpfile[256], *err = NULL;
    int dfd = -1, maxtries = 5;
    int psync_result;

    /* If this event fired after the user turned the instance into a master
     * with SLAVEOF NO ONE we must just return ASAP. */
    /**如果在用户使用 SLAVEOF NO ONE 将实例变为主实例后触发此事件，我们必须尽快返回。*/
    if (server.repl_state == REPL_STATE_NONE) {
        connClose(conn);
        return;
    }

    /* Check for errors in the socket: after a non blocking connect() we
     * may find that the socket is in error state. */
    /**检查套接字中的错误：在非阻塞 connect() 之后，我们可能会发现套接字处于错误状态。*/
    if (connGetState(conn) != CONN_STATE_CONNECTED) {
        serverLog(LL_WARNING,"Error condition on socket for SYNC: %s",
                connGetLastError(conn));
        goto error;
    }

    /* Send a PING to check the master is able to reply without errors. */
    /**发送 PING 以检查主机是否能够无错误地回复。*/
    if (server.repl_state == REPL_STATE_CONNECTING) {
        serverLog(LL_NOTICE,"Non blocking connect for SYNC fired the event.");
        /* Delete the writable event so that the readable event remains
         * registered and we can wait for the PONG reply. */
        /**删除可写事件，以便可读事件保持注册状态，我们可以等待 PONG 回复。*/
        connSetReadHandler(conn, syncWithMaster);
        connSetWriteHandler(conn, NULL);
        server.repl_state = REPL_STATE_RECEIVE_PING_REPLY;
        /* Send the PING, don't check for errors at all, we have the timeout
         * that will take care about this. */
        /**发送 PING，根本不检查错误，我们有超时来处理这个问题。*/
        err = sendCommand(conn,"PING",NULL);
        if (err) goto write_error;
        return;
    }

    /* Receive the PONG command. */
    if (server.repl_state == REPL_STATE_RECEIVE_PING_REPLY) {
        err = receiveSynchronousResponse(conn);

        /* The master did not reply */
        if (err == NULL) goto no_response_error;

        /* We accept only two replies as valid, a positive +PONG reply
         * (we just check for "+") or an authentication error.
         * Note that older versions of Redis replied with "operation not
         * permitted" instead of using a proper error code, so we test
         * both. */
        /**我们只接受两个有效的回复，一个是肯定的 +PONG 回复（我们只检查“+”）或一个身份验证错误。
         * 请注意，旧版本的 Redis 回复“不允许操作”而不是使用正确的错误代码，因此我们对两者都进行了测试。*/
        if (err[0] != '+' &&
            strncmp(err,"-NOAUTH",7) != 0 &&
            strncmp(err,"-NOPERM",7) != 0 &&
            strncmp(err,"-ERR operation not permitted",28) != 0)
        {
            serverLog(LL_WARNING,"Error reply to PING from master: '%s'",err);
            sdsfree(err);
            goto error;
        } else {
            serverLog(LL_NOTICE,
                "Master replied to PING, replication can continue...");
        }
        sdsfree(err);
        err = NULL;
        server.repl_state = REPL_STATE_SEND_HANDSHAKE;
    }

    if (server.repl_state == REPL_STATE_SEND_HANDSHAKE) {
        /* AUTH with the master if required. */
        if (server.masterauth) {
            char *args[3] = {"AUTH",NULL,NULL};
            size_t lens[3] = {4,0,0};
            int argc = 1;
            if (server.masteruser) {
                args[argc] = server.masteruser;
                lens[argc] = strlen(server.masteruser);
                argc++;
            }
            args[argc] = server.masterauth;
            lens[argc] = sdslen(server.masterauth);
            argc++;
            err = sendCommandArgv(conn, argc, args, lens);
            if (err) goto write_error;
        }

        /* Set the slave port, so that Master's INFO command can list the
         * slave listening port correctly. */
        /**设置从端口，以便Master的INFO命令能够正确列出从监听端口。*/
        {
            int port;
            if (server.slave_announce_port)
                port = server.slave_announce_port;
            else if (server.tls_replication && server.tls_port)
                port = server.tls_port;
            else
                port = server.port;
            sds portstr = sdsfromlonglong(port);
            err = sendCommand(conn,"REPLCONF",
                    "listening-port",portstr, NULL);
            sdsfree(portstr);
            if (err) goto write_error;
        }

        /* Set the slave ip, so that Master's INFO command can list the
         * slave IP address port correctly in case of port forwarding or NAT.
         * Skip REPLCONF ip-address if there is no slave-announce-ip option set. */
        /**设置slave ip，以便Master的INFO命令在端口转发或NAT的情况下能正确列出slave IP地址端口。
         * 如果没有设置 slave-announce-ip 选项，则跳过 REPLCONF ip-address。*/
        if (server.slave_announce_ip) {
            err = sendCommand(conn,"REPLCONF",
                    "ip-address",server.slave_announce_ip, NULL);
            if (err) goto write_error;
        }

        /* Inform the master of our (slave) capabilities.
         *
         * EOF: supports EOF-style RDB transfer for diskless replication.
         * PSYNC2: supports PSYNC v2, so understands +CONTINUE <new repl ID>.
         *
         * The master will ignore capabilities it does not understand. */
        /**通知主人我们的（奴隶）能力。
         *
         * EOF：支持 EOF 样式的 RDB 传输以进行无盘复制。
         * PSYNC2：支持 PSYNC v2，所以理解 +CONTINUE <new repl ID>。
         *
         * master会忽略它不理解的能力。*/
        err = sendCommand(conn,"REPLCONF",
                "capa","eof","capa","psync2",NULL);
        if (err) goto write_error;

        server.repl_state = REPL_STATE_RECEIVE_AUTH_REPLY;
        return;
    }

    if (server.repl_state == REPL_STATE_RECEIVE_AUTH_REPLY && !server.masterauth)
        server.repl_state = REPL_STATE_RECEIVE_PORT_REPLY;

    /* Receive AUTH reply. */
    if (server.repl_state == REPL_STATE_RECEIVE_AUTH_REPLY) {
        err = receiveSynchronousResponse(conn);
        if (err == NULL) goto no_response_error;
        if (err[0] == '-') {
            serverLog(LL_WARNING,"Unable to AUTH to MASTER: %s",err);
            sdsfree(err);
            goto error;
        }
        sdsfree(err);
        err = NULL;
        server.repl_state = REPL_STATE_RECEIVE_PORT_REPLY;
        return;
    }

    /* Receive REPLCONF listening-port reply. */
    if (server.repl_state == REPL_STATE_RECEIVE_PORT_REPLY) {
        err = receiveSynchronousResponse(conn);
        if (err == NULL) goto no_response_error;
        /* Ignore the error if any, not all the Redis versions support
         * REPLCONF listening-port. */
        if (err[0] == '-') {
            serverLog(LL_NOTICE,"(Non critical) Master does not understand "
                                "REPLCONF listening-port: %s", err);
        }
        sdsfree(err);
        server.repl_state = REPL_STATE_RECEIVE_IP_REPLY;
        return;
    }

    if (server.repl_state == REPL_STATE_RECEIVE_IP_REPLY && !server.slave_announce_ip)
        server.repl_state = REPL_STATE_RECEIVE_CAPA_REPLY;

    /* Receive REPLCONF ip-address reply. */
    if (server.repl_state == REPL_STATE_RECEIVE_IP_REPLY) {
        err = receiveSynchronousResponse(conn);
        if (err == NULL) goto no_response_error;
        /* Ignore the error if any, not all the Redis versions support
         * REPLCONF ip-address. */
        if (err[0] == '-') {
            serverLog(LL_NOTICE,"(Non critical) Master does not understand "
                                "REPLCONF ip-address: %s", err);
        }
        sdsfree(err);
        server.repl_state = REPL_STATE_RECEIVE_CAPA_REPLY;
        return;
    }

    /* Receive CAPA reply. */
    if (server.repl_state == REPL_STATE_RECEIVE_CAPA_REPLY) {
        err = receiveSynchronousResponse(conn);
        if (err == NULL) goto no_response_error;
        /* Ignore the error if any, not all the Redis versions support
         * REPLCONF capa. */
        if (err[0] == '-') {
            serverLog(LL_NOTICE,"(Non critical) Master does not understand "
                                  "REPLCONF capa: %s", err);
        }
        sdsfree(err);
        err = NULL;
        server.repl_state = REPL_STATE_SEND_PSYNC;
    }

    /* Try a partial resynchronization. If we don't have a cached master
     * slaveTryPartialResynchronization() will at least try to use PSYNC
     * to start a full resynchronization so that we get the master replid
     * and the global offset, to try a partial resync at the next
     * reconnection attempt. */
    /**尝试部分重新同步。如果我们没有缓存的主服务器，slaveTryPartialResynchronization() 将
     * 至少尝试使用 PSYNC 启动完全重新同步，以便我们获得主副本和全局偏移量，以便在下一次重新连接尝试时尝试部分重新同步。*/
    if (server.repl_state == REPL_STATE_SEND_PSYNC) {
        if (slaveTryPartialResynchronization(conn,0) == PSYNC_WRITE_ERROR) {
            err = sdsnew("Write error sending the PSYNC command.");
            abortFailover("Write error to failover target");
            goto write_error;
        }
        server.repl_state = REPL_STATE_RECEIVE_PSYNC_REPLY;
        return;
    }

    /* If reached this point, we should be in REPL_STATE_RECEIVE_PSYNC_REPLY. */
    /**如果达到这一点，我们应该在 REPL_STATE_RECEIVE_PSYNC_REPLY。*/
    if (server.repl_state != REPL_STATE_RECEIVE_PSYNC_REPLY) {
        serverLog(LL_WARNING,"syncWithMaster(): state machine error, "
                             "state should be RECEIVE_PSYNC but is %d",
                             server.repl_state);
        goto error;
    }

    psync_result = slaveTryPartialResynchronization(conn,1);
    if (psync_result == PSYNC_WAIT_REPLY) return; /* Try again later... */

    /* Check the status of the planned failover. We expect PSYNC_CONTINUE,
     * but there is nothing technically wrong with a full resync which
     * could happen in edge cases. */
    /**检查计划故障转移的状态。我们期望 PSYNC_CONTINUE，但是在边缘情况下可能发生的完全重新同步在技术上没有任何问题。*/
    if (server.failover_state == FAILOVER_IN_PROGRESS) {
        if (psync_result == PSYNC_CONTINUE || psync_result == PSYNC_FULLRESYNC) {
            clearFailoverState();
        } else {
            abortFailover("Failover target rejected psync request");
            return;
        }
    }

    /* If the master is in an transient error, we should try to PSYNC
     * from scratch later, so go to the error path. This happens when
     * the server is loading the dataset or is not connected with its
     * master and so forth. */
    /**如果 master 出现暂时性错误，我们应该稍后尝试从头开始 PSYNC，因此转到错误路径。
     * 当服务器正在加载数据集或未与其主服务器连接等时会发生这种情况。*/
    if (psync_result == PSYNC_TRY_LATER) goto error;

    /* Note: if PSYNC does not return WAIT_REPLY, it will take care of
     * uninstalling the read handler from the file descriptor. */
    /**注意：如果 PSYNC 不返回 WAIT_REPLY，它将负责从文件描述符中卸载读取处理程序。*/

    if (psync_result == PSYNC_CONTINUE) {
        serverLog(LL_NOTICE, "MASTER <-> REPLICA sync: Master accepted a Partial Resynchronization.");
        if (server.supervised_mode == SUPERVISED_SYSTEMD) {
            redisCommunicateSystemd("STATUS=MASTER <-> REPLICA sync: Partial Resynchronization accepted. Ready to accept connections in read-write mode.\n");
        }
        return;
    }

    /* Fall back to SYNC if needed. Otherwise psync_result == PSYNC_FULLRESYNC
     * and the server.master_replid and master_initial_offset are
     * already populated. */
    /**如果需要，回退到 SYNC。否则 psync_result == PSYNC_FULLRESYNC 和
     * server.master_replid 和 master_initial_offset 已经被填充。*/
    if (psync_result == PSYNC_NOT_SUPPORTED) {
        serverLog(LL_NOTICE,"Retrying with SYNC...");
        if (connSyncWrite(conn,"SYNC\r\n",6,server.repl_syncio_timeout*1000) == -1) {
            serverLog(LL_WARNING,"I/O error writing to MASTER: %s",
                strerror(errno));
            goto error;
        }
    }

    /* Prepare a suitable temp file for bulk transfer */
    /**为批量传输准备合适的临时文件*/
    if (!useDisklessLoad()) {
        while(maxtries--) {
            snprintf(tmpfile,256,
                "temp-%d.%ld.rdb",(int)server.unixtime,(long int)getpid());
            dfd = open(tmpfile,O_CREAT|O_WRONLY|O_EXCL,0644);
            if (dfd != -1) break;
            sleep(1);
        }
        if (dfd == -1) {
            serverLog(LL_WARNING,"Opening the temp file needed for MASTER <-> REPLICA synchronization: %s",strerror(errno));
            goto error;
        }
        server.repl_transfer_tmpfile = zstrdup(tmpfile);
        server.repl_transfer_fd = dfd;
    }

    /* Setup the non blocking download of the bulk file. */
    /**设置批量文件的非阻塞下载。*/
    if (connSetReadHandler(conn, readSyncBulkPayload)
            == C_ERR)
    {
        char conninfo[CONN_INFO_LEN];
        serverLog(LL_WARNING,
            "Can't create readable event for SYNC: %s (%s)",
            strerror(errno), connGetInfo(conn, conninfo, sizeof(conninfo)));
        goto error;
    }

    server.repl_state = REPL_STATE_TRANSFER;
    server.repl_transfer_size = -1;
    server.repl_transfer_read = 0;
    server.repl_transfer_last_fsync_off = 0;
    server.repl_transfer_lastio = server.unixtime;
    return;

no_response_error: /* Handle receiveSynchronousResponse() error when master has no reply */
    serverLog(LL_WARNING, "Master did not respond to command during SYNC handshake");
    /* Fall through to regular error handling */
    /**进行常规错误处理*/

error:
    if (dfd != -1) close(dfd);
    connClose(conn);
    server.repl_transfer_s = NULL;
    if (server.repl_transfer_fd != -1)
        close(server.repl_transfer_fd);
    if (server.repl_transfer_tmpfile)
        zfree(server.repl_transfer_tmpfile);
    server.repl_transfer_tmpfile = NULL;
    server.repl_transfer_fd = -1;
    server.repl_state = REPL_STATE_CONNECT;
    return;

write_error: /* Handle sendCommand() errors. */
    serverLog(LL_WARNING,"Sending command to master in replication handshake: %s", err);
    sdsfree(err);
    goto error;
}

int connectWithMaster(void) {
    server.repl_transfer_s = server.tls_replication ? connCreateTLS() : connCreateSocket();
    if (connConnect(server.repl_transfer_s, server.masterhost, server.masterport,
                server.bind_source_addr, syncWithMaster) == C_ERR) {
        serverLog(LL_WARNING,"Unable to connect to MASTER: %s",
                connGetLastError(server.repl_transfer_s));
        connClose(server.repl_transfer_s);
        server.repl_transfer_s = NULL;
        return C_ERR;
    }


    server.repl_transfer_lastio = server.unixtime;
    server.repl_state = REPL_STATE_CONNECTING;
    serverLog(LL_NOTICE,"MASTER <-> REPLICA sync started");
    return C_OK;
}

/* This function can be called when a non blocking connection is currently
 * in progress to undo it.
 * Never call this function directly, use cancelReplicationHandshake() instead. */
//当当前正在进行非阻塞连接以撤消它时，可以调用此函数。切勿直接调用此函数，请改用 cancelReplicationHandshake()。
void undoConnectWithMaster(void) {
    connClose(server.repl_transfer_s);
    server.repl_transfer_s = NULL;
}

/* Abort the async download of the bulk dataset while SYNC-ing with master.
 * Never call this function directly, use cancelReplicationHandshake() instead.
 */
//在与 master 同步时中止批量数据集的异步下载。切勿直接调用此函数，请改用 cancelReplicationHandshake()。
void replicationAbortSyncTransfer(void) {
    serverAssert(server.repl_state == REPL_STATE_TRANSFER);
    undoConnectWithMaster();
    if (server.repl_transfer_fd!=-1) {
        close(server.repl_transfer_fd);
        bg_unlink(server.repl_transfer_tmpfile);
        zfree(server.repl_transfer_tmpfile);
        server.repl_transfer_tmpfile = NULL;
        server.repl_transfer_fd = -1;
    }
}

/* This function aborts a non blocking replication attempt if there is one
 * in progress, by canceling the non-blocking connect attempt or
 * the initial bulk transfer.
 *
 * If there was a replication handshake in progress 1 is returned and
 * the replication state (server.repl_state) set to REPL_STATE_CONNECT.
 *
 * Otherwise zero is returned and no operation is performed at all. */
/**如果正在进行非阻塞复制尝试，此函数通过取消非阻塞连接尝试或初始批量传输来中止非阻塞复制尝试。
 *
 * 如果正在进行复制握手，则返回 1 并将复制状态 (server.repl_state) 设置为 REPL_STATE_CONNECT。
 * 否则返回零并且根本不执行任何操作。*/
int cancelReplicationHandshake(int reconnect) {
    if (server.repl_state == REPL_STATE_TRANSFER) {
        replicationAbortSyncTransfer();
        server.repl_state = REPL_STATE_CONNECT;
    } else if (server.repl_state == REPL_STATE_CONNECTING ||
               slaveIsInHandshakeState())
    {
        undoConnectWithMaster();
        server.repl_state = REPL_STATE_CONNECT;
    } else {
        return 0;
    }

    if (!reconnect)
        return 1;

    /* try to re-connect without waiting for replicationCron, this is needed
     * for the "diskless loading short read" test. */
    /**尝试重新连接而不等待replicationCron，这是“无盘加载短读”测试所必需的。*/
    serverLog(LL_NOTICE,"Reconnecting to MASTER %s:%d after failure",
        server.masterhost, server.masterport);
    connectWithMaster();

    return 1;
}

/* Set replication to the specified master address and port. */
//将复制设置为指定的主地址和端口。
void replicationSetMaster(char *ip, int port) {
    int was_master = server.masterhost == NULL;

    sdsfree(server.masterhost);
    server.masterhost = NULL;
    if (server.master) {
        freeClient(server.master);
    }
    disconnectAllBlockedClients(); /* Clients blocked in master, now slave.客户端在主服务器中被阻止，现在是从服务器。 */

    /* Setting masterhost only after the call to freeClient since it calls
     * replicationHandleMasterDisconnection which can trigger a re-connect
     * directly from within that call. */
    /**仅在调用 freeClient 之后设置 masterhost，因为它调用了可以直接从该调用中
     * 触发重新连接的 replicationHandleMasterDisconnection。*/
    server.masterhost = sdsnew(ip);
    server.masterport = port;

    /* Update oom_score_adj */
    setOOMScoreAdj(-1);

    /* Here we don't disconnect with replicas, since they may hopefully be able
     * to partially resync with us. We will disconnect with replicas and force
     * them to resync with us when changing replid on partially resync with new
     * master, or finishing transferring RDB and preparing loading DB on full
     * sync with new master. */
    /**在这里，我们不会与副本断开连接，因为它们可能希望能够与我们部分重新同步。
     * 我们将断开与副本的连接并强制它们与我们重新同步，当更改 replid 时与新的 master 部分重新同步，
     * 或者完成传输 RDB 并准备加载 DB 与新的 master 完全同步。*/

    cancelReplicationHandshake(0);
    /* Before destroying our master state, create a cached master using
     * our own parameters, to later PSYNC with the new master. */
    /**在销毁我们的 master 状态之前，使用我们自己的参数创建一个缓存的 master，以便稍后与新的 master 进行 PSYNC。*/
    if (was_master) {
        replicationDiscardCachedMaster();
        replicationCacheMasterUsingMyself();
    }

    /* Fire the role change modules event. */
    moduleFireServerEvent(REDISMODULE_EVENT_REPLICATION_ROLE_CHANGED,
                          REDISMODULE_EVENT_REPLROLECHANGED_NOW_REPLICA,
                          NULL);

    /* Fire the master link modules event. */
    if (server.repl_state == REPL_STATE_CONNECTED)
        moduleFireServerEvent(REDISMODULE_EVENT_MASTER_LINK_CHANGE,
                              REDISMODULE_SUBEVENT_MASTER_LINK_DOWN,
                              NULL);

    server.repl_state = REPL_STATE_CONNECT;
    serverLog(LL_NOTICE,"Connecting to MASTER %s:%d",
        server.masterhost, server.masterport);
    connectWithMaster();
}

/* Cancel replication, setting the instance as a master itself. */
//取消复制，将实例设置为主实例。
void replicationUnsetMaster(void) {
    if (server.masterhost == NULL) return; /* Nothing to do. */

    /* Fire the master link modules event. */
    if (server.repl_state == REPL_STATE_CONNECTED)
        moduleFireServerEvent(REDISMODULE_EVENT_MASTER_LINK_CHANGE,
                              REDISMODULE_SUBEVENT_MASTER_LINK_DOWN,
                              NULL);

    /* Clear masterhost first, since the freeClient calls
     * replicationHandleMasterDisconnection which can attempt to re-connect. */
    /**首先清除 masterhost，因为 freeClient 调用了可以尝试重新连接的 replicationHandleMasterDisconnection。*/
    sdsfree(server.masterhost);
    server.masterhost = NULL;
    if (server.master) freeClient(server.master);
    replicationDiscardCachedMaster();
    cancelReplicationHandshake(0);
    /* When a slave is turned into a master, the current replication ID
     * (that was inherited from the master at synchronization time) is
     * used as secondary ID up to the current offset, and a new replication
     * ID is created to continue with a new replication history. */
    /**当从属变为主控时，当前复制 ID（在同步时从主控继承）用作辅助 ID，直到当前偏移量，
     * 并创建一个新的复制 ID 以继续新的复制历史。*/
    shiftReplicationId();
    /* Disconnecting all the slaves is required: we need to inform slaves
     * of the replication ID change (see shiftReplicationId() call). However
     * the slaves will be able to partially resync with us, so it will be
     * a very fast reconnection. */
    /**需要断开所有从属设备：我们需要通知从属设备复制 ID 更改（请参阅 shiftReplicationId() 调用）。
     * 然而，从站将能够与我们部分重新同步，因此这将是一个非常快速的重新连接。*/
    disconnectSlaves();
    server.repl_state = REPL_STATE_NONE;

    /* We need to make sure the new master will start the replication stream
     * with a SELECT statement. This is forced after a full resync, but
     * with PSYNC version 2, there is no need for full resync after a
     * master switch. */
    /**我们需要确保新的 master 将使用 SELECT 语句启动复制流。
     * 这是在完全重新同步后强制执行的，但对于 PSYNC 版本 2，主切换后无需完全重新同步。*/
    server.slaveseldb = -1;

    /* Update oom_score_adj */
    setOOMScoreAdj(-1);

    /* Once we turn from slave to master, we consider the starting time without
     * slaves (that is used to count the replication backlog time to live) as
     * starting from now. Otherwise the backlog will be freed after a
     * failover if slaves do not connect immediately. */
    /**一旦我们从slave变为master，我们就认为没有slave的开始时间（用于计算复制积压的生存时间）从现在开始。
     * 否则，如果从站没有立即连接，则在故障转移后将释放积压。*/
    server.repl_no_slaves_since = server.unixtime;
    
    /* Reset down time so it'll be ready for when we turn into replica again. */
    /**重置停机时间，以便我们再次变成副本时准备好。*/
    server.repl_down_since = 0;

    /* Fire the role change modules event. */
    /**触发角色更改模块事件。*/
    moduleFireServerEvent(REDISMODULE_EVENT_REPLICATION_ROLE_CHANGED,
                          REDISMODULE_EVENT_REPLROLECHANGED_NOW_MASTER,
                          NULL);

    /* Restart the AOF subsystem in case we shut it down during a sync when
     * we were still a slave. */
    /**重新启动 AOF 子系统，以防我们在同步期间关闭它，而我们仍然是从站。*/
    if (server.aof_enabled && server.aof_state == AOF_OFF) restartAOFAfterSYNC();
}

/* This function is called when the slave lose the connection with the
 * master into an unexpected way. */
//当从站以意外方式失去与主站的连接时，将调用此函数。
void replicationHandleMasterDisconnection(void) {
    /* Fire the master link modules event. */
    /**触发主链接模块事件。*/
    if (server.repl_state == REPL_STATE_CONNECTED)
        moduleFireServerEvent(REDISMODULE_EVENT_MASTER_LINK_CHANGE,
                              REDISMODULE_SUBEVENT_MASTER_LINK_DOWN,
                              NULL);

    server.master = NULL;
    server.repl_state = REPL_STATE_CONNECT;
    server.repl_down_since = server.unixtime;
    /* We lost connection with our master, don't disconnect slaves yet,
     * maybe we'll be able to PSYNC with our master later. We'll disconnect
     * the slaves only if we'll have to do a full resync with our master. */
    /**我们失去了与我们的主人的联系，不要断开奴隶，也许我们稍后可以与我们的主人进行 PSYNC。
     * 只有当我们必须与我们的主服务器进行完全重新同步时，我们才会断开从服务器的连接。*/

    /* Try to re-connect immediately rather than wait for replicationCron
     * waiting 1 second may risk backlog being recycled. */
    /**尝试立即重新连接而不是等待 replicationCron 等待 1 秒可能会导致积压被回收。*/
    if (server.masterhost) {
        serverLog(LL_NOTICE,"Reconnecting to MASTER %s:%d",
            server.masterhost, server.masterport);
        connectWithMaster();
    }
}

void replicaofCommand(client *c) {
    /* SLAVEOF is not allowed in cluster mode as replication is automatically
     * configured using the current address of the master node. */
    /**集群模式下不允许使用 SLAVEOF，因为复制是使用主节点的当前地址自动配置的。*/
    if (server.cluster_enabled) {
        addReplyError(c,"REPLICAOF not allowed in cluster mode.");
        return;
    }

    if (server.failover_state != NO_FAILOVER) {
        addReplyError(c,"REPLICAOF not allowed while failing over.");
        return;
    }

    /* The special host/port combination "NO" "ONE" turns the instance
     * into a master. Otherwise the new master address is set. */
    /**特殊的主机端口组合“NO”“ONE”将实例变为主端口。否则设置新的主地址。*/
    if (!strcasecmp(c->argv[1]->ptr,"no") &&
        !strcasecmp(c->argv[2]->ptr,"one")) {
        if (server.masterhost) {
            replicationUnsetMaster();
            sds client = catClientInfoString(sdsempty(),c);
            serverLog(LL_NOTICE,"MASTER MODE enabled (user request from '%s')",
                client);
            sdsfree(client);
        }
    } else {
        long port;

        if (c->flags & CLIENT_SLAVE)
        {
            /* If a client is already a replica they cannot run this command,
             * because it involves flushing all replicas (including this
             * client) */
            /**如果客户端已经是副本，则他们无法运行此命令，因为它涉及刷新所有副本（包括此客户端）*/
            addReplyError(c, "Command is not valid when client is a replica.");
            return;
        }

        if (getRangeLongFromObjectOrReply(c, c->argv[2], 0, 65535, &port,
                                          "Invalid master port") != C_OK)
            return;

        /* Check if we are already attached to the specified master */
        if (server.masterhost && !strcasecmp(server.masterhost,c->argv[1]->ptr)
            && server.masterport == port) {
            serverLog(LL_NOTICE,"REPLICAOF would result into synchronization "
                                "with the master we are already connected "
                                "with. No operation performed.");
            addReplySds(c,sdsnew("+OK Already connected to specified "
                                 "master\r\n"));
            return;
        }
        /* There was no previous master or the user specified a different one,
         * we can continue. */
        replicationSetMaster(c->argv[1]->ptr, port);
        sds client = catClientInfoString(sdsempty(),c);
        serverLog(LL_NOTICE,"REPLICAOF %s:%d enabled (user request from '%s')",
            server.masterhost, server.masterport, client);
        sdsfree(client);
    }
    addReply(c,shared.ok);
}

/* ROLE command: provide information about the role of the instance
 * (master or slave) and additional information related to replication
 * in an easy to process format. */
//ROLE 命令：以易于处理的格式提供有关实例（主或从）角色的信息以及与复制相关的附加信息。
void roleCommand(client *c) {
    if (server.sentinel_mode) {
        sentinelRoleCommand(c);
        return;
    }

    if (server.masterhost == NULL) {
        listIter li;
        listNode *ln;
        void *mbcount;
        int slaves = 0;

        addReplyArrayLen(c,3);
        addReplyBulkCBuffer(c,"master",6);
        addReplyLongLong(c,server.master_repl_offset);
        mbcount = addReplyDeferredLen(c);
        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            client *slave = ln->value;
            char ip[NET_IP_STR_LEN], *slaveaddr = slave->slave_addr;

            if (!slaveaddr) {
                if (connPeerToString(slave->conn,ip,sizeof(ip),NULL) == -1)
                    continue;
                slaveaddr = ip;
            }
            if (slave->replstate != SLAVE_STATE_ONLINE) continue;
            addReplyArrayLen(c,3);
            addReplyBulkCString(c,slaveaddr);
            addReplyBulkLongLong(c,slave->slave_listening_port);
            addReplyBulkLongLong(c,slave->repl_ack_off);
            slaves++;
        }
        setDeferredArrayLen(c,mbcount,slaves);
    } else {
        char *slavestate = NULL;

        addReplyArrayLen(c,5);
        addReplyBulkCBuffer(c,"slave",5);
        addReplyBulkCString(c,server.masterhost);
        addReplyLongLong(c,server.masterport);
        if (slaveIsInHandshakeState()) {
            slavestate = "handshake";
        } else {
            switch(server.repl_state) {
            case REPL_STATE_NONE: slavestate = "none"; break;
            case REPL_STATE_CONNECT: slavestate = "connect"; break;
            case REPL_STATE_CONNECTING: slavestate = "connecting"; break;
            case REPL_STATE_TRANSFER: slavestate = "sync"; break;
            case REPL_STATE_CONNECTED: slavestate = "connected"; break;
            default: slavestate = "unknown"; break;
            }
        }
        addReplyBulkCString(c,slavestate);
        addReplyLongLong(c,server.master ? server.master->reploff : -1);
    }
}

/* Send a REPLCONF ACK command to the master to inform it about the current
 * processed offset. If we are not connected with a master, the command has
 * no effects. */
//向主设备发送一个 REPLCONF ACK 命令，通知它当前处理的偏移量。如果我们没有与 master 连接，则该命令无效。
void replicationSendAck(void) {
    client *c = server.master;

    if (c != NULL) {
        c->flags |= CLIENT_MASTER_FORCE_REPLY;
        addReplyArrayLen(c,3);
        addReplyBulkCString(c,"REPLCONF");
        addReplyBulkCString(c,"ACK");
        addReplyBulkLongLong(c,c->reploff);
        c->flags &= ~CLIENT_MASTER_FORCE_REPLY;
    }
}

/* ---------------------- MASTER CACHING FOR PSYNC -------------------------- */

/* In order to implement partial synchronization we need to be able to cache
 * our master's client structure after a transient disconnection.
 * It is cached into server.cached_master and flushed away using the following
 * functions. */

/* This function is called by freeClient() in order to cache the master
 * client structure instead of destroying it. freeClient() will return
 * ASAP after this function returns, so every action needed to avoid problems
 * with a client that is really "suspended" has to be done by this function.
 *
 * The other functions that will deal with the cached master are:
 *
 * replicationDiscardCachedMaster() that will make sure to kill the client
 * as for some reason we don't want to use it in the future.
 *
 * replicationResurrectCachedMaster() that is used after a successful PSYNC
 * handshake in order to reactivate the cached master.
 */
/**
 * ---------------------- PSYNC 的主缓存 ------------------------ --
 * 为了实现部分同步，我们需要能够在短暂断开连接后缓存我们主控的客户端结构。
 * 它被缓存到 server.cached_master 并使用以下函数刷新。此函数由 freeClient() 调用，以缓存主客户端结构而不是破坏它。
 * freeClient() 将在此函数返回后尽快返回，因此为避免真正“暂停”的客户端出现问题所需的每个操作都必须由该函数完成。
 *
 * 处理缓存的主控的其他函数是：replicationDiscardCachedMaster()，它将确保终止客户端，因为出于某种原因我们将来不想使用它。
 * 在成功的 PSYNC 握手之后使用的 replicationResurrectCachedMaster() 以重新激活缓存的 master。*/
void replicationCacheMaster(client *c) {
    serverAssert(server.master != NULL && server.cached_master == NULL);
    serverLog(LL_NOTICE,"Caching the disconnected master state.");

    /* Unlink the client from the server structures. */
    unlinkClient(c);

    /* Reset the master client so that's ready to accept new commands:
     * we want to discard the non processed query buffers and non processed
     * offsets, including pending transactions, already populated arguments,
     * pending outputs to the master. */
    /**重置主客户端，以便准备好接受新命令：我们要丢弃未处理的查询缓冲区和未处理的偏移量，
     * 包括待处理的事务、已填充的参数、待处理的主输出。*/
    sdsclear(server.master->querybuf);
    server.master->qb_pos = 0;
    server.master->repl_applied = 0;
    server.master->read_reploff = server.master->reploff;
    if (c->flags & CLIENT_MULTI) discardTransaction(c);
    listEmpty(c->reply);
    c->sentlen = 0;
    c->reply_bytes = 0;
    c->bufpos = 0;
    resetClient(c);

    /* Save the master. Server.master will be set to null later by
     * replicationHandleMasterDisconnection(). */
    /**救主。 Server.master 稍后将通过 replicationHandleMasterDisconnection() 设置为 null。*/
    server.cached_master = server.master;

    /* Invalidate the Peer ID cache. */
    if (c->peerid) {
        sdsfree(c->peerid);
        c->peerid = NULL;
    }
    /* Invalidate the Sock Name cache. */
    if (c->sockname) {
        sdsfree(c->sockname);
        c->sockname = NULL;
    }

    /* Caching the master happens instead of the actual freeClient() call,
     * so make sure to adjust the replication state. This function will
     * also set server.master to NULL. */
    /**缓存主服务器而不是实际的 freeClient() 调用，因此请确保调整复制状态。
     * 此函数还将 server.master 设置为 NULL。*/
    replicationHandleMasterDisconnection();
}

/* This function is called when a master is turned into a slave, in order to
 * create from scratch a cached master for the new client, that will allow
 * to PSYNC with the slave that was promoted as the new master after a
 * failover.
 *
 * Assuming this instance was previously the master instance of the new master,
 * the new master will accept its replication ID, and potential also the
 * current offset if no data was lost during the failover. So we use our
 * current replication ID and offset in order to synthesize a cached master. */
/**当master变成slave时调用这个函数，以便从头开始为新客户端创建一个缓存的master，
 * 这将允许在故障转移后与被提升为新master的slave进行PSYNC。
 *
 * 假设此实例以前是新主实例的主实例，则新主实例将接受其复制 ID，如果在故障转移期间没有数据丢失，
 * 则还可能接受当前偏移量。所以我们使用我们当前的复制 ID 和偏移量来合成一个缓存的 master。*/
void replicationCacheMasterUsingMyself(void) {
    serverLog(LL_NOTICE,
        "Before turning into a replica, using my own master parameters "
        "to synthesize a cached master: I may be able to synchronize with "
        "the new master with just a partial transfer.");

    /* This will be used to populate the field server.master->reploff
     * by replicationCreateMasterClient(). We'll later set the created
     * master as server.cached_master, so the replica will use such
     * offset for PSYNC. */
    /**这将用于通过 replicationCreateMasterClient() 填充字段 server.master->reploff。
     * 我们稍后将创建的主服务器设置为 server.cached_master，因此副本将使用这样的偏移量进行 PSYNC。*/
    server.master_initial_offset = server.master_repl_offset;

    /* The master client we create can be set to any DBID, because
     * the new master will start its replication stream with SELECT. */
    /**我们创建的主客户端可以设置为任何 DBID，因为新的主客户端将使用 SELECT 启动其复制流。*/
    replicationCreateMasterClient(NULL,-1);

    /* Use our own ID / offset. */
    memcpy(server.master->replid, server.replid, sizeof(server.replid));

    /* Set as cached master. */
    unlinkClient(server.master);
    server.cached_master = server.master;
    server.master = NULL;
}

/* Free a cached master, called when there are no longer the conditions for
 * a partial resync on reconnection. */
//释放缓存的 master，当重新连接时不再有条件进行部分重新同步时调用。
void replicationDiscardCachedMaster(void) {
    if (server.cached_master == NULL) return;

    serverLog(LL_NOTICE,"Discarding previously cached master state.");
    server.cached_master->flags &= ~CLIENT_MASTER;
    freeClient(server.cached_master);
    server.cached_master = NULL;
}

/* Turn the cached master into the current master, using the file descriptor
 * passed as argument as the socket for the new master.
 *
 * This function is called when successfully setup a partial resynchronization
 * so the stream of data that we'll receive will start from where this
 * master left. */
/**使用作为参数传递的文件描述符作为新 master 的套接字将缓存的 master 转换为当前 master。
 * 当成功设置部分重新同步时调用此函数，因此我们将接收的数据流将从该主设备离开的位置开始。*/
void replicationResurrectCachedMaster(connection *conn) {
    server.master = server.cached_master;
    server.cached_master = NULL;
    server.master->conn = conn;
    connSetPrivateData(server.master->conn, server.master);
    server.master->flags &= ~(CLIENT_CLOSE_AFTER_REPLY|CLIENT_CLOSE_ASAP);
    server.master->authenticated = 1;
    server.master->lastinteraction = server.unixtime;
    server.repl_state = REPL_STATE_CONNECTED;
    server.repl_down_since = 0;

    /* Fire the master link modules event. */
    moduleFireServerEvent(REDISMODULE_EVENT_MASTER_LINK_CHANGE,
                          REDISMODULE_SUBEVENT_MASTER_LINK_UP,
                          NULL);

    /* Re-add to the list of clients. */
    linkClient(server.master);
    if (connSetReadHandler(server.master->conn, readQueryFromClient)) {
        serverLog(LL_WARNING,"Error resurrecting the cached master, impossible to add the readable handler: %s", strerror(errno));
        freeClientAsync(server.master); /* Close ASAP. */
    }

    /* We may also need to install the write handler as well if there is
     * pending data in the write buffers. */
    if (clientHasPendingReplies(server.master)) {
        if (connSetWriteHandler(server.master->conn, sendReplyToClient)) {
            serverLog(LL_WARNING,"Error resurrecting the cached master, impossible to add the writable handler: %s", strerror(errno));
            freeClientAsync(server.master); /* Close ASAP. */
        }
    }
}

/* ------------------------- MIN-SLAVES-TO-WRITE  --------------------------- */

/* This function counts the number of slaves with lag <= min-slaves-max-lag.
 * If the option is active, the server will prevent writes if there are not
 * enough connected slaves with the specified lag (or less). */
/**
 * ------------------------- 最小从属写入 ------------------ ---------
 * 此函数计算滞后 <= min-slaves-max-lag 的从站数量。如果该选项处于活动状态，
 * 如果没有足够的具有指定延迟（或更少）的连接从属服务器，服务器将阻止写入。*/
void refreshGoodSlavesCount(void) {
    listIter li;
    listNode *ln;
    int good = 0;

    if (!server.repl_min_slaves_to_write ||
        !server.repl_min_slaves_max_lag) return;

    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;
        time_t lag = server.unixtime - slave->repl_ack_time;

        if (slave->replstate == SLAVE_STATE_ONLINE &&
            lag <= server.repl_min_slaves_max_lag) good++;
    }
    server.repl_good_slaves_count = good;
}

/* return true if status of good replicas is OK. otherwise false */
//如果良好副本的状态正常，则返回 true。否则为假
int checkGoodReplicasStatus(void) {
    return server.masterhost || /* not a primary status should be OK */
           !server.repl_min_slaves_max_lag || /* Min slave max lag not configured */
           !server.repl_min_slaves_to_write || /* Min slave to write not configured */
           server.repl_good_slaves_count >= server.repl_min_slaves_to_write; /* check if we have enough slaves */
}

/* ----------------------- SYNCHRONOUS REPLICATION --------------------------
 * Redis synchronous replication design can be summarized in points:
 *
 * - Redis masters have a global replication offset, used by PSYNC.
 * - Master increment the offset every time new commands are sent to slaves.
 * - Slaves ping back masters with the offset processed so far.
 *
 * So synchronous replication adds a new WAIT command in the form:
 *
 *   WAIT <num_replicas> <milliseconds_timeout>
 *
 * That returns the number of replicas that processed the query when
 * we finally have at least num_replicas, or when the timeout was
 * reached.
 *
 * The command is implemented in this way:
 *
 * - Every time a client processes a command, we remember the replication
 *   offset after sending that command to the slaves.
 * - When WAIT is called, we ask slaves to send an acknowledgement ASAP.
 *   The client is blocked at the same time (see blocked.c).
 * - Once we receive enough ACKs for a given offset or when the timeout
 *   is reached, the WAIT command is unblocked and the reply sent to the
 *   client.
 */
/**
 * ------------------------ 同步复制 ------------------------- -
 * Redis 同步复制设计可以总结为以下几点： - Redis master 有一个全局复制偏移量，由 PSYNC 使用。
 * - 每次向从站发送新命令时，主站都会增加偏移量。
 * - 从站用到目前为止处理的偏移量 ping 回主站。因此同步复制添加了一个新的 WAIT 命令，格式如下：
 *     WAIT <num_replicas> <milliseconds_timeout>
 *   当我们最终至少有 num_replicas 或达到超时时，它返回处理查询的副本数。该命令以这种方式实现：
 * - 每次客户端处理命令时，我们都会记住将该命令发送到从属设备后的复制偏移量。
 * - 当调用 WAIT 时，我们要求从站尽快发送确认。客户端同时被阻止（参见blocked.c）。
 * - 一旦我们为给定的偏移量接收到足够的 ACK 或达到超时时，WAIT 命令被解除阻塞并将回复发送到客户端。
 * */

/* This just set a flag so that we broadcast a REPLCONF GETACK command
 * to all the slaves in the beforeSleep() function. Note that this way
 * we "group" all the clients that want to wait for synchronous replication
 * in a given event loop iteration, and send a single GETACK for them all. */
/** 这只是设置了一个标志，以便我们在 beforeSleep() 函数中向所有从站广播 REPLCONF GETACK 命令。
 * 请注意，通过这种方式，我们将所有希望在给定事件循环迭代中等待同步复制的客户端“分组”，并为所有客户端发送一个 GETACK。*/
void replicationRequestAckFromSlaves(void) {
    server.get_ack_from_slaves = 1;
}

/* Return the number of slaves that already acknowledged the specified
 * replication offset. */
//返回已经确认指定复制偏移量的从属设备的数量。
int replicationCountAcksByOffset(long long offset) {
    listIter li;
    listNode *ln;
    int count = 0;

    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;

        if (slave->replstate != SLAVE_STATE_ONLINE) continue;
        if (slave->repl_ack_off >= offset) count++;
    }
    return count;
}

/* WAIT for N replicas to acknowledge the processing of our latest
 * write command (and all the previous commands). */
//等待 N 个副本确认我们最新的写入命令（以及所有先前的命令）的处理。
void waitCommand(client *c) {
    mstime_t timeout;
    long numreplicas, ackreplicas;
    long long offset = c->woff;

    if (server.masterhost) {
        addReplyError(c,"WAIT cannot be used with replica instances. Please also note that since Redis 4.0 if a replica is configured to be writable (which is not the default) writes to replicas are just local and are not propagated.");
        return;
    }

    /* Argument parsing. */
    if (getLongFromObjectOrReply(c,c->argv[1],&numreplicas,NULL) != C_OK)
        return;
    if (getTimeoutFromObjectOrReply(c,c->argv[2],&timeout,UNIT_MILLISECONDS)
        != C_OK) return;

    /* First try without blocking at all. */
    ackreplicas = replicationCountAcksByOffset(c->woff);
    if (ackreplicas >= numreplicas || c->flags & CLIENT_MULTI) {
        addReplyLongLong(c,ackreplicas);
        return;
    }

    /* Otherwise block the client and put it into our list of clients
     * waiting for ack from slaves. */
    /**否则阻塞客户端并将其放入我们的客户端列表中等待从服务器的确认。*/
    c->bpop.timeout = timeout;
    c->bpop.reploffset = offset;
    c->bpop.numreplicas = numreplicas;
    listAddNodeHead(server.clients_waiting_acks,c);
    blockClient(c,BLOCKED_WAIT);

    /* Make sure that the server will send an ACK request to all the slaves
     * before returning to the event loop. */
    /**确保服务器在返回事件循环之前将向所有从站发送 ACK 请求。*/
    replicationRequestAckFromSlaves();
}

/* This is called by unblockClient() to perform the blocking op type
 * specific cleanup. We just remove the client from the list of clients
 * waiting for replica acks. Never call it directly, call unblockClient()
 * instead. */
/** 这由 unblockClient() 调用以执行特定于阻塞操作类型的清理。我们只是从等待副本确认的客户端列表中删除客户端。
 * 永远不要直接调用它，而是调用 unblockClient() 。*/
void unblockClientWaitingReplicas(client *c) {
    listNode *ln = listSearchKey(server.clients_waiting_acks,c);
    serverAssert(ln != NULL);
    listDelNode(server.clients_waiting_acks,ln);
}

/* Check if there are clients blocked in WAIT that can be unblocked since
 * we received enough ACKs from slaves. */
//检查是否有在等待中阻塞的客户端可以解除阻塞，因为我们从从站收到了足够的 ACK。
void processClientsWaitingReplicas(void) {
    long long last_offset = 0;
    int last_numreplicas = 0;

    listIter li;
    listNode *ln;

    listRewind(server.clients_waiting_acks,&li);
    while((ln = listNext(&li))) {
        client *c = ln->value;

        /* Every time we find a client that is satisfied for a given
         * offset and number of replicas, we remember it so the next client
         * may be unblocked without calling replicationCountAcksByOffset()
         * if the requested offset / replicas were equal or less. */
        /**每次我们找到一个满足给定偏移量和副本数量的客户端时，我们都会记住它，因此如果请求的偏移量副本相等或更少，
         * 则下一个客户端可以在不调用 replicationCountAcksByOffset() 的情况下解除阻塞。*/
        if (last_offset && last_offset >= c->bpop.reploffset &&
                           last_numreplicas >= c->bpop.numreplicas)
        {
            unblockClient(c);
            addReplyLongLong(c,last_numreplicas);
        } else {
            int numreplicas = replicationCountAcksByOffset(c->bpop.reploffset);

            if (numreplicas >= c->bpop.numreplicas) {
                last_offset = c->bpop.reploffset;
                last_numreplicas = numreplicas;
                unblockClient(c);
                addReplyLongLong(c,numreplicas);
            }
        }
    }
}

/* Return the slave replication offset for this instance, that is
 * the offset for which we already processed the master replication stream. */
//返回此实例的从属复制偏移量，即我们已经处理了主复制流的偏移量。
long long replicationGetSlaveOffset(void) {
    long long offset = 0;

    if (server.masterhost != NULL) {
        if (server.master) {
            offset = server.master->reploff;
        } else if (server.cached_master) {
            offset = server.cached_master->reploff;
        }
    }
    /* offset may be -1 when the master does not support it at all, however
     * this function is designed to return an offset that can express the
     * amount of data processed by the master, so we return a positive
     * integer. */
    /**当master根本不支持的时候offset可能是-1，但是这个函数被设计成返回一个
     * 可以表示master处理的数据量的偏移量，所以我们返回一个正整数。*/
    if (offset < 0) offset = 0;
    return offset;
}

/* --------------------------- REPLICATION CRON  ---------------------------- */
// --------------------------------------- 复制 CRON --------- --------

/* Replication cron function, called 1 time per second. */
/**复制cron函数，每秒调用1次。 */
void replicationCron(void) {
    static long long replication_cron_loops = 0;

    /* Check failover status first, to see if we need to start
     * handling the failover. */
    /**首先检查故障转移状态，看看我们是否需要开始处理故障转移。*/
    updateFailoverStatus();

    /* Non blocking connection timeout? */
    /**非阻塞连接超时？*/
    if (server.masterhost &&
        (server.repl_state == REPL_STATE_CONNECTING ||
         slaveIsInHandshakeState()) &&
         (time(NULL)-server.repl_transfer_lastio) > server.repl_timeout)
    {
        serverLog(LL_WARNING,"Timeout connecting to the MASTER...");
        cancelReplicationHandshake(1);
    }

    /* Bulk transfer I/O timeout? */
    /**批量传输 IO 超时？*/
    if (server.masterhost && server.repl_state == REPL_STATE_TRANSFER &&
        (time(NULL)-server.repl_transfer_lastio) > server.repl_timeout)
    {
        serverLog(LL_WARNING,"Timeout receiving bulk data from MASTER... If the problem persists try to set the 'repl-timeout' parameter in redis.conf to a larger value.");
        cancelReplicationHandshake(1);
    }

    /* Timed out master when we are an already connected slave? */
    /**当我们是一个已经连接的奴隶时，主人超时了吗？*/
    if (server.masterhost && server.repl_state == REPL_STATE_CONNECTED &&
        (time(NULL)-server.master->lastinteraction) > server.repl_timeout)
    {
        serverLog(LL_WARNING,"MASTER timeout: no data nor PING received...");
        freeClient(server.master);
    }

    /* Check if we should connect to a MASTER */
    /**检查我们是否应该连接到 MASTER*/
    if (server.repl_state == REPL_STATE_CONNECT) {
        serverLog(LL_NOTICE,"Connecting to MASTER %s:%d",
            server.masterhost, server.masterport);
        connectWithMaster();
    }

    /* Send ACK to master from time to time.
     * Note that we do not send periodic acks to masters that don't
     * support PSYNC and replication offsets. */
    /**不时向主设备发送 ACK。请注意，我们不会向不支持 PSYNC 和复制偏移量的 master 发送定期确认。*/
    if (server.masterhost && server.master &&
        !(server.master->flags & CLIENT_PRE_PSYNC))
        replicationSendAck();

    /* If we have attached slaves, PING them from time to time.
     * So slaves can implement an explicit timeout to masters, and will
     * be able to detect a link disconnection even if the TCP connection
     * will not actually go down. */
    /**如果我们连接了从属服务器，请不时对它们进行 PING。因此，从站可以对主站实施显式超时，
     * 并且即使 TCP 连接实际上不会断开，也能够检测到链接断开。*/
    listIter li;
    listNode *ln;
    robj *ping_argv[1];

    /* First, send PING according to ping_slave_period. */
    /**首先，根据 ping_slave_period 发送 PING。*/
    if ((replication_cron_loops % server.repl_ping_slave_period) == 0 &&
        listLength(server.slaves))
    {
        /* Note that we don't send the PING if the clients are paused during
         * a Redis Cluster manual failover: the PING we send will otherwise
         * alter the replication offsets of master and slave, and will no longer
         * match the one stored into 'mf_master_offset' state. */
        /**请注意，如果客户端在 Redis 集群手动故障转移期间暂停，我们不会发送 PING：
         * 我们发送的 PING 将改变主从的复制偏移量，并且将不再匹配存储到“mf_master_offset”状态的那个。*/
        int manual_failover_in_progress =
            ((server.cluster_enabled &&
              server.cluster->mf_end) ||
            server.failover_end_time) &&
            checkClientPauseTimeoutAndReturnIfPaused();

        if (!manual_failover_in_progress) {
            ping_argv[0] = shared.ping;
            replicationFeedSlaves(server.slaves, server.slaveseldb,
                ping_argv, 1);
        }
    }

    /* Second, send a newline to all the slaves in pre-synchronization
     * stage, that is, slaves waiting for the master to create the RDB file.
     *
     * Also send the a newline to all the chained slaves we have, if we lost
     * connection from our master, to keep the slaves aware that their
     * master is online. This is needed since sub-slaves only receive proxied
     * data from top-level masters, so there is no explicit pinging in order
     * to avoid altering the replication offsets. This special out of band
     * pings (newlines) can be sent, they will have no effect in the offset.
     *
     * The newline will be ignored by the slave but will refresh the
     * last interaction timer preventing a timeout. In this case we ignore the
     * ping period and refresh the connection once per second since certain
     * timeouts are set at a few seconds (example: PSYNC response). */
    /**其次，在预同步阶段向所有从站发送换行符，即从站等待主站创建RDB文件。
     *
     * 如果我们失去与主人的连接，也向我们拥有的所有链式奴隶发送换行符，以让奴隶知道他们的主人在线。
     * 这是必需的，因为子从站仅接收来自顶级主站的代理数据，因此没有显式 ping 以避免更改复制偏移量。
     * 可以发送这种特殊的带外 ping（换行符），它们对偏移量没有影响。
     *
     * 从站将忽略换行符，但会刷新最后一个交互计时器以防止超时。
     * 在这种情况下，我们忽略 ping 周期并每秒刷新一次连接，因为某些超时设置为几秒钟（例如：PSYNC 响应）。*/
    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;

        int is_presync =
            (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START ||
            (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_END &&
             server.rdb_child_type != RDB_CHILD_TYPE_SOCKET));

        if (is_presync) {
            connWrite(slave->conn, "\n", 1);
        }
    }

    /* Disconnect timedout slaves. */
    if (listLength(server.slaves)) {
        listIter li;
        listNode *ln;

        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            client *slave = ln->value;

            if (slave->replstate == SLAVE_STATE_ONLINE) {
                if (slave->flags & CLIENT_PRE_PSYNC)
                    continue;
                if ((server.unixtime - slave->repl_ack_time) > server.repl_timeout) {
                    serverLog(LL_WARNING, "Disconnecting timedout replica (streaming sync): %s",
                          replicationGetSlaveName(slave));
                    freeClient(slave);
                    continue;
                }
            }
            /* We consider disconnecting only diskless replicas because disk-based replicas aren't fed
             * by the fork child so if a disk-based replica is stuck it doesn't prevent the fork child
             * from terminating. */
            /**我们考虑只断开无盘副本，因为基于磁盘的副本不是由 fork 子节点提供的，
             * 因此如果基于磁盘的副本卡住，它不会阻止 fork 子节点终止。*/
            if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_END && server.rdb_child_type == RDB_CHILD_TYPE_SOCKET) {
                if (slave->repl_last_partial_write != 0 &&
                    (server.unixtime - slave->repl_last_partial_write) > server.repl_timeout)
                {
                    serverLog(LL_WARNING, "Disconnecting timedout replica (full sync): %s",
                          replicationGetSlaveName(slave));
                    freeClient(slave);
                    continue;
                }
            }
        }
    }

    /* If this is a master without attached slaves and there is a replication
     * backlog active, in order to reclaim memory we can free it after some
     * (configured) time. Note that this cannot be done for slaves: slaves
     * without sub-slaves attached should still accumulate data into the
     * backlog, in order to reply to PSYNC queries if they are turned into
     * masters after a failover. */
    /**如果这是一个没有附加从属的主控并且有一个复制积压活动，为了回收内存，我们可以在一些（配置的）时间后释放它。
     * 请注意，这不能用于从站：没有附加子从站的从站仍应将数据累积到积压中，以便在故障转移后它们变成主站时回复 PSYNC 查询。 */
    if (listLength(server.slaves) == 0 && server.repl_backlog_time_limit &&
        server.repl_backlog && server.masterhost == NULL)
    {
        time_t idle = server.unixtime - server.repl_no_slaves_since;

        if (idle > server.repl_backlog_time_limit) {
            /* When we free the backlog, we always use a new
             * replication ID and clear the ID2. This is needed
             * because when there is no backlog, the master_repl_offset
             * is not updated, but we would still retain our replication
             * ID, leading to the following problem:
             *
             * 1. We are a master instance.
             * 2. Our slave is promoted to master. It's repl-id-2 will
             *    be the same as our repl-id.
             * 3. We, yet as master, receive some updates, that will not
             *    increment the master_repl_offset.
             * 4. Later we are turned into a slave, connect to the new
             *    master that will accept our PSYNC request by second
             *    replication ID, but there will be data inconsistency
             *    because we received writes. */
            /**
             * 当我们释放积压时，我们总是使用一个新的复制 ID 并清除 ID2。
             * 这是必要的，因为当没有积压时，master_repl_offset 没有更新，但我们仍然会保留我们的复制 ID，导致以下问题：
             *   1. 我们是一个主实例。
             *   2.我们的slave被提升为master。它的 repl-id-2 将与我们的 repl-id 相同。
             *   3. 作为master，我们收到了一些更新，这些更新不会增加master_repl_offset。
             *   4. 之后我们变成了slave，通过第二个replication ID连接到新的master上接受我们的PSYNC请求，
             *      但是会出现数据不一致的情况，因为我们收到了写。
             * */
            changeReplicationId();
            clearReplicationId2();
            freeReplicationBacklog();
            serverLog(LL_NOTICE,
                "Replication backlog freed after %d seconds "
                "without connected replicas.",
                (int) server.repl_backlog_time_limit);
        }
    }

    replicationStartPendingFork();

    /* Remove the RDB file used for replication if Redis is not running
     * with any persistence.
     * 如果 Redis 没有以任何持久性运行，则删除用于复制的 RDB 文件。*/
    removeRDBUsedToSyncReplicas();

    /* Sanity check replication buffer, the first block of replication buffer blocks
     * must be referenced by someone, since it will be freed when not referenced,
     * otherwise, server will OOM. also, its refcount must not be more than
     * replicas number + 1(replication backlog). */
    /**完整性检查复制缓冲区，复制缓冲区块的第一个块必须被某人引用，因为它会在没有被引用时被释放，否则，服务器将OOM。
     * 此外，它的引用计数不得超过副本数 + 1（复制积压）。*/
    if (listLength(server.repl_buffer_blocks) > 0) {
        replBufBlock *o = listNodeValue(listFirst(server.repl_buffer_blocks));
        serverAssert(o->refcount > 0 &&
            o->refcount <= (int)listLength(server.slaves)+1);
    }

    /* Refresh the number of slaves with lag <= min-slaves-max-lag. */
    refreshGoodSlavesCount();
    replication_cron_loops++; /* Incremented with frequency 1 HZ. */
}

int shouldStartChildReplication(int *mincapa_out, int *req_out) {
    /* We should start a BGSAVE good for replication if we have slaves in
     * WAIT_BGSAVE_START state.
     * 如果我们有处于 WAIT_BGSAVE_START 状态的从属设备，我们应该启动一个有利于复制的 BGSAVE。
     *
     * In case of diskless replication, we make sure to wait the specified
     * number of seconds (according to configuration) so that other slaves
     * have the time to arrive before we start streaming.
     * 在无盘复制的情况下，我们确保等待指定的秒数（根据配置），以便在我们开始流式传输之前其他从站有时间到达。*/
    if (!hasActiveChildProcess()) {
        time_t idle, max_idle = 0;
        int slaves_waiting = 0;
        int mincapa;
        int req;
        int first = 1;
        listNode *ln;
        listIter li;

        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            client *slave = ln->value;
            if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) {
                if (first) {
                    /* Get first slave's requirements */
                    req = slave->slave_req;
                } else if (req != slave->slave_req) {
                    /* Skip slaves that don't match */
                    continue;
                }
                idle = server.unixtime - slave->lastinteraction;
                if (idle > max_idle) max_idle = idle;
                slaves_waiting++;
                mincapa = first ? slave->slave_capa : (mincapa & slave->slave_capa);
                first = 0;
            }
        }

        if (slaves_waiting &&
            (!server.repl_diskless_sync ||
             (server.repl_diskless_sync_max_replicas > 0 &&
              slaves_waiting >= server.repl_diskless_sync_max_replicas) ||
             max_idle >= server.repl_diskless_sync_delay))
        {
            if (mincapa_out)
                *mincapa_out = mincapa;
            if (req_out)
                *req_out = req;
            return 1;
        }
    }

    return 0;
}

void replicationStartPendingFork(void) {
    int mincapa = -1;
    int req = -1;

    if (shouldStartChildReplication(&mincapa, &req)) {
        /* Start the BGSAVE. The called function may start a
         * BGSAVE with socket target or disk target depending on the
         * configuration and slaves capabilities and requirements. */
        //启动 BGSAVE。调用的函数可以根据配置和从属功能和要求启动具有套接字目标或磁盘目标的 BGSAVE。
        startBgsaveForReplication(mincapa, req);
    }
}

/* Find replica at IP:PORT from replica list */
//在 IP:PORT 从副本列表中查找副本
static client *findReplica(char *host, int port) {
    listIter li;
    listNode *ln;
    client *replica;

    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        replica = ln->value;
        char ip[NET_IP_STR_LEN], *replicaip = replica->slave_addr;

        if (!replicaip) {
            if (connPeerToString(replica->conn, ip, sizeof(ip), NULL) == -1)
                continue;
            replicaip = ip;
        }

        if (!strcasecmp(host, replicaip) &&
                (port == replica->slave_listening_port))
            return replica;
    }

    return NULL;
}

const char *getFailoverStateString() {
    switch(server.failover_state) {
        case NO_FAILOVER: return "no-failover";
        case FAILOVER_IN_PROGRESS: return "failover-in-progress";
        case FAILOVER_WAIT_FOR_SYNC: return "waiting-for-sync";
        default: return "unknown";
    }
}

/* Resets the internal failover configuration, this needs
 * to be called after a failover either succeeds or fails
 * as it includes the client unpause. */
//重置内部故障转移配置，这需要在故障转移成功或失败后调用，因为它包括客户端取消暂停。
void clearFailoverState() {
    server.failover_end_time = 0;
    server.force_failover = 0;
    zfree(server.target_replica_host);
    server.target_replica_host = NULL;
    server.target_replica_port = 0;
    server.failover_state = NO_FAILOVER;
    unpauseClients(PAUSE_DURING_FAILOVER);
}

/* Abort an ongoing failover if one is going on. */
//如果正在进行故障转移，请中止正在进行的故障转移。
void abortFailover(const char *err) {
    if (server.failover_state == NO_FAILOVER) return;

    if (server.target_replica_host) {
        serverLog(LL_NOTICE,"FAILOVER to %s:%d aborted: %s",
            server.target_replica_host,server.target_replica_port,err);  
    } else {
        serverLog(LL_NOTICE,"FAILOVER to any replica aborted: %s",err);  
    }
    if (server.failover_state == FAILOVER_IN_PROGRESS) {
        replicationUnsetMaster();
    }
    clearFailoverState();
}

/* 
 * FAILOVER [TO <HOST> <PORT> [FORCE]] [ABORT] [TIMEOUT <timeout>]
 * 
 * This command will coordinate a failover between the master and one
 * of its replicas. The happy path contains the following steps:
 * 1) The master will initiate a client pause write, to stop replication
 * traffic.
 * 2) The master will periodically check if any of its replicas has
 * consumed the entire replication stream through acks. 
 * 3) Once any replica has caught up, the master will itself become a replica.
 * 4) The master will send a PSYNC FAILOVER request to the target replica, which
 * if accepted will cause the replica to become the new master and start a sync.
 * 
 * FAILOVER ABORT is the only way to abort a failover command, as replicaof
 * will be disabled. This may be needed if the failover is unable to progress. 
 * 
 * The optional arguments [TO <HOST> <IP>] allows designating a specific replica
 * to be failed over to.
 * 
 * FORCE flag indicates that even if the target replica is not caught up,
 * failover to it anyway. This must be specified with a timeout and a target
 * HOST and IP.
 * 
 * TIMEOUT <timeout> indicates how long should the primary wait for 
 * a replica to sync up before aborting. If not specified, the failover
 * will attempt forever and must be manually aborted.
 */
/**
 * FAILOVER [TO <HOST> <PORT> [FORCE]] [ABORT] [TIMEOUT <timeout>]
 * 此命令将协调主服务器与其副本之一之间的故障转移。快乐路径包含以下步骤：
 *   1) master 将启动客户端暂停写入，以停止复制流量。
 *   2) master 会定期检查它的任何一个副本是否已经通过acks消耗了整个复制流。
 *   3）一旦任何副本赶上，主节点本身将成为副本。
 *   4) master 将向目标副本发送 PSYNC FAILOVER 请求，如果被接受，将导致副本成为新的 master 并开始同步。
 * FAILOVER ABORT 是中止故障转移命令的唯一方法，因为replicaof 将被禁用。如果故障转移无法进行，则可能需要这样做。
 * 可选参数 [TO <HOST> <IP>] 允许指定要故障转移到的特定副本。
 * FORCE 标志表明即使目标副本没有被赶上，故障转移到它无论如何。这必须使用超时和目标主机和 IP 来指定。
 * TIMEOUT <timeout> 表示主节点在中止之前应该等待副本同步多长时间。如果未指定，故障转移将永远尝试并且必须手动中止。
 * */
void failoverCommand(client *c) {
    if (server.cluster_enabled) {
        addReplyError(c,"FAILOVER not allowed in cluster mode. "
                        "Use CLUSTER FAILOVER command instead.");
        return;
    }
    
    /* Handle special case for abort */
    if ((c->argc == 2) && !strcasecmp(c->argv[1]->ptr,"abort")) {
        if (server.failover_state == NO_FAILOVER) {
            addReplyError(c, "No failover in progress.");
            return;
        }

        abortFailover("Failover manually aborted");
        addReply(c,shared.ok);
        return;
    }

    long timeout_in_ms = 0;
    int force_flag = 0;
    long port = 0;
    char *host = NULL;

    /* Parse the command for syntax and arguments. */
    for (int j = 1; j < c->argc; j++) {
        if (!strcasecmp(c->argv[j]->ptr,"timeout") && (j + 1 < c->argc) &&
            timeout_in_ms == 0)
        {
            if (getLongFromObjectOrReply(c,c->argv[j + 1],
                        &timeout_in_ms,NULL) != C_OK) return;
            if (timeout_in_ms <= 0) {
                addReplyError(c,"FAILOVER timeout must be greater than 0");
                return;
            }
            j++;
        } else if (!strcasecmp(c->argv[j]->ptr,"to") && (j + 2 < c->argc) &&
            !host) 
        {
            if (getLongFromObjectOrReply(c,c->argv[j + 2],&port,NULL) != C_OK)
                return;
            host = c->argv[j + 1]->ptr;
            j += 2;
        } else if (!strcasecmp(c->argv[j]->ptr,"force") && !force_flag) {
            force_flag = 1;
        } else {
            addReplyErrorObject(c,shared.syntaxerr);
            return;
        }
    }

    if (server.failover_state != NO_FAILOVER) {
        addReplyError(c,"FAILOVER already in progress.");
        return;
    }

    if (server.masterhost) {
        addReplyError(c,"FAILOVER is not valid when server is a replica.");
        return;
    }

    if (listLength(server.slaves) == 0) {
        addReplyError(c,"FAILOVER requires connected replicas.");
        return; 
    }

    if (force_flag && (!timeout_in_ms || !host)) {
        addReplyError(c,"FAILOVER with force option requires both a timeout "
            "and target HOST and IP.");
        return;     
    }

    /* If a replica address was provided, validate that it is connected. */
    if (host) {
        client *replica = findReplica(host, port);

        if (replica == NULL) {
            addReplyError(c,"FAILOVER target HOST and PORT is not "
                            "a replica.");
            return;
        }

        /* Check if requested replica is online */
        if (replica->replstate != SLAVE_STATE_ONLINE) {
            addReplyError(c,"FAILOVER target replica is not online.");
            return;
        }

        server.target_replica_host = zstrdup(host);
        server.target_replica_port = port;
        serverLog(LL_NOTICE,"FAILOVER requested to %s:%ld.",host,port);
    } else {
        serverLog(LL_NOTICE,"FAILOVER requested to any replica.");
    }

    mstime_t now = mstime();
    if (timeout_in_ms) {
        server.failover_end_time = now + timeout_in_ms;
    }
    
    server.force_failover = force_flag;
    server.failover_state = FAILOVER_WAIT_FOR_SYNC;
    /* Cluster failover will unpause eventually */
    pauseClients(PAUSE_DURING_FAILOVER, LLONG_MAX, CLIENT_PAUSE_WRITE);
    addReply(c,shared.ok);
}

/* Failover cron function, checks coordinated failover state. 
 *
 * Implementation note: The current implementation calls replicationSetMaster()
 * to start the failover request, this has some unintended side effects if the
 * failover doesn't work like blocked clients will be unblocked and replicas will
 * be disconnected. This could be optimized further.
 */
/**故障转移 cron 功能，检查协调的故障转移状态。
 * 实现说明：当前实现调用 replicationSetMaster() 来启动故障转移请求，如果故障转移不起作用，
 * 这会产生一些意想不到的副作用，例如被阻塞的客户端将被解除阻塞并且副本将被断开连接。这可以进一步优化。*/
void updateFailoverStatus(void) {
    if (server.failover_state != FAILOVER_WAIT_FOR_SYNC) return;
    mstime_t now = server.mstime;

    /* Check if failover operation has timed out */
    if (server.failover_end_time && server.failover_end_time <= now) {
        if (server.force_failover) {
            serverLog(LL_NOTICE,
                "FAILOVER to %s:%d time out exceeded, failing over.",
                server.target_replica_host, server.target_replica_port);
            server.failover_state = FAILOVER_IN_PROGRESS;
            /* If timeout has expired force a failover if requested. */
            replicationSetMaster(server.target_replica_host,
                server.target_replica_port);
            return;
        } else {
            /* Force was not requested, so timeout. */
            abortFailover("Replica never caught up before timeout");
            return;
        }
    }

    /* Check to see if the replica has caught up so failover can start */
    client *replica = NULL;
    if (server.target_replica_host) {
        replica = findReplica(server.target_replica_host, 
            server.target_replica_port);
    } else {
        listIter li;
        listNode *ln;

        listRewind(server.slaves,&li);
        /* Find any replica that has matched our repl_offset */
        while((ln = listNext(&li))) {
            replica = ln->value;
            if (replica->repl_ack_off == server.master_repl_offset) {
                char ip[NET_IP_STR_LEN], *replicaaddr = replica->slave_addr;

                if (!replicaaddr) {
                    if (connPeerToString(replica->conn,ip,sizeof(ip),NULL) == -1)
                        continue;
                    replicaaddr = ip;
                }

                /* We are now failing over to this specific node */
                server.target_replica_host = zstrdup(replicaaddr);
                server.target_replica_port = replica->slave_listening_port;
                break;
            }
        }
    }

    /* We've found a replica that is caught up */
    if (replica && (replica->repl_ack_off == server.master_repl_offset)) {
        server.failover_state = FAILOVER_IN_PROGRESS;
        serverLog(LL_NOTICE,
                "Failover target %s:%d is synced, failing over.",
                server.target_replica_host, server.target_replica_port);
        /* Designated replica is caught up, failover to it. */
        replicationSetMaster(server.target_replica_host,
            server.target_replica_port);
    }
}
