/*
 * Copyright (c) 2000-2007 Marc Alexander Lehmann <schmorp@schmorp.de>
 *
 * Redistribution and use in source and binary forms, with or without modifica-
 * tion, are permitted provided that the following conditions are met:
 *
 *   1.  Redistributions of source code must retain the above copyright notice,
 *       this list of conditions and the following disclaimer.
 *
 *   2.  Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MER-
 * CHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO
 * EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPE-
 * CIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTH-
 * ERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Alternatively, the contents of this file may be used under the terms of
 * the GNU General Public License ("GPL") version 2 or any later version,
 * in which case the provisions of the GPL are applicable instead of
 * the above. If you wish to allow the use of your version of this file
 * only under the terms of the GPL and not to allow others to use your
 * version of this file under the BSD license, indicate your decision
 * by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL. If you do not delete the
 * provisions above, a recipient may use your version of this file under
 * either the BSD or the GPL.
 */

#ifndef LZFP_h
#define LZFP_h

#define STANDALONE 1 /* at the moment, this is ok. */

#ifndef STANDALONE
# include "lzf.h"
#endif

/*
 * Size of hashtable is (1 << HLOG) * sizeof (char *)
 * decompression is independent of the hash table size
 * the difference between 15 and 14 is very small
 * for small blocks (and 14 is usually a bit faster).
 * For a low-memory/faster configuration, use HLOG == 13;
 * For best compression, use 15 or 16 (or more, up to 22).
 */
/**
 * 哈希表的大小为 (1 << HLOG) sizeof (char ) 解压与哈希表大小无关，
 * 对于小块来说，15 和 14 之间的差异非常小（而 14 通常会快一点）。对于低内存更快的配置，
 * 使用 HLOG == 13;为获得最佳压缩效果，请使用 15 或 16（或更多，最多 22）。
 * */
#ifndef HLOG
# define HLOG 16
#endif

/*
 * Sacrifice very little compression quality in favour of compression speed.
 * This gives almost the same compression as the default code, and is
 * (very roughly) 15% faster. This is the preferred mode of operation.
 */
//牺牲很少的压缩质量以提高压缩速度。这提供了与默认代码几乎相同的压缩，并且（非常粗略地）快了 15%。这是首选的操作模式。
#ifndef VERY_FAST
# define VERY_FAST 1
#endif

/*
 * Sacrifice some more compression quality in favour of compression speed.
 * (roughly 1-2% worse compression for large blocks and
 * 9-10% for small, redundant, blocks and >>20% better speed in both cases)
 * In short: when in need for speed, enable this for binary data,
 * possibly disable this for text data.
 */
/**
 * 牺牲一些更多的压缩质量以支持压缩速度。
 * （在这两种情况下，大块的压缩率大约降低 1-2%，小型冗余块的压缩率降低 9-10%，速度提高 >>20%）
 * 简而言之：当需要速度时，为二进制数据启用此功能，可能禁用这是文本数据。
 * */
#ifndef ULTRA_FAST
# define ULTRA_FAST 0
#endif

/*
 * Unconditionally aligning does not cost very much, so do it if unsure
 */
//无条件对齐不会花费太多，如果不确定就这样做
#ifndef STRICT_ALIGN
# if !(defined(__i386) || defined (__amd64))
#  define STRICT_ALIGN 1
# else
#  define STRICT_ALIGN 0
# endif
#endif

/*
 * You may choose to pre-set the hash table (might be faster on some
 * modern cpus and large (>>64k) blocks, and also makes compression
 * deterministic/repeatable when the configuration otherwise is the same).
 */
//您可以选择预先设置哈希表（在某些现代 cpu 和大型 (>>64k) 块上可能更快，并且在其他配置相同时也使压缩确定性可重复）。
#ifndef INIT_HTAB
# define INIT_HTAB 0
#endif

/*
 * Avoid assigning values to errno variable? for some embedding purposes
 * (linux kernel for example), this is necessary. NOTE: this breaks
 * the documentation in lzf.h. Avoiding errno has no speed impact.
 */
//避免为 errno 变量赋值？对于某些嵌入目的（例如 linux 内核），这是必要的。
// 注意：这会破坏 lzf.h 中的文档。避免 errno 对速度没有影响。
#ifndef AVOID_ERRNO
# define AVOID_ERRNO 0
#endif

/*
 * Whether to pass the LZF_STATE variable as argument, or allocate it
 * on the stack. For small-stack environments, define this to 1.
 * NOTE: this breaks the prototype in lzf.h.
 */
/**
 * 是否将 LZF_STATE 变量作为参数传递，或者在堆栈上分配。对于小型堆栈环境，将其定义为 1。
 * 注意：这会破坏 lzf.h 中的原型。
 * */
#ifndef LZF_STATE_ARG
# define LZF_STATE_ARG 0
#endif

/*
 * Whether to add extra checks for input validity in lzf_decompress
 * and return EINVAL if the input stream has been corrupted. This
 * only shields against overflowing the input buffer and will not
 * detect most corrupted streams.
 * This check is not normally noticeable on modern hardware
 * (<1% slowdown), but might slow down older cpus considerably.
 */
/**
 * 如果输入流已损坏，是否在 lzf_decompress 中添加额外的输入有效性检查并返回 EINVAL。
 * 这只能防止输入缓冲区溢出，并且不会检测到大多数损坏的流。
 * 这种检查在现代硬件上通常不明显（<1% 减速），但可能会大大降低旧 CPU 的速度。
 * */
#ifndef CHECK_INPUT
# define CHECK_INPUT 1
#endif

/*
 * Whether to store pointers or offsets inside the hash table. On
 * 64 bit architectures, pointers take up twice as much space,
 * and might also be slower. Default is to autodetect.
 * Notice: Don't set this value to 1, it will result in 'LZF_HSLOT'
 * not being able to store offset above UINT32_MAX in 64bit. */
/**
 * 是否在哈希表中存储指针或偏移量。在 64 位架构上，指针占用的空间是原来的两倍，而且可能更慢。默认为自动检测。
 * 注意：不要将此值设置为 1，这将导致 'LZF_HSLOT' 无法在 64 位中存储高于 UINT32_MAX 的偏移量。
 * */
#define LZF_USE_OFFSETS 0

/*****************************************************************************/
/* nothing should be changed below 下面什么都不应该改变*/

#ifdef __cplusplus
# include <cstring>
# include <climits>
using namespace std;
#else
# include <string.h>
# include <limits.h>
#endif

#ifndef LZF_USE_OFFSETS
# if defined (WIN32)
#  define LZF_USE_OFFSETS defined(_M_X64)
# else
#  if __cplusplus > 199711L
#   include <cstdint>
#  else
#   include <stdint.h>
#  endif
#  define LZF_USE_OFFSETS (UINTPTR_MAX > 0xffffffffU)
# endif
#endif

typedef unsigned char u8;

#if LZF_USE_OFFSETS
# define LZF_HSLOT_BIAS ((const u8 *)in_data)
  typedef unsigned int LZF_HSLOT;
#else
# define LZF_HSLOT_BIAS 0
  typedef const u8 *LZF_HSLOT;
#endif

typedef LZF_HSLOT LZF_STATE[1 << (HLOG)];

#if !STRICT_ALIGN
/* for unaligned accesses we need a 16 bit datatype. */
//对于未对齐的访问，我们需要 16 位数据类型。
# if USHRT_MAX == 65535
    typedef unsigned short u16;
# elif UINT_MAX == 65535
    typedef unsigned int u16;
# else
#  undef STRICT_ALIGN
#  define STRICT_ALIGN 1
# endif
#endif

#if ULTRA_FAST
# undef VERY_FAST
#endif

#endif

