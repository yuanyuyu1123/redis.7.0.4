/*
 * Copyright (c) 2013, Salvatore Sanfilippo <antirez at gmail dot com>
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

/* This file implements keyspace events notification via Pub/Sub and
 * described at https://redis.io/topics/notifications. */

/* Turn a string representing notification classes into an integer
 * representing notification classes flags xored.
 *
 * The function returns -1 if the input contains characters not mapping to
 * any class. */
/**该文件通过 Pub/Sub 实现键空间事件通知，并在 https:redis.iotopicsnotifications 中进行了描述。
 * 将表示通知类的字符串转换为表示异或通知类标志的整数。如果输入包含未映射到任何类的字符，则该函数返回 -1。*/
int keyspaceEventsStringToFlags(char *classes) {
    char *p = classes;
    int c, flags = 0;

    while((c = *p++) != '\0') {
        switch(c) {
        case 'A': flags |= NOTIFY_ALL; break;
        case 'g': flags |= NOTIFY_GENERIC; break;
        case '$': flags |= NOTIFY_STRING; break;
        case 'l': flags |= NOTIFY_LIST; break;
        case 's': flags |= NOTIFY_SET; break;
        case 'h': flags |= NOTIFY_HASH; break;
        case 'z': flags |= NOTIFY_ZSET; break;
        case 'x': flags |= NOTIFY_EXPIRED; break;
        case 'e': flags |= NOTIFY_EVICTED; break;
        case 'K': flags |= NOTIFY_KEYSPACE; break;
        case 'E': flags |= NOTIFY_KEYEVENT; break;
        case 't': flags |= NOTIFY_STREAM; break;
        case 'm': flags |= NOTIFY_KEY_MISS; break;
        case 'd': flags |= NOTIFY_MODULE; break;
        case 'n': flags |= NOTIFY_NEW; break;
        default: return -1;
        }
    }
    return flags;
}

/* This function does exactly the reverse of the function above: it gets
 * as input an integer with the xored flags and returns a string representing
 * the selected classes. The string returned is an sds string that needs to
 * be released with sdsfree(). */
/**这个函数与上面的函数完全相反：它获取一个带有异或标志的整数作为输入，并返回一个代表所选类的字符串。
 * 返回的字符串是一个 sds 字符串，需要用 sdsfree() 释放。*/
sds keyspaceEventsFlagsToString(int flags) {
    sds res;

    res = sdsempty();
    if ((flags & NOTIFY_ALL) == NOTIFY_ALL) {
        res = sdscatlen(res,"A",1);
    } else {
        if (flags & NOTIFY_GENERIC) res = sdscatlen(res,"g",1);
        if (flags & NOTIFY_STRING) res = sdscatlen(res,"$",1);
        if (flags & NOTIFY_LIST) res = sdscatlen(res,"l",1);
        if (flags & NOTIFY_SET) res = sdscatlen(res,"s",1);
        if (flags & NOTIFY_HASH) res = sdscatlen(res,"h",1);
        if (flags & NOTIFY_ZSET) res = sdscatlen(res,"z",1);
        if (flags & NOTIFY_EXPIRED) res = sdscatlen(res,"x",1);
        if (flags & NOTIFY_EVICTED) res = sdscatlen(res,"e",1);
        if (flags & NOTIFY_STREAM) res = sdscatlen(res,"t",1);
        if (flags & NOTIFY_MODULE) res = sdscatlen(res,"d",1);
        if (flags & NOTIFY_NEW) res = sdscatlen(res,"n",1);
    }
    if (flags & NOTIFY_KEYSPACE) res = sdscatlen(res,"K",1);
    if (flags & NOTIFY_KEYEVENT) res = sdscatlen(res,"E",1);
    if (flags & NOTIFY_KEY_MISS) res = sdscatlen(res,"m",1);
    return res;
}

/* The API provided to the rest of the Redis core is a simple function:
 *
 * notifyKeyspaceEvent(int type, char *event, robj *key, int dbid);
 *
 * 'type' is the notification class we define in `server.h`.
 * 'event' is a C string representing the event name.
 * 'key' is a Redis object representing the key name.
 * 'dbid' is the database ID where the key lives.  */
/**
 * 提供给 Redis 核心其余部分的 API 是一个简单的函数：
 *   notifyKeyspaceEvent(int type, char event, robj key, int dbid);
 * 'type' 是我们在 `server.h` 中定义的通知类。
 * 'event' 是一个表示事件名称的 C 字符串。
 * 'key' 是一个 Redis 对象，表示键名。
 * 'dbid' 是密钥所在的数据库 ID。
 * */
void notifyKeyspaceEvent(int type, char *event, robj *key, int dbid) {
    sds chan;
    robj *chanobj, *eventobj;
    int len = -1;
    char buf[24];

    /* If any modules are interested in events, notify the module system now.
     * This bypasses the notifications configuration, but the module engine
     * will only call event subscribers if the event type matches the types
     * they are interested in. */
    //如果有任何模块对事件感兴趣，请立即通知模块系统。
    // 这绕过了通知配置，但是如果事件类型与他们感兴趣的类型匹配，模块引擎只会调用事件订阅者。
     moduleNotifyKeyspaceEvent(type, event, key, dbid);

    /* If notifications for this class of events are off, return ASAP. */
    //如果此类事件的通知已关闭，请尽快返回。
    if (!(server.notify_keyspace_events & type)) return;

    eventobj = createStringObject(event,strlen(event));

    /* __keyspace@<db>__:<key> <event> notifications. */
    if (server.notify_keyspace_events & NOTIFY_KEYSPACE) {
        chan = sdsnewlen("__keyspace@",11);
        len = ll2string(buf,sizeof(buf),dbid);
        chan = sdscatlen(chan, buf, len);
        chan = sdscatlen(chan, "__:", 3);
        chan = sdscatsds(chan, key->ptr);
        chanobj = createObject(OBJ_STRING, chan);
        pubsubPublishMessage(chanobj, eventobj, 0);
        decrRefCount(chanobj);
    }

    /* __keyevent@<db>__:<event> <key> notifications. */
    if (server.notify_keyspace_events & NOTIFY_KEYEVENT) {
        chan = sdsnewlen("__keyevent@",11);
        if (len == -1) len = ll2string(buf,sizeof(buf),dbid);
        chan = sdscatlen(chan, buf, len);
        chan = sdscatlen(chan, "__:", 3);
        chan = sdscatsds(chan, eventobj->ptr);
        chanobj = createObject(OBJ_STRING, chan);
        pubsubPublishMessage(chanobj, key, 0);
        decrRefCount(chanobj);
    }
    decrRefCount(eventobj);
}
