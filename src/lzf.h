/*
 * Copyright (c) 2000-2008 Marc Alexander Lehmann <schmorp@schmorp.de>
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

#ifndef LZF_H
#define LZF_H

/***********************************************************************
**
**	lzf -- an extremely fast/free compression/decompression-method
**	http://liblzf.plan9.de/
**
**	This algorithm is believed to be patent-free.
**
***********************************************************************/

#define LZF_VERSION 0x0105 /* 1.5, API version */

/*
 * Compress in_len bytes stored at the memory block starting at
 * in_data and write the result to out_data, up to a maximum length
 * of out_len bytes.
 *
 * If the output buffer is not large enough or any error occurs return 0,
 * otherwise return the number of bytes used, which might be considerably
 * more than in_len (but less than 104% of the original size), so it
 * makes sense to always use out_len == in_len - 1), to ensure _some_
 * compression, and store the data uncompressed otherwise (with a flag, of
 * course.
 *
 * lzf_compress might use different algorithms on different systems and
 * even different runs, thus might result in different compressed strings
 * depending on the phase of the moon or similar factors. However, all
 * these strings are architecture-independent and will result in the
 * original data when decompressed using lzf_decompress.
 *
 * The buffers must not be overlapping.
 *
 * If the option LZF_STATE_ARG is enabled, an extra argument must be
 * supplied which is not reflected in this header file. Refer to lzfP.h
 * and lzf_c.c.
 *
 */
/**
 * 压缩存储在从 in_data 开始的内存块中的 in_len 字节，并将结果写入 out_data，最大长度为 out_len 字节。
 * 如果输出缓冲区不够大或发生任何错误，则返回 0，否则返回使用的字节数，这可能比 in_len 多得多（但小于原始大小的 104%），
 * 因此始终使用 out_len 是有意义的== in_len - 1)，以确保 _some_ 压缩，
 * 并存储未压缩的数据（当然，带有标志。lzf_compress 可能在不同的系统甚至不同的运行上使用不同的算法，
 * 因此可能会根据阶段产生不同的压缩字符串月亮或类似因素。
 * 但是，所有这些字符串都是架构独立的，并且在使用 lzf_decompress 解压缩时会产生原始数据。
 * 缓冲区不能重叠。如果启用了选项 LZF_STATE_ARG，则必须提供额外的参数，即没有体现在这个头文件中，参考lzfP.h和lzf_c.c。
 * */
size_t
lzf_compress (const void *const in_data,  size_t in_len,
              void             *out_data, size_t out_len);

/*
 * Decompress data compressed with some version of the lzf_compress
 * function and stored at location in_data and length in_len. The result
 * will be stored at out_data up to a maximum of out_len characters.
 *
 * If the output buffer is not large enough to hold the decompressed
 * data, a 0 is returned and errno is set to E2BIG. Otherwise the number
 * of decompressed bytes (i.e. the original length of the data) is
 * returned.
 *
 * If an error in the compressed data is detected, a zero is returned and
 * errno is set to EINVAL.
 *
 * This function is very fast, about as fast as a copying loop.
 */
/**
 * 解压缩使用某些版本的 lzf_compress 函数压缩并存储在位置 in_data 和长度 in_len 的数据。
 * 结果将存储在 out_data 中，最多为 out_len 个字符。如果输出缓冲区不足以容纳解压缩的数据，则返回 0 并将 errno 设置为 E2BIG。
 * 否则返回解压缩的字节数（即数据的原始长度）。如果在压缩数据中检测到错误，则返回零并将 errno 设置为 EINVAL。
 * 这个功能非常快，大约和复制循环一样快。
 * */
size_t
lzf_decompress (const void *const in_data,  size_t in_len,
                void             *out_data, size_t out_len);

#endif

