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
#include "lzf.h"    /* LZF compression library */
#include "zipmap.h"
#include "endianconv.h"
#include "stream.h"
#include "functions.h"

#include <math.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/param.h>

/* This macro is called when the internal RDB structure is corrupt */
/**当内部 RDB 结构损坏时调用此宏*/
#define rdbReportCorruptRDB(...) rdbReportError(1, __LINE__,__VA_ARGS__)
/* This macro is called when RDB read failed (possibly a short read) */
/**当 RDB 读取失败（可能是短读取）时调用此宏*/
#define rdbReportReadError(...) rdbReportError(0, __LINE__,__VA_ARGS__)

/* This macro tells if we are in the context of a RESTORE command, and not loading an RDB or AOF. */
/**这个宏告诉我们是否在 RESTORE 命令的上下文中，而不是加载 RDB 或 AOF。*/
#define isRestoreContext() \
    (server.current_client == NULL || server.current_client->id == CLIENT_ID_AOF) ? 0 : 1

char* rdbFileBeingLoaded = NULL; /* used for rdb checking on read error 用于 rdb 检查读取错误 */
extern int rdbCheckMode;
void rdbCheckError(const char *fmt, ...);
void rdbCheckSetError(const char *fmt, ...);

#ifdef __GNUC__
void rdbReportError(int corruption_error, int linenum, char *reason, ...) __attribute__ ((format (printf, 3, 4)));
#endif
void rdbReportError(int corruption_error, int linenum, char *reason, ...) {
    va_list ap;
    char msg[1024];
    int len;

    len = snprintf(msg,sizeof(msg),
        "Internal error in RDB reading offset %llu, function at rdb.c:%d -> ",
        (unsigned long long)server.loading_loaded_bytes, linenum);
    va_start(ap,reason);
    vsnprintf(msg+len,sizeof(msg)-len,reason,ap);
    va_end(ap);

    if (isRestoreContext()) {
        /* If we're in the context of a RESTORE command, just propagate the error. */
        /* log in VERBOSE, and return (don't exit). */
        /**如果我们在 RESTORE 命令的上下文中，只需传播错误。登录 VERBOSE，然后返回（不要退出）。*/
        serverLog(LL_VERBOSE, "%s", msg);
        return;
    } else if (rdbCheckMode) {
        /* If we're inside the rdb checker, let it handle the error. */
        /**如果我们在 rdb 检查器中，让它处理错误。*/
        rdbCheckError("%s",msg);
    } else if (rdbFileBeingLoaded) {
        /* If we're loading an rdb file form disk, run rdb check (and exit) */
        /**如果我们从磁盘加载一个 rdb 文件，运行 rdb check（并退出）*/
        serverLog(LL_WARNING, "%s", msg);
        char *argv[2] = {"",rdbFileBeingLoaded};
        redis_check_rdb_main(2,argv,NULL);
    } else if (corruption_error) {
        /* In diskless loading, in case of corrupt file, log and exit. */
        /**在无盘加载中，如果文件损坏，记录并退出。*/
        serverLog(LL_WARNING, "%s. Failure loading rdb format", msg);
    } else {
        /* In diskless loading, in case of a short read (not a corrupt
         * file), log and proceed (don't exit). */
        /**在无盘加载中，如果读取时间较短（不是损坏的文件），请记录并继续（不要退出）。*/
        serverLog(LL_WARNING, "%s. Failure loading rdb format from socket, assuming connection error, resuming operation.", msg);
        return;
    }
    serverLog(LL_WARNING, "Terminating server after rdb file reading failure.");
    exit(1);
}

static ssize_t rdbWriteRaw(rio *rdb, void *p, size_t len) {
    if (rdb && rioWrite(rdb,p,len) == 0)
        return -1;
    return len;
}

int rdbSaveType(rio *rdb, unsigned char type) {
    return rdbWriteRaw(rdb,&type,1);
}

/* Load a "type" in RDB format, that is a one byte unsigned integer.
 * This function is not only used to load object types, but also special
 * "types" like the end-of-file type, the EXPIRE type, and so forth. */
/**加载 RDB 格式的“类型”，即单字节无符号整数。此函数不仅用于加载对象类型，
 * 还用于加载特殊“类型”，如文件结尾类型、EXPIRE 类型等。*/
int rdbLoadType(rio *rdb) {
    unsigned char type;
    if (rioRead(rdb,&type,1) == 0) return -1;
    return type;
}

/* This is only used to load old databases stored with the RDB_OPCODE_EXPIRETIME
 * opcode. New versions of Redis store using the RDB_OPCODE_EXPIRETIME_MS
 * opcode. On error -1 is returned, however this could be a valid time, so
 * to check for loading errors the caller should call rioGetReadError() after
 * calling this function. */
/**这仅用于加载使用 RDB_OPCODE_EXPIRETIME 操作码存储的旧数据库。
 * 新版本的 Redis 存储使用 RDB_OPCODE_EXPIRETIME_MS 操作码。
 * 返回错误 -1，但这可能是一个有效时间，因此要检查加载错误，调用者应在调用此函数后调用 rioGetReadError()。*/
time_t rdbLoadTime(rio *rdb) {
    int32_t t32;
    if (rioRead(rdb,&t32,4) == 0) return -1;
    return (time_t)t32;
}

int rdbSaveMillisecondTime(rio *rdb, long long t) {
    int64_t t64 = (int64_t) t;
    memrev64ifbe(&t64); /* Store in little endian. */
    return rdbWriteRaw(rdb,&t64,8);
}

/* This function loads a time from the RDB file. It gets the version of the
 * RDB because, unfortunately, before Redis 5 (RDB version 9), the function
 * failed to convert data to/from little endian, so RDB files with keys having
 * expires could not be shared between big endian and little endian systems
 * (because the expire time will be totally wrong). The fix for this is just
 * to call memrev64ifbe(), however if we fix this for all the RDB versions,
 * this call will introduce an incompatibility for big endian systems:
 * after upgrading to Redis version 5 they will no longer be able to load their
 * own old RDB files. Because of that, we instead fix the function only for new
 * RDB versions, and load older RDB versions as we used to do in the past,
 * allowing big endian systems to load their own old RDB files.
 *
 * On I/O error the function returns LLONG_MAX, however if this is also a
 * valid stored value, the caller should use rioGetReadError() to check for
 * errors after calling this function. */
/**此函数从 RDB 文件加载时间。它获取了 RDB 的版本，因为不幸的是，在 Redis 5（RDB 版本 9）之前，
 * 该函数无法将数据从 little endian 转换，因此具有过期键的 RDB 文件
 * 无法在 big endian 和 little endian 系统之间共享（因为过期时间将完全错误）。
 * 对此的修复只是调用 memrev64ifbe()，但是如果我们为所有 RDB 版本修复此问题，
 * 此调用将引入大端系统的不兼容性：升级到 Redis 版本 5 后，它们将不再能够加载自己的旧的 RDB 文件。
 * 因此，我们只为新的 RDB 版本修复该功能，并像过去一样加载旧的 RDB 版本，允许大端系统加载自己的旧 RDB 文件。
 * 在 IO 错误时，该函数返回 LLONG_MAX，但是如果这也是一个有效的存储值，
 * 则调用者应在调用此函数后使用 rioGetReadError() 检查错误。*/
long long rdbLoadMillisecondTime(rio *rdb, int rdbver) {
    int64_t t64;
    if (rioRead(rdb,&t64,8) == 0) return LLONG_MAX;
    if (rdbver >= 9) /* Check the top comment of this function. 检查此功能的顶部评论。*/
        memrev64ifbe(&t64); /* Convert in big endian if the system is BE. 如果系统是 BE，则转换为大端。*/
    return (long long)t64;
}

/* Saves an encoded length. The first two bits in the first byte are used to
 * hold the encoding type. See the RDB_* definitions for more information
 * on the types of encoding. */
/**保存编码长度。第一个字节中的前两位用于保存编码类型。有关编码类型的更多信息，请参阅 RDB_ 定义。*/
int rdbSaveLen(rio *rdb, uint64_t len) {
    unsigned char buf[2];
    size_t nwritten;

    if (len < (1<<6)) {
        /* Save a 6 bit len */
        buf[0] = (len&0xFF)|(RDB_6BITLEN<<6);
        if (rdbWriteRaw(rdb,buf,1) == -1) return -1;
        nwritten = 1;
    } else if (len < (1<<14)) {
        /* Save a 14 bit len */
        buf[0] = ((len>>8)&0xFF)|(RDB_14BITLEN<<6);
        buf[1] = len&0xFF;
        if (rdbWriteRaw(rdb,buf,2) == -1) return -1;
        nwritten = 2;
    } else if (len <= UINT32_MAX) {
        /* Save a 32 bit len */
        buf[0] = RDB_32BITLEN;
        if (rdbWriteRaw(rdb,buf,1) == -1) return -1;
        uint32_t len32 = htonl(len);
        if (rdbWriteRaw(rdb,&len32,4) == -1) return -1;
        nwritten = 1+4;
    } else {
        /* Save a 64 bit len */
        buf[0] = RDB_64BITLEN;
        if (rdbWriteRaw(rdb,buf,1) == -1) return -1;
        len = htonu64(len);
        if (rdbWriteRaw(rdb,&len,8) == -1) return -1;
        nwritten = 1+8;
    }
    return nwritten;
}


/* Load an encoded length. If the loaded length is a normal length as stored
 * with rdbSaveLen(), the read length is set to '*lenptr'. If instead the
 * loaded length describes a special encoding that follows, then '*isencoded'
 * is set to 1 and the encoding format is stored at '*lenptr'.
 *
 * See the RDB_ENC_* definitions in rdb.h for more information on special
 * encodings.
 *
 * The function returns -1 on error, 0 on success. */
/**加载编码长度。如果加载的长度是使用 rdbSaveLen() 存储的正常长度，则读取长度设置为“lenptr”。
 * 如果加载的长度描述了随后的特殊编码，则“isencoded”设置为 1，并且编码格式存储在“lenptr”中。
 * 有关特殊编码的更多信息，请参阅 rdb.h 中的 RDB_ENC_ 定义。该函数在错误时返回 -1，在成功时返回 0。*/
int rdbLoadLenByRef(rio *rdb, int *isencoded, uint64_t *lenptr) {
    unsigned char buf[2];
    int type;

    if (isencoded) *isencoded = 0;
    if (rioRead(rdb,buf,1) == 0) return -1;
    type = (buf[0]&0xC0)>>6;
    if (type == RDB_ENCVAL) {
        /* Read a 6 bit encoding type. */
        if (isencoded) *isencoded = 1;
        *lenptr = buf[0]&0x3F;
    } else if (type == RDB_6BITLEN) {
        /* Read a 6 bit len. */
        *lenptr = buf[0]&0x3F;
    } else if (type == RDB_14BITLEN) {
        /* Read a 14 bit len. */
        if (rioRead(rdb,buf+1,1) == 0) return -1;
        *lenptr = ((buf[0]&0x3F)<<8)|buf[1];
    } else if (buf[0] == RDB_32BITLEN) {
        /* Read a 32 bit len. */
        uint32_t len;
        if (rioRead(rdb,&len,4) == 0) return -1;
        *lenptr = ntohl(len);
    } else if (buf[0] == RDB_64BITLEN) {
        /* Read a 64 bit len. */
        uint64_t len;
        if (rioRead(rdb,&len,8) == 0) return -1;
        *lenptr = ntohu64(len);
    } else {
        rdbReportCorruptRDB(
            "Unknown length encoding %d in rdbLoadLen()",type);
        return -1; /* Never reached. */
    }
    return 0;
}

/* This is like rdbLoadLenByRef() but directly returns the value read
 * from the RDB stream, signaling an error by returning RDB_LENERR
 * (since it is a too large count to be applicable in any Redis data
 * structure). */
/**这与 rdbLoadLenByRef() 类似，但直接返回从 RDB 流中读取的值，
 * 通过返回 RDB_LENERR 来表示错误（因为它的计数太大而无法应用于任何 Redis 数据结构）。*/
uint64_t rdbLoadLen(rio *rdb, int *isencoded) {
    uint64_t len;

    if (rdbLoadLenByRef(rdb,isencoded,&len) == -1) return RDB_LENERR;
    return len;
}

/* Encodes the "value" argument as integer when it fits in the supported ranges
 * for encoded types. If the function successfully encodes the integer, the
 * representation is stored in the buffer pointer to by "enc" and the string
 * length is returned. Otherwise 0 is returned. */
/**当它适合编码类型的支持范围时，将“值”参数编码为整数。
 * 如果函数成功地对整数进行编码，则表示将存储在“enc”指向的缓冲区指针中，并返回字符串长度。否则返回 0。*/
int rdbEncodeInteger(long long value, unsigned char *enc) {
    if (value >= -(1<<7) && value <= (1<<7)-1) {
        enc[0] = (RDB_ENCVAL<<6)|RDB_ENC_INT8;
        enc[1] = value&0xFF;
        return 2;
    } else if (value >= -(1<<15) && value <= (1<<15)-1) {
        enc[0] = (RDB_ENCVAL<<6)|RDB_ENC_INT16;
        enc[1] = value&0xFF;
        enc[2] = (value>>8)&0xFF;
        return 3;
    } else if (value >= -((long long)1<<31) && value <= ((long long)1<<31)-1) {
        enc[0] = (RDB_ENCVAL<<6)|RDB_ENC_INT32;
        enc[1] = value&0xFF;
        enc[2] = (value>>8)&0xFF;
        enc[3] = (value>>16)&0xFF;
        enc[4] = (value>>24)&0xFF;
        return 5;
    } else {
        return 0;
    }
}

/* Loads an integer-encoded object with the specified encoding type "enctype".
 * The returned value changes according to the flags, see
 * rdbGenericLoadStringObject() for more info. */
/**加载具有指定编码类型“enctype”的整数编码对象。返回值根据标志而变化，
 * 有关更多信息，请参阅 rdbGenericLoadStringObject()。*/
void *rdbLoadIntegerObject(rio *rdb, int enctype, int flags, size_t *lenptr) {
    int plain = flags & RDB_LOAD_PLAIN;
    int sds = flags & RDB_LOAD_SDS;
    int encode = flags & RDB_LOAD_ENC;
    unsigned char enc[4];
    long long val;

    if (enctype == RDB_ENC_INT8) {
        if (rioRead(rdb,enc,1) == 0) return NULL;
        val = (signed char)enc[0];
    } else if (enctype == RDB_ENC_INT16) {
        uint16_t v;
        if (rioRead(rdb,enc,2) == 0) return NULL;
        v = ((uint32_t)enc[0])|
            ((uint32_t)enc[1]<<8);
        val = (int16_t)v;
    } else if (enctype == RDB_ENC_INT32) {
        uint32_t v;
        if (rioRead(rdb,enc,4) == 0) return NULL;
        v = ((uint32_t)enc[0])|
            ((uint32_t)enc[1]<<8)|
            ((uint32_t)enc[2]<<16)|
            ((uint32_t)enc[3]<<24);
        val = (int32_t)v;
    } else {
        rdbReportCorruptRDB("Unknown RDB integer encoding type %d",enctype);
        return NULL; /* Never reached. */
    }
    if (plain || sds) {
        char buf[LONG_STR_SIZE], *p;
        int len = ll2string(buf,sizeof(buf),val);
        if (lenptr) *lenptr = len;
        p = plain ? zmalloc(len) : sdsnewlen(SDS_NOINIT,len);
        memcpy(p,buf,len);
        return p;
    } else if (encode) {
        return createStringObjectFromLongLongForValue(val);
    } else {
        return createObject(OBJ_STRING,sdsfromlonglong(val));
    }
}

/* String objects in the form "2391" "-100" without any space and with a
 * range of values that can fit in an 8, 16 or 32 bit signed value can be
 * encoded as integers to save space */
/**“2391”“-100”形式的字符串对象，没有任何空格，并且具有可以适合 8、16 或 32 位有符号值的值范围，可以编码为整数以节省空间*/
int rdbTryIntegerEncoding(char *s, size_t len, unsigned char *enc) {
    long long value;
    if (string2ll(s, len, &value)) {
        return rdbEncodeInteger(value, enc);
    } else {
        return 0;
    }
}

ssize_t rdbSaveLzfBlob(rio *rdb, void *data, size_t compress_len,
                       size_t original_len) {
    unsigned char byte;
    ssize_t n, nwritten = 0;

    /* Data compressed! Let's save it on disk */
    byte = (RDB_ENCVAL<<6)|RDB_ENC_LZF;
    if ((n = rdbWriteRaw(rdb,&byte,1)) == -1) goto writeerr;
    nwritten += n;

    if ((n = rdbSaveLen(rdb,compress_len)) == -1) goto writeerr;
    nwritten += n;

    if ((n = rdbSaveLen(rdb,original_len)) == -1) goto writeerr;
    nwritten += n;

    if ((n = rdbWriteRaw(rdb,data,compress_len)) == -1) goto writeerr;
    nwritten += n;

    return nwritten;

writeerr:
    return -1;
}

ssize_t rdbSaveLzfStringObject(rio *rdb, unsigned char *s, size_t len) {
    size_t comprlen, outlen;
    void *out;

    /* We require at least four bytes compression for this to be worth it */
    /**我们需要至少四个字节压缩才能做到这一点*/
    if (len <= 4) return 0;
    outlen = len-4;
    if ((out = zmalloc(outlen+1)) == NULL) return 0;
    comprlen = lzf_compress(s, len, out, outlen);
    if (comprlen == 0) {
        zfree(out);
        return 0;
    }
    ssize_t nwritten = rdbSaveLzfBlob(rdb, out, comprlen, len);
    zfree(out);
    return nwritten;
}

/* Load an LZF compressed string in RDB format. The returned value
 * changes according to 'flags'. For more info check the
 * rdbGenericLoadStringObject() function. */
/**加载 RDB 格式的 LZF 压缩字符串。返回值根据“标志”而变化。有关更多信息，请查看 rdbGenericLoadStringObject() 函数。*/
void *rdbLoadLzfStringObject(rio *rdb, int flags, size_t *lenptr) {
    int plain = flags & RDB_LOAD_PLAIN;
    int sds = flags & RDB_LOAD_SDS;
    uint64_t len, clen;
    unsigned char *c = NULL;
    char *val = NULL;

    if ((clen = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;
    if ((len = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;
    if ((c = ztrymalloc(clen)) == NULL) {
        serverLog(isRestoreContext()? LL_VERBOSE: LL_WARNING, "rdbLoadLzfStringObject failed allocating %llu bytes", (unsigned long long)clen);
        goto err;
    }

    /* Allocate our target according to the uncompressed size. */
    /**根据未压缩的大小分配我们的目标。*/
    if (plain) {
        val = ztrymalloc(len);
    } else {
        val = sdstrynewlen(SDS_NOINIT,len);
    }
    if (!val) {
        serverLog(isRestoreContext()? LL_VERBOSE: LL_WARNING, "rdbLoadLzfStringObject failed allocating %llu bytes", (unsigned long long)len);
        goto err;
    }

    if (lenptr) *lenptr = len;

    /* Load the compressed representation and uncompress it to target. */
    /**加载压缩表示并将其解压缩到目标。*/
    if (rioRead(rdb,c,clen) == 0) goto err;
    if (lzf_decompress(c,clen,val,len) != len) {
        rdbReportCorruptRDB("Invalid LZF compressed string");
        goto err;
    }
    zfree(c);

    if (plain || sds) {
        return val;
    } else {
        return createObject(OBJ_STRING,val);
    }
err:
    zfree(c);
    if (plain)
        zfree(val);
    else
        sdsfree(val);
    return NULL;
}

/* Save a string object as [len][data] on disk. If the object is a string
 * representation of an integer value we try to save it in a special form */
/**将字符串对象保存为磁盘上的 [len][data]。如果对象是整数值的字符串表示形式，我们会尝试以特殊形式保存它*/
ssize_t rdbSaveRawString(rio *rdb, unsigned char *s, size_t len) {
    int enclen;
    ssize_t n, nwritten = 0;

    /* Try integer encoding */
    if (len <= 11) {
        unsigned char buf[5];
        if ((enclen = rdbTryIntegerEncoding((char*)s,len,buf)) > 0) {
            if (rdbWriteRaw(rdb,buf,enclen) == -1) return -1;
            return enclen;
        }
    }

    /* Try LZF compression - under 20 bytes it's unable to compress even
     * aaaaaaaaaaaaaaaaaa so skip it */
    if (server.rdb_compression && len > 20) {
        n = rdbSaveLzfStringObject(rdb,s,len);
        if (n == -1) return -1;
        if (n > 0) return n;
        /* Return value of 0 means data can't be compressed, save the old way */
    }

    /* Store verbatim */
    if ((n = rdbSaveLen(rdb,len)) == -1) return -1;
    nwritten += n;
    if (len > 0) {
        if (rdbWriteRaw(rdb,s,len) == -1) return -1;
        nwritten += len;
    }
    return nwritten;
}

/* Save a long long value as either an encoded string or a string. */
/**将 long long 值保存为编码字符串或字符串。*/
ssize_t rdbSaveLongLongAsStringObject(rio *rdb, long long value) {
    unsigned char buf[32];
    ssize_t n, nwritten = 0;
    int enclen = rdbEncodeInteger(value,buf);
    if (enclen > 0) {
        return rdbWriteRaw(rdb,buf,enclen);
    } else {
        /* Encode as string */
        enclen = ll2string((char*)buf,32,value);
        serverAssert(enclen < 32);
        if ((n = rdbSaveLen(rdb,enclen)) == -1) return -1;
        nwritten += n;
        if ((n = rdbWriteRaw(rdb,buf,enclen)) == -1) return -1;
        nwritten += n;
    }
    return nwritten;
}

/* Like rdbSaveRawString() gets a Redis object instead. */
/**就像 rdbSaveRawString() 获取一个 Redis 对象一样。*/
ssize_t rdbSaveStringObject(rio *rdb, robj *obj) {
    /* Avoid to decode the object, then encode it again, if the
     * object is already integer encoded. */
    /**如果对象已经是整数编码，请避免解码对象，然后再次对其进行编码。*/
    if (obj->encoding == OBJ_ENCODING_INT) {
        return rdbSaveLongLongAsStringObject(rdb,(long)obj->ptr);
    } else {
        serverAssertWithInfo(NULL,obj,sdsEncodedObject(obj));
        return rdbSaveRawString(rdb,obj->ptr,sdslen(obj->ptr));
    }
}

/* Load a string object from an RDB file according to flags:
 *
 * RDB_LOAD_NONE (no flags): load an RDB object, unencoded.
 * RDB_LOAD_ENC: If the returned type is a Redis object, try to
 *               encode it in a special way to be more memory
 *               efficient. When this flag is passed the function
 *               no longer guarantees that obj->ptr is an SDS string.
 * RDB_LOAD_PLAIN: Return a plain string allocated with zmalloc()
 *                 instead of a Redis object with an sds in it.
 * RDB_LOAD_SDS: Return an SDS string instead of a Redis object.
 *
 * On I/O error NULL is returned.
 */
/**根据标志从 RDB 文件中加载字符串对象：
 *   RDB_LOAD_NONE（无标志）：加载未编码的 RDB 对象。
 *   RDB_LOAD_ENC：如果返回的类型是 Redis 对象，请尝试以特殊方式对其进行编码以提高内存效率。
 *     当这个标志被传递时，函数不再保证 obj->ptr 是一个 SDS 字符串。
 *   RDB_LOAD_PLAIN：返回使用 zmalloc() 分配的纯字符串，而不是其中包含 sds 的 Redis 对象。
 *   RDB_LOAD_SDS：返回 SDS 字符串而不是 Redis 对象。在 IO 错误时返回 NULL。*/
void *rdbGenericLoadStringObject(rio *rdb, int flags, size_t *lenptr) {
    int encode = flags & RDB_LOAD_ENC;
    int plain = flags & RDB_LOAD_PLAIN;
    int sds = flags & RDB_LOAD_SDS;
    int isencoded;
    unsigned long long len;

    len = rdbLoadLen(rdb,&isencoded);
    if (len == RDB_LENERR) return NULL;

    if (isencoded) {
        switch(len) {
        case RDB_ENC_INT8:
        case RDB_ENC_INT16:
        case RDB_ENC_INT32:
            return rdbLoadIntegerObject(rdb,len,flags,lenptr);
        case RDB_ENC_LZF:
            return rdbLoadLzfStringObject(rdb,flags,lenptr);
        default:
            rdbReportCorruptRDB("Unknown RDB string encoding type %llu",len);
            return NULL;
        }
    }

    if (plain || sds) {
        void *buf = plain ? ztrymalloc(len) : sdstrynewlen(SDS_NOINIT,len);
        if (!buf) {
            serverLog(isRestoreContext()? LL_VERBOSE: LL_WARNING, "rdbGenericLoadStringObject failed allocating %llu bytes", len);
            return NULL;
        }
        if (lenptr) *lenptr = len;
        if (len && rioRead(rdb,buf,len) == 0) {
            if (plain)
                zfree(buf);
            else
                sdsfree(buf);
            return NULL;
        }
        return buf;
    } else {
        robj *o = encode ? tryCreateStringObject(SDS_NOINIT,len) :
                           tryCreateRawStringObject(SDS_NOINIT,len);
        if (!o) {
            serverLog(isRestoreContext()? LL_VERBOSE: LL_WARNING, "rdbGenericLoadStringObject failed allocating %llu bytes", len);
            return NULL;
        }
        if (len && rioRead(rdb,o->ptr,len) == 0) {
            decrRefCount(o);
            return NULL;
        }
        return o;
    }
}

robj *rdbLoadStringObject(rio *rdb) {
    return rdbGenericLoadStringObject(rdb,RDB_LOAD_NONE,NULL);
}

robj *rdbLoadEncodedStringObject(rio *rdb) {
    return rdbGenericLoadStringObject(rdb,RDB_LOAD_ENC,NULL);
}

/* Save a double value. Doubles are saved as strings prefixed by an unsigned
 * 8 bit integer specifying the length of the representation.
 * This 8 bit integer has special values in order to specify the following
 * conditions:
 * 253: not a number
 * 254: + inf
 * 255: - inf
 */
int rdbSaveDoubleValue(rio *rdb, double val) {
    unsigned char buf[128];
    int len;

    if (isnan(val)) {
        buf[0] = 253;
        len = 1;
    } else if (!isfinite(val)) {
        len = 1;
        buf[0] = (val < 0) ? 255 : 254;
    } else {
        long long lvalue;
        /* Integer printing function is much faster, check if we can safely use it. */
        if (double2ll(val, &lvalue))
            ll2string((char*)buf+1,sizeof(buf)-1,lvalue);
        else
            snprintf((char*)buf+1,sizeof(buf)-1,"%.17g",val);
        buf[0] = strlen((char*)buf+1);
        len = buf[0]+1;
    }
    return rdbWriteRaw(rdb,buf,len);
}

/* For information about double serialization check rdbSaveDoubleValue() */
/**有关双重序列化检查 rdbSaveDoubleValue() 的信息*/
int rdbLoadDoubleValue(rio *rdb, double *val) {
    char buf[256];
    unsigned char len;

    if (rioRead(rdb,&len,1) == 0) return -1;
    switch(len) {
    case 255: *val = R_NegInf; return 0;
    case 254: *val = R_PosInf; return 0;
    case 253: *val = R_Nan; return 0;
    default:
        if (rioRead(rdb,buf,len) == 0) return -1;
        buf[len] = '\0';
        if (sscanf(buf, "%lg", val)!=1) return -1;
        return 0;
    }
}

/* Saves a double for RDB 8 or greater, where IE754 binary64 format is assumed.
 * We just make sure the integer is always stored in little endian, otherwise
 * the value is copied verbatim from memory to disk.
 * 为 RDB 8 或更高版本保存一个 double，其中假定 IE754 binary64 格式。
 * 我们只需确保整数始终以小端序存储，否则该值会逐字从内存复制到磁盘。
 *
 * Return -1 on error, the size of the serialized value on success.
 * 错误时返回 -1，成功时返回序列化值的大小。*/
int rdbSaveBinaryDoubleValue(rio *rdb, double val) {
    memrev64ifbe(&val);
    return rdbWriteRaw(rdb,&val,sizeof(val));
}

/* Loads a double from RDB 8 or greater. See rdbSaveBinaryDoubleValue() for
 * more info. On error -1 is returned, otherwise 0. */
/**从 RDB 8 或更高版本加载一个 double。有关详细信息，
 *
 * @return 请参阅 rdbSaveBinaryDoubleValue()。出错时返回 -1，否则返回 0。*/
int rdbLoadBinaryDoubleValue(rio *rdb, double *val) {
    if (rioRead(rdb,val,sizeof(*val)) == 0) return -1;
    memrev64ifbe(val);
    return 0;
}

/* Like rdbSaveBinaryDoubleValue() but single precision. */
/**像 rdbSaveBinaryDoubleValue() 但单精度。*/
int rdbSaveBinaryFloatValue(rio *rdb, float val) {
    memrev32ifbe(&val);
    return rdbWriteRaw(rdb,&val,sizeof(val));
}

/* Like rdbLoadBinaryDoubleValue() but single precision. */
/**像 rdbLoadBinaryDoubleValue() 但单精度。*/
int rdbLoadBinaryFloatValue(rio *rdb, float *val) {
    if (rioRead(rdb,val,sizeof(*val)) == 0) return -1;
    memrev32ifbe(val);
    return 0;
}

/* Save the object type of object "o". */
/**保存对象“o”的对象类型。*/
int rdbSaveObjectType(rio *rdb, robj *o) {
    switch (o->type) {
    case OBJ_STRING:
        return rdbSaveType(rdb,RDB_TYPE_STRING);
    case OBJ_LIST:
        if (o->encoding == OBJ_ENCODING_QUICKLIST)
            return rdbSaveType(rdb, RDB_TYPE_LIST_QUICKLIST_2);
        else
            serverPanic("Unknown list encoding");
    case OBJ_SET:
        if (o->encoding == OBJ_ENCODING_INTSET)
            return rdbSaveType(rdb,RDB_TYPE_SET_INTSET);
        else if (o->encoding == OBJ_ENCODING_HT)
            return rdbSaveType(rdb,RDB_TYPE_SET);
        else
            serverPanic("Unknown set encoding");
    case OBJ_ZSET:
        if (o->encoding == OBJ_ENCODING_LISTPACK)
            return rdbSaveType(rdb,RDB_TYPE_ZSET_LISTPACK);
        else if (o->encoding == OBJ_ENCODING_SKIPLIST)
            return rdbSaveType(rdb,RDB_TYPE_ZSET_2);
        else
            serverPanic("Unknown sorted set encoding");
    case OBJ_HASH:
        if (o->encoding == OBJ_ENCODING_LISTPACK)
            return rdbSaveType(rdb,RDB_TYPE_HASH_LISTPACK);
        else if (o->encoding == OBJ_ENCODING_HT)
            return rdbSaveType(rdb,RDB_TYPE_HASH);
        else
            serverPanic("Unknown hash encoding");
    case OBJ_STREAM:
        return rdbSaveType(rdb,RDB_TYPE_STREAM_LISTPACKS_2);
    case OBJ_MODULE:
        return rdbSaveType(rdb,RDB_TYPE_MODULE_2);
    default:
        serverPanic("Unknown object type");
    }
    return -1; /* avoid warning */
}

/* Use rdbLoadType() to load a TYPE in RDB format, but returns -1 if the
 * type is not specifically a valid Object Type. */
/**使用 rdbLoadType() 加载 RDB 格式的 TYPE，但如果该类型不是特定的有效对象类型，则返回 -1。*/
int rdbLoadObjectType(rio *rdb) {
    int type;
    if ((type = rdbLoadType(rdb)) == -1) return -1;
    if (!rdbIsObjectType(type)) return -1;
    return type;
}

/* This helper function serializes a consumer group Pending Entries List (PEL)
 * into the RDB file. The 'nacks' argument tells the function if also persist
 * the information about the not acknowledged message, or if to persist
 * just the IDs: this is useful because for the global consumer group PEL
 * we serialized the NACKs as well, but when serializing the local consumer
 * PELs we just add the ID, that will be resolved inside the global PEL to
 * put a reference to the same structure. */
/**此帮助函数将消费者组待处理条目列表 (PEL) 序列化到 RDB 文件中。
 * 'nacks' 参数告诉函数是否还保留有关未确认消息的信息，或者是否仅保留 ID：这很有用，
 * 因为对于全局消费者组 PEL，我们也序列化了 NACK，但是在序列化本地消费者时我们只需添加 ID 的 PEL，
 * 它将在全局 PEL 中解析以引用相同的结构。*/
ssize_t rdbSaveStreamPEL(rio *rdb, rax *pel, int nacks) {
    ssize_t n, nwritten = 0;

    /* Number of entries in the PEL. */
    if ((n = rdbSaveLen(rdb,raxSize(pel))) == -1) return -1;
    nwritten += n;

    /* Save each entry. */
    raxIterator ri;
    raxStart(&ri,pel);
    raxSeek(&ri,"^",NULL,0);
    while(raxNext(&ri)) {
        /* We store IDs in raw form as 128 big big endian numbers, like
         * they are inside the radix tree key. */
        if ((n = rdbWriteRaw(rdb,ri.key,sizeof(streamID))) == -1) {
            raxStop(&ri);
            return -1;
        }
        nwritten += n;

        if (nacks) {
            streamNACK *nack = ri.data;
            if ((n = rdbSaveMillisecondTime(rdb,nack->delivery_time)) == -1) {
                raxStop(&ri);
                return -1;
            }
            nwritten += n;
            if ((n = rdbSaveLen(rdb,nack->delivery_count)) == -1) {
                raxStop(&ri);
                return -1;
            }
            nwritten += n;
            /* We don't save the consumer name: we'll save the pending IDs
             * for each consumer in the consumer PEL, and resolve the consumer
             * at loading time. */
        }
    }
    raxStop(&ri);
    return nwritten;
}

/* Serialize the consumers of a stream consumer group into the RDB. Helper
 * function for the stream data type serialization. What we do here is to
 * persist the consumer metadata, and it's PEL, for each consumer. */
/**将流消费者组的消费者序列化到 RDB 中。流数据类型序列化的辅助函数。
 * 我们在这里所做的是为每个消费者持久保存消费者元数据，它是 PEL。*/
size_t rdbSaveStreamConsumers(rio *rdb, streamCG *cg) {
    ssize_t n, nwritten = 0;

    /* Number of consumers in this consumer group. */
    /**此消费组中的消费者数量。*/
    if ((n = rdbSaveLen(rdb,raxSize(cg->consumers))) == -1) return -1;
    nwritten += n;

    /* Save each consumer. */
    raxIterator ri;
    raxStart(&ri,cg->consumers);
    raxSeek(&ri,"^",NULL,0);
    while(raxNext(&ri)) {
        streamConsumer *consumer = ri.data;

        /* Consumer name. */
        if ((n = rdbSaveRawString(rdb,ri.key,ri.key_len)) == -1) {
            raxStop(&ri);
            return -1;
        }
        nwritten += n;

        /* Last seen time. 上次见时间。*/
        if ((n = rdbSaveMillisecondTime(rdb,consumer->seen_time)) == -1) {
            raxStop(&ri);
            return -1;
        }
        nwritten += n;

        /* Consumer PEL, without the ACKs (see last parameter of the function
         * passed with value of 0), at loading time we'll lookup the ID
         * in the consumer group global PEL and will put a reference in the
         * consumer local PEL. */
        /**消费者 PEL，没有 ACK（参见函数的最后一个参数传递的值为 0），在加载时，
         * 我们将在消费者组全局 PEL 中查找 ID，并将引用放在消费者本地 PEL 中。*/
        if ((n = rdbSaveStreamPEL(rdb,consumer->pel,0)) == -1) {
            raxStop(&ri);
            return -1;
        }
        nwritten += n;
    }
    raxStop(&ri);
    return nwritten;
}

/* Save a Redis object.
 * Returns -1 on error, number of bytes written on success. */
/**保存一个 Redis 对象。返回 -1 错误，成功写入的字节数。*/
ssize_t rdbSaveObject(rio *rdb, robj *o, robj *key, int dbid) {
    ssize_t n = 0, nwritten = 0;

    if (o->type == OBJ_STRING) {
        /* Save a string value */
        if ((n = rdbSaveStringObject(rdb,o)) == -1) return -1;
        nwritten += n;
    } else if (o->type == OBJ_LIST) {
        /* Save a list value */
        if (o->encoding == OBJ_ENCODING_QUICKLIST) {
            quicklist *ql = o->ptr;
            quicklistNode *node = ql->head;

            if ((n = rdbSaveLen(rdb,ql->len)) == -1) return -1;
            nwritten += n;

            while(node) {
                if ((n = rdbSaveLen(rdb,node->container)) == -1) return -1;
                nwritten += n;

                if (quicklistNodeIsCompressed(node)) {
                    void *data;
                    size_t compress_len = quicklistGetLzf(node, &data);
                    if ((n = rdbSaveLzfBlob(rdb,data,compress_len,node->sz)) == -1) return -1;
                    nwritten += n;
                } else {
                    if ((n = rdbSaveRawString(rdb,node->entry,node->sz)) == -1) return -1;
                    nwritten += n;
                }
                node = node->next;
            }
        } else {
            serverPanic("Unknown list encoding");
        }
    } else if (o->type == OBJ_SET) {
        /* Save a set value */
        if (o->encoding == OBJ_ENCODING_HT) {
            dict *set = o->ptr;
            dictIterator *di = dictGetIterator(set);
            dictEntry *de;

            if ((n = rdbSaveLen(rdb,dictSize(set))) == -1) {
                dictReleaseIterator(di);
                return -1;
            }
            nwritten += n;

            while((de = dictNext(di)) != NULL) {
                sds ele = dictGetKey(de);
                if ((n = rdbSaveRawString(rdb,(unsigned char*)ele,sdslen(ele)))
                    == -1)
                {
                    dictReleaseIterator(di);
                    return -1;
                }
                nwritten += n;
            }
            dictReleaseIterator(di);
        } else if (o->encoding == OBJ_ENCODING_INTSET) {
            size_t l = intsetBlobLen((intset*)o->ptr);

            if ((n = rdbSaveRawString(rdb,o->ptr,l)) == -1) return -1;
            nwritten += n;
        } else {
            serverPanic("Unknown set encoding");
        }
    } else if (o->type == OBJ_ZSET) {
        /* Save a sorted set value */
        if (o->encoding == OBJ_ENCODING_LISTPACK) {
            size_t l = lpBytes((unsigned char*)o->ptr);

            if ((n = rdbSaveRawString(rdb,o->ptr,l)) == -1) return -1;
            nwritten += n;
        } else if (o->encoding == OBJ_ENCODING_SKIPLIST) {
            zset *zs = o->ptr;
            zskiplist *zsl = zs->zsl;

            if ((n = rdbSaveLen(rdb,zsl->length)) == -1) return -1;
            nwritten += n;

            /* We save the skiplist elements from the greatest to the smallest
             * (that's trivial since the elements are already ordered in the
             * skiplist): this improves the load process, since the next loaded
             * element will always be the smaller, so adding to the skiplist
             * will always immediately stop at the head, making the insertion
             * O(1) instead of O(log(N)). */
            /**我们将skiplist元素从最大到最小保存（这是微不足道的，因为元素已经在skiplist中排序了）：
             * 这改善了加载过程，因为下一个加载的元素总是较小的，所以添加到skiplist总是会立即停在头部，
             * 插入 O(1) 而不是 O(log(N))。*/
            zskiplistNode *zn = zsl->tail;
            while (zn != NULL) {
                if ((n = rdbSaveRawString(rdb,
                    (unsigned char*)zn->ele,sdslen(zn->ele))) == -1)
                {
                    return -1;
                }
                nwritten += n;
                if ((n = rdbSaveBinaryDoubleValue(rdb,zn->score)) == -1)
                    return -1;
                nwritten += n;
                zn = zn->backward;
            }
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else if (o->type == OBJ_HASH) {
        /* Save a hash value */
        if (o->encoding == OBJ_ENCODING_LISTPACK) {
            size_t l = lpBytes((unsigned char*)o->ptr);

            if ((n = rdbSaveRawString(rdb,o->ptr,l)) == -1) return -1;
            nwritten += n;
        } else if (o->encoding == OBJ_ENCODING_HT) {
            dictIterator *di = dictGetIterator(o->ptr);
            dictEntry *de;

            if ((n = rdbSaveLen(rdb,dictSize((dict*)o->ptr))) == -1) {
                dictReleaseIterator(di);
                return -1;
            }
            nwritten += n;

            while((de = dictNext(di)) != NULL) {
                sds field = dictGetKey(de);
                sds value = dictGetVal(de);

                if ((n = rdbSaveRawString(rdb,(unsigned char*)field,
                        sdslen(field))) == -1)
                {
                    dictReleaseIterator(di);
                    return -1;
                }
                nwritten += n;
                if ((n = rdbSaveRawString(rdb,(unsigned char*)value,
                        sdslen(value))) == -1)
                {
                    dictReleaseIterator(di);
                    return -1;
                }
                nwritten += n;
            }
            dictReleaseIterator(di);
        } else {
            serverPanic("Unknown hash encoding");
        }
    } else if (o->type == OBJ_STREAM) {
        /* Store how many listpacks we have inside the radix tree. */
        /**存储我们在基数树中拥有多少个列表包。*/
        stream *s = o->ptr;
        rax *rax = s->rax;
        if ((n = rdbSaveLen(rdb,raxSize(rax))) == -1) return -1;
        nwritten += n;

        /* Serialize all the listpacks inside the radix tree as they are,
         * when loading back, we'll use the first entry of each listpack
         * to insert it back into the radix tree. */
        /**按原样序列化基数树中的所有列表包，当加载回来时，我们将使用每个列表包的第一个条目将其插入回基数树中。*/
        raxIterator ri;
        raxStart(&ri,rax);
        raxSeek(&ri,"^",NULL,0);
        while (raxNext(&ri)) {
            unsigned char *lp = ri.data;
            size_t lp_bytes = lpBytes(lp);
            if ((n = rdbSaveRawString(rdb,ri.key,ri.key_len)) == -1) {
                raxStop(&ri);
                return -1;
            }
            nwritten += n;
            if ((n = rdbSaveRawString(rdb,lp,lp_bytes)) == -1) {
                raxStop(&ri);
                return -1;
            }
            nwritten += n;
        }
        raxStop(&ri);

        /* Save the number of elements inside the stream. We cannot obtain
         * this easily later, since our macro nodes should be checked for
         * number of items: not a great CPU / space tradeoff. */
        /**保存流中的元素数量。我们以后不能轻易获得这个，因为我们的宏节点应该检查项目的数量：这不是一个很好的 CPU 空间折衷。*/
        if ((n = rdbSaveLen(rdb,s->length)) == -1) return -1;
        nwritten += n;
        /* Save the last entry ID. */
        if ((n = rdbSaveLen(rdb,s->last_id.ms)) == -1) return -1;
        nwritten += n;
        if ((n = rdbSaveLen(rdb,s->last_id.seq)) == -1) return -1;
        nwritten += n;
        /* Save the first entry ID. */
        if ((n = rdbSaveLen(rdb,s->first_id.ms)) == -1) return -1;
        nwritten += n;
        if ((n = rdbSaveLen(rdb,s->first_id.seq)) == -1) return -1;
        nwritten += n;
        /* Save the maximal tombstone ID. */
        if ((n = rdbSaveLen(rdb,s->max_deleted_entry_id.ms)) == -1) return -1;
        nwritten += n;
        if ((n = rdbSaveLen(rdb,s->max_deleted_entry_id.seq)) == -1) return -1;
        nwritten += n;
        /* Save the offset. */
        if ((n = rdbSaveLen(rdb,s->entries_added)) == -1) return -1;
        nwritten += n;

        /* The consumer groups and their clients are part of the stream
         * type, so serialize every consumer group. */
        /**消费者组及其客户端是流类型的一部分，因此请序列化每个消费者组。*/

        /* Save the number of groups. */
        size_t num_cgroups = s->cgroups ? raxSize(s->cgroups) : 0;
        if ((n = rdbSaveLen(rdb,num_cgroups)) == -1) return -1;
        nwritten += n;

        if (num_cgroups) {
            /* Serialize each consumer group. */
            raxStart(&ri,s->cgroups);
            raxSeek(&ri,"^",NULL,0);
            while(raxNext(&ri)) {
                streamCG *cg = ri.data;

                /* Save the group name. */
                if ((n = rdbSaveRawString(rdb,ri.key,ri.key_len)) == -1) {
                    raxStop(&ri);
                    return -1;
                }
                nwritten += n;

                /* Last ID. */
                if ((n = rdbSaveLen(rdb,cg->last_id.ms)) == -1) {
                    raxStop(&ri);
                    return -1;
                }
                nwritten += n;
                if ((n = rdbSaveLen(rdb,cg->last_id.seq)) == -1) {
                    raxStop(&ri);
                    return -1;
                }
                nwritten += n;
                
                /* Save the group's logical reads counter. */
                if ((n = rdbSaveLen(rdb,cg->entries_read)) == -1) {
                    raxStop(&ri);
                    return -1;
                }
                nwritten += n;

                /* Save the global PEL. */
                if ((n = rdbSaveStreamPEL(rdb,cg->pel,1)) == -1) {
                    raxStop(&ri);
                    return -1;
                }
                nwritten += n;

                /* Save the consumers of this group. */
                if ((n = rdbSaveStreamConsumers(rdb,cg)) == -1) {
                    raxStop(&ri);
                    return -1;
                }
                nwritten += n;
            }
            raxStop(&ri);
        }
    } else if (o->type == OBJ_MODULE) {
        /* Save a module-specific value. */
        RedisModuleIO io;
        moduleValue *mv = o->ptr;
        moduleType *mt = mv->type;

        /* Write the "module" identifier as prefix, so that we'll be able
         * to call the right module during loading. */
        int retval = rdbSaveLen(rdb,mt->id);
        if (retval == -1) return -1;
        moduleInitIOContext(io,mt,rdb,key,dbid);
        io.bytes += retval;

        /* Then write the module-specific representation + EOF marker. */
        mt->rdb_save(&io,mv->value);
        retval = rdbSaveLen(rdb,RDB_MODULE_OPCODE_EOF);
        if (retval == -1)
            io.error = 1;
        else
            io.bytes += retval;

        if (io.ctx) {
            moduleFreeContext(io.ctx);
            zfree(io.ctx);
        }
        return io.error ? -1 : (ssize_t)io.bytes;
    } else {
        serverPanic("Unknown object type");
    }
    return nwritten;
}

/* Return the length the object will have on disk if saved with
 * the rdbSaveObject() function. Currently we use a trick to get
 * this length with very little changes to the code. In the future
 * we could switch to a faster solution. */
/**如果使用 rdbSaveObject() 函数保存，则返回对象在磁盘上的长度。
 * 目前，我们使用一种技巧来获得这个长度，而对代码的更改很少。将来我们可以切换到更快的解决方案。*/
size_t rdbSavedObjectLen(robj *o, robj *key, int dbid) {
    ssize_t len = rdbSaveObject(NULL,o,key,dbid);
    serverAssertWithInfo(NULL,o,len != -1);
    return len;
}

/* Save a key-value pair, with expire time, type, key, value.
 * On error -1 is returned.
 * On success if the key was actually saved 1 is returned. */
/**保存一个键值对，包含过期时间、类型、键、值。返回错误 -1。如果密钥实际保存成功，则返回 1。*/
int rdbSaveKeyValuePair(rio *rdb, robj *key, robj *val, long long expiretime, int dbid) {
    int savelru = server.maxmemory_policy & MAXMEMORY_FLAG_LRU;
    int savelfu = server.maxmemory_policy & MAXMEMORY_FLAG_LFU;

    /* Save the expire time */
    if (expiretime != -1) {
        if (rdbSaveType(rdb,RDB_OPCODE_EXPIRETIME_MS) == -1) return -1;
        if (rdbSaveMillisecondTime(rdb,expiretime) == -1) return -1;
    }

    /* Save the LRU info. */
    if (savelru) {
        uint64_t idletime = estimateObjectIdleTime(val);
        idletime /= 1000; /* Using seconds is enough and requires less space. 使用秒就足够了，并且需要更少的空间。*/
        if (rdbSaveType(rdb,RDB_OPCODE_IDLE) == -1) return -1;
        if (rdbSaveLen(rdb,idletime) == -1) return -1;
    }

    /* Save the LFU info. */
    if (savelfu) {
        uint8_t buf[1];
        buf[0] = LFUDecrAndReturn(val);
        /* We can encode this in exactly two bytes: the opcode and an 8
         * bit counter, since the frequency is logarithmic with a 0-255 range.
         * Note that we do not store the halving time because to reset it
         * a single time when loading does not affect the frequency much. */
        /**我们可以用两个字节对其进行编码：操作码和一个 8 位计数器，因为频率是 0-255 范围内的对数。
         * 请注意，我们不存储减半时间，因为在加载时将其重置一次不会对频率产生太大影响。*/
        if (rdbSaveType(rdb,RDB_OPCODE_FREQ) == -1) return -1;
        if (rdbWriteRaw(rdb,buf,1) == -1) return -1;
    }

    /* Save type, key, value */
    if (rdbSaveObjectType(rdb,val) == -1) return -1;
    if (rdbSaveStringObject(rdb,key) == -1) return -1;
    if (rdbSaveObject(rdb,val,key,dbid) == -1) return -1;

    /* Delay return if required (for testing) */
    if (server.rdb_key_save_delay)
        debugDelay(server.rdb_key_save_delay);

    return 1;
}

/* Save an AUX field. 保存 AUX 字段。*/
ssize_t rdbSaveAuxField(rio *rdb, void *key, size_t keylen, void *val, size_t vallen) {
    ssize_t ret, len = 0;
    if ((ret = rdbSaveType(rdb,RDB_OPCODE_AUX)) == -1) return -1;
    len += ret;
    if ((ret = rdbSaveRawString(rdb,key,keylen)) == -1) return -1;
    len += ret;
    if ((ret = rdbSaveRawString(rdb,val,vallen)) == -1) return -1;
    len += ret;
    return len;
}

/* Wrapper for rdbSaveAuxField() used when key/val length can be obtained
 * with strlen(). */
/**当可以使用 strlen() 获得 keyval 长度时，使用 rdbSaveAuxField() 的包装器。*/
ssize_t rdbSaveAuxFieldStrStr(rio *rdb, char *key, char *val) {
    return rdbSaveAuxField(rdb,key,strlen(key),val,strlen(val));
}

/* Wrapper for strlen(key) + integer type (up to long long range). */
/**strlen(key) + 整数类型的包装器（最多 long long range）。*/
ssize_t rdbSaveAuxFieldStrInt(rio *rdb, char *key, long long val) {
    char buf[LONG_STR_SIZE];
    int vlen = ll2string(buf,sizeof(buf),val);
    return rdbSaveAuxField(rdb,key,strlen(key),buf,vlen);
}

/* Save a few default AUX fields with information about the RDB generated. */
/**保存一些默认的 AUX 字段，其中包含有关生成的 RDB 的信息。*/
int rdbSaveInfoAuxFields(rio *rdb, int rdbflags, rdbSaveInfo *rsi) {
    int redis_bits = (sizeof(void*) == 8) ? 64 : 32;
    int aof_base = (rdbflags & RDBFLAGS_AOF_PREAMBLE) != 0;

    /* Add a few fields about the state when the RDB was created. */
    if (rdbSaveAuxFieldStrStr(rdb,"redis-ver",REDIS_VERSION) == -1) return -1;
    if (rdbSaveAuxFieldStrInt(rdb,"redis-bits",redis_bits) == -1) return -1;
    if (rdbSaveAuxFieldStrInt(rdb,"ctime",time(NULL)) == -1) return -1;
    if (rdbSaveAuxFieldStrInt(rdb,"used-mem",zmalloc_used_memory()) == -1) return -1;

    /* Handle saving options that generate aux fields. */
    if (rsi) {
        if (rdbSaveAuxFieldStrInt(rdb,"repl-stream-db",rsi->repl_stream_db)
            == -1) return -1;
        if (rdbSaveAuxFieldStrStr(rdb,"repl-id",server.replid)
            == -1) return -1;
        if (rdbSaveAuxFieldStrInt(rdb,"repl-offset",server.master_repl_offset)
            == -1) return -1;
    }
    if (rdbSaveAuxFieldStrInt(rdb, "aof-base", aof_base) == -1) return -1;
    return 1;
}

ssize_t rdbSaveSingleModuleAux(rio *rdb, int when, moduleType *mt) {
    /* Save a module-specific aux value. */
    RedisModuleIO io;
    int retval = rdbSaveType(rdb, RDB_OPCODE_MODULE_AUX);
    if (retval == -1) return -1;
    moduleInitIOContext(io,mt,rdb,NULL,-1);
    io.bytes += retval;

    /* Write the "module" identifier as prefix, so that we'll be able
     * to call the right module during loading. */
    /**将“模块”标识符写为前缀，以便我们能够在加载过程中调用正确的模块。*/
    retval = rdbSaveLen(rdb,mt->id);
    if (retval == -1) return -1;
    io.bytes += retval;

    /* write the 'when' so that we can provide it on loading. add a UINT opcode
     * for backwards compatibility, everything after the MT needs to be prefixed
     * by an opcode. */
    /**写下“何时”，以便我们可以在加载时提供它。添加一个 UINT 操作码以实现向后兼容性，MT 之后的所有内容都需要以操作码为前缀。*/
    retval = rdbSaveLen(rdb,RDB_MODULE_OPCODE_UINT);
    if (retval == -1) return -1;
    io.bytes += retval;
    retval = rdbSaveLen(rdb,when);
    if (retval == -1) return -1;
    io.bytes += retval;

    /* Then write the module-specific representation + EOF marker. */
    mt->aux_save(&io,when);
    retval = rdbSaveLen(rdb,RDB_MODULE_OPCODE_EOF);
    if (retval == -1)
        io.error = 1;
    else
        io.bytes += retval;

    if (io.ctx) {
        moduleFreeContext(io.ctx);
        zfree(io.ctx);
    }
    if (io.error)
        return -1;
    return io.bytes;
}

ssize_t rdbSaveFunctions(rio *rdb) {
    dict *functions = functionsLibGet();
    dictIterator *iter = dictGetIterator(functions);
    dictEntry *entry = NULL;
    ssize_t written = 0;
    ssize_t ret;
    while ((entry = dictNext(iter))) {
        if ((ret = rdbSaveType(rdb, RDB_OPCODE_FUNCTION2)) < 0) goto werr;
        written += ret;
        functionLibInfo *li = dictGetVal(entry);
        if ((ret = rdbSaveRawString(rdb, (unsigned char *) li->code, sdslen(li->code))) < 0) goto werr;
        written += ret;
    }
    dictReleaseIterator(iter);
    return written;

werr:
    dictReleaseIterator(iter);
    return -1;
}

ssize_t rdbSaveDb(rio *rdb, int dbid, int rdbflags, long *key_counter) {
    dictIterator *di;
    dictEntry *de;
    ssize_t written = 0;
    ssize_t res;
    static long long info_updated_time = 0;
    char *pname = (rdbflags & RDBFLAGS_AOF_PREAMBLE) ? "AOF rewrite" :  "RDB";

    redisDb *db = server.db + dbid;
    dict *d = db->dict;
    if (dictSize(d) == 0) return 0;
    di = dictGetSafeIterator(d);

    /* Write the SELECT DB opcode */
    if ((res = rdbSaveType(rdb,RDB_OPCODE_SELECTDB)) < 0) goto werr;
    written += res;
    if ((res = rdbSaveLen(rdb, dbid)) < 0) goto werr;
    written += res;

    /* Write the RESIZE DB opcode. */
    uint64_t db_size, expires_size;
    db_size = dictSize(db->dict);
    expires_size = dictSize(db->expires);
    if ((res = rdbSaveType(rdb,RDB_OPCODE_RESIZEDB)) < 0) goto werr;
    written += res;
    if ((res = rdbSaveLen(rdb,db_size)) < 0) goto werr;
    written += res;
    if ((res = rdbSaveLen(rdb,expires_size)) < 0) goto werr;
    written += res;

    /* Iterate this DB writing every entry */
    while((de = dictNext(di)) != NULL) {
        sds keystr = dictGetKey(de);
        robj key, *o = dictGetVal(de);
        long long expire;
        size_t rdb_bytes_before_key = rdb->processed_bytes;

        initStaticStringObject(key,keystr);
        expire = getExpire(db,&key);
        if ((res = rdbSaveKeyValuePair(rdb, &key, o, expire, dbid)) < 0) goto werr;
        written += res;

        /* In fork child process, we can try to release memory back to the
         * OS and possibly avoid or decrease COW. We give the dismiss
         * mechanism a hint about an estimated size of the object we stored. */
        /**在 fork 子进程中，我们可以尝试将内存释放回操作系统，并可能避免或减少 COW。
         * 我们给dismiss机制一个关于我们存储的对象的估计大小的提示。*/
        size_t dump_size = rdb->processed_bytes - rdb_bytes_before_key;
        if (server.in_fork_child) dismissObject(o, dump_size);

        /* Update child info every 1 second (approximately).
         * in order to avoid calling mstime() on each iteration, we will
         * check the diff every 1024 keys */
        /**每 1 秒（大约）更新一次子信息。为了避免在每次迭代中调用 mstime()，我们将每 1024 个键检查一次 diff*/
        if (((*key_counter)++ & 1023) == 0) {
            long long now = mstime();
            if (now - info_updated_time >= 1000) {
                sendChildInfo(CHILD_INFO_TYPE_CURRENT_INFO, *key_counter, pname);
                info_updated_time = now;
            }
        }
    }

    dictReleaseIterator(di);
    return written;

werr:
    dictReleaseIterator(di);
    return -1;
}

/* Produces a dump of the database in RDB format sending it to the specified
 * Redis I/O channel. On success C_OK is returned, otherwise C_ERR
 * is returned and part of the output, or all the output, can be
 * missing because of I/O errors.
 *
 * When the function returns C_ERR and if 'error' is not NULL, the
 * integer pointed by 'error' is set to the value of errno just after the I/O
 * error. */
/**生成 RDB 格式的数据库转储，将其发送到指定的 Redis IO 通道。
 * 成功时返回 C_OK，否则返回 C_ERR，并且由于 IO 错误，可能会丢失部分输出或所有输出。
 * 当函数返回 C_ERR 并且如果 'error' 不为 NULL，则 'error' 指向的整数在 IO 错误之后被设置为 errno 的值。*/
int rdbSaveRio(int req, rio *rdb, int *error, int rdbflags, rdbSaveInfo *rsi) {
    char magic[10];
    uint64_t cksum;
    long key_counter = 0;
    int j;

    if (server.rdb_checksum)
        rdb->update_cksum = rioGenericUpdateChecksum;
    snprintf(magic,sizeof(magic),"REDIS%04d",RDB_VERSION);
    if (rdbWriteRaw(rdb,magic,9) == -1) goto werr;
    if (rdbSaveInfoAuxFields(rdb,rdbflags,rsi) == -1) goto werr;
    if (!(req & SLAVE_REQ_RDB_EXCLUDE_DATA) && rdbSaveModulesAux(rdb, REDISMODULE_AUX_BEFORE_RDB) == -1) goto werr;

    /* save functions */
    if (!(req & SLAVE_REQ_RDB_EXCLUDE_FUNCTIONS) && rdbSaveFunctions(rdb) == -1) goto werr;

    /* save all databases, skip this if we're in functions-only mode */
    if (!(req & SLAVE_REQ_RDB_EXCLUDE_DATA)) {
        for (j = 0; j < server.dbnum; j++) {
            if (rdbSaveDb(rdb, j, rdbflags, &key_counter) == -1) goto werr;
        }
    }

    if (!(req & SLAVE_REQ_RDB_EXCLUDE_DATA) && rdbSaveModulesAux(rdb, REDISMODULE_AUX_AFTER_RDB) == -1) goto werr;

    /* EOF opcode */
    if (rdbSaveType(rdb,RDB_OPCODE_EOF) == -1) goto werr;

    /* CRC64 checksum. It will be zero if checksum computation is disabled, the
     * loading code skips the check in this case. */
    cksum = rdb->cksum;
    memrev64ifbe(&cksum);
    if (rioWrite(rdb,&cksum,8) == 0) goto werr;
    return C_OK;

werr:
    if (error) *error = errno;
    return C_ERR;
}

/* This is just a wrapper to rdbSaveRio() that additionally adds a prefix
 * and a suffix to the generated RDB dump. The prefix is:
 *
 * $EOF:<40 bytes unguessable hex string>\r\n
 *
 * While the suffix is the 40 bytes hex string we announced in the prefix.
 * This way processes receiving the payload can understand when it ends
 * without doing any processing of the content. */
/**这只是 rdbSaveRio() 的一个包装器，它另外为生成的 RDB 转储添加了前缀和后缀。
 * 前缀是：EOF:<40 bytes unguessable hex string>\r\n 而后缀是我们在前缀中宣布的 40 bytes hex string。
 * 通过这种方式，接收有效负载的进程可以了解它何时结束，而无需对内容进行任何处理。*/
int rdbSaveRioWithEOFMark(int req, rio *rdb, int *error, rdbSaveInfo *rsi) {
    char eofmark[RDB_EOF_MARK_SIZE];

    startSaving(RDBFLAGS_REPLICATION);
    getRandomHexChars(eofmark,RDB_EOF_MARK_SIZE);
    if (error) *error = 0;
    if (rioWrite(rdb,"$EOF:",5) == 0) goto werr;
    if (rioWrite(rdb,eofmark,RDB_EOF_MARK_SIZE) == 0) goto werr;
    if (rioWrite(rdb,"\r\n",2) == 0) goto werr;
    if (rdbSaveRio(req,rdb,error,RDBFLAGS_NONE,rsi) == C_ERR) goto werr;
    if (rioWrite(rdb,eofmark,RDB_EOF_MARK_SIZE) == 0) goto werr;
    stopSaving(1);
    return C_OK;

werr: /* Write error. */
    /* Set 'error' only if not already set by rdbSaveRio() call. */
    if (error && *error == 0) *error = errno;
    stopSaving(0);
    return C_ERR;
}

/* Save the DB on disk. Return C_ERR on error, C_OK on success. */
/**将数据库保存在磁盘上。错误返回 C_ERR，成功返回 C_OK。*/
int rdbSave(int req, char *filename, rdbSaveInfo *rsi) {
    char tmpfile[256];
    char cwd[MAXPATHLEN]; /* Current working dir path for error messages. 错误消息的当前工作目录路径。*/
    FILE *fp = NULL;
    rio rdb;
    int error = 0;
    char *err_op;    /* For a detailed log */

    snprintf(tmpfile,256,"temp-%d.rdb", (int) getpid());
    fp = fopen(tmpfile,"w");
    if (!fp) {
        char *str_err = strerror(errno);
        char *cwdp = getcwd(cwd,MAXPATHLEN);
        serverLog(LL_WARNING,
            "Failed opening the temp RDB file %s (in server root dir %s) "
            "for saving: %s",
            tmpfile,
            cwdp ? cwdp : "unknown",
            str_err);
        return C_ERR;
    }

    rioInitWithFile(&rdb,fp);
    startSaving(RDBFLAGS_NONE);

    if (server.rdb_save_incremental_fsync)
        rioSetAutoSync(&rdb,REDIS_AUTOSYNC_BYTES);

    if (rdbSaveRio(req,&rdb,&error,RDBFLAGS_NONE,rsi) == C_ERR) {
        errno = error;
        err_op = "rdbSaveRio";
        goto werr;
    }

    /* Make sure data will not remain on the OS's output buffers */
    if (fflush(fp)) { err_op = "fflush"; goto werr; }
    if (fsync(fileno(fp))) { err_op = "fsync"; goto werr; }
    if (fclose(fp)) { fp = NULL; err_op = "fclose"; goto werr; }
    fp = NULL;
    
    /* Use RENAME to make sure the DB file is changed atomically only
     * if the generate DB file is ok. */
    if (rename(tmpfile,filename) == -1) {
        char *str_err = strerror(errno);
        char *cwdp = getcwd(cwd,MAXPATHLEN);
        serverLog(LL_WARNING,
            "Error moving temp DB file %s on the final "
            "destination %s (in server root dir %s): %s",
            tmpfile,
            filename,
            cwdp ? cwdp : "unknown",
            str_err);
        unlink(tmpfile);
        stopSaving(0);
        return C_ERR;
    }
    if (fsyncFileDir(filename) == -1) { err_op = "fsyncFileDir"; goto werr; }

    serverLog(LL_NOTICE,"DB saved on disk");
    server.dirty = 0;
    server.lastsave = time(NULL);
    server.lastbgsave_status = C_OK;
    stopSaving(1);
    return C_OK;

werr:
    serverLog(LL_WARNING,"Write error saving DB on disk(%s): %s", err_op, strerror(errno));
    if (fp) fclose(fp);
    unlink(tmpfile);
    stopSaving(0);
    return C_ERR;
}

int rdbSaveBackground(int req, char *filename, rdbSaveInfo *rsi) {
    pid_t childpid;

    if (hasActiveChildProcess()) return C_ERR;
    server.stat_rdb_saves++;

    server.dirty_before_bgsave = server.dirty;
    server.lastbgsave_try = time(NULL);

    if ((childpid = redisFork(CHILD_TYPE_RDB)) == 0) {
        int retval;

        /* Child */
        redisSetProcTitle("redis-rdb-bgsave");
        redisSetCpuAffinity(server.bgsave_cpulist);
        retval = rdbSave(req, filename,rsi);
        if (retval == C_OK) {
            sendChildCowInfo(CHILD_INFO_TYPE_RDB_COW_SIZE, "RDB");
        }
        exitFromChild((retval == C_OK) ? 0 : 1);
    } else {
        /* Parent */
        if (childpid == -1) {
            server.lastbgsave_status = C_ERR;
            serverLog(LL_WARNING,"Can't save in background: fork: %s",
                strerror(errno));
            return C_ERR;
        }
        serverLog(LL_NOTICE,"Background saving started by pid %ld",(long) childpid);
        server.rdb_save_time_start = time(NULL);
        server.rdb_child_type = RDB_CHILD_TYPE_DISK;
        return C_OK;
    }
    return C_OK; /* unreached */
}

/* Note that we may call this function in signal handle 'sigShutdownHandler',
 * so we need guarantee all functions we call are async-signal-safe.
 * If we call this function from signal handle, we won't call bg_unlink that
 * is not async-signal-safe. */
/**请注意，我们可能会在信号句柄“sigShutdownHandler”中调用此函数，
 * 因此我们需要保证我们调用的所有函数都是异步信号安全的。如果我们从信号句柄调用这个函数，
 * 我们将不会调用非异步信号安全的 bg_unlink。*/
void rdbRemoveTempFile(pid_t childpid, int from_signal) {
    char tmpfile[256];
    char pid[32];

    /* Generate temp rdb file name using async-signal safe functions. */
    int pid_len = ll2string(pid, sizeof(pid), childpid);
    strcpy(tmpfile, "temp-");
    strncpy(tmpfile+5, pid, pid_len);
    strcpy(tmpfile+5+pid_len, ".rdb");

    if (from_signal) {
        /* bg_unlink is not async-signal-safe, but in this case we don't really
         * need to close the fd, it'll be released when the process exists. */
        int fd = open(tmpfile, O_RDONLY|O_NONBLOCK);
        UNUSED(fd);
        unlink(tmpfile);
    } else {
        bg_unlink(tmpfile);
    }
}

/* This function is called by rdbLoadObject() when the code is in RDB-check
 * mode and we find a module value of type 2 that can be parsed without
 * the need of the actual module. The value is parsed for errors, finally
 * a dummy redis object is returned just to conform to the API. */
/**当代码处于 RDB 检查模式时，rdbLoadObject() 调用此函数，我们找到类型 2 的模块值，无需实际模块即可解析。
 * 解析该值是否有错误，最后返回一个虚拟 redis 对象以符合 API。*/
robj *rdbLoadCheckModuleValue(rio *rdb, char *modulename) {
    uint64_t opcode;
    while((opcode = rdbLoadLen(rdb,NULL)) != RDB_MODULE_OPCODE_EOF) {
        if (opcode == RDB_MODULE_OPCODE_SINT ||
            opcode == RDB_MODULE_OPCODE_UINT)
        {
            uint64_t len;
            if (rdbLoadLenByRef(rdb,NULL,&len) == -1) {
                rdbReportCorruptRDB(
                    "Error reading integer from module %s value", modulename);
            }
        } else if (opcode == RDB_MODULE_OPCODE_STRING) {
            robj *o = rdbGenericLoadStringObject(rdb,RDB_LOAD_NONE,NULL);
            if (o == NULL) {
                rdbReportCorruptRDB(
                    "Error reading string from module %s value", modulename);
            }
            decrRefCount(o);
        } else if (opcode == RDB_MODULE_OPCODE_FLOAT) {
            float val;
            if (rdbLoadBinaryFloatValue(rdb,&val) == -1) {
                rdbReportCorruptRDB(
                    "Error reading float from module %s value", modulename);
            }
        } else if (opcode == RDB_MODULE_OPCODE_DOUBLE) {
            double val;
            if (rdbLoadBinaryDoubleValue(rdb,&val) == -1) {
                rdbReportCorruptRDB(
                    "Error reading double from module %s value", modulename);
            }
        }
    }
    return createStringObject("module-dummy-value",18);
}

/* callback for hashZiplistConvertAndValidateIntegrity.
 * Check that the ziplist doesn't have duplicate hash field names.
 * The ziplist element pointed by 'p' will be converted and stored into listpack. */
/**hashZiplistConvertAndValidateIntegrity 的回调。检查 ziplist 是否没有重复的哈希字段名称。
 * 'p' 指向的 ziplist 元素将被转换并存储到 listpack 中。*/
static int _ziplistPairsEntryConvertAndValidate(unsigned char *p, unsigned int head_count, void *userdata) {
    unsigned char *str;
    unsigned int slen;
    long long vll;

    struct {
        long count;
        dict *fields;
        unsigned char **lp;
    } *data = userdata;

    if (data->fields == NULL) {
        data->fields = dictCreate(&hashDictType);
        dictExpand(data->fields, head_count/2);
    }

    if (!ziplistGet(p, &str, &slen, &vll))
        return 0;

    /* Even records are field names, add to dict and check that's not a dup */
    /**甚至记录也是字段名称，添加到 dict 并检查这不是重复*/
    if (((data->count) & 1) == 0) {
        sds field = str? sdsnewlen(str, slen): sdsfromlonglong(vll);
        if (dictAdd(data->fields, field, NULL) != DICT_OK) {
            /* Duplicate, return an error */
            sdsfree(field);
            return 0;
        }
    }

    if (str) {
        *(data->lp) = lpAppend(*(data->lp), (unsigned char*)str, slen);
    } else {
        *(data->lp) = lpAppendInteger(*(data->lp), vll);
    }

    (data->count)++;
    return 1;
}

/* Validate the integrity of the data structure while converting it to 
 * listpack and storing it at 'lp'.
 * The function is safe to call on non-validated ziplists, it returns 0
 * when encounter an integrity validation issue. */
/**验证数据结构的完整性，同时将其转换为 listpack 并将其存储在“lp”中。
 * 该函数可以安全地调用未验证的 ziplist，当遇到完整性验证问题时返回 0。*/
int ziplistPairsConvertAndValidateIntegrity(unsigned char *zl, size_t size, unsigned char **lp) {
    /* Keep track of the field names to locate duplicate ones */
    struct {
        long count;
        dict *fields; /* Initialisation at the first callback. */
        unsigned char **lp;
    } data = {0, NULL, lp};

    int ret = ziplistValidateIntegrity(zl, size, 1, _ziplistPairsEntryConvertAndValidate, &data);

    /* make sure we have an even number of records. */
    if (data.count & 1)
        ret = 0;

    if (data.fields) dictRelease(data.fields);
    return ret;
}

/* callback for ziplistValidateIntegrity.
 * The ziplist element pointed by 'p' will be converted and stored into listpack. */
/**ziplistValidateIntegrity 的回调。 'p' 指向的 ziplist 元素将被转换并存储到 listpack 中。*/
static int _ziplistEntryConvertAndValidate(unsigned char *p, unsigned int head_count, void *userdata) {
    UNUSED(head_count);
    unsigned char *str;
    unsigned int slen;
    long long vll;
    unsigned char **lp = (unsigned char**)userdata;

    if (!ziplistGet(p, &str, &slen, &vll)) return 0;

    if (str)
        *lp = lpAppend(*lp, (unsigned char*)str, slen);
    else
        *lp = lpAppendInteger(*lp, vll);

    return 1;
}

/* callback for ziplistValidateIntegrity.
 * The ziplist element pointed by 'p' will be converted and stored into quicklist. */
static int _listZiplistEntryConvertAndValidate(unsigned char *p, unsigned int head_count, void *userdata) {
    UNUSED(head_count);
    unsigned char *str;
    unsigned int slen;
    long long vll;
    char longstr[32] = {0};
    quicklist *ql = (quicklist*)userdata;

    if (!ziplistGet(p, &str, &slen, &vll)) return 0;
    if (!str) {
        /* Write the longval as a string so we can re-add it */
        /**将 longval 写为字符串，以便我们可以重新添加它*/
        slen = ll2string(longstr, sizeof(longstr), vll);
        str = (unsigned char *)longstr;
    }
    quicklistPushTail(ql, str, slen);
    return 1;
}

/* callback for to check the listpack doesn't have duplicate records */
/**检查列表包没有重复记录的回调*/
static int _lpPairsEntryValidation(unsigned char *p, unsigned int head_count, void *userdata) {
    struct {
        long count;
        dict *fields;
    } *data = userdata;

    if (data->fields == NULL) {
        data->fields = dictCreate(&hashDictType);
        dictExpand(data->fields, head_count/2);
    }

    /* Even records are field names, add to dict and check that's not a dup */
    if (((data->count) & 1) == 0) {
        unsigned char *str;
        int64_t slen;
        unsigned char buf[LP_INTBUF_SIZE];

        str = lpGet(p, &slen, buf);
        sds field = sdsnewlen(str, slen);
        if (dictAdd(data->fields, field, NULL) != DICT_OK) {
            /* Duplicate, return an error */
            sdsfree(field);
            return 0;
        }
    }

    (data->count)++;
    return 1;
}

/* Validate the integrity of the listpack structure.
 * when `deep` is 0, only the integrity of the header is validated.
 * when `deep` is 1, we scan all the entries one by one. */
/**验证 listpack 结构的完整性。当 `deep` 为 0 时，仅验证标头的完整性。当 `deep` 为 1 时，我们一一扫描所有条目。*/
int lpPairsValidateIntegrityAndDups(unsigned char *lp, size_t size, int deep) {
    if (!deep)
        return lpValidateIntegrity(lp, size, 0, NULL, NULL);

    /* Keep track of the field names to locate duplicate ones */
    struct {
        long count;
        dict *fields; /* Initialisation at the first callback. */
    } data = {0, NULL};

    int ret = lpValidateIntegrity(lp, size, 1, _lpPairsEntryValidation, &data);

    /* make sure we have an even number of records. */
    if (data.count & 1)
        ret = 0;

    if (data.fields) dictRelease(data.fields);
    return ret;
}

/* Load a Redis object of the specified type from the specified file.
 * On success a newly allocated object is returned, otherwise NULL.
 * When the function returns NULL and if 'error' is not NULL, the
 * integer pointed by 'error' is set to the type of error that occurred */
/**从指定文件加载指定类型的 Redis 对象。成功时返回新分配的对象，否则返回 NULL。
 * 当函数返回 NULL 并且 'error' 不是 NULL 时，'error' 指向的整数被设置为发生的错误类型*/
robj *rdbLoadObject(int rdbtype, rio *rdb, sds key, int dbid, int *error) {
    robj *o = NULL, *ele, *dec;
    uint64_t len;
    unsigned int i;

    /* Set default error of load object, it will be set to 0 on success. */
    if (error) *error = RDB_LOAD_ERR_OTHER;

    int deep_integrity_validation = server.sanitize_dump_payload == SANITIZE_DUMP_YES;
    if (server.sanitize_dump_payload == SANITIZE_DUMP_CLIENTS) {
        /* Skip sanitization when loading (an RDB), or getting a RESTORE command
         * from either the master or a client using an ACL user with the skip-sanitize-payload flag. */
        int skip = server.loading ||
            (server.current_client && (server.current_client->flags & CLIENT_MASTER));
        if (!skip && server.current_client && server.current_client->user)
            skip = !!(server.current_client->user->flags & USER_FLAG_SANITIZE_PAYLOAD_SKIP);
        deep_integrity_validation = !skip;
    }

    if (rdbtype == RDB_TYPE_STRING) {
        /* Read string value */
        if ((o = rdbLoadEncodedStringObject(rdb)) == NULL) return NULL;
        o = tryObjectEncoding(o);
    } else if (rdbtype == RDB_TYPE_LIST) {
        /* Read list value */
        if ((len = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;
        if (len == 0) goto emptykey;

        o = createQuicklistObject();
        quicklistSetOptions(o->ptr, server.list_max_listpack_size,
                            server.list_compress_depth);

        /* Load every single element of the list */
        while(len--) {
            if ((ele = rdbLoadEncodedStringObject(rdb)) == NULL) {
                decrRefCount(o);
                return NULL;
            }
            dec = getDecodedObject(ele);
            size_t len = sdslen(dec->ptr);
            quicklistPushTail(o->ptr, dec->ptr, len);
            decrRefCount(dec);
            decrRefCount(ele);
        }
    } else if (rdbtype == RDB_TYPE_SET) {
        /* Read Set value */
        if ((len = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;
        if (len == 0) goto emptykey;

        /* Use a regular set when there are too many entries. */
        size_t max_entries = server.set_max_intset_entries;
        if (max_entries >= 1<<30) max_entries = 1<<30;
        if (len > max_entries) {
            o = createSetObject();
            /* It's faster to expand the dict to the right size asap in order
             * to avoid rehashing */
            if (len > DICT_HT_INITIAL_SIZE && dictTryExpand(o->ptr,len) != DICT_OK) {
                rdbReportCorruptRDB("OOM in dictTryExpand %llu", (unsigned long long)len);
                decrRefCount(o);
                return NULL;
            }
        } else {
            o = createIntsetObject();
        }

        /* Load every single element of the set */
        for (i = 0; i < len; i++) {
            long long llval;
            sds sdsele;

            if ((sdsele = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL)) == NULL) {
                decrRefCount(o);
                return NULL;
            }

            if (o->encoding == OBJ_ENCODING_INTSET) {
                /* Fetch integer value from element. */
                if (isSdsRepresentableAsLongLong(sdsele,&llval) == C_OK) {
                    uint8_t success;
                    o->ptr = intsetAdd(o->ptr,llval,&success);
                    if (!success) {
                        rdbReportCorruptRDB("Duplicate set members detected");
                        decrRefCount(o);
                        sdsfree(sdsele);
                        return NULL;
                    }
                } else {
                    setTypeConvert(o,OBJ_ENCODING_HT);
                    if (dictTryExpand(o->ptr,len) != DICT_OK) {
                        rdbReportCorruptRDB("OOM in dictTryExpand %llu", (unsigned long long)len);
                        sdsfree(sdsele);
                        decrRefCount(o);
                        return NULL;
                    }
                }
            }

            /* This will also be called when the set was just converted
             * to a regular hash table encoded set. */
            if (o->encoding == OBJ_ENCODING_HT) {
                if (dictAdd((dict*)o->ptr,sdsele,NULL) != DICT_OK) {
                    rdbReportCorruptRDB("Duplicate set members detected");
                    decrRefCount(o);
                    sdsfree(sdsele);
                    return NULL;
                }
            } else {
                sdsfree(sdsele);
            }
        }
    } else if (rdbtype == RDB_TYPE_ZSET_2 || rdbtype == RDB_TYPE_ZSET) {
        /* Read sorted set value. */
        uint64_t zsetlen;
        size_t maxelelen = 0, totelelen = 0;
        zset *zs;

        if ((zsetlen = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;
        if (zsetlen == 0) goto emptykey;

        o = createZsetObject();
        zs = o->ptr;

        if (zsetlen > DICT_HT_INITIAL_SIZE && dictTryExpand(zs->dict,zsetlen) != DICT_OK) {
            rdbReportCorruptRDB("OOM in dictTryExpand %llu", (unsigned long long)zsetlen);
            decrRefCount(o);
            return NULL;
        }

        /* Load every single element of the sorted set. */
        while(zsetlen--) {
            sds sdsele;
            double score;
            zskiplistNode *znode;

            if ((sdsele = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL)) == NULL) {
                decrRefCount(o);
                return NULL;
            }

            if (rdbtype == RDB_TYPE_ZSET_2) {
                if (rdbLoadBinaryDoubleValue(rdb,&score) == -1) {
                    decrRefCount(o);
                    sdsfree(sdsele);
                    return NULL;
                }
            } else {
                if (rdbLoadDoubleValue(rdb,&score) == -1) {
                    decrRefCount(o);
                    sdsfree(sdsele);
                    return NULL;
                }
            }

            if (isnan(score)) {
                rdbReportCorruptRDB("Zset with NAN score detected");
                decrRefCount(o);
                sdsfree(sdsele);
                return NULL;
            }

            /* Don't care about integer-encoded strings. */
            if (sdslen(sdsele) > maxelelen) maxelelen = sdslen(sdsele);
            totelelen += sdslen(sdsele);

            znode = zslInsert(zs->zsl,score,sdsele);
            if (dictAdd(zs->dict,sdsele,&znode->score) != DICT_OK) {
                rdbReportCorruptRDB("Duplicate zset fields detected");
                decrRefCount(o);
                /* no need to free 'sdsele', will be released by zslFree together with 'o' */
                return NULL;
            }
        }

        /* Convert *after* loading, since sorted sets are not stored ordered. */
        if (zsetLength(o) <= server.zset_max_listpack_entries &&
            maxelelen <= server.zset_max_listpack_value &&
            lpSafeToAdd(NULL, totelelen))
        {
            zsetConvert(o,OBJ_ENCODING_LISTPACK);
        }
    } else if (rdbtype == RDB_TYPE_HASH) {
        uint64_t len;
        int ret;
        sds field, value;
        dict *dupSearchDict = NULL;

        len = rdbLoadLen(rdb, NULL);
        if (len == RDB_LENERR) return NULL;
        if (len == 0) goto emptykey;

        o = createHashObject();

        /* Too many entries? Use a hash table right from the start. */
        if (len > server.hash_max_listpack_entries)
            hashTypeConvert(o, OBJ_ENCODING_HT);
        else if (deep_integrity_validation) {
            /* In this mode, we need to guarantee that the server won't crash
             * later when the ziplist is converted to a dict.
             * Create a set (dict with no values) to for a dup search.
             * We can dismiss it as soon as we convert the ziplist to a hash. */
            /**在这种模式下，我们需要保证以后将ziplist转换为dict时服务器不会崩溃。
             * 为重复搜索创建一个集合（没有值的字典）。一旦我们将 ziplist 转换为哈希，我们就可以关闭它。*/
            dupSearchDict = dictCreate(&hashDictType);
        }


        /* Load every field and value into the ziplist */
        while (o->encoding == OBJ_ENCODING_LISTPACK && len > 0) {
            len--;
            /* Load raw strings */
            if ((field = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL)) == NULL) {
                decrRefCount(o);
                if (dupSearchDict) dictRelease(dupSearchDict);
                return NULL;
            }
            if ((value = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL)) == NULL) {
                sdsfree(field);
                decrRefCount(o);
                if (dupSearchDict) dictRelease(dupSearchDict);
                return NULL;
            }

            if (dupSearchDict) {
                sds field_dup = sdsdup(field);
                if (dictAdd(dupSearchDict, field_dup, NULL) != DICT_OK) {
                    rdbReportCorruptRDB("Hash with dup elements");
                    dictRelease(dupSearchDict);
                    decrRefCount(o);
                    sdsfree(field_dup);
                    sdsfree(field);
                    sdsfree(value);
                    return NULL;
                }
            }

            /* Convert to hash table if size threshold is exceeded */
            if (sdslen(field) > server.hash_max_listpack_value ||
                sdslen(value) > server.hash_max_listpack_value ||
                !lpSafeToAdd(o->ptr, sdslen(field)+sdslen(value)))
            {
                hashTypeConvert(o, OBJ_ENCODING_HT);
                ret = dictAdd((dict*)o->ptr, field, value);
                if (ret == DICT_ERR) {
                    rdbReportCorruptRDB("Duplicate hash fields detected");
                    if (dupSearchDict) dictRelease(dupSearchDict);
                    sdsfree(value);
                    sdsfree(field);
                    decrRefCount(o);
                    return NULL;
                }
                break;
            }

            /* Add pair to listpack */
            o->ptr = lpAppend(o->ptr, (unsigned char*)field, sdslen(field));
            o->ptr = lpAppend(o->ptr, (unsigned char*)value, sdslen(value));

            sdsfree(field);
            sdsfree(value);
        }

        if (dupSearchDict) {
            /* We no longer need this, from now on the entries are added
             * to a dict so the check is performed implicitly. */
            dictRelease(dupSearchDict);
            dupSearchDict = NULL;
        }

        if (o->encoding == OBJ_ENCODING_HT && len > DICT_HT_INITIAL_SIZE) {
            if (dictTryExpand(o->ptr,len) != DICT_OK) {
                rdbReportCorruptRDB("OOM in dictTryExpand %llu", (unsigned long long)len);
                decrRefCount(o);
                return NULL;
            }
        }

        /* Load remaining fields and values into the hash table */
        while (o->encoding == OBJ_ENCODING_HT && len > 0) {
            len--;
            /* Load encoded strings */
            if ((field = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL)) == NULL) {
                decrRefCount(o);
                return NULL;
            }
            if ((value = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL)) == NULL) {
                sdsfree(field);
                decrRefCount(o);
                return NULL;
            }

            /* Add pair to hash table */
            ret = dictAdd((dict*)o->ptr, field, value);
            if (ret == DICT_ERR) {
                rdbReportCorruptRDB("Duplicate hash fields detected");
                sdsfree(value);
                sdsfree(field);
                decrRefCount(o);
                return NULL;
            }
        }

        /* All pairs should be read by now */
        serverAssert(len == 0);
    } else if (rdbtype == RDB_TYPE_LIST_QUICKLIST || rdbtype == RDB_TYPE_LIST_QUICKLIST_2) {
        if ((len = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;
        if (len == 0) goto emptykey;

        o = createQuicklistObject();
        quicklistSetOptions(o->ptr, server.list_max_listpack_size,
                            server.list_compress_depth);
        uint64_t container = QUICKLIST_NODE_CONTAINER_PACKED;
        while (len--) {
            unsigned char *lp;
            size_t encoded_len;

            if (rdbtype == RDB_TYPE_LIST_QUICKLIST_2) {
                if ((container = rdbLoadLen(rdb,NULL)) == RDB_LENERR) {
                    decrRefCount(o);
                    return NULL;
                }

                if (container != QUICKLIST_NODE_CONTAINER_PACKED && container != QUICKLIST_NODE_CONTAINER_PLAIN) {
                    rdbReportCorruptRDB("Quicklist integrity check failed.");
                    decrRefCount(o);
                    return NULL;
                }
            }

            unsigned char *data =
                rdbGenericLoadStringObject(rdb,RDB_LOAD_PLAIN,&encoded_len);
            if (data == NULL || (encoded_len == 0)) {
                zfree(data);
                decrRefCount(o);
                return NULL;
            }

            if (container == QUICKLIST_NODE_CONTAINER_PLAIN) {
                quicklistAppendPlainNode(o->ptr, data, encoded_len);
                continue;
            }

            if (rdbtype == RDB_TYPE_LIST_QUICKLIST_2) {
                lp = data;
                if (deep_integrity_validation) server.stat_dump_payload_sanitizations++;
                if (!lpValidateIntegrity(lp, encoded_len, deep_integrity_validation, NULL, NULL)) {
                    rdbReportCorruptRDB("Listpack integrity check failed.");
                    decrRefCount(o);
                    zfree(lp);
                    return NULL;
                }
            } else {
                lp = lpNew(encoded_len);
                if (!ziplistValidateIntegrity(data, encoded_len, 1,
                        _ziplistEntryConvertAndValidate, &lp))
                {
                    rdbReportCorruptRDB("Ziplist integrity check failed.");
                    decrRefCount(o);
                    zfree(data);
                    zfree(lp);
                    return NULL;
                }
                zfree(data);
                lp = lpShrinkToFit(lp);
            }

            /* Silently skip empty ziplists, if we'll end up with empty quicklist we'll fail later. */
            if (lpLength(lp) == 0) {
                zfree(lp);
                continue;
            } else {
                quicklistAppendListpack(o->ptr, lp);
            }
        }

        if (quicklistCount(o->ptr) == 0) {
            decrRefCount(o);
            goto emptykey;
        }
    } else if (rdbtype == RDB_TYPE_HASH_ZIPMAP  ||
               rdbtype == RDB_TYPE_LIST_ZIPLIST ||
               rdbtype == RDB_TYPE_SET_INTSET   ||
               rdbtype == RDB_TYPE_ZSET_ZIPLIST ||
               rdbtype == RDB_TYPE_ZSET_LISTPACK ||
               rdbtype == RDB_TYPE_HASH_ZIPLIST ||
               rdbtype == RDB_TYPE_HASH_LISTPACK)
    {
        size_t encoded_len;
        unsigned char *encoded =
            rdbGenericLoadStringObject(rdb,RDB_LOAD_PLAIN,&encoded_len);
        if (encoded == NULL) return NULL;

        o = createObject(OBJ_STRING,encoded); /* Obj type fixed below. */

        /* Fix the object encoding, and make sure to convert the encoded
         * data type into the base type if accordingly to the current
         * configuration there are too many elements in the encoded data
         * type. Note that we only check the length and not max element
         * size as this is an O(N) scan. Eventually everything will get
         * converted. */
        /**修复对象编码，如果根据当前配置，编码数据类型中的元素过多，请确保将编码数据类型转换为基本类型。
         * 请注意，我们只检查长度而不是最大元素大小，因为这是一个 O(N) 扫描。最终一切都会被转化。*/
        switch(rdbtype) {
            case RDB_TYPE_HASH_ZIPMAP:
                /* Since we don't keep zipmaps anymore, the rdb loading for these
                 * is O(n) anyway, use `deep` validation. */
                if (!zipmapValidateIntegrity(encoded, encoded_len, 1)) {
                    rdbReportCorruptRDB("Zipmap integrity check failed.");
                    zfree(encoded);
                    o->ptr = NULL;
                    decrRefCount(o);
                    return NULL;
                }
                /* Convert to ziplist encoded hash. This must be deprecated
                 * when loading dumps created by Redis 2.4 gets deprecated. */
                {
                    unsigned char *lp = lpNew(0);
                    unsigned char *zi = zipmapRewind(o->ptr);
                    unsigned char *fstr, *vstr;
                    unsigned int flen, vlen;
                    unsigned int maxlen = 0;
                    dict *dupSearchDict = dictCreate(&hashDictType);

                    while ((zi = zipmapNext(zi, &fstr, &flen, &vstr, &vlen)) != NULL) {
                        if (flen > maxlen) maxlen = flen;
                        if (vlen > maxlen) maxlen = vlen;

                        /* search for duplicate records */
                        sds field = sdstrynewlen(fstr, flen);
                        if (!field || dictAdd(dupSearchDict, field, NULL) != DICT_OK ||
                            !lpSafeToAdd(lp, (size_t)flen + vlen)) {
                            rdbReportCorruptRDB("Hash zipmap with dup elements, or big length (%u)", flen);
                            dictRelease(dupSearchDict);
                            sdsfree(field);
                            zfree(encoded);
                            o->ptr = NULL;
                            decrRefCount(o);
                            return NULL;
                        }

                        lp = lpAppend(lp, fstr, flen);
                        lp = lpAppend(lp, vstr, vlen);
                    }

                    dictRelease(dupSearchDict);
                    zfree(o->ptr);
                    o->ptr = lp;
                    o->type = OBJ_HASH;
                    o->encoding = OBJ_ENCODING_LISTPACK;

                    if (hashTypeLength(o) > server.hash_max_listpack_entries ||
                        maxlen > server.hash_max_listpack_value)
                    {
                        hashTypeConvert(o, OBJ_ENCODING_HT);
                    }
                }
                break;
            case RDB_TYPE_LIST_ZIPLIST: 
                {
                    quicklist *ql = quicklistNew(server.list_max_listpack_size,
                                                 server.list_compress_depth);

                    if (!ziplistValidateIntegrity(encoded, encoded_len, 1,
                            _listZiplistEntryConvertAndValidate, ql))
                    {
                        rdbReportCorruptRDB("List ziplist integrity check failed.");
                        zfree(encoded);
                        o->ptr = NULL;
                        decrRefCount(o);
                        quicklistRelease(ql);
                        return NULL;
                    }

                    if (ql->len == 0) {
                        zfree(encoded);
                        o->ptr = NULL;
                        decrRefCount(o);
                        quicklistRelease(ql);
                        goto emptykey;
                    }

                    zfree(encoded);
                    o->type = OBJ_LIST;
                    o->ptr = ql;
                    o->encoding = OBJ_ENCODING_QUICKLIST;
                    break;
                }
            case RDB_TYPE_SET_INTSET:
                if (deep_integrity_validation) server.stat_dump_payload_sanitizations++;
                if (!intsetValidateIntegrity(encoded, encoded_len, deep_integrity_validation)) {
                    rdbReportCorruptRDB("Intset integrity check failed.");
                    zfree(encoded);
                    o->ptr = NULL;
                    decrRefCount(o);
                    return NULL;
                }
                o->type = OBJ_SET;
                o->encoding = OBJ_ENCODING_INTSET;
                if (intsetLen(o->ptr) > server.set_max_intset_entries)
                    setTypeConvert(o,OBJ_ENCODING_HT);
                break;
            case RDB_TYPE_ZSET_ZIPLIST:
                {
                    unsigned char *lp = lpNew(encoded_len);
                    if (!ziplistPairsConvertAndValidateIntegrity(encoded, encoded_len, &lp)) {
                        rdbReportCorruptRDB("Zset ziplist integrity check failed.");
                        zfree(lp);
                        zfree(encoded);
                        o->ptr = NULL;
                        decrRefCount(o);
                        return NULL;
                    }

                    zfree(o->ptr);
                    o->type = OBJ_ZSET;
                    o->ptr = lp;
                    o->encoding = OBJ_ENCODING_LISTPACK;
                    if (zsetLength(o) == 0) {
                        decrRefCount(o);
                        goto emptykey;
                    }

                    if (zsetLength(o) > server.zset_max_listpack_entries)
                        zsetConvert(o,OBJ_ENCODING_SKIPLIST);
                    else
                        o->ptr = lpShrinkToFit(o->ptr);
                    break;
                }
            case RDB_TYPE_ZSET_LISTPACK:
                if (deep_integrity_validation) server.stat_dump_payload_sanitizations++;
                if (!lpPairsValidateIntegrityAndDups(encoded, encoded_len, deep_integrity_validation)) {
                    rdbReportCorruptRDB("Zset listpack integrity check failed.");
                    zfree(encoded);
                    o->ptr = NULL;
                    decrRefCount(o);
                    return NULL;
                }
                o->type = OBJ_ZSET;
                o->encoding = OBJ_ENCODING_LISTPACK;
                if (zsetLength(o) == 0) {
                    decrRefCount(o);
                    goto emptykey;
                }

                if (zsetLength(o) > server.zset_max_listpack_entries)
                    zsetConvert(o,OBJ_ENCODING_SKIPLIST);
                break;
            case RDB_TYPE_HASH_ZIPLIST:
                {
                    unsigned char *lp = lpNew(encoded_len);
                    if (!ziplistPairsConvertAndValidateIntegrity(encoded, encoded_len, &lp)) {
                        rdbReportCorruptRDB("Hash ziplist integrity check failed.");
                        zfree(lp);
                        zfree(encoded);
                        o->ptr = NULL;
                        decrRefCount(o);
                        return NULL;
                    }

                    zfree(o->ptr);
                    o->ptr = lp;
                    o->type = OBJ_HASH;
                    o->encoding = OBJ_ENCODING_LISTPACK;
                    if (hashTypeLength(o) == 0) {
                        decrRefCount(o);
                        goto emptykey;
                    }

                    if (hashTypeLength(o) > server.hash_max_listpack_entries)
                        hashTypeConvert(o, OBJ_ENCODING_HT);
                    else
                        o->ptr = lpShrinkToFit(o->ptr);
                    break;
                }
            case RDB_TYPE_HASH_LISTPACK:
                if (deep_integrity_validation) server.stat_dump_payload_sanitizations++;
                if (!lpPairsValidateIntegrityAndDups(encoded, encoded_len, deep_integrity_validation)) {
                    rdbReportCorruptRDB("Hash listpack integrity check failed.");
                    zfree(encoded);
                    o->ptr = NULL;
                    decrRefCount(o);
                    return NULL;
                }
                o->type = OBJ_HASH;
                o->encoding = OBJ_ENCODING_LISTPACK;
                if (hashTypeLength(o) == 0) {
                    decrRefCount(o);
                    goto emptykey;
                }

                if (hashTypeLength(o) > server.hash_max_listpack_entries)
                    hashTypeConvert(o, OBJ_ENCODING_HT);
                break;
            default:
                /* totally unreachable */
                rdbReportCorruptRDB("Unknown RDB encoding type %d",rdbtype);
                break;
        }
    } else if (rdbtype == RDB_TYPE_STREAM_LISTPACKS || rdbtype == RDB_TYPE_STREAM_LISTPACKS_2) {
        o = createStreamObject();
        stream *s = o->ptr;
        uint64_t listpacks = rdbLoadLen(rdb,NULL);
        if (listpacks == RDB_LENERR) {
            rdbReportReadError("Stream listpacks len loading failed.");
            decrRefCount(o);
            return NULL;
        }

        while(listpacks--) {
            /* Get the master ID, the one we'll use as key of the radix tree
             * node: the entries inside the listpack itself are delta-encoded
             * relatively to this ID. */
            sds nodekey = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL);
            if (nodekey == NULL) {
                rdbReportReadError("Stream master ID loading failed: invalid encoding or I/O error.");
                decrRefCount(o);
                return NULL;
            }
            if (sdslen(nodekey) != sizeof(streamID)) {
                rdbReportCorruptRDB("Stream node key entry is not the "
                                        "size of a stream ID");
                sdsfree(nodekey);
                decrRefCount(o);
                return NULL;
            }

            /* Load the listpack. */
            size_t lp_size;
            unsigned char *lp =
                rdbGenericLoadStringObject(rdb,RDB_LOAD_PLAIN,&lp_size);
            if (lp == NULL) {
                rdbReportReadError("Stream listpacks loading failed.");
                sdsfree(nodekey);
                decrRefCount(o);
                return NULL;
            }
            if (deep_integrity_validation) server.stat_dump_payload_sanitizations++;
            if (!streamValidateListpackIntegrity(lp, lp_size, deep_integrity_validation)) {
                rdbReportCorruptRDB("Stream listpack integrity check failed.");
                sdsfree(nodekey);
                decrRefCount(o);
                zfree(lp);
                return NULL;
            }

            unsigned char *first = lpFirst(lp);
            if (first == NULL) {
                /* Serialized listpacks should never be empty, since on
                 * deletion we should remove the radix tree key if the
                 * resulting listpack is empty. */
                /**序列化的列表包永远不应为空，因为在删除时，如果结果列表包为空，我们应该删除基数树键。*/
                rdbReportCorruptRDB("Empty listpack inside stream");
                sdsfree(nodekey);
                decrRefCount(o);
                zfree(lp);
                return NULL;
            }

            /* Insert the key in the radix tree. */
            int retval = raxTryInsert(s->rax,
                (unsigned char*)nodekey,sizeof(streamID),lp,NULL);
            sdsfree(nodekey);
            if (!retval) {
                rdbReportCorruptRDB("Listpack re-added with existing key");
                decrRefCount(o);
                zfree(lp);
                return NULL;
            }
        }
        /* Load total number of items inside the stream. */
        s->length = rdbLoadLen(rdb,NULL);

        /* Load the last entry ID. */
        s->last_id.ms = rdbLoadLen(rdb,NULL);
        s->last_id.seq = rdbLoadLen(rdb,NULL);
        
        if (rdbtype == RDB_TYPE_STREAM_LISTPACKS_2) {
            /* Load the first entry ID. */
            s->first_id.ms = rdbLoadLen(rdb,NULL);
            s->first_id.seq = rdbLoadLen(rdb,NULL);

            /* Load the maximal deleted entry ID. */
            s->max_deleted_entry_id.ms = rdbLoadLen(rdb,NULL);
            s->max_deleted_entry_id.seq = rdbLoadLen(rdb,NULL);

            /* Load the offset. */
            s->entries_added = rdbLoadLen(rdb,NULL);
        } else {
            /* During migration the offset can be initialized to the stream's
             * length. At this point, we also don't care about tombstones
             * because CG offsets will be later initialized as well. */
            /**在迁移期间，偏移量可以初始化为流的长度。在这一点上，我们也不关心墓碑，因为 CG 偏移量稍后也会被初始化。*/
            s->max_deleted_entry_id.ms = 0;
            s->max_deleted_entry_id.seq = 0;
            s->entries_added = s->length;
            
            /* Since the rax is already loaded, we can find the first entry's
             * ID. */
            /**由于 rax 已经加载，我们可以找到第一个条目的 ID。*/
            streamGetEdgeID(s,1,1,&s->first_id);
        }

        if (rioGetReadError(rdb)) {
            rdbReportReadError("Stream object metadata loading failed.");
            decrRefCount(o);
            return NULL;
        }

        if (s->length && !raxSize(s->rax)) {
            rdbReportCorruptRDB("Stream length inconsistent with rax entries");
            decrRefCount(o);
            return NULL;
        }

        /* Consumer groups loading */
        uint64_t cgroups_count = rdbLoadLen(rdb,NULL);
        if (cgroups_count == RDB_LENERR) {
            rdbReportReadError("Stream cgroup count loading failed.");
            decrRefCount(o);
            return NULL;
        }
        while(cgroups_count--) {
            /* Get the consumer group name and ID. We can then create the
             * consumer group ASAP and populate its structure as
             * we read more data. */
            /**获取消费者组名称和 ID。然后，我们可以尽快创建消费者组并在读取更多数据时填充其结构。*/
            streamID cg_id;
            sds cgname = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL);
            if (cgname == NULL) {
                rdbReportReadError(
                    "Error reading the consumer group name from Stream");
                decrRefCount(o);
                return NULL;
            }

            cg_id.ms = rdbLoadLen(rdb,NULL);
            cg_id.seq = rdbLoadLen(rdb,NULL);
            if (rioGetReadError(rdb)) {
                rdbReportReadError("Stream cgroup ID loading failed.");
                sdsfree(cgname);
                decrRefCount(o);
                return NULL;
            }
            
            /* Load group offset. */
            uint64_t cg_offset;
            if (rdbtype == RDB_TYPE_STREAM_LISTPACKS_2) {
                cg_offset = rdbLoadLen(rdb,NULL);
                if (rioGetReadError(rdb)) {
                    rdbReportReadError("Stream cgroup offset loading failed.");
                    sdsfree(cgname);
                    decrRefCount(o);
                    return NULL;
                }
            } else {
                cg_offset = streamEstimateDistanceFromFirstEverEntry(s,&cg_id);
            }

            streamCG *cgroup = streamCreateCG(s,cgname,sdslen(cgname),&cg_id,cg_offset);
            if (cgroup == NULL) {
                rdbReportCorruptRDB("Duplicated consumer group name %s",
                                         cgname);
                decrRefCount(o);
                sdsfree(cgname);
                return NULL;
            }
            sdsfree(cgname);

            /* Load the global PEL for this consumer group, however we'll
             * not yet populate the NACK structures with the message
             * owner, since consumers for this group and their messages will
             * be read as a next step. So for now leave them not resolved
             * and later populate it. */
            /**为这个消费者组加载全局 PEL，但是我们还没有用消息所有者填充 NACK 结构，
             * 因为下一步将读取这个组的消费者和他们的消息。因此，暂时不要解决它们，然后再填充它。*/
            uint64_t pel_size = rdbLoadLen(rdb,NULL);
            if (pel_size == RDB_LENERR) {
                rdbReportReadError("Stream PEL size loading failed.");
                decrRefCount(o);
                return NULL;
            }
            while(pel_size--) {
                unsigned char rawid[sizeof(streamID)];
                if (rioRead(rdb,rawid,sizeof(rawid)) == 0) {
                    rdbReportReadError("Stream PEL ID loading failed.");
                    decrRefCount(o);
                    return NULL;
                }
                streamNACK *nack = streamCreateNACK(NULL);
                nack->delivery_time = rdbLoadMillisecondTime(rdb,RDB_VERSION);
                nack->delivery_count = rdbLoadLen(rdb,NULL);
                if (rioGetReadError(rdb)) {
                    rdbReportReadError("Stream PEL NACK loading failed.");
                    decrRefCount(o);
                    streamFreeNACK(nack);
                    return NULL;
                }
                if (!raxTryInsert(cgroup->pel,rawid,sizeof(rawid),nack,NULL)) {
                    rdbReportCorruptRDB("Duplicated global PEL entry "
                                            "loading stream consumer group");
                    decrRefCount(o);
                    streamFreeNACK(nack);
                    return NULL;
                }
            }

            /* Now that we loaded our global PEL, we need to load the
             * consumers and their local PELs. */
            /**现在我们加载了全局 PEL，我们需要加载消费者及其本地 PEL。*/
            uint64_t consumers_num = rdbLoadLen(rdb,NULL);
            if (consumers_num == RDB_LENERR) {
                rdbReportReadError("Stream consumers num loading failed.");
                decrRefCount(o);
                return NULL;
            }
            while(consumers_num--) {
                sds cname = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL);
                if (cname == NULL) {
                    rdbReportReadError(
                        "Error reading the consumer name from Stream group.");
                    decrRefCount(o);
                    return NULL;
                }
                streamConsumer *consumer = streamCreateConsumer(cgroup,cname,NULL,0,
                                                        SCC_NO_NOTIFY|SCC_NO_DIRTIFY);
                sdsfree(cname);
                if (!consumer) {
                    rdbReportCorruptRDB("Duplicate stream consumer detected.");
                    decrRefCount(o);
                    return NULL;
                }
                consumer->seen_time = rdbLoadMillisecondTime(rdb,RDB_VERSION);
                if (rioGetReadError(rdb)) {
                    rdbReportReadError("Stream short read reading seen time.");
                    decrRefCount(o);
                    return NULL;
                }

                /* Load the PEL about entries owned by this specific
                 * consumer. */
                pel_size = rdbLoadLen(rdb,NULL);
                if (pel_size == RDB_LENERR) {
                    rdbReportReadError(
                        "Stream consumer PEL num loading failed.");
                    decrRefCount(o);
                    return NULL;
                }
                while(pel_size--) {
                    unsigned char rawid[sizeof(streamID)];
                    if (rioRead(rdb,rawid,sizeof(rawid)) == 0) {
                        rdbReportReadError(
                            "Stream short read reading PEL streamID.");
                        decrRefCount(o);
                        return NULL;
                    }
                    streamNACK *nack = raxFind(cgroup->pel,rawid,sizeof(rawid));
                    if (nack == raxNotFound) {
                        rdbReportCorruptRDB("Consumer entry not found in "
                                                "group global PEL");
                        decrRefCount(o);
                        return NULL;
                    }

                    /* Set the NACK consumer, that was left to NULL when
                     * loading the global PEL. Then set the same shared
                     * NACK structure also in the consumer-specific PEL. */
                    nack->consumer = consumer;
                    if (!raxTryInsert(consumer->pel,rawid,sizeof(rawid),nack,NULL)) {
                        rdbReportCorruptRDB("Duplicated consumer PEL entry "
                                                " loading a stream consumer "
                                                "group");
                        decrRefCount(o);
                        streamFreeNACK(nack);
                        return NULL;
                    }
                }
            }

            /* Verify that each PEL eventually got a consumer assigned to it. */
            if (deep_integrity_validation) {
                raxIterator ri_cg_pel;
                raxStart(&ri_cg_pel,cgroup->pel);
                raxSeek(&ri_cg_pel,"^",NULL,0);
                while(raxNext(&ri_cg_pel)) {
                    streamNACK *nack = ri_cg_pel.data;
                    if (!nack->consumer) {
                        raxStop(&ri_cg_pel);
                        rdbReportCorruptRDB("Stream CG PEL entry without consumer");
                        decrRefCount(o);
                        return NULL;
                    }
                }
                raxStop(&ri_cg_pel);
            }
        }
    } else if (rdbtype == RDB_TYPE_MODULE || rdbtype == RDB_TYPE_MODULE_2) {
        uint64_t moduleid = rdbLoadLen(rdb,NULL);
        if (rioGetReadError(rdb)) {
            rdbReportReadError("Short read module id");
            return NULL;
        }
        moduleType *mt = moduleTypeLookupModuleByID(moduleid);

        if (rdbCheckMode && rdbtype == RDB_TYPE_MODULE_2) {
            char name[10];
            moduleTypeNameByID(name,moduleid);
            return rdbLoadCheckModuleValue(rdb,name);
        }

        if (mt == NULL) {
            char name[10];
            moduleTypeNameByID(name,moduleid);
            rdbReportCorruptRDB("The RDB file contains module data I can't load: no matching module type '%s'", name);
            return NULL;
        }
        RedisModuleIO io;
        robj keyobj;
        initStaticStringObject(keyobj,key);
        moduleInitIOContext(io,mt,rdb,&keyobj,dbid);
        io.ver = (rdbtype == RDB_TYPE_MODULE) ? 1 : 2;
        /* Call the rdb_load method of the module providing the 10 bit
         * encoding version in the lower 10 bits of the module ID. */
        void *ptr = mt->rdb_load(&io,moduleid&1023);
        if (io.ctx) {
            moduleFreeContext(io.ctx);
            zfree(io.ctx);
        }

        /* Module v2 serialization has an EOF mark at the end. */
        if (io.ver == 2) {
            uint64_t eof = rdbLoadLen(rdb,NULL);
            if (eof == RDB_LENERR) {
                if (ptr) {
                    o = createModuleObject(mt,ptr); /* creating just in order to easily destroy */
                    decrRefCount(o);
                }
                return NULL;
            }
            if (eof != RDB_MODULE_OPCODE_EOF) {
                rdbReportCorruptRDB("The RDB file contains module data for the module '%s' that is not terminated by "
                                    "the proper module value EOF marker", moduleTypeModuleName(mt));
                if (ptr) {
                    o = createModuleObject(mt,ptr); /* creating just in order to easily destroy */
                    decrRefCount(o);
                }
                return NULL;
            }
        }

        if (ptr == NULL) {
            rdbReportCorruptRDB("The RDB file contains module data for the module type '%s', that the responsible "
                                "module is not able to load. Check for modules log above for additional clues.",
                                moduleTypeModuleName(mt));
            return NULL;
        }
        o = createModuleObject(mt,ptr);
    } else {
        rdbReportReadError("Unknown RDB encoding type %d",rdbtype);
        return NULL;
    }
    if (error) *error = 0;
    return o;

emptykey:
    if (error) *error = RDB_LOAD_ERR_EMPTY_KEY;
    return NULL;
}

/* Mark that we are loading in the global state and setup the fields
 * needed to provide loading stats. */
/**标记我们正在以全局状态加载并设置提供加载统计信息所需的字段。*/
void startLoading(size_t size, int rdbflags, int async) {
    /* Load the DB */
    server.loading = 1;
    if (async == 1) server.async_loading = 1;
    server.loading_start_time = time(NULL);
    server.loading_loaded_bytes = 0;
    server.loading_total_bytes = size;
    server.loading_rdb_used_mem = 0;
    server.rdb_last_load_keys_expired = 0;
    server.rdb_last_load_keys_loaded = 0;
    blockingOperationStarts();

    /* Fire the loading modules start event. */
    int subevent;
    if (rdbflags & RDBFLAGS_AOF_PREAMBLE)
        subevent = REDISMODULE_SUBEVENT_LOADING_AOF_START;
    else if(rdbflags & RDBFLAGS_REPLICATION)
        subevent = REDISMODULE_SUBEVENT_LOADING_REPL_START;
    else
        subevent = REDISMODULE_SUBEVENT_LOADING_RDB_START;
    moduleFireServerEvent(REDISMODULE_EVENT_LOADING,subevent,NULL);
}

/* Mark that we are loading in the global state and setup the fields
 * needed to provide loading stats.
 * 'filename' is optional and used for rdb-check on error */
/**标记我们正在以全局状态加载并设置提供加载统计信息所需的字段。 'filename' 是可选的，用于 rdb-check 错误*/
void startLoadingFile(size_t size, char* filename, int rdbflags) {
    rdbFileBeingLoaded = filename;
    startLoading(size, rdbflags, 0);
}

/* Refresh the absolute loading progress info */
/**刷新绝对加载进度信息*/
void loadingAbsProgress(off_t pos) {
    server.loading_loaded_bytes = pos;
    if (server.stat_peak_memory < zmalloc_used_memory())
        server.stat_peak_memory = zmalloc_used_memory();
}

/* Refresh the incremental loading progress info */
/**刷新增量加载进度信息*/
void loadingIncrProgress(off_t size) {
    server.loading_loaded_bytes += size;
    if (server.stat_peak_memory < zmalloc_used_memory())
        server.stat_peak_memory = zmalloc_used_memory();
}

/* Update the file name currently being loaded */
/**更新当前正在加载的文件名*/
void updateLoadingFileName(char* filename) {
    rdbFileBeingLoaded = filename;
}

/* Loading finished */
void stopLoading(int success) {
    server.loading = 0;
    server.async_loading = 0;
    blockingOperationEnds();
    rdbFileBeingLoaded = NULL;

    /* Fire the loading modules end event. */
    moduleFireServerEvent(REDISMODULE_EVENT_LOADING,
                          success?
                            REDISMODULE_SUBEVENT_LOADING_ENDED:
                            REDISMODULE_SUBEVENT_LOADING_FAILED,
                           NULL);
}

void startSaving(int rdbflags) {
    /* Fire the persistence modules start event. */
    int subevent;
    if (rdbflags & RDBFLAGS_AOF_PREAMBLE && getpid() != server.pid)
        subevent = REDISMODULE_SUBEVENT_PERSISTENCE_AOF_START;
    else if (rdbflags & RDBFLAGS_AOF_PREAMBLE)
        subevent = REDISMODULE_SUBEVENT_PERSISTENCE_SYNC_AOF_START;
    else if (getpid()!=server.pid)
        subevent = REDISMODULE_SUBEVENT_PERSISTENCE_RDB_START;
    else
        subevent = REDISMODULE_SUBEVENT_PERSISTENCE_SYNC_RDB_START;
    moduleFireServerEvent(REDISMODULE_EVENT_PERSISTENCE,subevent,NULL);
}

void stopSaving(int success) {
    /* Fire the persistence modules end event. */
    moduleFireServerEvent(REDISMODULE_EVENT_PERSISTENCE,
                          success?
                            REDISMODULE_SUBEVENT_PERSISTENCE_ENDED:
                            REDISMODULE_SUBEVENT_PERSISTENCE_FAILED,
                          NULL);
}

/* Track loading progress in order to serve client's from time to time
   and if needed calculate rdb checksum  */
/**跟踪加载进度以便不时为客户服务，如果需要，计算 rdb 校验和*/
void rdbLoadProgressCallback(rio *r, const void *buf, size_t len) {
    if (server.rdb_checksum)
        rioGenericUpdateChecksum(r, buf, len);
    if (server.loading_process_events_interval_bytes &&
        (r->processed_bytes + len)/server.loading_process_events_interval_bytes > r->processed_bytes/server.loading_process_events_interval_bytes)
    {
        if (server.masterhost && server.repl_state == REPL_STATE_TRANSFER)
            replicationSendNewlineToMaster();
        loadingAbsProgress(r->processed_bytes);
        processEventsWhileBlocked();
        processModuleLoadingProgressEvent(0);
    }
    if (server.repl_state == REPL_STATE_TRANSFER && rioCheckType(r) == RIO_TYPE_CONN) {
        atomicIncr(server.stat_net_repl_input_bytes, len);
    }
}

/* Save the given functions_ctx to the rdb.
 * The err output parameter is optional and will be set with relevant error
 * message on failure, it is the caller responsibility to free the error
 * message on failure.
 *
 * The lib_ctx argument is also optional. If NULL is given, only verify rdb
 * structure with out performing the actual functions loading. */
/**将给定的 functions_ctx 保存到 rdb。 err 输出参数是可选的，将在失败时设置相关的错误消息，调用者有责任在失败时释放错误消息。
 * lib_ctx 参数也是可选的。如果给出 NULL，则只验证 rdb 结构而不执行实际的函数加载。*/
int rdbFunctionLoad(rio *rdb, int ver, functionsLibCtx* lib_ctx, int type, int rdbflags, sds *err) {
    UNUSED(ver);
    sds error = NULL;
    sds final_payload = NULL;
    int res = C_ERR;
    if (type == RDB_OPCODE_FUNCTION) {
        /* RDB that was generated on versions 7.0 rc1 and 7.0 rc2 has another
         * an old format that contains the library name, engine and description.
         * To support this format we must read those values. */
        /**在 7.0 rc1 和 7.0 rc2 版本上生成的 RDB 具有另一种旧格式，其中包含库名称、引擎和描述。
         * 为了支持这种格式，我们必须读取这些值。*/
        sds name = NULL;
        sds engine_name = NULL;
        sds desc = NULL;
        sds blob = NULL;
        uint64_t has_desc;

        if (!(name = rdbGenericLoadStringObject(rdb, RDB_LOAD_SDS, NULL))) {
            error = sdsnew("Failed loading library name");
            goto cleanup;
        }

        if (!(engine_name = rdbGenericLoadStringObject(rdb, RDB_LOAD_SDS, NULL))) {
            error = sdsnew("Failed loading engine name");
            goto cleanup;
        }

        if ((has_desc = rdbLoadLen(rdb, NULL)) == RDB_LENERR) {
            error = sdsnew("Failed loading library description indicator");
            goto cleanup;
        }

        if (has_desc && !(desc = rdbGenericLoadStringObject(rdb, RDB_LOAD_SDS, NULL))) {
            error = sdsnew("Failed loading library description");
            goto cleanup;
        }

        if (!(blob = rdbGenericLoadStringObject(rdb, RDB_LOAD_SDS, NULL))) {
            error = sdsnew("Failed loading library blob");
            goto cleanup;
        }
        /* Translate old format (versions 7.0 rc1 and 7.0 rc2) to new format.
         * The new format has the library name and engine inside the script payload.
         * Add those parameters to the original script payload (ignore the description if exists). */
        /**将旧格式（版本 7.0 rc1 和 7.0 rc2）转换为新格式。新格式在脚本有效负载中包含库名称和引擎。
         * 将这些参数添加到原始脚本有效负载（如果存在则忽略描述）。*/
        final_payload = sdscatfmt(sdsempty(), "#!%s name=%s\n%s", engine_name, name, blob);
cleanup:
        if (name) sdsfree(name);
        if (engine_name) sdsfree(engine_name);
        if (desc) sdsfree(desc);
        if (blob) sdsfree(blob);
        if (error) goto done;
    } else if (type == RDB_OPCODE_FUNCTION2) {
        if (!(final_payload = rdbGenericLoadStringObject(rdb, RDB_LOAD_SDS, NULL))) {
            error = sdsnew("Failed loading library payload");
            goto done;
        }
    } else {
        serverPanic("Bad function type was given to rdbFunctionLoad");
    }

    if (lib_ctx) {
        sds library_name = NULL;
        if (!(library_name = functionsCreateWithLibraryCtx(final_payload, rdbflags & RDBFLAGS_ALLOW_DUP, &error, lib_ctx))) {
            if (!error) {
                error = sdsnew("Failed creating the library");
            }
            goto done;
        }
        sdsfree(library_name);
    }

    res = C_OK;

done:
    if (final_payload) sdsfree(final_payload);
    if (error) {
        if (err) {
            *err = error;
        } else {
            serverLog(LL_WARNING, "Failed creating function, %s", error);
            sdsfree(error);
        }
    }
    return res;
}

/* Load an RDB file from the rio stream 'rdb'. On success C_OK is returned,
 * otherwise C_ERR is returned and 'errno' is set accordingly. */
/**从 rio 流 'rdb' 加载 RDB 文件。成功时返回 C_OK，否则返回 C_ERR 并相应地设置 'errno'。*/
int rdbLoadRio(rio *rdb, int rdbflags, rdbSaveInfo *rsi) {
    functionsLibCtx* functions_lib_ctx = functionsLibCtxGetCurrent();
    rdbLoadingCtx loading_ctx = { .dbarray = server.db, .functions_lib_ctx = functions_lib_ctx };
    int retval = rdbLoadRioWithLoadingCtx(rdb,rdbflags,rsi,&loading_ctx);
    return retval;
}


/* Load an RDB file from the rio stream 'rdb'. On success C_OK is returned,
 * otherwise C_ERR is returned and 'errno' is set accordingly. 
 * The rdb_loading_ctx argument holds objects to which the rdb will be loaded to,
 * currently it only allow to set db object and functionLibCtx to which the data
 * will be loaded (in the future it might contains more such objects). */
/**从 rio 流 'rdb' 加载 RDB 文件。成功时返回 C_OK，否则返回 C_ERR 并相应地设置 'errno'。
 * rdb_loading_ctx 参数保存 rdb 将被加载到的对象，目前它只允许设置数据将
 * 被加载到的 db 对象和 functionLibCtx（将来它可能包含更多这样的对象）。*/
int rdbLoadRioWithLoadingCtx(rio *rdb, int rdbflags, rdbSaveInfo *rsi, rdbLoadingCtx *rdb_loading_ctx) {
    uint64_t dbid = 0;
    int type, rdbver;
    redisDb *db = rdb_loading_ctx->dbarray+0;
    char buf[1024];
    int error;
    long long empty_keys_skipped = 0;

    rdb->update_cksum = rdbLoadProgressCallback;
    rdb->max_processing_chunk = server.loading_process_events_interval_bytes;
    if (rioRead(rdb,buf,9) == 0) goto eoferr;
    buf[9] = '\0';
    if (memcmp(buf,"REDIS",5) != 0) {
        serverLog(LL_WARNING,"Wrong signature trying to load DB from file");
        errno = EINVAL;
        return C_ERR;
    }
    rdbver = atoi(buf+5);
    if (rdbver < 1 || rdbver > RDB_VERSION) {
        serverLog(LL_WARNING,"Can't handle RDB format version %d",rdbver);
        errno = EINVAL;
        return C_ERR;
    }

    /* Key-specific attributes, set by opcodes before the key type. */
    long long lru_idle = -1, lfu_freq = -1, expiretime = -1, now = mstime();
    long long lru_clock = LRU_CLOCK();

    while(1) {
        sds key;
        robj *val;

        /* Read type. */
        if ((type = rdbLoadType(rdb)) == -1) goto eoferr;

        /* Handle special types. */
        if (type == RDB_OPCODE_EXPIRETIME) {
            /* EXPIRETIME: load an expire associated with the next key
             * to load. Note that after loading an expire we need to
             * load the actual type, and continue. */
            expiretime = rdbLoadTime(rdb);
            expiretime *= 1000;
            if (rioGetReadError(rdb)) goto eoferr;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_EXPIRETIME_MS) {
            /* EXPIRETIME_MS: milliseconds precision expire times introduced
             * with RDB v3. Like EXPIRETIME but no with more precision. */
            expiretime = rdbLoadMillisecondTime(rdb,rdbver);
            if (rioGetReadError(rdb)) goto eoferr;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_FREQ) {
            /* FREQ: LFU frequency. */
            uint8_t byte;
            if (rioRead(rdb,&byte,1) == 0) goto eoferr;
            lfu_freq = byte;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_IDLE) {
            /* IDLE: LRU idle time. */
            uint64_t qword;
            if ((qword = rdbLoadLen(rdb,NULL)) == RDB_LENERR) goto eoferr;
            lru_idle = qword;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_EOF) {
            /* EOF: End of file, exit the main loop. */
            break;
        } else if (type == RDB_OPCODE_SELECTDB) {
            /* SELECTDB: Select the specified database. */
            if ((dbid = rdbLoadLen(rdb,NULL)) == RDB_LENERR) goto eoferr;
            if (dbid >= (unsigned)server.dbnum) {
                serverLog(LL_WARNING,
                    "FATAL: Data file was created with a Redis "
                    "server configured to handle more than %d "
                    "databases. Exiting\n", server.dbnum);
                exit(1);
            }
            db = rdb_loading_ctx->dbarray+dbid;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_RESIZEDB) {
            /* RESIZEDB: Hint about the size of the keys in the currently
             * selected data base, in order to avoid useless rehashing. */
            uint64_t db_size, expires_size;
            if ((db_size = rdbLoadLen(rdb,NULL)) == RDB_LENERR)
                goto eoferr;
            if ((expires_size = rdbLoadLen(rdb,NULL)) == RDB_LENERR)
                goto eoferr;
            dictExpand(db->dict,db_size);
            dictExpand(db->expires,expires_size);
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_AUX) {
            /* AUX: generic string-string fields. Use to add state to RDB
             * which is backward compatible. Implementations of RDB loading
             * are required to skip AUX fields they don't understand.
             *
             * An AUX field is composed of two strings: key and value. */
            robj *auxkey, *auxval;
            if ((auxkey = rdbLoadStringObject(rdb)) == NULL) goto eoferr;
            if ((auxval = rdbLoadStringObject(rdb)) == NULL) {
                decrRefCount(auxkey);
                goto eoferr;
            }

            if (((char*)auxkey->ptr)[0] == '%') {
                /* All the fields with a name staring with '%' are considered
                 * information fields and are logged at startup with a log
                 * level of NOTICE. */
                serverLog(LL_NOTICE,"RDB '%s': %s",
                    (char*)auxkey->ptr,
                    (char*)auxval->ptr);
            } else if (!strcasecmp(auxkey->ptr,"repl-stream-db")) {
                if (rsi) rsi->repl_stream_db = atoi(auxval->ptr);
            } else if (!strcasecmp(auxkey->ptr,"repl-id")) {
                if (rsi && sdslen(auxval->ptr) == CONFIG_RUN_ID_SIZE) {
                    memcpy(rsi->repl_id,auxval->ptr,CONFIG_RUN_ID_SIZE+1);
                    rsi->repl_id_is_set = 1;
                }
            } else if (!strcasecmp(auxkey->ptr,"repl-offset")) {
                if (rsi) rsi->repl_offset = strtoll(auxval->ptr,NULL,10);
            } else if (!strcasecmp(auxkey->ptr,"lua")) {
                /* Won't load the script back in memory anymore. */
            } else if (!strcasecmp(auxkey->ptr,"redis-ver")) {
                serverLog(LL_NOTICE,"Loading RDB produced by version %s",
                    (char*)auxval->ptr);
            } else if (!strcasecmp(auxkey->ptr,"ctime")) {
                time_t age = time(NULL)-strtol(auxval->ptr,NULL,10);
                if (age < 0) age = 0;
                serverLog(LL_NOTICE,"RDB age %ld seconds",
                    (unsigned long) age);
            } else if (!strcasecmp(auxkey->ptr,"used-mem")) {
                long long usedmem = strtoll(auxval->ptr,NULL,10);
                serverLog(LL_NOTICE,"RDB memory usage when created %.2f Mb",
                    (double) usedmem / (1024*1024));
                server.loading_rdb_used_mem = usedmem;
            } else if (!strcasecmp(auxkey->ptr,"aof-preamble")) {
                long long haspreamble = strtoll(auxval->ptr,NULL,10);
                if (haspreamble) serverLog(LL_NOTICE,"RDB has an AOF tail");
            } else if (!strcasecmp(auxkey->ptr, "aof-base")) {
                long long isbase = strtoll(auxval->ptr, NULL, 10);
                if (isbase) serverLog(LL_NOTICE, "RDB is base AOF");
            } else if (!strcasecmp(auxkey->ptr,"redis-bits")) {
                /* Just ignored. */
            } else {
                /* We ignore fields we don't understand, as by AUX field
                 * contract. */
                serverLog(LL_DEBUG,"Unrecognized RDB AUX field: '%s'",
                    (char*)auxkey->ptr);
            }

            decrRefCount(auxkey);
            decrRefCount(auxval);
            continue; /* Read type again. */
        } else if (type == RDB_OPCODE_MODULE_AUX) {
            /* Load module data that is not related to the Redis key space.
             * Such data can be potentially be stored both before and after the
             * RDB keys-values section. */
            uint64_t moduleid = rdbLoadLen(rdb,NULL);
            int when_opcode = rdbLoadLen(rdb,NULL);
            int when = rdbLoadLen(rdb,NULL);
            if (rioGetReadError(rdb)) goto eoferr;
            if (when_opcode != RDB_MODULE_OPCODE_UINT) {
                rdbReportReadError("bad when_opcode");
                goto eoferr;
            }
            moduleType *mt = moduleTypeLookupModuleByID(moduleid);
            char name[10];
            moduleTypeNameByID(name,moduleid);

            if (!rdbCheckMode && mt == NULL) {
                /* Unknown module. */
                serverLog(LL_WARNING,"The RDB file contains AUX module data I can't load: no matching module '%s'", name);
                exit(1);
            } else if (!rdbCheckMode && mt != NULL) {
                if (!mt->aux_load) {
                    /* Module doesn't support AUX. */
                    serverLog(LL_WARNING,"The RDB file contains module AUX data, but the module '%s' doesn't seem to support it.", name);
                    exit(1);
                }

                RedisModuleIO io;
                moduleInitIOContext(io,mt,rdb,NULL,-1);
                io.ver = 2;
                /* Call the rdb_load method of the module providing the 10 bit
                 * encoding version in the lower 10 bits of the module ID. */
                int rc = mt->aux_load(&io,moduleid&1023, when);
                if (io.ctx) {
                    moduleFreeContext(io.ctx);
                    zfree(io.ctx);
                }
                if (rc != REDISMODULE_OK || io.error) {
                    moduleTypeNameByID(name,moduleid);
                    serverLog(LL_WARNING,"The RDB file contains module AUX data for the module type '%s', that the responsible module is not able to load. Check for modules log above for additional clues.", name);
                    goto eoferr;
                }
                uint64_t eof = rdbLoadLen(rdb,NULL);
                if (eof != RDB_MODULE_OPCODE_EOF) {
                    serverLog(LL_WARNING,"The RDB file contains module AUX data for the module '%s' that is not terminated by the proper module value EOF marker", name);
                    goto eoferr;
                }
                continue;
            } else {
                /* RDB check mode. */
                robj *aux = rdbLoadCheckModuleValue(rdb,name);
                decrRefCount(aux);
                continue; /* Read next opcode. */
            }
        } else if (type == RDB_OPCODE_FUNCTION || type == RDB_OPCODE_FUNCTION2) {
            sds err = NULL;
            if (rdbFunctionLoad(rdb, rdbver, rdb_loading_ctx->functions_lib_ctx, type, rdbflags, &err) != C_OK) {
                serverLog(LL_WARNING,"Failed loading library, %s", err);
                sdsfree(err);
                goto eoferr;
            }
            continue;
        }

        /* Read key */
        if ((key = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL)) == NULL)
            goto eoferr;
        /* Read value */
        val = rdbLoadObject(type,rdb,key,db->id,&error);

        /* Check if the key already expired. This function is used when loading
         * an RDB file from disk, either at startup, or when an RDB was
         * received from the master. In the latter case, the master is
         * responsible for key expiry. If we would expire keys here, the
         * snapshot taken by the master may not be reflected on the slave.
         * Similarly, if the base AOF is RDB format, we want to load all 
         * the keys they are, since the log of operations in the incr AOF 
         * is assumed to work in the exact keyspace state. */
        /**检查密钥是否已过期。当从磁盘加载 RDB 文件时使用此函数，无论是在启动时，还是从主服务器接收到 RDB 时。
         * 在后一种情况下，主人负责密钥到期。如果我们在这里使密钥过期，则主服务器拍摄的快照可能不会反映在从服务器上。
         * 类似地，如果基本 AOF 是 RDB 格式，我们想要加载它们的所有键，
         * 因为 incr AOF 中的操作日志被假定在精确的键空间状态下工作。*/
        if (val == NULL) {
            /* Since we used to have bug that could lead to empty keys
             * (See #8453), we rather not fail when empty key is encountered
             * in an RDB file, instead we will silently discard it and
             * continue loading. */
            /**由于我们曾经有可能导致空键的错误（参见 8453），我们宁愿在 RDB 文件中遇到空键时不失败，
             * 而是默默地丢弃它并继续加载。*/
            if (error == RDB_LOAD_ERR_EMPTY_KEY) {
                if(empty_keys_skipped++ < 10)
                    serverLog(LL_WARNING, "rdbLoadObject skipping empty key: %s", key);
                sdsfree(key);
            } else {
                sdsfree(key);
                goto eoferr;
            }
        } else if (iAmMaster() &&
            !(rdbflags&RDBFLAGS_AOF_PREAMBLE) &&
            expiretime != -1 && expiretime < now)
        {
            if (rdbflags & RDBFLAGS_FEED_REPL) {
                /* Caller should have created replication backlog,
                 * and now this path only works when rebooting,
                 * so we don't have replicas yet. */
                /**调用者应该已经创建了复制积压，现在这个路径只在重启时有效，所以我们还没有副本。*/
                serverAssert(server.repl_backlog != NULL && listLength(server.slaves) == 0);
                robj keyobj;
                initStaticStringObject(keyobj,key);
                robj *argv[2];
                argv[0] = server.lazyfree_lazy_expire ? shared.unlink : shared.del;
                argv[1] = &keyobj;
                replicationFeedSlaves(server.slaves,dbid,argv,2);
            }
            sdsfree(key);
            decrRefCount(val);
            server.rdb_last_load_keys_expired++;
        } else {
            robj keyobj;
            initStaticStringObject(keyobj,key);

            /* Add the new object in the hash table */
            /**在哈希表中添加新对象*/
            int added = dbAddRDBLoad(db,key,val);
            server.rdb_last_load_keys_loaded++;
            if (!added) {
                if (rdbflags & RDBFLAGS_ALLOW_DUP) {
                    /* This flag is useful for DEBUG RELOAD special modes.
                     * When it's set we allow new keys to replace the current
                     * keys with the same name. */
                    /**该标志对于 DEBUG RELOAD 特殊模式很有用。设置后，我们允许新键替换具有相同名称的当前键。*/
                    dbSyncDelete(db,&keyobj);
                    dbAddRDBLoad(db,key,val);
                } else {
                    serverLog(LL_WARNING,
                        "RDB has duplicated key '%s' in DB %d",key,db->id);
                    serverPanic("Duplicated key found in RDB file");
                }
            }

            /* Set the expire time if needed */
            /**根据需要设置过期时间*/
            if (expiretime != -1) {
                setExpire(NULL,db,&keyobj,expiretime);
            }

            /* Set usage information (for eviction). */
            /**设置使用信息（用于驱逐）。*/
            objectSetLRUOrLFU(val,lfu_freq,lru_idle,lru_clock,1000);

            /* call key space notification on key loaded for modules only */
            /**仅针对模块加载的键调用键空间通知*/
            moduleNotifyKeyspaceEvent(NOTIFY_LOADED, "loaded", &keyobj, db->id);
        }

        /* Loading the database more slowly is useful in order to test
         * certain edge cases. */
        /**为了测试某些边缘情况，更慢地加载数据库很有用。*/
        if (server.key_load_delay)
            debugDelay(server.key_load_delay);

        /* Reset the state that is key-specified and is populated by
         * opcodes before the key, so that we start from scratch again. */
        /**重置键指定并由键之前的操作码填充的状态，以便我们重新从头开始。*/
        expiretime = -1;
        lfu_freq = -1;
        lru_idle = -1;
    }
    /* Verify the checksum if RDB version is >= 5 */
    /**如果 RDB 版本 >= 5，则验证校验和*/
    if (rdbver >= 5) {
        uint64_t cksum, expected = rdb->cksum;

        if (rioRead(rdb,&cksum,8) == 0) goto eoferr;
        if (server.rdb_checksum && !server.skip_checksum_validation) {
            memrev64ifbe(&cksum);
            if (cksum == 0) {
                serverLog(LL_WARNING,"RDB file was saved with checksum disabled: no check performed.");
            } else if (cksum != expected) {
                serverLog(LL_WARNING,"Wrong RDB checksum expected: (%llx) but "
                    "got (%llx). Aborting now.",
                        (unsigned long long)expected,
                        (unsigned long long)cksum);
                rdbReportCorruptRDB("RDB CRC error");
                return C_ERR;
            }
        }
    }

    if (empty_keys_skipped) {
        serverLog(LL_WARNING,
            "Done loading RDB, keys loaded: %lld, keys expired: %lld, empty keys skipped: %lld.",
                server.rdb_last_load_keys_loaded, server.rdb_last_load_keys_expired, empty_keys_skipped);
    } else {
        serverLog(LL_NOTICE,
            "Done loading RDB, keys loaded: %lld, keys expired: %lld.",
                server.rdb_last_load_keys_loaded, server.rdb_last_load_keys_expired);
    }
    return C_OK;

    /* Unexpected end of file is handled here calling rdbReportReadError():
     * this will in turn either abort Redis in most cases, or if we are loading
     * the RDB file from a socket during initial SYNC (diskless replica mode),
     * we'll report the error to the caller, so that we can retry. */
    /**此处调用 rdbReportReadError() 处理意外的文件结尾：这将反过来在大多数情况下中止 Redis，
     * 或者如果我们在初始 SYNC（无盘副本模式）期间从套接字加载 RDB 文件，我们将报告错误给调用者，以便我们可以重试。*/
eoferr:
    serverLog(LL_WARNING,
        "Short read or OOM loading DB. Unrecoverable error, aborting now.");
    rdbReportReadError("Unexpected EOF reading RDB file");
    return C_ERR;
}

/* Like rdbLoadRio() but takes a filename instead of a rio stream. The
 * filename is open for reading and a rio stream object created in order
 * to do the actual loading. Moreover the ETA displayed in the INFO
 * output is initialized and finalized.
 *
 * If you pass an 'rsi' structure initialized with RDB_SAVE_INFO_INIT, the
 * loading code will fill the information fields in the structure. */
/**与 rdbLoadRio() 类似，但采用文件名而不是 rio 流。
 * 文件名已打开以供读取，并创建了一个 rio 流对象以进行实际加载。
 * 此外，INFO 输出中显示的 ETA 已初始化并最终确定。
 * 如果您传递使用 RDB_SAVE_INFO_INIT 初始化的“rsi”结构，加载代码将填充结构中的信息字段。*/
int rdbLoad(char *filename, rdbSaveInfo *rsi, int rdbflags) {
    FILE *fp;
    rio rdb;
    int retval;
    struct stat sb;

    if ((fp = fopen(filename,"r")) == NULL) return C_ERR;

    if (fstat(fileno(fp), &sb) == -1)
        sb.st_size = 0;

    startLoadingFile(sb.st_size, filename, rdbflags);
    rioInitWithFile(&rdb,fp);

    retval = rdbLoadRio(&rdb,rdbflags,rsi);

    fclose(fp);
    stopLoading(retval==C_OK);
    return retval;
}

/* A background saving child (BGSAVE) terminated its work. Handle this.
 * This function covers the case of actual BGSAVEs. */
/**后台保存子 (BGSAVE) 终止了它的工作。处理这个（事情。此功能涵盖实际 BGSAVE 的情况。*/
static void backgroundSaveDoneHandlerDisk(int exitcode, int bysignal) {
    if (!bysignal && exitcode == 0) {
        serverLog(LL_NOTICE,
            "Background saving terminated with success");
        server.dirty = server.dirty - server.dirty_before_bgsave;
        server.lastsave = time(NULL);
        server.lastbgsave_status = C_OK;
    } else if (!bysignal && exitcode != 0) {
        serverLog(LL_WARNING, "Background saving error");
        server.lastbgsave_status = C_ERR;
    } else {
        mstime_t latency;

        serverLog(LL_WARNING,
            "Background saving terminated by signal %d", bysignal);
        latencyStartMonitor(latency);
        rdbRemoveTempFile(server.child_pid, 0);
        latencyEndMonitor(latency);
        latencyAddSampleIfNeeded("rdb-unlink-temp-file",latency);
        /* SIGUSR1 is whitelisted, so we have a way to kill a child without
         * triggering an error condition. */
        /**SIGUSR1 被列入白名单，因此我们有办法在不触发错误条件的情况下杀死孩子。*/
        if (bysignal != SIGUSR1)
            server.lastbgsave_status = C_ERR;
    }
}

/* A background saving child (BGSAVE) terminated its work. Handle this.
 * This function covers the case of RDB -> Slaves socket transfers for
 * diskless replication. */
/**后台保存子 (BGSAVE) 终止了它的工作。处理这个（事情。此功能涵盖了 RDB -> Slaves 套接字传输以进行无盘复制的情况。*/
static void backgroundSaveDoneHandlerSocket(int exitcode, int bysignal) {
    if (!bysignal && exitcode == 0) {
        serverLog(LL_NOTICE,
            "Background RDB transfer terminated with success");
    } else if (!bysignal && exitcode != 0) {
        serverLog(LL_WARNING, "Background transfer error");
    } else {
        serverLog(LL_WARNING,
            "Background transfer terminated by signal %d", bysignal);
    }
    if (server.rdb_child_exit_pipe!=-1)
        close(server.rdb_child_exit_pipe);
    aeDeleteFileEvent(server.el, server.rdb_pipe_read, AE_READABLE);
    close(server.rdb_pipe_read);
    server.rdb_child_exit_pipe = -1;
    server.rdb_pipe_read = -1;
    zfree(server.rdb_pipe_conns);
    server.rdb_pipe_conns = NULL;
    server.rdb_pipe_numconns = 0;
    server.rdb_pipe_numconns_writing = 0;
    zfree(server.rdb_pipe_buff);
    server.rdb_pipe_buff = NULL;
    server.rdb_pipe_bufflen = 0;
}

/* When a background RDB saving/transfer terminates, call the right handler. */
/**当后台 RDB 保存传输终止时，调用正确的处理程序。*/
void backgroundSaveDoneHandler(int exitcode, int bysignal) {
    int type = server.rdb_child_type;
    switch(server.rdb_child_type) {
    case RDB_CHILD_TYPE_DISK:
        backgroundSaveDoneHandlerDisk(exitcode,bysignal);
        break;
    case RDB_CHILD_TYPE_SOCKET:
        backgroundSaveDoneHandlerSocket(exitcode,bysignal);
        break;
    default:
        serverPanic("Unknown RDB child type.");
        break;
    }

    server.rdb_child_type = RDB_CHILD_TYPE_NONE;
    server.rdb_save_time_last = time(NULL)-server.rdb_save_time_start;
    server.rdb_save_time_start = -1;
    /* Possibly there are slaves waiting for a BGSAVE in order to be served
     * (the first stage of SYNC is a bulk transfer of dump.rdb) */
    /**可能有从服务器等待 BGSAVE 以得到服务（SYNC 的第一阶段是 dump.rdb 的批量传输）*/
    updateSlavesWaitingBgsave((!bysignal && exitcode == 0) ? C_OK : C_ERR, type);
}

/* Kill the RDB saving child using SIGUSR1 (so that the parent will know
 * the child did not exit for an error, but because we wanted), and performs
 * the cleanup needed. */
/**使用 SIGUSR1 杀死 RDB 保存子节点（这样父节点就会知道子节点不是因为错误而退出，而是因为我们想要），并执行所需的清理。*/
void killRDBChild(void) {
    kill(server.child_pid, SIGUSR1);
    /* Because we are not using here waitpid (like we have in killAppendOnlyChild
     * and TerminateModuleForkChild), all the cleanup operations is done by
     * checkChildrenDone, that later will find that the process killed.
     * This includes:
     * - resetChildState
     * - rdbRemoveTempFile */
    /**因为我们这里没有使用waitpid（就像我们在killAppendOnlyChild和TerminateModuleForkChild中一样），
     * 所以所有的清理操作都是通过checkChildrenDone来完成的，后面会发现进程被杀死了。
     * 这包括：
     *   - resetChildState
     *   - rdbRemoveTempFile*/
}

/* Spawn an RDB child that writes the RDB to the sockets of the slaves
 * that are currently in SLAVE_STATE_WAIT_BGSAVE_START state. */
/**生成一个将 RDB 写入当前处于 SLAVE_STATE_WAIT_BGSAVE_START 状态的从属设备的套接字的 RDB 子级。*/
int rdbSaveToSlavesSockets(int req, rdbSaveInfo *rsi) {
    listNode *ln;
    listIter li;
    pid_t childpid;
    int pipefds[2], rdb_pipe_write, safe_to_exit_pipe;

    if (hasActiveChildProcess()) return C_ERR;

    /* Even if the previous fork child exited, don't start a new one until we
     * drained the pipe. */
    /**即使之前的 fork child 退出了，在我们排空管道之前不要启动一个新的。*/
    if (server.rdb_pipe_conns) return C_ERR;

    /* Before to fork, create a pipe that is used to transfer the rdb bytes to
     * the parent, we can't let it write directly to the sockets, since in case
     * of TLS we must let the parent handle a continuous TLS state when the
     * child terminates and parent takes over. */
    /**在 fork 之前，创建一个用于将 rdb 字节传输到父级的管道，我们不能让它直接写入套接字，
     * 因为在 TLS 的情况下，我们必须让父级在子级终止时处理连续的 TLS 状态和父母接管。*/
    if (anetPipe(pipefds, O_NONBLOCK, 0) == -1) return C_ERR;
    server.rdb_pipe_read = pipefds[0]; /* read end */
    rdb_pipe_write = pipefds[1]; /* write end */

    /* create another pipe that is used by the parent to signal to the child
     * that it can exit. */
    /**创建另一个管道，父级使用该管道向子级发出可以退出的信号。*/
    if (anetPipe(pipefds, 0, 0) == -1) {
        close(rdb_pipe_write);
        close(server.rdb_pipe_read);
        return C_ERR;
    }
    safe_to_exit_pipe = pipefds[0]; /* read end */
    server.rdb_child_exit_pipe = pipefds[1]; /* write end */

    /* Collect the connections of the replicas we want to transfer
     * the RDB to, which are i WAIT_BGSAVE_START state. */
    /**收集我们想要将 RDB 传输到的副本的连接，即 WAIT_BGSAVE_START 状态。*/
    server.rdb_pipe_conns = zmalloc(sizeof(connection *)*listLength(server.slaves));
    server.rdb_pipe_numconns = 0;
    server.rdb_pipe_numconns_writing = 0;
    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;
        if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) {
            /* Check slave has the exact requirements 检查从站有确切的要求*/
            if (slave->slave_req != req)
                continue;
            server.rdb_pipe_conns[server.rdb_pipe_numconns++] = slave->conn;
            replicationSetupSlaveForFullResync(slave,getPsyncInitialOffset());
        }
    }

    /* Create the child process. 创建子进程。*/
    if ((childpid = redisFork(CHILD_TYPE_RDB)) == 0) {
        /* Child */
        int retval, dummy;
        rio rdb;

        rioInitWithFd(&rdb,rdb_pipe_write);

        redisSetProcTitle("redis-rdb-to-slaves");
        redisSetCpuAffinity(server.bgsave_cpulist);

        retval = rdbSaveRioWithEOFMark(req,&rdb,NULL,rsi);
        if (retval == C_OK && rioFlush(&rdb) == 0)
            retval = C_ERR;

        if (retval == C_OK) {
            sendChildCowInfo(CHILD_INFO_TYPE_RDB_COW_SIZE, "RDB");
        }

        rioFreeFd(&rdb);
        /* wake up the reader, tell it we're done. */
        /**唤醒读者，告诉它我们完成了。*/
        close(rdb_pipe_write);
        close(server.rdb_child_exit_pipe); /* close write end so that we can detect the close on the parent. close write end
 *                                                以便我们可以检测到父节点的关闭。*/
        /* hold exit until the parent tells us it's safe. we're not expecting
         * to read anything, just get the error when the pipe is closed. */
        /**保持退出，直到父母告诉我们它是安全的。我们不希望读取任何内容，只是在管道关闭时得到错误。*/
        dummy = read(safe_to_exit_pipe, pipefds, 1);
        UNUSED(dummy);
        exitFromChild((retval == C_OK) ? 0 : 1);
    } else {
        /* Parent */
        if (childpid == -1) {
            serverLog(LL_WARNING,"Can't save in background: fork: %s",
                strerror(errno));

            /* Undo the state change. The caller will perform cleanup on
             * all the slaves in BGSAVE_START state, but an early call to
             * replicationSetupSlaveForFullResync() turned it into BGSAVE_END */
            /**撤消状态更改。调用者将对所有处于 BGSAVE_START 状态的从站执行清理，
             * 但对 replicationSetupSlaveForFullResync() 的早期调用将其变为 BGSAVE_END*/
            listRewind(server.slaves,&li);
            while((ln = listNext(&li))) {
                client *slave = ln->value;
                if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_END) {
                    slave->replstate = SLAVE_STATE_WAIT_BGSAVE_START;
                }
            }
            close(rdb_pipe_write);
            close(server.rdb_pipe_read);
            zfree(server.rdb_pipe_conns);
            server.rdb_pipe_conns = NULL;
            server.rdb_pipe_numconns = 0;
            server.rdb_pipe_numconns_writing = 0;
        } else {
            serverLog(LL_NOTICE,"Background RDB transfer started by pid %ld",
                (long) childpid);
            server.rdb_save_time_start = time(NULL);
            server.rdb_child_type = RDB_CHILD_TYPE_SOCKET;
            close(rdb_pipe_write); /* close write in parent so that it can detect the close on the child. */
                                      /**关闭在父项中写入，以便它可以检测到子项的关闭。*/
            if (aeCreateFileEvent(server.el, server.rdb_pipe_read, AE_READABLE, rdbPipeReadHandler,NULL) == AE_ERR) {
                serverPanic("Unrecoverable error creating server.rdb_pipe_read file event.");
            }
        }
        close(safe_to_exit_pipe);
        return (childpid == -1) ? C_ERR : C_OK;
    }
    return C_OK; /* Unreached. */
}

void saveCommand(client *c) {
    if (server.child_type == CHILD_TYPE_RDB) {
        addReplyError(c,"Background save already in progress");
        return;
    }

    server.stat_rdb_saves++;

    rdbSaveInfo rsi, *rsiptr;
    rsiptr = rdbPopulateSaveInfo(&rsi);
    if (rdbSave(SLAVE_REQ_NONE,server.rdb_filename,rsiptr) == C_OK) {
        addReply(c,shared.ok);
    } else {
        addReplyErrorObject(c,shared.err);
    }
}

/* BGSAVE [SCHEDULE] */
void bgsaveCommand(client *c) {
    int schedule = 0;

    /* The SCHEDULE option changes the behavior of BGSAVE when an AOF rewrite
     * is in progress. Instead of returning an error a BGSAVE gets scheduled. */
    /**当 AOF 重写正在进行时，SCHEDULE 选项会更改 BGSAVE 的行为。 BGSAVE 被安排而不是返回错误。*/
    if (c->argc > 1) {
        if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"schedule")) {
            schedule = 1;
        } else {
            addReplyErrorObject(c,shared.syntaxerr);
            return;
        }
    }

    rdbSaveInfo rsi, *rsiptr;
    rsiptr = rdbPopulateSaveInfo(&rsi);

    if (server.child_type == CHILD_TYPE_RDB) {
        addReplyError(c,"Background save already in progress");
    } else if (hasActiveChildProcess() || server.in_exec) {
        if (schedule || server.in_exec) {
            server.rdb_bgsave_scheduled = 1;
            addReplyStatus(c,"Background saving scheduled");
        } else {
            addReplyError(c,
            "Another child process is active (AOF?): can't BGSAVE right now. "
            "Use BGSAVE SCHEDULE in order to schedule a BGSAVE whenever "
            "possible.");
        }
    } else if (rdbSaveBackground(SLAVE_REQ_NONE,server.rdb_filename,rsiptr) == C_OK) {
        addReplyStatus(c,"Background saving started");
    } else {
        addReplyErrorObject(c,shared.err);
    }
}

/* Populate the rdbSaveInfo structure used to persist the replication
 * information inside the RDB file. Currently the structure explicitly
 * contains just the currently selected DB from the master stream, however
 * if the rdbSave*() family functions receive a NULL rsi structure also
 * the Replication ID/offset is not saved. The function populates 'rsi'
 * that is normally stack-allocated in the caller, returns the populated
 * pointer if the instance has a valid master client, otherwise NULL
 * is returned, and the RDB saving will not persist any replication related
 * information. */
/**填充用于将复制信息持久保存在 RDB 文件中的 rdbSaveInfo 结构。
 * 目前，该结构明确地仅包含当前从主流中选择的 DB，但是如果 rdbSave() 系列函数接收到 NULL rsi 结构，
 * 则复制 IDoffset 也不会保存。该函数填充通常在调用者中堆栈分配的“rsi”，
 * 如果实例具有有效的主客户端，则返回填充的指针，否则返回 NULL，并且 RDB 保存不会保留任何与复制相关的信息。*/
rdbSaveInfo *rdbPopulateSaveInfo(rdbSaveInfo *rsi) {
    rdbSaveInfo rsi_init = RDB_SAVE_INFO_INIT;
    *rsi = rsi_init;

    /* If the instance is a master, we can populate the replication info
     * only when repl_backlog is not NULL. If the repl_backlog is NULL,
     * it means that the instance isn't in any replication chains. In this
     * scenario the replication info is useless, because when a slave
     * connects to us, the NULL repl_backlog will trigger a full
     * synchronization, at the same time we will use a new replid and clear
     * replid2. */
    /**如果实例是主实例，我们只能在 repl_backlog 不为 NULL 时填充复制信息。
     * 如果 repl_backlog 为 NULL，则表示该实例不在任何复制链中。
     * 在这种情况下复制信息是没有用的，因为当slave连接到我们时，NULL repl_backlog会触发全同步，
     * 同时我们会使用一个新的replid并清除replid2。*/
    if (!server.masterhost && server.repl_backlog) {
        /* Note that when server.slaveseldb is -1, it means that this master
         * didn't apply any write commands after a full synchronization.
         * So we can let repl_stream_db be 0, this allows a restarted slave
         * to reload replication ID/offset, it's safe because the next write
         * command must generate a SELECT statement. */
        /**请注意，当 server.slaveseldb 为 -1 时，表示此 master 在完全同步后没有应用任何写入命令。
         * 所以我们可以让repl_stream_db为0，这允许重启的slave重新加载复制IDoffset，
         * 这是安全的，因为下一个写命令必须生成一个SELECT语句。*/
        rsi->repl_stream_db = server.slaveseldb == -1 ? 0 : server.slaveseldb;
        return rsi;
    }

    /* If the instance is a slave we need a connected master
     * in order to fetch the currently selected DB. */
    /**如果实例是从站，我们需要一个连接的主站来获取当前选择的数据库。*/
    if (server.master) {
        rsi->repl_stream_db = server.master->db->id;
        return rsi;
    }

    /* If we have a cached master we can use it in order to populate the
     * replication selected DB info inside the RDB file: the slave can
     * increment the master_repl_offset only from data arriving from the
     * master, so if we are disconnected the offset in the cached master
     * is valid. */
    /**如果我们有一个缓存的 master，我们可以使用它来填充 RDB 文件中的复制选择的 DB 信息：
     * slave 只能从来自 master 的数据中增加 master_repl_offset，
     * 所以如果我们断开连接，缓存的 master 中的偏移量是有效的。*/
    if (server.cached_master) {
        rsi->repl_stream_db = server.cached_master->db->id;
        return rsi;
    }
    return NULL;
}
