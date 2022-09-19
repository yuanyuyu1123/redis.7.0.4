/* String -> String Map data structure optimized for size.
 * This file implements a data structure mapping strings to other strings
 * implementing an O(n) lookup data structure designed to be very memory
 * efficient.
 *
 * The Redis Hash type uses this data structure for hashes composed of a small
 * number of elements, to switch to a hash table once a given number of
 * elements is reached.
 *
 * Given that many times Redis Hashes are used to represent objects composed
 * of few fields, this is a very big win in terms of used memory.
 *
 * String -> String 为大小优化的映射数据结构。该文件实现了一个将字符串映射到其他字符串的数据结构，
 * 该结构实现了一个 O(n) 查找数据结构，旨在提高内存效率。 Redis Hash 类型将此数据结构用于由少量元素组成的哈希，
 * 一旦达到给定数量的元素，就会切换到哈希表。
 * 鉴于 Redis 哈希多次用于表示由少数字段组成的对象，这在已用内存方面是一个非常大的胜利。
 * --------------------------------------------------------------------------
 *
 * Copyright (c) 2009-2010, Salvatore Sanfilippo <antirez at gmail dot com>
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

/* Memory layout of a zipmap, for the map "foo" => "bar", "hello" => "world":
 *
 * <zmlen><len>"foo"<len><free>"bar"<len>"hello"<len><free>"world"
 *
 * <zmlen> is 1 byte length that holds the current size of the zipmap.
 * When the zipmap length is greater than or equal to 254, this value
 * is not used and the zipmap needs to be traversed to find out the length.
 *
 * <len> is the length of the following string (key or value).
 * <len> lengths are encoded in a single value or in a 5 bytes value.
 * If the first byte value (as an unsigned 8 bit value) is between 0 and
 * 253, it's a single-byte length. If it is 254 then a four bytes unsigned
 * integer follows (in the host byte ordering). A value of 255 is used to
 * signal the end of the hash.
 *
 * <free> is the number of free unused bytes after the string, resulting
 * from modification of values associated to a key. For instance if "foo"
 * is set to "bar", and later "foo" will be set to "hi", it will have a
 * free byte to use if the value will enlarge again later, or even in
 * order to add a key/value pair if it fits.
 *
 * <free> is always an unsigned 8 bit number, because if after an
 * update operation there are more than a few free bytes, the zipmap will be
 * reallocated to make sure it is as small as possible.
 *
 * The most compact representation of the above two elements hash is actually:
 *
 * "\x02\x03foo\x03\x00bar\x05hello\x05\x00world\xff"
 *
 * Note that because keys and values are prefixed length "objects",
 * the lookup will take O(N) where N is the number of elements
 * in the zipmap and *not* the number of bytes needed to represent the zipmap.
 * This lowers the constant times considerably.
 */
/**zipmap 的内存布局，
 * map "foo" => "bar", "hello" => "world":
 * <zmlen><len>"foo"<len><free>"bar"<len>"hello "<len><free>"world"
 * <zmlen> 是 1 字节长度，用于保存 zipmap 的当前大小。
 * 当zipmap长度大于等于254时，不使用该值，需要遍历zipmap找出长度。
 * <len> 是后面的字符串（键或值）的长度。 <len> 长度以单个值或 5 字节值编码。
 * 如果第一个字节值（作为无符号 8 位值）介于 0 和 253 之间，则它是单字节长度。
 * 如果它是 254，那么后面是一个四字节的无符号整数（在主机字节顺序中）。值 255 用于表示散列结束。
 * <free> 是字符串后面的空闲未使用字节数，由修改与键关联的值产生。
 * 例如，如果 "foo" 设置为 "bar"，然后 "foo" 将设置为 "hi"，如果值稍后再次放大，甚至为了添加键值，
 * 它将有一个空闲字节可供使用如果合适就配对。
 * <free> 始终是一个无符号的 8 位数字，因为如果在更新操作后有多个空闲字节，则会重新分配 zipmap 以确保它尽可能小。
 * 上面两个元素hash最简洁的表示其实是："\x02\x03foo\x03\x00bar\x05hello\x05\x00world\xff"
 * 注意因为key和value都是前缀长度为“objects”的，所以查找需要O( N) 其中 N 是 zipmap 中的元素数，而不是表示 zipmap 所需的字节数。
 * 这大大降低了恒定时间。*/

#include <stdio.h>
#include <string.h>
#include "zmalloc.h"
#include "endianconv.h"

#define ZIPMAP_BIGLEN 254
#define ZIPMAP_END 255

/* The following defines the max value for the <free> field described in the
 * comments above, that is, the max number of trailing bytes in a value. */
//下面定义了上面注释中描述的 <free> 字段的最大值，即一个值中的最大尾随字节数。
#define ZIPMAP_VALUE_MAX_FREE 4

/* The following macro returns the number of bytes needed to encode the length
 * for the integer value _l, that is, 1 byte for lengths < ZIPMAP_BIGLEN and
 * 5 bytes for all the other lengths. */
//以下宏返回对整数值 _l 的长度进行编码所需的字节数，即长度 < ZIPMAP_BIGLEN 为 1 个字节，所有其他长度为 5 个字节。
#define ZIPMAP_LEN_BYTES(_l) (((_l) < ZIPMAP_BIGLEN) ? 1 : sizeof(unsigned int)+1)

/* Create a new empty zipmap. */
unsigned char *zipmapNew(void) {
    unsigned char *zm = zmalloc(2);

    zm[0] = 0; /* Length */
    zm[1] = ZIPMAP_END;
    return zm;
}

/* Decode the encoded length pointed by 'p' */
//解码 'p' 指向的编码长度
static unsigned int zipmapDecodeLength(unsigned char *p) {
    unsigned int len = *p;

    if (len < ZIPMAP_BIGLEN) return len;
    memcpy(&len,p+1,sizeof(unsigned int));
    memrev32ifbe(&len);
    return len;
}

static unsigned int zipmapGetEncodedLengthSize(unsigned char *p) {
    return (*p < ZIPMAP_BIGLEN) ? 1: 5;
}

/* Encode the length 'l' writing it in 'p'. If p is NULL it just returns
 * the amount of bytes required to encode such a length. */
//将长度 'l' 编码为 'p'。如果 p 为 NULL，它只返回编码这种长度所需的字节数。
static unsigned int zipmapEncodeLength(unsigned char *p, unsigned int len) {
    if (p == NULL) {
        return ZIPMAP_LEN_BYTES(len);
    } else {
        if (len < ZIPMAP_BIGLEN) {
            p[0] = len;
            return 1;
        } else {
            p[0] = ZIPMAP_BIGLEN;
            memcpy(p+1,&len,sizeof(len));
            memrev32ifbe(p+1);
            return 1+sizeof(len);
        }
    }
}

/* Search for a matching key, returning a pointer to the entry inside the
 * zipmap. Returns NULL if the key is not found.
 *
 * If NULL is returned, and totlen is not NULL, it is set to the entire
 * size of the zipmap, so that the calling function will be able to
 * reallocate the original zipmap to make room for more entries. */
/**搜索匹配的键，返回指向 zipmap 中条目的指针。如果未找到密钥，则返回 NULL。
 *
 * 如果返回 NULL，并且 totlen 不为 NULL，
 * 则将其设置为 zipmap 的整个大小，以便调用函数能够重新分配原始 zipmap 为更多条目腾出空间。*/
static unsigned char *zipmapLookupRaw(unsigned char *zm, unsigned char *key, unsigned int klen, unsigned int *totlen) {
    unsigned char *p = zm+1, *k = NULL;
    unsigned int l,llen;

    while(*p != ZIPMAP_END) {
        unsigned char free;

        /* Match or skip the key */
        l = zipmapDecodeLength(p);
        llen = zipmapEncodeLength(NULL,l);
        if (key != NULL && k == NULL && l == klen && !memcmp(p+llen,key,l)) {
            /* Only return when the user doesn't care
             * for the total length of the zipmap. */
            //仅当用户不关心 zipmap 的总长度时才返回。
            if (totlen != NULL) {
                k = p;
            } else {
                return p;
            }
        }
        p += llen+l;
        /* Skip the value as well */
        l = zipmapDecodeLength(p);
        p += zipmapEncodeLength(NULL,l);
        free = p[0];
        p += l+1+free; /* +1 to skip the free byte  +1 跳过空闲字节*/
    }
    if (totlen != NULL) *totlen = (unsigned int)(p-zm)+1;
    return k;
}

static unsigned long zipmapRequiredLength(unsigned int klen, unsigned int vlen) {
    unsigned int l;

    l = klen+vlen+3;
    if (klen >= ZIPMAP_BIGLEN) l += 4;
    if (vlen >= ZIPMAP_BIGLEN) l += 4;
    return l;
}

/* Return the total amount used by a key (encoded length + payload) */
//返回一个key使用的总量（编码长度+payload）
static unsigned int zipmapRawKeyLength(unsigned char *p) {
    unsigned int l = zipmapDecodeLength(p);
    return zipmapEncodeLength(NULL,l) + l;
}

/* Return the total amount used by a value
 * (encoded length + single byte free count + payload) */
//返回一个值使用的总量（编码长度+单字节空闲计数+有效负载）
static unsigned int zipmapRawValueLength(unsigned char *p) {
    unsigned int l = zipmapDecodeLength(p);
    unsigned int used;

    used = zipmapEncodeLength(NULL,l);
    used += p[used] + 1 + l;
    return used;
}

/* If 'p' points to a key, this function returns the total amount of
 * bytes used to store this entry (entry = key + associated value + trailing
 * free space if any). */
//如果 'p' 指向一个键，则此函数返回用于存储此条目的总字节数（条目 = 键 + 关联值 + 尾随可用空间，如果有的话）。
static unsigned int zipmapRawEntryLength(unsigned char *p) {
    unsigned int l = zipmapRawKeyLength(p);
    return l + zipmapRawValueLength(p+l);
}

static inline unsigned char *zipmapResize(unsigned char *zm, unsigned int len) {
    zm = zrealloc(zm, len);
    zm[len-1] = ZIPMAP_END;
    return zm;
}

/* Set key to value, creating the key if it does not already exist.
 * If 'update' is not NULL, *update is set to 1 if the key was
 * already preset, otherwise to 0. */
//将键设置为值，如果键不存在则创建键。如果 'update' 不为 NULL，如果 key 已经预设，则 update 设置为 1，否则设置为 0。
unsigned char *zipmapSet(unsigned char *zm, unsigned char *key, unsigned int klen, unsigned char *val, unsigned int vlen, int *update) {
    unsigned int zmlen, offset;
    unsigned int freelen, reqlen = zipmapRequiredLength(klen,vlen);
    unsigned int empty, vempty;
    unsigned char *p;

    freelen = reqlen;
    if (update) *update = 0;
    p = zipmapLookupRaw(zm,key,klen,&zmlen);
    if (p == NULL) {
        /* Key not found: enlarge 未找到密钥：放大*/
        zm = zipmapResize(zm, zmlen+reqlen);
        p = zm+zmlen-1;
        zmlen = zmlen+reqlen;

        /* Increase zipmap length (this is an insert) */
        if (zm[0] < ZIPMAP_BIGLEN) zm[0]++;
    } else {
        /* Key found. Is there enough space for the new value? */
        /* Compute the total length: */
        //找到钥匙。新值是否有足够的空间？计算总长度：
        if (update) *update = 1;
        freelen = zipmapRawEntryLength(p);
        if (freelen < reqlen) {
            /* Store the offset of this key within the current zipmap, so
             * it can be resized. Then, move the tail backwards so this
             * pair fits at the current position. */
            //将此键的偏移量存储在当前 zipmap 中，以便调整大小。然后，向后移动尾巴，使这对适合当前位置。
            offset = p-zm;
            zm = zipmapResize(zm, zmlen-freelen+reqlen);
            p = zm+offset;

            /* The +1 in the number of bytes to be moved is caused by the
             * end-of-zipmap byte. Note: the *original* zmlen is used. */
            //要移动的字节数中的 +1 是由 zipmap 字节的结尾引起的。注意：使用原始的zmlen。
            memmove(p+reqlen, p+freelen, zmlen-(offset+freelen+1));
            zmlen = zmlen-freelen+reqlen;
            freelen = reqlen;
        }
    }

    /* We now have a suitable block where the key/value entry can
     * be written. If there is too much free space, move the tail
     * of the zipmap a few bytes to the front and shrink the zipmap,
     * as we want zipmaps to be very space efficient. */
    /**我们现在有一个合适的块，可以在其中写入键值条目。
     * 如果可用空间过多，请将 zipmap 的尾部移到前面几个字节并缩小 zipmap，因为我们希望 zipmap 非常节省空间。*/
    empty = freelen-reqlen;
    if (empty >= ZIPMAP_VALUE_MAX_FREE) {
        /* First, move the tail <empty> bytes to the front, then resize
         * the zipmap to be <empty> bytes smaller. */
        //首先，将尾部 <empty> 字节移动到前面，然后将 zipmap 的大小调整为小于 <empty> 字节。
        offset = p-zm;
        memmove(p+reqlen, p+freelen, zmlen-(offset+freelen+1));
        zmlen -= empty;
        zm = zipmapResize(zm, zmlen);
        p = zm+offset;
        vempty = 0;
    } else {
        vempty = empty;
    }

    /* Just write the key + value and we are done. 只需编写键+值，我们就完成了。*/
    /* Key: */
    p += zipmapEncodeLength(p,klen);
    memcpy(p,key,klen);
    p += klen;
    /* Value: */
    p += zipmapEncodeLength(p,vlen);
    *p++ = vempty;
    memcpy(p,val,vlen);
    return zm;
}

/* Remove the specified key. If 'deleted' is not NULL the pointed integer is
 * set to 0 if the key was not found, to 1 if it was found and deleted. */
//删除指定的键。如果 'deleted' 不为 NULL，则如果未找到密钥，则指向的整数设置为 0，如果找到并删除，则设置为 1。
unsigned char *zipmapDel(unsigned char *zm, unsigned char *key, unsigned int klen, int *deleted) {
    unsigned int zmlen, freelen;
    unsigned char *p = zipmapLookupRaw(zm,key,klen,&zmlen);
    if (p) {
        freelen = zipmapRawEntryLength(p);
        memmove(p, p+freelen, zmlen-((p-zm)+freelen+1));
        zm = zipmapResize(zm, zmlen-freelen);

        /* Decrease zipmap length 减少 zipmap 长度*/
        if (zm[0] < ZIPMAP_BIGLEN) zm[0]--;

        if (deleted) *deleted = 1;
    } else {
        if (deleted) *deleted = 0;
    }
    return zm;
}

/* Call before iterating through elements via zipmapNext() */
//在通过 zipmapNext() 遍历元素之前调用
unsigned char *zipmapRewind(unsigned char *zm) {
    return zm+1;
}

/* This function is used to iterate through all the zipmap elements.
 * In the first call the first argument is the pointer to the zipmap + 1.
 * In the next calls what zipmapNext returns is used as first argument.
 * 此函数用于遍历所有 zipmap 元素。在第一次调用中，第一个参数是指向 zipmap + 1 的指针。
 * 在下一次调用中，zipmapNext 返回的内容用作第一个参数。
 * Example:
 *
 * unsigned char *i = zipmapRewind(my_zipmap);
 * while((i = zipmapNext(i,&key,&klen,&value,&vlen)) != NULL) {
 *     printf("%d bytes key at $p\n", klen, key);
 *     printf("%d bytes value at $p\n", vlen, value);
 * }
 */
unsigned char *zipmapNext(unsigned char *zm, unsigned char **key, unsigned int *klen, unsigned char **value, unsigned int *vlen) {
    if (zm[0] == ZIPMAP_END) return NULL;
    if (key) {
        *key = zm;
        *klen = zipmapDecodeLength(zm);
        *key += ZIPMAP_LEN_BYTES(*klen);
    }
    zm += zipmapRawKeyLength(zm);
    if (value) {
        *value = zm+1;
        *vlen = zipmapDecodeLength(zm);
        *value += ZIPMAP_LEN_BYTES(*vlen);
    }
    zm += zipmapRawValueLength(zm);
    return zm;
}

/* Search a key and retrieve the pointer and len of the associated value.
 * If the key is found the function returns 1, otherwise 0. */
//搜索一个键并检索相关值的指针和 len。如果找到键，则函数返回 1，否则返回 0。
int zipmapGet(unsigned char *zm, unsigned char *key, unsigned int klen, unsigned char **value, unsigned int *vlen) {
    unsigned char *p;

    if ((p = zipmapLookupRaw(zm,key,klen,NULL)) == NULL) return 0;
    p += zipmapRawKeyLength(p);
    *vlen = zipmapDecodeLength(p);
    *value = p + ZIPMAP_LEN_BYTES(*vlen) + 1;
    return 1;
}

/* Return 1 if the key exists, otherwise 0 is returned. */
int zipmapExists(unsigned char *zm, unsigned char *key, unsigned int klen) {
    return zipmapLookupRaw(zm,key,klen,NULL) != NULL;
}

/* Return the number of entries inside a zipmap */
//返回 zipmap 中的条目数
unsigned int zipmapLen(unsigned char *zm) {
    unsigned int len = 0;
    if (zm[0] < ZIPMAP_BIGLEN) {
        len = zm[0];
    } else {
        unsigned char *p = zipmapRewind(zm);
        while((p = zipmapNext(p,NULL,NULL,NULL,NULL)) != NULL) len++;

        /* Re-store length if small enough 如果足够小，重新存储长度*/
        if (len < ZIPMAP_BIGLEN) zm[0] = len;
    }
    return len;
}

/* Return the raw size in bytes of a zipmap, so that we can serialize
 * the zipmap on disk (or everywhere is needed) just writing the returned
 * amount of bytes of the C array starting at the zipmap pointer. */
//返回 zipmap 的原始大小（以字节为单位），以便我们可以序列化磁盘上的 zipmap（或任何需要的地方），
// 只需从 zipmap 指针开始写入返回的 C 数组的字节数。
size_t zipmapBlobLen(unsigned char *zm) {
    unsigned int totlen;
    zipmapLookupRaw(zm,NULL,0,&totlen);
    return totlen;
}

/* Validate the integrity of the data structure.
 * when `deep` is 0, only the integrity of the header is validated.
 * when `deep` is 1, we scan all the entries one by one. */
/**验证数据结构的完整性。
 * 当 `deep` 为 0 时，仅验证标头的完整性。
 * 当 `deep` 为 1 时，我们一一扫描所有条目。*/
int zipmapValidateIntegrity(unsigned char *zm, size_t size, int deep) {
#define OUT_OF_RANGE(p) ( \
        (p) < zm + 2 || \
        (p) > zm + size - 1)
    unsigned int l, s, e;

    /* check that we can actually read the header (or ZIPMAP_END). */
    //检查我们是否可以实际读取标题（或 ZIPMAP_END）。
    if (size < 2)
        return 0;

    /* the last byte must be the terminator. */
    //最后一个字节必须是终止符。
    if (zm[size-1] != ZIPMAP_END)
        return 0;

    if (!deep)
        return 1;

    unsigned int count = 0;
    unsigned char *p = zm + 1; /* skip the count */
    while(*p != ZIPMAP_END) {
        /* read the field name length encoding type */
        //读取字段名长度编码类型
        s = zipmapGetEncodedLengthSize(p);
        /* make sure the entry length doesn't reach outside the edge of the zipmap */
        //确保条目长度没有超出 zipmap 的边缘
        if (OUT_OF_RANGE(p+s))
            return 0;

        /* read the field name length 读取字段名称长度*/
        l = zipmapDecodeLength(p);
        p += s; /* skip the encoded field size 跳过编码字段大小*/
        p += l; /* skip the field */

        /* make sure the entry doesn't reach outside the edge of the zipmap */
        //确保条目没有超出 zipmap 的边缘
        if (OUT_OF_RANGE(p))
            return 0;

        /* read the value length encoding type 读取值长度编码类型*/
        s = zipmapGetEncodedLengthSize(p);
        /* make sure the entry length doesn't reach outside the edge of the zipmap */
        /**确保条目长度没有超出 zipmap 的边缘*/
        if (OUT_OF_RANGE(p+s))
            return 0;

        /* read the value length 读取值长度*/
        l = zipmapDecodeLength(p);
        p += s; /* skip the encoded value size 跳过编码值大小*/
        e = *p++; /* skip the encoded free space (always encoded in one byte) 跳过已编码的可用空间（始终以一个字节编码）*/
        p += l+e; /* skip the value and free space 跳过值和可用空间*/
        count++;

        /* make sure the entry doesn't reach outside the edge of the zipmap */
        /**确保条目没有超出 zipmap 的边缘*/
        if (OUT_OF_RANGE(p))
            return 0;
    }

    /* check that the zipmap is not empty. 检查 zipmap 是否为空。*/
    if (count == 0) return 0;

    /* check that the count in the header is correct */
    //检查标题中的计数是否正确
    if (zm[0] != ZIPMAP_BIGLEN && zm[0] != count)
        return 0;

    return 1;
#undef OUT_OF_RANGE
}

#ifdef REDIS_TEST
static void zipmapRepr(unsigned char *p) {
    unsigned int l;

    printf("{status %u}",*p++);
    while(1) {
        if (p[0] == ZIPMAP_END) {
            printf("{end}");
            break;
        } else {
            unsigned char e;

            l = zipmapDecodeLength(p);
            printf("{key %u}",l);
            p += zipmapEncodeLength(NULL,l);
            if (l != 0 && fwrite(p,l,1,stdout) == 0) perror("fwrite");
            p += l;

            l = zipmapDecodeLength(p);
            printf("{value %u}",l);
            p += zipmapEncodeLength(NULL,l);
            e = *p++;
            if (l != 0 && fwrite(p,l,1,stdout) == 0) perror("fwrite");
            p += l+e;
            if (e) {
                printf("[");
                while(e--) printf(".");
                printf("]");
            }
        }
    }
    printf("\n");
}

#define UNUSED(x) (void)(x)
int zipmapTest(int argc, char *argv[], int flags) {
    unsigned char *zm;

    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    zm = zipmapNew();

    zm = zipmapSet(zm,(unsigned char*) "name",4, (unsigned char*) "foo",3,NULL);
    zm = zipmapSet(zm,(unsigned char*) "surname",7, (unsigned char*) "foo",3,NULL);
    zm = zipmapSet(zm,(unsigned char*) "age",3, (unsigned char*) "foo",3,NULL);
    zipmapRepr(zm);

    zm = zipmapSet(zm,(unsigned char*) "hello",5, (unsigned char*) "world!",6,NULL);
    zm = zipmapSet(zm,(unsigned char*) "foo",3, (unsigned char*) "bar",3,NULL);
    zm = zipmapSet(zm,(unsigned char*) "foo",3, (unsigned char*) "!",1,NULL);
    zipmapRepr(zm);
    zm = zipmapSet(zm,(unsigned char*) "foo",3, (unsigned char*) "12345",5,NULL);
    zipmapRepr(zm);
    zm = zipmapSet(zm,(unsigned char*) "new",3, (unsigned char*) "xx",2,NULL);
    zm = zipmapSet(zm,(unsigned char*) "noval",5, (unsigned char*) "",0,NULL);
    zipmapRepr(zm);
    zm = zipmapDel(zm,(unsigned char*) "new",3,NULL);
    zipmapRepr(zm);

    printf("\nLook up large key:\n");
    {
        unsigned char buf[512];
        unsigned char *value;
        unsigned int vlen, i;
        for (i = 0; i < 512; i++) buf[i] = 'a';

        zm = zipmapSet(zm,buf,512,(unsigned char*) "long",4,NULL);
        if (zipmapGet(zm,buf,512,&value,&vlen)) {
            printf("  <long key> is associated to the %d bytes value: %.*s\n",
                vlen, vlen, value);
        }
    }

    printf("\nPerform a direct lookup:\n");
    {
        unsigned char *value;
        unsigned int vlen;

        if (zipmapGet(zm,(unsigned char*) "foo",3,&value,&vlen)) {
            printf("  foo is associated to the %d bytes value: %.*s\n",
                vlen, vlen, value);
        }
    }
    printf("\nIterate through elements:\n");
    {
        unsigned char *i = zipmapRewind(zm);
        unsigned char *key, *value;
        unsigned int klen, vlen;

        while((i = zipmapNext(i,&key,&klen,&value,&vlen)) != NULL) {
            printf("  %d:%.*s => %d:%.*s\n", klen, klen, key, vlen, vlen, value);
        }
    }
    zfree(zm);
    return 0;
}
#endif
