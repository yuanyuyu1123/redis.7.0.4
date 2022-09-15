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

#ifndef __RDB_H
#define __RDB_H

#include <stdio.h>
#include "rio.h"

/* TBD: include only necessary headers. */
//待定：仅包括必要的标题。
#include "server.h"

/* The current RDB version. When the format changes in a way that is no longer
 * backward compatible this number gets incremented. */
//当前的 RDB 版本。当格式以不再向后兼容的方式更改时，此数字会增加。
#define RDB_VERSION 10

/* Defines related to the dump file format. To store 32 bits lengths for short
 * keys requires a lot of space, so we check the most significant 2 bits of
 * the first byte to interpreter the length:
 *
 * 00|XXXXXX => if the two MSB are 00 the len is the 6 bits of this byte
 * 01|XXXXXX XXXXXXXX =>  01, the len is 14 bits, 6 bits + 8 bits of next byte
 * 10|000000 [32 bit integer] => A full 32 bit len in net byte order will follow
 * 10|000001 [64 bit integer] => A full 64 bit len in net byte order will follow
 * 11|OBKIND this means: specially encoded object will follow. The six bits
 *           number specify the kind of object that follows.
 *           See the RDB_ENC_* defines.
 *
 * Lengths up to 63 are stored using a single byte, most DB keys, and may
 * values, will fit inside. */
/**
 * 与转储文件格式相关的定义。为短密钥存储 32 位长度需要大量空间，因此我们检查第一个字节的最高有效 2 位来解释长度：
 * 00|XXXXXX => 如果两个 MSB 为 00，则 len 是这个的 6 位字节
 * 01|XXXXXX XXXXXXXXX => 01，长度为 14 位，6 位 + 下一个字节的 8 位
 * 10|000000 [32 位整数] => 净字节顺序中的完整 32 位 len 将遵循
 * 10|000001 [64 位integer] => 一个完整的 64 位 len 净字节顺序将跟随
 * 11|OBKIND 这意味着：特殊编码的对象将跟随。六位数字指定后面的对象类型。请参阅 RDB_ENC_ 定义。
 * 最多 63 的长度使用单个字节存储，大多数 DB 键和可能的值都可以放入其中。
 * */
#define RDB_6BITLEN 0
#define RDB_14BITLEN 1
#define RDB_32BITLEN 0x80
#define RDB_64BITLEN 0x81
#define RDB_ENCVAL 3
#define RDB_LENERR UINT64_MAX

/* When a length of a string object stored on disk has the first two bits
 * set, the remaining six bits specify a special encoding for the object
 * accordingly to the following defines: */
//当存储在磁盘上的字符串对象的长度设置了前两位时，其余六位根据以下定义为对象指定特殊编码：
#define RDB_ENC_INT8 0        /* 8 bit signed integer */
#define RDB_ENC_INT16 1       /* 16 bit signed integer */
#define RDB_ENC_INT32 2       /* 32 bit signed integer */
#define RDB_ENC_LZF 3         /* string compressed with FASTLZ 用 FASTLZ 压缩的字符串*/

/* Map object types to RDB object types. Macros starting with OBJ_ are for
 * memory storage and may change. Instead RDB types must be fixed because
 * we store them on disk. */
//将对象类型映射到 RDB 对象类型。以 OBJ_ 开头的宏用于内存存储，可能会发生变化。
// 相反，RDB 类型必须是固定的，因为我们将它们存储在磁盘上。
#define RDB_TYPE_STRING 0
#define RDB_TYPE_LIST   1
#define RDB_TYPE_SET    2
#define RDB_TYPE_ZSET   3
#define RDB_TYPE_HASH   4
#define RDB_TYPE_ZSET_2 5 /* ZSET version 2 with doubles stored in binary. ZSET 版本 2 以二进制形式存储双精度数。*/
#define RDB_TYPE_MODULE 6
#define RDB_TYPE_MODULE_2 7 /* Module value with annotations for parsing without
                               the generating module being loaded.
                               带有注释的模块值，用于在不加载生成模块的情况下进行解析。*/

/* NOTE: WHEN ADDING NEW RDB TYPE, UPDATE rdbIsObjectType() BELOW
 * 注意：添加新的 RDB 类型时，请在下面更新 rdbIsObjectType()*/

/* Object types for encoded objects.  编码对象的对象类型。*/
#define RDB_TYPE_HASH_ZIPMAP    9
#define RDB_TYPE_LIST_ZIPLIST  10
#define RDB_TYPE_SET_INTSET    11
#define RDB_TYPE_ZSET_ZIPLIST  12
#define RDB_TYPE_HASH_ZIPLIST  13
#define RDB_TYPE_LIST_QUICKLIST 14
#define RDB_TYPE_STREAM_LISTPACKS 15
#define RDB_TYPE_HASH_LISTPACK 16
#define RDB_TYPE_ZSET_LISTPACK 17
#define RDB_TYPE_LIST_QUICKLIST_2   18
#define RDB_TYPE_STREAM_LISTPACKS_2 19
/* NOTE: WHEN ADDING NEW RDB TYPE, UPDATE rdbIsObjectType() BELOW
 * 注意：添加新的 RDB 类型时，请在下面更新 rdbIsObjectType()*/

/* Test if a type is an object type. 测试一个类型是否是一个对象类型。 */
#define rdbIsObjectType(t) ((t >= 0 && t <= 7) || (t >= 9 && t <= 19))

/* Special RDB opcodes (saved/loaded with rdbSaveType/rdbLoadType). */
//特殊的 RDB 操作码（使用 rdbSaveTyperdbLoadType 保存加载）。
#define RDB_OPCODE_FUNCTION2  245   /* function library data 函数库数据*/
#define RDB_OPCODE_FUNCTION   246   /* old function library data for 7.0 rc1 and rc2 7.0  rc1 和 rc2 的旧函数库数据*/
#define RDB_OPCODE_MODULE_AUX 247   /* Module auxiliary data. 模块辅助数据。*/
#define RDB_OPCODE_IDLE       248   /* LRU idle time. LRU 空闲时间。*/
#define RDB_OPCODE_FREQ       249   /* LFU frequency. LFU 频率。*/
#define RDB_OPCODE_AUX        250   /* RDB aux field. RDB 到字段。*/
#define RDB_OPCODE_RESIZEDB   251   /* Hash table resize hint. 哈希表调整大小提示。*/
#define RDB_OPCODE_EXPIRETIME_MS 252    /* Expire time in milliseconds. 以毫秒为单位的过期时间。*/
#define RDB_OPCODE_EXPIRETIME 253       /* Old expire time in seconds. 旧过期时间（以秒为单位）。*/
#define RDB_OPCODE_SELECTDB   254   /* DB number of the following keys. 以下键的 DB 编号。*/
#define RDB_OPCODE_EOF        255   /* End of the RDB file. RDB 文件的结尾。*/

/* Module serialized values sub opcodes  模块序列化值子操作码*/
#define RDB_MODULE_OPCODE_EOF   0   /* End of module value. 模块值结束。 */
#define RDB_MODULE_OPCODE_SINT  1   /* Signed integer. */
#define RDB_MODULE_OPCODE_UINT  2   /* Unsigned integer. */
#define RDB_MODULE_OPCODE_FLOAT 3   /* Float. */
#define RDB_MODULE_OPCODE_DOUBLE 4  /* Double. */
#define RDB_MODULE_OPCODE_STRING 5  /* String. */

/* rdbLoad...() functions flags. */
//rdbLoad...() 函数标志。
#define RDB_LOAD_NONE   0
#define RDB_LOAD_ENC    (1<<0)
#define RDB_LOAD_PLAIN  (1<<1)
#define RDB_LOAD_SDS    (1<<2)

/* flags on the purpose of rdb save or load */
//用于 rdb 保存或加载的标志
#define RDBFLAGS_NONE 0                 /* No special RDB loading. 没有特殊的 RDB 加载。*/
#define RDBFLAGS_AOF_PREAMBLE (1<<0)    /* Load/save the RDB as AOF preamble. 将 RDB 加载保存为 AOF 前导码。*/
#define RDBFLAGS_REPLICATION (1<<1)     /* Load/save for SYNC. 同步的负载保存。*/
#define RDBFLAGS_ALLOW_DUP (1<<2)       /* Allow duplicated keys when loading. 加载时允许重复键。*/
#define RDBFLAGS_FEED_REPL (1<<3)       /* Feed replication stream when loading. 加载时提供复制流。*/

/* When rdbLoadObject() returns NULL, the err flag is
 * set to hold the type of error that occurred */
//当 rdbLoadObject() 返回 NULL 时，设置 err 标志以保存发生的错误类型
#define RDB_LOAD_ERR_EMPTY_KEY  1   /* Error of empty key 空键错误*/
#define RDB_LOAD_ERR_OTHER      2   /* Any other errors 任何其他错误*/

int rdbSaveType(rio *rdb, unsigned char type);
int rdbLoadType(rio *rdb);
time_t rdbLoadTime(rio *rdb);
int rdbSaveLen(rio *rdb, uint64_t len);
int rdbSaveMillisecondTime(rio *rdb, long long t);
long long rdbLoadMillisecondTime(rio *rdb, int rdbver);
uint64_t rdbLoadLen(rio *rdb, int *isencoded);
int rdbLoadLenByRef(rio *rdb, int *isencoded, uint64_t *lenptr);
int rdbSaveObjectType(rio *rdb, robj *o);
int rdbLoadObjectType(rio *rdb);
int rdbLoad(char *filename, rdbSaveInfo *rsi, int rdbflags);
int rdbSaveBackground(int req, char *filename, rdbSaveInfo *rsi);
int rdbSaveToSlavesSockets(int req, rdbSaveInfo *rsi);
void rdbRemoveTempFile(pid_t childpid, int from_signal);
int rdbSave(int req, char *filename, rdbSaveInfo *rsi);
ssize_t rdbSaveObject(rio *rdb, robj *o, robj *key, int dbid);
size_t rdbSavedObjectLen(robj *o, robj *key, int dbid);
robj *rdbLoadObject(int type, rio *rdb, sds key, int dbid, int *error);
void backgroundSaveDoneHandler(int exitcode, int bysignal);
int rdbSaveKeyValuePair(rio *rdb, robj *key, robj *val, long long expiretime,int dbid);
ssize_t rdbSaveSingleModuleAux(rio *rdb, int when, moduleType *mt);
robj *rdbLoadCheckModuleValue(rio *rdb, char *modulename);
robj *rdbLoadStringObject(rio *rdb);
ssize_t rdbSaveStringObject(rio *rdb, robj *obj);
ssize_t rdbSaveRawString(rio *rdb, unsigned char *s, size_t len);
void *rdbGenericLoadStringObject(rio *rdb, int flags, size_t *lenptr);
int rdbSaveBinaryDoubleValue(rio *rdb, double val);
int rdbLoadBinaryDoubleValue(rio *rdb, double *val);
int rdbSaveBinaryFloatValue(rio *rdb, float val);
int rdbLoadBinaryFloatValue(rio *rdb, float *val);
int rdbLoadRio(rio *rdb, int rdbflags, rdbSaveInfo *rsi);
int rdbLoadRioWithLoadingCtx(rio *rdb, int rdbflags, rdbSaveInfo *rsi, rdbLoadingCtx *rdb_loading_ctx);
int rdbFunctionLoad(rio *rdb, int ver, functionsLibCtx* lib_ctx, int type, int rdbflags, sds *err);
int rdbSaveRio(int req, rio *rdb, int *error, int rdbflags, rdbSaveInfo *rsi);
ssize_t rdbSaveFunctions(rio *rdb);
rdbSaveInfo *rdbPopulateSaveInfo(rdbSaveInfo *rsi);

#endif
