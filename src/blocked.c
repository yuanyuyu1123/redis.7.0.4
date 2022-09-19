/* blocked.c - generic support for blocking operations like BLPOP & WAIT.
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
 *
 * ---------------------------------------------------------------------------
 *
 * API:
 *
 * blockClient() set the CLIENT_BLOCKED flag in the client, and set the
 * specified block type 'btype' filed to one of BLOCKED_* macros.
 *
 * unblockClient() unblocks the client doing the following:
 * 1) It calls the btype-specific function to cleanup the state.
 * 2) It unblocks the client by unsetting the CLIENT_BLOCKED flag.
 * 3) It puts the client into a list of just unblocked clients that are
 *    processed ASAP in the beforeSleep() event loop callback, so that
 *    if there is some query buffer to process, we do it. This is also
 *    required because otherwise there is no 'readable' event fired, we
 *    already read the pending commands. We also set the CLIENT_UNBLOCKED
 *    flag to remember the client is in the unblocked_clients list.
 *
 * processUnblockedClients() is called inside the beforeSleep() function
 * to process the query buffer from unblocked clients and remove the clients
 * from the blocked_clients queue.
 *
 * replyToBlockedClientTimedOut() is called by the cron function when
 * a client blocked reaches the specified timeout (if the timeout is set
 * to 0, no timeout is processed).
 * It usually just needs to send a reply to the client.
 *
 * When implementing a new type of blocking operation, the implementation
 * should modify unblockClient() and replyToBlockedClientTimedOut() in order
 * to handle the btype-specific behavior of this two functions.
 * If the blocking operation waits for certain keys to change state, the
 * clusterRedirectBlockedClientIfNeeded() function should also be updated.
 */
/**
 * API：blockClient() 设置客户端中的 CLIENT_BLOCKED 标志，并将指定的块类型 'btype' 字段设置为 BLOCKED_ 宏之一。
 * unblockClient() 解除对客户端的阻塞，执行以下操作：
 *   1) 它调用特定于 btype 的函数来清理状态。
 *   2) 它通过取消设置 CLIENT_BLOCKED 标志来解除对客户端的阻塞。
 *   3) 它将客户端放入一个刚刚未阻塞的客户端列表中，这些客户端在 beforeSleep() 事件循环回调中被尽快处理，
 *     因此如果有一些查询缓冲区要处理，我们就会这样做。这也是必需的，因为否则不会触发“可读”事件，我们已经读取了待处理的命令。
 *     我们还设置了 CLIENT_UNBLOCKED 标志来记住客户端在 unblocked_clients 列表中。
 * 在 beforeSleep() 函数内部调用 processUnblockedClients() 以处理来自
 * 未阻塞客户端的查询缓冲区并将客户端从阻塞_clients 队列中删除。
 * 当被阻塞的客户端达到指定的超时时间（如果超时设置为 0，则不处理超时）时，cron 函数会调用 replyToBlockedClientTimedOut()。
 * 它通常只需要向客户端发送回复。当实现一种新类型的阻塞操作时，
 * 实现应该修改 unblockClient() 和 replyToBlockedClientTimedOut() 以处理这两个函数的 btype-specific 行为。
 * 如果阻塞操作等待某些键改变状态，则 clusterRedirectBlockedClientIfNeeded() 函数也应该更新。
 * */

#include "server.h"
#include "slowlog.h"
#include "latency.h"
#include "monotonic.h"

void serveClientBlockedOnList(client *receiver, robj *o, robj *key, robj *dstkey, redisDb *db, int wherefrom, int whereto, int *deleted);
int getListPositionFromObjectOrReply(client *c, robj *arg, int *position);

/* This structure represents the blocked key information that we store
 * in the client structure. Each client blocked on keys, has a
 * client->bpop.keys hash table. The keys of the hash table are Redis
 * keys pointers to 'robj' structures. The value is this structure.
 * The structure has two goals: firstly we store the list node that this
 * client uses to be listed in the database "blocked clients for this key"
 * list, so we can later unblock in O(1) without a list scan.
 * Secondly for certain blocking types, we have additional info. Right now
 * the only use for additional info we have is when clients are blocked
 * on streams, as we have to remember the ID it blocked for. */
/**
 * 这个结构代表了我们存储在客户端结构中的被阻止的关键信息。每个被键阻塞的客户端都有一个 client->bpop.keys 哈希表。
 * 哈希表的键是指向“robj”结构的 Redis 键指针。值就是这个结构。该结构有两个目标：
 *   首先，我们将这个客户端使用的列表节点存储在数据库“此键的阻塞客户端”列表中，以便稍后我们可以在 O(1) 中解除阻塞，而无需列表扫描。
 *   其次，对于某些阻塞类型，我们还有其他信息。
 * 现在，我们拥有的其他信息的唯一用途是客户端在流上被阻止时，因为我们必须记住它被阻止的 ID。
 * */
typedef struct bkinfo {
    listNode *listnode;     /* List node for db->blocking_keys[key] list.  db->blocking_keys[key] 列表的列表节点。*/
    streamID stream_id;     /* Stream ID if we blocked in a stream. 如果我们在流中阻塞，则流 ID。*/
} bkinfo;

/* Block a client for the specific operation type. Once the CLIENT_BLOCKED
 * flag is set client query buffer is not longer processed, but accumulated,
 * and will be processed when the client is unblocked. */
/**阻止特定操作类型的客户端。一旦设置了 CLIENT_BLOCKED 标志，客户端查询缓冲区就不再处理，
 * 而是累积，并且将在客户端解除阻塞时进行处理。*/
void blockClient(client *c, int btype) {
    /* Master client should never be blocked unless pause or module */
    //除非暂停或模块，否则不应阻塞主客户端
    serverAssert(!(c->flags & CLIENT_MASTER &&
                   btype != BLOCKED_MODULE &&
                   btype != BLOCKED_POSTPONE));

    c->flags |= CLIENT_BLOCKED;
    c->btype = btype;
    server.blocked_clients++;
    server.blocked_clients_by_type[btype]++;
    addClientToTimeoutTable(c);
    if (btype == BLOCKED_POSTPONE) {
        listAddNodeTail(server.postponed_clients, c);
        c->postponed_list_node = listLast(server.postponed_clients);
        /* Mark this client to execute its command */
        //标记此客户端以执行其命令
        c->flags |= CLIENT_PENDING_COMMAND;
    }
}

/* This function is called after a client has finished a blocking operation
 * in order to update the total command duration, log the command into
 * the Slow log if needed, and log the reply duration event if needed. */
//在客户端完成阻塞操作后调用此函数，以更新总命令持续时间，如果需要将命令记录到慢速日志中，并在需要时记录回复持续时间事件。
void updateStatsOnUnblock(client *c, long blocked_us, long reply_us, int had_errors){
    const ustime_t total_cmd_duration = c->duration + blocked_us + reply_us;
    c->lastcmd->microseconds += total_cmd_duration;
    if (had_errors)
        c->lastcmd->failed_calls++;
    if (server.latency_tracking_enabled)
        updateCommandLatencyHistogram(&(c->lastcmd->latency_histogram), total_cmd_duration*1000);
    /* Log the command into the Slow log if needed. */
    //如果需要，将命令记录到慢速日志中。
    slowlogPushCurrentCommand(c, c->lastcmd, total_cmd_duration);
    /* Log the reply duration event. */
    //记录回复持续时间事件。
    latencyAddSampleIfNeeded("command-unblocking",reply_us/1000);
}

/* This function is called in the beforeSleep() function of the event loop
 * in order to process the pending input buffer of clients that were
 * unblocked after a blocking operation. */
//此函数在事件循环的 beforeSleep() 函数中调用，以处理在阻塞操作后解除阻塞的客户端的待处理输入缓冲区。
void processUnblockedClients(void) {
    listNode *ln;
    client *c;

    while (listLength(server.unblocked_clients)) {
        ln = listFirst(server.unblocked_clients);
        serverAssert(ln != NULL);
        c = ln->value;
        listDelNode(server.unblocked_clients,ln);
        c->flags &= ~CLIENT_UNBLOCKED;

        /* Process remaining data in the input buffer, unless the client
         * is blocked again. Actually processInputBuffer() checks that the
         * client is not blocked before to proceed, but things may change and
         * the code is conceptually more correct this way. */
        /**处理输入缓冲区中剩余的数据，除非客户端再次被阻塞。
         * 实际上 processInputBuffer() 在继续之前检查客户端是否没有被阻塞，但是事情可能会发生变化，
         * 并且这种方式的代码在概念上更正确。*/
        if (!(c->flags & CLIENT_BLOCKED)) {
            /* If we have a queued command, execute it now. */
            //如果我们有一个排队的命令，现在就执行它。
            if (processPendingCommandAndInputBuffer(c) == C_ERR) {
                c = NULL;
            }
        }
        beforeNextClient(c);
    }
}

/* This function will schedule the client for reprocessing at a safe time.
 *
 * This is useful when a client was blocked for some reason (blocking operation,
 * CLIENT PAUSE, or whatever), because it may end with some accumulated query
 * buffer that needs to be processed ASAP:
 *
 * 1. When a client is blocked, its readable handler is still active.
 * 2. However in this case it only gets data into the query buffer, but the
 *    query is not parsed or executed once there is enough to proceed as
 *    usually (because the client is blocked... so we can't execute commands).
 * 3. When the client is unblocked, without this function, the client would
 *    have to write some query in order for the readable handler to finally
 *    call processQueryBuffer*() on it.
 * 4. With this function instead we can put the client in a queue that will
 *    process it for queries ready to be executed at a safe time.
 */
/**
 * 此功能将安排客户端在安全时间进行再处理。当客户端由于某种原因（阻塞操作、客户端暂停或其他）被阻塞时，这很有用，
 * 因为它可能以一些需要尽快处理的累积查询缓冲区结束：
 *   1. 当客户端被阻塞时，它的可读处理程序是仍然活跃。
 *   2. 但是在这种情况下，它只会将数据获取到查询缓冲区中，但是一旦有足够的数据继续进行，
 *     查询就不会被解析或执行（因为客户端被阻塞......所以我们无法执行命令）。
 *   3. 当客户端被解除阻塞时，如果没有这个函数，客户端将不得不编写一些查询，
 *     以便可读的处理程序最终调用它的 processQueryBuffer()。
 *   4. 使用这个函数，我们可以将客户端放入一个队列中，该队列将处理它以准备在安全时间执行的查询。
 * */
void queueClientForReprocessing(client *c) {
    /* The client may already be into the unblocked list because of a previous
     * blocking operation, don't add back it into the list multiple times. */
    //由于之前的阻塞操作，客户端可能已经进入未阻塞列表，请勿多次将其添加回列表中。
    if (!(c->flags & CLIENT_UNBLOCKED)) {
        c->flags |= CLIENT_UNBLOCKED;
        listAddNodeTail(server.unblocked_clients,c);
    }
}

/* Unblock a client calling the right function depending on the kind
 * of operation the client is blocking for. */
//根据客户端阻塞的操作类型，取消阻塞调用正确函数的客户端。
void unblockClient(client *c) {
    if (c->btype == BLOCKED_LIST ||
        c->btype == BLOCKED_ZSET ||
        c->btype == BLOCKED_STREAM) {
        unblockClientWaitingData(c);
    } else if (c->btype == BLOCKED_WAIT) {
        unblockClientWaitingReplicas(c);
    } else if (c->btype == BLOCKED_MODULE) {
        if (moduleClientIsBlockedOnKeys(c)) unblockClientWaitingData(c);
        unblockClientFromModule(c);
    } else if (c->btype == BLOCKED_POSTPONE) {
        listDelNode(server.postponed_clients,c->postponed_list_node);
        c->postponed_list_node = NULL;
    } else if (c->btype == BLOCKED_SHUTDOWN) {
        /* No special cleanup. 没有特别的清理。 */
    } else {
        serverPanic("Unknown btype in unblockClient().");
    }

    /* Reset the client for a new query since, for blocking commands
     * we do not do it immediately after the command returns (when the
     * client got blocked) in order to be still able to access the argument
     * vector from module callbacks and updateStatsOnUnblock. */
    /**重置客户端以进行新查询，因为对于阻塞命令，我们不会在命令返回后立即执行此操作（当客户端被阻塞时），
     * 以便仍然能够从模块回调和 updateStatsOnUnblock 访问参数向量。*/
    if (c->btype != BLOCKED_POSTPONE && c->btype != BLOCKED_SHUTDOWN) {
        freeClientOriginalArgv(c);
        resetClient(c);
    }

    /* Clear the flags, and put the client in the unblocked list so that
     * we'll process new commands in its query buffer ASAP. */
    //清除标志，并将客户端放入未阻止列表中，以便我们尽快处理其查询缓冲区中的新命令。
    server.blocked_clients--;
    server.blocked_clients_by_type[c->btype]--;
    c->flags &= ~CLIENT_BLOCKED;
    c->btype = BLOCKED_NONE;
    removeClientFromTimeoutTable(c);
    queueClientForReprocessing(c);
}

/* This function gets called when a blocked client timed out in order to
 * send it a reply of some kind. After this function is called,
 * unblockClient() will be called with the same client as argument. */
/**
 * 当被阻止的客户端超时时调用此函数，以便向其发送某种回复。调用此函数后，将使用相同的客户端作为参数调用 unblockClient()。
 * */
void replyToBlockedClientTimedOut(client *c) {
    if (c->btype == BLOCKED_LIST ||
        c->btype == BLOCKED_ZSET ||
        c->btype == BLOCKED_STREAM) {
        addReplyNullArray(c);
    } else if (c->btype == BLOCKED_WAIT) {
        addReplyLongLong(c,replicationCountAcksByOffset(c->bpop.reploffset));
    } else if (c->btype == BLOCKED_MODULE) {
        moduleBlockedClientTimedOut(c);
    } else {
        serverPanic("Unknown btype in replyToBlockedClientTimedOut().");
    }
}

/* If one or more clients are blocked on the SHUTDOWN command, this function
 * sends them an error reply and unblocks them. */
//如果一个或多个客户端在 SHUTDOWN 命令上被阻塞，该函数会向它们发送一个错误回复并解除阻塞。
void replyToClientsBlockedOnShutdown(void) {
    if (server.blocked_clients_by_type[BLOCKED_SHUTDOWN] == 0) return;
    listNode *ln;
    listIter li;
    listRewind(server.clients, &li);
    while((ln = listNext(&li))) {
        client *c = listNodeValue(ln);
        if (c->flags & CLIENT_BLOCKED && c->btype == BLOCKED_SHUTDOWN) {
            addReplyError(c, "Errors trying to SHUTDOWN. Check logs.");
            unblockClient(c);
        }
    }
}

/* Mass-unblock clients because something changed in the instance that makes
 * blocking no longer safe. For example clients blocked in list operations
 * in an instance which turns from master to slave is unsafe, so this function
 * is called when a master turns into a slave.
 *
 * The semantics is to send an -UNBLOCKED error to the client, disconnecting
 * it at the same time. */
/**大规模取消阻止客户端，因为实例中的某些更改使阻止不再安全。
 * 例如，在从主变为从的实例中，在列表操作中阻塞的客户端是不安全的，因此当主变为从时会调用此函数。
 *
 * 语义是向客户端发送一个 -UNBLOCKED 错误，同时断开它。*/
void disconnectAllBlockedClients(void) {
    listNode *ln;
    listIter li;

    listRewind(server.clients,&li);
    while((ln = listNext(&li))) {
        client *c = listNodeValue(ln);

        if (c->flags & CLIENT_BLOCKED) {
            /* POSTPONEd clients are an exception, when they'll be unblocked, the
             * command processing will start from scratch, and the command will
             * be either executed or rejected. (unlike LIST blocked clients for
             * which the command is already in progress in a way. */
            /**POSTPONed 客户端是一个例外，当它们被解除阻塞时，命令处理将从头开始，命令将被执行或拒绝。
             * （与 LIST 阻止的客户端不同，该客户端的命令已经在某种程度上正在进行中。*/
            if (c->btype == BLOCKED_POSTPONE)
                continue;

            addReplyError(c,
                "-UNBLOCKED force unblock from blocking operation, "
                "instance state changed (master -> replica?)");
            unblockClient(c);
            c->flags |= CLIENT_CLOSE_AFTER_REPLY;
        }
    }
}

/* Helper function for handleClientsBlockedOnKeys(). This function is called
 * when there may be clients blocked on a list key, and there may be new
 * data to fetch (the key is ready). */
//handleClientsBlockedOnKeys() 的辅助函数。当可能有客户端阻塞在列表键上时调用此函数，并且可能有新数据要获取（键已准备好）。
void serveClientsBlockedOnListKey(robj *o, readyList *rl) {
    /* Optimization: If no clients are in type BLOCKED_LIST,
     * we can skip this loop. */
    //优化：如果没有客户端属于 BLOCKED_LIST 类型，我们可以跳过这个循环。
    if (!server.blocked_clients_by_type[BLOCKED_LIST]) return;

    /* We serve clients in the same order they blocked for
     * this key, from the first blocked to the last. */
    //我们按照客户为此密钥阻止的相同顺序为客户提供服务，从第一个阻止到最后一个。
    dictEntry *de = dictFind(rl->db->blocking_keys,rl->key);
    if (de) {
        list *clients = dictGetVal(de);
        listNode *ln;
        listIter li;
        listRewind(clients,&li);

        while((ln = listNext(&li))) {
            client *receiver = listNodeValue(ln);
            if (receiver->btype != BLOCKED_LIST) continue;

            int deleted = 0;
            robj *dstkey = receiver->bpop.target;
            int wherefrom = receiver->bpop.blockpos.wherefrom;
            int whereto = receiver->bpop.blockpos.whereto;

            /* Protect receiver->bpop.target, that will be
             * freed by the next unblockClient()
             * call. */
            //保护接收器-> bpop.target，它将被下一次 unblockClient() 调用释放。
            if (dstkey) incrRefCount(dstkey);

            long long prev_error_replies = server.stat_total_error_replies;
            client *old_client = server.current_client;
            server.current_client = receiver;
            monotime replyTimer;
            elapsedStart(&replyTimer);
            serveClientBlockedOnList(receiver, o,
                                     rl->key, dstkey, rl->db,
                                     wherefrom, whereto,
                                     &deleted);
            updateStatsOnUnblock(receiver, 0, elapsedUs(replyTimer), server.stat_total_error_replies != prev_error_replies);
            unblockClient(receiver);
            afterCommand(receiver);
            server.current_client = old_client;

            if (dstkey) decrRefCount(dstkey);

            /* The list is empty and has been deleted. */
            //该列表为空，已被删除。
            if (deleted) break;
        }
    }
}

/* Helper function for handleClientsBlockedOnKeys(). This function is called
 * when there may be clients blocked on a sorted set key, and there may be new
 * data to fetch (the key is ready). */
/** handleClientsBlockedOnKeys() 的辅助函数。
 * 当可能有客户端阻塞在排序的集合键上，并且可能有新数据要获取（键已准备好）时，将调用此函数。*/
void serveClientsBlockedOnSortedSetKey(robj *o, readyList *rl) {
    /* Optimization: If no clients are in type BLOCKED_ZSET,
     * we can skip this loop. */
    /**优化：如果没有客户端属于 BLOCKED_ZSET 类型，我们可以跳过这个循环。*/
    if (!server.blocked_clients_by_type[BLOCKED_ZSET]) return;

    /* We serve clients in the same order they blocked for
     * this key, from the first blocked to the last. */
    /**我们按照客户为此密钥阻止的相同顺序为客户提供服务，从第一个阻止到最后一个。*/
    dictEntry *de = dictFind(rl->db->blocking_keys,rl->key);
    if (de) {
        list *clients = dictGetVal(de);
        listNode *ln;
        listIter li;
        listRewind(clients,&li);

        while((ln = listNext(&li))) {
            client *receiver = listNodeValue(ln);
            if (receiver->btype != BLOCKED_ZSET) continue;

            int deleted = 0;
            long llen = zsetLength(o);
            long count = receiver->bpop.count;
            int where = receiver->bpop.blockpos.wherefrom;
            int use_nested_array = (receiver->lastcmd &&
                                    receiver->lastcmd->proc == bzmpopCommand)
                                    ? 1 : 0;
            int reply_nil_when_empty = use_nested_array;

            long long prev_error_replies = server.stat_total_error_replies;
            client *old_client = server.current_client;
            server.current_client = receiver;
            monotime replyTimer;
            elapsedStart(&replyTimer);
            genericZpopCommand(receiver, &rl->key, 1, where, 1, count, use_nested_array, reply_nil_when_empty, &deleted);

            /* Replicate the command. */
            int argc = 2;
            robj *argv[3];
            argv[0] = where == ZSET_MIN ? shared.zpopmin : shared.zpopmax;
            argv[1] = rl->key;
            incrRefCount(rl->key);
            if (count != -1) {
                /* Replicate it as command with COUNT. */
                robj *count_obj = createStringObjectFromLongLong((count > llen) ? llen : count);
                argv[2] = count_obj;
                argc++;
            }
            alsoPropagate(receiver->db->id, argv, argc, PROPAGATE_AOF|PROPAGATE_REPL);
            decrRefCount(argv[1]);
            if (count != -1) decrRefCount(argv[2]);

            updateStatsOnUnblock(receiver, 0, elapsedUs(replyTimer), server.stat_total_error_replies != prev_error_replies);
            unblockClient(receiver);
            afterCommand(receiver);
            server.current_client = old_client;

            /* The zset is empty and has been deleted. */
            if (deleted) break;
        }
    }
}

/* Helper function for handleClientsBlockedOnKeys(). This function is called
 * when there may be clients blocked on a stream key, and there may be new
 * data to fetch (the key is ready). */
//handleClientsBlockedOnKeys() 的辅助函数。当可能有客户端阻塞在流密钥上时调用此函数，并且可能有新数据要获取（密钥已准备好）。
void serveClientsBlockedOnStreamKey(robj *o, readyList *rl) {
    /* Optimization: If no clients are in type BLOCKED_STREAM,
     * we can skip this loop. */
    /**优化：如果 BLOCKED_STREAM 类型中没有客户端，我们可以跳过此循环。*/
    if (!server.blocked_clients_by_type[BLOCKED_STREAM]) return;

    dictEntry *de = dictFind(rl->db->blocking_keys,rl->key);
    stream *s = o->ptr;

    /* We need to provide the new data arrived on the stream
     * to all the clients that are waiting for an offset smaller
     * than the current top item. */
    /**我们需要将到达流中的新数据提供给所有等待小于当前顶部项目的偏移量的客户端。*/
    if (de) {
        list *clients = dictGetVal(de);
        listNode *ln;
        listIter li;
        listRewind(clients,&li);

        while((ln = listNext(&li))) {
            client *receiver = listNodeValue(ln);
            if (receiver->btype != BLOCKED_STREAM) continue;
            bkinfo *bki = dictFetchValue(receiver->bpop.keys,rl->key);
            streamID *gt = &bki->stream_id;

            long long prev_error_replies = server.stat_total_error_replies;
            client *old_client = server.current_client;
            server.current_client = receiver;
            monotime replyTimer;
            elapsedStart(&replyTimer);

            /* If we blocked in the context of a consumer
             * group, we need to resolve the group and update the
             * last ID the client is blocked for: this is needed
             * because serving other clients in the same consumer
             * group will alter the "last ID" of the consumer
             * group, and clients blocked in a consumer group are
             * always blocked for the ">" ID: we need to deliver
             * only new messages and avoid unblocking the client
             * otherwise. */
            /**如果我们在消费者组的上下文中阻塞，我们需要解析组并更新客户端被阻塞的最后一个 ID：这是必需的，
             * 因为为同一消费者组中的其他客户端提供服务将改变消费者的“最后一个 ID”组，
             * 并且在消费者组中被阻止的客户端总是因为“>” ID而被阻止：我们只需要传递新消息并避免取消阻止客户端。*/
            streamCG *group = NULL;
            if (receiver->bpop.xread_group) {
                group = streamLookupCG(s,
                        receiver->bpop.xread_group->ptr);
                /* If the group was not found, send an error
                 * to the consumer. */
                /**如果未找到该组，则向消费者发送错误。*/
                if (!group) {
                    addReplyError(receiver,
                        "-NOGROUP the consumer group this client "
                        "was blocked on no longer exists");
                    goto unblock_receiver;
                } else {
                    *gt = group->last_id;
                }
            }

            if (streamCompareID(&s->last_id, gt) > 0) {
                streamID start = *gt;
                streamIncrID(&start);

                /* Lookup the consumer for the group, if any. */
                /**查找组的消费者（如果有）。*/
                streamConsumer *consumer = NULL;
                int noack = 0;

                if (group) {
                    noack = receiver->bpop.xread_group_noack;
                    sds name = receiver->bpop.xread_consumer->ptr;
                    consumer = streamLookupConsumer(group,name,SLC_DEFAULT);
                    if (consumer == NULL) {
                        consumer = streamCreateConsumer(group,name,rl->key,
                                                        rl->db->id,SCC_DEFAULT);
                        if (noack) {
                            streamPropagateConsumerCreation(receiver,rl->key,
                                                            receiver->bpop.xread_group,
                                                            consumer->name);
                        }
                    }
                }

                /* Emit the two elements sub-array consisting of
                 * the name of the stream and the data we
                 * extracted from it. Wrapped in a single-item
                 * array, since we have just one key. */
                /**发出由流的名称和我们从中提取的数据组成的两个元素子数组。包裹在一个单项数组中，因为我们只有一个键。*/
                if (receiver->resp == 2) {
                    addReplyArrayLen(receiver,1);
                    addReplyArrayLen(receiver,2);
                } else {
                    addReplyMapLen(receiver,1);
                }
                addReplyBulk(receiver,rl->key);

                streamPropInfo pi = {
                    rl->key,
                    receiver->bpop.xread_group
                };
                streamReplyWithRange(receiver,s,&start,NULL,
                                     receiver->bpop.xread_count,
                                     0, group, consumer, noack, &pi);
                /* Note that after we unblock the client, 'gt'
                 * and other receiver->bpop stuff are no longer
                 * valid, so we must do the setup above before
                 * the unblockClient call. */
                /**请注意，在我们解除对客户端的阻塞后，'gt' 和其他 receiver->bpop 内容不再有效，
                 * 因此我们必须在调用 unblockClient 之前进行上述设置。*/

unblock_receiver:
                updateStatsOnUnblock(receiver, 0, elapsedUs(replyTimer), server.stat_total_error_replies != prev_error_replies);
                unblockClient(receiver);
                afterCommand(receiver);
                server.current_client = old_client;
            }
        }
    }
}

/* Helper function for handleClientsBlockedOnKeys(). This function is called
 * in order to check if we can serve clients blocked by modules using
 * RM_BlockClientOnKeys(), when the corresponding key was signaled as ready:
 * our goal here is to call the RedisModuleBlockedClient reply() callback to
 * see if the key is really able to serve the client, and in that case,
 * unblock it. */
/**handleClientsBlockedOnKeys() 的辅助函数。
 * 调用此函数是为了检查我们是否可以使用 RM_BlockClientOnKeys() 为被模块阻塞的客户端提供服务，当相应的键被指示为就绪时：
 * 我们的目标是调用 RedisModuleBlockedClient 回复（）回调以查看该键是否真的能够为客户服务，在这种情况下，取消阻止它。*/
void serveClientsBlockedOnKeyByModule(readyList *rl) {
    /* Optimization: If no clients are in type BLOCKED_MODULE,
     * we can skip this loop. */
    /**优化：如果没有客户端是 BLOCKED_MODULE 类型的，我们可以跳过这个循环。*/
    if (!server.blocked_clients_by_type[BLOCKED_MODULE]) return;

    /* We serve clients in the same order they blocked for
     * this key, from the first blocked to the last. */
    /**我们按照客户为此密钥阻止的相同顺序为客户提供服务，从第一个阻止到最后一个。*/
    dictEntry *de = dictFind(rl->db->blocking_keys,rl->key);
    if (de) {
        list *clients = dictGetVal(de);
        listNode *ln;
        listIter li;
        listRewind(clients,&li);

        while((ln = listNext(&li))) {
            client *receiver = listNodeValue(ln);
            if (receiver->btype != BLOCKED_MODULE) continue;

            /* Note that if *this* client cannot be served by this key,
             * it does not mean that another client that is next into the
             * list cannot be served as well: they may be blocked by
             * different modules with different triggers to consider if a key
             * is ready or not. This means we can't exit the loop but need
             * to continue after the first failure. */
            /**请注意，如果此客户端无法通过此密钥提供服务，这并不意味着列表中的下一个客户端也无法提供服务：
             * 它们可能被具有不同触发器的不同模块阻止，以考虑密钥是否准备好.这意味着我们不能退出循环，
             * 但需要在第一次失败后继续。*/
            long long prev_error_replies = server.stat_total_error_replies;
            client *old_client = server.current_client;
            server.current_client = receiver;
            monotime replyTimer;
            elapsedStart(&replyTimer);
            if (!moduleTryServeClientBlockedOnKey(receiver, rl->key)) continue;
            updateStatsOnUnblock(receiver, 0, elapsedUs(replyTimer), server.stat_total_error_replies != prev_error_replies);
            moduleUnblockClient(receiver);
            afterCommand(receiver);
            server.current_client = old_client;
        }
    }
}

/* Helper function for handleClientsBlockedOnKeys(). This function is called
 * when there may be clients blocked, via XREADGROUP, on an existing stream which
 * was deleted. We need to unblock the clients in that case.
 * The idea is that a client that is blocked via XREADGROUP is different from
 * any other blocking type in the sense that it depends on the existence of both
 * the key and the group. Even if the key is deleted and then revived with XADD
 * it won't help any clients blocked on XREADGROUP because the group no longer
 * exist, so they would fail with -NOGROUP anyway.
 * The conclusion is that it's better to unblock these client (with error) upon
 * the deletion of the key, rather than waiting for the first XADD. */
/** handleClientsBlockedOnKeys() 的辅助函数。当可能有客户端通过 XREADGROUP 在已删除的现有流上被阻止时，将调用此函数。
 * 在这种情况下，我们需要解除对客户端的阻止。这个想法是，通过 XREADGROUP 阻止的客户端不同于任何其他阻止类型，
 * 因为它取决于密钥和组的存在。即使密钥被删除然后使用 XADD 恢复它也不会帮助任何在 XREADGROUP 上阻止的客户端，
 * 因为该组不再存在，因此无论如何它们都会因 -NOGROUP 而失败。
 * 结论是最好在删除密钥时解除阻止这些客户端（有错误），而不是等待第一个 XADD。*/
void unblockDeletedStreamReadgroupClients(readyList *rl) {
    /* Optimization: If no clients are in type BLOCKED_STREAM,
     * we can skip this loop. */
    /**优化：如果 BLOCKED_STREAM 类型中没有客户端，我们可以跳过此循环。*/
    if (!server.blocked_clients_by_type[BLOCKED_STREAM]) return;

    /* We serve clients in the same order they blocked for
     * this key, from the first blocked to the last. */
    /**我们按照客户为此密钥阻止的相同顺序为客户提供服务，从第一个阻止到最后一个。*/
    dictEntry *de = dictFind(rl->db->blocking_keys,rl->key);
    if (de) {
        list *clients = dictGetVal(de);
        listNode *ln;
        listIter li;
        listRewind(clients,&li);

        while((ln = listNext(&li))) {
            client *receiver = listNodeValue(ln);
            if (receiver->btype != BLOCKED_STREAM || !receiver->bpop.xread_group)
                continue;

            long long prev_error_replies = server.stat_total_error_replies;
            client *old_client = server.current_client;
            server.current_client = receiver;
            monotime replyTimer;
            elapsedStart(&replyTimer);
            addReplyError(receiver, "-UNBLOCKED the stream key no longer exists");
            updateStatsOnUnblock(receiver, 0, elapsedUs(replyTimer), server.stat_total_error_replies != prev_error_replies);
            unblockClient(receiver);
            afterCommand(receiver);
            server.current_client = old_client;
        }
    }
}

/* This function should be called by Redis every time a single command,
 * a MULTI/EXEC block, or a Lua script, terminated its execution after
 * being called by a client. It handles serving clients blocked in
 * lists, streams, and sorted sets, via a blocking commands.
 *
 * All the keys with at least one client blocked that received at least
 * one new element via some write operation are accumulated into
 * the server.ready_keys list. This function will run the list and will
 * serve clients accordingly. Note that the function will iterate again and
 * again as a result of serving BLMOVE we can have new blocking clients
 * to serve because of the PUSH side of BLMOVE.
 *
 * This function is normally "fair", that is, it will server clients
 * using a FIFO behavior. However this fairness is violated in certain
 * edge cases, that is, when we have clients blocked at the same time
 * in a sorted set and in a list, for the same key (a very odd thing to
 * do client side, indeed!). Because mismatching clients (blocking for
 * a different type compared to the current key type) are moved in the
 * other side of the linked list. However as long as the key starts to
 * be used only for a single type, like virtually any Redis application will
 * do, the function is already fair. */
/**每次单个命令、MULTIEXEC 块或 Lua 脚本在被客户端调用后终止其执行时，Redis 都应调用此函数。
 * 它通过阻塞命令处理在列表、流和排序集中被阻塞的服务客户端。
 *
 * 至少有一个客户端被阻塞且通过某个写入操作接收到至少一个新元素的所有密钥都被累积到 server.ready_keys 列表中。
 * 此函数将运行列表并相应地为客户端提供服务。
 * 请注意，由于服务 BLMOVE，该函数将一次又一次地迭代，由于 BLMOVE 的 PUSH 端，我们可以有新的阻塞客户端来服务。
 *
 * 这个函数通常是“公平的”，也就是说，它将使用 FIFO 行为服务客户端。
 * 然而，这种公平性在某些边缘情况下被违反了，也就是说，当我们让客户端在排序集和列表中同时阻塞时，
 * 对于相同的键（在客户端做一件非常奇怪的事情，确实！）。
 * 因为不匹配的客户端（阻止与当前键类型相比的不同类型）被移动到链表的另一侧。
 * 但是，只要密钥开始仅用于单一类型，就像几乎任何 Redis 应用程序一样，该功能就已经是公平的。*/
void handleClientsBlockedOnKeys(void) {
    /* This function is called only when also_propagate is in its basic state
     * (i.e. not from call(), module context, etc.) */
    /**仅当also_propagate 处于其基本状态时才调用此函数（即不是来自 call()、模块上下文等）*/
    serverAssert(server.also_propagate.numops == 0);
    server.core_propagates = 1;

    while(listLength(server.ready_keys) != 0) {
        list *l;

        /* Point server.ready_keys to a fresh list and save the current one
         * locally. This way as we run the old list we are free to call
         * signalKeyAsReady() that may push new elements in server.ready_keys
         * when handling clients blocked into BLMOVE. */
        /**将 server.ready_keys 指向一个新列表并将当前列表保存在本地。
         * 这样，当我们运行旧列表时，我们可以自由地调用 signalKeyAsReady()，
         * 这可能会在处理被阻止进入 BLMOVE 的客户端时推送 server.ready_keys 中的新元素。*/
        l = server.ready_keys;
        server.ready_keys = listCreate();

        while(listLength(l) != 0) {
            listNode *ln = listFirst(l);
            readyList *rl = ln->value;

            /* First of all remove this key from db->ready_keys so that
             * we can safely call signalKeyAsReady() against this key. */
            /**首先从 db->ready_keys 中移除这个键，这样我们就可以安全地调用 signalKeyAsReady() 来处理这个键。*/
            dictDelete(rl->db->ready_keys,rl->key);

            /* Even if we are not inside call(), increment the call depth
             * in order to make sure that keys are expired against a fixed
             * reference time, and not against the wallclock time. This
             * way we can lookup an object multiple times (BLMOVE does
             * that) without the risk of it being freed in the second
             * lookup, invalidating the first one.
             * See https://github.com/redis/redis/pull/6554. */
            /**即使我们不在 call() 内部，也要增加调用深度，以确保密钥在固定参考时间而不是挂钟时间过期。
             * 这样，我们可以多次查找一个对象（BLMOVE 会这样做），而不会在第二次查找中释放它，从而使第一次无效。
             * 见 https:github.comredisredispull6554。*/
            server.fixed_time_expire++;
            updateCachedTime(0);

            /* Serve clients blocked on the key. 为密钥上被阻止的客户提供服务。*/
            robj *o = lookupKeyReadWithFlags(rl->db, rl->key, LOOKUP_NONOTIFY | LOOKUP_NOSTATS);
            if (o != NULL) {
                int objtype = o->type;
                if (objtype == OBJ_LIST)
                    serveClientsBlockedOnListKey(o,rl);
                else if (objtype == OBJ_ZSET)
                    serveClientsBlockedOnSortedSetKey(o,rl);
                else if (objtype == OBJ_STREAM)
                    serveClientsBlockedOnStreamKey(o,rl);
                /* We want to serve clients blocked on module keys
                 * regardless of the object type: we don't know what the
                 * module is trying to accomplish right now. */
                /**无论对象类型如何，我们都希望为模块键上阻塞的客户端提供服务：我们不知道模块现在试图完成什么。*/
                serveClientsBlockedOnKeyByModule(rl);
                /* If we have XREADGROUP clients blocked on this key, and
                 * the key is not a stream, it must mean that the key was
                 * overwritten by either SET or something like
                 * (MULTI, DEL key, SADD key e, EXEC).
                 * In this case we need to unblock all these clients. */
                /**如果我们在这个键上阻止了 XREADGROUP 客户端，并且该键不是流，
                 * 则它必须意味着该键被 SET 或类似的东西（MULTI、DEL 键、SADD 键 e、EXEC）覆盖。
                 * 在这种情况下，我们需要取消阻止所有这些客户端。*/
                 if (objtype != OBJ_STREAM)
                     unblockDeletedStreamReadgroupClients(rl);
            } else {
                /* Unblock all XREADGROUP clients of this deleted key */
                /**取消阻止此已删除密钥的所有 XREADGROUP 客户端*/
                unblockDeletedStreamReadgroupClients(rl);
                /* Edge case: If lookupKeyReadWithFlags decides to expire the key we have to
                 * take care of the propagation here, because afterCommand wasn't called */
                /**边缘情况：如果 lookupKeyReadWithFlags 决定使密钥过期，我们必须在这里处理传播，因为没有调用 afterCommand*/
                if (server.also_propagate.numops > 0)
                    propagatePendingCommands();
            }
            server.fixed_time_expire--;

            /* Free this item. */
            decrRefCount(rl->key);
            zfree(rl);
            listDelNode(l,ln);
        }
        listRelease(l); /* We have the new list on place at this point. 我们现在有新的清单。*/
    }

    serverAssert(server.core_propagates); /* This function should not be re-entrant 这个函数不应该是可重入的*/

    server.core_propagates = 0;
}

/* This is how the current blocking lists/sorted sets/streams work, we use
 * BLPOP as example, but the concept is the same for other list ops, sorted
 * sets and XREAD.
 * - If the user calls BLPOP and the key exists and contains a non empty list
 *   then LPOP is called instead. So BLPOP is semantically the same as LPOP
 *   if blocking is not required.
 * - If instead BLPOP is called and the key does not exists or the list is
 *   empty we need to block. In order to do so we remove the notification for
 *   new data to read in the client socket (so that we'll not serve new
 *   requests if the blocking request is not served). Also we put the client
 *   in a dictionary (db->blocking_keys) mapping keys to a list of clients
 *   blocking for this keys.
 * - If a PUSH operation against a key with blocked clients waiting is
 *   performed, we mark this key as "ready", and after the current command,
 *   MULTI/EXEC block, or script, is executed, we serve all the clients waiting
 *   for this list, from the one that blocked first, to the last, accordingly
 *   to the number of elements we have in the ready list.
 */

/* Set a client in blocking mode for the specified key (list, zset or stream),
 * with the specified timeout. The 'type' argument is BLOCKED_LIST,
 * BLOCKED_ZSET or BLOCKED_STREAM depending on the kind of operation we are
 * waiting for an empty key in order to awake the client. The client is blocked
 * for all the 'numkeys' keys as in the 'keys' argument. When we block for
 * stream keys, we also provide an array of streamID structures: clients will
 * be unblocked only when items with an ID greater or equal to the specified
 * one is appended to the stream.
 *
 * 'count' for those commands that support the optional count argument.
 * Otherwise the value is 0. */
/**
 * 这就是当前阻塞列表排序集流的工作方式，我们以 BLPOP 为例，但其他列表操作、排序集和 XREAD 的概念相同。
 * - 如果用户调用 BLPOP 并且密钥存在并且包含非空列表，则改为调用 LPOP。因此，如果不需要阻塞，BLPOP 在语义上与 LPOP 相同。
 * - 如果调用了 BLPOP 并且密钥不存在或列表为空，我们需要阻止。
 *   为了做到这一点，我们删除了在客户端套接字中读取新数据的通知（这样如果阻塞请求没有被服务，我们就不会服务新请求）。
 *   我们还将客户端放在字典（db->blocking_keys）中，将键映射到阻止此键的客户端列表。
 * - 如果对阻塞客户端等待的键执行 PUSH 操作，我们将此键标记为“就绪”，并且在执行当前命令、MULTIEXEC 块或脚本后，
 *   我们为所有等待此列表的客户端提供服务，从根据我们在就绪列表中的元素数量，第一个阻塞到最后一个阻塞。
 *
 * 为指定的键（列表、zset 或流）设置阻塞模式的客户端，并指定超时。
 * 'type' 参数是 BLOCKED_LIST、BLOCKED_ZSET 或 BLOCKED_STREAM，具体取决于我们等待空键以唤醒客户端的操作类型。
 * 客户端被阻止所有 'numkeys' 键，如 'keys' 参数。当我们为流键阻塞时，我们还提供了一个 streamID 结构数组：
 * 只有当 ID 大于或等于指定 ID 的项目附加到流中时，客户端才会被解除阻塞。
 *
 * 'count' 表示那些支持可选计数参数的命令。否则值为 0。
 * */
void blockForKeys(client *c, int btype, robj **keys, int numkeys, long count, mstime_t timeout, robj *target, struct blockPos *blockpos, streamID *ids) {
    dictEntry *de;
    list *l;
    int j;

    c->bpop.count = count;
    c->bpop.timeout = timeout;
    c->bpop.target = target;

    if (blockpos != NULL) c->bpop.blockpos = *blockpos;

    if (target != NULL) incrRefCount(target);

    for (j = 0; j < numkeys; j++) {
        /* Allocate our bkinfo structure, associated to each key the client
         * is blocked for. */
        /**分配我们的 bkinfo 结构，与客户端被阻止的每个键相关联。*/
        bkinfo *bki = zmalloc(sizeof(*bki));
        if (btype == BLOCKED_STREAM)
            bki->stream_id = ids[j];

        /* If the key already exists in the dictionary ignore it. */
        /**如果该键已存在于字典中，则忽略它。*/
        if (dictAdd(c->bpop.keys,keys[j],bki) != DICT_OK) {
            zfree(bki);
            continue;
        }
        incrRefCount(keys[j]);

        /* And in the other "side", to map keys -> clients */
        /**而在另一个“方面”，映射键 - >客户端*/
        de = dictFind(c->db->blocking_keys,keys[j]);
        if (de == NULL) {
            int retval;

            /* For every key we take a list of clients blocked for it */
            /**对于每个键，我们都会列出一个被阻止的客户端列表*/
            l = listCreate();
            retval = dictAdd(c->db->blocking_keys,keys[j],l);
            incrRefCount(keys[j]);
            serverAssertWithInfo(c,keys[j],retval == DICT_OK);
        } else {
            l = dictGetVal(de);
        }
        listAddNodeTail(l,c);
        bki->listnode = listLast(l);
    }
    blockClient(c,btype);
}

/* Unblock a client that's waiting in a blocking operation such as BLPOP.
 * You should never call this function directly, but unblockClient() instead. */
//取消阻止正在等待阻塞操作（例如 BLPOP）的客户端。你不应该直接调用这个函数，而是 unblockClient() 。
void unblockClientWaitingData(client *c) {
    dictEntry *de;
    dictIterator *di;
    list *l;

    serverAssertWithInfo(c,NULL,dictSize(c->bpop.keys) != 0);
    di = dictGetIterator(c->bpop.keys);
    /* The client may wait for multiple keys, so unblock it for every key. */
    /**客户端可能会等待多个键，因此为每个键解除阻塞。*/
    while((de = dictNext(di)) != NULL) {
        robj *key = dictGetKey(de);
        bkinfo *bki = dictGetVal(de);

        /* Remove this client from the list of clients waiting for this key. */
        /**从等待此密钥的客户端列表中删除此客户端。*/
        l = dictFetchValue(c->db->blocking_keys,key);
        serverAssertWithInfo(c,key,l != NULL);
        listDelNode(l,bki->listnode);
        /* If the list is empty we need to remove it to avoid wasting memory */
        /**如果列表为空，我们需要将其删除以避免浪费内存*/
        if (listLength(l) == 0)
            dictDelete(c->db->blocking_keys,key);
    }
    dictReleaseIterator(di);

    /* Cleanup the client structure */
    dictEmpty(c->bpop.keys,NULL);
    if (c->bpop.target) {
        decrRefCount(c->bpop.target);
        c->bpop.target = NULL;
    }
    if (c->bpop.xread_group) {
        decrRefCount(c->bpop.xread_group);
        decrRefCount(c->bpop.xread_consumer);
        c->bpop.xread_group = NULL;
        c->bpop.xread_consumer = NULL;
    }
}

static int getBlockedTypeByType(int type) {
    switch (type) {
        case OBJ_LIST: return BLOCKED_LIST;
        case OBJ_ZSET: return BLOCKED_ZSET;
        case OBJ_MODULE: return BLOCKED_MODULE;
        case OBJ_STREAM: return BLOCKED_STREAM;
        default: return BLOCKED_NONE;
    }
}

/* If the specified key has clients blocked waiting for list pushes, this
 * function will put the key reference into the server.ready_keys list.
 * Note that db->ready_keys is a hash table that allows us to avoid putting
 * the same key again and again in the list in case of multiple pushes
 * made by a script or in the context of MULTI/EXEC.
 *
 * The list will be finally processed by handleClientsBlockedOnKeys() */
/**如果指定的键有客户端阻塞等待列表推送，此函数会将键引用放入 server.ready_keys 列表中。
 * 请注意，db->ready_keys 是一个哈希表，它允许我们避免在脚本或 MULTI/EXEC 的上下文中多次推送的情况下
 * 一次又一次地将相同的密钥放入列表中。
 *
 * 该列表将最终由 handleClientsBlockedOnKeys() 处理*/
void signalKeyAsReady(redisDb *db, robj *key, int type) {
    readyList *rl;

    /* Quick returns. */
    int btype = getBlockedTypeByType(type);
    if (btype == BLOCKED_NONE) {
        /* The type can never block. */
        return;
    }
    if (!server.blocked_clients_by_type[btype] &&
        !server.blocked_clients_by_type[BLOCKED_MODULE]) {
        /* No clients block on this type. Note: Blocked modules are represented
         * by BLOCKED_MODULE, even if the intention is to wake up by normal
         * types (list, zset, stream), so we need to check that there are no
         * blocked modules before we do a quick return here. */
        /**没有客户端阻止此类型。注意：被阻塞的模块用 BLOCKED_MODULE 表示，
         * 即使意图是通过普通类型（list、zset、stream）唤醒，所以我们需要在此处快速返回之前检查没有阻塞的模块。*/
        return;
    }

    /* No clients blocking for this key? No need to queue it. */
    /**没有客户端阻止此密钥？无需排队。*/
    if (dictFind(db->blocking_keys,key) == NULL) return;

    /* Key was already signaled? No need to queue it again. */
    /**钥匙已经发出信号了？无需再次排队。*/
    if (dictFind(db->ready_keys,key) != NULL) return;

    /* Ok, we need to queue this key into server.ready_keys. */
    /**好的，我们需要将此密钥排队到 server.ready_keys 中。*/
    rl = zmalloc(sizeof(*rl));
    rl->key = key;
    rl->db = db;
    incrRefCount(key);
    listAddNodeTail(server.ready_keys,rl);

    /* We also add the key in the db->ready_keys dictionary in order
     * to avoid adding it multiple times into a list with a simple O(1)
     * check. */
    /**我们还在 db->ready_keys 字典中添加键，以避免通过简单的 O(1) 检查将其多次添加到列表中。*/
    incrRefCount(key);
    serverAssert(dictAdd(db->ready_keys,key,NULL) == DICT_OK);
}
