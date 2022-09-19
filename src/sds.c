/* SDSLib 2.0 -- A C dynamic strings library
 *
 * Copyright (c) 2006-2015, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2015, Oran Agra
 * Copyright (c) 2015, Redis Labs, Inc
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <limits.h>
#include "sds.h"
#include "sdsalloc.h"

const char *SDS_NOINIT = "SDS_NOINIT";

//根据类型获得字符串的结构体大小
static inline int sdsHdrSize(char type) {
    switch(type&SDS_TYPE_MASK) {
        case SDS_TYPE_5:
            return sizeof(struct sdshdr5);//1B
        case SDS_TYPE_8:
            return sizeof(struct sdshdr8);//3B
        case SDS_TYPE_16:
            return sizeof(struct sdshdr16);//5B
        case SDS_TYPE_32:
            return sizeof(struct sdshdr32);//9B
        case SDS_TYPE_64:
            return sizeof(struct sdshdr64);//17B
    }
    return 0;
}

//根据字符串大小决定使用什么类型存储
static inline char sdsReqType(size_t string_size) {
    if (string_size < 1<<5)
        return SDS_TYPE_5;
    if (string_size < 1<<8)
        return SDS_TYPE_8;
    if (string_size < 1<<16)
        return SDS_TYPE_16;
#if (LONG_MAX == LLONG_MAX)
    if (string_size < 1ll<<32)
        return SDS_TYPE_32;
    return SDS_TYPE_64;
#else
    return SDS_TYPE_32;
#endif
}

//获得当前类型最大存储长度
static inline size_t sdsTypeMaxSize(char type) {
    if (type == SDS_TYPE_5)
        return (1<<5) - 1;
    if (type == SDS_TYPE_8)
        return (1<<8) - 1;
    if (type == SDS_TYPE_16)
        return (1<<16) - 1;
#if (LONG_MAX == LLONG_MAX)
    if (type == SDS_TYPE_32)
        return (1ll<<32) - 1;
#endif
    return -1; /* this is equivalent to the max SDS_TYPE_64 or SDS_TYPE_32 */
}

/* Create a new sds string with the content specified by the 'init' pointer
 * and 'initlen'.
 * If NULL is used for 'init' the string is initialized with zero bytes.
 * If SDS_NOINIT is used, the buffer is left uninitialized;
 *
 * The string is always null-terminated (all the sds strings are, always) so
 * even if you create an sds string with:
 *
 * mystring = sdsnewlen("abc",3);
 *
 * You can print the string with printf() as there is an implicit \0 at the
 * end of the string. However the string is binary safe and can contain
 * \0 characters in the middle, as the length is stored in the sds header. */
/**
 * 使用“init”指针和“initlen”指定的内容创建一个新的 sds 字符串。
 * 如果 NULL 用于“init”，则字符串初始化为零字节。
 * 如果使用 SDS_NOINIT，则缓冲区未初始化；该字符串始终以空值结尾（所有 sds 字符串始终都是），
 * 因此即使您使用以下命令创建 sds 字符串：
 * mystring = sdsnewlen("abc",3);
 * 您可以使用 printf() 打印字符串，因为字符串末尾有一个隐含的 \0。
 * 但是该字符串是二进制安全的，并且可以在中间包含 \0 字符，因为长度存储在 sds 标头中。
 * */
sds _sdsnewlen(const void *init, size_t initlen, int trymalloc) {
    void *sh;
    sds s;
    char type = sdsReqType(initlen);
    /* Empty strings are usually created in order to append. Use type 8
     * since type 5 is not good at this. */
    //通常创建空字符串是为了追加。使用类型 8，因为类型 5 不擅长此操作。
    if (type == SDS_TYPE_5 && initlen == 0) type = SDS_TYPE_8;
    int hdrlen = sdsHdrSize(type);
    unsigned char *fp; /* flags pointer. */
    size_t usable;

    assert(initlen + hdrlen + 1 > initlen); /* Catch size_t overflow */
    sh = trymalloc?
         s_trymalloc_usable(hdrlen+initlen+1, &usable) :/**分配失败返回NULL,成功返回分配地址并将usable设为分配的大小*/
         s_malloc_usable(hdrlen+initlen+1, &usable);/**分配失败panic，成功返回分配地址并将usable设为分配的大小*/
    if (sh == NULL) return NULL;
    if (init==SDS_NOINIT)
        init = NULL;
    else if (!init)
        memset(sh, 0, hdrlen+initlen+1);//将分配的空间初始化为0
    s = (char*)sh+hdrlen;//字符串地址
    fp = ((unsigned char*)s)-1;//flags地址
    usable = usable-hdrlen-1;//可以分配的字符串空间=分配空间-结构体空间-\0
    if (usable > sdsTypeMaxSize(type))
        usable = sdsTypeMaxSize(type);//字符串所占空间不能大于该类型所最大能使用的空间
    switch(type) {
        case SDS_TYPE_5: {
            *fp = type | (initlen << SDS_TYPE_BITS);
            break;
        }
        case SDS_TYPE_8: {
            SDS_HDR_VAR(8,s);
            sh->len = initlen;
            sh->alloc = usable;
            *fp = type;
            break;
        }
        case SDS_TYPE_16: {
            SDS_HDR_VAR(16,s);
            sh->len = initlen;
            sh->alloc = usable;
            *fp = type;
            break;
        }
        case SDS_TYPE_32: {
            SDS_HDR_VAR(32,s);
            sh->len = initlen;
            sh->alloc = usable;
            *fp = type;
            break;
        }
        case SDS_TYPE_64: {
            SDS_HDR_VAR(64,s);
            sh->len = initlen;
            sh->alloc = usable;
            *fp = type;
            break;
        }
    }
    if (initlen && init)
        memcpy(s, init, initlen);//当存在初始化字符串时,将该字符串拷贝进字符串s
    s[initlen] = '\0';
    return s;
}

sds sdsnewlen(const void *init, size_t initlen) {
    return _sdsnewlen(init, initlen, 0);
}

sds sdstrynewlen(const void *init, size_t initlen) {
    return _sdsnewlen(init, initlen, 1);
}

/* Create an empty (zero length) sds string. Even in this case the string
 * always has an implicit null term. */
//创建一个空（零长度）sds 字符串。即使在这种情况下，字符串也总是有一个隐含的空项。
sds sdsempty(void) {
    return sdsnewlen("",0);
}

/* Create a new sds string starting from a null terminated C string. */
//从一个空终止的 C 字符串开始创建一个新的 sds 字符串。
sds sdsnew(const char *init) {
    size_t initlen = (init == NULL) ? 0 : strlen(init);
    return sdsnewlen(init, initlen);
}

/* Duplicate an sds string. */
//复制一个 sds 字符串。
sds sdsdup(const sds s) {
    return sdsnewlen(s, sdslen(s));
}

/* Free an sds string. No operation is performed if 's' is NULL. */
//释放一个 sds 字符串。如果 's' 为 NULL，则不执行任何操作。
void sdsfree(sds s) {
    if (s == NULL) return;
    s_free((char*)s-sdsHdrSize(s[-1]));
}

/* Set the sds string length to the length as obtained with strlen(), so
 * considering as content only up to the first null term character.
 *
 * This function is useful when the sds string is hacked manually in some
 * way, like in the following example:
 *
 * s = sdsnew("foobar");
 * s[2] = '\0';
 * sdsupdatelen(s);
 * printf("%d\n", sdslen(s));
 *
 * The output will be "2", but if we comment out the call to sdsupdatelen()
 * the output will be "6" as the string was modified but the logical length
 * remains 6 bytes. */
void sdsupdatelen(sds s) {
    size_t reallen = strlen(s);
    sdssetlen(s, reallen);
}

/* Modify an sds string in-place to make it empty (zero length).
 * However all the existing buffer is not discarded but set as free space
 * so that next append operations will not require allocations up to the
 * number of bytes previously available. */
/**
 * 就地修改 sds 字符串以使其为空（零长度）。
 * 但是，所有现有缓冲区都不会被丢弃，而是设置为空闲空间，以便下一个附加操作不需要分配多达先前可用的字节数。*/
void sdsclear(sds s) {
    sdssetlen(s, 0);
    s[0] = '\0';
}

/* Enlarge the free space at the end of the sds string so that the caller
 * is sure that after calling this function can overwrite up to addlen
 * bytes after the end of the string, plus one more byte for nul term.
 * If there's already sufficient free space, this function returns without any
 * action, if there isn't sufficient free space, it'll allocate what's missing,
 * and possibly more:
 * When greedy is 1, enlarge more than needed, to avoid need for future reallocs
 * on incremental growth.
 * When greedy is 0, enlarge just enough so that there's free space for 'addlen'.
 *
 * Note: this does not change the *length* of the sds string as returned
 * by sdslen(), but only the free buffer space we have. */
/** 扩大 sds 字符串末尾的可用空间，以便调用者确定调用此函数后可以覆盖字符串末尾后的 addlen 个字节，
 * 再加上一个字节用于 nul 项。如果已经有足够的可用空间，则此函数不执行任何操作就返回，
 * 如果没有足够的可用空间，它将分配缺少的空间，
 * 甚至可能更多：当贪婪为 1 时，扩大超出需要的范围，以避免将来需要重新分配关于增量增长。
 * 当 greedy 为 0 时，放大到刚好有足够的空间供 'addlen' 使用。
 * 注意：这不会改变 sdslen() 返回的 sds 字符串的长度，而只会改变我们拥有的可用缓冲区空间。*/
sds _sdsMakeRoomFor(sds s, size_t addlen, int greedy) {
    void *sh, *newsh;
    size_t avail = sdsavail(s);
    size_t len, newlen, reqlen;
    char type, oldtype = s[-1] & SDS_TYPE_MASK;
    int hdrlen;
    size_t usable;

    /* Return ASAP if there is enough space left. */
    if (avail >= addlen) return s;//如果有足够的空间，请尽快返回。

    len = sdslen(s);
    sh = (char*)s-sdsHdrSize(oldtype);
    reqlen = newlen = (len+addlen);//默认分配空间
    assert(newlen > len);   /* Catch size_t overflow */
    if (greedy == 1) { //贪心分配时
        if (newlen < SDS_MAX_PREALLOC)
            newlen *= 2;//两倍分配
        else
            newlen += SDS_MAX_PREALLOC;//1MB+newlen
    }

    type = sdsReqType(newlen);

    /* Don't use type 5: the user is appending to the string and type 5 is
     * not able to remember empty space, so sdsMakeRoomFor() must be called
     * at every appending operation. */
    //不要使用类型 5：用户正在追加到字符串,而类型 5 无法记住空格.因此必须在每次追加操作时调用 sdsMakeRoomFor()。
    if (type == SDS_TYPE_5) type = SDS_TYPE_8;

    hdrlen = sdsHdrSize(type);
    assert(hdrlen + newlen + 1 > reqlen);  //Catch size_t overflow 捕捉 size_t 溢出
    if (oldtype==type) {
        newsh = s_realloc_usable(sh, hdrlen+newlen+1, &usable);
        if (newsh == NULL) return NULL;
        s = (char*)newsh+hdrlen;
    } else {
        /* Since the header size changes, need to move the string forward,
         * and can't use realloc */
        //由于 header 大小发生变化，需要将字符串向前移动，并且不能使用 realloc
        newsh = s_malloc_usable(hdrlen+newlen+1, &usable);
        if (newsh == NULL) return NULL;
        memcpy((char*)newsh+hdrlen, s, len+1);
        s_free(sh);
        s = (char*)newsh+hdrlen;
        s[-1] = type;
        sdssetlen(s, len);
    }
    usable = usable-hdrlen-1;
    if (usable > sdsTypeMaxSize(type))
        usable = sdsTypeMaxSize(type);
    sdssetalloc(s, usable);
    return s;
}

/* Enlarge the free space at the end of the sds string more than needed,
 * This is useful to avoid repeated re-allocations when repeatedly appending to the sds. */
//将 sds 字符串末尾的可用空间扩大到超出需要的程度，这对于在重复附加到 sds 时避免重复重新分配很有用。
sds sdsMakeRoomFor(sds s, size_t addlen) {
    return _sdsMakeRoomFor(s, addlen, 1);
}

/* Unlike sdsMakeRoomFor(), this one just grows to the necessary size. */
//与 sdsMakeRoomFor() 不同，这个只是增长到必要的大小。
sds sdsMakeRoomForNonGreedy(sds s, size_t addlen) {
    return _sdsMakeRoomFor(s, addlen, 0);
}

/* Reallocate the sds string so that it has no free space at the end. The
 * contained string remains not altered, but next concatenation operations
 * will require a reallocation.
 *
 * After the call, the passed sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
/**
 * 重新分配 sds 字符串，使其末尾没有可用空间。
 * 包含的字符串保持不变，但接下来的连接操作将需要重新分配。
 * 调用后，传递的 sds 字符串不再有效，所有引用都必须替换为调用返回的新指针。*/
sds sdsRemoveFreeSpace(sds s) {
    void *sh, *newsh;
    char type, oldtype = s[-1] & SDS_TYPE_MASK;
    int hdrlen, oldhdrlen = sdsHdrSize(oldtype);
    size_t len = sdslen(s);
    size_t avail = sdsavail(s);
    sh = (char*)s-oldhdrlen;

    /* Return ASAP if there is no space left. */
    if (avail == 0) return s;//如果没有剩余空间，请尽快返回。

    /* Check what would be the minimum SDS header that is just good enough to
     * fit this string. */
    type = sdsReqType(len);//检查足以容纳此字符串的最小 SDS 标头。
    hdrlen = sdsHdrSize(type);

    /* If the type is the same, or at least a large enough type is still
     * required, we just realloc(), letting the allocator to do the copy
     * only if really needed. Otherwise if the change is huge, we manually
     * reallocate the string to use the different header type. */
    /**如果类型相同，或者至少仍然需要足够大的类型，我们只需 realloc()，让分配器仅在真正需要时进行复制。
     * 否则，如果变化很大，我们手动重新分配字符串以使用不同的标头类型。*/
    if (oldtype==type || type > SDS_TYPE_8) {//显然当新旧类型不同时没有将类型缩小
        newsh = s_realloc(sh, oldhdrlen+len+1);
        if (newsh == NULL) return NULL;
        s = (char*)newsh+oldhdrlen;
    } else { //显然当新旧类型不同时如果类型不大于type 8将可能类型缩小为type 5
        newsh = s_malloc(hdrlen+len+1);
        if (newsh == NULL) return NULL;
        memcpy((char*)newsh+hdrlen, s, len+1);
        s_free(sh);
        s = (char*)newsh+hdrlen;
        s[-1] = type;
        sdssetlen(s, len);
    }
    sdssetalloc(s, len);
    return s;
}

/* Resize the allocation, this can make the allocation bigger or smaller,
 * if the size is smaller than currently used len, the data will be truncated */
//调整分配大小，这可以使分配变大或变小，如果大小小于当前使用的len，数据将被截断
sds sdsResize(sds s, size_t size) {
    void *sh, *newsh;
    char type, oldtype = s[-1] & SDS_TYPE_MASK;
    int hdrlen, oldhdrlen = sdsHdrSize(oldtype);
    size_t len = sdslen(s);
    sh = (char*)s-oldhdrlen;

    /* Return ASAP if the size is already good. */
    if (sdsalloc(s) == size) return s;//如果尺寸已经很好，请尽快返回。

    /* Truncate len if needed. */
    if (size < len) len = size;//如果需要，截断 len。

    /* Check what would be the minimum SDS header that is just good enough to
     * fit this string. */
    type = sdsReqType(size);//检查足以容纳此字符串的最小 SDS 标头。
    /* Don't use type 5, it is not good for strings that are resized. */
    if (type == SDS_TYPE_5) type = SDS_TYPE_8;//不要使用类型 5，它不适合调整大小的字符串。
    hdrlen = sdsHdrSize(type);

    /* If the type is the same, or can hold the size in it with low overhead
     * (larger than SDS_TYPE_8), we just realloc(), letting the allocator
     * to do the copy only if really needed. Otherwise if the change is
     * huge, we manually reallocate the string to use the different header
     * type. */
    /**
     * 如果类型相同，或者可以以较低的开销（大于 SDS_TYPE_8）保持其中的大小，我们只需 realloc()，
     * 让分配器仅在真正需要时进行复制。否则，如果变化很大，我们手动重新分配字符串以使用不同的标头类型。
     * */
    if (oldtype==type || (type < oldtype && type > SDS_TYPE_8)) {
        newsh = s_realloc(sh, oldhdrlen+size+1);
        if (newsh == NULL) return NULL;
        s = (char*)newsh+oldhdrlen;
    } else {
        newsh = s_malloc(hdrlen+size+1);
        if (newsh == NULL) return NULL;
        memcpy((char*)newsh+hdrlen, s, len);
        s_free(sh);
        s = (char*)newsh+hdrlen;
        s[-1] = type;
    }
    s[len] = 0;
    sdssetlen(s, len);
    sdssetalloc(s, size);
    return s;
}

/* Return the total size of the allocation of the specified sds string,
 * including:
 * 1) The sds header before the pointer.
 * 2) The string.
 * 3) The free buffer at the end if any.
 * 4) The implicit null term.
 */
/**
 * 返回指定sds字符串分配的总大小，包括：
 * 1) 指针前的sds header。
 * 2) 字符串。
 * 3) 最后的空闲缓冲区（如果有）。
 * 4) 隐含的空项。
 * */
size_t sdsAllocSize(sds s) {
    size_t alloc = sdsalloc(s);
    return sdsHdrSize(s[-1])+alloc+1;
}

/* Return the pointer of the actual SDS allocation (normally SDS strings
 * are referenced by the start of the string buffer). */
//返回实际 SDS 分配的指针（通常 SDS 字符串由字符串缓冲区的开头引用）。
void *sdsAllocPtr(sds s) {
    return (void*) (s-sdsHdrSize(s[-1]));
}

/* Increment the sds length and decrements the left free space at the
 * end of the string according to 'incr'. Also set the null term
 * in the new end of the string.
 *
 * This function is used in order to fix the string length after the
 * user calls sdsMakeRoomFor(), writes something after the end of
 * the current string, and finally needs to set the new length.
 *
 * Note: it is possible to use a negative increment in order to
 * right-trim the string.
 *
 * Usage example:
 *
 * Using sdsIncrLen() and sdsMakeRoomFor() it is possible to mount the
 * following schema, to cat bytes coming from the kernel to the end of an
 * sds string without copying into an intermediate buffer:
 *
 * oldlen = sdslen(s);
 * s = sdsMakeRoomFor(s, BUFFER_SIZE);
 * nread = read(fd, s+oldlen, BUFFER_SIZE);
 * ... check for nread <= 0 and handle it ...
 * sdsIncrLen(s, nread);
 */
/**
 * 根据'incr'增加sds长度并减少字符串末尾的剩余可用空间。
 * 还要在字符串的新末尾设置空项。
 * 该函数用于在用户调用 sdsMakeRoomFor() 后固定字符串长度，在当前字符串末尾写入一些内容，最后需要设置新长度。
 * 注意：可以使用负增量来右修剪字符串。
 * 使用示例：使用 sdsIncrLen() 和 sdsMakeRoomFor() 可以挂载以下模式，将来自内核的字节分类到 sds 字符串的末尾，
 * 而无需复制到中间缓冲区：
 * oldlen = sdslen(s);
 * s = sdsMakeRoomFor(s, BUFFER_SIZE);
 * nread = read(fd, s+oldlen, BUFFER_SIZE);
 * ... 检查 nread <= 0 并处理它 ...
 * sdsIncrLen(s, nread);
 * */
void sdsIncrLen(sds s, ssize_t incr) {
    unsigned char flags = s[-1];
    size_t len;
    switch(flags&SDS_TYPE_MASK) {
        case SDS_TYPE_5: {
            unsigned char *fp = ((unsigned char*)s)-1;
            unsigned char oldlen = SDS_TYPE_5_LEN(flags);
            assert((incr > 0 && oldlen+incr < 32) || (incr < 0 && oldlen >= (unsigned int)(-incr)));
            *fp = SDS_TYPE_5 | ((oldlen+incr) << SDS_TYPE_BITS);//将增长后的长度放在flags的高5位
            len = oldlen+incr;
            break;
        }
        case SDS_TYPE_8: {
            SDS_HDR_VAR(8,s);
            assert((incr >= 0 && sh->alloc-sh->len >= incr) || (incr < 0 && sh->len >= (unsigned int)(-incr)));
            len = (sh->len += incr);
            break;
        }
        case SDS_TYPE_16: {
            SDS_HDR_VAR(16,s);
            assert((incr >= 0 && sh->alloc-sh->len >= incr) || (incr < 0 && sh->len >= (unsigned int)(-incr)));
            len = (sh->len += incr);
            break;
        }
        case SDS_TYPE_32: {
            SDS_HDR_VAR(32,s);
            assert((incr >= 0 && sh->alloc-sh->len >= (unsigned int)incr) || (incr < 0 && sh->len >= (unsigned int)(-incr)));
            len = (sh->len += incr);
            break;
        }
        case SDS_TYPE_64: {
            SDS_HDR_VAR(64,s);
            assert((incr >= 0 && sh->alloc-sh->len >= (uint64_t)incr) || (incr < 0 && sh->len >= (uint64_t)(-incr)));
            len = (sh->len += incr);
            break;
        }
        default: len = 0; /* Just to avoid compilation warnings. 只是为了避免编译警告。*/
    }
    s[len] = '\0';
}

/* Grow the sds to have the specified length. Bytes that were not part of
 * the original length of the sds will be set to zero.
 *
 * if the specified length is smaller than the current length, no operation
 * is performed. */
//将 sds 增长到指定的长度。不属于 sds 原始长度的字节将设置为零。如果指定长度小于当前长度，则不执行任何操作。
sds sdsgrowzero(sds s, size_t len) {
    size_t curlen = sdslen(s);

    if (len <= curlen) return s;
    s = sdsMakeRoomFor(s,len-curlen);
    if (s == NULL) return NULL;

    /* Make sure added region doesn't contain garbage */
    //确保添加的区域不包含垃圾
    memset(s+curlen,0,(len-curlen+1)); /* also set trailing \0 byte 还设置尾随 \0 字节*/
    sdssetlen(s, len);
    return s;
}

/* Append the specified binary-safe string pointed by 't' of 'len' bytes to the
 * end of the specified sds string 's'.
 *
 * After the call, the passed sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
/**
 * 将 'len' 字节的 't' 指向的指定二进制安全字符串附加到指定 sds 字符串 's' 的末尾。
 * 调用后，传递的 sds 字符串不再有效，所有引用都必须替换为调用返回的新指针。
 * */
sds sdscatlen(sds s, const void *t, size_t len) {
    size_t curlen = sdslen(s);

    s = sdsMakeRoomFor(s,len);
    if (s == NULL) return NULL;
    memcpy(s+curlen, t, len);
    sdssetlen(s, curlen+len);
    s[curlen+len] = '\0';
    return s;
}

/* Append the specified null terminated C string to the sds string 's'.
 *
 * After the call, the passed sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
/**
 * 将指定的以空结尾的 C 字符串附加到 sds 字符串“s”。
 * 调用后，传递的 sds 字符串不再有效，所有引用都必须替换为调用返回的新指针。
 * */
sds sdscat(sds s, const char *t) {
    return sdscatlen(s, t, strlen(t));
}

/* Append the specified sds 't' to the existing sds 's'.
 *
 * After the call, the modified sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
/**
 * 将指定的 sds 't' 附加到现有的 sds 's'。
 * 调用后，修改后的 sds 字符串不再有效，所有引用都必须替换为调用返回的新指针。
 * */
sds sdscatsds(sds s, const sds t) {
    return sdscatlen(s, t, sdslen(t));
}

/* Destructively modify the sds string 's' to hold the specified binary
 * safe string pointed by 't' of length 'len' bytes. */
//破坏性地修改 sds 字符串 's' 以保存由长度为 'len' 字节的 't' 指向的指定二进制安全字符串。
sds sdscpylen(sds s, const char *t, size_t len) {
    if (sdsalloc(s) < len) {
        s = sdsMakeRoomFor(s,len-sdslen(s));
        if (s == NULL) return NULL;
    }
    memcpy(s, t, len);
    s[len] = '\0';
    sdssetlen(s, len);
    return s;
}

/* Like sdscpylen() but 't' must be a null-terminated string so that the length
 * of the string is obtained with strlen(). */
//与 sdscpylen() 类似，但 't' 必须是一个以 null 结尾的字符串，以便使用 strlen() 获得字符串的长度。
sds sdscpy(sds s, const char *t) {
    return sdscpylen(s, t, strlen(t));
}

/* Helper for sdscatlonglong() doing the actual number -> string
 * conversion. 's' must point to a string with room for at least
 * SDS_LLSTR_SIZE bytes.
 *
 * The function returns the length of the null-terminated string
 * representation stored at 's'. */
/**
 * sdscatlonglong() 的助手进行实际数字 -> 字符串转换。
 * 's' 必须指向一个至少有 SDS_LLSTR_SIZE 字节空间的字符串。
 * 该函数返回存储在“s”处的以空字符结尾的字符串表示的长度。*/
#define SDS_LLSTR_SIZE 21
int sdsll2str(char *s, long long value) {
    char *p, aux;
    unsigned long long v;
    size_t l;

    /* Generate the string representation, this method produces
     * a reversed string. */
    //生成字符串表示，这个方法产生一个反转的字符串。
    if (value < 0) {
        /* Since v is unsigned, if value==LLONG_MIN, -LLONG_MIN will overflow. */
        //由于 v 是无符号的，如果 value==LLONG_MIN，-LLONG_MIN 将溢出。
        if (value != LLONG_MIN) {
            v = -value;
        } else {
            v = ((unsigned long long)LLONG_MAX) + 1;
        }
    } else {
        v = value;
    }

    p = s;
    do {//反转数字,并转化为字符串
        *p++ = '0'+(v%10);
        v /= 10;
    } while(v);
    if (value < 0) *p++ = '-';

    /* Compute length and add null term. */
    //计算长度并添加空项。
    l = p-s;
    *p = '\0';

    /* Reverse the string. */
    //反转字符串,反转后放在s处。
    p--;
    while(s < p) {
        aux = *s;
        *s = *p;
        *p = aux;
        s++;
        p--;
    }
    return l;
}

/* Identical sdsll2str(), but for unsigned long long type. */
//相同的 sdsll2str()，但用于 unsigned long long 类型。
int sdsull2str(char *s, unsigned long long v) {
    char *p, aux;
    size_t l;

    /* Generate the string representation, this method produces
     * a reversed string. */
    //生成字符串表示，这个方法产生一个反转的字符串。
    p = s;
    do {
        *p++ = '0'+(v%10);
        v /= 10;
    } while(v);

    /* Compute length and add null term. */
    //计算长度并添加空项。
    l = p-s;
    *p = '\0';

    /* Reverse the string. */
    //反转字符串。
    p--;
    while(s < p) {
        aux = *s;
        *s = *p;
        *p = aux;
        s++;
        p--;
    }
    return l;
}

/* Create an sds string from a long long value. It is much faster than:
 *
 * sdscatprintf(sdsempty(),"%lld\n", value);
 */
//从 long long 值创建一个 sds 字符串。它比： sdscatprintf(sdsempty(),"%lld\n", value); 快得多
sds sdsfromlonglong(long long value) {
    char buf[SDS_LLSTR_SIZE];
    int len = sdsll2str(buf,value);

    return sdsnewlen(buf,len);
}

/* Like sdscatprintf() but gets va_list instead of being variadic. */
//像 sdscatprintf() 但获取 va_list 而不是可变参数。
sds sdscatvprintf(sds s, const char *fmt, va_list ap) {
    va_list cpy;
    char staticbuf[1024], *buf = staticbuf, *t;
    size_t buflen = strlen(fmt)*2;
    int bufstrlen;

    /* We try to start using a static buffer for speed.
     * If not possible we revert to heap allocation. */
    //我们尝试开始使用静态缓冲区来提高速度。如果不可能，我们将恢复到堆分配。
    if (buflen > sizeof(staticbuf)) {
        buf = s_malloc(buflen);
        if (buf == NULL) return NULL;
    } else {
        buflen = sizeof(staticbuf);
    }

    /* Alloc enough space for buffer and \0 after failing to
     * fit the string in the current buffer size. */
    //在当前缓冲区大小无法容纳字符串后，为缓冲区和 \0 分配足够的空间。
    while(1) {
        va_copy(cpy,ap);
        bufstrlen = vsnprintf(buf, buflen, fmt, cpy);
        va_end(cpy);
        if (bufstrlen < 0) {
            if (buf != staticbuf) s_free(buf);
            return NULL;
        }
        if (((size_t)bufstrlen) >= buflen) {
            if (buf != staticbuf) s_free(buf);
            buflen = ((size_t)bufstrlen) + 1;
            buf = s_malloc(buflen);
            if (buf == NULL) return NULL;
            continue;
        }
        break;
    }

    /* Finally concat the obtained string to the SDS string and return it. */
    //最后将得到的字符串concat到SDS字符串并返回。
    t = sdscatlen(s, buf, bufstrlen);
    if (buf != staticbuf) s_free(buf);
    return t;
}

/* Append to the sds string 's' a string obtained using printf-alike format
 * specifier.
 *
 * After the call, the modified sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call.
 *
 * Example:
 *
 * s = sdsnew("Sum is: ");
 * s = sdscatprintf(s,"%d+%d = %d",a,b,a+b).
 *
 * Often you need to create a string from scratch with the printf-alike
 * format. When this is the need, just use sdsempty() as the target string:
 *
 * s = sdscatprintf(sdsempty(), "... your format ...", args);
 */
/**
 * 将使用类似 printf 的格式说明符获得的字符串附加到 sds 字符串“s”。
 * 调用后，修改后的 sds 字符串不再有效，所有引用都必须替换为调用返回的新指针。
 * 示例：
 * s = sdsnew("总和为：");
 * s = sdscatprintf(s,"%d+%d = %d",a,b,a+b)。
 * 通常，您需要使用类似 printf 的格式从头开始创建字符串。
 * 如果需要，只需使用 sdsempty() 作为目标字符串：
 * s = sdscatprintf(sdsempty(), "... your format ...", args);
 * */
sds sdscatprintf(sds s, const char *fmt, ...) {
    va_list ap;
    char *t;
    va_start(ap, fmt);
    t = sdscatvprintf(s,fmt,ap);
    va_end(ap);
    return t;
}

/* This function is similar to sdscatprintf, but much faster as it does
 * not rely on sprintf() family functions implemented by the libc that
 * are often very slow. Moreover directly handling the sds string as
 * new data is concatenated provides a performance improvement.
 *
 * However this function only handles an incompatible subset of printf-alike
 * format specifiers:
 *
 * %s - C String
 * %S - SDS string
 * %i - signed int
 * %I - 64 bit signed integer (long long, int64_t)
 * %u - unsigned int
 * %U - 64 bit unsigned integer (unsigned long long, uint64_t)
 * %% - Verbatim "%" character.
 */
/**
 * 此函数类似于 sdscatprintf，但速度要快得多，因为它不依赖于 libc 实现的通常非常慢的 sprintf() 系列函数。
 * 此外，在连接新数据时直接处理 sds 字符串可以提高性能。
 * 然而这个函数只处理 printf-alike 格式说明符的不兼容子集：
 * %s - C String
 * %S - SDS string
 * %i - signed int
 * %I - 64 bit signed integer (long long, int64_t)
 * %u - unsigned int
 * %U - 64 位无符号整数 (unsigned long long, uint64_t)
 * %% - 逐字“%”字符。
 * */
sds sdscatfmt(sds s, char const *fmt, ...) {
    size_t initlen = sdslen(s);
    const char *f = fmt;
    long i;
    va_list ap;

    /* To avoid continuous reallocations, let's start with a buffer that
     * can hold at least two times the format string itself. It's not the
     * best heuristic but seems to work in practice. */
    //为了避免连续的重新分配，让我们从一个可以容纳至少两倍于格式字符串本身的缓冲区开始。
    // 这不是最好的启发式方法，但似乎在实践中有效。
    s = sdsMakeRoomFor(s, strlen(fmt)*2);
    va_start(ap,fmt);
    f = fmt;    /* Next format specifier byte to process. 下一个要处理的格式说明符字节。 */
    i = initlen; /* Position of the next byte to write to dest str. 要写入 dest str 的下一个字节的位置。*/
    while(*f) {
        char next, *str;
        size_t l;
        long long num;
        unsigned long long unum;

        /* Make sure there is always space for at least 1 char. */
        //确保始终有至少 1 个字符的空间。
        if (sdsavail(s)==0) {
            s = sdsMakeRoomFor(s,1);
        }

        switch(*f) {
            case '%':
                next = *(f+1);
                if (next == '\0') break;
                f++;
                switch(next) {
                    case 's':
                    case 'S':
                        str = va_arg(ap,char*);
                        l = (next == 's') ? strlen(str) : sdslen(str);
                        if (sdsavail(s) < l) {
                            s = sdsMakeRoomFor(s,l);
                        }
                        memcpy(s+i,str,l);
                        sdsinclen(s,l);
                        i += l;
                        break;
                    case 'i':
                    case 'I':
                        if (next == 'i')
                            num = va_arg(ap,int);
                        else
                            num = va_arg(ap,long long);
                        {
                            char buf[SDS_LLSTR_SIZE];
                            l = sdsll2str(buf,num);
                            if (sdsavail(s) < l) {
                                s = sdsMakeRoomFor(s,l);
                            }
                            memcpy(s+i,buf,l);
                            sdsinclen(s,l);
                            i += l;
                        }
                        break;
                    case 'u':
                    case 'U':
                        if (next == 'u')
                            unum = va_arg(ap,unsigned int);
                        else
                            unum = va_arg(ap,unsigned long long);
                        {
                            char buf[SDS_LLSTR_SIZE];
                            l = sdsull2str(buf,unum);
                            if (sdsavail(s) < l) {
                                s = sdsMakeRoomFor(s,l);
                            }
                            memcpy(s+i,buf,l);
                            sdsinclen(s,l);
                            i += l;
                        }
                        break;
                    default: /* Handle %% and generally %<unknown>. 处理 %% 并且通常处理 %<unknown>。*/
                        s[i++] = next;
                        sdsinclen(s,1);
                        break;
                }
                break;
            default:
                s[i++] = *f;
                sdsinclen(s,1);
                break;
        }
        f++;
    }
    va_end(ap);

    /* Add null-term */
    //添加空项
    s[i] = '\0';
    return s;
}

/* Remove the part of the string from left and from right composed just of
 * contiguous characters found in 'cset', that is a null terminated C string.
 *
 * After the call, the modified sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call.
 *
 * Example:
 *
 * s = sdsnew("AA...AA.a.aa.aHelloWorld     :::");
 * s = sdstrim(s,"Aa. :");
 * printf("%s\n", s);
 *
 * Output will be just "HelloWorld".
 */
/**
 * 从左侧和右侧删除仅由在“cset”中找到的连续字符组成的字符串部分，即空终止的 C 字符串。
 * 调用后，修改后的 sds 字符串不再有效，所有引用都必须替换为调用返回的新指针。
 * 示例：
 * s = sdsnew("AA...AA.a.aa.aHelloWorld :::");
 * s = sdstrim(s,"Aa. :");
 * printf("%s\n", s);
 * 输出将只是“HelloWorld”。
 * */
sds sdstrim(sds s, const char *cset) {
    char *end, *sp, *ep;
    size_t len;

    sp = s;
    ep = end = s+sdslen(s)-1;
    while(sp <= end && strchr(cset, *sp)) sp++;
    while(ep > sp && strchr(cset, *ep)) ep--;
    len = (ep-sp)+1;
    if (s != sp) memmove(s, sp, len);
    s[len] = '\0';
    sdssetlen(s,len);
    return s;
}

/* Changes the input string to be a subset of the original.
 * It does not release the free space in the string, so a call to
 * sdsRemoveFreeSpace may be wise after. */
//将输入字符串更改为原始字符串的子集。它不会释放字符串中的可用空间，因此调用 sdsRemoveFreeSpace 可能是明智之举。
void sdssubstr(sds s, size_t start, size_t len) {
    /* Clamp out of range input */
    //钳位超出范围输入
    size_t oldlen = sdslen(s);
    if (start >= oldlen) start = len = 0;
    if (len > oldlen-start) len = oldlen-start;

    /* Move the data */
    //移动数据
    if (len) memmove(s, s+start, len);
    s[len] = 0;
    sdssetlen(s,len);
}

/* Turn the string into a smaller (or equal) string containing only the
 * substring specified by the 'start' and 'end' indexes.
 *
 * start and end can be negative, where -1 means the last character of the
 * string, -2 the penultimate character, and so forth.
 *
 * The interval is inclusive, so the start and end characters will be part
 * of the resulting string.
 *
 * The string is modified in-place.
 *
 * NOTE: this function can be misleading and can have unexpected behaviour,
 * specifically when you want the length of the new string to be 0.
 * Having start==end will result in a string with one character.
 * please consider using sdssubstr instead.
 *
 * Example:
 *
 * s = sdsnew("Hello World");
 * sdsrange(s,1,-1); => "ello World"
 */
/**
 * 将字符串变成一个较小（或相等）的字符串，只包含由“开始”和“结束”索引指定的子字符串。
 * start 和 end 可以是负数，其中 -1 表示字符串的最后一个字符，-2 表示倒数第二个字符，依此类推。
 * 间隔包含在内，因此开始和结束字符将成为结果字符串的一部分。
 * 字符串被就地修改。注意：此函数可能会产生误导，并且可能会出现意外行为，特别是当您希望新字符串的长度为 0 时。
 * 设置 start==end 将导致字符串只有一个字符。请考虑改用 sdssubstr。
 * 示例：s = sdsnew("Hello World"); sdsrange(s,1,-1); =>“你好世界”
 * */
void sdsrange(sds s, ssize_t start, ssize_t end) {
    size_t newlen, len = sdslen(s);
    if (len == 0) return;
    if (start < 0)
        start = len + start;
    if (end < 0)
        end = len + end;
    newlen = (start > end) ? 0 : (end-start)+1;
    sdssubstr(s, start, newlen);
}

/* Apply tolower() to every character of the sds string 's'. */
//将 tolower() 应用于 sds 字符串 's' 的每个字符。
void sdstolower(sds s) {
    size_t len = sdslen(s), j;

    for (j = 0; j < len; j++) s[j] = tolower(s[j]);
}

/* Apply toupper() to every character of the sds string 's'. */
//将 toupper() 应用于 sds 字符串 's' 的每个字符。
void sdstoupper(sds s) {
    size_t len = sdslen(s), j;

    for (j = 0; j < len; j++) s[j] = toupper(s[j]);
}

/* Compare two sds strings s1 and s2 with memcmp().
 *
 * Return value:
 *
 *     positive if s1 > s2.
 *     negative if s1 < s2.
 *     0 if s1 and s2 are exactly the same binary string.
 *
 * If two strings share exactly the same prefix, but one of the two has
 * additional characters, the longer string is considered to be greater than
 * the smaller one. */
/**
 * 用 memcmp() 比较两个 sds 字符串 s1 和 s2。返回值：如果 s1 > s2，则为正。如果 s1 < s2，则为负数。
 * 如果 s1 和 s2 是完全相同的二进制字符串，则为 0。
 * 如果两个字符串共享完全相同的前缀，但其中一个有附加字符，则认为较长的字符串大于较小的字符串。
 * */
int sdscmp(const sds s1, const sds s2) {
    size_t l1, l2, minlen;
    int cmp;

    l1 = sdslen(s1);
    l2 = sdslen(s2);
    minlen = (l1 < l2) ? l1 : l2;
    cmp = memcmp(s1,s2,minlen);
    if (cmp == 0) return l1>l2? 1: (l1<l2? -1: 0);
    return cmp;
}

/* Split 's' with separator in 'sep'. An array
 * of sds strings is returned. *count will be set
 * by reference to the number of tokens returned.
 *
 * On out of memory, zero length string, zero length
 * separator, NULL is returned.
 *
 * Note that 'sep' is able to split a string using
 * a multi-character separator. For example
 * sdssplit("foo_-_bar","_-_"); will return two
 * elements "foo" and "bar".
 *
 * This version of the function is binary-safe but
 * requires length arguments. sdssplit() is just the
 * same function but for zero-terminated strings.
 */
/**
 * 在 'sep' 中使用分隔符拆分 's'。返回一个 sds 字符串数组。
 * count 将通过引用返回的令牌数来设置。如果内存不足，则返回零长度字符串、零长度分隔符、NULL。
 * 请注意，'sep' 能够使用多字符分隔符拆分字符串。
 * 例如 sdssplit("foo_-_bar","_-_");将返回两个元素“foo”和“bar”。
 * 此版本的函数是二进制安全的，但需要长度参数。
 * sdssplit() 只是相同的函数，但用于以零结尾的字符串。
 * */
sds *sdssplitlen(const char *s, ssize_t len, const char *sep, int seplen, int *count) {
    int elements = 0, slots = 5;
    long start = 0, j;
    sds *tokens;

    if (seplen < 1 || len <= 0) {
        *count = 0;
        return NULL;
    }
    tokens = s_malloc(sizeof(sds)*slots);
    if (tokens == NULL) return NULL;

    for (j = 0; j < (len-(seplen-1)); j++) {
        /* make sure there is room for the next element and the final one */
        //确保有空间容纳下一个元素和最后一个元素
        if (slots < elements+2) {
            sds *newtokens;

            slots *= 2;
            newtokens = s_realloc(tokens,sizeof(sds)*slots);
            if (newtokens == NULL) goto cleanup;
            tokens = newtokens;
        }
        /* search the separator */
        //搜索分隔符
        if ((seplen == 1 && *(s+j) == sep[0]) || (memcmp(s+j,sep,seplen) == 0)) {
            tokens[elements] = sdsnewlen(s+start,j-start);
            if (tokens[elements] == NULL) goto cleanup;
            elements++;
            start = j+seplen;
            j = j+seplen-1; /* skip the separator 跳过分隔符*/
        }
    }
    /* Add the final element. We are sure there is room in the tokens array. */
    //添加最后一个元素。我们确信令牌数组中有空间。
    tokens[elements] = sdsnewlen(s+start,len-start);
    if (tokens[elements] == NULL) goto cleanup;
    elements++;
    *count = elements;
    return tokens;

    cleanup:
    {
        int i;
        for (i = 0; i < elements; i++) sdsfree(tokens[i]);
        s_free(tokens);
        *count = 0;
        return NULL;
    }
}

/* Free the result returned by sdssplitlen(), or do nothing if 'tokens' is NULL. */
//释放 sdssplitlen() 返回的结果，如果 'tokens' 为 NULL，则不执行任何操作。
void sdsfreesplitres(sds *tokens, int count) {
    if (!tokens) return;
    while(count--)
        sdsfree(tokens[count]);
    s_free(tokens);
}

/* Append to the sds string "s" an escaped string representation where
 * all the non-printable characters (tested with isprint()) are turned into
 * escapes in the form "\n\r\a...." or "\x<hex-number>".
 *
 * After the call, the modified sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */

/**
 * 向 sds 字符串“s”附加一个转义字符串表示形式，
 * 其中所有不可打印字符（用 isprint() 测试）都以“\n\r\a....”或“\x<十六进制数>”。
 * 调用后，修改后的 sds 字符串不再有效，所有引用都必须替换为调用返回的新指针。
 * */
sds sdscatrepr(sds s, const char *p, size_t len) {
    s = sdscatlen(s,"\"",1);
    while(len--) {
        switch(*p) {
            case '\\':
            case '"':
                s = sdscatprintf(s,"\\%c",*p);
                break;
            case '\n': s = sdscatlen(s,"\\n",2); break;
            case '\r': s = sdscatlen(s,"\\r",2); break;
            case '\t': s = sdscatlen(s,"\\t",2); break;
            case '\a': s = sdscatlen(s,"\\a",2); break;
            case '\b': s = sdscatlen(s,"\\b",2); break;
            default:
                if (isprint(*p))
                    s = sdscatprintf(s,"%c",*p);
                else
                    s = sdscatprintf(s,"\\x%02x",(unsigned char)*p);
                break;
        }
        p++;
    }
    return sdscatlen(s,"\"",1);
}

/* Returns one if the string contains characters to be escaped
 * by sdscatrepr(), zero otherwise.
 *
 * Typically, this should be used to help protect aggregated strings in a way
 * that is compatible with sdssplitargs(). For this reason, also spaces will be
 * treated as needing an escape.
 */
/**
 * 如果字符串包含要被 sdscatrepr() 转义的字符，则返回 1，否则返回 0。
 * 通常，这应该用于以与 sdssplitargs() 兼容的方式帮助保护聚合字符串。
 * 出于这个原因，空格也将被视为需要转义。
 * */
int sdsneedsrepr(const sds s) {
    size_t len = sdslen(s);
    const char *p = s;

    while (len--) {
        if (*p == '\\' || *p == '"' || *p == '\n' || *p == '\r' ||
            *p == '\t' || *p == '\a' || *p == '\b' || !isprint(*p) || isspace(*p)) return 1;
        p++;
    }

    return 0;
}

/* Helper function for sdssplitargs() that returns non zero if 'c'
 * is a valid hex digit. */
//如果 'c' 是有效的十六进制数字，则 sdssplitargs() 的帮助函数返回非零。
int is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

/* Helper function for sdssplitargs() that converts a hex digit into an
 * integer from 0 to 15 */
//sdssplitargs() 的辅助函数，将十六进制数字转换为 0 到 15 的整数
int hex_digit_to_int(char c) {
    switch(c) {
        case '0': return 0;
        case '1': return 1;
        case '2': return 2;
        case '3': return 3;
        case '4': return 4;
        case '5': return 5;
        case '6': return 6;
        case '7': return 7;
        case '8': return 8;
        case '9': return 9;
        case 'a': case 'A': return 10;
        case 'b': case 'B': return 11;
        case 'c': case 'C': return 12;
        case 'd': case 'D': return 13;
        case 'e': case 'E': return 14;
        case 'f': case 'F': return 15;
        default: return 0;
    }
}

/* Split a line into arguments, where every argument can be in the
 * following programming-language REPL-alike form:
 *
 * foo bar "newline are supported\n" and "\xff\x00otherstuff"
 *
 * The number of arguments is stored into *argc, and an array
 * of sds is returned.
 *
 * The caller should free the resulting array of sds strings with
 * sdsfreesplitres().
 *
 * Note that sdscatrepr() is able to convert back a string into
 * a quoted string in the same format sdssplitargs() is able to parse.
 *
 * The function returns the allocated tokens on success, even when the
 * input string is empty, or NULL if the input contains unbalanced
 * quotes or closed quotes followed by non space characters
 * as in: "foo"bar or "foo'
 */
/**
 * 将一行拆分为参数，其中每个参数都可以采用以下类似于 REPL 的编程语言形式：
 * foo bar "newline are supported\n" 和 "\xff\x00otherstuff" 参数的数量存储在 argc 中，以及一个数组的 sds 被退回。
 * 调用者应该使用 sdsfreesplitres() 释放生成的 sds 字符串数组。
 * 请注意，sdscatrepr() 能够将字符串转换回带引号的字符串，格式与 sdssplitargs() 能够解析的格式相同。
 * 该函数在成功时返回分配的标记，即使输入字符串为空，
 * 如果输入包含不平衡引号或闭合引号后跟非空格字符，则返回 NULL，如：“foo”bar 或“foo”
 * */
sds *sdssplitargs(const char *line, int *argc) {
    const char *p = line;
    char *current = NULL;
    char **vector = NULL;

    *argc = 0;
    while(1) {
        /* skip blanks */
        while(*p && isspace(*p)) p++;//跳过空格
        if (*p) {
            /* get a token */
            //获得令牌
            int inq=0;  /* set to 1 if we are in "quotes" 如果我们在“引号”中，则设置为 1*/
            int insq=0; /* set to 1 if we are in 'single quotes' 如果我们在“单引号”中，则设置为 1*/
            int done=0;

            if (current == NULL) current = sdsempty();
            while(!done) {
                if (inq) {
                    if (*p == '\\' && *(p+1) == 'x' &&
                        is_hex_digit(*(p+2)) &&
                        is_hex_digit(*(p+3)))
                    {
                        unsigned char byte;

                        byte = (hex_digit_to_int(*(p+2))*16)+
                               hex_digit_to_int(*(p+3));
                        current = sdscatlen(current,(char*)&byte,1);
                        p += 3;
                    } else if (*p == '\\' && *(p+1)) {
                        char c;

                        p++;
                        switch(*p) {
                            case 'n': c = '\n'; break;
                            case 'r': c = '\r'; break;
                            case 't': c = '\t'; break;
                            case 'b': c = '\b'; break;
                            case 'a': c = '\a'; break;
                            default: c = *p; break;
                        }
                        current = sdscatlen(current,&c,1);
                    } else if (*p == '"') {
                        /* closing quote must be followed by a space or
                         * nothing at all. */
                        //结束引号后面必须跟一个空格或什么都没有。
                        if (*(p+1) && !isspace(*(p+1))) goto err;
                        done=1;
                    } else if (!*p) {
                        /* unterminated quotes */
                        //未结束的引号
                        goto err;
                    } else {
                        current = sdscatlen(current,p,1);
                    }
                } else if (insq) {
                    if (*p == '\\' && *(p+1) == '\'') {
                        p++;
                        current = sdscatlen(current,"'",1);
                    } else if (*p == '\'') {
                        /* closing quote must be followed by a space or
                         * nothing at all. */
                        //结束引号后面必须跟一个空格或什么都没有。
                        if (*(p+1) && !isspace(*(p+1))) goto err;
                        done=1;
                    } else if (!*p) {
                        /* unterminated quotes */
                        //未结束的引号
                        goto err;
                    } else {
                        current = sdscatlen(current,p,1);
                    }
                } else {
                    switch(*p) {
                        case ' ':
                        case '\n':
                        case '\r':
                        case '\t':
                        case '\0':
                            done=1;
                            break;
                        case '"':
                            inq=1;
                            break;
                        case '\'':
                            insq=1;
                            break;
                        default:
                            current = sdscatlen(current,p,1);
                            break;
                    }
                }
                if (*p) p++;
            }
            /* add the token to the vector */
            //将标记添加到向量中
            vector = s_realloc(vector,((*argc)+1)*sizeof(char*));
            vector[*argc] = current;
            (*argc)++;
            current = NULL;
        } else {
            /* Even on empty input string return something not NULL. */
            //即使在空输入字符串上也返回不为 NULL 的内容。
            if (vector == NULL) vector = s_malloc(sizeof(void*));
            return vector;
        }
    }

    err:
    while((*argc)--)
        sdsfree(vector[*argc]);
    s_free(vector);
    if (current) sdsfree(current);
    *argc = 0;
    return NULL;
}

/* Modify the string substituting all the occurrences of the set of
 * characters specified in the 'from' string to the corresponding character
 * in the 'to' array.
 *
 * For instance: sdsmapchars(mystring, "ho", "01", 2)
 * will have the effect of turning the string "hello" into "0ell1".
 *
 * The function returns the sds string pointer, that is always the same
 * as the input pointer since no resize is needed. */
/**
 * 修改字符串，将“from”字符串中指定的所有字符集替换为“to”数组中的相应字符。
 * 例如：
 * sdsmapchars(mystring, "ho", "01", 2)
 * 将具有将字符串 "hello" 转换为 "0ell1" 的效果。
 * 该函数返回 sds 字符串指针，该指针始终与输入指针相同，因为不需要调整大小。
 * */
sds sdsmapchars(sds s, const char *from, const char *to, size_t setlen) {
    size_t j, i, l = sdslen(s);

    for (j = 0; j < l; j++) {
        for (i = 0; i < setlen; i++) {
            if (s[j] == from[i]) {
                s[j] = to[i];
                break;
            }
        }
    }
    return s;
}

/* Join an array of C strings using the specified separator (also a C string).
 * Returns the result as an sds string. */
//使用指定的分隔符连接一个 C 字符串数组（也是一个 C 字符串）。将结果作为 sds 字符串返回。
sds sdsjoin(char **argv, int argc, char *sep) {
    sds join = sdsempty();
    int j;

    for (j = 0; j < argc; j++) {
        join = sdscat(join, argv[j]);
        if (j != argc-1) join = sdscat(join,sep);
    }
    return join;
}

/* Like sdsjoin, but joins an array of SDS strings. */
//与 sdsjoin 类似，但加入了 SDS 字符串数组。
sds sdsjoinsds(sds *argv, int argc, const char *sep, size_t seplen) {
    sds join = sdsempty();
    int j;

    for (j = 0; j < argc; j++) {
        join = sdscatsds(join, argv[j]);
        if (j != argc-1) join = sdscatlen(join,sep,seplen);
    }
    return join;
}

/* Wrappers to the allocators used by SDS. Note that SDS will actually
 * just use the macros defined into sdsalloc.h in order to avoid to pay
 * the overhead of function calls. Here we define these wrappers only for
 * the programs SDS is linked to, if they want to touch the SDS internals
 * even if they use a different allocator. */
/**
 * SDS 使用的分配器的包装器。
 * 请注意，SDS 实际上只使用定义在 sdsalloc.h 中的宏，以避免支付函数调用的开销。
 * 在这里，我们仅为 SDS 链接到的程序定义这些包装器，如果它们想要接触 SDS 内部，即使它们使用不同的分配器。
 * */
void *sds_malloc(size_t size) { return s_malloc(size); }
void *sds_realloc(void *ptr, size_t size) { return s_realloc(ptr,size); }
void sds_free(void *ptr) { s_free(ptr); }

/* Perform expansion of a template string and return the result as a newly
 * allocated sds.
 *
 * Template variables are specified using curly brackets, e.g. {variable}.
 * An opening bracket can be quoted by repeating it twice.
 */
/**
 * 执行模板字符串的扩展并将结果作为新分配的 sds 返回。
 * 模板变量使用大括号指定，例如{多变的}。可以通过重复两次来引用左括号。*/
sds sdstemplate(const char *template, sdstemplate_callback_t cb_func, void *cb_arg)
{
    sds res = sdsempty();
    const char *p = template;

    while (*p) {
        /* Find next variable, copy everything until there */
        const char *sv = strchr(p, '{');//找到下一个变量，复制所有内容直到那里
        if (!sv) {
            /* Not found: copy till rest of template and stop */
            res = sdscat(res, p);//未找到：复制到模板的其余部分并停止
            break;
        } else if (sv > p) {
            /* Found: copy anything up to the beginning of the variable */
            res = sdscatlen(res, p, sv - p);//找到：将任何内容复制到变量的开头
        }

        /* Skip into variable name, handle premature end or quoting */
        sv++;//跳入变量名，处理过早结束或引用
        if (!*sv) goto error; /* Premature end of template 模板过早结束*/
        if (*sv == '{') { //引用'{'
            /* Quoted '{' */
            p = sv + 1;
            res = sdscat(res, "{");
            continue;
        }

        /* Find end of variable name, handle premature end of template */
        const char *ev = strchr(sv, '}');//查找变量名的结尾，处理模板的过早结束
        if (!ev) goto error;

        /* Pass variable name to callback and obtain value. If callback failed,
         * abort. */
        sds varname = sdsnewlen(sv, ev - sv);//将变量名传递给回调并获取值。如果回调失败，则中止。
        sds value = cb_func(varname, cb_arg);
        sdsfree(varname);
        if (!value) goto error;

        /* Append value to result and continue */
        res = sdscat(res, value);//将值附加到结果并继续
        sdsfree(value);
        p = ev + 1;
    }

    return res;

    error:
    sdsfree(res);
    return NULL;
}

#ifdef REDIS_TEST
#include <stdio.h>
#include <limits.h>
#include "testhelp.h"

#define UNUSED(x) (void)(x)

static sds sdsTestTemplateCallback(sds varname, void *arg) {
    UNUSED(arg);
    static const char *_var1 = "variable1";
    static const char *_var2 = "variable2";

    if (!strcmp(varname, _var1)) return sdsnew("value1");
    else if (!strcmp(varname, _var2)) return sdsnew("value2");
    else return NULL;
}

int sdsTest(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    {
        sds x = sdsnew("foo"), y;

        test_cond("Create a string and obtain the length",
            sdslen(x) == 3 && memcmp(x,"foo\0",4) == 0);

        sdsfree(x);
        x = sdsnewlen("foo",2);
        test_cond("Create a string with specified length",
            sdslen(x) == 2 && memcmp(x,"fo\0",3) == 0);

        x = sdscat(x,"bar");
        test_cond("Strings concatenation",
            sdslen(x) == 5 && memcmp(x,"fobar\0",6) == 0);

        x = sdscpy(x,"a");
        test_cond("sdscpy() against an originally longer string",
            sdslen(x) == 1 && memcmp(x,"a\0",2) == 0);

        x = sdscpy(x,"xyzxxxxxxxxxxyyyyyyyyyykkkkkkkkkk");
        test_cond("sdscpy() against an originally shorter string",
            sdslen(x) == 33 &&
            memcmp(x,"xyzxxxxxxxxxxyyyyyyyyyykkkkkkkkkk\0",33) == 0);

        sdsfree(x);
        x = sdscatprintf(sdsempty(),"%d",123);
        test_cond("sdscatprintf() seems working in the base case",
            sdslen(x) == 3 && memcmp(x,"123\0",4) == 0);

        sdsfree(x);
        x = sdscatprintf(sdsempty(),"a%cb",0);
        test_cond("sdscatprintf() seems working with \\0 inside of result",
            sdslen(x) == 3 && memcmp(x,"a\0""b\0",4) == 0);

        {
            sdsfree(x);
            char etalon[1024*1024];
            for (size_t i = 0; i < sizeof(etalon); i++) {
                etalon[i] = '0';
            }
            x = sdscatprintf(sdsempty(),"%0*d",(int)sizeof(etalon),0);
            test_cond("sdscatprintf() can print 1MB",
                sdslen(x) == sizeof(etalon) && memcmp(x,etalon,sizeof(etalon)) == 0);
        }

        sdsfree(x);
        x = sdsnew("--");
        x = sdscatfmt(x, "Hello %s World %I,%I--", "Hi!", LLONG_MIN,LLONG_MAX);
        test_cond("sdscatfmt() seems working in the base case",
            sdslen(x) == 60 &&
            memcmp(x,"--Hello Hi! World -9223372036854775808,"
                     "9223372036854775807--",60) == 0);
        printf("[%s]\n",x);

        sdsfree(x);
        x = sdsnew("--");
        x = sdscatfmt(x, "%u,%U--", UINT_MAX, ULLONG_MAX);
        test_cond("sdscatfmt() seems working with unsigned numbers",
            sdslen(x) == 35 &&
            memcmp(x,"--4294967295,18446744073709551615--",35) == 0);

        sdsfree(x);
        x = sdsnew(" x ");
        sdstrim(x," x");
        test_cond("sdstrim() works when all chars match",
            sdslen(x) == 0);

        sdsfree(x);
        x = sdsnew(" x ");
        sdstrim(x," ");
        test_cond("sdstrim() works when a single char remains",
            sdslen(x) == 1 && x[0] == 'x');

        sdsfree(x);
        x = sdsnew("xxciaoyyy");
        sdstrim(x,"xy");
        test_cond("sdstrim() correctly trims characters",
            sdslen(x) == 4 && memcmp(x,"ciao\0",5) == 0);

        y = sdsdup(x);
        sdsrange(y,1,1);
        test_cond("sdsrange(...,1,1)",
            sdslen(y) == 1 && memcmp(y,"i\0",2) == 0);

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,1,-1);
        test_cond("sdsrange(...,1,-1)",
            sdslen(y) == 3 && memcmp(y,"iao\0",4) == 0);

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,-2,-1);
        test_cond("sdsrange(...,-2,-1)",
            sdslen(y) == 2 && memcmp(y,"ao\0",3) == 0);

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,2,1);
        test_cond("sdsrange(...,2,1)",
            sdslen(y) == 0 && memcmp(y,"\0",1) == 0);

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,1,100);
        test_cond("sdsrange(...,1,100)",
            sdslen(y) == 3 && memcmp(y,"iao\0",4) == 0);

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,100,100);
        test_cond("sdsrange(...,100,100)",
            sdslen(y) == 0 && memcmp(y,"\0",1) == 0);

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,4,6);
        test_cond("sdsrange(...,4,6)",
            sdslen(y) == 0 && memcmp(y,"\0",1) == 0);

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,3,6);
        test_cond("sdsrange(...,3,6)",
            sdslen(y) == 1 && memcmp(y,"o\0",2) == 0);

        sdsfree(y);
        sdsfree(x);
        x = sdsnew("foo");
        y = sdsnew("foa");
        test_cond("sdscmp(foo,foa)", sdscmp(x,y) > 0);

        sdsfree(y);
        sdsfree(x);
        x = sdsnew("bar");
        y = sdsnew("bar");
        test_cond("sdscmp(bar,bar)", sdscmp(x,y) == 0);

        sdsfree(y);
        sdsfree(x);
        x = sdsnew("aar");
        y = sdsnew("bar");
        test_cond("sdscmp(bar,bar)", sdscmp(x,y) < 0);

        sdsfree(y);
        sdsfree(x);
        x = sdsnewlen("\a\n\0foo\r",7);
        y = sdscatrepr(sdsempty(),x,sdslen(x));
        test_cond("sdscatrepr(...data...)",
            memcmp(y,"\"\\a\\n\\x00foo\\r\"",15) == 0);

        {
            unsigned int oldfree;
            char *p;
            int i;
            size_t step = 10, j;

            sdsfree(x);
            sdsfree(y);
            x = sdsnew("0");
            test_cond("sdsnew() free/len buffers", sdslen(x) == 1 && sdsavail(x) == 0);

            /* Run the test a few times in order to hit the first two
             * SDS header types. */
            for (i = 0; i < 10; i++) {
                size_t oldlen = sdslen(x);
                x = sdsMakeRoomFor(x,step);
                int type = x[-1]&SDS_TYPE_MASK;

                test_cond("sdsMakeRoomFor() len", sdslen(x) == oldlen);
                if (type != SDS_TYPE_5) {
                    test_cond("sdsMakeRoomFor() free", sdsavail(x) >= step);
                    oldfree = sdsavail(x);
                    UNUSED(oldfree);
                }
                p = x+oldlen;
                for (j = 0; j < step; j++) {
                    p[j] = 'A'+j;
                }
                sdsIncrLen(x,step);
            }
            test_cond("sdsMakeRoomFor() content",
                memcmp("0ABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJ",x,101) == 0);
            test_cond("sdsMakeRoomFor() final length",sdslen(x)==101);

            sdsfree(x);
        }

        /* Simple template */
        x = sdstemplate("v1={variable1} v2={variable2}", sdsTestTemplateCallback, NULL);
        test_cond("sdstemplate() normal flow",
                  memcmp(x,"v1=value1 v2=value2",19) == 0);
        sdsfree(x);

        /* Template with callback error */
        x = sdstemplate("v1={variable1} v3={doesnotexist}", sdsTestTemplateCallback, NULL);
        test_cond("sdstemplate() with callback error", x == NULL);

        /* Template with empty var name */
        x = sdstemplate("v1={", sdsTestTemplateCallback, NULL);
        test_cond("sdstemplate() with empty var name", x == NULL);

        /* Template with truncated var name */
        x = sdstemplate("v1={start", sdsTestTemplateCallback, NULL);
        test_cond("sdstemplate() with truncated var name", x == NULL);

        /* Template with quoting */
        x = sdstemplate("v1={{{variable1}} {{} v2={variable2}", sdsTestTemplateCallback, NULL);
        test_cond("sdstemplate() with quoting",
                  memcmp(x,"v1={value1} {} v2=value2",24) == 0);
        sdsfree(x);

        /* Test sdsresize - extend */
        x = sdsnew("1234567890123456789012345678901234567890");
        x = sdsResize(x, 200);
        test_cond("sdsrezie() expand len", sdslen(x) == 40);
        test_cond("sdsrezie() expand strlen", strlen(x) == 40);
        test_cond("sdsrezie() expand alloc", sdsalloc(x) == 200);
        /* Test sdsresize - trim free space */
        x = sdsResize(x, 80);
        test_cond("sdsrezie() shrink len", sdslen(x) == 40);
        test_cond("sdsrezie() shrink strlen", strlen(x) == 40);
        test_cond("sdsrezie() shrink alloc", sdsalloc(x) == 80);
        /* Test sdsresize - crop used space */
        x = sdsResize(x, 30);
        test_cond("sdsrezie() crop len", sdslen(x) == 30);
        test_cond("sdsrezie() crop strlen", strlen(x) == 30);
        test_cond("sdsrezie() crop alloc", sdsalloc(x) == 30);
        /* Test sdsresize - extend to different class */
        x = sdsResize(x, 400);
        test_cond("sdsrezie() expand len", sdslen(x) == 30);
        test_cond("sdsrezie() expand strlen", strlen(x) == 30);
        test_cond("sdsrezie() expand alloc", sdsalloc(x) == 400);
        /* Test sdsresize - shrink to different class */
        x = sdsResize(x, 4);
        test_cond("sdsrezie() crop len", sdslen(x) == 4);
        test_cond("sdsrezie() crop strlen", strlen(x) == 4);
        test_cond("sdsrezie() crop alloc", sdsalloc(x) == 4);
        sdsfree(x);
    }
    return 0;
}
#endif