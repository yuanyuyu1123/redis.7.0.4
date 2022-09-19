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
#include "atomicvar.h"
#include "cluster.h"
#include "script.h"
#include <sys/socket.h>
#include <sys/uio.h>
#include <math.h>
#include <ctype.h>

static void setProtocolError(const char *errstr, client *c);
int postponeClientRead(client *c);
int ProcessingEventsWhileBlocked = 0; /* See processEventsWhileBlocked(). */

/* Return the size consumed from the allocator, for the specified SDS string,
 * including internal fragmentation. This function is used in order to compute
 * the client output buffer size. */
/**返回分配器消耗的大小，用于指定的 SDS 字符串，包括内部碎片。此函数用于计算客户端输出缓冲区大小。*/
size_t sdsZmallocSize(sds s) {
    void *sh = sdsAllocPtr(s);
    return zmalloc_size(sh);
}

/* Return the amount of memory used by the sds string at object->ptr
 * for a string object. This includes internal fragmentation. */
//在 object->ptr 处为字符串对象返回 sds 字符串使用的内存量。这包括内部碎片。
size_t getStringObjectSdsUsedMemory(robj *o) {
    serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);
    switch(o->encoding) {
    case OBJ_ENCODING_RAW: return sdsZmallocSize(o->ptr);
    case OBJ_ENCODING_EMBSTR: return zmalloc_size(o)-sizeof(robj);
    default: return 0; /* Just integer encoding for now. */
    }
}

/* Return the length of a string object.
 * This does NOT includes internal fragmentation or sds unused space. */
//返回字符串对象的长度。这不包括内部碎片或 sds 未使用的空间。
size_t getStringObjectLen(robj *o) {
    serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);
    switch(o->encoding) {
    case OBJ_ENCODING_RAW: return sdslen(o->ptr);
    case OBJ_ENCODING_EMBSTR: return sdslen(o->ptr);
    default: return 0; /* Just integer encoding for now. 现在只是整数编码。*/
    }
}

/* Client.reply list dup and free methods. Client.reply 列出 dup 和 free 方法。*/
void *dupClientReplyValue(void *o) {
    clientReplyBlock *old = o;
    clientReplyBlock *buf = zmalloc(sizeof(clientReplyBlock) + old->size);
    memcpy(buf, o, sizeof(clientReplyBlock) + old->size);
    return buf;
}

void freeClientReplyValue(void *o) {
    zfree(o);
}

int listMatchObjects(void *a, void *b) {
    return equalStringObjects(a,b);
}

/* This function links the client to the global linked list of clients.
 * unlinkClient() does the opposite, among other things. */
//该函数将客户端链接到客户端的全局链表。 unlinkClient() 做相反的事情，除其他外。
void linkClient(client *c) {
    listAddNodeTail(server.clients,c);
    /* Note that we remember the linked list node where the client is stored,
     * this way removing the client in unlinkClient() will not require
     * a linear scan, but just a constant time operation. */
    //请注意，我们记住存储客户端的链表节点，这样在 unlinkClient() 中删除客户端将不需要线性扫描，而只是一个恒定时间的操作。
    c->client_list_node = listLast(server.clients);
    uint64_t id = htonu64(c->id);
    raxInsert(server.clients_index,(unsigned char*)&id,sizeof(id),c,NULL);
}

/* Initialize client authentication state.
 */
//初始化客户端认证状态。
static void clientSetDefaultAuth(client *c) {
    /* If the default user does not require authentication, the user is
     * directly authenticated. */
    //如果默认用户不需要认证，则直接认证用户。
    c->user = DefaultUser;
    c->authenticated = (c->user->flags & USER_FLAG_NOPASS) &&
                       !(c->user->flags & USER_FLAG_DISABLED);
}

int authRequired(client *c) {
    /* Check if the user is authenticated. This check is skipped in case
     * the default user is flagged as "nopass" and is active. */
    //检查用户是否已通过身份验证。如果默认用户被标记为“nopass”并且处于活动状态，则会跳过此检查。
    int auth_required = (!(DefaultUser->flags & USER_FLAG_NOPASS) ||
                          (DefaultUser->flags & USER_FLAG_DISABLED)) &&
                        !c->authenticated;
    return auth_required;
}

client *createClient(connection *conn) {
    client *c = zmalloc(sizeof(client));

    /* passing NULL as conn it is possible to create a non connected client.
     * This is useful since all the commands needs to be executed
     * in the context of a client. When commands are executed in other
     * contexts (for instance a Lua script) we need a non connected client. */
    /**将 NULL 作为 conn 传递，可以创建未连接的客户端。这很有用，因为所有命令都需要在客户端的上下文中执行。
     * 当命令在其他上下文（例如 Lua 脚本）中执行时，我们需要一个未连接的客户端。*/
    if (conn) {
        connEnableTcpNoDelay(conn);
        if (server.tcpkeepalive)
            connKeepAlive(conn,server.tcpkeepalive);
        connSetReadHandler(conn, readQueryFromClient);
        connSetPrivateData(conn, c);
    }
    c->buf = zmalloc(PROTO_REPLY_CHUNK_BYTES);
    selectDb(c,0);
    uint64_t client_id;
    atomicGetIncr(server.next_client_id, client_id, 1);
    c->id = client_id;
    c->resp = 2;
    c->conn = conn;
    c->name = NULL;
    c->bufpos = 0;
    c->buf_usable_size = zmalloc_usable_size(c->buf);
    c->buf_peak = c->buf_usable_size;
    c->buf_peak_last_reset_time = server.unixtime;
    c->ref_repl_buf_node = NULL;
    c->ref_block_pos = 0;
    c->qb_pos = 0;
    c->querybuf = sdsempty();
    c->querybuf_peak = 0;
    c->reqtype = 0;
    c->argc = 0;
    c->argv = NULL;
    c->argv_len = 0;
    c->argv_len_sum = 0;
    c->original_argc = 0;
    c->original_argv = NULL;
    c->cmd = c->lastcmd = c->realcmd = NULL;
    c->multibulklen = 0;
    c->bulklen = -1;
    c->sentlen = 0;
    c->flags = 0;
    c->slot = -1;
    c->ctime = c->lastinteraction = server.unixtime;
    clientSetDefaultAuth(c);
    c->replstate = REPL_STATE_NONE;
    c->repl_start_cmd_stream_on_ack = 0;
    c->reploff = 0;
    c->read_reploff = 0;
    c->repl_applied = 0;
    c->repl_ack_off = 0;
    c->repl_ack_time = 0;
    c->repl_last_partial_write = 0;
    c->slave_listening_port = 0;
    c->slave_addr = NULL;
    c->slave_capa = SLAVE_CAPA_NONE;
    c->slave_req = SLAVE_REQ_NONE;
    c->reply = listCreate();
    c->deferred_reply_errors = NULL;
    c->reply_bytes = 0;
    c->obuf_soft_limit_reached_time = 0;
    listSetFreeMethod(c->reply,freeClientReplyValue);
    listSetDupMethod(c->reply,dupClientReplyValue);
    c->btype = BLOCKED_NONE;
    c->bpop.timeout = 0;
    c->bpop.keys = dictCreate(&objectKeyHeapPointerValueDictType);
    c->bpop.target = NULL;
    c->bpop.xread_group = NULL;
    c->bpop.xread_consumer = NULL;
    c->bpop.xread_group_noack = 0;
    c->bpop.numreplicas = 0;
    c->bpop.reploffset = 0;
    c->woff = 0;
    c->watched_keys = listCreate();
    c->pubsub_channels = dictCreate(&objectKeyPointerValueDictType);
    c->pubsub_patterns = listCreate();
    c->pubsubshard_channels = dictCreate(&objectKeyPointerValueDictType);
    c->peerid = NULL;
    c->sockname = NULL;
    c->client_list_node = NULL;
    c->postponed_list_node = NULL;
    c->pending_read_list_node = NULL;
    c->client_tracking_redirection = 0;
    c->client_tracking_prefixes = NULL;
    c->last_memory_usage = 0;
    c->last_memory_type = CLIENT_TYPE_NORMAL;
    c->auth_callback = NULL;
    c->auth_callback_privdata = NULL;
    c->auth_module = NULL;
    listSetFreeMethod(c->pubsub_patterns,decrRefCountVoid);
    listSetMatchMethod(c->pubsub_patterns,listMatchObjects);
    c->mem_usage_bucket = NULL;
    c->mem_usage_bucket_node = NULL;
    if (conn) linkClient(c);
    initClientMultiState(c);
    return c;
}

void installClientWriteHandler(client *c) {
    int ae_barrier = 0;
    /* For the fsync=always policy, we want that a given FD is never
     * served for reading and writing in the same event loop iteration,
     * so that in the middle of receiving the query, and serving it
     * to the client, we'll call beforeSleep() that will do the
     * actual fsync of AOF to disk. the write barrier ensures that. */
    /**对于 fsync=always 策略，我们希望给定的 FD 永远不会在同一个事件循环迭代中用于读取和写入，
     * 因此在接收查询并将其提供给客户端的过程中，我们将调用 beforeSleep( ) 这将执行 AOF 到磁盘的实际 fsync。
     * 写屏障确保了这一点。*/
    if (server.aof_state == AOF_ON &&
        server.aof_fsync == AOF_FSYNC_ALWAYS)
    {
        ae_barrier = 1;
    }
    if (connSetWriteHandlerWithBarrier(c->conn, sendReplyToClient, ae_barrier) == C_ERR) {
        freeClientAsync(c);
    }
}

/* This function puts the client in the queue of clients that should write
 * their output buffers to the socket. Note that it does not *yet* install
 * the write handler, to start clients are put in a queue of clients that need
 * to write, so we try to do that before returning in the event loop (see the
 * handleClientsWithPendingWrites() function).
 * If we fail and there is more data to write, compared to what the socket
 * buffers can hold, then we'll really install the handler. */
/**此函数将客户端放入应将其输出缓冲区写入套接字的客户端队列中。
 * 请注意，它还没有安装写处理程序，启动客户端被放入需要写的客户端队列中，
 * 因此我们尝试在事件循环中返回之前执行此操作（参见 handleClientsWithPendingWrites() 函数）。
 * 如果我们失败并且与套接字缓冲区可以容纳的数据相比还有更多数据要写入，那么我们将真正安装处理程序。*/
void putClientInPendingWriteQueue(client *c) {
    /* Schedule the client to write the output buffers to the socket only
     * if not already done and, for slaves, if the slave can actually receive
     * writes at this stage. */
    //调度客户端仅在尚未完成时将输出缓冲区写入套接字，对于从属，如果从属在此阶段实际上可以接收写入。
    if (!(c->flags & CLIENT_PENDING_WRITE) &&
        (c->replstate == REPL_STATE_NONE ||
         (c->replstate == SLAVE_STATE_ONLINE && !c->repl_start_cmd_stream_on_ack)))
    {
        /* Here instead of installing the write handler, we just flag the
         * client and put it into a list of clients that have something
         * to write to the socket. This way before re-entering the event
         * loop, we can try to directly write to the client sockets avoiding
         * a system call. We'll only really install the write handler if
         * we'll not be able to write the whole reply at once. */
        /**这里没有安装写处理程序，我们只是标记客户端并将其放入有内容要写入套接字的客户端列表中。
         * 这样在重新进入事件循环之前，我们可以尝试直接写入客户端套接字，避免系统调用。如果我们不能一次写出整个回复，
         * 我们只会真正安装写处理程序。*/
        c->flags |= CLIENT_PENDING_WRITE;
        listAddNodeHead(server.clients_pending_write,c);
    }
}

/* This function is called every time we are going to transmit new data
 * to the client. The behavior is the following:
 *
 * If the client should receive new data (normal clients will) the function
 * returns C_OK, and make sure to install the write handler in our event
 * loop so that when the socket is writable new data gets written.
 *
 * If the client should not receive new data, because it is a fake client
 * (used to load AOF in memory), a master or because the setup of the write
 * handler failed, the function returns C_ERR.
 *
 * The function may return C_OK without actually installing the write
 * event handler in the following cases:
 *
 * 1) The event handler should already be installed since the output buffer
 *    already contains something.
 * 2) The client is a slave but not yet online, so we want to just accumulate
 *    writes in the buffer but not actually sending them yet.
 *
 * Typically gets called every time a reply is built, before adding more
 * data to the clients output buffers. If the function returns C_ERR no
 * data should be appended to the output buffers. */
/**
 * 每次我们要向客户端传输新数据时都会调用此函数。行为如下：
 *   如果客户端应该接收新数据（普通客户端会），该函数返回 C_OK，并确保在我们的事件循环中安装写处理程序，以便在套接字可写时写入新数据。
 *   如果客户端不应该接收新数据，因为它是假客户端（用于将 AOF 加载到内存中）、master 或
 * 因为写入处理程序的设置失败，该函数返回 C_ERR。在以下情况下，该函数可能会返回 C_OK 而不实际安装写事件处理程序：
 *   1) 应该已经安装了事件处理程序，因为输出缓冲区已经包含一些东西。
 *   2) 客户端是从机但尚未上线，所以我们只想在缓冲区中累积写入，但还没有真正发送它们。
 * 通常在每次构建回复时调用，然后将更多数据添加到客户端输出缓冲区。如果函数返回 C_ERR，则不应将数据附加到输出缓冲区。
 * */
int prepareClientToWrite(client *c) {
    /* If it's the Lua client we always return ok without installing any
     * handler since there is no socket at all. */
    /**如果是 Lua 客户端，我们总是返回 ok 而不安装任何处理程序，因为根本没有套接字。*/
    if (c->flags & (CLIENT_SCRIPT|CLIENT_MODULE)) return C_OK;

    /* If CLIENT_CLOSE_ASAP flag is set, we need not write anything. */
    /**如果设置了 CLIENT_CLOSE_ASAP 标志，我们不需要写任何东西。*/
    if (c->flags & CLIENT_CLOSE_ASAP) return C_ERR;

    /* CLIENT REPLY OFF / SKIP handling: don't send replies. */
    /**客户回复关闭跳过处理：不发送回复。*/
    if (c->flags & (CLIENT_REPLY_OFF|CLIENT_REPLY_SKIP)) return C_ERR;

    /* Masters don't receive replies, unless CLIENT_MASTER_FORCE_REPLY flag
     * is set. */
    /**除非设置了 CLIENT_MASTER_FORCE_REPLY 标志，否则 Master 不会收到回复。*/
    if ((c->flags & CLIENT_MASTER) &&
        !(c->flags & CLIENT_MASTER_FORCE_REPLY)) return C_ERR;

    if (!c->conn) return C_ERR; /* Fake client for AOF loading. 用于 AOF 加载的假客户端。*/

    /* Schedule the client to write the output buffers to the socket, unless
     * it should already be setup to do so (it has already pending data).
     *
     * If CLIENT_PENDING_READ is set, we're in an IO thread and should
     * not put the client in pending write queue. Instead, it will be
     * done by handleClientsWithPendingReadsUsingThreads() upon return.
     */
    /**安排客户端将输出缓冲区写入套接字，除非它应该已经设置为这样做（它已经有待处理的数据）。
     * 如果设置了 CLIENT_PENDING_READ，则我们处于 IO 线程中，不应将客户端置于挂起的写入队列中。
     * 相反，它会在返回时由 handleClientsWithPendingReadsUsingThreads() 完成。*/
    if (!clientHasPendingReplies(c) && io_threads_op == IO_THREADS_OP_IDLE)
        putClientInPendingWriteQueue(c);

    /* Authorize the caller to queue in the output buffer of this client. */
    /**授权调用者在此客户端的输出缓冲区中排队。*/
    return C_OK;
}

/* -----------------------------------------------------------------------------
 * Low level functions to add more data to output buffers.
 * 将更多数据添加到输出缓冲区的低级函数。
 * -------------------------------------------------------------------------- */

/* Attempts to add the reply to the static buffer in the client struct.
 * Returns the length of data that is added to the reply buffer.
 *
 * Sanitizer suppression: client->buf_usable_size determined by
 * zmalloc_usable_size() call. Writing beyond client->buf boundaries confuses
 * sanitizer and generates a false positive out-of-bounds error */
/**尝试将回复添加到客户端结构中的静态缓冲区。返回添加到回复缓冲区的数据长度。
 * 消毒剂抑制：client->buf_usable_size 由 zmalloc_usable_size() 调用确定。
 * 超出 client->buf 边界的写入会混淆 sanitizer 并生成误报越界错误*/
REDIS_NO_SANITIZE("bounds")
size_t _addReplyToBuffer(client *c, const char *s, size_t len) {
    size_t available = c->buf_usable_size - c->bufpos;

    /* If there already are entries in the reply list, we cannot
     * add anything more to the static buffer. */
    /**如果回复列表中已经有条目，我们不能再向静态缓冲区添加任何内容。*/
    if (listLength(c->reply) > 0) return 0;

    size_t reply_len = len > available ? available : len;
    memcpy(c->buf+c->bufpos,s,reply_len);
    c->bufpos+=reply_len;
    /* We update the buffer peak after appending the reply to the buffer */
    /**我们在将回复附加到缓冲区后更新缓冲区峰值*/
    if(c->buf_peak < (size_t)c->bufpos)
        c->buf_peak = (size_t)c->bufpos;
    return reply_len;
}

/* Adds the reply to the reply linked list.
 * Note: some edits to this function need to be relayed to AddReplyFromClient. */
/**将回复添加到回复链接列表。注意：对该函数的一些编辑需要转发给 AddReplyFromClient。*/
void _addReplyProtoToList(client *c, const char *s, size_t len) {
    listNode *ln = listLast(c->reply);
    clientReplyBlock *tail = ln? listNodeValue(ln): NULL;

    /* Note that 'tail' may be NULL even if we have a tail node, because when
     * addReplyDeferredLen() is used, it sets a dummy node to NULL just
     * to fill it later, when the size of the bulk length is set. */
    /**请注意，即使我们有一个尾节点，'tail' 也可能为 NULL，因为当使用 addReplyDeferredLen() 时，
     * 它会将一个虚拟节点设置为 NULL，只是为了稍后在设置块长度的大小时填充它。*/

    /* Append to tail string when possible. 尽可能附加到尾字符串。*/
    if (tail) {
        /* Copy the part we can fit into the tail, and leave the rest for a
         * new node */
        /**复制我们可以放入尾部的部分，并将其余部分留给新节点*/
        size_t avail = tail->size - tail->used;
        size_t copy = avail >= len? len: avail;
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
        size_t size = len < PROTO_REPLY_CHUNK_BYTES? PROTO_REPLY_CHUNK_BYTES: len;
        tail = zmalloc_usable(size + sizeof(clientReplyBlock), &usable_size);
        /* take over the allocation's internal fragmentation */
        /**接管分配的内部碎片*/
        tail->size = usable_size - sizeof(clientReplyBlock);
        tail->used = len;
        memcpy(tail->buf, s, len);
        listAddNodeTail(c->reply, tail);
        c->reply_bytes += tail->size;

        closeClientOnOutputBufferLimitReached(c, 1);
    }
}

void _addReplyToBufferOrList(client *c, const char *s, size_t len) {
    if (c->flags & CLIENT_CLOSE_AFTER_REPLY) return;

    /* Replicas should normally not cause any writes to the reply buffer. In case a rogue replica sent a command on the
     * replication link that caused a reply to be generated we'll simply disconnect it.
     * Note this is the simplest way to check a command added a response. Replication links are used to write data but
     * not for responses, so we should normally never get here on a replica client. */
    /**副本通常不应导致对回复缓冲区的任何写入。如果恶意副本在复制链接上发送了导致生成回复的命令，我们将简单地断开它。
     * 请注意，这是检查命令添加响应的最简单方法。复制链接用于写入数据但不用于响应，因此我们通常不应该在副本客户端上到达这里。*/
    if (getClientType(c) == CLIENT_TYPE_SLAVE) {
        sds cmdname = c->lastcmd ? c->lastcmd->fullname : NULL;
        logInvalidUseAndFreeClientAsync(c, "Replica generated a reply to command '%s'",
                                        cmdname ? cmdname : "<unknown>");
        return;
    }

    size_t reply_len = _addReplyToBuffer(c,s,len);
    if (len > reply_len) _addReplyProtoToList(c,s+reply_len,len-reply_len);
}

/* -----------------------------------------------------------------------------
 * Higher level functions to queue data on the client output buffer.
 * The following functions are the ones that commands implementations will call.
 * 更高级别的函数在客户端输出缓冲区上排队数据。以下函数是命令实现将调用的函数。
 * -------------------------------------------------------------------------- */

/* Add the object 'obj' string representation to the client output buffer. */
/**将对象“obj”字符串表示添加到客户端输出缓冲区。*/
void addReply(client *c, robj *obj) {
    if (prepareClientToWrite(c) != C_OK) return;

    if (sdsEncodedObject(obj)) {
        _addReplyToBufferOrList(c,obj->ptr,sdslen(obj->ptr));
    } else if (obj->encoding == OBJ_ENCODING_INT) {
        /* For integer encoded strings we just convert it into a string
         * using our optimized function, and attach the resulting string
         * to the output buffer. */
        /**对于整数编码字符串，我们只需使用优化函数将其转换为字符串，并将结果字符串附加到输出缓冲区。*/
        char buf[32];
        size_t len = ll2string(buf,sizeof(buf),(long)obj->ptr);
        _addReplyToBufferOrList(c,buf,len);
    } else {
        serverPanic("Wrong obj->encoding in addReply()");
    }
}

/* Add the SDS 's' string to the client output buffer, as a side effect
 * the SDS string is freed. */
/**将 SDS 的字符串添加到客户端输出缓冲区，作为释放 SDS 字符串的副作用。*/
void addReplySds(client *c, sds s) {
    if (prepareClientToWrite(c) != C_OK) {
        /* The caller expects the sds to be free'd. */
        /**调用者希望 sds 被释放。*/
        sdsfree(s);
        return;
    }
    _addReplyToBufferOrList(c,s,sdslen(s));
    sdsfree(s);
}

/* This low level function just adds whatever protocol you send it to the
 * client buffer, trying the static buffer initially, and using the string
 * of objects if not possible.
 *
 * It is efficient because does not create an SDS object nor an Redis object
 * if not needed. The object will only be created by calling
 * _addReplyProtoToList() if we fail to extend the existing tail object
 * in the list of objects. */
/**这个低级函数只是将您发送到客户端缓冲区的任何协议添加到客户端缓冲区，最初尝试静态缓冲区，并在不可能的情况下使用对象字符串。
 * 它是高效的，因为在不需要时不会创建 SDS 对象或 Redis 对象。
 * 如果我们未能扩展对象列表中的现有尾部对象，则只会通过调用 _addReplyProtoToList() 创建对象。*/
void addReplyProto(client *c, const char *s, size_t len) {
    if (prepareClientToWrite(c) != C_OK) return;
    _addReplyToBufferOrList(c,s,len);
}

/* Low level function called by the addReplyError...() functions.
 * It emits the protocol for a Redis error, in the form:
 *
 * -ERRORCODE Error Message<CR><LF>
 *
 * If the error code is already passed in the string 's', the error
 * code provided is used, otherwise the string "-ERR " for the generic
 * error code is automatically added.
 * Note that 's' must NOT end with \r\n. */
/**addReplyError...() 函数调用的低级函数。它发出 Redis 错误的协议，格式为：
 *    -ERRORCODE Error Message<CR><LF>
 * 如果错误代码已在字符串 's' 中传递，则使用提供的错误代码，否则使用字符串“-ERR " 自动添加通用错误代码。
 * 请注意，'s' 不能以 \r\n 结尾。*/
void addReplyErrorLength(client *c, const char *s, size_t len) {
    /* If the string already starts with "-..." then the error code
     * is provided by the caller. Otherwise we use "-ERR". */
    /**如果字符串已经以“-...”开头，则错误代码由调用者提供。否则我们使用“-ERR”。*/
    if (!len || s[0] != '-') addReplyProto(c,"-ERR ",5);
    addReplyProto(c,s,len);
    addReplyProto(c,"\r\n",2);
}

/* Do some actions after an error reply was sent (Log if needed, updates stats, etc.)
 * Possible flags:
 * * ERR_REPLY_FLAG_NO_STATS_UPDATE - indicate not to update any error stats. */
/**在发送错误回复后执行一些操作（如果需要记录，更新统计信息等） 可能的标志：
 * ERR_REPLY_FLAG_NO_STATS_UPDATE - 指示不更新任何错误统计信息。*/
void afterErrorReply(client *c, const char *s, size_t len, int flags) {
    /* Module clients fall into two categories:
     * Calls to RM_Call, in which case the error isn't being returned to a client, so should not be counted.
     * Module thread safe context calls to RM_ReplyWithError,
     * which will be added to a real client by the main thread later. */
    /**模块客户端分为两类： 对 RM_Call 的调用，在这种情况下，错误不会返回给客户端，因此不应计算在内。
     * 模块线程安全上下文调用 RM_ReplyWithError，稍后将由主线程添加到真实客户端。*/
    if (c->flags & CLIENT_MODULE) {
        if (!c->deferred_reply_errors) {
            c->deferred_reply_errors = listCreate();
            listSetFreeMethod(c->deferred_reply_errors, (void (*)(void*))sdsfree);
        }
        listAddNodeTail(c->deferred_reply_errors, sdsnewlen(s, len));
        return;
    }

    if (!(flags & ERR_REPLY_FLAG_NO_STATS_UPDATE)) {
        /* Increment the global error counter 增加全局错误计数器*/
        server.stat_total_error_replies++;
        /* Increment the error stats
         * If the string already starts with "-..." then the error prefix
         * is provided by the caller ( we limit the search to 32 chars). Otherwise we use "-ERR". */
        /**增加错误统计 如果字符串已经以“-...”开头，则错误前缀由调用者提供（我们将搜索限制为 32 个字符）。否则我们使用“-ERR”。*/
        if (s[0] != '-') {
            incrementErrorCount("ERR", 3);
        } else {
            char *spaceloc = memchr(s, ' ', len < 32 ? len : 32);
            if (spaceloc) {
                const size_t errEndPos = (size_t)(spaceloc - s);
                incrementErrorCount(s+1, errEndPos-1);
            } else {
                /* Fallback to ERR if we can't retrieve the error prefix */
                /**如果我们无法检索错误前缀，则回退到 ERR*/
                incrementErrorCount("ERR", 3);
            }
        }
    } else {
        /* stat_total_error_replies will not be updated, which means that
         * the cmd stats will not be updated as well, we still want this command
         * to be counted as failed so we update it here. We update c->realcmd in
         * case c->cmd was changed (like in GEOADD). */
        /**stat_total_error_replies 不会被更新，这意味着 cmd stats 也不会被更新，
         * 我们仍然希望这个命令被算作失败，所以我们在这里更新它。
         * 我们更新 c->realcmd 以防 c->cmd 被更改（如在 GEOADD 中）。*/
        c->realcmd->failed_calls++;
    }

    /* Sometimes it could be normal that a slave replies to a master with
     * an error and this function gets called. Actually the error will never
     * be sent because addReply*() against master clients has no effect...
     * A notable example is:
     *
     *    EVAL 'redis.call("incr",KEYS[1]); redis.call("nonexisting")' 1 x
     *
     * Where the master must propagate the first change even if the second
     * will produce an error. However it is useful to log such events since
     * they are rare and may hint at errors in a script or a bug in Redis. */
    /**有时，从站向主站回复错误并调用此函数可能是正常的。实际上永远不会发送错误，因为对主客户端的 addReply() 没有效果......
     * 一个值得注意的例子是：
     *   EVAL 'redis.call("incr",KEYS[1]); redis.call("nonexisting")' 1 x
     * master 必须传播第一个更改，即使第二个更改会产生错误。
     * 但是，记录此类事件很有用，因为它们很少见，并且可能暗示脚本中的错误或 Redis 中的错误。*/
    int ctype = getClientType(c);
    if (ctype == CLIENT_TYPE_MASTER || ctype == CLIENT_TYPE_SLAVE || c->id == CLIENT_ID_AOF) {
        char *to, *from;

        if (c->id == CLIENT_ID_AOF) {
            to = "AOF-loading-client";
            from = "server";
        } else if (ctype == CLIENT_TYPE_MASTER) {
            to = "master";
            from = "replica";
        } else {
            to = "replica";
            from = "master";
        }

        if (len > 4096) len = 4096;
        sds cmdname = c->lastcmd ? c->lastcmd->fullname : NULL;
        serverLog(LL_WARNING,"== CRITICAL == This %s is sending an error "
                             "to its %s: '%.*s' after processing the command "
                             "'%s'", from, to, (int)len, s, cmdname ? cmdname : "<unknown>");
        if (ctype == CLIENT_TYPE_MASTER && server.repl_backlog &&
            server.repl_backlog->histlen > 0)
        {
            showLatestBacklog();
        }
        server.stat_unexpected_error_replies++;

        /* Based off the propagation error behavior, check if we need to panic here. There
         * are currently two checked cases:
         * * If this command was from our master and we are not a writable replica.
         * * We are reading from an AOF file. */
        /**根据传播错误行为，检查我们是否需要在这里恐慌。当前有两种检查情况：
         * 如果此命令来自我们的主服务器并且我们不是可写副本。我们正在读取 AOF 文件。*/
        int panic_in_replicas = (ctype == CLIENT_TYPE_MASTER && server.repl_slave_ro)
            && (server.propagation_error_behavior == PROPAGATION_ERR_BEHAVIOR_PANIC ||
            server.propagation_error_behavior == PROPAGATION_ERR_BEHAVIOR_PANIC_ON_REPLICAS);
        int panic_in_aof = c->id == CLIENT_ID_AOF 
            && server.propagation_error_behavior == PROPAGATION_ERR_BEHAVIOR_PANIC;
        if (panic_in_replicas || panic_in_aof) {
            serverPanic("This %s panicked sending an error to its %s"
                " after processing the command '%s'",
                from, to, cmdname ? cmdname : "<unknown>");
        }
    }
}

/* The 'err' object is expected to start with -ERRORCODE and end with \r\n.
 * Unlike addReplyErrorSds and others alike which rely on addReplyErrorLength. */
/**'err' 对象应以 -ERRORCODE 开头并以 \r\n 结尾。与依赖 addReplyErrorLength 的 addReplyErrorSds 和其他类似方法不同。*/
void addReplyErrorObject(client *c, robj *err) {
    addReply(c, err);
    afterErrorReply(c, err->ptr, sdslen(err->ptr)-2, 0); /* Ignore trailing \r\n */
}

/* Sends either a reply or an error reply by checking the first char.
 * If the first char is '-' the reply is considered an error.
 * In any case the given reply is sent, if the reply is also recognize
 * as an error we also perform some post reply operations such as
 * logging and stats update. */
/**通过检查第一个字符来发送回复或错误回复。如果第一个字符是“-”，则回复被视为错误。
 * 在任何情况下都会发送给定的回复，如果回复也被识别为错误，我们还会执行一些发布回复操作，例如日志记录和统计更新。*/
void addReplyOrErrorObject(client *c, robj *reply) {
    serverAssert(sdsEncodedObject(reply));
    sds rep = reply->ptr;
    if (sdslen(rep) > 1 && rep[0] == '-') {
        addReplyErrorObject(c, reply);
    } else {
        addReply(c, reply);
    }
}

/* See addReplyErrorLength for expectations from the input string. */
/**有关输入字符串的期望，请参见 addReplyErrorLength。*/
void addReplyError(client *c, const char *err) {
    addReplyErrorLength(c,err,strlen(err));
    afterErrorReply(c,err,strlen(err),0);
}

/* Add error reply to the given client.
 * Supported flags:
 * * ERR_REPLY_FLAG_NO_STATS_UPDATE - indicate not to perform any error stats updates */
/**向给定的客户端添加错误回复。支持的标志：ERR_REPLY_FLAG_NO_STATS_UPDATE - 表示不执行任何错误统计更新*/
void addReplyErrorSdsEx(client *c, sds err, int flags) {
    addReplyErrorLength(c,err,sdslen(err));
    afterErrorReply(c,err,sdslen(err),flags);
    sdsfree(err);
}

/* See addReplyErrorLength for expectations from the input string. */
/* As a side effect the SDS string is freed. */
/**有关输入字符串的期望，请参见 addReplyErrorLength。作为副作用，SDS 字符串被释放。*/
void addReplyErrorSds(client *c, sds err) {
    addReplyErrorSdsEx(c, err, 0);
}

/* Internal function used by addReplyErrorFormat and addReplyErrorFormatEx.
 * Refer to afterErrorReply for more information about the flags. */
/**addReplyErrorFormat 和 addReplyErrorFormatEx 使用的内部函数。有关标志的更多信息，请参阅 afterErrorReply。*/
static void addReplyErrorFormatInternal(client *c, int flags, const char *fmt, va_list ap) {
    va_list cpy;
    va_copy(cpy,ap);
    sds s = sdscatvprintf(sdsempty(),fmt,cpy);
    va_end(cpy);
    /* Trim any newlines at the end (ones will be added by addReplyErrorLength) */
    /**修剪最后的任何换行符（将由 addReplyErrorLength 添加）*/
    s = sdstrim(s, "\r\n");
    /* Make sure there are no newlines in the middle of the string, otherwise
     * invalid protocol is emitted. */
    /**确保字符串中间没有换行符，否则会发出无效协议。*/
    s = sdsmapchars(s, "\r\n", "  ",  2);
    addReplyErrorLength(c,s,sdslen(s));
    afterErrorReply(c,s,sdslen(s),flags);
    sdsfree(s);
}

void addReplyErrorFormatEx(client *c, int flags, const char *fmt, ...) {
    va_list ap;
    va_start(ap,fmt);
    addReplyErrorFormatInternal(c, flags, fmt, ap);
    va_end(ap);
}

/* See addReplyErrorLength for expectations from the formatted string.
 * The formatted string is safe to contain \r and \n anywhere. */
/**有关格式化字符串的期望，请参阅 addReplyErrorLength。格式化的字符串可以安全地在任何地方包含 \r 和 \n。*/
void addReplyErrorFormat(client *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap,fmt);
    addReplyErrorFormatInternal(c, 0, fmt, ap);
    va_end(ap);
}

void addReplyErrorArity(client *c) {
    addReplyErrorFormat(c, "wrong number of arguments for '%s' command",
                        c->cmd->fullname);
}

void addReplyErrorExpireTime(client *c) {
    addReplyErrorFormat(c, "invalid expire time in '%s' command",
                        c->cmd->fullname);
}

void addReplyStatusLength(client *c, const char *s, size_t len) {
    addReplyProto(c,"+",1);
    addReplyProto(c,s,len);
    addReplyProto(c,"\r\n",2);
}

void addReplyStatus(client *c, const char *status) {
    addReplyStatusLength(c,status,strlen(status));
}

void addReplyStatusFormat(client *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap,fmt);
    sds s = sdscatvprintf(sdsempty(),fmt,ap);
    va_end(ap);
    addReplyStatusLength(c,s,sdslen(s));
    sdsfree(s);
}

/* Sometimes we are forced to create a new reply node, and we can't append to
 * the previous one, when that happens, we wanna try to trim the unused space
 * at the end of the last reply node which we won't use anymore. */
/**有时我们被迫创建一个新的回复节点，并且我们无法附加到前一个回复节点，当这种情况发生时，
 * 我们想尝试在我们不再使用的最后一个回复节点的末尾修剪未使用的空间。*/
void trimReplyUnusedTailSpace(client *c) {
    listNode *ln = listLast(c->reply);
    clientReplyBlock *tail = ln? listNodeValue(ln): NULL;

    /* Note that 'tail' may be NULL even if we have a tail node, because when
     * addReplyDeferredLen() is used */
    /**请注意，即使我们有一个尾节点，'tail' 也可能为 NULL，因为当使用 addReplyDeferredLen()*/
    if (!tail) return;

    /* We only try to trim the space is relatively high (more than a 1/4 of the
     * allocation), otherwise there's a high chance realloc will NOP.
     * Also, to avoid large memmove which happens as part of realloc, we only do
     * that if the used part is small.  */
    /**我们只尝试修剪比较高的空间（超过 14 的分配），否则 realloc 很有可能会 NOP。
     * 此外，为了避免作为 realloc 的一部分发生的大型 memmove，我们仅在使用的部分很小时才这样做。*/
    if (tail->size - tail->used > tail->size / 4 &&
        tail->used < PROTO_REPLY_CHUNK_BYTES)
    {
        size_t old_size = tail->size;
        tail = zrealloc(tail, tail->used + sizeof(clientReplyBlock));
        /* take over the allocation's internal fragmentation (at least for
         * memory usage tracking) */
        /**接管分配的内部碎片（至少用于内存使用跟踪）*/
        tail->size = zmalloc_usable_size(tail) - sizeof(clientReplyBlock);
        c->reply_bytes = c->reply_bytes + tail->size - old_size;
        listNodeValue(ln) = tail;
    }
}

/* Adds an empty object to the reply list that will contain the multi bulk
 * length, which is not known when this function is called. */
/**将一个空对象添加到将包含多批量长度的回复列表，该长度在调用此函数时是未知的。*/
void *addReplyDeferredLen(client *c) {
    /* Note that we install the write event here even if the object is not
     * ready to be sent, since we are sure that before returning to the
     * event loop setDeferredAggregateLen() will be called. */
    /**请注意，即使对象尚未准备好发送，我们也会在此处安装写入事件，
     * 因为我们确信在返回事件循环之前会调用 setDeferredAggregateLen()。*/
    if (prepareClientToWrite(c) != C_OK) return NULL;

    /* Replicas should normally not cause any writes to the reply buffer. In case a rogue replica sent a command on the
     * replication link that caused a reply to be generated we'll simply disconnect it.
     * Note this is the simplest way to check a command added a response. Replication links are used to write data but
     * not for responses, so we should normally never get here on a replica client. */
    /**副本通常不应导致对回复缓冲区的任何写入。如果恶意副本在复制链接上发送了导致生成回复的命令，我们将简单地断开它。
     * 请注意，这是检查命令添加响应的最简单方法。复制链接用于写入数据但不用于响应，因此我们通常不应该在副本客户端上到达这里。*/
    if (getClientType(c) == CLIENT_TYPE_SLAVE) {
        sds cmdname = c->lastcmd ? c->lastcmd->fullname : NULL;
        logInvalidUseAndFreeClientAsync(c, "Replica generated a reply to command '%s'",
                                        cmdname ? cmdname : "<unknown>");
        return NULL;
    }

    trimReplyUnusedTailSpace(c);
    listAddNodeTail(c->reply,NULL); /* NULL is our placeholder. NULL 是我们的占位符。*/
    return listLast(c->reply);
}

void setDeferredReply(client *c, void *node, const char *s, size_t length) {
    listNode *ln = (listNode*)node;
    clientReplyBlock *next, *prev;

    /* Abort when *node is NULL: when the client should not accept writes
     * we return NULL in addReplyDeferredLen() */
    /**当节点为 NULL 时中止：当客户端不应接受写入时，我们在 addReplyDeferredLen() 中返回 NULL*/
    if (node == NULL) return;
    serverAssert(!listNodeValue(ln));

    /* Normally we fill this dummy NULL node, added by addReplyDeferredLen(),
     * with a new buffer structure containing the protocol needed to specify
     * the length of the array following. However sometimes there might be room
     * in the previous/next node so we can instead remove this NULL node, and
     * suffix/prefix our data in the node immediately before/after it, in order
     * to save a write(2) syscall later. Conditions needed to do it:
     *
     * - The prev node is non-NULL and has space in it or
     * - The next node is non-NULL,
     * - It has enough room already allocated
     * - And not too large (avoid large memmove) */
    /**通常，我们用一个新的缓冲区结构填充这个由 addReplyDeferredLen() 添加的虚拟 NULL 节点，
     * 其中包含指定后面数组长度所需的协议。但是有时可能在前一个下一个节点中有空间，所以我们可以删除这个 NULL 节点，
     * 并在它之前的节点中为我们的数据添加后缀前缀，以便稍后保存 write(2) 系统调用。执行此操作所需的条件：
     *   - 上一个节点是非 NULL 并且其中有空间或
     *   - 下一个节点是非 NULL，
     *   - 它已经分配了足够的空间
     *   - 并且不会太大（避免使用大的 memmove）*/
    if (ln->prev != NULL && (prev = listNodeValue(ln->prev)) &&
        prev->size - prev->used > 0)
    {
        size_t len_to_copy = prev->size - prev->used;
        if (len_to_copy > length)
            len_to_copy = length;
        memcpy(prev->buf + prev->used, s, len_to_copy);
        prev->used += len_to_copy;
        length -= len_to_copy;
        if (length == 0) {
            listDelNode(c->reply, ln);
            return;
        }
        s += len_to_copy;
    }

    if (ln->next != NULL && (next = listNodeValue(ln->next)) &&
        next->size - next->used >= length &&
        next->used < PROTO_REPLY_CHUNK_BYTES * 4)
    {
        memmove(next->buf + length, next->buf, next->used);
        memcpy(next->buf, s, length);
        next->used += length;
        listDelNode(c->reply,ln);
    } else {
        /* Create a new node */
        clientReplyBlock *buf = zmalloc(length + sizeof(clientReplyBlock));
        /* Take over the allocation's internal fragmentation */
        /**接管分配的内部碎片*/
        buf->size = zmalloc_usable_size(buf) - sizeof(clientReplyBlock);
        buf->used = length;
        memcpy(buf->buf, s, length);
        listNodeValue(ln) = buf;
        c->reply_bytes += buf->size;

        closeClientOnOutputBufferLimitReached(c, 1);
    }
}

/* Populate the length object and try gluing it to the next chunk. */
/**填充长度对象并尝试将其粘合到下一个块。*/
void setDeferredAggregateLen(client *c, void *node, long length, char prefix) {
    serverAssert(length >= 0);

    /* Abort when *node is NULL: when the client should not accept writes
     * we return NULL in addReplyDeferredLen() */
    /**当节点为 NULL 时中止：当客户端不应接受写入时，我们在 addReplyDeferredLen() 中返回 NULL*/
    if (node == NULL) return;

    /* Things like *2\r\n, %3\r\n or ~4\r\n are emitted very often by the protocol
     * so we have a few shared objects to use if the integer is small
     * like it is most of the times. */
    /**协议经常发出诸如 2\r\n、%3\r\n 或 ~4\r\n 之类的东西，因此如果整数像大多数时候一样小，我们可以使用一些共享对象。*/
    const size_t hdr_len = OBJ_SHARED_HDR_STRLEN(length);
    const int opt_hdr = length < OBJ_SHARED_BULKHDR_LEN;
    if (prefix == '*' && opt_hdr) {
        setDeferredReply(c, node, shared.mbulkhdr[length]->ptr, hdr_len);
        return;
    }
    if (prefix == '%' && opt_hdr) {
        setDeferredReply(c, node, shared.maphdr[length]->ptr, hdr_len);
        return;
    }
    if (prefix == '~' && opt_hdr) {
        setDeferredReply(c, node, shared.sethdr[length]->ptr, hdr_len);
        return;
    }

    char lenstr[128];
    size_t lenstr_len = sprintf(lenstr, "%c%ld\r\n", prefix, length);
    setDeferredReply(c, node, lenstr, lenstr_len);
}

void setDeferredArrayLen(client *c, void *node, long length) {
    setDeferredAggregateLen(c,node,length,'*');
}

void setDeferredMapLen(client *c, void *node, long length) {
    int prefix = c->resp == 2 ? '*' : '%';
    if (c->resp == 2) length *= 2;
    setDeferredAggregateLen(c,node,length,prefix);
}

void setDeferredSetLen(client *c, void *node, long length) {
    int prefix = c->resp == 2 ? '*' : '~';
    setDeferredAggregateLen(c,node,length,prefix);
}

void setDeferredAttributeLen(client *c, void *node, long length) {
    serverAssert(c->resp >= 3);
    setDeferredAggregateLen(c,node,length,'|');
}

void setDeferredPushLen(client *c, void *node, long length) {
    serverAssert(c->resp >= 3);
    setDeferredAggregateLen(c,node,length,'>');
}

/* Add a double as a bulk reply 添加双重作为批量回复*/
void addReplyDouble(client *c, double d) {
    if (isinf(d)) {
        /* Libc in odd systems (Hi Solaris!) will format infinite in a
         * different way, so better to handle it in an explicit way. */
        /**奇数系统（嗨 Solaris！）中的 Libc 将以不同的方式格式化无限，因此最好以显式方式处理它。*/
        if (c->resp == 2) {
            addReplyBulkCString(c, d > 0 ? "inf" : "-inf");
        } else {
            addReplyProto(c, d > 0 ? ",inf\r\n" : ",-inf\r\n",
                              d > 0 ? 6 : 7);
        }
    } else {
        char dbuf[MAX_LONG_DOUBLE_CHARS+3],
             sbuf[MAX_LONG_DOUBLE_CHARS+32];
        int dlen, slen;
        if (c->resp == 2) {
            dlen = snprintf(dbuf,sizeof(dbuf),"%.17g",d);
            slen = snprintf(sbuf,sizeof(sbuf),"$%d\r\n%s\r\n",dlen,dbuf);
            addReplyProto(c,sbuf,slen);
        } else {
            dlen = snprintf(dbuf,sizeof(dbuf),",%.17g\r\n",d);
            addReplyProto(c,dbuf,dlen);
        }
    }
}

void addReplyBigNum(client *c, const char* num, size_t len) {
    if (c->resp == 2) {
        addReplyBulkCBuffer(c, num, len);
    } else {
        addReplyProto(c,"(",1);
        addReplyProto(c,num,len);
        addReply(c,shared.crlf);
    }
}

/* Add a long double as a bulk reply, but uses a human readable formatting
 * of the double instead of exposing the crude behavior of doubles to the
 * dear user. */
/**添加 long double 作为批量回复，但使用人类可读的 double 格式，而不是将 double 的粗略行为暴露给亲爱的用户。*/
void addReplyHumanLongDouble(client *c, long double d) {
    if (c->resp == 2) {
        robj *o = createStringObjectFromLongDouble(d,1);
        addReplyBulk(c,o);
        decrRefCount(o);
    } else {
        char buf[MAX_LONG_DOUBLE_CHARS];
        int len = ld2string(buf,sizeof(buf),d,LD_STR_HUMAN);
        addReplyProto(c,",",1);
        addReplyProto(c,buf,len);
        addReplyProto(c,"\r\n",2);
    }
}

/* Add a long long as integer reply or bulk len / multi bulk count.
 * Basically this is used to output <prefix><long long><crlf>. */
/**添加 long long as integer 回复或 bulk len 多批量计数。基本上这用于输出 <prefix><long long><crlf>。*/
void addReplyLongLongWithPrefix(client *c, long long ll, char prefix) {
    char buf[128];
    int len;

    /* Things like $3\r\n or *2\r\n are emitted very often by the protocol
     * so we have a few shared objects to use if the integer is small
     * like it is most of the times. */
    /**协议经常发出 3\r\n 或 2\r\n 之类的东西，因此如果整数像大多数时候一样小，我们可以使用一些共享对象。*/
    const int opt_hdr = ll < OBJ_SHARED_BULKHDR_LEN && ll >= 0;
    const size_t hdr_len = OBJ_SHARED_HDR_STRLEN(ll);
    if (prefix == '*' && opt_hdr) {
        addReplyProto(c,shared.mbulkhdr[ll]->ptr,hdr_len);
        return;
    } else if (prefix == '$' && opt_hdr) {
        addReplyProto(c,shared.bulkhdr[ll]->ptr,hdr_len);
        return;
    } else if (prefix == '%' && opt_hdr) {
        addReplyProto(c,shared.maphdr[ll]->ptr,hdr_len);
        return;
    } else if (prefix == '~' && opt_hdr) {
        addReplyProto(c,shared.sethdr[ll]->ptr,hdr_len);
        return;
    }

    buf[0] = prefix;
    len = ll2string(buf+1,sizeof(buf)-1,ll);
    buf[len+1] = '\r';
    buf[len+2] = '\n';
    addReplyProto(c,buf,len+3);
}

void addReplyLongLong(client *c, long long ll) {
    if (ll == 0)
        addReply(c,shared.czero);
    else if (ll == 1)
        addReply(c,shared.cone);
    else
        addReplyLongLongWithPrefix(c,ll,':');
}

void addReplyAggregateLen(client *c, long length, int prefix) {
    serverAssert(length >= 0);
    addReplyLongLongWithPrefix(c,length,prefix);
}

void addReplyArrayLen(client *c, long length) {
    addReplyAggregateLen(c,length,'*');
}

void addReplyMapLen(client *c, long length) {
    int prefix = c->resp == 2 ? '*' : '%';
    if (c->resp == 2) length *= 2;
    addReplyAggregateLen(c,length,prefix);
}

void addReplySetLen(client *c, long length) {
    int prefix = c->resp == 2 ? '*' : '~';
    addReplyAggregateLen(c,length,prefix);
}

void addReplyAttributeLen(client *c, long length) {
    serverAssert(c->resp >= 3);
    addReplyAggregateLen(c,length,'|');
}

void addReplyPushLen(client *c, long length) {
    serverAssert(c->resp >= 3);
    addReplyAggregateLen(c,length,'>');
}

void addReplyNull(client *c) {
    if (c->resp == 2) {
        addReplyProto(c,"$-1\r\n",5);
    } else {
        addReplyProto(c,"_\r\n",3);
    }
}

void addReplyBool(client *c, int b) {
    if (c->resp == 2) {
        addReply(c, b ? shared.cone : shared.czero);
    } else {
        addReplyProto(c, b ? "#t\r\n" : "#f\r\n",4);
    }
}

/* A null array is a concept that no longer exists in RESP3. However
 * RESP2 had it, so API-wise we have this call, that will emit the correct
 * RESP2 protocol, however for RESP3 the reply will always be just the
 * Null type "_\r\n". */
/**空数组是 RESP3 中不再存在的概念。然而 RESP2 有它，所以 API 方面我们有这个调用，
 * 它将发出正确的 RESP2 协议，但是对于 RESP3，回复将始终只是 Null 类型“_\r\n”。*/
void addReplyNullArray(client *c) {
    if (c->resp == 2) {
        addReplyProto(c,"*-1\r\n",5);
    } else {
        addReplyProto(c,"_\r\n",3);
    }
}

/* Create the length prefix of a bulk reply, example: $2234 */
/**创建批量回复的长度前缀，例如：2234*/
void addReplyBulkLen(client *c, robj *obj) {
    size_t len = stringObjectLen(obj);

    addReplyLongLongWithPrefix(c,len,'$');
}

/* Add a Redis Object as a bulk reply */
/**添加 Redis 对象作为批量回复*/
void addReplyBulk(client *c, robj *obj) {
    addReplyBulkLen(c,obj);
    addReply(c,obj);
    addReply(c,shared.crlf);
}

/* Add a C buffer as bulk reply 添加 C 缓冲区作为批量回复*/
void addReplyBulkCBuffer(client *c, const void *p, size_t len) {
    addReplyLongLongWithPrefix(c,len,'$');
    addReplyProto(c,p,len);
    addReply(c,shared.crlf);
}

/* Add sds to reply (takes ownership of sds and frees it) */
/**添加 sds 以回复（获取 sds 的所有权并释放它）*/
void addReplyBulkSds(client *c, sds s)  {
    addReplyLongLongWithPrefix(c,sdslen(s),'$');
    addReplySds(c,s);
    addReply(c,shared.crlf);
}

/* Set sds to a deferred reply (for symmetry with addReplyBulkSds it also frees the sds) */
/**将 sds 设置为延迟回复（为了与 addReplyBulkSds 对称，它还释放了 sds）*/
void setDeferredReplyBulkSds(client *c, void *node, sds s) {
    sds reply = sdscatprintf(sdsempty(), "$%d\r\n%s\r\n", (unsigned)sdslen(s), s);
    setDeferredReply(c, node, reply, sdslen(reply));
    sdsfree(reply);
    sdsfree(s);
}

/* Add a C null term string as bulk reply */
/**添加一个 C 空术语字符串作为批量回复*/
void addReplyBulkCString(client *c, const char *s) {
    if (s == NULL) {
        addReplyNull(c);
    } else {
        addReplyBulkCBuffer(c,s,strlen(s));
    }
}

/* Add a long long as a bulk reply */
/**添加long long as 批量回复*/
void addReplyBulkLongLong(client *c, long long ll) {
    char buf[64];
    int len;

    len = ll2string(buf,64,ll);
    addReplyBulkCBuffer(c,buf,len);
}

/* Reply with a verbatim type having the specified extension.
 *
 * The 'ext' is the "extension" of the file, actually just a three
 * character type that describes the format of the verbatim string.
 * For instance "txt" means it should be interpreted as a text only
 * file by the receiver, "md " as markdown, and so forth. Only the
 * three first characters of the extension are used, and if the
 * provided one is shorter than that, the remaining is filled with
 * spaces. */
/**使用具有指定扩展名的逐字类型进行回复。 'ext' 是文件的“扩展名”，实际上只是描述逐字字符串格式的三个字符类型。
 * 例如“txt”意味着它应该被接收者解释为一个纯文本文件，“md”作为markdown，等等。
 * 仅使用扩展名的前三个字符，如果提供的一个比这短，则剩余的用空格填充。*/
void addReplyVerbatim(client *c, const char *s, size_t len, const char *ext) {
    if (c->resp == 2) {
        addReplyBulkCBuffer(c,s,len);
    } else {
        char buf[32];
        size_t preflen = snprintf(buf,sizeof(buf),"=%zu\r\nxxx:",len+4);
        char *p = buf+preflen-4;
        for (int i = 0; i < 3; i++) {
            if (*ext == '\0') {
                p[i] = ' ';
            } else {
                p[i] = *ext++;
            }
        }
        addReplyProto(c,buf,preflen);
        addReplyProto(c,s,len);
        addReplyProto(c,"\r\n",2);
    }
}

/* Add an array of C strings as status replies with a heading.
 * This function is typically invoked by from commands that support
 * subcommands in response to the 'help' subcommand. The help array
 * is terminated by NULL sentinel. */
/**添加一个 C 字符串数组作为带有标题的状态回复。此函数通常由支持子命令的命令调用，以响应“帮助”子命令。
 * 帮助数组由 NULL sentinel 终止。*/
void addReplyHelp(client *c, const char **help) {
    sds cmd = sdsnew((char*) c->argv[0]->ptr);
    void *blenp = addReplyDeferredLen(c);
    int blen = 0;

    sdstoupper(cmd);
    addReplyStatusFormat(c,
        "%s <subcommand> [<arg> [value] [opt] ...]. Subcommands are:",cmd);
    sdsfree(cmd);

    while (help[blen]) addReplyStatus(c,help[blen++]);

    addReplyStatus(c,"HELP");
    addReplyStatus(c,"    Prints this help.");

    blen += 1;  /* Account for the header. */
    blen += 2;  /* Account for the footer. */
    setDeferredArrayLen(c,blenp,blen);
}

/* Add a suggestive error reply.
 * This function is typically invoked by from commands that support
 * subcommands in response to an unknown subcommand or argument error. */
/**添加提示性错误回复。此函数通常由支持子命令的命令调用，以响应未知的子命令或参数错误。*/
void addReplySubcommandSyntaxError(client *c) {
    sds cmd = sdsnew((char*) c->argv[0]->ptr);
    sdstoupper(cmd);
    addReplyErrorFormat(c,
        "unknown subcommand or wrong number of arguments for '%.128s'. Try %s HELP.",
        (char*)c->argv[1]->ptr,cmd);
    sdsfree(cmd);
}

/* Append 'src' client output buffers into 'dst' client output buffers.
 * This function clears the output buffers of 'src' */
/**将“src”客户端输出缓冲区附加到“dst”客户端输出缓冲区中。此函数清除“src”的输出缓冲区*/
void AddReplyFromClient(client *dst, client *src) {
    /* If the source client contains a partial response due to client output
     * buffer limits, propagate that to the dest rather than copy a partial
     * reply. We don't wanna run the risk of copying partial response in case
     * for some reason the output limits don't reach the same decision (maybe
     * they changed) */
    /**如果由于客户端输出缓冲区限制，源客户端包含部分响应，请将其传播到 dest 而不是复制部分回复。
     * 我们不想冒着复制部分响应的风险，以防由于某种原因输出限制没有达到相同的决定（也许他们改变了）*/
    if (src->flags & CLIENT_CLOSE_ASAP) {
        sds client = catClientInfoString(sdsempty(),dst);
        freeClientAsync(dst);
        serverLog(LL_WARNING,"Client %s scheduled to be closed ASAP for overcoming of output buffer limits.", client);
        sdsfree(client);
        return;
    }

    /* First add the static buffer (either into the static buffer or reply list) */
    /**首先添加静态缓冲区（进入静态缓冲区或回复列表）*/
    addReplyProto(dst,src->buf, src->bufpos);

    /* We need to check with prepareClientToWrite again (after addReplyProto)
     * since addReplyProto may have changed something (like CLIENT_CLOSE_ASAP) */
    /**我们需要再次检查 prepareClientToWrite（在 addReplyProto 之后），
     * 因为 addReplyProto 可能已经改变了一些东西（比如 CLIENT_CLOSE_ASAP）*/
    if (prepareClientToWrite(dst) != C_OK)
        return;

    /* We're bypassing _addReplyProtoToList, so we need to add the pre/post
     * checks in it. */
    /**我们绕过了 _addReplyProtoToList，所以我们需要在其中添加 prepost 检查。*/
    if (dst->flags & CLIENT_CLOSE_AFTER_REPLY) return;

    /* Concatenate the reply list into the dest */
    /**将回复列表连接到 dest*/
    if (listLength(src->reply))
        listJoin(dst->reply,src->reply);
    dst->reply_bytes += src->reply_bytes;
    src->reply_bytes = 0;
    src->bufpos = 0;

    if (src->deferred_reply_errors) {
        deferredAfterErrorReply(dst, src->deferred_reply_errors);
        listRelease(src->deferred_reply_errors);
        src->deferred_reply_errors = NULL;
    }

    /* Check output buffer limits 检查输出缓冲区限制*/
    closeClientOnOutputBufferLimitReached(dst, 1);
}

/* Append the listed errors to the server error statistics. the input
 * list is not modified and remains the responsibility of the caller. */
/**将列出的错误附加到服务器错误统计信息中。输入列表不会被修改，仍然是调用者的责任。*/
void deferredAfterErrorReply(client *c, list *errors) {
    listIter li;
    listNode *ln;
    listRewind(errors,&li);
    while((ln = listNext(&li))) {
        sds err = ln->value;
        afterErrorReply(c, err, sdslen(err), 0);
    }
}

/* Logically copy 'src' replica client buffers info to 'dst' replica.
 * Basically increase referenced buffer block node reference count. */
/**将“src”副本客户端缓冲区信息逻辑复制到“dst”副本。基本上增加引用的缓冲块节点引用计数。*/
void copyReplicaOutputBuffer(client *dst, client *src) {
    serverAssert(src->bufpos == 0 && listLength(src->reply) == 0);

    if (src->ref_repl_buf_node == NULL) return;
    dst->ref_repl_buf_node = src->ref_repl_buf_node;
    dst->ref_block_pos = src->ref_block_pos;
    ((replBufBlock *)listNodeValue(dst->ref_repl_buf_node))->refcount++;
}

/* Return true if the specified client has pending reply buffers to write to
 * the socket. */
/**如果指定的客户端有待写的回复缓冲区要写入套接字，则返回 true。*/
int clientHasPendingReplies(client *c) {
    if (getClientType(c) == CLIENT_TYPE_SLAVE) {
        /* Replicas use global shared replication buffer instead of
         * private output buffer. */
        /**副本使用全局共享复制缓冲区而不是私有输出缓冲区。*/
        serverAssert(c->bufpos == 0 && listLength(c->reply) == 0);
        if (c->ref_repl_buf_node == NULL) return 0;

        /* If the last replication buffer block content is totally sent,
         * we have nothing to send. */
        /**如果最后一个复制缓冲区块的内容被完全发送，我们就没有什么可发送的了。*/
        listNode *ln = listLast(server.repl_buffer_blocks);
        replBufBlock *tail = listNodeValue(ln);
        if (ln == c->ref_repl_buf_node &&
            c->ref_block_pos == tail->used) return 0;

        return 1;
    } else {
        return c->bufpos || listLength(c->reply);
    }
}

/* Return true if client connected from loopback interface */
/**如果客户端从环回接口连接，则返回 true*/
int islocalClient(client *c) {
    /* unix-socket */
    if (c->flags & CLIENT_UNIX_SOCKET) return 1;

    /* tcp */
    char cip[NET_IP_STR_LEN+1] = { 0 };
    connPeerToString(c->conn, cip, sizeof(cip)-1, NULL);

    return !strcmp(cip,"127.0.0.1") || !strcmp(cip,"::1");
}

void clientAcceptHandler(connection *conn) {
    client *c = connGetPrivateData(conn);

    if (connGetState(conn) != CONN_STATE_CONNECTED) {
        serverLog(LL_WARNING,
                "Error accepting a client connection: %s",
                connGetLastError(conn));
        freeClientAsync(c);
        return;
    }

    /* If the server is running in protected mode (the default) and there
     * is no password set, nor a specific interface is bound, we don't accept
     * requests from non loopback interfaces. Instead we try to explain the
     * user what to do to fix it if needed. */
    /**如果服务器运行在保护模式（默认）并且没有设置密码，也没有绑定特定的接口，我们不接受来自非环回接口的请求。
     * 相反，我们尝试向用户解释如何在需要时修复它。*/
    if (server.protected_mode &&
        DefaultUser->flags & USER_FLAG_NOPASS)
    {
        if (!islocalClient(c)) {
            char *err =
                "-DENIED Redis is running in protected mode because protected "
                "mode is enabled and no password is set for the default user. "
                "In this mode connections are only accepted from the loopback interface. "
                "If you want to connect from external computers to Redis you "
                "may adopt one of the following solutions: "
                "1) Just disable protected mode sending the command "
                "'CONFIG SET protected-mode no' from the loopback interface "
                "by connecting to Redis from the same host the server is "
                "running, however MAKE SURE Redis is not publicly accessible "
                "from internet if you do so. Use CONFIG REWRITE to make this "
                "change permanent. "
                "2) Alternatively you can just disable the protected mode by "
                "editing the Redis configuration file, and setting the protected "
                "mode option to 'no', and then restarting the server. "
                "3) If you started the server manually just for testing, restart "
                "it with the '--protected-mode no' option. "
                "4) Setup a an authentication password for the default user. "
                "NOTE: You only need to do one of the above things in order for "
                "the server to start accepting connections from the outside.\r\n";
            if (connWrite(c->conn,err,strlen(err)) == -1) {
                /* Nothing to do, Just to avoid the warning... */
                /**无事可做，只是为了避免警告......*/
            }
            server.stat_rejected_conn++;
            freeClientAsync(c);
            return;
        }
    }

    server.stat_numconnections++;
    moduleFireServerEvent(REDISMODULE_EVENT_CLIENT_CHANGE,
                          REDISMODULE_SUBEVENT_CLIENT_CHANGE_CONNECTED,
                          c);
}

#define MAX_ACCEPTS_PER_CALL 1000
static void acceptCommonHandler(connection *conn, int flags, char *ip) {
    client *c;
    char conninfo[100];
    UNUSED(ip);

    if (connGetState(conn) != CONN_STATE_ACCEPTING) {
        serverLog(LL_VERBOSE,
            "Accepted client connection in error state: %s (conn: %s)",
            connGetLastError(conn),
            connGetInfo(conn, conninfo, sizeof(conninfo)));
        connClose(conn);
        return;
    }

    /* Limit the number of connections we take at the same time.
     *
     * Admission control will happen before a client is created and connAccept()
     * called, because we don't want to even start transport-level negotiation
     * if rejected. */
    /**限制我们同时连接的数量。准入控制将在创建客户端并调用 connAccept() 之前发生，因为如果被拒绝，我们甚至不想启动传输级协商。*/
    if (listLength(server.clients) + getClusterConnectionsCount()
        >= server.maxclients)
    {
        char *err;
        if (server.cluster_enabled)
            err = "-ERR max number of clients + cluster "
                  "connections reached\r\n";
        else
            err = "-ERR max number of clients reached\r\n";

        /* That's a best effort error message, don't check write errors.
         * Note that for TLS connections, no handshake was done yet so nothing
         * is written and the connection will just drop. */
        /**这是一个尽力而为的错误消息，不要检查写入错误。请注意，对于 TLS 连接，
         * 尚未完成任何握手，因此没有写入任何内容，连接将直接断开。*/
        if (connWrite(conn,err,strlen(err)) == -1) {
            /* Nothing to do, Just to avoid the warning... */
        }
        server.stat_rejected_conn++;
        connClose(conn);
        return;
    }

    /* Create connection and client */
    if ((c = createClient(conn)) == NULL) {
        serverLog(LL_WARNING,
            "Error registering fd event for the new client: %s (conn: %s)",
            connGetLastError(conn),
            connGetInfo(conn, conninfo, sizeof(conninfo)));
        connClose(conn); /* May be already closed, just ignore errors 可能已经关闭，忽略错误*/
        return;
    }

    /* Last chance to keep flags 保留旗帜的最后机会*/
    c->flags |= flags;

    /* Initiate accept.
     *
     * Note that connAccept() is free to do two things here:
     * 1. Call clientAcceptHandler() immediately;
     * 2. Schedule a future call to clientAcceptHandler().
     *
     * Because of that, we must do nothing else afterwards.
     */
    /**发起接受。注意 connAccept() 在这里可以自由地做两件事：
     *   1. 立即调用 clientAcceptHandler()；
     *   2. 安排未来对 clientAcceptHandler() 的调用。
     * 正因为如此，我们以后必须什么都不做。*/
    if (connAccept(conn, clientAcceptHandler) == C_ERR) {
        char conninfo[100];
        if (connGetState(conn) == CONN_STATE_ERROR)
            serverLog(LL_WARNING,
                    "Error accepting a client connection: %s (conn: %s)",
                    connGetLastError(conn), connGetInfo(conn, conninfo, sizeof(conninfo)));
        freeClient(connGetPrivateData(conn));
        return;
    }
}

void acceptTcpHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cport, cfd, max = MAX_ACCEPTS_PER_CALL;
    char cip[NET_IP_STR_LEN];
    UNUSED(el);
    UNUSED(mask);
    UNUSED(privdata);

    while(max--) {
        cfd = anetTcpAccept(server.neterr, fd, cip, sizeof(cip), &cport);
        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK)
                serverLog(LL_WARNING,
                    "Accepting client connection: %s", server.neterr);
            return;
        }
        serverLog(LL_VERBOSE,"Accepted %s:%d", cip, cport);
        acceptCommonHandler(connCreateAcceptedSocket(cfd),0,cip);
    }
}

void acceptTLSHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cport, cfd, max = MAX_ACCEPTS_PER_CALL;
    char cip[NET_IP_STR_LEN];
    UNUSED(el);
    UNUSED(mask);
    UNUSED(privdata);

    while(max--) {
        cfd = anetTcpAccept(server.neterr, fd, cip, sizeof(cip), &cport);
        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK)
                serverLog(LL_WARNING,
                    "Accepting client connection: %s", server.neterr);
            return;
        }
        serverLog(LL_VERBOSE,"Accepted %s:%d", cip, cport);
        acceptCommonHandler(connCreateAcceptedTLS(cfd, server.tls_auth_clients),0,cip);
    }
}

void acceptUnixHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cfd, max = MAX_ACCEPTS_PER_CALL;
    UNUSED(el);
    UNUSED(mask);
    UNUSED(privdata);

    while(max--) {
        cfd = anetUnixAccept(server.neterr, fd);
        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK)
                serverLog(LL_WARNING,
                    "Accepting client connection: %s", server.neterr);
            return;
        }
        serverLog(LL_VERBOSE,"Accepted connection to %s", server.unixsocket);
        acceptCommonHandler(connCreateAcceptedSocket(cfd),CLIENT_UNIX_SOCKET,NULL);
    }
}

void freeClientOriginalArgv(client *c) {
    /* We didn't rewrite this client 我们没有重写这个客户端*/
    if (!c->original_argv) return;

    for (int j = 0; j < c->original_argc; j++)
        decrRefCount(c->original_argv[j]);
    zfree(c->original_argv);
    c->original_argv = NULL;
    c->original_argc = 0;
}

void freeClientArgv(client *c) {
    int j;
    for (j = 0; j < c->argc; j++)
        decrRefCount(c->argv[j]);
    c->argc = 0;
    c->cmd = NULL;
    c->argv_len_sum = 0;
    c->argv_len = 0;
    zfree(c->argv);
    c->argv = NULL;
}

/* Close all the slaves connections. This is useful in chained replication
 * when we resync with our own master and want to force all our slaves to
 * resync with us as well. */
/**关闭所有从属连接。当我们与自己的主服务器重新同步并希望强制所有从服务器也与我们重新同步时，这在链式复制中很有用。*/
void disconnectSlaves(void) {
    listIter li;
    listNode *ln;
    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        freeClient((client*)ln->value);
    }
}

/* Check if there is any other slave waiting dumping RDB finished expect me.
 * This function is useful to judge current dumping RDB can be used for full
 * synchronization or not. */
/**检查是否有任何其他从站等待转储 RDB 完成期待我。该函数用于判断当前转储RDB是否可以用于全同步。*/
int anyOtherSlaveWaitRdb(client *except_me) {
    listIter li;
    listNode *ln;

    listRewind(server.slaves, &li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;
        if (slave != except_me &&
            slave->replstate == SLAVE_STATE_WAIT_BGSAVE_END)
        {
            return 1;
        }
    }
    return 0;
}

/* Remove the specified client from global lists where the client could
 * be referenced, not including the Pub/Sub channels.
 * This is used by freeClient() and replicationCacheMaster(). */
/**从可以引用客户端的全局列表中删除指定的客户端，不包括 PubSub 频道。
 *
 * @return 这由 freeClient() 和 replicationCacheMaster() 使用。*/
void unlinkClient(client *c) {
    listNode *ln;

    /* If this is marked as current client unset it. */
    /**如果这被标记为当前客户端取消设置它。*/
    if (server.current_client == c) server.current_client = NULL;

    /* Certain operations must be done only if the client has an active connection.
     * If the client was already unlinked or if it's a "fake client" the
     * conn is already set to NULL. */
    /**只有当客户端具有活动连接时，才必须执行某些操作。如果客户端已经取消链接或者它是“假客户端”，则 conn 已经设置为 NULL。*/
    if (c->conn) {
        /* Remove from the list of active clients. */
        /**从活动客户端列表中删除。*/
        if (c->client_list_node) {
            uint64_t id = htonu64(c->id);
            raxRemove(server.clients_index,(unsigned char*)&id,sizeof(id),NULL);
            listDelNode(server.clients,c->client_list_node);
            c->client_list_node = NULL;
        }

        /* Check if this is a replica waiting for diskless replication (rdb pipe),
         * in which case it needs to be cleaned from that list */
        /**检查这是否是等待无盘复制（rdb 管道）的副本，在这种情况下，需要从该列表中清除它*/
        if (c->flags & CLIENT_SLAVE &&
            c->replstate == SLAVE_STATE_WAIT_BGSAVE_END &&
            server.rdb_pipe_conns)
        {
            int i;
            for (i=0; i < server.rdb_pipe_numconns; i++) {
                if (server.rdb_pipe_conns[i] == c->conn) {
                    rdbPipeWriteHandlerConnRemoved(c->conn);
                    server.rdb_pipe_conns[i] = NULL;
                    break;
                }
            }
        }
        connClose(c->conn);
        c->conn = NULL;
    }

    /* Remove from the list of pending writes if needed. */
    /**如果需要，从挂起的写入列表中删除。*/
    if (c->flags & CLIENT_PENDING_WRITE) {
        ln = listSearchKey(server.clients_pending_write,c);
        serverAssert(ln != NULL);
        listDelNode(server.clients_pending_write,ln);
        c->flags &= ~CLIENT_PENDING_WRITE;
    }

    /* Remove from the list of pending reads if needed. */
    /**如果需要，从待处理读取列表中删除。*/
    serverAssert(io_threads_op == IO_THREADS_OP_IDLE);
    if (c->pending_read_list_node != NULL) {
        listDelNode(server.clients_pending_read,c->pending_read_list_node);
        c->pending_read_list_node = NULL;
    }


    /* When client was just unblocked because of a blocking operation,
     * remove it from the list of unblocked clients. */
    /**当客户端由于阻塞操作而刚刚被解除阻塞时，将其从解除阻塞的客户端列表中删除。*/
    if (c->flags & CLIENT_UNBLOCKED) {
        ln = listSearchKey(server.unblocked_clients,c);
        serverAssert(ln != NULL);
        listDelNode(server.unblocked_clients,ln);
        c->flags &= ~CLIENT_UNBLOCKED;
    }

    /* Clear the tracking status. 清除跟踪状态。*/
    if (c->flags & CLIENT_TRACKING) disableTracking(c);
}

/* Clear the client state to resemble a newly connected client. */
/**清除客户端状态以类似于新连接的客户端。*/
void clearClientConnectionState(client *c) {
    listNode *ln;

    /* MONITOR clients are also marked with CLIENT_SLAVE, we need to
     * distinguish between the two.*/
    /**MONITOR客户端也标有CLIENT_SLAVE，我们需要区分这两者。*/
    if (c->flags & CLIENT_MONITOR) {
        ln = listSearchKey(server.monitors,c);
        serverAssert(ln != NULL);
        listDelNode(server.monitors,ln);

        c->flags &= ~(CLIENT_MONITOR|CLIENT_SLAVE);
    }

    serverAssert(!(c->flags &(CLIENT_SLAVE|CLIENT_MASTER)));

    if (c->flags & CLIENT_TRACKING) disableTracking(c);
    selectDb(c,0);
    c->resp = 2;

    clientSetDefaultAuth(c);
    moduleNotifyUserChanged(c);
    discardTransaction(c);

    pubsubUnsubscribeAllChannels(c,0);
    pubsubUnsubscribeShardAllChannels(c, 0);
    pubsubUnsubscribeAllPatterns(c,0);

    if (c->name) {
        decrRefCount(c->name);
        c->name = NULL;
    }

    /* Selectively clear state flags not covered above */
    /**有选择地清除上面未涵盖的状态标志*/
    c->flags &= ~(CLIENT_ASKING|CLIENT_READONLY|CLIENT_PUBSUB|
                  CLIENT_REPLY_OFF|CLIENT_REPLY_SKIP_NEXT);
}

void freeClient(client *c) {
    listNode *ln;

    /* If a client is protected, yet we need to free it right now, make sure
     * to at least use asynchronous freeing. */
    /**如果客户端受到保护，但我们需要立即释放它，请确保至少使用异步释放。*/
    if (c->flags & CLIENT_PROTECTED) {
        freeClientAsync(c);
        return;
    }

    /* For connected clients, call the disconnection event of modules hooks. */
    /**对于已连接的客户端，调用模块挂钩的断开连接事件。*/
    if (c->conn) {
        moduleFireServerEvent(REDISMODULE_EVENT_CLIENT_CHANGE,
                              REDISMODULE_SUBEVENT_CLIENT_CHANGE_DISCONNECTED,
                              c);
    }

    /* Notify module system that this client auth status changed. */
    /**通知模块系统此客户端身份验证状态已更改。*/
    moduleNotifyUserChanged(c);

    /* If this client was scheduled for async freeing we need to remove it
     * from the queue. Note that we need to do this here, because later
     * we may call replicationCacheMaster() and the client should already
     * be removed from the list of clients to free. */
    /**如果此客户端被安排为异步释放，我们需要将其从队列中删除。
     * 请注意，我们需要在这里执行此操作，因为稍后我们可能会调用 replicationCacheMaster() 并且
     * 客户端应该已经从客户端列表中删除以释放。*/
    if (c->flags & CLIENT_CLOSE_ASAP) {
        ln = listSearchKey(server.clients_to_close,c);
        serverAssert(ln != NULL);
        listDelNode(server.clients_to_close,ln);
    }

    /* If it is our master that's being disconnected we should make sure
     * to cache the state to try a partial resynchronization later.
     *
     * Note that before doing this we make sure that the client is not in
     * some unexpected state, by checking its flags. */
    /**如果是我们的主服务器断开连接，我们应该确保缓存状态以便稍后尝试部分重新同步。
     * 请注意，在执行此操作之前，我们通过检查其标志来确保客户端没有处于某种意外状态。*/
    if (server.master && c->flags & CLIENT_MASTER) {
        serverLog(LL_WARNING,"Connection with master lost.");
        if (!(c->flags & (CLIENT_PROTOCOL_ERROR|CLIENT_BLOCKED))) {
            c->flags &= ~(CLIENT_CLOSE_ASAP|CLIENT_CLOSE_AFTER_REPLY);
            replicationCacheMaster(c);
            return;
        }
    }

    /* Log link disconnection with slave 记录与从机的链接断开*/
    if (getClientType(c) == CLIENT_TYPE_SLAVE) {
        serverLog(LL_WARNING,"Connection with replica %s lost.",
            replicationGetSlaveName(c));
    }

    /* Free the query buffer 释放查询缓冲区*/
    sdsfree(c->querybuf);
    c->querybuf = NULL;

    /* Deallocate structures used to block on blocking ops. */
    /**释放用于阻塞阻塞操作的结构。*/
    if (c->flags & CLIENT_BLOCKED) unblockClient(c);
    dictRelease(c->bpop.keys);

    /* UNWATCH all the keys   UNWATCH 所有的钥匙*/
    unwatchAllKeys(c);
    listRelease(c->watched_keys);

    /* Unsubscribe from all the pubsub channels */
    /**取消订阅所有 pubsub 频道*/
    pubsubUnsubscribeAllChannels(c,0);
    pubsubUnsubscribeShardAllChannels(c, 0);
    pubsubUnsubscribeAllPatterns(c,0);
    dictRelease(c->pubsub_channels);
    listRelease(c->pubsub_patterns);
    dictRelease(c->pubsubshard_channels);

    /* Free data structures. */
    listRelease(c->reply);
    zfree(c->buf);
    freeReplicaReferencedReplBuffer(c);
    freeClientArgv(c);
    freeClientOriginalArgv(c);
    if (c->deferred_reply_errors)
        listRelease(c->deferred_reply_errors);

    /* Unlink the client: this will close the socket, remove the I/O
     * handlers, and remove references of the client from different
     * places where active clients may be referenced. */
    /**取消链接客户端：这将关闭套接字，删除 IO 处理程序，并从可能引用活动客户端的不同位置删除客户端的引用。*/
    unlinkClient(c);

    /* Master/slave cleanup Case 1:
     * we lost the connection with a slave. */
    /**主从清理案例 1：我们失去了与从站的连接。*/
    if (c->flags & CLIENT_SLAVE) {
        /* If there is no any other slave waiting dumping RDB finished, the
         * current child process need not continue to dump RDB, then we kill it.
         * So child process won't use more memory, and we also can fork a new
         * child process asap to dump rdb for next full synchronization or bgsave.
         * But we also need to check if users enable 'save' RDB, if enable, we
         * should not remove directly since that means RDB is important for users
         * to keep data safe and we may delay configured 'save' for full sync. */
        /**如果没有其他slave等待dump RDB完成，当前子进程不需要继续dump RDB，那么我们就kill掉它。
         * 所以子进程不会占用更多内存，我们也可以尽快 fork 一个新的子进程来转储 rdb 以进行下一次完全同步或 bgsave。
         * 但是我们还需要检查用户是否启用了“保存”RDB，如果启用，我们不应该直接删除，
         * 因为这意味着 RDB 对于用户保持数据安全很重要，我们可能会延迟配置的“保存”以进行完全同步。*/
        if (server.saveparamslen == 0 &&
            c->replstate == SLAVE_STATE_WAIT_BGSAVE_END &&
            server.child_type == CHILD_TYPE_RDB &&
            server.rdb_child_type == RDB_CHILD_TYPE_DISK &&
            anyOtherSlaveWaitRdb(c) == 0)
        {
            killRDBChild();
        }
        if (c->replstate == SLAVE_STATE_SEND_BULK) {
            if (c->repldbfd != -1) close(c->repldbfd);
            if (c->replpreamble) sdsfree(c->replpreamble);
        }
        list *l = (c->flags & CLIENT_MONITOR) ? server.monitors : server.slaves;
        ln = listSearchKey(l,c);
        serverAssert(ln != NULL);
        listDelNode(l,ln);
        /* We need to remember the time when we started to have zero
         * attached slaves, as after some time we'll free the replication backlog. */
        /**我们需要记住我们开始零连接从属设备的时间，因为一段时间后我们将释放复制积压。*/
        if (getClientType(c) == CLIENT_TYPE_SLAVE && listLength(server.slaves) == 0)
            server.repl_no_slaves_since = server.unixtime;
        refreshGoodSlavesCount();
        /* Fire the replica change modules event. */
        /**触发副本更改模块事件。*/
        if (c->replstate == SLAVE_STATE_ONLINE)
            moduleFireServerEvent(REDISMODULE_EVENT_REPLICA_CHANGE,
                                  REDISMODULE_SUBEVENT_REPLICA_CHANGE_OFFLINE,
                                  NULL);
    }

    /* Master/slave cleanup Case 2:
     * we lost the connection with the master. */
    /**Masterslave cleanup 案例2：我们失去了与master的连接。*/
    if (c->flags & CLIENT_MASTER) replicationHandleMasterDisconnection();

    /* Remove the contribution that this client gave to our
     * incrementally computed memory usage. */
    /**删除此客户端对我们增量计算的内存使用量的贡献。*/
    server.stat_clients_type_memory[c->last_memory_type] -=
        c->last_memory_usage;
    /* Remove client from memory usage buckets */
    /**从内存使用桶中删除客户端*/
    if (c->mem_usage_bucket) {
        c->mem_usage_bucket->mem_usage_sum -= c->last_memory_usage;
        listDelNode(c->mem_usage_bucket->clients, c->mem_usage_bucket_node);
    }

    /* Release other dynamically allocated client structure fields,
     * and finally release the client structure itself. */
    /**释放其他动态分配的客户端结构字段，最后释放客户端结构本身。*/
    if (c->name) decrRefCount(c->name);
    freeClientMultiState(c);
    sdsfree(c->peerid);
    sdsfree(c->sockname);
    sdsfree(c->slave_addr);
    zfree(c);
}

/* Schedule a client to free it at a safe time in the serverCron() function.
 * This function is useful when we need to terminate a client but we are in
 * a context where calling freeClient() is not possible, because the client
 * should be valid for the continuation of the flow of the program. */
/**在 serverCron() 函数中安排客户端在安全时间释放它。
 *
 * @return 当我们需要终止客户端但我们处于无法调用 freeClient() 的上下文中时，此函数很有用，
 * 因为客户端应该对程序流程的继续有效。*/
void freeClientAsync(client *c) {
    /* We need to handle concurrent access to the server.clients_to_close list
     * only in the freeClientAsync() function, since it's the only function that
     * may access the list while Redis uses I/O threads. All the other accesses
     * are in the context of the main thread while the other threads are
     * idle. */
    /**我们只需要在 freeClientAsync() 函数中处理对 server.clients_to_close 列表的并发访问，
     * 因为当 Redis 使用 IO 线程时，它是唯一可以访问列表的函数。
     * 所有其他访问都在主线程的上下文中，而其他线程处于空闲状态。*/
    if (c->flags & CLIENT_CLOSE_ASAP || c->flags & CLIENT_SCRIPT) return;
    c->flags |= CLIENT_CLOSE_ASAP;
    if (server.io_threads_num == 1) {
        /* no need to bother with locking if there's just one thread (the main thread) */
        /**如果只有一个线程（主线程），则无需担心锁定*/
        listAddNodeTail(server.clients_to_close,c);
        return;
    }
    static pthread_mutex_t async_free_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&async_free_queue_mutex);
    listAddNodeTail(server.clients_to_close,c);
    pthread_mutex_unlock(&async_free_queue_mutex);
}

/* Log errors for invalid use and free the client in async way.
 * We will add additional information about the client to the message. */
/**记录无效使用错误并以异步方式释放客户端。我们将在消息中添加有关客户端的其他信息。*/
void logInvalidUseAndFreeClientAsync(client *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    sds info = sdscatvprintf(sdsempty(), fmt, ap);
    va_end(ap);

    sds client = catClientInfoString(sdsempty(), c);
    serverLog(LL_WARNING, "%s, disconnecting it: %s", info, client);

    sdsfree(info);
    sdsfree(client);
    freeClientAsync(c);
}

/* Perform processing of the client before moving on to processing the next client
 * this is useful for performing operations that affect the global state but can't
 * wait until we're done with all clients. In other words can't wait until beforeSleep()
 * return C_ERR in case client is no longer valid after call.
 * The input client argument: c, may be NULL in case the previous client was
 * freed before the call. */
/**在继续处理下一个客户端之前执行客户端的处理，这对于执行影响全局状态但不能等到我们完成所有客户端的操作很有用。
 * 换句话说，不能等到 beforeSleep() 返回 C_ERR，以防客户端在调用后不再有效。
 * 输入客户端参数：c，如果前一个客户端在调用之前被释放，则可能为 NULL。*/
int beforeNextClient(client *c) {
    /* Skip the client processing if we're in an IO thread, in that case we'll perform
       this operation later (this function is called again) in the fan-in stage of the threading mechanism */
    /**如果我们在 IO 线程中，则跳过客户端处理，在这种情况下，我们稍后会在线程机制的扇入阶段执行此操作（再次调用此函数）*/
    if (io_threads_op != IO_THREADS_OP_IDLE)
        return C_OK;
    /* Handle async frees */
    /* Note: this doesn't make the server.clients_to_close list redundant because of
     * cases where we want an async free of a client other than myself. For example
     * in ACL modifications we disconnect clients authenticated to non-existent
     * users (see ACL LOAD). */
    /**处理异步释放注意：这不会使 server.clients_to_close 列表变得多余，因为我们希望异步释放除我自己以外的客户端。
     * 例如，在 ACL 修改中，我们断开对不存在的用户进行身份验证的客户端（请参阅 ACL 加载）。*/
    if (c && (c->flags & CLIENT_CLOSE_ASAP)) {
        freeClient(c);
        return C_ERR;
    }
    return C_OK;
}

/* Free the clients marked as CLOSE_ASAP, return the number of clients
 * freed. */
/**释放标记为 CLOSE_ASAP 的客户端，返回释放的客户端数量。*/
int freeClientsInAsyncFreeQueue(void) {
    int freed = 0;
    listIter li;
    listNode *ln;

    listRewind(server.clients_to_close,&li);
    while ((ln = listNext(&li)) != NULL) {
        client *c = listNodeValue(ln);

        if (c->flags & CLIENT_PROTECTED) continue;

        c->flags &= ~CLIENT_CLOSE_ASAP;
        freeClient(c);
        listDelNode(server.clients_to_close,ln);
        freed++;
    }
    return freed;
}

/* Return a client by ID, or NULL if the client ID is not in the set
 * of registered clients. Note that "fake clients", created with -1 as FD,
 * are not registered clients. */
/**按 ID 返回客户端，如果客户端 ID 不在注册客户端集中，则返回 NULL。
 * 请注意，使用 -1 作为 FD 创建的“假客户端”不是注册客户端。*/
client *lookupClientByID(uint64_t id) {
    id = htonu64(id);
    client *c = raxFind(server.clients_index,(unsigned char*)&id,sizeof(id));
    return (c == raxNotFound) ? NULL : c;
}

/* This function should be called from _writeToClient when the reply list is not empty,
 * it gathers the scattered buffers from reply list and sends them away with connWritev.
 * If we write successfully, it returns C_OK, otherwise, C_ERR is returned,
 * and 'nwritten' is an output parameter, it means how many bytes server write
 * to client. */
/**当回复列表不为空时，应该从 _writeToClient 调用此函数，它从回复列表中收集分散的缓冲区并使用 connWritev 将它们发送出去。
 * 如果写入成功，则返回 C_OK，否则返回 C_ERR，'nwritten' 为输出参数，表示服务器写入客户端的字节数。*/
static int _writevToClient(client *c, ssize_t *nwritten) {
    struct iovec iov[IOV_MAX];
    int iovcnt = 0;
    size_t iov_bytes_len = 0;
    /* If the static reply buffer is not empty, 
     * add it to the iov array for writev() as well. */
    if (c->bufpos > 0) {
        iov[iovcnt].iov_base = c->buf + c->sentlen;
        iov[iovcnt].iov_len = c->bufpos - c->sentlen;
        iov_bytes_len += iov[iovcnt++].iov_len;
    }
    /* The first node of reply list might be incomplete from the last call,
     * thus it needs to be calibrated to get the actual data address and length. */
    size_t offset = c->bufpos > 0 ? 0 : c->sentlen;
    listIter iter;
    listNode *next;
    clientReplyBlock *o;
    listRewind(c->reply, &iter);
    while ((next = listNext(&iter)) && iovcnt < IOV_MAX && iov_bytes_len < NET_MAX_WRITES_PER_EVENT) {
        o = listNodeValue(next);
        if (o->used == 0) { /* empty node, just release it and skip. */
            c->reply_bytes -= o->size;
            listDelNode(c->reply, next);
            offset = 0;
            continue;
        }

        iov[iovcnt].iov_base = o->buf + offset;
        iov[iovcnt].iov_len = o->used - offset;
        iov_bytes_len += iov[iovcnt++].iov_len;
        offset = 0;
    }
    if (iovcnt == 0) return C_OK;
    *nwritten = connWritev(c->conn, iov, iovcnt);
    if (*nwritten <= 0) return C_ERR;

    /* Locate the new node which has leftover data and
     * release all nodes in front of it. */
    ssize_t remaining = *nwritten;
    if (c->bufpos > 0) { /* deal with static reply buffer first. */
        int buf_len = c->bufpos - c->sentlen;
        c->sentlen += remaining;
        /* If the buffer was sent, set bufpos to zero to continue with
         * the remainder of the reply. */
        if (remaining >= buf_len) {
            c->bufpos = 0;
            c->sentlen = 0;
        }
        remaining -= buf_len;
    }
    listRewind(c->reply, &iter);
    while (remaining > 0) {
        next = listNext(&iter);
        o = listNodeValue(next);
        if (remaining < (ssize_t)(o->used - c->sentlen)) {
            c->sentlen += remaining;
            break;
        }
        remaining -= (ssize_t)(o->used - c->sentlen);
        c->reply_bytes -= o->size;
        listDelNode(c->reply, next);
        c->sentlen = 0;
    }

    return C_OK;
}

/* This function does actual writing output buffers to different types of
 * clients, it is called by writeToClient.
 * If we write successfully, it returns C_OK, otherwise, C_ERR is returned,
 * and 'nwritten' is an output parameter, it means how many bytes server write
 * to client. */
/**该函数对不同类型的客户端实际写入输出缓冲区，由 writeToClient 调用。
 * 如果写入成功，则返回 C_OK，否则返回 C_ERR，'nwritten' 为输出参数，表示服务器写入客户端的字节数。*/
int _writeToClient(client *c, ssize_t *nwritten) {
    *nwritten = 0;
    if (getClientType(c) == CLIENT_TYPE_SLAVE) {
        serverAssert(c->bufpos == 0 && listLength(c->reply) == 0);

        replBufBlock *o = listNodeValue(c->ref_repl_buf_node);
        serverAssert(o->used >= c->ref_block_pos);
        /* Send current block if it is not fully sent. */
        if (o->used > c->ref_block_pos) {
            *nwritten = connWrite(c->conn, o->buf+c->ref_block_pos,
                                  o->used-c->ref_block_pos);
            if (*nwritten <= 0) return C_ERR;
            c->ref_block_pos += *nwritten;
        }

        /* If we fully sent the object on head, go to the next one. */
        listNode *next = listNextNode(c->ref_repl_buf_node);
        if (next && c->ref_block_pos == o->used) {
            o->refcount--;
            ((replBufBlock *)(listNodeValue(next)))->refcount++;
            c->ref_repl_buf_node = next;
            c->ref_block_pos = 0;
            incrementalTrimReplicationBacklog(REPL_BACKLOG_TRIM_BLOCKS_PER_CALL);
        }
        return C_OK;
    }

    /* When the reply list is not empty, it's better to use writev to save us some
     * system calls and TCP packets. */
    if (listLength(c->reply) > 0) {
        int ret = _writevToClient(c, nwritten);
        if (ret != C_OK) return ret;

        /* If there are no longer objects in the list, we expect
         * the count of reply bytes to be exactly zero. */
        if (listLength(c->reply) == 0)
            serverAssert(c->reply_bytes == 0);
    } else if (c->bufpos > 0) {
        *nwritten = connWrite(c->conn, c->buf + c->sentlen, c->bufpos - c->sentlen);
        if (*nwritten <= 0) return C_ERR;
        c->sentlen += *nwritten;

        /* If the buffer was sent, set bufpos to zero to continue with
         * the remainder of the reply. */
        if ((int)c->sentlen == c->bufpos) {
            c->bufpos = 0;
            c->sentlen = 0;
        }
    } 

    return C_OK;
}

/* Write data in output buffers to client. Return C_OK if the client
 * is still valid after the call, C_ERR if it was freed because of some
 * error.  If handler_installed is set, it will attempt to clear the
 * write event.
 *
 * This function is called by threads, but always with handler_installed
 * set to 0. So when handler_installed is set to 0 the function must be
 * thread safe. */
/**将输出缓冲区中的数据写入客户端。如果客户端在调用后仍然有效，则返回 C_OK，如果由于某些错误而被释放，则返回 C_ERR。
 * 如果设置了 handler_installed，它将尝试清除写入事件。
 *
 * 此函数由线程调用，但始终将 handler_installed 设置为 0。因此，当 handler_installed 设置为 0 时，该函数必须是线程安全的。*/
int writeToClient(client *c, int handler_installed) {
    /* Update total number of writes on server */
    atomicIncr(server.stat_total_writes_processed, 1);

    ssize_t nwritten = 0, totwritten = 0;

    while(clientHasPendingReplies(c)) {
        int ret = _writeToClient(c, &nwritten);
        if (ret == C_ERR) break;
        totwritten += nwritten;
        /* Note that we avoid to send more than NET_MAX_WRITES_PER_EVENT
         * bytes, in a single threaded server it's a good idea to serve
         * other clients as well, even if a very large request comes from
         * super fast link that is always able to accept data (in real world
         * scenario think about 'KEYS *' against the loopback interface).
         *
         * However if we are over the maxmemory limit we ignore that and
         * just deliver as much data as it is possible to deliver.
         *
         * Moreover, we also send as much as possible if the client is
         * a slave or a monitor (otherwise, on high-speed traffic, the
         * replication/output buffer will grow indefinitely) */
        /**请注意，我们避免发送超过 NET_MAX_WRITES_PER_EVENT 字节，在单线程服务器中，
         * 最好也为其他客户端提供服务，即使非常大的请求来自始终能够接受数据的超快速链接
         * （在现实世界场景中）考虑针对环回接口的'KEYS'）。
         *
         * 但是，如果我们超过了 maxmemory 限制，我们会忽略它并只提供尽可能多的数据。
         *
         * 而且，如果客户端是slave或者monitor，我们也尽量发送（否则在高速流量上，replicationoutput缓冲区会无限增长）*/
        if (totwritten > NET_MAX_WRITES_PER_EVENT &&
            (server.maxmemory == 0 ||
             zmalloc_used_memory() < server.maxmemory) &&
            !(c->flags & CLIENT_SLAVE)) break;
    }

    if (getClientType(c) == CLIENT_TYPE_SLAVE) {
        atomicIncr(server.stat_net_repl_output_bytes, totwritten);
    } else {
        atomicIncr(server.stat_net_output_bytes, totwritten);
    }

    if (nwritten == -1) {
        if (connGetState(c->conn) != CONN_STATE_CONNECTED) {
            serverLog(LL_VERBOSE,
                "Error writing to client: %s", connGetLastError(c->conn));
            freeClientAsync(c);
            return C_ERR;
        }
    }
    if (totwritten > 0) {
        /* For clients representing masters we don't count sending data
         * as an interaction, since we always send REPLCONF ACK commands
         * that take some time to just fill the socket output buffer.
         * We just rely on data / pings received for timeout detection. */
        /**对于代表 master 的客户端，我们不将发送数据视为交互，因为我们总是发送 REPLCONF ACK 命令，
         * 这需要一些时间来填充套接字输出缓冲区。我们只是依靠接收到的数据 ping 来检测超时。*/
        if (!(c->flags & CLIENT_MASTER)) c->lastinteraction = server.unixtime;
    }
    if (!clientHasPendingReplies(c)) {
        c->sentlen = 0;
        /* Note that writeToClient() is called in a threaded way, but
         * aeDeleteFileEvent() is not thread safe: however writeToClient()
         * is always called with handler_installed set to 0 from threads
         * so we are fine. */
        /**请注意，writeToClient() 是以线程方式调用的，但 aeDeleteFileEvent() 不是线程安全的：
         * 但是 writeToClient() 总是在 handler_installed 设置为 0 的情况下从线程调用，所以我们很好。*/
        if (handler_installed) {
            serverAssert(io_threads_op == IO_THREADS_OP_IDLE);
            connSetWriteHandler(c->conn, NULL);
        }

        /* Close connection after entire reply has been sent. */
        if (c->flags & CLIENT_CLOSE_AFTER_REPLY) {
            freeClientAsync(c);
            return C_ERR;
        }
    }
    /* Update client's memory usage after writing.
     * Since this isn't thread safe we do this conditionally. In case of threaded writes this is done in
     * handleClientsWithPendingWritesUsingThreads(). */
    /**写入后更新客户端的内存使用情况。由于这不是线程安全的，我们有条件地这样做。
     * 在线程写入的情况下，这是在 handleClientsWithPendingWritesUsingThreads() 中完成的。*/
    if (io_threads_op == IO_THREADS_OP_IDLE)
        updateClientMemUsage(c);
    return C_OK;
}

/* Write event handler. Just send data to the client. */
/**编写事件处理程序。只需向客户端发送数据。*/
void sendReplyToClient(connection *conn) {
    client *c = connGetPrivateData(conn);
    writeToClient(c,1);
}

/* This function is called just before entering the event loop, in the hope
 * we can just write the replies to the client output buffer without any
 * need to use a syscall in order to install the writable event handler,
 * get it called, and so forth. */
/**这个函数在进入事件循环之前被调用，希望我们可以将回复写入客户端输出缓冲区，
 * 而不需要使用系统调用来安装可写事件处理程序，调用它等等。*/
int handleClientsWithPendingWrites(void) {
    listIter li;
    listNode *ln;
    int processed = listLength(server.clients_pending_write);

    listRewind(server.clients_pending_write,&li);
    while((ln = listNext(&li))) {
        client *c = listNodeValue(ln);
        c->flags &= ~CLIENT_PENDING_WRITE;
        listDelNode(server.clients_pending_write,ln);

        /* If a client is protected, don't do anything,
         * that may trigger write error or recreate handler. */
        if (c->flags & CLIENT_PROTECTED) continue;

        /* Don't write to clients that are going to be closed anyway. */
        if (c->flags & CLIENT_CLOSE_ASAP) continue;

        /* Try to write buffers to the client socket. */
        if (writeToClient(c,0) == C_ERR) continue;

        /* If after the synchronous writes above we still have data to
         * output to the client, we need to install the writable handler. */
        /**如果在上面的同步写入之后我们仍然有数据要输出到客户端，我们需要安装可写处理程序。*/
        if (clientHasPendingReplies(c)) {
            installClientWriteHandler(c);
        }
    }
    return processed;
}

/* resetClient prepare the client to process the next command */
/**resetClient 准备客户端处理下一个命令*/
void resetClient(client *c) {
    redisCommandProc *prevcmd = c->cmd ? c->cmd->proc : NULL;

    freeClientArgv(c);
    c->reqtype = 0;
    c->multibulklen = 0;
    c->bulklen = -1;
    c->slot = -1;

    if (c->deferred_reply_errors)
        listRelease(c->deferred_reply_errors);
    c->deferred_reply_errors = NULL;

    /* We clear the ASKING flag as well if we are not inside a MULTI, and
     * if what we just executed is not the ASKING command itself. */
    if (!(c->flags & CLIENT_MULTI) && prevcmd != askingCommand)
        c->flags &= ~CLIENT_ASKING;

    /* We do the same for the CACHING command as well. It also affects
     * the next command or transaction executed, in a way very similar
     * to ASKING. */
    if (!(c->flags & CLIENT_MULTI) && prevcmd != clientCommand)
        c->flags &= ~CLIENT_TRACKING_CACHING;

    /* Remove the CLIENT_REPLY_SKIP flag if any so that the reply
     * to the next command will be sent, but set the flag if the command
     * we just processed was "CLIENT REPLY SKIP". */
    c->flags &= ~CLIENT_REPLY_SKIP;
    if (c->flags & CLIENT_REPLY_SKIP_NEXT) {
        c->flags |= CLIENT_REPLY_SKIP;
        c->flags &= ~CLIENT_REPLY_SKIP_NEXT;
    }
}

/* This function is used when we want to re-enter the event loop but there
 * is the risk that the client we are dealing with will be freed in some
 * way. This happens for instance in:
 *
 * * DEBUG RELOAD and similar.
 * * When a Lua script is in -BUSY state.
 *
 * So the function will protect the client by doing two things:
 *
 * 1) It removes the file events. This way it is not possible that an
 *    error is signaled on the socket, freeing the client.
 * 2) Moreover it makes sure that if the client is freed in a different code
 *    path, it is not really released, but only marked for later release. */
/**当我们想要重新进入事件循环但存在我们正在处理的客户端将以某种方式被释放的风险时使用此函数。
 * 例如，这发生在：DEBUG RELOAD 和类似情况下。当 Lua 脚本处于 -BUSY 状态时。
 * 所以该函数将通过做两件事来保护客户端：
 *   1）它删除文件事件。这样就不可能在套接字上发出错误信号，从而释放客户端。
 *   2）此外，它确保如果客户端在不同的代码路径中被释放，它并没有真正释放，而只是标记为以后发布。*/
void protectClient(client *c) {
    c->flags |= CLIENT_PROTECTED;
    if (c->conn) {
        connSetReadHandler(c->conn,NULL);
        connSetWriteHandler(c->conn,NULL);
    }
}

/* This will undo the client protection done by protectClient() */
/**这将撤消由protectClient() 完成的客户端保护*/
void unprotectClient(client *c) {
    if (c->flags & CLIENT_PROTECTED) {
        c->flags &= ~CLIENT_PROTECTED;
        if (c->conn) {
            connSetReadHandler(c->conn,readQueryFromClient);
            if (clientHasPendingReplies(c)) putClientInPendingWriteQueue(c);
        }
    }
}

/* Like processMultibulkBuffer(), but for the inline protocol instead of RESP,
 * this function consumes the client query buffer and creates a command ready
 * to be executed inside the client structure. Returns C_OK if the command
 * is ready to be executed, or C_ERR if there is still protocol to read to
 * have a well formed command. The function also returns C_ERR when there is
 * a protocol error: in such a case the client structure is setup to reply
 * with the error and close the connection. */
/**与 processMultibulkBuffer() 类似，但对于内联协议而不是 RESP，
 * 此函数使用客户端查询缓冲区并创建一个准备好在客户端结构内执行的命令。
 * 如果命令已准备好执行，则返回 C_OK，如果仍有要读取的协议以具有格式正确的命令，则返回 C_ERR。
 * 当出现协议错误时，该函数还返回 C_ERR：在这种情况下，客户端结构设置为回复错误并关闭连接。*/
int processInlineBuffer(client *c) {
    char *newline;
    int argc, j, linefeed_chars = 1;
    sds *argv, aux;
    size_t querylen;

    /* Search for end of line */
    newline = strchr(c->querybuf+c->qb_pos,'\n');

    /* Nothing to do without a \r\n */
    if (newline == NULL) {
        if (sdslen(c->querybuf)-c->qb_pos > PROTO_INLINE_MAX_SIZE) {
            addReplyError(c,"Protocol error: too big inline request");
            setProtocolError("too big inline request",c);
        }
        return C_ERR;
    }

    /* Handle the \r\n case. */
    if (newline != c->querybuf+c->qb_pos && *(newline-1) == '\r')
        newline--, linefeed_chars++;

    /* Split the input buffer up to the \r\n */
    querylen = newline-(c->querybuf+c->qb_pos);
    aux = sdsnewlen(c->querybuf+c->qb_pos,querylen);
    argv = sdssplitargs(aux,&argc);
    sdsfree(aux);
    if (argv == NULL) {
        addReplyError(c,"Protocol error: unbalanced quotes in request");
        setProtocolError("unbalanced quotes in inline request",c);
        return C_ERR;
    }

    /* Newline from slaves can be used to refresh the last ACK time.
     * This is useful for a slave to ping back while loading a big
     * RDB file. */
    if (querylen == 0 && getClientType(c) == CLIENT_TYPE_SLAVE)
        c->repl_ack_time = server.unixtime;

    /* Masters should never send us inline protocol to run actual
     * commands. If this happens, it is likely due to a bug in Redis where
     * we got some desynchronization in the protocol, for example
     * because of a PSYNC gone bad.
     *
     * However there is an exception: masters may send us just a newline
     * to keep the connection active. */
    /**大师永远不应该向我们发送内联协议来运行实际命令。
     * 如果发生这种情况，很可能是由于 Redis 中的一个错误导致我们在协议中出现了一些不同步，例如因为 PSYNC 坏了。
     * 但是有一个例外：masters 可能只向我们发送一个换行符来保持连接处于活动状态。*/
    if (querylen != 0 && c->flags & CLIENT_MASTER) {
        sdsfreesplitres(argv,argc);
        serverLog(LL_WARNING,"WARNING: Receiving inline protocol from master, master stream corruption? Closing the master connection and discarding the cached master.");
        setProtocolError("Master using the inline protocol. Desync?",c);
        return C_ERR;
    }

    /* Move querybuffer position to the next query in the buffer. */
    c->qb_pos += querylen+linefeed_chars;

    /* Setup argv array on client structure */
    if (argc) {
        if (c->argv) zfree(c->argv);
        c->argv_len = argc;
        c->argv = zmalloc(sizeof(robj*)*c->argv_len);
        c->argv_len_sum = 0;
    }

    /* Create redis objects for all arguments. */
    for (c->argc = 0, j = 0; j < argc; j++) {
        c->argv[c->argc] = createObject(OBJ_STRING,argv[j]);
        c->argc++;
        c->argv_len_sum += sdslen(argv[j]);
    }
    zfree(argv);
    return C_OK;
}

/* Helper function. Record protocol error details in server log,
 * and set the client as CLIENT_CLOSE_AFTER_REPLY and
 * CLIENT_PROTOCOL_ERROR. */
/**辅助功能。在服务器日志中记录协议错误详细信息，并将客户端设置为 CLIENT_CLOSE_AFTER_REPLY 和 CLIENT_PROTOCOL_ERROR。*/
#define PROTO_DUMP_LEN 128
static void setProtocolError(const char *errstr, client *c) {
    if (server.verbosity <= LL_VERBOSE || c->flags & CLIENT_MASTER) {
        sds client = catClientInfoString(sdsempty(),c);

        /* Sample some protocol to given an idea about what was inside. */
        char buf[256];
        if (sdslen(c->querybuf)-c->qb_pos < PROTO_DUMP_LEN) {
            snprintf(buf,sizeof(buf),"Query buffer during protocol error: '%s'", c->querybuf+c->qb_pos);
        } else {
            snprintf(buf,sizeof(buf),"Query buffer during protocol error: '%.*s' (... more %zu bytes ...) '%.*s'", PROTO_DUMP_LEN/2, c->querybuf+c->qb_pos, sdslen(c->querybuf)-c->qb_pos-PROTO_DUMP_LEN, PROTO_DUMP_LEN/2, c->querybuf+sdslen(c->querybuf)-PROTO_DUMP_LEN/2);
        }

        /* Remove non printable chars. */
        char *p = buf;
        while (*p != '\0') {
            if (!isprint(*p)) *p = '.';
            p++;
        }

        /* Log all the client and protocol info. */
        int loglevel = (c->flags & CLIENT_MASTER) ? LL_WARNING :
                                                    LL_VERBOSE;
        serverLog(loglevel,
            "Protocol error (%s) from client: %s. %s", errstr, client, buf);
        sdsfree(client);
    }
    c->flags |= (CLIENT_CLOSE_AFTER_REPLY|CLIENT_PROTOCOL_ERROR);
}

/* Process the query buffer for client 'c', setting up the client argument
 * vector for command execution. Returns C_OK if after running the function
 * the client has a well-formed ready to be processed command, otherwise
 * C_ERR if there is still to read more buffer to get the full command.
 * The function also returns C_ERR when there is a protocol error: in such a
 * case the client structure is setup to reply with the error and close
 * the connection.
 *
 * This function is called if processInputBuffer() detects that the next
 * command is in RESP format, so the first byte in the command is found
 * to be '*'. Otherwise for inline commands processInlineBuffer() is called. */
/**处理客户端“c”的查询缓冲区，设置客户端参数向量以执行命令。
 * 如果在运行函数后客户端有一个格式良好的准备好被处理的命令，则返回 C_OK，
 * 否则如果仍有读取更多缓冲区以获取完整命令，则返回 C_ERR。
 * 当出现协议错误时，该函数还返回 C_ERR：在这种情况下，客户端结构设置为回复错误并关闭连接。
 * 如果 processInputBuffer() 检测到下一个命令是 RESP 格式，则调用此函数，因此发现命令中的第一个字节是 ''。
 * 否则，对于内联命令 processInlineBuffer() 被调用。*/
int processMultibulkBuffer(client *c) {
    char *newline = NULL;
    int ok;
    long long ll;

    if (c->multibulklen == 0) {
        /* The client should have been reset */
        serverAssertWithInfo(c,NULL,c->argc == 0);

        /* Multi bulk length cannot be read without a \r\n */
        newline = strchr(c->querybuf+c->qb_pos,'\r');
        if (newline == NULL) {
            if (sdslen(c->querybuf)-c->qb_pos > PROTO_INLINE_MAX_SIZE) {
                addReplyError(c,"Protocol error: too big mbulk count string");
                setProtocolError("too big mbulk count string",c);
            }
            return C_ERR;
        }

        /* Buffer should also contain \n */
        if (newline-(c->querybuf+c->qb_pos) > (ssize_t)(sdslen(c->querybuf)-c->qb_pos-2))
            return C_ERR;

        /* We know for sure there is a whole line since newline != NULL,
         * so go ahead and find out the multi bulk length. */
        serverAssertWithInfo(c,NULL,c->querybuf[c->qb_pos] == '*');
        ok = string2ll(c->querybuf+1+c->qb_pos,newline-(c->querybuf+1+c->qb_pos),&ll);
        if (!ok || ll > INT_MAX) {
            addReplyError(c,"Protocol error: invalid multibulk length");
            setProtocolError("invalid mbulk count",c);
            return C_ERR;
        } else if (ll > 10 && authRequired(c)) {
            addReplyError(c, "Protocol error: unauthenticated multibulk length");
            setProtocolError("unauth mbulk count", c);
            return C_ERR;
        }

        c->qb_pos = (newline-c->querybuf)+2;

        if (ll <= 0) return C_OK;

        c->multibulklen = ll;

        /* Setup argv array on client structure */
        if (c->argv) zfree(c->argv);
        c->argv_len = min(c->multibulklen, 1024);
        c->argv = zmalloc(sizeof(robj*)*c->argv_len);
        c->argv_len_sum = 0;
    }

    serverAssertWithInfo(c,NULL,c->multibulklen > 0);
    while(c->multibulklen) {
        /* Read bulk length if unknown */
        if (c->bulklen == -1) {
            newline = strchr(c->querybuf+c->qb_pos,'\r');
            if (newline == NULL) {
                if (sdslen(c->querybuf)-c->qb_pos > PROTO_INLINE_MAX_SIZE) {
                    addReplyError(c,
                        "Protocol error: too big bulk count string");
                    setProtocolError("too big bulk count string",c);
                    return C_ERR;
                }
                break;
            }

            /* Buffer should also contain \n */
            if (newline-(c->querybuf+c->qb_pos) > (ssize_t)(sdslen(c->querybuf)-c->qb_pos-2))
                break;

            if (c->querybuf[c->qb_pos] != '$') {
                addReplyErrorFormat(c,
                    "Protocol error: expected '$', got '%c'",
                    c->querybuf[c->qb_pos]);
                setProtocolError("expected $ but got something else",c);
                return C_ERR;
            }

            ok = string2ll(c->querybuf+c->qb_pos+1,newline-(c->querybuf+c->qb_pos+1),&ll);
            if (!ok || ll < 0 ||
                (!(c->flags & CLIENT_MASTER) && ll > server.proto_max_bulk_len)) {
                addReplyError(c,"Protocol error: invalid bulk length");
                setProtocolError("invalid bulk length",c);
                return C_ERR;
            } else if (ll > 16384 && authRequired(c)) {
                addReplyError(c, "Protocol error: unauthenticated bulk length");
                setProtocolError("unauth bulk length", c);
                return C_ERR;
            }

            c->qb_pos = newline-c->querybuf+2;
            if (!(c->flags & CLIENT_MASTER) && ll >= PROTO_MBULK_BIG_ARG) {
                /* When the client is not a master client (because master
                 * client's querybuf can only be trimmed after data applied
                 * and sent to replicas).
                 *
                 * If we are going to read a large object from network
                 * try to make it likely that it will start at c->querybuf
                 * boundary so that we can optimize object creation
                 * avoiding a large copy of data.
                 *
                 * But only when the data we have not parsed is less than
                 * or equal to ll+2. If the data length is greater than
                 * ll+2, trimming querybuf is just a waste of time, because
                 * at this time the querybuf contains not only our bulk. */
                /**当客户端不是主客户端时（因为主客户端的querybuf只能在数据应用并发送到副本后进行修剪）。
                 * 如果我们要从网络中读取一个大对象，尽量让它从 c->querybuf 边界开始，这样我们就可以优化对象创建，
                 * 避免大量数据副本。但只有当我们没有解析的数据小于等于ll+2时。
                 * 如果数据长度大于ll+2，剪裁querybuf只是浪费时间，因为此时querybuf包含的不仅仅是我们的bulk。*/
                if (sdslen(c->querybuf)-c->qb_pos <= (size_t)ll+2) {
                    sdsrange(c->querybuf,c->qb_pos,-1);
                    c->qb_pos = 0;
                    /* Hint the sds library about the amount of bytes this string is
                     * going to contain. */
                    /**提示 sds 库有关此字符串将包含的字节数。*/
                    c->querybuf = sdsMakeRoomForNonGreedy(c->querybuf,ll+2-sdslen(c->querybuf));
                }
            }
            c->bulklen = ll;
        }

        /* Read bulk argument */
        if (sdslen(c->querybuf)-c->qb_pos < (size_t)(c->bulklen+2)) {
            /* Not enough data (+2 == trailing \r\n) */
            break;
        } else {
            /* Check if we have space in argv, grow if needed */
            if (c->argc >= c->argv_len) {
                c->argv_len = min(c->argv_len < INT_MAX/2 ? c->argv_len*2 : INT_MAX, c->argc+c->multibulklen);
                c->argv = zrealloc(c->argv, sizeof(robj*)*c->argv_len);
            }

            /* Optimization: if a non-master client's buffer contains JUST our bulk element
             * instead of creating a new object by *copying* the sds we
             * just use the current sds string. */
            if (!(c->flags & CLIENT_MASTER) &&
                c->qb_pos == 0 &&
                c->bulklen >= PROTO_MBULK_BIG_ARG &&
                sdslen(c->querybuf) == (size_t)(c->bulklen+2))
            {
                c->argv[c->argc++] = createObject(OBJ_STRING,c->querybuf);
                c->argv_len_sum += c->bulklen;
                sdsIncrLen(c->querybuf,-2); /* remove CRLF */
                /* Assume that if we saw a fat argument we'll see another one
                 * likely... */
                c->querybuf = sdsnewlen(SDS_NOINIT,c->bulklen+2);
                sdsclear(c->querybuf);
            } else {
                c->argv[c->argc++] =
                    createStringObject(c->querybuf+c->qb_pos,c->bulklen);
                c->argv_len_sum += c->bulklen;
                c->qb_pos += c->bulklen+2;
            }
            c->bulklen = -1;
            c->multibulklen--;
        }
    }

    /* We're done when c->multibulk == 0 */
    if (c->multibulklen == 0) return C_OK;

    /* Still not ready to process the command */
    return C_ERR;
}

/* Perform necessary tasks after a command was executed:
 *
 * 1. The client is reset unless there are reasons to avoid doing it.
 * 2. In the case of master clients, the replication offset is updated.
 * 3. Propagate commands we got from our master to replicas down the line. */
/**执行命令后执行必要的任务：
 *   1. 除非有理由避免这样做，否则会重置客户端。
 *   2. 在主客户端的情况下，更新复制偏移量。
 *   3. 将我们从 master 获得的命令传播到下线的副本。*/
void commandProcessed(client *c) {
    /* If client is blocked(including paused), just return avoid reset and replicate.
     *
     * 1. Don't reset the client structure for blocked clients, so that the reply
     *    callback will still be able to access the client argv and argc fields.
     *    The client will be reset in unblockClient().
     * 2. Don't update replication offset or propagate commands to replicas,
     *    since we have not applied the command. */
    /**如果客户端被阻塞（包括暂停），只需返回避免重置和复制。
     *   1. 不要为被阻塞的客户端重置客户端结构，这样回复回调仍然可以访问客户端的 argv 和 argc 字段。
     *      客户端将在 unblockClient() 中重置。
     *   2. 不要更新复制偏移或将命令传播到副本，因为我们还没有应用命令。*/
    if (c->flags & CLIENT_BLOCKED) return;

    resetClient(c);

    long long prev_offset = c->reploff;
    if (c->flags & CLIENT_MASTER && !(c->flags & CLIENT_MULTI)) {
        /* Update the applied replication offset of our master. */
        c->reploff = c->read_reploff - sdslen(c->querybuf) + c->qb_pos;
    }

    /* If the client is a master we need to compute the difference
     * between the applied offset before and after processing the buffer,
     * to understand how much of the replication stream was actually
     * applied to the master state: this quantity, and its corresponding
     * part of the replication stream, will be propagated to the
     * sub-replicas and to the replication backlog. */
    /**如果客户端是主控，我们需要计算处理缓冲区前后应用偏移量的差值，以了解有多少复制流实际应用到主控状态：
     * 此数量及其对应的复制流部分，将传播到子副本和复制积压。*/
    if (c->flags & CLIENT_MASTER) {
        long long applied = c->reploff - prev_offset;
        if (applied) {
            replicationFeedStreamFromMasterStream(c->querybuf+c->repl_applied,applied);
            c->repl_applied += applied;
        }
    }
}

/* This function calls processCommand(), but also performs a few sub tasks
 * for the client that are useful in that context:
 *
 * 1. It sets the current client to the client 'c'.
 * 2. calls commandProcessed() if the command was handled.
 *
 * The function returns C_ERR in case the client was freed as a side effect
 * of processing the command, otherwise C_OK is returned. */
/**此函数调用 processCommand()，但还为客户端执行一些在该上下文中有用的子任务：
 *   1. 它将当前客户端设置为客户端“c”。
 *   2. 如果命令已处理，则调用 commandProcessed()。
 * 如果客户端被释放作为处理命令的副作用，该函数返回 C_ERR，否则返回 C_OK。*/
int processCommandAndResetClient(client *c) {
    int deadclient = 0;
    client *old_client = server.current_client;
    server.current_client = c;
    if (processCommand(c) == C_OK) {
        commandProcessed(c);
        /* Update the client's memory to include output buffer growth following the
         * processed command. */
        updateClientMemUsage(c);
    }

    if (server.current_client == NULL) deadclient = 1;
    /*
     * Restore the old client, this is needed because when a script
     * times out, we will get into this code from processEventsWhileBlocked.
     * Which will cause to set the server.current_client. If not restored
     * we will return 1 to our caller which will falsely indicate the client
     * is dead and will stop reading from its buffer.
     */
    /**恢复旧客户端，这是需要的，因为当脚本超时时，我们将从 processEventsWhileBlocked 进入这段代码。
     * 这将导致设置 server.current_client。如果没有恢复，我们将向调用者返回 1，
     * 这将错误地指示客户端已死并停止从其缓冲区读取。*/
    server.current_client = old_client;
    /* performEvictions may flush slave output buffers. This may
     * result in a slave, that may be the active client, to be
     * freed. */
    /**performEvictions 可能会刷新从属输出缓冲区。这可能会导致从机（可能是活动客户端）被释放。*/
    return deadclient ? C_ERR : C_OK;
}


/* This function will execute any fully parsed commands pending on
 * the client. Returns C_ERR if the client is no longer valid after executing
 * the command, and C_OK for all other cases. */
/**此函数将执行客户端上未决的任何完全解析的命令。
 * 如果客户端在执行命令后不再有效，则返回 C_ERR，对于所有其他情况，返回 C_OK。*/
int processPendingCommandAndInputBuffer(client *c) {
    if (c->flags & CLIENT_PENDING_COMMAND) {
        c->flags &= ~CLIENT_PENDING_COMMAND;
        if (processCommandAndResetClient(c) == C_ERR) {
            return C_ERR;
        }
    }

    /* Now process client if it has more data in it's buffer.
     *
     * Note: when a master client steps into this function,
     * it can always satisfy this condition, because its querbuf
     * contains data not applied. */
    if (c->querybuf && sdslen(c->querybuf) > 0) {
        return processInputBuffer(c);
    }
    return C_OK;
}

/* This function is called every time, in the client structure 'c', there is
 * more query buffer to process, because we read more data from the socket
 * or because a client was blocked and later reactivated, so there could be
 * pending query buffer, already representing a full command, to process.
 * return C_ERR in case the client was freed during the processing */
/**每次调用这个函数，在客户端结构'c'中，有更多的查询缓冲区要处理，因为我们从套接字读取了更多的数据，
 * 或者因为客户端被阻塞并稍后重新激活，所以可能已经有待处理的查询缓冲区代表一个完整的命令，要处理。
 * 如果客户端在处理过程中被释放，则返回 C_ERR*/
int processInputBuffer(client *c) {
    /* Keep processing while there is something in the input buffer */
    while(c->qb_pos < sdslen(c->querybuf)) {
        /* Immediately abort if the client is in the middle of something. */
        if (c->flags & CLIENT_BLOCKED) break;

        /* Don't process more buffers from clients that have already pending
         * commands to execute in c->argv. */
        if (c->flags & CLIENT_PENDING_COMMAND) break;

        /* Don't process input from the master while there is a busy script
         * condition on the slave. We want just to accumulate the replication
         * stream (instead of replying -BUSY like we do with other clients) and
         * later resume the processing. */
        if (isInsideYieldingLongCommand() && c->flags & CLIENT_MASTER) break;

        /* CLIENT_CLOSE_AFTER_REPLY closes the connection once the reply is
         * written to the client. Make sure to not let the reply grow after
         * this flag has been set (i.e. don't process more commands).
         *
         * The same applies for clients we want to terminate ASAP. */
        if (c->flags & (CLIENT_CLOSE_AFTER_REPLY|CLIENT_CLOSE_ASAP)) break;

        /* Determine request type when unknown. */
        if (!c->reqtype) {
            if (c->querybuf[c->qb_pos] == '*') {
                c->reqtype = PROTO_REQ_MULTIBULK;
            } else {
                c->reqtype = PROTO_REQ_INLINE;
            }
        }

        if (c->reqtype == PROTO_REQ_INLINE) {
            if (processInlineBuffer(c) != C_OK) break;
        } else if (c->reqtype == PROTO_REQ_MULTIBULK) {
            if (processMultibulkBuffer(c) != C_OK) break;
        } else {
            serverPanic("Unknown request type");
        }

        /* Multibulk processing could see a <= 0 length. */
        if (c->argc == 0) {
            resetClient(c);
        } else {
            /* If we are in the context of an I/O thread, we can't really
             * execute the command here. All we can do is to flag the client
             * as one that needs to process the command. */
            if (io_threads_op != IO_THREADS_OP_IDLE) {
                serverAssert(io_threads_op == IO_THREADS_OP_READ);
                c->flags |= CLIENT_PENDING_COMMAND;
                break;
            }

            /* We are finally ready to execute the command. */
            if (processCommandAndResetClient(c) == C_ERR) {
                /* If the client is no longer valid, we avoid exiting this
                 * loop and trimming the client buffer later. So we return
                 * ASAP in that case. */
                return C_ERR;
            }
        }
    }

    if (c->flags & CLIENT_MASTER) {
        /* If the client is a master, trim the querybuf to repl_applied,
         * since master client is very special, its querybuf not only
         * used to parse command, but also proxy to sub-replicas.
         *
         * Here are some scenarios we cannot trim to qb_pos:
         * 1. we don't receive complete command from master
         * 2. master client blocked cause of client pause
         * 3. io threads operate read, master client flagged with CLIENT_PENDING_COMMAND
         *
         * In these scenarios, qb_pos points to the part of the current command
         * or the beginning of next command, and the current command is not applied yet,
         * so the repl_applied is not equal to qb_pos. */
        /**如果客户端是master，则将querybuf修剪为repl_applied，因为master客户端非常特殊，
         * 它的querybuf不仅用来解析命令，还可以代理到子副本。
         * 以下是一些我们无法修剪到 qb_pos 的场景：
         *   1. 我们没有收到来自 master 的完整命令
         *   2. master 客户端阻塞导致客户端暂停
         *   3. io 线程操作读取，master 客户端标记为 CLIENT_PENDING_COMMAND
         * 在这些场景中，qb_pos 指向当前命令的一部分或下一条命令的开头，并且当前命令还没有应用，所以repl_applied不等于qb_pos。*/
        if (c->repl_applied) {
            sdsrange(c->querybuf,c->repl_applied,-1);
            c->qb_pos -= c->repl_applied;
            c->repl_applied = 0;
        }
    } else if (c->qb_pos) {
        /* Trim to pos */
        sdsrange(c->querybuf,c->qb_pos,-1);
        c->qb_pos = 0;
    }

    /* Update client memory usage after processing the query buffer, this is
     * important in case the query buffer is big and wasn't drained during
     * the above loop (because of partially sent big commands). */
    /**在处理查询缓冲区后更新客户端内存使用情况，这在查询缓冲区很大并且在上述循环期间没有耗尽
     * （因为部分发送大命令）的情况下很重要。*/
    if (io_threads_op == IO_THREADS_OP_IDLE)
        updateClientMemUsage(c);

    return C_OK;
}

void readQueryFromClient(connection *conn) {
    client *c = connGetPrivateData(conn);
    int nread, big_arg = 0;
    size_t qblen, readlen;

    /* Check if we want to read from the client later when exiting from
     * the event loop. This is the case if threaded I/O is enabled. */
    if (postponeClientRead(c)) return;

    /* Update total number of reads on server */
    atomicIncr(server.stat_total_reads_processed, 1);

    readlen = PROTO_IOBUF_LEN;
    /* If this is a multi bulk request, and we are processing a bulk reply
     * that is large enough, try to maximize the probability that the query
     * buffer contains exactly the SDS string representing the object, even
     * at the risk of requiring more read(2) calls. This way the function
     * processMultiBulkBuffer() can avoid copying buffers to create the
     * Redis Object representing the argument. */
    /**如果这是一个多批量请求，并且我们正在处理一个足够大的批量回复，
     * 请尝试最大化查询缓冲区恰好包含表示对象的 SDS 字符串的概率，即使可能需要更多的 read(2) 调用.
     * 这样，函数 processMultiBulkBuffer() 可以避免复制缓冲区来创建表示参数的 Redis 对象。*/
    if (c->reqtype == PROTO_REQ_MULTIBULK && c->multibulklen && c->bulklen != -1
        && c->bulklen >= PROTO_MBULK_BIG_ARG)
    {
        ssize_t remaining = (size_t)(c->bulklen+2)-(sdslen(c->querybuf)-c->qb_pos);
        big_arg = 1;

        /* Note that the 'remaining' variable may be zero in some edge case,
         * for example once we resume a blocked client after CLIENT PAUSE. */
        if (remaining > 0) readlen = remaining;

        /* Master client needs expand the readlen when meet BIG_ARG(see #9100),
         * but doesn't need align to the next arg, we can read more data. */
        if (c->flags & CLIENT_MASTER && readlen < PROTO_IOBUF_LEN)
            readlen = PROTO_IOBUF_LEN;
    }

    qblen = sdslen(c->querybuf);
    if (!(c->flags & CLIENT_MASTER) && // master client's querybuf can grow greedy.
        (big_arg || sdsalloc(c->querybuf) < PROTO_IOBUF_LEN)) {
        /* When reading a BIG_ARG we won't be reading more than that one arg
         * into the query buffer, so we don't need to pre-allocate more than we
         * need, so using the non-greedy growing. For an initial allocation of
         * the query buffer, we also don't wanna use the greedy growth, in order
         * to avoid collision with the RESIZE_THRESHOLD mechanism. */
        /**在读取 BIG_ARG 时，我们不会将超过一个 arg 读取到查询缓冲区中，
         * 因此我们不需要预先分配超过我们需要的数量，因此使用非贪婪增长。
         * 对于查询缓冲区的初始分配，我们也不想使用贪婪增长，以避免与 RESIZE_THRESHOLD 机制发生冲突。*/
        c->querybuf = sdsMakeRoomForNonGreedy(c->querybuf, readlen);
    } else {
        c->querybuf = sdsMakeRoomFor(c->querybuf, readlen);

        /* Read as much as possible from the socket to save read(2) system calls. */
        readlen = sdsavail(c->querybuf);
    }
    nread = connRead(c->conn, c->querybuf+qblen, readlen);
    if (nread == -1) {
        if (connGetState(conn) == CONN_STATE_CONNECTED) {
            return;
        } else {
            serverLog(LL_VERBOSE, "Reading from client: %s",connGetLastError(c->conn));
            freeClientAsync(c);
            goto done;
        }
    } else if (nread == 0) {
        if (server.verbosity <= LL_VERBOSE) {
            sds info = catClientInfoString(sdsempty(), c);
            serverLog(LL_VERBOSE, "Client closed connection %s", info);
            sdsfree(info);
        }
        freeClientAsync(c);
        goto done;
    }

    sdsIncrLen(c->querybuf,nread);
    qblen = sdslen(c->querybuf);
    if (c->querybuf_peak < qblen) c->querybuf_peak = qblen;

    c->lastinteraction = server.unixtime;
    if (c->flags & CLIENT_MASTER) {
        c->read_reploff += nread;
        atomicIncr(server.stat_net_repl_input_bytes, nread);
    } else {
        atomicIncr(server.stat_net_input_bytes, nread);
    }

    if (!(c->flags & CLIENT_MASTER) && sdslen(c->querybuf) > server.client_max_querybuf_len) {
        sds ci = catClientInfoString(sdsempty(),c), bytes = sdsempty();

        bytes = sdscatrepr(bytes,c->querybuf,64);
        serverLog(LL_WARNING,"Closing client that reached max query buffer length: %s (qbuf initial bytes: %s)", ci, bytes);
        sdsfree(ci);
        sdsfree(bytes);
        freeClientAsync(c);
        goto done;
    }

    /* There is more data in the client input buffer, continue parsing it
     * and check if there is a full command to execute. */
    if (processInputBuffer(c) == C_ERR)
         c = NULL;

done:
    beforeNextClient(c);
}

/* A Redis "Address String" is a colon separated ip:port pair.
 * For IPv4 it's in the form x.y.z.k:port, example: "127.0.0.1:1234".
 * For IPv6 addresses we use [] around the IP part, like in "[::1]:1234".
 * For Unix sockets we use path:0, like in "/tmp/redis:0".
 *
 * An Address String always fits inside a buffer of NET_ADDR_STR_LEN bytes,
 * including the null term.
 *
 * On failure the function still populates 'addr' with the "?:0" string in case
 * you want to relax error checking or need to display something anyway (see
 * anetFdToString implementation for more info). */
/**Redis“地址字符串”是一个冒号分隔的 ip:port 对。对于 IPv4，其格式为 x.y.z.k:port，例如：“127.0.0.1:1234”。
 * 对于 IPv6 地址，我们在 IP 部分周围使用 []，如“[::1]:1234”。
 * 对于 Unix 套接字，我们使用 path:0，就像在 "tmpredis:0" 中一样。
 * 地址字符串始终适合 NET_ADDR_STR_LEN 字节的缓冲区，包括空项。
 * 失败时，该函数仍会使用“?:0”字符串填充“addr”，以防您想放松错误检查或无论如何都需要显示某些内容
 * （有关更多信息，请参见 anetFdToString 实现）。*/
void genClientAddrString(client *client, char *addr,
                         size_t addr_len, int fd_to_str_type) {
    if (client->flags & CLIENT_UNIX_SOCKET) {
        /* Unix socket client. */
        snprintf(addr,addr_len,"%s:0",server.unixsocket);
    } else {
        /* TCP client. */
        connFormatFdAddr(client->conn,addr,addr_len,fd_to_str_type);
    }
}

/* This function returns the client peer id, by creating and caching it
 * if client->peerid is NULL, otherwise returning the cached value.
 * The Peer ID never changes during the life of the client, however it
 * is expensive to compute. */
/**如果client->peerid 为NULL，则此函数通过创建和缓存它来返回客户端对等ID，否则返回缓存的值。
 * Peer ID 在客户端的生命周期内永远不会改变，但是计算成本很高。*/
char *getClientPeerId(client *c) {
    char peerid[NET_ADDR_STR_LEN];

    if (c->peerid == NULL) {
        genClientAddrString(c,peerid,sizeof(peerid),FD_TO_PEER_NAME);
        c->peerid = sdsnew(peerid);
    }
    return c->peerid;
}

/* This function returns the client bound socket name, by creating and caching
 * it if client->sockname is NULL, otherwise returning the cached value.
 * The Socket Name never changes during the life of the client, however it
 * is expensive to compute. */
/**此函数返回客户端绑定的套接字名称，如果 client->sockname 为 NULL，则通过创建和缓存它，否则返回缓存的值。
 * 套接字名称在客户端的生命周期内永远不会改变，但是计算成本很高。*/
char *getClientSockname(client *c) {
    char sockname[NET_ADDR_STR_LEN];

    if (c->sockname == NULL) {
        genClientAddrString(c,sockname,sizeof(sockname),FD_TO_SOCK_NAME);
        c->sockname = sdsnew(sockname);
    }
    return c->sockname;
}

/* Concatenate a string representing the state of a client in a human
 * readable format, into the sds string 's'. */
/**将一个以人类可读格式表示客户端状态的字符串连接到 sds 字符串“s”中。*/
sds catClientInfoString(sds s, client *client) {
    char flags[16], events[3], conninfo[CONN_INFO_LEN], *p;

    p = flags;
    if (client->flags & CLIENT_SLAVE) {
        if (client->flags & CLIENT_MONITOR)
            *p++ = 'O';
        else
            *p++ = 'S';
    }
    if (client->flags & CLIENT_MASTER) *p++ = 'M';
    if (client->flags & CLIENT_PUBSUB) *p++ = 'P';
    if (client->flags & CLIENT_MULTI) *p++ = 'x';
    if (client->flags & CLIENT_BLOCKED) *p++ = 'b';
    if (client->flags & CLIENT_TRACKING) *p++ = 't';
    if (client->flags & CLIENT_TRACKING_BROKEN_REDIR) *p++ = 'R';
    if (client->flags & CLIENT_TRACKING_BCAST) *p++ = 'B';
    if (client->flags & CLIENT_DIRTY_CAS) *p++ = 'd';
    if (client->flags & CLIENT_CLOSE_AFTER_REPLY) *p++ = 'c';
    if (client->flags & CLIENT_UNBLOCKED) *p++ = 'u';
    if (client->flags & CLIENT_CLOSE_ASAP) *p++ = 'A';
    if (client->flags & CLIENT_UNIX_SOCKET) *p++ = 'U';
    if (client->flags & CLIENT_READONLY) *p++ = 'r';
    if (client->flags & CLIENT_NO_EVICT) *p++ = 'e';
    if (p == flags) *p++ = 'N';
    *p++ = '\0';

    p = events;
    if (client->conn) {
        if (connHasReadHandler(client->conn)) *p++ = 'r';
        if (connHasWriteHandler(client->conn)) *p++ = 'w';
    }
    *p = '\0';

    /* Compute the total memory consumed by this client. */
    size_t obufmem, total_mem = getClientMemoryUsage(client, &obufmem);

    size_t used_blocks_of_repl_buf = 0;
    if (client->ref_repl_buf_node) {
        replBufBlock *last = listNodeValue(listLast(server.repl_buffer_blocks));
        replBufBlock *cur = listNodeValue(client->ref_repl_buf_node);
        used_blocks_of_repl_buf = last->id - cur->id + 1;
    }

    sds ret = sdscatfmt(s,
        "id=%U addr=%s laddr=%s %s name=%s age=%I idle=%I flags=%s db=%i sub=%i psub=%i ssub=%i multi=%i qbuf=%U qbuf-free=%U argv-mem=%U multi-mem=%U rbs=%U rbp=%U obl=%U oll=%U omem=%U tot-mem=%U events=%s cmd=%s user=%s redir=%I resp=%i",
        (unsigned long long) client->id,
        getClientPeerId(client),
        getClientSockname(client),
        connGetInfo(client->conn, conninfo, sizeof(conninfo)),
        client->name ? (char*)client->name->ptr : "",
        (long long)(server.unixtime - client->ctime),
        (long long)(server.unixtime - client->lastinteraction),
        flags,
        client->db->id,
        (int) dictSize(client->pubsub_channels),
        (int) listLength(client->pubsub_patterns),
        (int) dictSize(client->pubsubshard_channels),
        (client->flags & CLIENT_MULTI) ? client->mstate.count : -1,
        (unsigned long long) sdslen(client->querybuf),
        (unsigned long long) sdsavail(client->querybuf),
        (unsigned long long) client->argv_len_sum,
        (unsigned long long) client->mstate.argv_len_sums,
        (unsigned long long) client->buf_usable_size,
        (unsigned long long) client->buf_peak,
        (unsigned long long) client->bufpos,
        (unsigned long long) listLength(client->reply) + used_blocks_of_repl_buf,
        (unsigned long long) obufmem, /* should not include client->buf since we want to see 0 for static clients. */
        (unsigned long long) total_mem,
        events,
        client->lastcmd ? client->lastcmd->fullname : "NULL",
        client->user ? client->user->name : "(superuser)",
        (client->flags & CLIENT_TRACKING) ? (long long) client->client_tracking_redirection : -1,
        client->resp);
    return ret;
}

sds getAllClientsInfoString(int type) {
    listNode *ln;
    listIter li;
    client *client;
    sds o = sdsnewlen(SDS_NOINIT,200*listLength(server.clients));
    sdsclear(o);
    listRewind(server.clients,&li);
    while ((ln = listNext(&li)) != NULL) {
        client = listNodeValue(ln);
        if (type != -1 && getClientType(client) != type) continue;
        o = catClientInfoString(o,client);
        o = sdscatlen(o,"\n",1);
    }
    return o;
}

/* Returns C_OK if the name has been set or C_ERR if the name is invalid. */
/**如果名称已设置，则返回 C_OK；如果名称无效，则返回 C_ERR。*/
int clientSetName(client *c, robj *name) {
    int len = (name != NULL) ? sdslen(name->ptr) : 0;

    /* Setting the client name to an empty string actually removes
     * the current name. */
    if (len == 0) {
        if (c->name) decrRefCount(c->name);
        c->name = NULL;
        return C_OK;
    }

    /* Otherwise check if the charset is ok. We need to do this otherwise
     * CLIENT LIST format will break. You should always be able to
     * split by space to get the different fields. */
    /**否则检查字符集是否正常。我们需要这样做，否则 CLIENT LIST 格式会中断。
     * 您应该始终能够按空间拆分以获取不同的字段。*/
    char *p = name->ptr;
    for (int j = 0; j < len; j++) {
        if (p[j] < '!' || p[j] > '~') { /* ASCII is assumed. */
            return C_ERR;
        }
    }
    if (c->name) decrRefCount(c->name);
    c->name = name;
    incrRefCount(name);
    return C_OK;
}

/* This function implements CLIENT SETNAME, including replying to the
 * user with an error if the charset is wrong (in that case C_ERR is
 * returned). If the function succeeded C_OK is returned, and it's up
 * to the caller to send a reply if needed.
 *
 * Setting an empty string as name has the effect of unsetting the
 * currently set name: the client will remain unnamed.
 *
 * This function is also used to implement the HELLO SETNAME option. */
/**此函数实现 CLIENT SETNAME，包括在字符集错误时向用户回复错误（在这种情况下返回 C_ERR）。
 * 如果函数成功，则返回 C_OK，如果需要，由调用者发送回复。
 *
 * 将空字符串设置为名称具有取消设置当前设置名称的效果：客户端将保持未命名。
 *
 * 该函数也用于实现 HELLO SETNAME 选项。*/
int clientSetNameOrReply(client *c, robj *name) {
    int result = clientSetName(c, name);
    if (result == C_ERR) {
        addReplyError(c,
                      "Client names cannot contain spaces, "
                      "newlines or special characters.");
    }
    return result;
}

/* Reset the client state to resemble a newly connected client.
 */
/**将客户端状态重置为类似于新连接的客户端。*/
void resetCommand(client *c) {
    /* MONITOR clients are also marked with CLIENT_SLAVE, we need to
     * distinguish between the two.
     */
    uint64_t flags = c->flags;
    if (flags & CLIENT_MONITOR) flags &= ~(CLIENT_MONITOR|CLIENT_SLAVE);

    if (flags & (CLIENT_SLAVE|CLIENT_MASTER|CLIENT_MODULE)) {
        addReplyError(c,"can only reset normal client connections");
        return;
    }

    clearClientConnectionState(c);
    addReplyStatus(c,"RESET");
}

/* Disconnect the current client */
void quitCommand(client *c) {
    addReply(c,shared.ok);
    c->flags |= CLIENT_CLOSE_AFTER_REPLY;
}

void clientCommand(client *c) {
    listNode *ln;
    listIter li;

    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"help")) {
        const char *help[] = {
"CACHING (YES|NO)",
"    Enable/disable tracking of the keys for next command in OPTIN/OPTOUT modes.",
"GETREDIR",
"    Return the client ID we are redirecting to when tracking is enabled.",
"GETNAME",
"    Return the name of the current connection.",
"ID",
"    Return the ID of the current connection.",
"INFO",
"    Return information about the current client connection.",
"KILL <ip:port>",
"    Kill connection made from <ip:port>.",
"KILL <option> <value> [<option> <value> [...]]",
"    Kill connections. Options are:",
"    * ADDR (<ip:port>|<unixsocket>:0)",
"      Kill connections made from the specified address",
"    * LADDR (<ip:port>|<unixsocket>:0)",
"      Kill connections made to specified local address",
"    * TYPE (NORMAL|MASTER|REPLICA|PUBSUB)",
"      Kill connections by type.",
"    * USER <username>",
"      Kill connections authenticated by <username>.",
"    * SKIPME (YES|NO)",
"      Skip killing current connection (default: yes).",
"LIST [options ...]",
"    Return information about client connections. Options:",
"    * TYPE (NORMAL|MASTER|REPLICA|PUBSUB)",
"      Return clients of specified type.",
"UNPAUSE",
"    Stop the current client pause, resuming traffic.",
"PAUSE <timeout> [WRITE|ALL]",
"    Suspend all, or just write, clients for <timeout> milliseconds.",
"REPLY (ON|OFF|SKIP)",
"    Control the replies sent to the current connection.",
"SETNAME <name>",
"    Assign the name <name> to the current connection.",
"UNBLOCK <clientid> [TIMEOUT|ERROR]",
"    Unblock the specified blocked client.",
"TRACKING (ON|OFF) [REDIRECT <id>] [BCAST] [PREFIX <prefix> [...]]",
"         [OPTIN] [OPTOUT] [NOLOOP]",
"    Control server assisted client side caching.",
"TRACKINGINFO",
"    Report tracking status for the current connection.",
"NO-EVICT (ON|OFF)",
"    Protect current client connection from eviction.",
NULL
        };
        addReplyHelp(c, help);
    } else if (!strcasecmp(c->argv[1]->ptr,"id") && c->argc == 2) {
        /* CLIENT ID */
        addReplyLongLong(c,c->id);
    } else if (!strcasecmp(c->argv[1]->ptr,"info") && c->argc == 2) {
        /* CLIENT INFO */
        sds o = catClientInfoString(sdsempty(), c);
        o = sdscatlen(o,"\n",1);
        addReplyVerbatim(c,o,sdslen(o),"txt");
        sdsfree(o);
    } else if (!strcasecmp(c->argv[1]->ptr,"list")) {
        /* CLIENT LIST */
        int type = -1;
        sds o = NULL;
        if (c->argc == 4 && !strcasecmp(c->argv[2]->ptr,"type")) {
            type = getClientTypeByName(c->argv[3]->ptr);
            if (type == -1) {
                addReplyErrorFormat(c,"Unknown client type '%s'",
                    (char*) c->argv[3]->ptr);
                return;
            }
        } else if (c->argc > 3 && !strcasecmp(c->argv[2]->ptr,"id")) {
            int j;
            o = sdsempty();
            for (j = 3; j < c->argc; j++) {
                long long cid;
                if (getLongLongFromObjectOrReply(c, c->argv[j], &cid,
                            "Invalid client ID")) {
                    sdsfree(o);
                    return;
                }
                client *cl = lookupClientByID(cid);
                if (cl) {
                    o = catClientInfoString(o, cl);
                    o = sdscatlen(o, "\n", 1);
                }
            }
        } else if (c->argc != 2) {
            addReplyErrorObject(c,shared.syntaxerr);
            return;
        }

        if (!o)
            o = getAllClientsInfoString(type);
        addReplyVerbatim(c,o,sdslen(o),"txt");
        sdsfree(o);
    } else if (!strcasecmp(c->argv[1]->ptr,"reply") && c->argc == 3) {
        /* CLIENT REPLY ON|OFF|SKIP */
        if (!strcasecmp(c->argv[2]->ptr,"on")) {
            c->flags &= ~(CLIENT_REPLY_SKIP|CLIENT_REPLY_OFF);
            addReply(c,shared.ok);
        } else if (!strcasecmp(c->argv[2]->ptr,"off")) {
            c->flags |= CLIENT_REPLY_OFF;
        } else if (!strcasecmp(c->argv[2]->ptr,"skip")) {
            if (!(c->flags & CLIENT_REPLY_OFF))
                c->flags |= CLIENT_REPLY_SKIP_NEXT;
        } else {
            addReplyErrorObject(c,shared.syntaxerr);
            return;
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"no-evict") && c->argc == 3) {
        /* CLIENT NO-EVICT ON|OFF */
        if (!strcasecmp(c->argv[2]->ptr,"on")) {
            c->flags |= CLIENT_NO_EVICT;
            addReply(c,shared.ok);
        } else if (!strcasecmp(c->argv[2]->ptr,"off")) {
            c->flags &= ~CLIENT_NO_EVICT;
            addReply(c,shared.ok);
        } else {
            addReplyErrorObject(c,shared.syntaxerr);
            return;
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"kill")) {
        /* CLIENT KILL <ip:port>
         * CLIENT KILL <option> [value] ... <option> [value] */
        char *addr = NULL;
        char *laddr = NULL;
        user *user = NULL;
        int type = -1;
        uint64_t id = 0;
        int skipme = 1;
        int killed = 0, close_this_client = 0;

        if (c->argc == 3) {
            /* Old style syntax: CLIENT KILL <addr> */
            addr = c->argv[2]->ptr;
            skipme = 0; /* With the old form, you can kill yourself. */
        } else if (c->argc > 3) {
            int i = 2; /* Next option index. */

            /* New style syntax: parse options. */
            while(i < c->argc) {
                int moreargs = c->argc > i+1;

                if (!strcasecmp(c->argv[i]->ptr,"id") && moreargs) {
                    long tmp;

                    if (getRangeLongFromObjectOrReply(c, c->argv[i+1], 1, LONG_MAX, &tmp,
                                                      "client-id should be greater than 0") != C_OK)
                        return;
                    id = tmp;
                } else if (!strcasecmp(c->argv[i]->ptr,"type") && moreargs) {
                    type = getClientTypeByName(c->argv[i+1]->ptr);
                    if (type == -1) {
                        addReplyErrorFormat(c,"Unknown client type '%s'",
                            (char*) c->argv[i+1]->ptr);
                        return;
                    }
                } else if (!strcasecmp(c->argv[i]->ptr,"addr") && moreargs) {
                    addr = c->argv[i+1]->ptr;
                } else if (!strcasecmp(c->argv[i]->ptr,"laddr") && moreargs) {
                    laddr = c->argv[i+1]->ptr;
                } else if (!strcasecmp(c->argv[i]->ptr,"user") && moreargs) {
                    user = ACLGetUserByName(c->argv[i+1]->ptr,
                                            sdslen(c->argv[i+1]->ptr));
                    if (user == NULL) {
                        addReplyErrorFormat(c,"No such user '%s'",
                            (char*) c->argv[i+1]->ptr);
                        return;
                    }
                } else if (!strcasecmp(c->argv[i]->ptr,"skipme") && moreargs) {
                    if (!strcasecmp(c->argv[i+1]->ptr,"yes")) {
                        skipme = 1;
                    } else if (!strcasecmp(c->argv[i+1]->ptr,"no")) {
                        skipme = 0;
                    } else {
                        addReplyErrorObject(c,shared.syntaxerr);
                        return;
                    }
                } else {
                    addReplyErrorObject(c,shared.syntaxerr);
                    return;
                }
                i += 2;
            }
        } else {
            addReplyErrorObject(c,shared.syntaxerr);
            return;
        }

        /* Iterate clients killing all the matching clients. */
        listRewind(server.clients,&li);
        while ((ln = listNext(&li)) != NULL) {
            client *client = listNodeValue(ln);
            if (addr && strcmp(getClientPeerId(client),addr) != 0) continue;
            if (laddr && strcmp(getClientSockname(client),laddr) != 0) continue;
            if (type != -1 && getClientType(client) != type) continue;
            if (id != 0 && client->id != id) continue;
            if (user && client->user != user) continue;
            if (c == client && skipme) continue;

            /* Kill it. */
            if (c == client) {
                close_this_client = 1;
            } else {
                freeClient(client);
            }
            killed++;
        }

        /* Reply according to old/new format. */
        if (c->argc == 3) {
            if (killed == 0)
                addReplyError(c,"No such client");
            else
                addReply(c,shared.ok);
        } else {
            addReplyLongLong(c,killed);
        }

        /* If this client has to be closed, flag it as CLOSE_AFTER_REPLY
         * only after we queued the reply to its output buffers. */
        if (close_this_client) c->flags |= CLIENT_CLOSE_AFTER_REPLY;
    } else if (!strcasecmp(c->argv[1]->ptr,"unblock") && (c->argc == 3 ||
                                                          c->argc == 4))
    {
        /* CLIENT UNBLOCK <id> [timeout|error] */
        long long id;
        int unblock_error = 0;

        if (c->argc == 4) {
            if (!strcasecmp(c->argv[3]->ptr,"timeout")) {
                unblock_error = 0;
            } else if (!strcasecmp(c->argv[3]->ptr,"error")) {
                unblock_error = 1;
            } else {
                addReplyError(c,
                    "CLIENT UNBLOCK reason should be TIMEOUT or ERROR");
                return;
            }
        }
        if (getLongLongFromObjectOrReply(c,c->argv[2],&id,NULL)
            != C_OK) return;
        struct client *target = lookupClientByID(id);
        /* Note that we never try to unblock a client blocked on a module command, which
         * doesn't have a timeout callback (even in the case of UNBLOCK ERROR).
         * The reason is that we assume that if a command doesn't expect to be timedout,
         * it also doesn't expect to be unblocked by CLIENT UNBLOCK */
        /**请注意，我们从不尝试解除阻止被模块命令阻止的客户端，该命令没有超时回调（即使在 UNBLOCK ERROR 的情况下）。
         * 原因是我们假设如果一个命令不期望超时，它也不期望被 CLIENT UNBLOCK 解除阻塞*/
        if (target && target->flags & CLIENT_BLOCKED && moduleBlockedClientMayTimeout(target)) {
            if (unblock_error)
                addReplyError(target,
                    "-UNBLOCKED client unblocked via CLIENT UNBLOCK");
            else
                replyToBlockedClientTimedOut(target);
            unblockClient(target);
            updateStatsOnUnblock(target, 0, 0, 1);
            addReply(c,shared.cone);
        } else {
            addReply(c,shared.czero);
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"setname") && c->argc == 3) {
        /* CLIENT SETNAME */
        if (clientSetNameOrReply(c,c->argv[2]) == C_OK)
            addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"getname") && c->argc == 2) {
        /* CLIENT GETNAME */
        if (c->name)
            addReplyBulk(c,c->name);
        else
            addReplyNull(c);
    } else if (!strcasecmp(c->argv[1]->ptr,"unpause") && c->argc == 2) {
        /* CLIENT UNPAUSE */
        unpauseClients(PAUSE_BY_CLIENT_COMMAND);
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"pause") && (c->argc == 3 ||
                                                        c->argc == 4))
    {
        /* CLIENT PAUSE TIMEOUT [WRITE|ALL] */
        mstime_t end;
        int type = CLIENT_PAUSE_ALL;
        if (c->argc == 4) {
            if (!strcasecmp(c->argv[3]->ptr,"write")) {
                type = CLIENT_PAUSE_WRITE;
            } else if (!strcasecmp(c->argv[3]->ptr,"all")) {
                type = CLIENT_PAUSE_ALL;
            } else {
                addReplyError(c,
                    "CLIENT PAUSE mode must be WRITE or ALL");  
                return;       
            }
        }

        if (getTimeoutFromObjectOrReply(c,c->argv[2],&end,
            UNIT_MILLISECONDS) != C_OK) return;
        pauseClients(PAUSE_BY_CLIENT_COMMAND, end, type);
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"tracking") && c->argc >= 3) {
        /* CLIENT TRACKING (on|off) [REDIRECT <id>] [BCAST] [PREFIX first]
         *                          [PREFIX second] [OPTIN] [OPTOUT] [NOLOOP]... */
        long long redir = 0;
        uint64_t options = 0;
        robj **prefix = NULL;
        size_t numprefix = 0;

        /* Parse the options. */
        for (int j = 3; j < c->argc; j++) {
            int moreargs = (c->argc-1) - j;

            if (!strcasecmp(c->argv[j]->ptr,"redirect") && moreargs) {
                j++;
                if (redir != 0) {
                    addReplyError(c,"A client can only redirect to a single "
                                    "other client");
                    zfree(prefix);
                    return;
                }

                if (getLongLongFromObjectOrReply(c,c->argv[j],&redir,NULL) !=
                    C_OK)
                {
                    zfree(prefix);
                    return;
                }
                /* We will require the client with the specified ID to exist
                 * right now, even if it is possible that it gets disconnected
                 * later. Still a valid sanity check. */
                if (lookupClientByID(redir) == NULL) {
                    addReplyError(c,"The client ID you want redirect to "
                                    "does not exist");
                    zfree(prefix);
                    return;
                }
            } else if (!strcasecmp(c->argv[j]->ptr,"bcast")) {
                options |= CLIENT_TRACKING_BCAST;
            } else if (!strcasecmp(c->argv[j]->ptr,"optin")) {
                options |= CLIENT_TRACKING_OPTIN;
            } else if (!strcasecmp(c->argv[j]->ptr,"optout")) {
                options |= CLIENT_TRACKING_OPTOUT;
            } else if (!strcasecmp(c->argv[j]->ptr,"noloop")) {
                options |= CLIENT_TRACKING_NOLOOP;
            } else if (!strcasecmp(c->argv[j]->ptr,"prefix") && moreargs) {
                j++;
                prefix = zrealloc(prefix,sizeof(robj*)*(numprefix+1));
                prefix[numprefix++] = c->argv[j];
            } else {
                zfree(prefix);
                addReplyErrorObject(c,shared.syntaxerr);
                return;
            }
        }

        /* Options are ok: enable or disable the tracking for this client. */
        if (!strcasecmp(c->argv[2]->ptr,"on")) {
            /* Before enabling tracking, make sure options are compatible
             * among each other and with the current state of the client. */
            if (!(options & CLIENT_TRACKING_BCAST) && numprefix) {
                addReplyError(c,
                    "PREFIX option requires BCAST mode to be enabled");
                zfree(prefix);
                return;
            }

            if (c->flags & CLIENT_TRACKING) {
                int oldbcast = !!(c->flags & CLIENT_TRACKING_BCAST);
                int newbcast = !!(options & CLIENT_TRACKING_BCAST);
                if (oldbcast != newbcast) {
                    addReplyError(c,
                    "You can't switch BCAST mode on/off before disabling "
                    "tracking for this client, and then re-enabling it with "
                    "a different mode.");
                    zfree(prefix);
                    return;
                }
            }

            if (options & CLIENT_TRACKING_BCAST &&
                options & (CLIENT_TRACKING_OPTIN|CLIENT_TRACKING_OPTOUT))
            {
                addReplyError(c,
                "OPTIN and OPTOUT are not compatible with BCAST");
                zfree(prefix);
                return;
            }

            if (options & CLIENT_TRACKING_OPTIN && options & CLIENT_TRACKING_OPTOUT)
            {
                addReplyError(c,
                "You can't specify both OPTIN mode and OPTOUT mode");
                zfree(prefix);
                return;
            }

            if ((options & CLIENT_TRACKING_OPTIN && c->flags & CLIENT_TRACKING_OPTOUT) ||
                (options & CLIENT_TRACKING_OPTOUT && c->flags & CLIENT_TRACKING_OPTIN))
            {
                addReplyError(c,
                "You can't switch OPTIN/OPTOUT mode before disabling "
                "tracking for this client, and then re-enabling it with "
                "a different mode.");
                zfree(prefix);
                return;
            }

            if (options & CLIENT_TRACKING_BCAST) {
                if (!checkPrefixCollisionsOrReply(c,prefix,numprefix)) {
                    zfree(prefix);
                    return;
                }
            }

            enableTracking(c,redir,options,prefix,numprefix);
        } else if (!strcasecmp(c->argv[2]->ptr,"off")) {
            disableTracking(c);
        } else {
            zfree(prefix);
            addReplyErrorObject(c,shared.syntaxerr);
            return;
        }
        zfree(prefix);
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"caching") && c->argc >= 3) {
        if (!(c->flags & CLIENT_TRACKING)) {
            addReplyError(c,"CLIENT CACHING can be called only when the "
                            "client is in tracking mode with OPTIN or "
                            "OPTOUT mode enabled");
            return;
        }

        char *opt = c->argv[2]->ptr;
        if (!strcasecmp(opt,"yes")) {
            if (c->flags & CLIENT_TRACKING_OPTIN) {
                c->flags |= CLIENT_TRACKING_CACHING;
            } else {
                addReplyError(c,"CLIENT CACHING YES is only valid when tracking is enabled in OPTIN mode.");
                return;
            }
        } else if (!strcasecmp(opt,"no")) {
            if (c->flags & CLIENT_TRACKING_OPTOUT) {
                c->flags |= CLIENT_TRACKING_CACHING;
            } else {
                addReplyError(c,"CLIENT CACHING NO is only valid when tracking is enabled in OPTOUT mode.");
                return;
            }
        } else {
            addReplyErrorObject(c,shared.syntaxerr);
            return;
        }

        /* Common reply for when we succeeded. */
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"getredir") && c->argc == 2) {
        /* CLIENT GETREDIR */
        if (c->flags & CLIENT_TRACKING) {
            addReplyLongLong(c,c->client_tracking_redirection);
        } else {
            addReplyLongLong(c,-1);
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"trackinginfo") && c->argc == 2) {
        addReplyMapLen(c,3);

        /* Flags */
        addReplyBulkCString(c,"flags");
        void *arraylen_ptr = addReplyDeferredLen(c);
        int numflags = 0;
        addReplyBulkCString(c,c->flags & CLIENT_TRACKING ? "on" : "off");
        numflags++;
        if (c->flags & CLIENT_TRACKING_BCAST) {
            addReplyBulkCString(c,"bcast");
            numflags++;
        }
        if (c->flags & CLIENT_TRACKING_OPTIN) {
            addReplyBulkCString(c,"optin");
            numflags++;
            if (c->flags & CLIENT_TRACKING_CACHING) {
                addReplyBulkCString(c,"caching-yes");
                numflags++;        
            }
        }
        if (c->flags & CLIENT_TRACKING_OPTOUT) {
            addReplyBulkCString(c,"optout");
            numflags++;
            if (c->flags & CLIENT_TRACKING_CACHING) {
                addReplyBulkCString(c,"caching-no");
                numflags++;        
            }
        }
        if (c->flags & CLIENT_TRACKING_NOLOOP) {
            addReplyBulkCString(c,"noloop");
            numflags++;
        }
        if (c->flags & CLIENT_TRACKING_BROKEN_REDIR) {
            addReplyBulkCString(c,"broken_redirect");
            numflags++;
        }
        setDeferredSetLen(c,arraylen_ptr,numflags);

        /* Redirect */
        addReplyBulkCString(c,"redirect");
        if (c->flags & CLIENT_TRACKING) {
            addReplyLongLong(c,c->client_tracking_redirection);
        } else {
            addReplyLongLong(c,-1);
        }

        /* Prefixes */
        addReplyBulkCString(c,"prefixes");
        if (c->client_tracking_prefixes) {
            addReplyArrayLen(c,raxSize(c->client_tracking_prefixes));
            raxIterator ri;
            raxStart(&ri,c->client_tracking_prefixes);
            raxSeek(&ri,"^",NULL,0);
            while(raxNext(&ri)) {
                addReplyBulkCBuffer(c,ri.key,ri.key_len);
            }
            raxStop(&ri);
        } else {
            addReplyArrayLen(c,0);
        }
    } else {
        addReplySubcommandSyntaxError(c);
    }
}

/* HELLO [<protocol-version> [AUTH <user> <password>] [SETNAME <name>] ] */
void helloCommand(client *c) {
    long long ver = 0;
    int next_arg = 1;

    if (c->argc >= 2) {
        if (getLongLongFromObjectOrReply(c, c->argv[next_arg++], &ver,
            "Protocol version is not an integer or out of range") != C_OK) {
            return;
        }

        if (ver < 2 || ver > 3) {
            addReplyError(c,"-NOPROTO unsupported protocol version");
            return;
        }
    }

    for (int j = next_arg; j < c->argc; j++) {
        int moreargs = (c->argc-1) - j;
        const char *opt = c->argv[j]->ptr;
        if (!strcasecmp(opt,"AUTH") && moreargs >= 2) {
            redactClientCommandArgument(c, j+1);
            redactClientCommandArgument(c, j+2);
            if (ACLAuthenticateUser(c, c->argv[j+1], c->argv[j+2]) == C_ERR) {
                addReplyError(c,"-WRONGPASS invalid username-password pair or user is disabled.");
                return;
            }
            j += 2;
        } else if (!strcasecmp(opt,"SETNAME") && moreargs) {
            if (clientSetNameOrReply(c, c->argv[j+1]) == C_ERR) return;
            j++;
        } else {
            addReplyErrorFormat(c,"Syntax error in HELLO option '%s'",opt);
            return;
        }
    }

    /* At this point we need to be authenticated to continue. */
    if (!c->authenticated) {
        addReplyError(c,"-NOAUTH HELLO must be called with the client already "
                        "authenticated, otherwise the HELLO AUTH <user> <pass> "
                        "option can be used to authenticate the client and "
                        "select the RESP protocol version at the same time");
        return;
    }

    /* Let's switch to the specified RESP mode. */
    if (ver) c->resp = ver;
    addReplyMapLen(c,6 + !server.sentinel_mode);

    addReplyBulkCString(c,"server");
    addReplyBulkCString(c,"redis");

    addReplyBulkCString(c,"version");
    addReplyBulkCString(c,REDIS_VERSION);

    addReplyBulkCString(c,"proto");
    addReplyLongLong(c,c->resp);

    addReplyBulkCString(c,"id");
    addReplyLongLong(c,c->id);

    addReplyBulkCString(c,"mode");
    if (server.sentinel_mode) addReplyBulkCString(c,"sentinel");
    else if (server.cluster_enabled) addReplyBulkCString(c,"cluster");
    else addReplyBulkCString(c,"standalone");

    if (!server.sentinel_mode) {
        addReplyBulkCString(c,"role");
        addReplyBulkCString(c,server.masterhost ? "replica" : "master");
    }

    addReplyBulkCString(c,"modules");
    addReplyLoadedModules(c);
}

/* This callback is bound to POST and "Host:" command names. Those are not
 * really commands, but are used in security attacks in order to talk to
 * Redis instances via HTTP, with a technique called "cross protocol scripting"
 * which exploits the fact that services like Redis will discard invalid
 * HTTP headers and will process what follows.
 *
 * As a protection against this attack, Redis will terminate the connection
 * when a POST or "Host:" header is seen, and will log the event from
 * time to time (to avoid creating a DOS as a result of too many logs). */
/**此回调绑定到 POST 和“Host:”命令名称。这些并不是真正的命令，
 * 而是用于安全攻击，以便通过 HTTP 与 Redis 实例通信，使用一种称为“跨协议脚本”的技术，
 * 该技术利用了 Redis 等服务将丢弃无效 HTTP 标头并处理后续内容的事实。
 *
 * 作为对这种攻击的保护，Redis 将在看到 POST 或“Host:”标头时终止连接，
 * 并不时记录该事件（以避免由于日志过多而创建 DOS）。*/
void securityWarningCommand(client *c) {
    static time_t logged_time = 0;
    time_t now = time(NULL);

    if (llabs(now-logged_time) > 60) {
        serverLog(LL_WARNING,"Possible SECURITY ATTACK detected. It looks like somebody is sending POST or Host: commands to Redis. This is likely due to an attacker attempting to use Cross Protocol Scripting to compromise your Redis instance. Connection aborted.");
        logged_time = now;
    }
    freeClientAsync(c);
}

/* Keep track of the original command arguments so that we can generate
 * an accurate slowlog entry after the command has been executed. */
/**跟踪原始命令参数，以便我们可以在命令执行后生成准确的慢日志条目。*/
static void retainOriginalCommandVector(client *c) {
    /* We already rewrote this command, so don't rewrite it again */
    if (c->original_argv) return;
    c->original_argc = c->argc;
    c->original_argv = zmalloc(sizeof(robj*)*(c->argc));
    for (int j = 0; j < c->argc; j++) {
        c->original_argv[j] = c->argv[j];
        incrRefCount(c->argv[j]);
    }
}

/* Redact a given argument to prevent it from being shown
 * in the slowlog. This information is stored in the
 * original_argv array. */
/**编辑给定参数以防止它显示在慢日志中。此信息存储在 original_argv 数组中。*/
void redactClientCommandArgument(client *c, int argc) {
    retainOriginalCommandVector(c);
    if (c->original_argv[argc] == shared.redacted) {
        /* This argument has already been redacted */
        return;
    }
    decrRefCount(c->original_argv[argc]);
    c->original_argv[argc] = shared.redacted;
}

/* Rewrite the command vector of the client. All the new objects ref count
 * is incremented. The old command vector is freed, and the old objects
 * ref count is decremented. */
/**重写客户端的命令向量。所有新对象的引用计数都会增加。旧的命令向量被释放，旧的对象引用计数减少。*/
void rewriteClientCommandVector(client *c, int argc, ...) {
    va_list ap;
    int j;
    robj **argv; /* The new argument vector */

    argv = zmalloc(sizeof(robj*)*argc);
    va_start(ap,argc);
    for (j = 0; j < argc; j++) {
        robj *a;

        a = va_arg(ap, robj*);
        argv[j] = a;
        incrRefCount(a);
    }
    replaceClientCommandVector(c, argc, argv);
    va_end(ap);
}

/* Completely replace the client command vector with the provided one. */
/**用提供的命令向量完全替换客户端命令向量。*/
void replaceClientCommandVector(client *c, int argc, robj **argv) {
    int j;
    retainOriginalCommandVector(c);
    freeClientArgv(c);
    c->argv = argv;
    c->argc = argc;
    c->argv_len_sum = 0;
    for (j = 0; j < c->argc; j++)
        if (c->argv[j])
            c->argv_len_sum += getStringObjectLen(c->argv[j]);
    c->cmd = lookupCommandOrOriginal(c->argv,c->argc);
    serverAssertWithInfo(c,NULL,c->cmd != NULL);
}

/* Rewrite a single item in the command vector.
 * The new val ref count is incremented, and the old decremented.
 *
 * It is possible to specify an argument over the current size of the
 * argument vector: in this case the array of objects gets reallocated
 * and c->argc set to the max value. However it's up to the caller to
 *
 * 1. Make sure there are no "holes" and all the arguments are set.
 * 2. If the original argument vector was longer than the one we
 *    want to end with, it's up to the caller to set c->argc and
 *    free the no longer used objects on c->argv. */
/**重写命令向量中的单个项目。新的 val ref 计数增加，旧的减少。
 *
 * 可以在参数向量的当前大小上指定一个参数：在这种情况下，对象数组被重新分配并且 c->argc 设置为最大值。
 * 但是，这取决于调用者
 *   1。确保没有“漏洞”并且所有参数都已设置。
 *   2. 如果原始参数向量比我们想要结束的那个长，则由调用者设置 c->argc 并释放 c->argv 上不再使用的对象。*/
void rewriteClientCommandArgument(client *c, int i, robj *newval) {
    robj *oldval;
    retainOriginalCommandVector(c);

    /* We need to handle both extending beyond argc (just update it and
     * initialize the new element) or beyond argv_len (realloc is needed).
     */
    if (i >= c->argc) {
        if (i >= c->argv_len) {
            c->argv = zrealloc(c->argv,sizeof(robj*)*(i+1));
            c->argv_len = i+1;
        }
        c->argc = i+1;
        c->argv[i] = NULL;
    }
    oldval = c->argv[i];
    if (oldval) c->argv_len_sum -= getStringObjectLen(oldval);
    if (newval) c->argv_len_sum += getStringObjectLen(newval);
    c->argv[i] = newval;
    incrRefCount(newval);
    if (oldval) decrRefCount(oldval);

    /* If this is the command name make sure to fix c->cmd. */
    if (i == 0) {
        c->cmd = lookupCommandOrOriginal(c->argv,c->argc);
        serverAssertWithInfo(c,NULL,c->cmd != NULL);
    }
}

/* This function returns the number of bytes that Redis is
 * using to store the reply still not read by the client.
 *
 * Note: this function is very fast so can be called as many time as
 * the caller wishes. The main usage of this function currently is
 * enforcing the client output length limits. */
/**此函数返回 Redis 用于存储客户端仍未读取的回复的字节数。
 *
 * 注意：此函数非常快，因此可以根据调用者的意愿多次调用。
 * 此功能目前的主要用途是强制执行客户端输出长度限制。*/
size_t getClientOutputBufferMemoryUsage(client *c) {
    if (getClientType(c) == CLIENT_TYPE_SLAVE) {
        size_t repl_buf_size = 0;
        size_t repl_node_num = 0;
        size_t repl_node_size = sizeof(listNode) + sizeof(replBufBlock);
        if (c->ref_repl_buf_node) {
            replBufBlock *last = listNodeValue(listLast(server.repl_buffer_blocks));
            replBufBlock *cur = listNodeValue(c->ref_repl_buf_node);
            repl_buf_size = last->repl_offset + last->size - cur->repl_offset;
            repl_node_num = last->id - cur->id + 1;
        }
        return repl_buf_size + (repl_node_size*repl_node_num);
    } else { 
        size_t list_item_size = sizeof(listNode) + sizeof(clientReplyBlock);
        return c->reply_bytes + (list_item_size*listLength(c->reply));
    }
}

/* Returns the total client's memory usage.
 * Optionally, if output_buffer_mem_usage is not NULL, it fills it with
 * the client output buffer memory usage portion of the total. */
/**返回客户端的总内存使用量。
 * 可选地，如果 output_buffer_mem_usage 不为 NULL，它会用总的客户端输出缓冲区内存使用部分填充它。*/
size_t getClientMemoryUsage(client *c, size_t *output_buffer_mem_usage) {
    size_t mem = getClientOutputBufferMemoryUsage(c);
    if (output_buffer_mem_usage != NULL)
        *output_buffer_mem_usage = mem;
    mem += sdsZmallocSize(c->querybuf);
    mem += zmalloc_size(c);
    mem += c->buf_usable_size;
    /* For efficiency (less work keeping track of the argv memory), it doesn't include the used memory
     * i.e. unused sds space and internal fragmentation, just the string length. but this is enough to
     * spot problematic clients. */
    /**为了提高效率（跟踪 argv 内存的工作量更少），它不包括已用内存，
     * 即未使用的 sds 空间和内部碎片，仅包括字符串长度。但这足以发现有问题的客户。*/
    mem += c->argv_len_sum + sizeof(robj*)*c->argc;
    mem += multiStateMemOverhead(c);

    /* Add memory overhead of pubsub channels and patterns. Note: this is just the overhead of the robj pointers
     * to the strings themselves because they aren't stored per client. */
    /**添加 pubsub 通道和模式的内存开销。
     * 注意：这只是指向字符串本身的 robj 指针的开销，因为它们不是按客户端存储的。*/
    mem += pubsubMemOverhead(c);

    /* Add memory overhead of the tracking prefixes, this is an underestimation so we don't need to traverse the entire rax */
    /**添加跟踪前缀的内存开销，这是一个低估，所以我们不需要遍历整个 rax*/
    if (c->client_tracking_prefixes)
        mem += c->client_tracking_prefixes->numnodes * (sizeof(raxNode) * sizeof(raxNode*));

    return mem;
}

/* Get the class of a client, used in order to enforce limits to different
 * classes of clients.
 * 获取客户端的类别，用于对不同类别的客户端实施限制。
 *
 * The function will return one of the following:
 * 该函数将返回以下之一：
 * CLIENT_TYPE_NORMAL -> Normal client
 * CLIENT_TYPE_SLAVE  -> Slave
 * CLIENT_TYPE_PUBSUB -> Client subscribed to Pub/Sub channels 客户订阅了 Pub/Sub 频道
 * CLIENT_TYPE_MASTER -> The client representing our replication master.代表我们的复制主机的客户端。
 */
int getClientType(client *c) {
    if (c->flags & CLIENT_MASTER) return CLIENT_TYPE_MASTER;
    /* Even though MONITOR clients are marked as replicas, we
     * want the expose them as normal clients. */
    /**即使 MONITOR 客户端被标记为副本，我们也希望将它们公开为普通客户端。*/
    if ((c->flags & CLIENT_SLAVE) && !(c->flags & CLIENT_MONITOR))
        return CLIENT_TYPE_SLAVE;
    if (c->flags & CLIENT_PUBSUB) return CLIENT_TYPE_PUBSUB;
    return CLIENT_TYPE_NORMAL;
}

int getClientTypeByName(char *name) {
    if (!strcasecmp(name,"normal")) return CLIENT_TYPE_NORMAL;
    else if (!strcasecmp(name,"slave")) return CLIENT_TYPE_SLAVE;
    else if (!strcasecmp(name,"replica")) return CLIENT_TYPE_SLAVE;
    else if (!strcasecmp(name,"pubsub")) return CLIENT_TYPE_PUBSUB;
    else if (!strcasecmp(name,"master")) return CLIENT_TYPE_MASTER;
    else return -1;
}

char *getClientTypeName(int class) {
    switch(class) {
    case CLIENT_TYPE_NORMAL: return "normal";
    case CLIENT_TYPE_SLAVE:  return "slave";
    case CLIENT_TYPE_PUBSUB: return "pubsub";
    case CLIENT_TYPE_MASTER: return "master";
    default:                       return NULL;
    }
}

/* The function checks if the client reached output buffer soft or hard
 * limit, and also update the state needed to check the soft limit as
 * a side effect.
 *
 * Return value: non-zero if the client reached the soft or the hard limit.
 *               Otherwise zero is returned. */
/**该函数检查客户端是否达到输出缓冲区软限制或硬限制，并更新检查软限制所需的状态作为副作用。
 * 返回值：如果客户端达到软或硬限制，则非零。否则返回零。*/
int checkClientOutputBufferLimits(client *c) {
    int soft = 0, hard = 0, class;
    unsigned long used_mem = getClientOutputBufferMemoryUsage(c);

    class = getClientType(c);
    /* For the purpose of output buffer limiting, masters are handled
     * like normal clients. */
    /**出于输出缓冲区限制的目的，master 的处理方式与普通客户端一样。*/
    if (class == CLIENT_TYPE_MASTER) class = CLIENT_TYPE_NORMAL;

    /* Note that it doesn't make sense to set the replica clients output buffer
     * limit lower than the repl-backlog-size config (partial sync will succeed
     * and then replica will get disconnected).
     * Such a configuration is ignored (the size of repl-backlog-size will be used).
     * This doesn't have memory consumption implications since the replica client
     * will share the backlog buffers memory. */
    /**请注意，将副本客户端输出缓冲区限制设置为低于 repl-backlog-size 配置是没有意义的（部分同步将成功，然后副本将断开连接）。
     * 这样的配置将被忽略（将使用 repl-backlog-size 的大小）。
     * 这不会影响内存消耗，因为副本客户端将共享积压缓冲区内存。*/
    size_t hard_limit_bytes = server.client_obuf_limits[class].hard_limit_bytes;
    if (class == CLIENT_TYPE_SLAVE && hard_limit_bytes &&
        (long long)hard_limit_bytes < server.repl_backlog_size)
        hard_limit_bytes = server.repl_backlog_size;
    if (server.client_obuf_limits[class].hard_limit_bytes &&
        used_mem >= hard_limit_bytes)
        hard = 1;
    if (server.client_obuf_limits[class].soft_limit_bytes &&
        used_mem >= server.client_obuf_limits[class].soft_limit_bytes)
        soft = 1;

    /* We need to check if the soft limit is reached continuously for the
     * specified amount of seconds. */
    /**我们需要检查是否在指定的秒数内连续达到软限制。*/
    if (soft) {
        if (c->obuf_soft_limit_reached_time == 0) {
            c->obuf_soft_limit_reached_time = server.unixtime;
            soft = 0; /* First time we see the soft limit reached 我们第一次看到达到了软限制*/
        } else {
            time_t elapsed = server.unixtime - c->obuf_soft_limit_reached_time;

            if (elapsed <=
                server.client_obuf_limits[class].soft_limit_seconds) {
                soft = 0; /* The client still did not reached the max number of
                             seconds for the soft limit to be considered
                             reached. */
                          /**客户端仍然没有达到软限制被认为达到的最大秒数。*/
            }
        }
    } else {
        c->obuf_soft_limit_reached_time = 0;
    }
    return soft || hard;
}

/* Asynchronously close a client if soft or hard limit is reached on the
 * output buffer size. The caller can check if the client will be closed
 * checking if the client CLIENT_CLOSE_ASAP flag is set.
 *
 * Note: we need to close the client asynchronously because this function is
 * called from contexts where the client can't be freed safely, i.e. from the
 * lower level functions pushing data inside the client output buffers.
 * When `async` is set to 0, we close the client immediately, this is
 * useful when called from cron.
 *
 * Returns 1 if client was (flagged) closed. */
/**如果输出缓冲区大小达到软或硬限制，则异步关闭客户端。
 * 调用者可以检查客户端是否将关闭，检查客户端 CLIENT_CLOSE_ASAP 标志是否设置。
 * 注意：我们需要异步关闭客户端，因为此函数是从无法安全释放客户端的上下文中调用的，
 * 即从将数据推送到客户端输出缓冲区内的较低级别函数中调用。当 `async` 设置为 0 时，我们会立即关闭客户端，
 * 这在从 cron 调用时很有用。如果客户端已（标记）关闭，则返回 1。*/
int closeClientOnOutputBufferLimitReached(client *c, int async) {
    if (!c->conn) return 0; /* It is unsafe to free fake clients. 释放假客户是不安全的。*/
    serverAssert(c->reply_bytes < SIZE_MAX-(1024*64));
    /* Note that c->reply_bytes is irrelevant for replica clients
     * (they use the global repl buffers). */
    /**请注意，c->reply_bytes 与副本客户端无关（它们使用全局 repl 缓冲区）。*/
    if ((c->reply_bytes == 0 && getClientType(c) != CLIENT_TYPE_SLAVE) ||
        c->flags & CLIENT_CLOSE_ASAP) return 0;
    if (checkClientOutputBufferLimits(c)) {
        sds client = catClientInfoString(sdsempty(),c);

        if (async) {
            freeClientAsync(c);
            serverLog(LL_WARNING,
                      "Client %s scheduled to be closed ASAP for overcoming of output buffer limits.",
                      client);
        } else {
            freeClient(c);
            serverLog(LL_WARNING,
                      "Client %s closed for overcoming of output buffer limits.",
                      client);
        }
        sdsfree(client);
        return  1;
    }
    return 0;
}

/* Helper function used by performEvictions() in order to flush slaves
 * output buffers without returning control to the event loop.
 * This is also called by SHUTDOWN for a best-effort attempt to send
 * slaves the latest writes. */
/**performEvictions() 使用的帮助函数，以便在不将控制权返回给事件循环的情况下刷新从属输出缓冲区。
 * 这也被 SHUTDOWN 调用，以尽最大努力向从属设备发送最新的写入。*/
void flushSlavesOutputBuffers(void) {
    listIter li;
    listNode *ln;

    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = listNodeValue(ln);
        int can_receive_writes = connHasWriteHandler(slave->conn) ||
                                 (slave->flags & CLIENT_PENDING_WRITE);

        /* We don't want to send the pending data to the replica in a few
         * cases:
         *
         * 1. For some reason there is neither the write handler installed
         *    nor the client is flagged as to have pending writes: for some
         *    reason this replica may not be set to receive data. This is
         *    just for the sake of defensive programming.
         *
         * 2. The put_online_on_ack flag is true. To know why we don't want
         *    to send data to the replica in this case, please grep for the
         *    flag for this flag.
         *
         * 3. Obviously if the slave is not ONLINE.
         */
        /**在以下几种情况下，我们不想将挂起的数据发送到副本：
         *   1. 由于某种原因，既没有安装写处理程序，也没有将客户端标记为有挂起的写入：
         *      由于某种原因，此副本可能未设置接收数据。这只是为了防御性编程。
         *   2. put_online_on_ack 标志为真。要知道为什么我们不想在这种情况下向副本发送数据，
         *      请使用 grep 查找该标志的标志。
         *   3.显然，如果从站不在线。*/
        if (slave->replstate == SLAVE_STATE_ONLINE &&
            can_receive_writes &&
            !slave->repl_start_cmd_stream_on_ack &&
            clientHasPendingReplies(slave))
        {
            writeToClient(slave,0);
        }
    }
}

/* Compute current most restrictive pause type and its end time, aggregated for
 * all pause purposes. */
/**计算当前最严格的暂停类型及其结束时间，为所有暂停目的汇总。*/
static void updateClientPauseTypeAndEndTime(void) {
    pause_type old_type = server.client_pause_type;
    pause_type type = CLIENT_PAUSE_OFF;
    mstime_t end = 0;
    for (int i = 0; i < NUM_PAUSE_PURPOSES; i++) {
        pause_event *p = server.client_pause_per_purpose[i];
        if (p == NULL) {
            /* Nothing to do. */
        } else if (p->end < server.mstime) {
            /* This one expired. */
            zfree(p);
            server.client_pause_per_purpose[i] = NULL;
        } else if (p->type > type) {
            /* This type is the most restrictive so far. */
            type = p->type;
        }
    }

    /* Find the furthest end time among the pause purposes of the most
     * restrictive type */
    /**在最严格类型的暂停目的中找到最远的结束时间*/
    for (int i = 0; i < NUM_PAUSE_PURPOSES; i++) {
        pause_event *p = server.client_pause_per_purpose[i];
        if (p != NULL && p->type == type && p->end > end) end = p->end;
    }
    server.client_pause_type = type;
    server.client_pause_end_time = end;

    /* If the pause type is less restrictive than before, we unblock all clients
     * so they are reprocessed (may get re-paused). */
    /**如果暂停类型的限制比以前少，我们会取消阻止所有客户端，以便重新处理它们（可能会重新暂停）。*/
    if (type < old_type) {
        unblockPostponedClients();
    }
}

/* Unblock all paused clients (ones that where blocked by BLOCKED_POSTPONE (possibly in processCommand).
 * This means they'll get re-processed in beforeSleep, and may get paused again if needed. */
/**取消阻止所有暂停的客户端（那些被 BLOCKED_POSTPONE 阻止的客户端（可能在 processCommand 中）。
 * 这意味着它们将在 beforeSleep 中重新处理，并且可能会在需要时再次暂停。*/
void unblockPostponedClients() {
    listNode *ln;
    listIter li;
    listRewind(server.postponed_clients, &li);
    while ((ln = listNext(&li)) != NULL) {
        client *c = listNodeValue(ln);
        unblockClient(c);
    }
}

/* Pause clients up to the specified unixtime (in ms) for a given type of
 * commands.
 *
 * A main use case of this function is to allow pausing replication traffic
 * so that a failover without data loss to occur. Replicas will continue to receive
 * traffic to facilitate this functionality.
 * 
 * This function is also internally used by Redis Cluster for the manual
 * failover procedure implemented by CLUSTER FAILOVER.
 *
 * The function always succeed, even if there is already a pause in progress.
 * In such a case, the duration is set to the maximum and new end time and the
 * type is set to the more restrictive type of pause. */
/**对于给定类型的命令，将客户端暂停到指定的 unixtime（以毫秒为单位）。
 *
 * 此功能的一个主要用例是允许暂停复制流量，以便在不丢失数据的情况下进行故障转移。副本将继续接收流量以促进此功能。
 *
 * Redis 集群内部也使用此功能，用于由 CLUSTER FAILOVER 实现的手动故障转移过程。
 *
 * 该函数总是成功的，即使已经有一个暂停在进行中。
 * 在这种情况下，持续时间设置为最大和新的结束时间，类型设置为更严格的暂停类型。*/
void pauseClients(pause_purpose purpose, mstime_t end, pause_type type) {
    /* Manage pause type and end time per pause purpose. */
    if (server.client_pause_per_purpose[purpose] == NULL) {
        server.client_pause_per_purpose[purpose] = zmalloc(sizeof(pause_event));
        server.client_pause_per_purpose[purpose]->type = type;
        server.client_pause_per_purpose[purpose]->end = end;
    } else {
        pause_event *p = server.client_pause_per_purpose[purpose];
        p->type = max(p->type, type);
        p->end = max(p->end, end);
    }
    updateClientPauseTypeAndEndTime();

    /* We allow write commands that were queued
     * up before and after to execute. We need
     * to track this state so that we don't assert
     * in propagateNow(). */
    /**我们允许执行之前和之后排队的写入命令。我们需要跟踪这个状态，这样我们就不会在propagateNow() 中断言。*/
    if (server.in_exec) {
        server.client_pause_in_transaction = 1;
    }
}

/* Unpause clients and queue them for reprocessing. */
/**取消暂停客户端并将它们排队等待重新处理。*/
void unpauseClients(pause_purpose purpose) {
    if (server.client_pause_per_purpose[purpose] == NULL) return;
    zfree(server.client_pause_per_purpose[purpose]);
    server.client_pause_per_purpose[purpose] = NULL;
    updateClientPauseTypeAndEndTime();
}

/* Returns true if clients are paused and false otherwise. */
/**如果客户端暂停，则返回 true，否则返回 false。*/
int areClientsPaused(void) {
    return server.client_pause_type != CLIENT_PAUSE_OFF;
}

/* Checks if the current client pause has elapsed and unpause clients
 * if it has. Also returns true if clients are now paused and false 
 * otherwise. */
/**检查当前客户端暂停是否已经过去，如果有则取消暂停客户端。如果客户端现在暂停，则返回 true，否则返回 false。*/
int checkClientPauseTimeoutAndReturnIfPaused(void) {
    if (!areClientsPaused())
        return 0;
    if (server.client_pause_end_time < server.mstime) {
        updateClientPauseTypeAndEndTime();
    }
    return areClientsPaused();
}

/* This function is called by Redis in order to process a few events from
 * time to time while blocked into some not interruptible operation.
 * This allows to reply to clients with the -LOADING error while loading the
 * data set at startup or after a full resynchronization with the master
 * and so forth.
 *
 * It calls the event loop in order to process a few events. Specifically we
 * try to call the event loop 4 times as long as we receive acknowledge that
 * some event was processed, in order to go forward with the accept, read,
 * write, close sequence needed to serve a client.
 *
 * The function returns the total number of events processed. */
/**Redis调用此函数是为了不时处理一些事件，同时阻止一些不可中断的操作。
 * 这允许在启动时加载数据集时或在与主服务器完全重新同步后等以 -LOADING 错误回复客户端。
 *
 * 它调用事件循环以处理一些事件。具体来说，只要我们收到确认某个事件已处理，
 * 我们就会尝试调用事件循环 4 次，以便继续为客户端提供服务所需的接受、读取、写入、关闭序列。
 *
 * 该函数返回处理的事件总数。*/
void processEventsWhileBlocked(void) {
    int iterations = 4; /* See the function top-comment. */

    /* Update our cached time since it is used to create and update the last
     * interaction time with clients and for other important things. */
    /*(更新我们的缓存时间，因为它用于创建和更新与客户端的最后交互时间以及其他重要的事情。*/
    updateCachedTime(0);

    /* Note: when we are processing events while blocked (for instance during
     * busy Lua scripts), we set a global flag. When such flag is set, we
     * avoid handling the read part of clients using threaded I/O.
     * See https://github.com/redis/redis/issues/6988 for more info.
     * Note that there could be cases of nested calls to this function,
     * specifically on a busy script during async_loading rdb, and scripts
     * that came from AOF. */
    /**注意：当我们在阻塞的情况下处理事件时（例如在繁忙的 Lua 脚本期间），我们设置了一个全局标志。
     * 当设置了这样的标志时，我们避免使用线程 IO 处理客户端的读取部分。
     * 有关更多信息，请参见 https:github.comredisredisissues6988。
     * 请注意，可能存在对此函数的嵌套调用，特别是在 async_loading rdb 期间的繁忙脚本以及来自 AOF 的脚本上。*/
    ProcessingEventsWhileBlocked++;
    while (iterations--) {
        long long startval = server.events_processed_while_blocked;
        long long ae_events = aeProcessEvents(server.el,
            AE_FILE_EVENTS|AE_DONT_WAIT|
            AE_CALL_BEFORE_SLEEP|AE_CALL_AFTER_SLEEP);
        /* Note that server.events_processed_while_blocked will also get
         * incremented by callbacks called by the event loop handlers. */
        server.events_processed_while_blocked += ae_events;
        long long events = server.events_processed_while_blocked - startval;
        if (!events) break;
    }

    whileBlockedCron();

    ProcessingEventsWhileBlocked--;
    serverAssert(ProcessingEventsWhileBlocked >= 0);
}

/* ==========================================================================
 * Threaded I/O
 * ========================================================================== */

#define IO_THREADS_MAX_NUM 128
#define CACHE_LINE_SIZE 64

typedef struct __attribute__((aligned(CACHE_LINE_SIZE))) threads_pending {
    redisAtomic unsigned long value;
} threads_pending;

pthread_t io_threads[IO_THREADS_MAX_NUM];
pthread_mutex_t io_threads_mutex[IO_THREADS_MAX_NUM];
threads_pending io_threads_pending[IO_THREADS_MAX_NUM];
int io_threads_op;      /* IO_THREADS_OP_IDLE, IO_THREADS_OP_READ or IO_THREADS_OP_WRITE. */ // TODO: should access to this be atomic??!

/* This is the list of clients each thread will serve when threaded I/O is
 * used. We spawn io_threads_num-1 threads, since one is the main thread
 * itself. */
/**这是使用线程 IO 时每个线程将服务的客户端列表。我们生成 io_threads_num-1 个线程，因为其中一个是主线程本身。*/
list *io_threads_list[IO_THREADS_MAX_NUM];

static inline unsigned long getIOPendingCount(int i) {
    unsigned long count = 0;
    atomicGetWithSync(io_threads_pending[i].value, count);
    return count;
}

static inline void setIOPendingCount(int i, unsigned long count) {
    atomicSetWithSync(io_threads_pending[i].value, count);
}

void *IOThreadMain(void *myid) {
    /* The ID is the thread number (from 0 to server.iothreads_num-1), and is
     * used by the thread to just manipulate a single sub-array of clients. */
    /**ID 是线程号（从 0 到 server.iothreads_num-1），线程使用它来操作单个客户端子数组。*/
    long id = (unsigned long)myid;
    char thdname[16];

    snprintf(thdname, sizeof(thdname), "io_thd_%ld", id);
    redis_set_thread_title(thdname);
    redisSetCpuAffinity(server.server_cpulist);
    makeThreadKillable();

    while(1) {
        /* Wait for start */
        for (int j = 0; j < 1000000; j++) {
            if (getIOPendingCount(id) != 0) break;
        }

        /* Give the main thread a chance to stop this thread. */
        if (getIOPendingCount(id) == 0) {
            pthread_mutex_lock(&io_threads_mutex[id]);
            pthread_mutex_unlock(&io_threads_mutex[id]);
            continue;
        }

        serverAssert(getIOPendingCount(id) != 0);

        /* Process: note that the main thread will never touch our list
         * before we drop the pending count to 0. */
        listIter li;
        listNode *ln;
        listRewind(io_threads_list[id],&li);
        while((ln = listNext(&li))) {
            client *c = listNodeValue(ln);
            if (io_threads_op == IO_THREADS_OP_WRITE) {
                writeToClient(c,0);
            } else if (io_threads_op == IO_THREADS_OP_READ) {
                readQueryFromClient(c->conn);
            } else {
                serverPanic("io_threads_op value is unknown");
            }
        }
        listEmpty(io_threads_list[id]);
        setIOPendingCount(id, 0);
    }
}

/* Initialize the data structures needed for threaded I/O. */
/**初始化线程 IO 所需的数据结构。*/
void initThreadedIO(void) {
    server.io_threads_active = 0; /* We start with threads not active. */

    /* Indicate that io-threads are currently idle */
    io_threads_op = IO_THREADS_OP_IDLE;

    /* Don't spawn any thread if the user selected a single thread:
     * we'll handle I/O directly from the main thread. */
    if (server.io_threads_num == 1) return;

    if (server.io_threads_num > IO_THREADS_MAX_NUM) {
        serverLog(LL_WARNING,"Fatal: too many I/O threads configured. "
                             "The maximum number is %d.", IO_THREADS_MAX_NUM);
        exit(1);
    }

    /* Spawn and initialize the I/O threads. */
    for (int i = 0; i < server.io_threads_num; i++) {
        /* Things we do for all the threads including the main thread. */
        io_threads_list[i] = listCreate();
        if (i == 0) continue; /* Thread 0 is the main thread. */

        /* Things we do only for the additional threads. */
        pthread_t tid;
        pthread_mutex_init(&io_threads_mutex[i],NULL);
        setIOPendingCount(i, 0);
        pthread_mutex_lock(&io_threads_mutex[i]); /* Thread will be stopped. */
        if (pthread_create(&tid,NULL,IOThreadMain,(void*)(long)i) != 0) {
            serverLog(LL_WARNING,"Fatal: Can't initialize IO thread.");
            exit(1);
        }
        io_threads[i] = tid;
    }
}

void killIOThreads(void) {
    int err, j;
    for (j = 0; j < server.io_threads_num; j++) {
        if (io_threads[j] == pthread_self()) continue;
        if (io_threads[j] && pthread_cancel(io_threads[j]) == 0) {
            if ((err = pthread_join(io_threads[j],NULL)) != 0) {
                serverLog(LL_WARNING,
                    "IO thread(tid:%lu) can not be joined: %s",
                        (unsigned long)io_threads[j], strerror(err));
            } else {
                serverLog(LL_WARNING,
                    "IO thread(tid:%lu) terminated",(unsigned long)io_threads[j]);
            }
        }
    }
}

void startThreadedIO(void) {
    serverAssert(server.io_threads_active == 0);
    for (int j = 1; j < server.io_threads_num; j++)
        pthread_mutex_unlock(&io_threads_mutex[j]);
    server.io_threads_active = 1;
}

void stopThreadedIO(void) {
    /* We may have still clients with pending reads when this function
     * is called: handle them before stopping the threads. */
    /**调用此函数时，我们可能仍然有等待读取的客户端：在停止线程之前处理它们。*/
    handleClientsWithPendingReadsUsingThreads();
    serverAssert(server.io_threads_active == 1);
    for (int j = 1; j < server.io_threads_num; j++)
        pthread_mutex_lock(&io_threads_mutex[j]);
    server.io_threads_active = 0;
}

/* This function checks if there are not enough pending clients to justify
 * taking the I/O threads active: in that case I/O threads are stopped if
 * currently active. We track the pending writes as a measure of clients
 * we need to handle in parallel, however the I/O threading is disabled
 * globally for reads as well if we have too little pending clients.
 *
 * The function returns 0 if the I/O threading should be used because there
 * are enough active threads, otherwise 1 is returned and the I/O threads
 * could be possibly stopped (if already active) as a side effect. */
/**该函数检查是否没有足够的挂起客户端来证明使 IO 线程处于活动状态：在这种情况下，如果当前处于活动状态，则 IO 线程将停止。
 * 我们跟踪挂起的写入来衡量我们需要并行处理的客户端，但是如果挂起的客户端太少，IO 线程也会全局禁用以进行读取。
 * 如果应该使用 IO 线程，则该函数返回 0，因为有足够的活动线程，否则返回 1，并且 IO 线程可能会停止（如果已经活动）作为副作用。*/
int stopThreadedIOIfNeeded(void) {
    int pending = listLength(server.clients_pending_write);

    /* Return ASAP if IO threads are disabled (single threaded mode). */
    if (server.io_threads_num == 1) return 1;

    if (pending < (server.io_threads_num*2)) {
        if (server.io_threads_active) stopThreadedIO();
        return 1;
    } else {
        return 0;
    }
}

/* This function achieves thread safety using a fan-out -> fan-in paradigm:
 * Fan out: The main thread fans out work to the io-threads which block until
 * setIOPendingCount() is called with a value larger than 0 by the main thread.
 * Fan in: The main thread waits until getIOPendingCount() returns 0. Then
 * it can safely perform post-processing and return to normal synchronous
 * work. */
/**此函数使用扇出 -> 扇入范式实现线程安全：
 *   扇出：主线程将工作扇出到 io 线程，这些线程阻塞直到 setIOPendingCount() 被主线程以大于 0 的值调用。
 *   扇入：主线程一直等到getIOPendingCount()返回0，然后就可以安全的进行后期处理，恢复正常的同步工作。*/
int handleClientsWithPendingWritesUsingThreads(void) {
    int processed = listLength(server.clients_pending_write);
    if (processed == 0) return 0; /* Return ASAP if there are no clients. */

    /* If I/O threads are disabled or we have few clients to serve, don't
     * use I/O threads, but the boring synchronous code. */
    if (server.io_threads_num == 1 || stopThreadedIOIfNeeded()) {
        return handleClientsWithPendingWrites();
    }

    /* Start threads if needed. */
    if (!server.io_threads_active) startThreadedIO();

    /* Distribute the clients across N different lists. */
    listIter li;
    listNode *ln;
    listRewind(server.clients_pending_write,&li);
    int item_id = 0;
    while((ln = listNext(&li))) {
        client *c = listNodeValue(ln);
        c->flags &= ~CLIENT_PENDING_WRITE;

        /* Remove clients from the list of pending writes since
         * they are going to be closed ASAP. */
        if (c->flags & CLIENT_CLOSE_ASAP) {
            listDelNode(server.clients_pending_write, ln);
            continue;
        }

        /* Since all replicas and replication backlog use global replication
         * buffer, to guarantee data accessing thread safe, we must put all
         * replicas client into io_threads_list[0] i.e. main thread handles
         * sending the output buffer of all replicas. */
        if (getClientType(c) == CLIENT_TYPE_SLAVE) {
            listAddNodeTail(io_threads_list[0],c);
            continue;
        }

        int target_id = item_id % server.io_threads_num;
        listAddNodeTail(io_threads_list[target_id],c);
        item_id++;
    }

    /* Give the start condition to the waiting threads, by setting the
     * start condition atomic var. */
    io_threads_op = IO_THREADS_OP_WRITE;
    for (int j = 1; j < server.io_threads_num; j++) {
        int count = listLength(io_threads_list[j]);
        setIOPendingCount(j, count);
    }

    /* Also use the main thread to process a slice of clients. */
    listRewind(io_threads_list[0],&li);
    while((ln = listNext(&li))) {
        client *c = listNodeValue(ln);
        writeToClient(c,0);
    }
    listEmpty(io_threads_list[0]);

    /* Wait for all the other threads to end their work. */
    while(1) {
        unsigned long pending = 0;
        for (int j = 1; j < server.io_threads_num; j++)
            pending += getIOPendingCount(j);
        if (pending == 0) break;
    }

    io_threads_op = IO_THREADS_OP_IDLE;

    /* Run the list of clients again to install the write handler where
     * needed. */
    listRewind(server.clients_pending_write,&li);
    while((ln = listNext(&li))) {
        client *c = listNodeValue(ln);

        /* Update the client in the mem usage after we're done processing it in the io-threads */
        updateClientMemUsage(c);

        /* Install the write handler if there are pending writes in some
         * of the clients. */
        if (clientHasPendingReplies(c)) {
            installClientWriteHandler(c);
        }
    }
    listEmpty(server.clients_pending_write);

    /* Update processed count on server */
    server.stat_io_writes_processed += processed;

    return processed;
}

/* Return 1 if we want to handle the client read later using threaded I/O.
 * This is called by the readable handler of the event loop.
 * As a side effect of calling this function the client is put in the
 * pending read clients and flagged as such. */
/**如果我们想稍后使用线程 IO 处理客户端读取，则返回 1。这由事件循环的可读处理程序调用。
 * 作为调用此函数的副作用，客户端被放入待处理的读取客户端并被标记为这样。*/
int postponeClientRead(client *c) {
    if (server.io_threads_active &&
        server.io_threads_do_reads &&
        !ProcessingEventsWhileBlocked &&
        !(c->flags & (CLIENT_MASTER|CLIENT_SLAVE|CLIENT_BLOCKED)) &&
        io_threads_op == IO_THREADS_OP_IDLE)
    {
        listAddNodeHead(server.clients_pending_read,c);
        c->pending_read_list_node = listFirst(server.clients_pending_read);
        return 1;
    } else {
        return 0;
    }
}

/* When threaded I/O is also enabled for the reading + parsing side, the
 * readable handler will just put normal clients into a queue of clients to
 * process (instead of serving them synchronously). This function runs
 * the queue using the I/O threads, and process them in order to accumulate
 * the reads in the buffers, and also parse the first command available
 * rendering it in the client structures.
 * This function achieves thread safety using a fan-out -> fan-in paradigm:
 * Fan out: The main thread fans out work to the io-threads which block until
 * setIOPendingCount() is called with a value larger than 0 by the main thread.
 * Fan in: The main thread waits until getIOPendingCount() returns 0. Then
 * it can safely perform post-processing and return to normal synchronous
 * work. */
/**当还为读取+解析端启用线程 IO 时，可读处理程序只会将普通客户端放入客户端队列中进行处理（而不是同步地为它们提供服务）。
 * 此函数使用 IO 线程运行队列，并处理它们以累积缓冲区中的读取，并解析第一个可用的命令，将其呈现在客户端结构中。
 * 此函数使用扇出 -> 扇入范式实现线程安全：
 *   扇出：主线程将工作扇出到 io 线程，这些线程阻塞直到 setIOPendingCount() 被主线程以大于 0 的值调用。
 *   扇入：主线程一直等到getIOPendingCount()返回0，然后就可以安全的进行后期处理，恢复正常的同步工作。*/
int handleClientsWithPendingReadsUsingThreads(void) {
    if (!server.io_threads_active || !server.io_threads_do_reads) return 0;
    int processed = listLength(server.clients_pending_read);
    if (processed == 0) return 0;

    /* Distribute the clients across N different lists. */
    /**将客户端分布在 N 个不同的列表中。*/
    listIter li;
    listNode *ln;
    listRewind(server.clients_pending_read,&li);
    int item_id = 0;
    while((ln = listNext(&li))) {
        client *c = listNodeValue(ln);
        int target_id = item_id % server.io_threads_num;
        listAddNodeTail(io_threads_list[target_id],c);
        item_id++;
    }

    /* Give the start condition to the waiting threads, by setting the
     * start condition atomic var. */
    /**通过设置启动条件 atomic var 为等待线程提供启动条件。*/
    io_threads_op = IO_THREADS_OP_READ;
    for (int j = 1; j < server.io_threads_num; j++) {
        int count = listLength(io_threads_list[j]);
        setIOPendingCount(j, count);
    }

    /* Also use the main thread to process a slice of clients. */
    /**还使用主线程来处理一部分客户端。*/
    listRewind(io_threads_list[0],&li);
    while((ln = listNext(&li))) {
        client *c = listNodeValue(ln);
        readQueryFromClient(c->conn);
    }
    listEmpty(io_threads_list[0]);

    /* Wait for all the other threads to end their work. */
    /**等待所有其他线程结束它们的工作。*/
    while(1) {
        unsigned long pending = 0;
        for (int j = 1; j < server.io_threads_num; j++)
            pending += getIOPendingCount(j);
        if (pending == 0) break;
    }

    io_threads_op = IO_THREADS_OP_IDLE;

    /* Run the list of clients again to process the new buffers. */
    /**再次运行客户端列表以处理新缓冲区。*/
    while(listLength(server.clients_pending_read)) {
        ln = listFirst(server.clients_pending_read);
        client *c = listNodeValue(ln);
        listDelNode(server.clients_pending_read,ln);
        c->pending_read_list_node = NULL;

        serverAssert(!(c->flags & CLIENT_BLOCKED));

        if (beforeNextClient(c) == C_ERR) {
            /* If the client is no longer valid, we avoid
             * processing the client later. So we just go
             * to the next. */
            /**如果客户端不再有效，我们避免稍后处理客户端。所以我们只是去下一个。*/
            continue;
        }

        /* Once io-threads are idle we can update the client in the mem usage */
        /**一旦 io-threads 空闲，我们可以更新客户端的内存使用情况*/
        updateClientMemUsage(c);

        if (processPendingCommandAndInputBuffer(c) == C_ERR) {
            /* If the client is no longer valid, we avoid
             * processing the client later. So we just go
             * to the next. */
            /**如果客户端不再有效，我们避免稍后处理客户端。所以我们只是去下一个。*/
            continue;
        }

        /* We may have pending replies if a thread readQueryFromClient() produced
         * replies and did not put the client in pending write queue (it can't).
         */
        /**如果线程 readQueryFromClient() 产生了回复并且没有将客户端放入待处理的写入队列（它不能），我们可能会有待处理的回复。*/
        if (!(c->flags & CLIENT_PENDING_WRITE) && clientHasPendingReplies(c))
            putClientInPendingWriteQueue(c);
    }

    /* Update processed count on server */
    /**更新服务器上处理的计数*/
    server.stat_io_reads_processed += processed;

    return processed;
}

/* Returns the actual client eviction limit based on current configuration or
 * 0 if no limit. */
/**根据当前配置返回实际的客户端驱逐限制，如果没有限制，则返回 0。*/
size_t getClientEvictionLimit(void) {
    size_t maxmemory_clients_actual = SIZE_MAX;

    /* Handle percentage of maxmemory 处理 maxmemory 的百分比*/
    if (server.maxmemory_clients < 0 && server.maxmemory > 0) {
        unsigned long long maxmemory_clients_bytes = (unsigned long long)((double)server.maxmemory * -(double) server.maxmemory_clients / 100);
        if (maxmemory_clients_bytes <= SIZE_MAX)
            maxmemory_clients_actual = maxmemory_clients_bytes;
    }
    else if (server.maxmemory_clients > 0)
        maxmemory_clients_actual = server.maxmemory_clients;
    else
        return 0;

    /* Don't allow a too small maxmemory-clients to avoid cases where we can't communicate
     * at all with the server because of bad configuration */
    /**不要让 maxmemory-clients 太小，以避免由于配置错误而导致我们根本无法与服务器通信的情况*/
    if (maxmemory_clients_actual < 1024*128)
        maxmemory_clients_actual = 1024*128;

    return maxmemory_clients_actual;
}

void evictClients(void) {
    /* Start eviction from topmost bucket (largest clients) */
    /**从最顶层的存储桶开始驱逐（最大的客户）*/
    int curr_bucket = CLIENT_MEM_USAGE_BUCKETS-1;
    listIter bucket_iter;
    listRewind(server.client_mem_usage_buckets[curr_bucket].clients, &bucket_iter);
    size_t client_eviction_limit = getClientEvictionLimit();
    if (client_eviction_limit == 0)
        return;
    while (server.stat_clients_type_memory[CLIENT_TYPE_NORMAL] +
           server.stat_clients_type_memory[CLIENT_TYPE_PUBSUB] >= client_eviction_limit) {
        listNode *ln = listNext(&bucket_iter);
        if (ln) {
            client *c = ln->value;
            sds ci = catClientInfoString(sdsempty(),c);
            serverLog(LL_NOTICE, "Evicting client: %s", ci);
            freeClient(c);
            sdsfree(ci);
            server.stat_evictedclients++;
        } else {
            curr_bucket--;
            if (curr_bucket < 0) {
                serverLog(LL_WARNING, "Over client maxmemory after evicting all evictable clients");
                break;
            }
            listRewind(server.client_mem_usage_buckets[curr_bucket].clients, &bucket_iter);
        }
    }
}
