/*
 * Copyright (c) 2009-2021, Redis Labs Ltd.
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
#include "call_reply.h"

#define REPLY_FLAG_ROOT (1<<0)
#define REPLY_FLAG_PARSED (1<<1)
#define REPLY_FLAG_RESP3 (1<<2)

/* --------------------------------------------------------
 * An opaque struct used to parse a RESP protocol reply and
 * represent it. Used when parsing replies such as in RM_Call
 * or Lua scripts.
 * //用于解析 RESP 协议回复并表示它的不透明结构。在解析回复时使用，例如在 RM_Call 或 Lua 脚本中。
 * -------------------------------------------------------- */
struct CallReply {
    void *private_data;
    sds original_proto; /* Available only for root reply. 仅适用于根回复。*/
    const char *proto;
    size_t proto_len;
    int type;       /* REPLY_... */
    int flags;      /* REPLY_FLAG... */
    size_t len;     /* Length of a string, or the number elements in an array. 字符串的长度，或数组中的数字元素。*/
    union {
        const char *str; /* String pointer for string and error replies. This
                          * does not need to be freed, always points inside
                          * a reply->proto buffer of the reply object or, in
                          * case of array elements, of parent reply objects.
                          * 字符串和错误回复的字符串指针。这不需要被释放，它总是指向回复对象的回复->原型缓冲区，
                          * 或者如果是数组元素，则指向父回复对象。*/
        struct {
            const char *str;
            const char *format;
        } verbatim_str;  /* Reply value for verbatim string 逐字字符串的回复值*/
        long long ll;    /* Reply value for integer reply. */
        double d;        /* Reply value for double reply. */
        struct CallReply *array; /* Array of sub-reply elements. used for set, array, map, and attribute
 *                                  子回复元素的数组。用于集合、数组、映射和属性*/
    } val;
    list *deferred_error_list;   /* list of errors in sds form or NULL  sds 形式的错误列表或 NULL*/
    struct CallReply *attribute; /* attribute reply, NULL if not exists 属性回复，如果不存在则为 NULL*/
};

static void callReplySetSharedData(CallReply *rep, int type, const char *proto, size_t proto_len, int extra_flags) {
    rep->type = type;
    rep->proto = proto;
    rep->proto_len = proto_len;
    rep->flags |= extra_flags;
}

static void callReplyNull(void *ctx, const char *proto, size_t proto_len) {
    CallReply *rep = ctx;
    callReplySetSharedData(rep, REDISMODULE_REPLY_NULL, proto, proto_len, REPLY_FLAG_RESP3);
}

static void callReplyNullBulkString(void *ctx, const char *proto, size_t proto_len) {
    CallReply *rep = ctx;
    callReplySetSharedData(rep, REDISMODULE_REPLY_NULL, proto, proto_len, 0);
}

static void callReplyNullArray(void *ctx, const char *proto, size_t proto_len) {
    CallReply *rep = ctx;
    callReplySetSharedData(rep, REDISMODULE_REPLY_NULL, proto, proto_len, 0);
}

static void callReplyBulkString(void *ctx, const char *str, size_t len, const char *proto, size_t proto_len) {
    CallReply *rep = ctx;
    callReplySetSharedData(rep, REDISMODULE_REPLY_STRING, proto, proto_len, 0);
    rep->len = len;
    rep->val.str = str;
}

static void callReplyError(void *ctx, const char *str, size_t len, const char *proto, size_t proto_len) {
    CallReply *rep = ctx;
    callReplySetSharedData(rep, REDISMODULE_REPLY_ERROR, proto, proto_len, 0);
    rep->len = len;
    rep->val.str = str;
}

static void callReplySimpleStr(void *ctx, const char *str, size_t len, const char *proto, size_t proto_len) {
    CallReply *rep = ctx;
    callReplySetSharedData(rep, REDISMODULE_REPLY_STRING, proto, proto_len, 0);
    rep->len = len;
    rep->val.str = str;
}

static void callReplyLong(void *ctx, long long val, const char *proto, size_t proto_len) {
    CallReply *rep = ctx;
    callReplySetSharedData(rep, REDISMODULE_REPLY_INTEGER, proto, proto_len, 0);
    rep->val.ll = val;
}

static void callReplyDouble(void *ctx, double val, const char *proto, size_t proto_len) {
    CallReply *rep = ctx;
    callReplySetSharedData(rep, REDISMODULE_REPLY_DOUBLE, proto, proto_len, REPLY_FLAG_RESP3);
    rep->val.d = val;
}

static void callReplyVerbatimString(void *ctx, const char *format, const char *str, size_t len, const char *proto, size_t proto_len) {
    CallReply *rep = ctx;
    callReplySetSharedData(rep, REDISMODULE_REPLY_VERBATIM_STRING, proto, proto_len, REPLY_FLAG_RESP3);
    rep->len = len;
    rep->val.verbatim_str.str = str;
    rep->val.verbatim_str.format = format;
}

static void callReplyBigNumber(void *ctx, const char *str, size_t len, const char *proto, size_t proto_len) {
    CallReply *rep = ctx;
    callReplySetSharedData(rep, REDISMODULE_REPLY_BIG_NUMBER, proto, proto_len, REPLY_FLAG_RESP3);
    rep->len = len;
    rep->val.str = str;
}

static void callReplyBool(void *ctx, int val, const char *proto, size_t proto_len) {
    CallReply *rep = ctx;
    callReplySetSharedData(rep, REDISMODULE_REPLY_BOOL, proto, proto_len, REPLY_FLAG_RESP3);
    rep->val.ll = val;
}

static void callReplyParseCollection(ReplyParser *parser, CallReply *rep, size_t len, const char *proto, size_t elements_per_entry) {
    rep->len = len;
    rep->val.array = zcalloc(elements_per_entry * len * sizeof(CallReply));
    for (size_t i = 0; i < len * elements_per_entry; i += elements_per_entry) {
        for (size_t j = 0 ; j < elements_per_entry ; ++j) {
            rep->val.array[i + j].private_data = rep->private_data;
            parseReply(parser, rep->val.array + i + j);
            rep->val.array[i + j].flags |= REPLY_FLAG_PARSED;
            if (rep->val.array[i + j].flags & REPLY_FLAG_RESP3) {
                /* If one of the sub-replies is RESP3, then the current reply is also RESP3. */
                //如果子回复之一是 RESP3，则当前回复也是 RESP3。
                rep->flags |= REPLY_FLAG_RESP3;
            }
        }
    }
    rep->proto = proto;
    rep->proto_len = parser->curr_location - proto;
}

static void callReplyAttribute(ReplyParser *parser, void *ctx, size_t len, const char *proto) {
    CallReply *rep = ctx;
    rep->attribute = zcalloc(sizeof(CallReply));

    /* Continue parsing the attribute reply 继续解析属性回复*/
    rep->attribute->len = len;
    rep->attribute->type = REDISMODULE_REPLY_ATTRIBUTE;
    callReplyParseCollection(parser, rep->attribute, len, proto, 2);
    rep->attribute->flags |= REPLY_FLAG_PARSED | REPLY_FLAG_RESP3;
    rep->attribute->private_data = rep->private_data;

    /* Continue parsing the reply 继续解析回复*/
    parseReply(parser, rep);

    /* In this case we need to fix the proto address and len, it should start from the attribute */
    //在这种情况下我们需要修复proto地址和len，它应该从属性开始
    rep->proto = proto;
    rep->proto_len = parser->curr_location - proto;
    rep->flags |= REPLY_FLAG_RESP3;
}

static void callReplyArray(ReplyParser *parser, void *ctx, size_t len, const char *proto) {
    CallReply *rep = ctx;
    rep->type = REDISMODULE_REPLY_ARRAY;
    callReplyParseCollection(parser, rep, len, proto, 1);
}

static void callReplySet(ReplyParser *parser, void *ctx, size_t len, const char *proto) {
    CallReply *rep = ctx;
    rep->type = REDISMODULE_REPLY_SET;
    callReplyParseCollection(parser, rep, len, proto, 1);
    rep->flags |= REPLY_FLAG_RESP3;
}

static void callReplyMap(ReplyParser *parser, void *ctx, size_t len, const char *proto) {
    CallReply *rep = ctx;
    rep->type = REDISMODULE_REPLY_MAP;
    callReplyParseCollection(parser, rep, len, proto, 2);
    rep->flags |= REPLY_FLAG_RESP3;
}

static void callReplyParseError(void *ctx) {
    CallReply *rep = ctx;
    rep->type = REDISMODULE_REPLY_UNKNOWN;
}

/* Recursively free the current call reply and its sub-replies. */
//递归释放当前呼叫回复及其子回复。
static void freeCallReplyInternal(CallReply *rep) {
    if (rep->type == REDISMODULE_REPLY_ARRAY || rep->type == REDISMODULE_REPLY_SET) {
        for (size_t i = 0 ; i < rep->len ; ++i) {
            freeCallReplyInternal(rep->val.array + i);
        }
        zfree(rep->val.array);
    }

    if (rep->type == REDISMODULE_REPLY_MAP || rep->type == REDISMODULE_REPLY_ATTRIBUTE) {
        for (size_t i = 0 ; i < rep->len ; ++i) {
            freeCallReplyInternal(rep->val.array + i * 2);
            freeCallReplyInternal(rep->val.array + i * 2 + 1);
        }
        zfree(rep->val.array);
    }

    if (rep->attribute) {
        freeCallReplyInternal(rep->attribute);
        zfree(rep->attribute);
    }
}

/* Free the given call reply and its children (in case of nested reply) recursively.
 * If private data was set when the CallReply was created it will not be freed, as it's
 * the caller's responsibility to free it before calling freeCallReply(). */
/**
 * 递归地释放给定的呼叫回复及其子项（在嵌套回复的情况下）。
 * 如果在创建 CallReply 时设置了私有数据，它将不会被释放，因为调用者有责任在调用 freeCallReply() 之前释放它。
 * */
void freeCallReply(CallReply *rep) {
    if (!(rep->flags & REPLY_FLAG_ROOT)) {
        return;
    }
    if (rep->flags & REPLY_FLAG_PARSED) {
        freeCallReplyInternal(rep);
    }
    sdsfree(rep->original_proto);
    if (rep->deferred_error_list)
        listRelease(rep->deferred_error_list);
    zfree(rep);
}

static const ReplyParserCallbacks DefaultParserCallbacks = {
    .null_callback = callReplyNull,
    .bulk_string_callback = callReplyBulkString,
    .null_bulk_string_callback = callReplyNullBulkString,
    .null_array_callback = callReplyNullArray,
    .error_callback = callReplyError,
    .simple_str_callback = callReplySimpleStr,
    .long_callback = callReplyLong,
    .array_callback = callReplyArray,
    .set_callback = callReplySet,
    .map_callback = callReplyMap,
    .double_callback = callReplyDouble,
    .bool_callback = callReplyBool,
    .big_number_callback = callReplyBigNumber,
    .verbatim_string_callback = callReplyVerbatimString,
    .attribute_callback = callReplyAttribute,
    .error = callReplyParseError,
};

/* Parse the buffer located in rep->original_proto and update the CallReply
 * structure to represent its contents. */
//解析位于 rep->original_proto 中的缓冲区并更新 CallReply 结构以表示其内容。
static void callReplyParse(CallReply *rep) {
    if (rep->flags & REPLY_FLAG_PARSED) {
        return;
    }

    ReplyParser parser = {.curr_location = rep->proto, .callbacks = DefaultParserCallbacks};

    parseReply(&parser, rep);
    rep->flags |= REPLY_FLAG_PARSED;
}

/* Return the call reply type (REDISMODULE_REPLY_...). */
int callReplyType(CallReply *rep) {
    if (!rep) return REDISMODULE_REPLY_UNKNOWN;
    callReplyParse(rep);
    return rep->type;
}

/* Return reply string as buffer and len. Applicable to:
 * - REDISMODULE_REPLY_STRING
 * - REDISMODULE_REPLY_ERROR
 *
 * The return value is borrowed from CallReply, so it must not be freed
 * explicitly or used after CallReply itself is freed.
 *
 * The returned value is not NULL terminated and its length is returned by
 * reference through len, which must not be NULL.
 */
/**
 * 返回值是从 CallReply 借来的，所以它不能被显式释放或在 CallReply 本身被释放后使用。
 * 返回值不是以NULL结尾的，它的长度是通过len引用返回的，len不能为NULL。
 * */
const char *callReplyGetString(CallReply *rep, size_t *len) {
    callReplyParse(rep);
    if (rep->type != REDISMODULE_REPLY_STRING &&
        rep->type != REDISMODULE_REPLY_ERROR) return NULL;
    if (len) *len = rep->len;
    return rep->val.str;
}

/* Return a long long reply value. Applicable to:
 * - REDISMODULE_REPLY_INTEGER
 */
long long callReplyGetLongLong(CallReply *rep) {
    callReplyParse(rep);
    if (rep->type != REDISMODULE_REPLY_INTEGER) return LLONG_MIN;
    return rep->val.ll;
}

/* Return a double reply value. Applicable to:
 * - REDISMODULE_REPLY_DOUBLE
 */
double callReplyGetDouble(CallReply *rep) {
    callReplyParse(rep);
    if (rep->type != REDISMODULE_REPLY_DOUBLE) return LLONG_MIN;
    return rep->val.d;
}

/* Return a reply Boolean value. Applicable to:
 * - REDISMODULE_REPLY_BOOL
 */
int callReplyGetBool(CallReply *rep) {
    callReplyParse(rep);
    if (rep->type != REDISMODULE_REPLY_BOOL) return INT_MIN;
    return rep->val.ll;
}

/* Return reply length. Applicable to:
 * - REDISMODULE_REPLY_STRING
 * - REDISMODULE_REPLY_ERROR
 * - REDISMODULE_REPLY_ARRAY
 * - REDISMODULE_REPLY_SET
 * - REDISMODULE_REPLY_MAP
 * - REDISMODULE_REPLY_ATTRIBUTE
 */
size_t callReplyGetLen(CallReply *rep) {
    callReplyParse(rep);
    switch(rep->type) {
        case REDISMODULE_REPLY_STRING:
        case REDISMODULE_REPLY_ERROR:
        case REDISMODULE_REPLY_ARRAY:
        case REDISMODULE_REPLY_SET:
        case REDISMODULE_REPLY_MAP:
        case REDISMODULE_REPLY_ATTRIBUTE:
            return rep->len;
        default:
            return 0;
    }
}

static CallReply *callReplyGetCollectionElement(CallReply *rep, size_t idx, int elements_per_entry) {
    if (idx >= rep->len * elements_per_entry) return NULL; // real len is rep->len * elements_per_entry
    return rep->val.array+idx;
}

/* Return a reply array element at a given index. Applicable to:
 * - REDISMODULE_REPLY_ARRAY
 *
 * The return value is borrowed from CallReply, so it must not be freed
 * explicitly or used after CallReply itself is freed.
 */
/**
 * 返回给定索引处的回复数组元素。适用于：
 * - REDISMODULE_REPLY_ARRAY
 * 返回值是从 CallReply 借来的，所以不能显式释放，也不能在 CallReply 本身释放后使用。
 * */
CallReply *callReplyGetArrayElement(CallReply *rep, size_t idx) {
    callReplyParse(rep);
    if (rep->type != REDISMODULE_REPLY_ARRAY) return NULL;
    return callReplyGetCollectionElement(rep, idx, 1);
}

/* Return a reply set element at a given index. Applicable to:
 * - REDISMODULE_REPLY_SET
 *
 * The return value is borrowed from CallReply, so it must not be freed
 * explicitly or used after CallReply itself is freed.
 */
/** 返回给定索引处的回复集元素。适用于：
 * - REDISMODULE_REPLY_SET
 * 返回值是从 CallReply 借来的，所以不能显式释放，也不能在 CallReply 本身释放后使用。*/
CallReply *callReplyGetSetElement(CallReply *rep, size_t idx) {
    callReplyParse(rep);
    if (rep->type != REDISMODULE_REPLY_SET) return NULL;
    return callReplyGetCollectionElement(rep, idx, 1);
}

static int callReplyGetMapElementInternal(CallReply *rep, size_t idx, CallReply **key, CallReply **val, int type) {
    callReplyParse(rep);
    if (rep->type != type) return C_ERR;
    if (idx >= rep->len) return C_ERR;
    if (key) *key = callReplyGetCollectionElement(rep, idx * 2, 2);
    if (val) *val = callReplyGetCollectionElement(rep, idx * 2 + 1, 2);
    return C_OK;
}

/* Retrieve a map reply key and value at a given index. Applicable to:
 * - REDISMODULE_REPLY_MAP
 *
 * The key and value are returned by reference through key and val,
 * which may also be NULL if not needed.
 *
 * Returns C_OK on success or C_ERR if reply type mismatches, or if idx is out
 * of range.
 *
 * The returned values are borrowed from CallReply, so they must not be freed
 * explicitly or used after CallReply itself is freed.
 */
/**在给定索引处检索地图回复键和值。适用于：
 * - REDISMODULE_REPLY_MAP
 * 通过key和val引用返回的key和value，如果不需要也可以为NULL。
 * 如果回复类型不匹配或 idx 超出范围，则成功返回 C_OK 或 C_ERR。
 * 返回的值是从 CallReply 借来的，因此它们不能被显式释放或在 CallReply 本身被释放后使用。 */
int callReplyGetMapElement(CallReply *rep, size_t idx, CallReply **key, CallReply **val) {
    return callReplyGetMapElementInternal(rep, idx, key, val, REDISMODULE_REPLY_MAP);
}

/* Return reply attribute, or NULL if it does not exist. Applicable to all replies.
 *
 * The returned values are borrowed from CallReply, so they must not be freed
 * explicitly or used after CallReply itself is freed.
 */
/**返回回复属性，如果不存在则返回 NULL。适用于所有回复。
 * 返回的值是从 CallReply 借来的，因此它们不能被显式释放或在 CallReply 本身被释放后使用。*/
CallReply *callReplyGetAttribute(CallReply *rep) {
    return rep->attribute;
}

/* Retrieve attribute reply key and value at a given index. Applicable to:
 * - REDISMODULE_REPLY_ATTRIBUTE
 *
 * The key and value are returned by reference through key and val,
 * which may also be NULL if not needed.
 *
 * Returns C_OK on success or C_ERR if reply type mismatches, or if idx is out
 * of range.
 *
 * The returned values are borrowed from CallReply, so they must not be freed
 * explicitly or used after CallReply itself is freed.
 */
/**在给定索引处检索属性回复键和值。适用于：
 * - REDISMODULE_REPLY_ATTRIBUTE
 * key和 value 通过 key 和 val 引用返回，如果不需要也可以为 NULL。
 * 如果回复类型不匹配或 idx 超出范围，则成功返回 C_OK 或 C_ERR。
 * 返回的值是从 CallReply 借来的，因此它们不能被显式释放或在 CallReply 本身被释放后使用。*/
int callReplyGetAttributeElement(CallReply *rep, size_t idx, CallReply **key, CallReply **val) {
    return callReplyGetMapElementInternal(rep, idx, key, val, REDISMODULE_REPLY_MAP);
}

/* Return a big number reply value. Applicable to:
 * - REDISMODULE_REPLY_BIG_NUMBER
 *
 * The returned values are borrowed from CallReply, so they must not be freed
 * explicitly or used after CallReply itself is freed.
 *
 * The return value is guaranteed to be a big number, as described in the RESP3
 * protocol specifications.
 *
 * The returned value is not NULL terminated and its length is returned by
 * reference through len, which must not be NULL.
 */
/**返回一个大数字回复值。适用于：
 * - REDISMODULE_REPLY_BIG_NUMBER
 * 返回值是从 CallReply 借来的，所以不能显式释放，也不能在 CallReply 本身释放后使用。
 * 如 RESP3 协议规范中所述，返回值保证为大数。返回值不是以NULL结尾的，它的长度是通过len引用返回的，len不能为NULL。*/
const char *callReplyGetBigNumber(CallReply *rep, size_t *len) {
    callReplyParse(rep);
    if (rep->type != REDISMODULE_REPLY_BIG_NUMBER) return NULL;
    *len = rep->len;
    return rep->val.str;
}

/* Return a verbatim string reply value. Applicable to:
 * - REDISMODULE_REPLY_VERBATIM_STRING
 *
 * If format is non-NULL, the verbatim reply format is also returned by value.
 *
 * The optional output argument can be given to get a verbatim reply
 * format, or can be set NULL if not needed.
 *
 * The return value is borrowed from CallReply, so it must not be freed
 * explicitly or used after CallReply itself is freed.
 *
 * The returned value is not NULL terminated and its length is returned by
 * reference through len, which must not be NULL.
 */
/**返回逐字字符串回复值。适用于：
 * - REDISMODULE_REPLY_VERBATIM_STRING
 * 如果格式为非NULL，则逐字回复格式也按值返回。可以给出可选的输出参数以获取逐字回复格式，或者如果不需要，可以将其设置为 NULL。
 * 返回值是从 CallReply 借来的，所以它不能被显式释放或在 CallReply 本身被释放后使用。
 * 返回值不是以NULL结尾的，它的长度是通过len引用返回的，len不能为NULL。 */
const char *callReplyGetVerbatim(CallReply *rep, size_t *len, const char **format){
    callReplyParse(rep);
    if (rep->type != REDISMODULE_REPLY_VERBATIM_STRING) return NULL;
    *len = rep->len;
    if (format) *format = rep->val.verbatim_str.format;
    return rep->val.verbatim_str.str;
}

/* Return the current reply blob.
 *
 * The return value is borrowed from CallReply, so it must not be freed
 * explicitly or used after CallReply itself is freed.
 */
/**返回当前回复 blob。返回值是从 CallReply 借来的，所以它不能被显式释放或在 CallReply 本身被释放后使用。*/
const char *callReplyGetProto(CallReply *rep, size_t *proto_len) {
    *proto_len = rep->proto_len;
    return rep->proto;
}

/* Return CallReply private data, as set by the caller on callReplyCreate().
 */
//返回 CallReply 私有数据，由调用者在 callReplyCreate() 上设置。
void *callReplyGetPrivateData(CallReply *rep) {
    return rep->private_data;
}

/* Return true if the reply or one of it sub-replies is RESP3 formatted. */
//如果回复或其子回复之一是 RESP3 格式，则返回 true。
int callReplyIsResp3(CallReply *rep) {
    return rep->flags & REPLY_FLAG_RESP3;
}

/* Returns a list of errors in sds form, or NULL. */
//以 sds 形式返回错误列表，或 NULL。
list *callReplyDeferredErrorList(CallReply *rep) {
    return rep->deferred_error_list;
}

/* Create a new CallReply struct from the reply blob.
 *
 * The function will own the reply blob, so it must not be used or freed by
 * the caller after passing it to this function.
 *
 * The reply blob will be freed when the returned CallReply struct is later
 * freed using freeCallReply().
 *
 * The deferred_error_list is an optional list of errors that are present
 * in the reply blob, if given, this function will take ownership on it.
 *
 * The private_data is optional and can later be accessed using
 * callReplyGetPrivateData().
 *
 * NOTE: The parser used for parsing the reply and producing CallReply is
 * designed to handle valid replies created by Redis itself. IT IS NOT
 * DESIGNED TO HANDLE USER INPUT and using it to parse invalid replies is
 * unsafe.
 */
/**从回复 blob 创建一个新的 CallReply 结构。该函数将拥有回复 blob，因此在将其传递给此函数后，调用者不得使用或释放它。
 * 稍后使用 freeCallReply() 释放返回的 CallReply 结构时，将释放回复 blob。
 * deferred_error_list 是存在于回复 blob 中的可选错误列表，如果给定，此函数将对其拥有所有权。
 * private_data 是可选的，以后可以使用 callReplyGetPrivateData() 访问。
 * 注意：用于解析回复和生成 CallReply 的解析器旨在处理 Redis 自身创建的有效回复。
 * 它不是为处理用户输入而设计的，使用它来解析无效回复是不安全的。*/
CallReply *callReplyCreate(sds reply, list *deferred_error_list, void *private_data) {
    CallReply *res = zmalloc(sizeof(*res));
    res->flags = REPLY_FLAG_ROOT;
    res->original_proto = reply;
    res->proto = reply;
    res->proto_len = sdslen(reply);
    res->private_data = private_data;
    res->attribute = NULL;
    res->deferred_error_list = deferred_error_list;
    return res;
}

/* Create a new CallReply struct from the reply blob representing an error message.
 * Automatically creating deferred_error_list and set a copy of the reply in it.
 * Refer to callReplyCreate for detailed explanation.
 * Reply string can come in one of two forms:
 * 1. A protocol reply starting with "-CODE" and ending with "\r\n"
 * 2. A plain string, in which case this function adds the protocol header and footer. */
/**
 * 从表示错误消息的回复 blob 创建一个新的 CallReply 结构。自动创建 deferred_error_list 并在其中设置回复的副本。
 * 详细解释请参考 callReplyCreate。回复字符串可以采用以下两种形式之一：
 *   1. 以“-CODE”开头并以“\r\n”结尾的协议回复
 *   2. 纯字符串，在这种情况下，此函数添加协议页眉和页脚。
 * */
CallReply *callReplyCreateError(sds reply, void *private_data) {
    sds err_buff = reply;
    if (err_buff[0] != '-') {
        err_buff = sdscatfmt(sdsempty(), "-ERR %S\r\n", reply);
        sdsfree(reply);
    }
    list *deferred_error_list = listCreate();
    listSetFreeMethod(deferred_error_list, (void (*)(void*))sdsfree);
    listAddNodeTail(deferred_error_list, sdsnew(err_buff));
    return callReplyCreate(err_buff, deferred_error_list, private_data);
}
