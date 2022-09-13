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

/* ================================ MULTI/EXEC ============================== */

/* Client state initialization for MULTI/EXEC */
//MULTI/EXEC 的客户端状态初始化
void initClientMultiState(client *c) {
    c->mstate.commands = NULL;
    c->mstate.count = 0;
    c->mstate.cmd_flags = 0;
    c->mstate.cmd_inv_flags = 0;
    c->mstate.argv_len_sums = 0;
    c->mstate.alloc_count = 0;
}

/* Release all the resources associated with MULTI/EXEC state */
//释放所有与 MULTIEXEC 状态相关的资源
void freeClientMultiState(client *c) {
    int j;

    for (j = 0; j < c->mstate.count; j++) {
        int i;
        multiCmd *mc = c->mstate.commands+j;

        for (i = 0; i < mc->argc; i++)
            decrRefCount(mc->argv[i]);
        zfree(mc->argv);
    }
    zfree(c->mstate.commands);
}

/* Add a new command into the MULTI commands queue */
//将新命令添加到 MULTI 命令队列
void queueMultiCommand(client *c, uint64_t cmd_flags) {
    multiCmd *mc;

    /* No sense to waste memory if the transaction is already aborted.
     * this is useful in case client sends these in a pipeline, or doesn't
     * bother to read previous responses and didn't notice the multi was already
     * aborted. */
    /**
     * 如果事务已经中止，那么浪费内存是没有意义的。
     * 如果客户端在管道中发送这些，或者不费心阅读以前的响应并且没有注意到多路已经中止，这很有用。
     * */
    if (c->flags & (CLIENT_DIRTY_CAS|CLIENT_DIRTY_EXEC))
        return;
    if (c->mstate.count == 0) {
        /* If a client is using multi/exec, assuming it is used to execute at least
         * two commands. Hence, creating by default size of 2. */
        //如果客户端使用 multiexec，假设它用于执行至少两个命令。因此，默认创建大小为 2。
        c->mstate.commands = zmalloc(sizeof(multiCmd)*2);
        c->mstate.alloc_count = 2;
    }
    if (c->mstate.count == c->mstate.alloc_count) {
        c->mstate.alloc_count = c->mstate.alloc_count < INT_MAX/2 ? c->mstate.alloc_count*2 : INT_MAX;
        c->mstate.commands = zrealloc(c->mstate.commands, sizeof(multiCmd)*(c->mstate.alloc_count));
    }
    mc = c->mstate.commands+c->mstate.count;
    mc->cmd = c->cmd;
    mc->argc = c->argc;
    mc->argv = c->argv;
    mc->argv_len = c->argv_len;

    c->mstate.count++;
    c->mstate.cmd_flags |= cmd_flags;
    c->mstate.cmd_inv_flags |= ~cmd_flags;
    c->mstate.argv_len_sums += c->argv_len_sum + sizeof(robj*)*c->argc;

    /* Reset the client's args since we copied them into the mstate and shouldn't
     * reference them from c anymore. */
    //重置客户端的参数，因为我们将它们复制到 mstate 中，并且不应再从 c 引用它们。
    c->argv = NULL;
    c->argc = 0;
    c->argv_len_sum = 0;
    c->argv_len = 0;
}

void discardTransaction(client *c) {
    freeClientMultiState(c);
    initClientMultiState(c);
    c->flags &= ~(CLIENT_MULTI|CLIENT_DIRTY_CAS|CLIENT_DIRTY_EXEC);
    unwatchAllKeys(c);
}

/* Flag the transaction as DIRTY_EXEC so that EXEC will fail.
 * Should be called every time there is an error while queueing a command. */
//将事务标记为 DIRTY_EXEC，以便 EXEC 失败。每次在排队命令时出现错误时都应该调用。
void flagTransaction(client *c) {
    if (c->flags & CLIENT_MULTI)
        c->flags |= CLIENT_DIRTY_EXEC;
}

void multiCommand(client *c) {
    if (c->flags & CLIENT_MULTI) {
        addReplyError(c,"MULTI calls can not be nested");
        return;
    }
    c->flags |= CLIENT_MULTI;

    addReply(c,shared.ok);
}

void discardCommand(client *c) {
    if (!(c->flags & CLIENT_MULTI)) {
        addReplyError(c,"DISCARD without MULTI");
        return;
    }
    discardTransaction(c);
    addReply(c,shared.ok);
}

/* Aborts a transaction, with a specific error message.
 * The transaction is always aborted with -EXECABORT so that the client knows
 * the server exited the multi state, but the actual reason for the abort is
 * included too.
 * Note: 'error' may or may not end with \r\n. see addReplyErrorFormat. */
/**
 * 中止事务，并带有特定的错误消息。事务总是用 -EXECABORT 中止，以便客户端知道服务器退出了多状态，但也包括中止的实际原因。
 * 注意：“错误”可能以也可能不以 \r\n 结尾。请参阅 addReplyErrorFormat。
 * */
void execCommandAbort(client *c, sds error) {
    discardTransaction(c);

    if (error[0] == '-') error++;
    addReplyErrorFormat(c, "-EXECABORT Transaction discarded because of: %s", error);

    /* Send EXEC to clients waiting data from MONITOR. We did send a MULTI
     * already, and didn't send any of the queued commands, now we'll just send
     * EXEC so it is clear that the transaction is over. */
    //向等待来自 MONITOR 的数据的客户端发送 EXEC。我们确实已经发送了一个 MULTI，
    // 并且没有发送任何排队的命令，现在我们只发送 EXEC，因此很明显事务已经结束。
    replicationFeedMonitors(c,server.monitors,c->db->id,c->argv,c->argc);
}

void execCommand(client *c) {
    int j;
    robj **orig_argv;
    int orig_argc, orig_argv_len;
    struct redisCommand *orig_cmd;

    if (!(c->flags & CLIENT_MULTI)) {
        addReplyError(c,"EXEC without MULTI");
        return;
    }

    /* EXEC with expired watched key is disallowed*/
    //不允许使用过期的监视密钥执行 EXEC
    if (isWatchedKeyExpired(c)) {
        c->flags |= (CLIENT_DIRTY_CAS);
    }

    /* Check if we need to abort the EXEC because:
     * 1) Some WATCHed key was touched.
     * 2) There was a previous error while queueing commands.
     * A failed EXEC in the first case returns a multi bulk nil object
     * (technically it is not an error but a special behavior), while
     * in the second an EXECABORT error is returned. */
    /**
     * 检查我们是否需要中止执行，因为：
     *   1) 触摸了某个 WATCHed 键。
     *   2) 排队命令时出现先前的错误。
     * 在第一种情况下，失败的 EXEC 返回一个多批量 nil 对象（从技术上讲，它不是错误，而是一种特殊行为），
     * 而在第二种情况下，则返回 EXECABORT 错误。
     * */
    if (c->flags & (CLIENT_DIRTY_CAS | CLIENT_DIRTY_EXEC)) {
        if (c->flags & CLIENT_DIRTY_EXEC) {
            addReplyErrorObject(c, shared.execaborterr);
        } else {
            addReply(c, shared.nullarray[c->resp]);
        }

        discardTransaction(c);
        return;
    }

    uint64_t old_flags = c->flags;

    /* we do not want to allow blocking commands inside multi */
    //我们不想在 multi 中允许阻塞命令
    c->flags |= CLIENT_DENY_BLOCKING;

    /* Exec all the queued commands  执行所有排队的命令*/
    unwatchAllKeys(c); /* Unwatch ASAP otherwise we'll waste CPU cycles 尽快取消观看，否则我们将浪费 CPU 周期*/

    server.in_exec = 1;

    orig_argv = c->argv;
    orig_argv_len = c->argv_len;
    orig_argc = c->argc;
    orig_cmd = c->cmd;
    addReplyArrayLen(c,c->mstate.count);
    for (j = 0; j < c->mstate.count; j++) {
        c->argc = c->mstate.commands[j].argc;
        c->argv = c->mstate.commands[j].argv;
        c->argv_len = c->mstate.commands[j].argv_len;
        c->cmd = c->realcmd = c->mstate.commands[j].cmd;

        /* ACL permissions are also checked at the time of execution in case
         * they were changed after the commands were queued. */
        //ACL 权限也会在执行时检查，以防在命令排队后更改。
        int acl_errpos;
        int acl_retval = ACLCheckAllPerm(c,&acl_errpos);
        if (acl_retval != ACL_OK) {
            char *reason;
            switch (acl_retval) {
            case ACL_DENIED_CMD:
                reason = "no permission to execute the command or subcommand";
                break;
            case ACL_DENIED_KEY:
                reason = "no permission to touch the specified keys";
                break;
            case ACL_DENIED_CHANNEL:
                reason = "no permission to access one of the channels used "
                         "as arguments";
                break;
            default:
                reason = "no permission";
                break;
            }
            addACLLogEntry(c,acl_retval,ACL_LOG_CTX_MULTI,acl_errpos,NULL,NULL);
            addReplyErrorFormat(c,
                "-NOPERM ACLs rules changed between the moment the "
                "transaction was accumulated and the EXEC call. "
                "This command is no longer allowed for the "
                "following reason: %s", reason);
        } else {
            if (c->id == CLIENT_ID_AOF)
                call(c,CMD_CALL_NONE);
            else
                call(c,CMD_CALL_FULL);

            serverAssert((c->flags & CLIENT_BLOCKED) == 0);
        }

        /* Commands may alter argc/argv, restore mstate. */
        //命令可能会改变 argcargv，恢复 mstate。
        c->mstate.commands[j].argc = c->argc;
        c->mstate.commands[j].argv = c->argv;
        c->mstate.commands[j].argv_len = c->argv_len;
        c->mstate.commands[j].cmd = c->cmd;
    }

    // restore old DENY_BLOCKING value 恢复旧的 DENY_BLOCKING 值
    if (!(old_flags & CLIENT_DENY_BLOCKING))
        c->flags &= ~CLIENT_DENY_BLOCKING;

    c->argv = orig_argv;
    c->argv_len = orig_argv_len;
    c->argc = orig_argc;
    c->cmd = c->realcmd = orig_cmd;
    discardTransaction(c);

    server.in_exec = 0;
}

/* ===================== WATCH (CAS alike for MULTI/EXEC) ===================
 *
 * The implementation uses a per-DB hash table mapping keys to list of clients
 * WATCHing those keys, so that given a key that is going to be modified
 * we can mark all the associated clients as dirty.
 *
 * Also every client contains a list of WATCHed keys so that's possible to
 * un-watch such keys when the client is freed or when UNWATCH is called. */

/* In the client->watched_keys list we need to use watchedKey structures
 * as in order to identify a key in Redis we need both the key name and the
 * DB. This struct is also referenced from db->watched_keys dict, where the
 * values are lists of watchedKey pointers. */
/**
 * ===================== WATCH（CAS 与 MULTIEXEC 类似）===================
 * 实现使用每个 DB 哈希表将键映射到正在监视这些键的客户端列表，因此给定将要修改的键，我们可以将所有关联的客户端标记为脏。
 * 此外，每个客户端都包含一个 WATCHed 键的列表，因此可以在客户端被释放或调用 UNWATCH 时取消监视这些键。
 * 在 client->watched_keys 列表中，我们需要使用 watchKey 结构，因为为了识别 Redis 中的键，我们需要键名和数据库。
 * 这个结构体也被 db->watched_keys dict 引用，其中的值是 watchKey 指针的列表。
 * */
typedef struct watchedKey {
    robj *key;
    redisDb *db;
    client *client;
    unsigned expired:1; /* Flag that we're watching an already expired key.  标记我们正在查看一个已经过期的密钥。*/
} watchedKey;

/* Watch for the specified key  注意指定的键*/
void watchForKey(client *c, robj *key) {
    list *clients = NULL;
    listIter li;
    listNode *ln;
    watchedKey *wk;

    /* Check if we are already watching for this key */
    //检查我们是否已经在关注这个密钥
    listRewind(c->watched_keys,&li);
    while((ln = listNext(&li))) {
        wk = listNodeValue(ln);
        if (wk->db == c->db && equalStringObjects(key,wk->key))
            return; /* Key already watched 钥匙已经看过*/
    }
    /* This key is not already watched in this DB. Let's add it */
    //此数据库中尚未查看此密钥。让我们添加它
    clients = dictFetchValue(c->db->watched_keys,key);
    if (!clients) {
        clients = listCreate();
        dictAdd(c->db->watched_keys,key,clients);
        incrRefCount(key);
    }
    /* Add the new key to the list of keys watched by this client */
    //将新密钥添加到此客户端监视的密钥列表中
    wk = zmalloc(sizeof(*wk));
    wk->key = key;
    wk->client = c;
    wk->db = c->db;
    wk->expired = keyIsExpired(c->db, key);
    incrRefCount(key);
    listAddNodeTail(c->watched_keys,wk);
    listAddNodeTail(clients,wk);
}

/* Unwatch all the keys watched by this client. To clean the EXEC dirty
 * flag is up to the caller. */
//取消监视此客户端监视的所有键。清除 EXEC 脏标志取决于调用者。
void unwatchAllKeys(client *c) {
    listIter li;
    listNode *ln;

    if (listLength(c->watched_keys) == 0) return;
    listRewind(c->watched_keys,&li);
    while((ln = listNext(&li))) {
        list *clients;
        watchedKey *wk;

        /* Lookup the watched key -> clients list and remove the client's wk
         * from the list */
        //查找被监视的键 -> 客户端列表并从列表中删除客户端的 wk
        wk = listNodeValue(ln);
        clients = dictFetchValue(wk->db->watched_keys, wk->key);
        serverAssertWithInfo(c,NULL,clients != NULL);
        listDelNode(clients,listSearchKey(clients,wk));
        /* Kill the entry at all if this was the only client */
        //如果这是唯一的客户端，则完全终止该条目
        if (listLength(clients) == 0)
            dictDelete(wk->db->watched_keys, wk->key);
        /* Remove this watched key from the client->watched list */
        //从客户端->监视列表中删除此监视键
        listDelNode(c->watched_keys,ln);
        decrRefCount(wk->key);
        zfree(wk);
    }
}

/* Iterates over the watched_keys list and looks for an expired key. Keys which
 * were expired already when WATCH was called are ignored. */
//遍历 watch_keys 列表并查找过期的密钥。调用 WATCH 时已经过期的键将被忽略。
int isWatchedKeyExpired(client *c) {
    listIter li;
    listNode *ln;
    watchedKey *wk;
    if (listLength(c->watched_keys) == 0) return 0;
    listRewind(c->watched_keys,&li);
    while ((ln = listNext(&li))) {
        wk = listNodeValue(ln);
        if (wk->expired) continue; /* was expired when WATCH was called 调用 WATCH 时已过期*/
        if (keyIsExpired(wk->db, wk->key)) return 1;
    }

    return 0;
}

/* "Touch" a key, so that if this key is being WATCHed by some client the
 * next EXEC will fail. */
//“触摸”一个键，这样如果某个客户端正在监视该键，则下一个 EXEC 将失败。
void touchWatchedKey(redisDb *db, robj *key) {
    list *clients;
    listIter li;
    listNode *ln;

    if (dictSize(db->watched_keys) == 0) return;
    clients = dictFetchValue(db->watched_keys, key);
    if (!clients) return;

    /* Mark all the clients watching this key as CLIENT_DIRTY_CAS */
    /* Check if we are already watching for this key */
    //将所有正在观看此密钥的客户端标记为 CLIENT_DIRTY_CAS 检查我们是否已经在观看此密钥
    listRewind(clients,&li);
    while((ln = listNext(&li))) {
        watchedKey *wk = listNodeValue(ln);
        client *c = wk->client;

        if (wk->expired) {
            /* The key was already expired when WATCH was called. */
            //调用 WATCH 时，密钥已经过期。
            if (db == wk->db &&
                equalStringObjects(key, wk->key) &&
                dictFind(db->dict, key->ptr) == NULL)
            {
                /* Already expired key is deleted, so logically no change. Clear
                 * the flag. Deleted keys are not flagged as expired. */
                //已经过期的key被删除了，所以逻辑上没有变化。清除标志。已删除的密钥不会标记为已过期。
                wk->expired = 0;
                goto skip_client;
            }
            break;
        }

        c->flags |= CLIENT_DIRTY_CAS;
        /* As the client is marked as dirty, there is no point in getting here
         * again in case that key (or others) are modified again (or keep the
         * memory overhead till EXEC). */
        //由于客户端被标记为脏，因此如果再次修改密钥（或其他密钥）（或将内存开销保持到 EXEC），再次到达这里是没有意义的。
        unwatchAllKeys(c);

    skip_client:
        continue;
    }
}

/* Set CLIENT_DIRTY_CAS to all clients of DB when DB is dirty.
 * It may happen in the following situations:
 * FLUSHDB, FLUSHALL, SWAPDB, end of successful diskless replication.
 *
 * replaced_with: for SWAPDB, the WATCH should be invalidated if
 * the key exists in either of them, and skipped only if it
 * doesn't exist in both. */
/**
 * 当 DB 脏时，将 CLIENT_DIRTY_CAS 设置为 DB 的所有客户端。
 * 可能发生在以下情况：FLUSHDB、FLUSHALL、SWAPDB、无盘复制成功结束。
 * replace_with：对于 SWAPDB，如果密钥存在于其中任何一个中，则应使 WATCH 无效，并且仅当两者都不存在时才跳过。
 * */
void touchAllWatchedKeysInDb(redisDb *emptied, redisDb *replaced_with) {
    listIter li;
    listNode *ln;
    dictEntry *de;

    if (dictSize(emptied->watched_keys) == 0) return;

    dictIterator *di = dictGetSafeIterator(emptied->watched_keys);
    while((de = dictNext(di)) != NULL) {
        robj *key = dictGetKey(de);
        int exists_in_emptied = dictFind(emptied->dict, key->ptr) != NULL;
        if (exists_in_emptied ||
            (replaced_with && dictFind(replaced_with->dict, key->ptr)))
        {
            list *clients = dictGetVal(de);
            if (!clients) continue;
            listRewind(clients,&li);
            while((ln = listNext(&li))) {
                watchedKey *wk = listNodeValue(ln);
                if (wk->expired) {
                    if (!replaced_with || !dictFind(replaced_with->dict, key->ptr)) {
                        /* Expired key now deleted. No logical change. Clear the
                         * flag. Deleted keys are not flagged as expired. */
                        //过期密钥现已删除。没有逻辑变化。清除标志。已删除的密钥不会标记为已过期。
                        wk->expired = 0;
                        continue;
                    } else if (keyIsExpired(replaced_with, key)) {
                        /* Expired key remains expired. 过期的密钥仍然过期。*/
                        continue;
                    }
                } else if (!exists_in_emptied && keyIsExpired(replaced_with, key)) {
                    /* Non-existing key is replaced with an expired key. */
                    //不存在的密钥将替换为过期的密钥。
                    wk->expired = 1;
                    continue;
                }
                client *c = wk->client;
                c->flags |= CLIENT_DIRTY_CAS;
                /* As the client is marked as dirty, there is no point in getting here
                 * again for others keys (or keep the memory overhead till EXEC). */
                //由于客户端被标记为脏，没有必要再次到这里获取其他密钥（或将内存开销保持到 EXEC）。
                unwatchAllKeys(c);
            }
        }
    }
    dictReleaseIterator(di);
}

void watchCommand(client *c) {
    int j;

    if (c->flags & CLIENT_MULTI) {
        addReplyError(c,"WATCH inside MULTI is not allowed");
        return;
    }
    /* No point in watching if the client is already dirty. */
    //看客户端是否已经脏了是没有意义的。
    if (c->flags & CLIENT_DIRTY_CAS) {
        addReply(c,shared.ok);
        return;
    }
    for (j = 1; j < c->argc; j++)
        watchForKey(c,c->argv[j]);
    addReply(c,shared.ok);
}

void unwatchCommand(client *c) {
    unwatchAllKeys(c);
    c->flags &= (~CLIENT_DIRTY_CAS);
    addReply(c,shared.ok);
}

size_t multiStateMemOverhead(client *c) {
    size_t mem = c->mstate.argv_len_sums;
    /* Add watched keys overhead, Note: this doesn't take into account the watched keys themselves, because they aren't managed per-client. */
    //添加监视密钥开销，注意：这不考虑监视密钥本身，因为它们不是按客户端管理的。
    mem += listLength(c->watched_keys) * (sizeof(listNode) + sizeof(watchedKey));
    /* Reserved memory for queued multi commands. */
    //为排队的多命令保留的内存。
    mem += c->mstate.alloc_count * sizeof(multiCmd);
    return mem;
}
