/* Copyright (c) 2009-2020, Salvatore Sanfilippo <antirez at gmail dot com>
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

/* ========================== Clients timeouts ============================= */

/* Check if this blocked client timedout (does nothing if the client is
 * not blocked right now). If so send a reply, unblock it, and return 1.
 * Otherwise 0 is returned and no operation is performed. */
/**
 * 检查这个被阻止的客户端是否超时（如果客户端现在没有被阻止，则什么都不做）。
 * 如果是则发送回复，解除阻塞，并返回 1。否则返回 0，不执行任何操作。
 * */
int checkBlockedClientTimeout(client *c, mstime_t now) {
    if (c->flags & CLIENT_BLOCKED &&
        c->bpop.timeout != 0
        && c->bpop.timeout < now)
    {
        /* Handle blocking operation specific timeout. */
        //处理阻塞操作特定的超时。
        replyToBlockedClientTimedOut(c);
        unblockClient(c);
        return 1;
    } else {
        return 0;
    }
}

/* Check for timeouts. Returns non-zero if the client was terminated.
 * The function gets the current time in milliseconds as argument since
 * it gets called multiple times in a loop, so calling gettimeofday() for
 * each iteration would be costly without any actual gain. */
/**
 * 检查超时。如果客户端终止，则返回非零值。
 * 该函数获取当前时间（以毫秒为单位）作为参数，因为它在循环中被多次调用，
 * 因此每次迭代调用 gettimeofday() 代价高昂且没有任何实际收益。
 * */
int clientsCronHandleTimeout(client *c, mstime_t now_ms) {
    time_t now = now_ms/1000;

    if (server.maxidletime &&
        /* This handles the idle clients connection timeout if set. */
        //如果设置，这将处理空闲客户端连接超时。
        !(c->flags & CLIENT_SLAVE) &&   /* No timeout for slaves and monitors */
        !mustObeyClient(c) &&         /* No timeout for masters and AOF */
        !(c->flags & CLIENT_BLOCKED) && /* No timeout for BLPOP */
        !(c->flags & CLIENT_PUBSUB) &&  /* No timeout for Pub/Sub clients */
        (now - c->lastinteraction > server.maxidletime))
    {
        serverLog(LL_VERBOSE,"Closing idle client");
        freeClient(c);
        return 1;
    } else if (c->flags & CLIENT_BLOCKED) {
        /* Cluster: handle unblock & redirect of clients blocked
         * into keys no longer served by this server. */
        //集群：处理被阻止的客户端的解除阻止和重定向到此服务器不再提供的密钥。
        if (server.cluster_enabled) {
            if (clusterRedirectBlockedClientIfNeeded(c))
                unblockClient(c);
        }
    }
    return 0;
}

/* For blocked clients timeouts we populate a radix tree of 128 bit keys
 * composed as such:
 *
 *  [8 byte big endian expire time]+[8 byte client ID]
 *
 * We don't do any cleanup in the Radix tree: when we run the clients that
 * reached the timeout already, if they are no longer existing or no longer
 * blocked with such timeout, we just go forward.
 *
 * Every time a client blocks with a timeout, we add the client in
 * the tree. In beforeSleep() we call handleBlockedClientsTimeout() to run
 * the tree and unblock the clients. */
/**
 * 对于阻塞的客户端超时，我们填充 128 位密钥的基数树，其组成如下：[8 字节大端过期时间]+[8 字节客户端 ID]
 * 我们不对基数树进行任何清理：当我们运行客户端时已经达到超时，如果它们不再存在或不再被这种超时阻塞，我们就继续前进。
 * 每次客户端因超时而阻塞时，我们都会在树中添加客户端。在 beforeSleep() 中，
 * 我们调用 handleBlockedClientsTimeout() 来运行树并解除对客户端的阻塞。
 * */

#define CLIENT_ST_KEYLEN 16    /* 8 bytes mstime + 8 bytes client ID. */

/* Given client ID and timeout, write the resulting radix tree key in buf. */
//给定客户端 ID 和超时，将生成的基数树键写入 buf。
void encodeTimeoutKey(unsigned char *buf, uint64_t timeout, client *c) {
    timeout = htonu64(timeout);
    memcpy(buf,&timeout,sizeof(timeout));
    memcpy(buf+8,&c,sizeof(c));
    if (sizeof(c) == 4) memset(buf+12,0,4); /* Zero padding for 32bit target. */
}

/* Given a key encoded with encodeTimeoutKey(), resolve the fields and write
 * the timeout into *toptr and the client pointer into *cptr. */
//给定一个使用 encodeTimeoutKey() 编码的密钥，解析字段并将超时写入 toptr 并将客户端指针写入 cptr。
void decodeTimeoutKey(unsigned char *buf, uint64_t *toptr, client **cptr) {
    memcpy(toptr,buf,sizeof(*toptr));
    *toptr = ntohu64(*toptr);
    memcpy(cptr,buf+8,sizeof(*cptr));
}

/* Add the specified client id / timeout as a key in the radix tree we use
 * to handle blocked clients timeouts. The client is not added to the list
 * if its timeout is zero (block forever). */
/**
 * 在我们用来处理阻塞客户端超时的基数树中添加指定的客户端 ID 超时作为键。
 * 如果客户端超时为零（永远阻塞），则客户端不会添加到列表中。
 * */
void addClientToTimeoutTable(client *c) {
    if (c->bpop.timeout == 0) return;
    uint64_t timeout = c->bpop.timeout;
    unsigned char buf[CLIENT_ST_KEYLEN];
    encodeTimeoutKey(buf,timeout,c);
    if (raxTryInsert(server.clients_timeout_table,buf,sizeof(buf),NULL,NULL))
        c->flags |= CLIENT_IN_TO_TABLE;
}

/* Remove the client from the table when it is unblocked for reasons
 * different than timing out. */
//当客户端由于与超时不同的原因而被解除阻塞时，将其从表中删除。
void removeClientFromTimeoutTable(client *c) {
    if (!(c->flags & CLIENT_IN_TO_TABLE)) return;
    c->flags &= ~CLIENT_IN_TO_TABLE;
    uint64_t timeout = c->bpop.timeout;
    unsigned char buf[CLIENT_ST_KEYLEN];
    encodeTimeoutKey(buf,timeout,c);
    raxRemove(server.clients_timeout_table,buf,sizeof(buf),NULL);
}

/* This function is called in beforeSleep() in order to unblock clients
 * that are waiting in blocking operations with a timeout set. */
//此函数在 beforeSleep() 中调用，以解除对正在等待设置超时的阻塞操作的客户端的阻塞。
void handleBlockedClientsTimeout(void) {
    if (raxSize(server.clients_timeout_table) == 0) return;
    uint64_t now = mstime();
    raxIterator ri;
    raxStart(&ri,server.clients_timeout_table);
    raxSeek(&ri,"^",NULL,0);

    while(raxNext(&ri)) {
        uint64_t timeout;
        client *c;
        decodeTimeoutKey(ri.key,&timeout,&c);
        if (timeout >= now) break; /* All the timeouts are in the future. 所有超时都在将来。*/
        c->flags &= ~CLIENT_IN_TO_TABLE;
        checkBlockedClientTimeout(c,now);
        raxRemove(server.clients_timeout_table,ri.key,ri.key_len,NULL);
        raxSeek(&ri,"^",NULL,0);
    }
    raxStop(&ri);
}

/* Get a timeout value from an object and store it into 'timeout'.
 * The final timeout is always stored as milliseconds as a time where the
 * timeout will expire, however the parsing is performed according to
 * the 'unit' that can be seconds or milliseconds.
 *
 * Note that if the timeout is zero (usually from the point of view of
 * commands API this means no timeout) the value stored into 'timeout'
 * is zero. */
/**
 * 从对象中获取超时值并将其存储到“超时”中。
 * 最终超时始终以毫秒为单位存储为超时将到期的时间，但是解析是根据“单位”执行的，可以是秒或毫秒。
 * 请注意，如果超时为零（通常从命令 API 的角度来看，这意味着没有超时），则存储在“超时”中的值为零。
 * */
int getTimeoutFromObjectOrReply(client *c, robj *object, mstime_t *timeout, int unit) {
    long long tval;
    long double ftval;

    if (unit == UNIT_SECONDS) {
        if (getLongDoubleFromObjectOrReply(c,object,&ftval,
            "timeout is not a float or out of range") != C_OK)
            return C_ERR;
        tval = (long long) (ftval * 1000.0);
    } else {
        if (getLongLongFromObjectOrReply(c,object,&tval,
            "timeout is not an integer or out of range") != C_OK)
            return C_ERR;
    }

    if (tval < 0) {
        addReplyError(c,"timeout is negative");
        return C_ERR;
    }

    if (tval > 0) {
        tval += mstime();
    }
    *timeout = tval;

    return C_OK;
}
