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

/*-----------------------------------------------------------------------------
 * List API
 *----------------------------------------------------------------------------*/

/* The function pushes an element to the specified list object 'subject',
 * at head or tail position as specified by 'where'.
 *
 * There is no need for the caller to increment the refcount of 'value' as
 * the function takes care of it if needed. */
/**
 * -------------------------- 列表 API --------- ------------
 * 该函数将元素推送到指定的列表对象“subject”，位于“where”指定的头部或尾部位置。
 * 调用者无需增加“值”的引用计数，因为函数会在需要时处理它。*/
void listTypePush(robj *subject, robj *value, int where) {
    if (subject->encoding == OBJ_ENCODING_QUICKLIST) {
        int pos = (where == LIST_HEAD) ? QUICKLIST_HEAD : QUICKLIST_TAIL;
        if (value->encoding == OBJ_ENCODING_INT) {
            char buf[32];
            ll2string(buf, 32, (long)value->ptr);
            quicklistPush(subject->ptr, buf, strlen(buf), pos);
        } else {
            quicklistPush(subject->ptr, value->ptr, sdslen(value->ptr), pos);
        }
    } else {
        serverPanic("Unknown list encoding");
    }
}

void *listPopSaver(unsigned char *data, size_t sz) {
    return createStringObject((char*)data,sz);
}

robj *listTypePop(robj *subject, int where) {
    long long vlong;
    robj *value = NULL;

    int ql_where = where == LIST_HEAD ? QUICKLIST_HEAD : QUICKLIST_TAIL;
    if (subject->encoding == OBJ_ENCODING_QUICKLIST) {
        if (quicklistPopCustom(subject->ptr, ql_where, (unsigned char **)&value,
                               NULL, &vlong, listPopSaver)) {
            if (!value)
                value = createStringObjectFromLongLong(vlong);
        }
    } else {
        serverPanic("Unknown list encoding");
    }
    return value;
}

unsigned long listTypeLength(const robj *subject) {
    if (subject->encoding == OBJ_ENCODING_QUICKLIST) {
        return quicklistCount(subject->ptr);
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Initialize an iterator at the specified index. */
//在指定索引处初始化迭代器。
listTypeIterator *listTypeInitIterator(robj *subject, long index,
                                       unsigned char direction) {
    listTypeIterator *li = zmalloc(sizeof(listTypeIterator));
    li->subject = subject;
    li->encoding = subject->encoding;
    li->direction = direction;
    li->iter = NULL;
    /* LIST_HEAD means start at TAIL and move *towards* head.
     * LIST_TAIL means start at HEAD and move *towards tail. */
    //LIST_HEAD 表示从 TAIL 开始向头部移动。 LIST_TAIL 表示从 HEAD 开始向尾部移动。
    int iter_direction =
        direction == LIST_HEAD ? AL_START_TAIL : AL_START_HEAD;
    if (li->encoding == OBJ_ENCODING_QUICKLIST) {
        li->iter = quicklistGetIteratorAtIdx(li->subject->ptr,
                                             iter_direction, index);
    } else {
        serverPanic("Unknown list encoding");
    }
    return li;
}

/* Sets the direction of an iterator. 设置迭代器的方向。*/
void listTypeSetIteratorDirection(listTypeIterator *li, unsigned char direction) {
    li->direction = direction;
    int dir = direction == LIST_HEAD ? AL_START_TAIL : AL_START_HEAD;
    quicklistSetDirection(li->iter, dir);
}

/* Clean up the iterator. */
void listTypeReleaseIterator(listTypeIterator *li) {
    quicklistReleaseIterator(li->iter);
    zfree(li);
}

/* Stores pointer to current the entry in the provided entry structure
 * and advances the position of the iterator. Returns 1 when the current
 * entry is in fact an entry, 0 otherwise. */
//在提供的条目结构中存储指向当前条目的指针并推进迭代器的位置。当当前条目实际上是一个条目时返回 1，否则返回 0。
int listTypeNext(listTypeIterator *li, listTypeEntry *entry) {
    /* Protect from converting when iterating */
    serverAssert(li->subject->encoding == li->encoding);

    entry->li = li;
    if (li->encoding == OBJ_ENCODING_QUICKLIST) {
        return quicklistNext(li->iter, &entry->entry);
    } else {
        serverPanic("Unknown list encoding");
    }
    return 0;
}

/* Return entry or NULL at the current position of the iterator. */
//在迭代器的当前位置返回条目或 NULL。
robj *listTypeGet(listTypeEntry *entry) {
    robj *value = NULL;
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        if (entry->entry.value) {
            value = createStringObject((char *)entry->entry.value,
                                       entry->entry.sz);
        } else {
            value = createStringObjectFromLongLong(entry->entry.longval);
        }
    } else {
        serverPanic("Unknown list encoding");
    }
    return value;
}

void listTypeInsert(listTypeEntry *entry, robj *value, int where) {
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        value = getDecodedObject(value);
        sds str = value->ptr;
        size_t len = sdslen(str);
        if (where == LIST_TAIL) {
            quicklistInsertAfter(entry->li->iter, &entry->entry, str, len);
        } else if (where == LIST_HEAD) {
            quicklistInsertBefore(entry->li->iter, &entry->entry, str, len);
        }
        decrRefCount(value);
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Replaces entry at the current position of the iterator. */
//替换迭代器当前位置的条目。
void listTypeReplace(listTypeEntry *entry, robj *value) {
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        value = getDecodedObject(value);
        sds str = value->ptr;
        size_t len = sdslen(str);
        quicklistReplaceEntry(entry->li->iter, &entry->entry, str, len);
        decrRefCount(value);
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Compare the given object with the entry at the current position. */
//将给定对象与当前位置的条目进行比较。
int listTypeEqual(listTypeEntry *entry, robj *o) {
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        serverAssertWithInfo(NULL,o,sdsEncodedObject(o));
        return quicklistCompare(&entry->entry,o->ptr,sdslen(o->ptr));
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Delete the element pointed to. 删除指向的元素。*/
void listTypeDelete(listTypeIterator *iter, listTypeEntry *entry) {
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklistDelEntry(iter->iter, &entry->entry);
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* This is a helper function for the COPY command.
 * Duplicate a list object, with the guarantee that the returned object
 * has the same encoding as the original one.
 *
 * The resulting object always has refcount set to 1 */
//这是 COPY 命令的辅助函数。复制一个列表对象，并保证返回的对象与原始对象具有相同的编码。结果对象始终将 refcount 设置为 1
robj *listTypeDup(robj *o) {
    robj *lobj;

    serverAssert(o->type == OBJ_LIST);

    switch (o->encoding) {
        case OBJ_ENCODING_QUICKLIST:
            lobj = createObject(OBJ_LIST, quicklistDup(o->ptr));
            lobj->encoding = o->encoding;
            break;
        default:
            serverPanic("Unknown list encoding");
            break;
    }
    return lobj;
}

/* Delete a range of elements from the list. */
//从列表中删除一系列元素。
int listTypeDelRange(robj *subject, long start, long count) {
    if (subject->encoding == OBJ_ENCODING_QUICKLIST) {
        return quicklistDelRange(subject->ptr, start, count);
    } else {
        serverPanic("Unknown list encoding");
    }
}

/*-----------------------------------------------------------------------------
 * List Commands
 *----------------------------------------------------------------------------*/

/* Implements LPUSH/RPUSH/LPUSHX/RPUSHX. 
 * 'xx': push if key exists. */
void pushGenericCommand(client *c, int where, int xx) {
    int j;

    robj *lobj = lookupKeyWrite(c->db, c->argv[1]);
    if (checkType(c,lobj,OBJ_LIST)) return;
    if (!lobj) {
        if (xx) {
            addReply(c, shared.czero);
            return;
        }

        lobj = createQuicklistObject();
        quicklistSetOptions(lobj->ptr, server.list_max_listpack_size,
                            server.list_compress_depth);
        dbAdd(c->db,c->argv[1],lobj);
    }

    for (j = 2; j < c->argc; j++) {
        listTypePush(lobj,c->argv[j],where);
        server.dirty++;
    }

    addReplyLongLong(c, listTypeLength(lobj));

    char *event = (where == LIST_HEAD) ? "lpush" : "rpush";
    signalModifiedKey(c,c->db,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_LIST,event,c->argv[1],c->db->id);
}

/* LPUSH <key> <element> [<element> ...] */
void lpushCommand(client *c) {
    pushGenericCommand(c,LIST_HEAD,0);
}

/* RPUSH <key> <element> [<element> ...] */
void rpushCommand(client *c) {
    pushGenericCommand(c,LIST_TAIL,0);
}

/* LPUSHX <key> <element> [<element> ...] */
void lpushxCommand(client *c) {
    pushGenericCommand(c,LIST_HEAD,1);
}

/* RPUSH <key> <element> [<element> ...] */
void rpushxCommand(client *c) {
    pushGenericCommand(c,LIST_TAIL,1);
}

/* LINSERT <key> (BEFORE|AFTER) <pivot> <element> */
void linsertCommand(client *c) {
    int where;
    robj *subject;
    listTypeIterator *iter;
    listTypeEntry entry;
    int inserted = 0;

    if (strcasecmp(c->argv[2]->ptr,"after") == 0) {
        where = LIST_TAIL;
    } else if (strcasecmp(c->argv[2]->ptr,"before") == 0) {
        where = LIST_HEAD;
    } else {
        addReplyErrorObject(c,shared.syntaxerr);
        return;
    }

    if ((subject = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,subject,OBJ_LIST)) return;

    /* Seek pivot from head to tail 从头到尾寻找支点*/
    iter = listTypeInitIterator(subject,0,LIST_TAIL);
    while (listTypeNext(iter,&entry)) {
        if (listTypeEqual(&entry,c->argv[3])) {
            listTypeInsert(&entry,c->argv[4],where);
            inserted = 1;
            break;
        }
    }
    listTypeReleaseIterator(iter);

    if (inserted) {
        signalModifiedKey(c,c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_LIST,"linsert",
                            c->argv[1],c->db->id);
        server.dirty++;
    } else {
        /* Notify client of a failed insert 通知客户插入失败*/
        addReplyLongLong(c,-1);
        return;
    }

    addReplyLongLong(c,listTypeLength(subject));
}

/* LLEN <key> */
void llenCommand(client *c) {
    robj *o = lookupKeyReadOrReply(c,c->argv[1],shared.czero);
    if (o == NULL || checkType(c,o,OBJ_LIST)) return;
    addReplyLongLong(c,listTypeLength(o));
}

/* LINDEX <key> <index> */
void lindexCommand(client *c) {
    robj *o = lookupKeyReadOrReply(c,c->argv[1],shared.null[c->resp]);
    if (o == NULL || checkType(c,o,OBJ_LIST)) return;
    long index;

    if ((getLongFromObjectOrReply(c, c->argv[2], &index, NULL) != C_OK))
        return;

    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklistEntry entry;
        quicklistIter *iter = quicklistGetIteratorEntryAtIdx(o->ptr, index, &entry);
        if (iter) {
            if (entry.value) {
                addReplyBulkCBuffer(c, entry.value, entry.sz);
            } else {
                addReplyBulkLongLong(c, entry.longval);
            }
        } else {
            addReplyNull(c);
        }
        quicklistReleaseIterator(iter);
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* LSET <key> <index> <element> */
void lsetCommand(client *c) {
    robj *o = lookupKeyWriteOrReply(c,c->argv[1],shared.nokeyerr);
    if (o == NULL || checkType(c,o,OBJ_LIST)) return;
    long index;
    robj *value = c->argv[3];

    if ((getLongFromObjectOrReply(c, c->argv[2], &index, NULL) != C_OK))
        return;

    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklist *ql = o->ptr;
        int replaced = quicklistReplaceAtIndex(ql, index,
                                               value->ptr, sdslen(value->ptr));
        if (!replaced) {
            addReplyErrorObject(c,shared.outofrangeerr);
        } else {
            addReply(c,shared.ok);
            signalModifiedKey(c,c->db,c->argv[1]);
            notifyKeyspaceEvent(NOTIFY_LIST,"lset",c->argv[1],c->db->id);
            server.dirty++;
        }
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* A helper function like addListRangeReply, more details see below.
 * The difference is that here we are returning nested arrays, like:
 * 1) keyname
 * 2) 1) element1
 *    2) element2
 *
 * And also actually pop out from the list by calling listElementsRemoved.
 * We maintain the server.dirty and notifications there.
 *
 * 'deleted' is an optional output argument to get an indication
 * if the key got deleted by this function. */
/**
 * 像 addListRangeReply 这样的辅助函数，更多细节见下文。不同之处在于，这里我们返回嵌套数组，例如：
 *   1) keyname
 *   2)
 *     1) element1
 *     2) element2
 * 并且实际上还通过调用 listElementsRemoved 从列表中弹出。我们在那里维护 server.dirty 和通知。
 * 'deleted' 是一个可选的输出参数，用于指示键是否被此函数删除。
 * */
void listPopRangeAndReplyWithKey(client *c, robj *o, robj *key, int where, long count, int *deleted) {
    long llen = listTypeLength(o);
    long rangelen = (count > llen) ? llen : count;
    long rangestart = (where == LIST_HEAD) ? 0 : -rangelen;
    long rangeend = (where == LIST_HEAD) ? rangelen - 1 : -1;
    int reverse = (where == LIST_HEAD) ? 0 : 1;

    /* We return key-name just once, and an array of elements */
    /**我们只返回一次键名和一个元素数组*/
    addReplyArrayLen(c, 2);
    addReplyBulk(c, key);
    addListRangeReply(c, o, rangestart, rangeend, reverse);

    /* Pop these elements. */
    listTypeDelRange(o, rangestart, rangelen);
    /* Maintain the notifications and dirty. 保持通知和脏。*/
    listElementsRemoved(c, key, where, o, rangelen, deleted);
}

/* A helper for replying with a list's range between the inclusive start and end
 * indexes as multi-bulk, with support for negative indexes. Note that start
 * must be less than end or an empty array is returned. When the reverse
 * argument is set to a non-zero value, the reply is reversed so that elements
 * are returned from end to start. */
/**一个帮助器，用于将包含开始和结束索引之间的列表范围作为多批量回复，并支持负索引。
 * 注意 start 必须小于 end 否则返回一个空数组。
 * 当 reverse 参数设置为非零值时，回复将反转，以便从 end 到 start 返回元素。 */
void addListRangeReply(client *c, robj *o, long start, long end, int reverse) {
    long rangelen, llen = listTypeLength(o);

    /* Convert negative indexes. */
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. */
    /**不变量：start >= 0，因此当 end < 0 时该测试为真。当 start > end 或 start >= length 时范围为空。*/
    if (start > end || start >= llen) {
        addReply(c,shared.emptyarray);
        return;
    }
    if (end >= llen) end = llen-1;
    rangelen = (end-start)+1;

    /* Return the result in form of a multi-bulk reply */
    /**以多批回复的形式返回结果*/
    addReplyArrayLen(c,rangelen);
    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        int from = reverse ? end : start;
        int direction = reverse ? LIST_HEAD : LIST_TAIL;
        listTypeIterator *iter = listTypeInitIterator(o,from,direction);

        while(rangelen--) {
            listTypeEntry entry;
            serverAssert(listTypeNext(iter, &entry)); /* fail on corrupt data */
            quicklistEntry *qe = &entry.entry;
            if (qe->value) {
                addReplyBulkCBuffer(c,qe->value,qe->sz);
            } else {
                addReplyBulkLongLong(c,qe->longval);
            }
        }
        listTypeReleaseIterator(iter);
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* A housekeeping helper for list elements popping tasks.
 *
 * 'deleted' is an optional output argument to get an indication
 * if the key got deleted by this function. */
//列表元素弹出任务的管家助手。
// 'deleted' 是一个可选的输出参数，用于指示键是否被此函数删除。
void listElementsRemoved(client *c, robj *key, int where, robj *o, long count, int *deleted) {
    char *event = (where == LIST_HEAD) ? "lpop" : "rpop";

    notifyKeyspaceEvent(NOTIFY_LIST, event, key, c->db->id);
    if (listTypeLength(o) == 0) {
        if (deleted) *deleted = 1;

        dbDelete(c->db, key);
        notifyKeyspaceEvent(NOTIFY_GENERIC, "del", key, c->db->id);
    } else {
        if (deleted) *deleted = 0;
    }
    signalModifiedKey(c, c->db, key);
    server.dirty += count;
}

/* Implements the generic list pop operation for LPOP/RPOP.
 * The where argument specifies which end of the list is operated on. An
 * optional count may be provided as the third argument of the client's
 * command. */
//实现 LPOPRPOP 的通用列表弹出操作。
// where 参数指定操作列表的哪一端。
// 可以提供一个可选的计数作为客户端命令的第三个参数。
void popGenericCommand(client *c, int where) {
    int hascount = (c->argc == 3);
    long count = 0;
    robj *value;

    if (c->argc > 3) {
        addReplyErrorArity(c);
        return;
    } else if (hascount) {
        /* Parse the optional count argument. 解析可选的计数参数。*/
        if (getPositiveLongFromObjectOrReply(c,c->argv[2],&count,NULL) != C_OK) 
            return;
    }

    robj *o = lookupKeyWriteOrReply(c, c->argv[1], hascount ? shared.nullarray[c->resp]: shared.null[c->resp]);
    if (o == NULL || checkType(c, o, OBJ_LIST))
        return;

    if (hascount && !count) {
        /* Fast exit path. */
        addReply(c,shared.emptyarray);
        return;
    }

    if (!count) {
        /* Pop a single element. This is POP's original behavior that replies
         * with a bulk string. */
        /**弹出单个元素。这是 POP 的原始行为，它使用批量字符串进行回复。*/
        value = listTypePop(o,where);
        serverAssert(value != NULL);
        addReplyBulk(c,value);
        decrRefCount(value);
        listElementsRemoved(c,c->argv[1],where,o,1,NULL);
    } else {
        /* Pop a range of elements. An addition to the original POP command,
         *  which replies with a multi-bulk. */
        /**弹出一系列元素。对原始 POP 命令的补充，它以多批量回复。*/
        long llen = listTypeLength(o);
        long rangelen = (count > llen) ? llen : count;
        long rangestart = (where == LIST_HEAD) ? 0 : -rangelen;
        long rangeend = (where == LIST_HEAD) ? rangelen - 1 : -1;
        int reverse = (where == LIST_HEAD) ? 0 : 1;

        addListRangeReply(c,o,rangestart,rangeend,reverse);
        listTypeDelRange(o,rangestart,rangelen);
        listElementsRemoved(c,c->argv[1],where,o,rangelen,NULL);
    }
}

/* Like popGenericCommand but work with multiple keys.
 * Take multiple keys and return multiple elements from just one key.
 *
 * 'numkeys' the number of keys.
 * 'count' is the number of elements requested to pop.
 *
 * Always reply with array. */
/**与 popGenericCommand 类似，但可以使用多个键。获取多个键并从一个键返回多个元素。
 *
 *   'numkeys' 键的数量。
 *   'count' 是请求弹出的元素数。
 *
 * 总是用数组回复。*/
void mpopGenericCommand(client *c, robj **keys, int numkeys, int where, long count) {
    int j;
    robj *o;
    robj *key;

    for (j = 0; j < numkeys; j++) {
        key = keys[j];
        o = lookupKeyWrite(c->db, key);

        /* Non-existing key, move to next key. */
        if (o == NULL) continue;

        if (checkType(c, o, OBJ_LIST)) return;

        long llen = listTypeLength(o);
        /* Empty list, move to next key. */
        if (llen == 0) continue;

        /* Pop a range of elements in a nested arrays way. */
        /**以嵌套数组的方式弹出一系列元素。*/
        listPopRangeAndReplyWithKey(c, o, key, where, count, NULL);

        /* Replicate it as [LR]POP COUNT. */
        robj *count_obj = createStringObjectFromLongLong((count > llen) ? llen : count);
        rewriteClientCommandVector(c, 3,
                                   (where == LIST_HEAD) ? shared.lpop : shared.rpop,
                                   key, count_obj);
        decrRefCount(count_obj);
        return;
    }

    /* Look like we are not able to pop up any elements. */
    /**看起来我们无法弹出任何元素。*/
    addReplyNullArray(c);
}

/* LPOP <key> [count] */
void lpopCommand(client *c) {
    popGenericCommand(c,LIST_HEAD);
}

/* RPOP <key> [count] */
void rpopCommand(client *c) {
    popGenericCommand(c,LIST_TAIL);
}

/* LRANGE <key> <start> <stop> */
void lrangeCommand(client *c) {
    robj *o;
    long start, end;

    if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != C_OK) ||
        (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != C_OK)) return;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptyarray)) == NULL
         || checkType(c,o,OBJ_LIST)) return;

    addListRangeReply(c,o,start,end,0);
}

/* LTRIM <key> <start> <stop> */
void ltrimCommand(client *c) {
    robj *o;
    long start, end, llen, ltrim, rtrim;

    if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != C_OK) ||
        (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != C_OK)) return;

    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.ok)) == NULL ||
        checkType(c,o,OBJ_LIST)) return;
    llen = listTypeLength(o);

    /* convert negative indexes */
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. */
    if (start > end || start >= llen) {
        /* Out of range start or start > end result in empty list */
        /**超出范围开始或开始>结束导致空列表*/
        ltrim = llen;
        rtrim = 0;
    } else {
        if (end >= llen) end = llen-1;
        ltrim = start;
        rtrim = llen-end-1;
    }

    /* Remove list elements to perform the trim */
    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklistDelRange(o->ptr,0,ltrim);
        quicklistDelRange(o->ptr,-rtrim,rtrim);
    } else {
        serverPanic("Unknown list encoding");
    }

    notifyKeyspaceEvent(NOTIFY_LIST,"ltrim",c->argv[1],c->db->id);
    if (listTypeLength(o) == 0) {
        dbDelete(c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],c->db->id);
    }
    signalModifiedKey(c,c->db,c->argv[1]);
    server.dirty += (ltrim + rtrim);
    addReply(c,shared.ok);
}

/* LPOS key element [RANK rank] [COUNT num-matches] [MAXLEN len]
 *
 * The "rank" is the position of the match, so if it is 1, the first match
 * is returned, if it is 2 the second match is returned and so forth.
 * It is 1 by default. If negative has the same meaning but the search is
 * performed starting from the end of the list.
 *
 * If COUNT is given, instead of returning the single element, a list of
 * all the matching elements up to "num-matches" are returned. COUNT can
 * be combined with RANK in order to returning only the element starting
 * from the Nth. If COUNT is zero, all the matching elements are returned.
 *
 * MAXLEN tells the command to scan a max of len elements. If zero (the
 * default), all the elements in the list are scanned if needed.
 *
 * The returned elements indexes are always referring to what LINDEX
 * would return. So first element from head is 0, and so forth. */
/**LPOS key element [RANK rank] [COUNT num-matches] [MAXLEN len] “rank”是匹配的位置，所以如果是1，
 * 返回第一个匹配，如果是2，返回第二个匹配等等。默认为 1。如果否定具有相同的含义，但从列表末尾开始执行搜索。
 * 如果给出了 COUNT，则不返回单个元素，而是返回直到“num-matches”的所有匹配元素的列表。
 * COUNT 可以与 RANK 结合使用，以便仅返回从第 N 个开始的元素。如果 COUNT 为零，则返回所有匹配的元素。
 * MAXLEN 告诉命令扫描最大 len 个元素。如果为零（默认值），则在需要时扫描列表中的所有元素。
 * 返回的元素索引始终指的是 LINDEX 将返回的内容。所以 head 的第一个元素是 0，依此类推。*/
void lposCommand(client *c) {
    robj *o, *ele;
    ele = c->argv[2];
    int direction = LIST_TAIL;
    long rank = 1, count = -1, maxlen = 0; /* Count -1: option not given. */

    /* Parse the optional arguments. */
    for (int j = 3; j < c->argc; j++) {
        char *opt = c->argv[j]->ptr;
        int moreargs = (c->argc-1)-j;

        if (!strcasecmp(opt,"RANK") && moreargs) {
            j++;
            if (getLongFromObjectOrReply(c, c->argv[j], &rank, NULL) != C_OK)
                return;
            if (rank == 0) {
                addReplyError(c,"RANK can't be zero: use 1 to start from "
                                "the first match, 2 from the second ... "
                                "or use negative to start from the end of the list");
                return;
            }
        } else if (!strcasecmp(opt,"COUNT") && moreargs) {
            j++;
            if (getPositiveLongFromObjectOrReply(c, c->argv[j], &count,
              "COUNT can't be negative") != C_OK)
                return;
        } else if (!strcasecmp(opt,"MAXLEN") && moreargs) {
            j++;
            if (getPositiveLongFromObjectOrReply(c, c->argv[j], &maxlen, 
              "MAXLEN can't be negative") != C_OK)
                return;
        } else {
            addReplyErrorObject(c,shared.syntaxerr);
            return;
        }
    }

    /* A negative rank means start from the tail. */
    /**负排名意味着从尾部开始。*/
    if (rank < 0) {
        rank = -rank;
        direction = LIST_HEAD;
    }

    /* We return NULL or an empty array if there is no such key (or
     * if we find no matches, depending on the presence of the COUNT option. */
    /**如果没有这样的键（或者如果我们没有找到匹配项，取决于 COUNT 选项的存在，我们将返回 NULL 或空数组。*/
    if ((o = lookupKeyRead(c->db,c->argv[1])) == NULL) {
        if (count != -1)
            addReply(c,shared.emptyarray);
        else
            addReply(c,shared.null[c->resp]);
        return;
    }
    if (checkType(c,o,OBJ_LIST)) return;

    /* If we got the COUNT option, prepare to emit an array. */
    /**如果我们有 COUNT 选项，准备发射一个数组。*/
    void *arraylenptr = NULL;
    if (count != -1) arraylenptr = addReplyDeferredLen(c);

    /* Seek the element. */
    listTypeIterator *li;
    li = listTypeInitIterator(o,direction == LIST_HEAD ? -1 : 0,direction);
    listTypeEntry entry;
    long llen = listTypeLength(o);
    long index = 0, matches = 0, matchindex = -1, arraylen = 0;
    while (listTypeNext(li,&entry) && (maxlen == 0 || index < maxlen)) {
        if (listTypeEqual(&entry,ele)) {
            matches++;
            matchindex = (direction == LIST_TAIL) ? index : llen - index - 1;
            if (matches >= rank) {
                if (arraylenptr) {
                    arraylen++;
                    addReplyLongLong(c,matchindex);
                    if (count && matches-rank+1 >= count) break;
                } else {
                    break;
                }
            }
        }
        index++;
        matchindex = -1; /* Remember if we exit the loop without a match.  请记住，如果我们在没有匹配的情况下退出循环。*/
    }
    listTypeReleaseIterator(li);

    /* Reply to the client. Note that arraylenptr is not NULL only if
     * the COUNT option was selected. */
    if (arraylenptr != NULL) {
        setDeferredArrayLen(c,arraylenptr,arraylen);
    } else {
        if (matchindex != -1)
            addReplyLongLong(c,matchindex);
        else
            addReply(c,shared.null[c->resp]);
    }
}

/* LREM <key> <count> <element> */
void lremCommand(client *c) {
    robj *subject, *obj;
    obj = c->argv[3];
    long toremove;
    long removed = 0;

    if ((getLongFromObjectOrReply(c, c->argv[2], &toremove, NULL) != C_OK))
        return;

    subject = lookupKeyWriteOrReply(c,c->argv[1],shared.czero);
    if (subject == NULL || checkType(c,subject,OBJ_LIST)) return;

    listTypeIterator *li;
    if (toremove < 0) {
        toremove = -toremove;
        li = listTypeInitIterator(subject,-1,LIST_HEAD);
    } else {
        li = listTypeInitIterator(subject,0,LIST_TAIL);
    }

    listTypeEntry entry;
    while (listTypeNext(li,&entry)) {
        if (listTypeEqual(&entry,obj)) {
            listTypeDelete(li, &entry);
            server.dirty++;
            removed++;
            if (toremove && removed == toremove) break;
        }
    }
    listTypeReleaseIterator(li);

    if (removed) {
        signalModifiedKey(c,c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_LIST,"lrem",c->argv[1],c->db->id);
    }

    if (listTypeLength(subject) == 0) {
        dbDelete(c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],c->db->id);
    }

    addReplyLongLong(c,removed);
}

void lmoveHandlePush(client *c, robj *dstkey, robj *dstobj, robj *value,
                     int where) {
    /* Create the list if the key does not exist */
    /**如果密钥不存在，则创建列表*/
    if (!dstobj) {
        dstobj = createQuicklistObject();
        quicklistSetOptions(dstobj->ptr, server.list_max_listpack_size,
                            server.list_compress_depth);
        dbAdd(c->db,dstkey,dstobj);
    }
    signalModifiedKey(c,c->db,dstkey);
    listTypePush(dstobj,value,where);
    notifyKeyspaceEvent(NOTIFY_LIST,
                        where == LIST_HEAD ? "lpush" : "rpush",
                        dstkey,
                        c->db->id);
    /* Always send the pushed value to the client. */
    /**始终将推送的值发送给客户端。*/
    addReplyBulk(c,value);
}

int getListPositionFromObjectOrReply(client *c, robj *arg, int *position) {
    if (strcasecmp(arg->ptr,"right") == 0) {
        *position = LIST_TAIL;
    } else if (strcasecmp(arg->ptr,"left") == 0) {
        *position = LIST_HEAD;
    } else {
        addReplyErrorObject(c,shared.syntaxerr);
        return C_ERR;
    }
    return C_OK;
}

robj *getStringObjectFromListPosition(int position) {
    if (position == LIST_HEAD) {
        return shared.left;
    } else {
        // LIST_TAIL
        return shared.right;
    }
}

void lmoveGenericCommand(client *c, int wherefrom, int whereto) {
    robj *sobj, *value;
    if ((sobj = lookupKeyWriteOrReply(c,c->argv[1],shared.null[c->resp]))
        == NULL || checkType(c,sobj,OBJ_LIST)) return;

    if (listTypeLength(sobj) == 0) {
        /* This may only happen after loading very old RDB files. Recent
         * versions of Redis delete keys of empty lists. */
        /**这可能仅在加载非常旧的 RDB 文件后才会发生。 Redis 的最新版本删除了空列表的键。*/
        addReplyNull(c);
    } else {
        robj *dobj = lookupKeyWrite(c->db,c->argv[2]);
        robj *touchedkey = c->argv[1];

        if (checkType(c,dobj,OBJ_LIST)) return;
        value = listTypePop(sobj,wherefrom);
        serverAssert(value); /* assertion for valgrind        valgrind 的断言（避免 NPD） */
        lmoveHandlePush(c,c->argv[2],dobj,value,whereto);

        /* listTypePop returns an object with its refcount incremented */
        /**listTypePop 返回一个对象，其引用计数递增*/
        decrRefCount(value);

        /* Delete the source list when it is empty 源列表为空时删除*/
        notifyKeyspaceEvent(NOTIFY_LIST,
                            wherefrom == LIST_HEAD ? "lpop" : "rpop",
                            touchedkey,
                            c->db->id);
        if (listTypeLength(sobj) == 0) {
            dbDelete(c->db,touchedkey);
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",
                                touchedkey,c->db->id);
        }
        signalModifiedKey(c,c->db,touchedkey);
        server.dirty++;
        if (c->cmd->proc == blmoveCommand) {
            rewriteClientCommandVector(c,5,shared.lmove,
                                       c->argv[1],c->argv[2],c->argv[3],c->argv[4]);
        } else if (c->cmd->proc == brpoplpushCommand) {
            rewriteClientCommandVector(c,3,shared.rpoplpush,
                                       c->argv[1],c->argv[2]);
        }
    }
}

/* LMOVE <source> <destination> (LEFT|RIGHT) (LEFT|RIGHT) */
void lmoveCommand(client *c) {
    int wherefrom, whereto;
    if (getListPositionFromObjectOrReply(c,c->argv[3],&wherefrom)
        != C_OK) return;
    if (getListPositionFromObjectOrReply(c,c->argv[4],&whereto)
        != C_OK) return;
    lmoveGenericCommand(c, wherefrom, whereto);
}

/* This is the semantic of this command:
 *  RPOPLPUSH srclist dstlist:
 *    IF LLEN(srclist) > 0
 *      element = RPOP srclist
 *      LPUSH dstlist element
 *      RETURN element
 *    ELSE
 *      RETURN nil
 *    END
 *  END
 *
 * The idea is to be able to get an element from a list in a reliable way
 * since the element is not just returned but pushed against another list
 * as well. This command was originally proposed by Ezra Zygmuntowicz.
 * 这个想法是能够以可靠的方式从列表中获取元素，因为该元素不仅被返回，而且还被推送到另一个列表中。
 * 这个命令最初是由 Ezra Zygmuntowicz 提出的。
 */
void rpoplpushCommand(client *c) {
    lmoveGenericCommand(c, LIST_TAIL, LIST_HEAD);
}

/*-----------------------------------------------------------------------------
 * Blocking POP operations
 *----------------------------------------------------------------------------*/

/* This is a helper function for handleClientsBlockedOnKeys(). Its work
 * is to serve a specific client (receiver) that is blocked on 'key'
 * in the context of the specified 'db', doing the following:
 *
 * 1) Provide the client with the 'value' element or a range of elements.
 *    We will do the pop in here and caller does not need to bother the return.
 * 2) If the dstkey is not NULL (we are serving a BLMOVE) also push the
 *    'value' element on the destination list (the "push" side of the command).
 * 3) Propagate the resulting BRPOP, BLPOP, BLMPOP and additional xPUSH if any into
 *    the AOF and replication channel.
 *
 * The argument 'wherefrom' is LIST_TAIL or LIST_HEAD, and indicates if the
 * 'value' element was popped from the head (BLPOP) or tail (BRPOP) so that
 * we can propagate the command properly.
 *
 * The argument 'whereto' is LIST_TAIL or LIST_HEAD, and indicates if the
 * 'value' element is to be pushed to the head or tail so that we can
 * propagate the command properly.
 *
 * 'deleted' is an optional output argument to get an indication
 * if the key got deleted by this function. */
/**
 * 这是 handleClientsBlockedOnKeys() 的辅助函数。
 * 它的工作是为在指定'db'的上下文中被'key'阻塞的特定客户端（接收器）提供服务，执行以下操作：
 *   1）为客户端提供'value'元素或一系列元素。我们将在这里进行弹出，调用者不需要打扰返回。
 *   2) 如果 dstkey 不为 NULL（我们正在为 BLMOVE 提供服务），也将 'value' 元素推送到目标列表中（命令的“推送”端）。
 *   3) 将生成的 BRPOP、BLPOP、BLMPOP 和额外的 xPUSH（如果有）传播到 AOF 和复制通道中。
 * 参数 'wherefrom' 是 LIST_TAIL 或 LIST_HEAD，并指示 'value' 元素是从头部（BLPOP）还是尾部（BRPOP）弹出，
 * 以便我们可以正确地传播命令。参数 'whereto' 是 LIST_TAIL 或 LIST_HEAD，并指示是否要将 'value' 元素推到头部或尾部，
 * 以便我们可以正确地传播命令。 'deleted' 是一个可选的输出参数，用于指示键是否被此函数删除。
 * */
void serveClientBlockedOnList(client *receiver, robj *o, robj *key, robj *dstkey, redisDb *db, int wherefrom, int whereto, int *deleted)
{
    robj *argv[5];
    robj *value = NULL;

    if (deleted) *deleted = 0;

    if (dstkey == NULL) {
        /* Propagate the [LR]POP operation. */
        argv[0] = (wherefrom == LIST_HEAD) ? shared.lpop :
                                             shared.rpop;
        argv[1] = key;

        if (receiver->lastcmd->proc == blmpopCommand) {
            /* Propagate the [LR]POP COUNT operation. */
            long count = receiver->bpop.count;
            serverAssert(count > 0);
            long llen = listTypeLength(o);
            serverAssert(llen > 0);

            argv[2] = createStringObjectFromLongLong((count > llen) ? llen : count);
            alsoPropagate(db->id, argv, 3, PROPAGATE_AOF|PROPAGATE_REPL);
            decrRefCount(argv[2]);

            /* Pop a range of elements in a nested arrays way. */
            listPopRangeAndReplyWithKey(receiver, o, key, wherefrom, count, deleted);
            return;
        }

        alsoPropagate(db->id, argv, 2, PROPAGATE_AOF|PROPAGATE_REPL);

        /* BRPOP/BLPOP */
        value = listTypePop(o, wherefrom);
        serverAssert(value != NULL);

        addReplyArrayLen(receiver,2);
        addReplyBulk(receiver,key);
        addReplyBulk(receiver,value);

        /* Notify event. */
        char *event = (wherefrom == LIST_HEAD) ? "lpop" : "rpop";
        notifyKeyspaceEvent(NOTIFY_LIST,event,key,receiver->db->id);
    } else {
        /* BLMOVE */
        robj *dstobj =
            lookupKeyWrite(receiver->db,dstkey);
        if (!(dstobj &&
             checkType(receiver,dstobj,OBJ_LIST)))
        {
            value = listTypePop(o, wherefrom);
            serverAssert(value != NULL);

            lmoveHandlePush(receiver,dstkey,dstobj,value,whereto);
            /* Propagate the LMOVE/RPOPLPUSH operation. */
            int isbrpoplpush = (receiver->lastcmd->proc == brpoplpushCommand);
            argv[0] = isbrpoplpush ? shared.rpoplpush : shared.lmove;
            argv[1] = key;
            argv[2] = dstkey;
            argv[3] = getStringObjectFromListPosition(wherefrom);
            argv[4] = getStringObjectFromListPosition(whereto);
            alsoPropagate(db->id,argv,(isbrpoplpush ? 3 : 5),PROPAGATE_AOF|PROPAGATE_REPL);

            /* Notify event ("lpush" or "rpush" was notified by lmoveHandlePush). */
            notifyKeyspaceEvent(NOTIFY_LIST,wherefrom == LIST_TAIL ? "rpop" : "lpop",
                                key,receiver->db->id);
        }
    }

    if (value) decrRefCount(value);

    if (listTypeLength(o) == 0) {
        if (deleted) *deleted = 1;

        dbDelete(receiver->db, key);
        notifyKeyspaceEvent(NOTIFY_GENERIC, "del", key, receiver->db->id);
    }
    /* We don't call signalModifiedKey() as it was already called
     * when an element was pushed on the list.
     * 我们不调用 signalModifiedKey() ，因为它在元素被推入列表时已被调用。*/
}

/* Blocking RPOP/LPOP/LMPOP
 *
 * 'numkeys' is the number of keys.
 * 'timeout_idx' parameter position of block timeout.
 * 'where' LIST_HEAD for LEFT, LIST_TAIL for RIGHT.
 * 'count' is the number of elements requested to pop, or -1 for plain single pop.
 *
 * When count is -1, a reply of a single bulk-string will be used.
 * When count > 0, an array reply will be used. */
/**
 * 'numkeys' 是键的数量。 'timeout_idx' 参数块超时位置。 'where' LIST_HEAD 为 LEFT，LIST_TAIL 为 RIGHT
 * 。 'count' 是请求弹出的元素数，或 -1 表示普通的单个弹出。当 count 为 -1 时，将使用单个批量字符串的回复。
 * 当 count > 0 时，将使用数组回复。
 * */
void blockingPopGenericCommand(client *c, robj **keys, int numkeys, int where, int timeout_idx, long count) {
    robj *o;
    robj *key;
    mstime_t timeout;
    int j;

    if (getTimeoutFromObjectOrReply(c,c->argv[timeout_idx],&timeout,UNIT_SECONDS)
        != C_OK) return;

    /* Traverse all input keys, we take action only based on one key. */
    for (j = 0; j < numkeys; j++) {
        key = keys[j];
        o = lookupKeyWrite(c->db, key);

        /* Non-existing key, move to next key. */
        if (o == NULL) continue;

        if (checkType(c, o, OBJ_LIST)) return;

        long llen = listTypeLength(o);
        /* Empty list, move to next key. */
        if (llen == 0) continue;

        if (count != -1) {
            /* BLMPOP, non empty list, like a normal [LR]POP with count option.
             * The difference here we pop a range of elements in a nested arrays way. */
            listPopRangeAndReplyWithKey(c, o, key, where, count, NULL);

            /* Replicate it as [LR]POP COUNT. */
            robj *count_obj = createStringObjectFromLongLong((count > llen) ? llen : count);
            rewriteClientCommandVector(c, 3,
                                       (where == LIST_HEAD) ? shared.lpop : shared.rpop,
                                       key, count_obj);
            decrRefCount(count_obj);
            return;
        }

        /* Non empty list, this is like a normal [LR]POP. */
        robj *value = listTypePop(o,where);
        serverAssert(value != NULL);

        addReplyArrayLen(c,2);
        addReplyBulk(c,key);
        addReplyBulk(c,value);
        decrRefCount(value);
        listElementsRemoved(c,key,where,o,1,NULL);

        /* Replicate it as an [LR]POP instead of B[LR]POP. */
        rewriteClientCommandVector(c,2,
            (where == LIST_HEAD) ? shared.lpop : shared.rpop,
            key);
        return;
    }

    /* If we are not allowed to block the client, the only thing
     * we can do is treating it as a timeout (even with timeout 0). */
    if (c->flags & CLIENT_DENY_BLOCKING) {
        addReplyNullArray(c);
        return;
    }

    /* If the keys do not exist we must block */
    struct blockPos pos = {where};
    blockForKeys(c,BLOCKED_LIST,keys,numkeys,count,timeout,NULL,&pos,NULL);
}

/* BLPOP <key> [<key> ...] <timeout> */
void blpopCommand(client *c) {
    blockingPopGenericCommand(c,c->argv+1,c->argc-2,LIST_HEAD,c->argc-1,-1);
}

/* BRPOP <key> [<key> ...] <timeout> */
void brpopCommand(client *c) {
    blockingPopGenericCommand(c,c->argv+1,c->argc-2,LIST_TAIL,c->argc-1,-1);
}

void blmoveGenericCommand(client *c, int wherefrom, int whereto, mstime_t timeout) {
    robj *key = lookupKeyWrite(c->db, c->argv[1]);
    if (checkType(c,key,OBJ_LIST)) return;

    if (key == NULL) {
        if (c->flags & CLIENT_DENY_BLOCKING) {
            /* Blocking against an empty list when blocking is not allowed
             * returns immediately. */
            addReplyNull(c);
        } else {
            /* The list is empty and the client blocks. */
            struct blockPos pos = {wherefrom, whereto};
            blockForKeys(c,BLOCKED_LIST,c->argv + 1,1,-1,timeout,c->argv[2],&pos,NULL);
        }
    } else {
        /* The list exists and has elements, so
         * the regular lmoveCommand is executed. */
        serverAssertWithInfo(c,key,listTypeLength(key) > 0);
        lmoveGenericCommand(c,wherefrom,whereto);
    }
}

/* BLMOVE <source> <destination> (LEFT|RIGHT) (LEFT|RIGHT) <timeout> */
void blmoveCommand(client *c) {
    mstime_t timeout;
    int wherefrom, whereto;
    if (getListPositionFromObjectOrReply(c,c->argv[3],&wherefrom)
        != C_OK) return;
    if (getListPositionFromObjectOrReply(c,c->argv[4],&whereto)
        != C_OK) return;
    if (getTimeoutFromObjectOrReply(c,c->argv[5],&timeout,UNIT_SECONDS)
        != C_OK) return;
    blmoveGenericCommand(c,wherefrom,whereto,timeout);
}

/* BRPOPLPUSH <source> <destination> <timeout> */
void brpoplpushCommand(client *c) {
    mstime_t timeout;
    if (getTimeoutFromObjectOrReply(c,c->argv[3],&timeout,UNIT_SECONDS)
        != C_OK) return;
    blmoveGenericCommand(c, LIST_TAIL, LIST_HEAD, timeout);
}

/* LMPOP/BLMPOP
 *
 * 'numkeys_idx' parameter position of key number.
 * 'is_block' this indicates whether it is a blocking variant. */
//numkeys_idx' 参数键号位置。 'is_block' 这表明它是否是一个阻塞变体。
void lmpopGenericCommand(client *c, int numkeys_idx, int is_block) {
    long j;
    long numkeys = 0;      /* Number of keys. */
    int where = 0;         /* HEAD for LEFT, TAIL for RIGHT. */
    long count = -1;       /* Reply will consist of up to count elements, depending on the list's length. */

    /* Parse the numkeys. */
    if (getRangeLongFromObjectOrReply(c, c->argv[numkeys_idx], 1, LONG_MAX,
                                      &numkeys, "numkeys should be greater than 0") != C_OK)
        return;

    /* Parse the where. where_idx: the index of where in the c->argv. */
    long where_idx = numkeys_idx + numkeys + 1;
    if (where_idx >= c->argc) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }
    if (getListPositionFromObjectOrReply(c, c->argv[where_idx], &where) != C_OK)
        return;

    /* Parse the optional arguments. */
    for (j = where_idx + 1; j < c->argc; j++) {
        char *opt = c->argv[j]->ptr;
        int moreargs = (c->argc - 1) - j;

        if (count == -1 && !strcasecmp(opt, "COUNT") && moreargs) {
            j++;
            if (getRangeLongFromObjectOrReply(c, c->argv[j], 1, LONG_MAX,
                                              &count,"count should be greater than 0") != C_OK)
                return;
        } else {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
    }

    if (count == -1) count = 1;

    if (is_block) {
        /* BLOCK. We will handle CLIENT_DENY_BLOCKING flag in blockingPopGenericCommand. */
        blockingPopGenericCommand(c, c->argv+numkeys_idx+1, numkeys, where, 1, count);
    } else {
        /* NON-BLOCK */
        mpopGenericCommand(c, c->argv+numkeys_idx+1, numkeys, where, count);
    }
}

/* LMPOP numkeys <key> [<key> ...] (LEFT|RIGHT) [COUNT count] */
void lmpopCommand(client *c) {
    lmpopGenericCommand(c, 1, 0);
}

/* BLMPOP timeout numkeys <key> [<key> ...] (LEFT|RIGHT) [COUNT count] */
void blmpopCommand(client *c) {
    lmpopGenericCommand(c, 2, 1);
}
