/* SORT command and helper functions.
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
#include "pqsort.h" /* Partial qsort for SORT+LIMIT */
#include <math.h> /* isnan() */

zskiplistNode* zslGetElementByRank(zskiplist *zsl, unsigned long rank);

redisSortOperation *createSortOperation(int type, robj *pattern) {
    redisSortOperation *so = zmalloc(sizeof(*so));
    so->type = type;
    so->pattern = pattern;
    return so;
}

/* Return the value associated to the key with a name obtained using
 * the following rules:
 *
 * 1) The first occurrence of '*' in 'pattern' is substituted with 'subst'.
 *
 * 2) If 'pattern' matches the "->" string, everything on the left of
 *    the arrow is treated as the name of a hash field, and the part on the
 *    left as the key name containing a hash. The value of the specified
 *    field is returned.
 *
 * 3) If 'pattern' equals "#", the function simply returns 'subst' itself so
 *    that the SORT command can be used like: SORT key GET # to retrieve
 *    the Set/List elements directly.
 *
 * The returned object will always have its refcount increased by 1
 * when it is non-NULL. */
/**返回与使用以下规则获得的名称的键关联的值：
 *   1) 'pattern' 中第一次出现的 '' 被替换为 'subst'。
 *   2) 如果 'pattern' 匹配 "->" 字符串，则箭头左侧的所有内容都被视为散列字段的名称，左侧的部分作为包含散列的键名。
 *      返回指定字段的值。
 *   3) 如果 'pattern' 等于 ""，该函数简单地返回 'subst' 本身，这样 SORT 命令可以像这样使用：
 *      SORT key GET 直接检索 SetList 元素。
 * 返回的对象在非 NULL 时，其引用计数将始终增加 1。*/
robj *lookupKeyByPattern(redisDb *db, robj *pattern, robj *subst) {
    char *p, *f, *k;
    sds spat, ssub;
    robj *keyobj, *fieldobj = NULL, *o;
    int prefixlen, sublen, postfixlen, fieldlen;

    /* If the pattern is "#" return the substitution object itself in order
     * to implement the "SORT ... GET #" feature. */
    //如果模式是“”，则返回替换对象本身以实现“SORT ... GET”功能。
    spat = pattern->ptr;
    if (spat[0] == '#' && spat[1] == '\0') {
        incrRefCount(subst);
        return subst;
    }

    /* The substitution object may be specially encoded. If so we create
     * a decoded object on the fly. Otherwise getDecodedObject will just
     * increment the ref count, that we'll decrement later. */
    //替换对象可以被特殊编码。如果是这样，我们会即时创建一个解码对象。否则 getDecodedObject 只会增加引用计数，我们稍后会减少。
    subst = getDecodedObject(subst);
    ssub = subst->ptr;

    /* If we can't find '*' in the pattern we return NULL as to GET a
     * fixed key does not make sense. */
    //如果我们在模式中找不到 ''，我们返回 NULL，因为 GET 固定键没有意义。
    p = strchr(spat,'*');
    if (!p) {
        decrRefCount(subst);
        return NULL;
    }

    /* Find out if we're dealing with a hash dereference. */
    //找出我们是否正在处理哈希取消引用。
    if ((f = strstr(p+1, "->")) != NULL && *(f+2) != '\0') {
        fieldlen = sdslen(spat)-(f-spat)-2;
        fieldobj = createStringObject(f+2,fieldlen);
    } else {
        fieldlen = 0;
    }

    /* Perform the '*' substitution. */
    //执行 '' 替换。
    prefixlen = p-spat;
    sublen = sdslen(ssub);
    postfixlen = sdslen(spat)-(prefixlen+1)-(fieldlen ? fieldlen+2 : 0);
    keyobj = createStringObject(NULL,prefixlen+sublen+postfixlen);
    k = keyobj->ptr;
    memcpy(k,spat,prefixlen);
    memcpy(k+prefixlen,ssub,sublen);
    memcpy(k+prefixlen+sublen,p+1,postfixlen);
    decrRefCount(subst); /* Incremented by decodeObject() */

    /* Lookup substituted key 查找替换键*/
    o = lookupKeyRead(db, keyobj);
    if (o == NULL) goto noobj;

    if (fieldobj) {
        if (o->type != OBJ_HASH) goto noobj;

        /* Retrieve value from hash by the field name. The returned object
         * is a new object with refcount already incremented. */
        //通过字段名称从哈希中检索值。返回的对象是一个新对象，其 refcount 已经增加。
        o = hashTypeGetValueObject(o, fieldobj->ptr);
    } else {
        if (o->type != OBJ_STRING) goto noobj;

        /* Every object that this function returns needs to have its refcount
         * increased. sortCommand decreases it again. */
        //此函数返回的每个对象都需要增加其引用计数。 sortCommand 再次减少它。
        incrRefCount(o);
    }
    decrRefCount(keyobj);
    if (fieldobj) decrRefCount(fieldobj);
    return o;

noobj:
    decrRefCount(keyobj);
    if (fieldlen) decrRefCount(fieldobj);
    return NULL;
}

/* sortCompare() is used by qsort in sortCommand(). Given that qsort_r with
 * the additional parameter is not standard but a BSD-specific we have to
 * pass sorting parameters via the global 'server' structure */
/**
 * sortCompare() 由 qsort 在 sortCommand() 中使用。
 * 鉴于带有附加参数的 qsort_r 不是标准的，而是特定于 BSD 的，我们必须通过全局“服务器”结构传递排序参数
 * */
int sortCompare(const void *s1, const void *s2) {
    const redisSortObject *so1 = s1, *so2 = s2;
    int cmp;

    if (!server.sort_alpha) {
        /* Numeric sorting. Here it's trivial as we precomputed scores */
        //数字排序。这很简单，因为我们预先计算了分数
        if (so1->u.score > so2->u.score) {
            cmp = 1;
        } else if (so1->u.score < so2->u.score) {
            cmp = -1;
        } else {
            /* Objects have the same score, but we don't want the comparison
             * to be undefined, so we compare objects lexicographically.
             * This way the result of SORT is deterministic. */
            //对象具有相同的分数，但我们不希望比较未定义，因此我们按字典顺序比较对象。这样 SORT 的结果是确定性的。
            cmp = compareStringObjects(so1->obj,so2->obj);
        }
    } else {
        /* Alphanumeric sorting  字母数字排序*/
        if (server.sort_bypattern) {
            if (!so1->u.cmpobj || !so2->u.cmpobj) {
                /* At least one compare object is NULL 至少一个比较对象为 NULL*/
                if (so1->u.cmpobj == so2->u.cmpobj)
                    cmp = 0;
                else if (so1->u.cmpobj == NULL)
                    cmp = -1;
                else
                    cmp = 1;
            } else {
                /* We have both the objects, compare them. 我们有两个对象，比较它们。*/
                if (server.sort_store) {
                    cmp = compareStringObjects(so1->u.cmpobj,so2->u.cmpobj);
                } else {
                    /* Here we can use strcoll() directly as we are sure that
                     * the objects are decoded string objects. */
                    //在这里我们可以直接使用 strcoll() ，因为我们确定对象是解码的字符串对象。
                    cmp = strcoll(so1->u.cmpobj->ptr,so2->u.cmpobj->ptr);
                }
            }
        } else {
            /* Compare elements directly. 直接比较元素。*/
            if (server.sort_store) {
                cmp = compareStringObjects(so1->obj,so2->obj);
            } else {
                cmp = collateStringObjects(so1->obj,so2->obj);
            }
        }
    }
    return server.sort_desc ? -cmp : cmp;
}

/* The SORT command is the most complex command in Redis. Warning: this code
 * is optimized for speed and a bit less for readability */
//SORT 命令是 Redis 中最复杂的命令。警告：此代码针对速度进行了优化，但在可读性方面有所优化
void sortCommandGeneric(client *c, int readonly) {
    list *operations;
    unsigned int outputlen = 0;
    int desc = 0, alpha = 0;
    long limit_start = 0, limit_count = -1, start, end;
    int j, dontsort = 0, vectorlen;
    int getop = 0; /* GET operation counter */
    int int_conversion_error = 0;
    int syntax_error = 0;
    robj *sortval, *sortby = NULL, *storekey = NULL;
    redisSortObject *vector; /* Resulting vector to sort  要排序的结果向量*/
    int user_has_full_key_access = 0; /* ACL - used in order to verify 'get' and 'by' options can be used
 *                                        ACL - 用于验证 'get' 和 'by' 选项可以使用*/
    /* Create a list of operations to perform for every sorted element.
     * Operations can be GET */
    //为每个已排序的元素创建要执行的操作列表。操作可以是 GET
    operations = listCreate();
    listSetFreeMethod(operations,zfree);
    j = 2; /* options start at argv[2] */

    user_has_full_key_access = ACLUserCheckCmdWithUnrestrictedKeyAccess(c->user, c->cmd, c->argv, c->argc, CMD_KEY_ACCESS);

    /* The SORT command has an SQL-alike syntax, parse it */
    //SORT 命令具有类似 SQL 的语法，解析它
    while(j < c->argc) {
        int leftargs = c->argc-j-1;
        if (!strcasecmp(c->argv[j]->ptr,"asc")) {
            desc = 0;
        } else if (!strcasecmp(c->argv[j]->ptr,"desc")) {
            desc = 1;
        } else if (!strcasecmp(c->argv[j]->ptr,"alpha")) {
            alpha = 1;
        } else if (!strcasecmp(c->argv[j]->ptr,"limit") && leftargs >= 2) {
            if ((getLongFromObjectOrReply(c, c->argv[j+1], &limit_start, NULL)
                 != C_OK) ||
                (getLongFromObjectOrReply(c, c->argv[j+2], &limit_count, NULL)
                 != C_OK))
            {
                syntax_error++;
                break;
            }
            j+=2;
        } else if (readonly == 0 && !strcasecmp(c->argv[j]->ptr,"store") && leftargs >= 1) {
            storekey = c->argv[j+1];
            j++;
        } else if (!strcasecmp(c->argv[j]->ptr,"by") && leftargs >= 1) {
            sortby = c->argv[j+1];
            /* If the BY pattern does not contain '*', i.e. it is constant,
             * we don't need to sort nor to lookup the weight keys. */
            //如果 BY 模式不包含 ''，即它是常量，我们不需要排序也不需要查找权重键。
            if (strchr(c->argv[j+1]->ptr,'*') == NULL) {
                dontsort = 1;
            } else {
                /* If BY is specified with a real pattern, we can't accept
                 * it in cluster mode. */
                //如果 BY 指定了一个真实的模式，我们不能在集群模式下接受它。
                if (server.cluster_enabled) {
                    addReplyError(c,"BY option of SORT denied in Cluster mode.");
                    syntax_error++;
                    break;
                }
                /* If BY is specified with a real pattern, we can't accept
                 * it if no full ACL key access is applied for this command. */
                //如果 BY 以真实模式指定，如果此命令没有应用完整的 ACL 密钥访问，我们将无法接受它。
                if (!user_has_full_key_access) {
                    addReplyError(c,"BY option of SORT denied due to insufficient ACL permissions.");
                    syntax_error++;
                    break;
                }
            }
            j++;
        } else if (!strcasecmp(c->argv[j]->ptr,"get") && leftargs >= 1) {
            if (server.cluster_enabled) {
                addReplyError(c,"GET option of SORT denied in Cluster mode.");
                syntax_error++;
                break;
            }
            if (!user_has_full_key_access) {
                addReplyError(c,"GET option of SORT denied due to insufficient ACL permissions.");
                syntax_error++;
                break;
            }
            listAddNodeTail(operations,createSortOperation(
                SORT_OP_GET,c->argv[j+1]));
            getop++;
            j++;
        } else {
            addReplyErrorObject(c,shared.syntaxerr);
            syntax_error++;
            break;
        }
        j++;
    }

    /* Handle syntax errors set during options parsing. */
    //处理选项解析期间设置的语法错误。
    if (syntax_error) {
        listRelease(operations);
        return;
    }

    /* Lookup the key to sort. It must be of the right types */
    //查找要排序的键。它必须是正确的类型
    sortval = lookupKeyRead(c->db, c->argv[1]);
    if (sortval && sortval->type != OBJ_SET &&
                   sortval->type != OBJ_LIST &&
                   sortval->type != OBJ_ZSET)
    {
        listRelease(operations);
        addReplyErrorObject(c,shared.wrongtypeerr);
        return;
    }

    /* Now we need to protect sortval incrementing its count, in the future
     * SORT may have options able to overwrite/delete keys during the sorting
     * and the sorted key itself may get destroyed */
    //现在我们需要保护 sortval 增加它的计数，将来 SORT 可能有选项能够在排序期间覆盖删除键，并且排序键本身可能会被破坏
    if (sortval)
        incrRefCount(sortval);
    else
        sortval = createQuicklistObject();


    /* When sorting a set with no sort specified, we must sort the output
     * so the result is consistent across scripting and replication.
     *
     * The other types (list, sorted set) will retain their native order
     * even if no sort order is requested, so they remain stable across
     * scripting and replication. */
    /**
     * 在对未指定排序的集合进行排序时，我们必须对输出进行排序，以便结果在脚本和复制之间保持一致。
     * 即使没有请求排序顺序，其他类型（列表、排序集）也将保留其本机顺序，因此它们在脚本和复制过程中保持稳定。
     * */
    if (dontsort &&
        sortval->type == OBJ_SET &&
        (storekey || c->flags & CLIENT_SCRIPT))
    {
        /* Force ALPHA sorting 强制 ALPHA 排序*/
        dontsort = 0;
        alpha = 1;
        sortby = NULL;
    }

    /* Destructively convert encoded sorted sets for SORT. */
    //破坏性地转换编码的排序集以进行排序。
    if (sortval->type == OBJ_ZSET)
        zsetConvert(sortval, OBJ_ENCODING_SKIPLIST);

    /* Obtain the length of the object to sort. */
    //获取要排序的对象的长度。
    switch(sortval->type) {
    case OBJ_LIST: vectorlen = listTypeLength(sortval); break;
    case OBJ_SET: vectorlen =  setTypeSize(sortval); break;
    case OBJ_ZSET: vectorlen = dictSize(((zset*)sortval->ptr)->dict); break;
    default: vectorlen = 0; serverPanic("Bad SORT type"); /* Avoid GCC warning */
    }

    /* Perform LIMIT start,count sanity checking. */
    //执行 LIMIT start，count sanity 检查。
    start = (limit_start < 0) ? 0 : limit_start;
    end = (limit_count < 0) ? vectorlen-1 : start+limit_count-1;
    if (start >= vectorlen) {
        start = vectorlen-1;
        end = vectorlen-2;
    }
    if (end >= vectorlen) end = vectorlen-1;

    /* Whenever possible, we load elements into the output array in a more
     * direct way. This is possible if:
     *
     * 1) The object to sort is a sorted set or a list (internally sorted).
     * 2) There is nothing to sort as dontsort is true (BY <constant string>).
     *
     * In this special case, if we have a LIMIT option that actually reduces
     * the number of elements to fetch, we also optimize to just load the
     * range we are interested in and allocating a vector that is big enough
     * for the selected range length. */
    /**
     * 只要有可能，我们就会以更直接的方式将元素加载到输出数组中。这在以下情况下是可能的：
     *   1) 要排序的对象是排序集或列表（内部排序）。
     *   2) 没有什么可排序的，因为 dontsort 是真的 (BY <constant string>)。在
     * 这种特殊情况下，如果我们有一个实际减少要获取的元素数量的 LIMIT 选项，
     * 我们还会优化以仅加载我们感兴趣的范围并分配一个足够大的向量来满足所选范围的长度。
     * */
    if ((sortval->type == OBJ_ZSET || sortval->type == OBJ_LIST) &&
        dontsort &&
        (start != 0 || end != vectorlen-1))
    {
        vectorlen = end-start+1;
    }

    /* Load the sorting vector with all the objects to sort */
    //加载所有要排序的对象的排序向量
    vector = zmalloc(sizeof(redisSortObject)*vectorlen);
    j = 0;

    if (sortval->type == OBJ_LIST && dontsort) {
        /* Special handling for a list, if 'dontsort' is true.
         * This makes sure we return elements in the list original
         * ordering, accordingly to DESC / ASC options.
         *
         * Note that in this case we also handle LIMIT here in a direct
         * way, just getting the required range, as an optimization. */
        /**
         * 如果 'dontsort' 为真，则对列表进行特殊处理。这确保我们根据 DESC ASC 选项返回列表原始排序中的元素。
         * 请注意，在这种情况下，我们在这里也直接处理 LIMIT ，只是获得所需的范围，作为优化。
         * */
        if (end >= start) {
            listTypeIterator *li;
            listTypeEntry entry;
            li = listTypeInitIterator(sortval,
                    desc ? (long)(listTypeLength(sortval) - start - 1) : start,
                    desc ? LIST_HEAD : LIST_TAIL);

            while(j < vectorlen && listTypeNext(li,&entry)) {
                vector[j].obj = listTypeGet(&entry);
                vector[j].u.score = 0;
                vector[j].u.cmpobj = NULL;
                j++;
            }
            listTypeReleaseIterator(li);
            /* Fix start/end: output code is not aware of this optimization. */
            //修复 start/end：输出代码不知道此优化。
            end -= start;
            start = 0;
        }
    } else if (sortval->type == OBJ_LIST) {
        listTypeIterator *li = listTypeInitIterator(sortval,0,LIST_TAIL);
        listTypeEntry entry;
        while(listTypeNext(li,&entry)) {
            vector[j].obj = listTypeGet(&entry);
            vector[j].u.score = 0;
            vector[j].u.cmpobj = NULL;
            j++;
        }
        listTypeReleaseIterator(li);
    } else if (sortval->type == OBJ_SET) {
        setTypeIterator *si = setTypeInitIterator(sortval);
        sds sdsele;
        while((sdsele = setTypeNextObject(si)) != NULL) {
            vector[j].obj = createObject(OBJ_STRING,sdsele);
            vector[j].u.score = 0;
            vector[j].u.cmpobj = NULL;
            j++;
        }
        setTypeReleaseIterator(si);
    } else if (sortval->type == OBJ_ZSET && dontsort) {
        /* Special handling for a sorted set, if 'dontsort' is true.
         * This makes sure we return elements in the sorted set original
         * ordering, accordingly to DESC / ASC options.
         *
         * Note that in this case we also handle LIMIT here in a direct
         * way, just getting the required range, as an optimization. */
        /**
         * 如果 'dontsort' 为真，则对排序集进行特殊处理。这确保我们根据 DESC ASC 选项返回有序集合原始排序中的元素。
         * 请注意，在这种情况下，我们在这里也直接处理 LIMIT ，只是获得所需的范围，作为优化。
         * */

        zset *zs = sortval->ptr;
        zskiplist *zsl = zs->zsl;
        zskiplistNode *ln;
        sds sdsele;
        int rangelen = vectorlen;

        /* Check if starting point is trivial, before doing log(N) lookup. */
        //在进行 log(N) 查找之前检查起点是否微不足道。
        if (desc) {
            long zsetlen = dictSize(((zset*)sortval->ptr)->dict);

            ln = zsl->tail;
            if (start > 0)
                ln = zslGetElementByRank(zsl,zsetlen-start);
        } else {
            ln = zsl->header->level[0].forward;
            if (start > 0)
                ln = zslGetElementByRank(zsl,start+1);
        }

        while(rangelen--) {
            serverAssertWithInfo(c,sortval,ln != NULL);
            sdsele = ln->ele;
            vector[j].obj = createStringObject(sdsele,sdslen(sdsele));
            vector[j].u.score = 0;
            vector[j].u.cmpobj = NULL;
            j++;
            ln = desc ? ln->backward : ln->level[0].forward;
        }
        /* Fix start/end: output code is not aware of this optimization. */
        //修复 startend：输出代码不知道此优化。
        end -= start;
        start = 0;
    } else if (sortval->type == OBJ_ZSET) {
        dict *set = ((zset*)sortval->ptr)->dict;
        dictIterator *di;
        dictEntry *setele;
        sds sdsele;
        di = dictGetIterator(set);
        while((setele = dictNext(di)) != NULL) {
            sdsele =  dictGetKey(setele);
            vector[j].obj = createStringObject(sdsele,sdslen(sdsele));
            vector[j].u.score = 0;
            vector[j].u.cmpobj = NULL;
            j++;
        }
        dictReleaseIterator(di);
    } else {
        serverPanic("Unknown type");
    }
    serverAssertWithInfo(c,sortval,j == vectorlen);

    /* Now it's time to load the right scores in the sorting vector */
    //现在是时候在排序向量中加载正确的分数了
    if (!dontsort) {
        for (j = 0; j < vectorlen; j++) {
            robj *byval;
            if (sortby) {
                /* lookup value to sort by 要排序的查找值 */
                byval = lookupKeyByPattern(c->db,sortby,vector[j].obj);
                if (!byval) continue;
            } else {
                /* use object itself to sort by  使用对象本身进行排序*/
                byval = vector[j].obj;
            }

            if (alpha) {
                if (sortby) vector[j].u.cmpobj = getDecodedObject(byval);
            } else {
                if (sdsEncodedObject(byval)) {
                    char *eptr;

                    vector[j].u.score = strtod(byval->ptr,&eptr);
                    if (eptr[0] != '\0' || errno == ERANGE ||
                        isnan(vector[j].u.score))
                    {
                        int_conversion_error = 1;
                    }
                } else if (byval->encoding == OBJ_ENCODING_INT) {
                    /* Don't need to decode the object if it's
                     * integer-encoded (the only encoding supported) so
                     * far. We can just cast it */
                    //到目前为止，如果它是整数编码的（唯一支持的编码），则不需要解码对象。我们可以投它
                    vector[j].u.score = (long)byval->ptr;
                } else {
                    serverAssertWithInfo(c,sortval,1 != 1);
                }
            }

            /* when the object was retrieved using lookupKeyByPattern,
             * its refcount needs to be decreased. */
            //当使用lookupKeyByPattern 检索对象时，需要减少其引用计数。
            if (sortby) {
                decrRefCount(byval);
            }
        }

        server.sort_desc = desc;
        server.sort_alpha = alpha;
        server.sort_bypattern = sortby ? 1 : 0;
        server.sort_store = storekey ? 1 : 0;
        if (sortby && (start != 0 || end != vectorlen-1))
            pqsort(vector,vectorlen,sizeof(redisSortObject),sortCompare, start,end);
        else
            qsort(vector,vectorlen,sizeof(redisSortObject),sortCompare);
    }

    /* Send command output to the output buffer, performing the specified
     * GET/DEL/INCR/DECR operations if any. */
    //将命令输出发送到输出缓冲区，执行指定的 GETDELINCRDECR 操作（如果有）。
    outputlen = getop ? getop*(end-start+1) : end-start+1;
    if (int_conversion_error) {
        addReplyError(c,"One or more scores can't be converted into double");
    } else if (storekey == NULL) {
        /* STORE option not specified, sent the sorting result to client */
        //未指定 STORE 选项，将排序结果发送给客户端
        addReplyArrayLen(c,outputlen);
        for (j = start; j <= end; j++) {
            listNode *ln;
            listIter li;

            if (!getop) addReplyBulk(c,vector[j].obj);
            listRewind(operations,&li);
            while((ln = listNext(&li))) {
                redisSortOperation *sop = ln->value;
                robj *val = lookupKeyByPattern(c->db,sop->pattern,
                                               vector[j].obj);

                if (sop->type == SORT_OP_GET) {
                    if (!val) {
                        addReplyNull(c);
                    } else {
                        addReplyBulk(c,val);
                        decrRefCount(val);
                    }
                } else {
                    /* Always fails */
                    serverAssertWithInfo(c,sortval,sop->type == SORT_OP_GET);
                }
            }
        }
    } else {
        robj *sobj = createQuicklistObject();

        /* STORE option specified, set the sorting result as a List object */
        //指定 STORE 选项，将排序结果设置为 List 对象
        for (j = start; j <= end; j++) {
            listNode *ln;
            listIter li;

            if (!getop) {
                listTypePush(sobj,vector[j].obj,LIST_TAIL);
            } else {
                listRewind(operations,&li);
                while((ln = listNext(&li))) {
                    redisSortOperation *sop = ln->value;
                    robj *val = lookupKeyByPattern(c->db,sop->pattern,
                                                   vector[j].obj);

                    if (sop->type == SORT_OP_GET) {
                        if (!val) val = createStringObject("",0);

                        /* listTypePush does an incrRefCount, so we should take care
                         * care of the incremented refcount caused by either
                         * lookupKeyByPattern or createStringObject("",0) */
                        /**
                         * listTypePush 做了一个 incrRefCount，所以我们应该注意由 lookupKeyByPattern
                         * 或 createStringObject("",0) 引起的增加的 refcount
                         * */
                        listTypePush(sobj,val,LIST_TAIL);
                        decrRefCount(val);
                    } else {
                        /* Always fails */
                        serverAssertWithInfo(c,sortval,sop->type == SORT_OP_GET);
                    }
                }
            }
        }
        if (outputlen) {
            setKey(c,c->db,storekey,sobj,0);
            notifyKeyspaceEvent(NOTIFY_LIST,"sortstore",storekey,
                                c->db->id);
            server.dirty += outputlen;
        } else if (dbDelete(c->db,storekey)) {
            signalModifiedKey(c,c->db,storekey);
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",storekey,c->db->id);
            server.dirty++;
        }
        decrRefCount(sobj);
        addReplyLongLong(c,outputlen);
    }

    /* Cleanup */
    for (j = 0; j < vectorlen; j++)
        decrRefCount(vector[j].obj);

    decrRefCount(sortval);
    listRelease(operations);
    for (j = 0; j < vectorlen; j++) {
        if (alpha && vector[j].u.cmpobj)
            decrRefCount(vector[j].u.cmpobj);
    }
    zfree(vector);
}

/* SORT wrapper function for read-only mode. */
//只读模式的 SORT 包装函数。
void sortroCommand(client *c) {
    sortCommandGeneric(c, 1);
}

void sortCommand(client *c) {
    sortCommandGeneric(c, 0);
}
