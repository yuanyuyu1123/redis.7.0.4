/* hyperloglog.c - Redis HyperLogLog probabilistic cardinality approximation.
 * This file implements the algorithm and the exported Redis commands.
 *
 * Copyright (c) 2014, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include <stdint.h>
#include <math.h>

/* The Redis HyperLogLog implementation is based on the following ideas:
 *
 * * The use of a 64 bit hash function as proposed in [1], in order to estimate
 *   cardinalities larger than 10^9, at the cost of just 1 additional bit per
 *   register.
 * * The use of 16384 6-bit registers for a great level of accuracy, using
 *   a total of 12k per key.
 * * The use of the Redis string data type. No new type is introduced.
 * * No attempt is made to compress the data structure as in [1]. Also the
 *   algorithm used is the original HyperLogLog Algorithm as in [2], with
 *   the only difference that a 64 bit hash function is used, so no correction
 *   is performed for values near 2^32 as in [1].
 *
 * [1] Heule, Nunkesser, Hall: HyperLogLog in Practice: Algorithmic
 *     Engineering of a State of The Art Cardinality Estimation Algorithm.
 *
 * [2] P. Flajolet, Éric Fusy, O. Gandouet, and F. Meunier. Hyperloglog: The
 *     analysis of a near-optimal cardinality estimation algorithm.
 *
 * Redis uses two representations:
 *
 * 1) A "dense" representation where every entry is represented by
 *    a 6-bit integer.
 * 2) A "sparse" representation using run length compression suitable
 *    for representing HyperLogLogs with many registers set to 0 in
 *    a memory efficient way.
 *
 *
 * HLL header
 * ===
 *
 * Both the dense and sparse representation have a 16 byte header as follows:
 *
 * +------+---+-----+----------+
 * | HYLL | E | N/U | Cardin.  |
 * +------+---+-----+----------+
 *
 * The first 4 bytes are a magic string set to the bytes "HYLL".
 * "E" is one byte encoding, currently set to HLL_DENSE or
 * HLL_SPARSE. N/U are three not used bytes.
 *
 * The "Cardin." field is a 64 bit integer stored in little endian format
 * with the latest cardinality computed that can be reused if the data
 * structure was not modified since the last computation (this is useful
 * because there are high probabilities that HLLADD operations don't
 * modify the actual data structure and hence the approximated cardinality).
 *
 * When the most significant bit in the most significant byte of the cached
 * cardinality is set, it means that the data structure was modified and
 * we can't reuse the cached value that must be recomputed.
 *
 * Dense representation
 * ===
 *
 * The dense representation used by Redis is the following:
 *
 * +--------+--------+--------+------//      //--+
 * |11000000|22221111|33333322|55444444 ....     |
 * +--------+--------+--------+------//      //--+
 *
 * The 6 bits counters are encoded one after the other starting from the
 * LSB to the MSB, and using the next bytes as needed.
 *
 * Sparse representation
 * ===
 *
 * The sparse representation encodes registers using a run length
 * encoding composed of three opcodes, two using one byte, and one using
 * of two bytes. The opcodes are called ZERO, XZERO and VAL.
 *
 * ZERO opcode is represented as 00xxxxxx. The 6-bit integer represented
 * by the six bits 'xxxxxx', plus 1, means that there are N registers set
 * to 0. This opcode can represent from 1 to 64 contiguous registers set
 * to the value of 0.
 *
 * XZERO opcode is represented by two bytes 01xxxxxx yyyyyyyy. The 14-bit
 * integer represented by the bits 'xxxxxx' as most significant bits and
 * 'yyyyyyyy' as least significant bits, plus 1, means that there are N
 * registers set to 0. This opcode can represent from 0 to 16384 contiguous
 * registers set to the value of 0.
 *
 * VAL opcode is represented as 1vvvvvxx. It contains a 5-bit integer
 * representing the value of a register, and a 2-bit integer representing
 * the number of contiguous registers set to that value 'vvvvv'.
 * To obtain the value and run length, the integers vvvvv and xx must be
 * incremented by one. This opcode can represent values from 1 to 32,
 * repeated from 1 to 4 times.
 *
 * The sparse representation can't represent registers with a value greater
 * than 32, however it is very unlikely that we find such a register in an
 * HLL with a cardinality where the sparse representation is still more
 * memory efficient than the dense representation. When this happens the
 * HLL is converted to the dense representation.
 *
 * The sparse representation is purely positional. For example a sparse
 * representation of an empty HLL is just: XZERO:16384.
 *
 * An HLL having only 3 non-zero registers at position 1000, 1020, 1021
 * respectively set to 2, 3, 3, is represented by the following three
 * opcodes:
 *
 * XZERO:1000 (Registers 0-999 are set to 0)
 * VAL:2,1    (1 register set to value 2, that is register 1000)
 * ZERO:19    (Registers 1001-1019 set to 0)
 * VAL:3,2    (2 registers set to value 3, that is registers 1020,1021)
 * XZERO:15362 (Registers 1022-16383 set to 0)
 *
 * In the example the sparse representation used just 7 bytes instead
 * of 12k in order to represent the HLL registers. In general for low
 * cardinality there is a big win in terms of space efficiency, traded
 * with CPU time since the sparse representation is slower to access:
 *
 * The following table shows average cardinality vs bytes used, 100
 * samples per cardinality (when the set was not representable because
 * of registers with too big value, the dense representation size was used
 * as a sample).
 *
 * 100 267
 * 200 485
 * 300 678
 * 400 859
 * 500 1033
 * 600 1205
 * 700 1375
 * 800 1544
 * 900 1713
 * 1000 1882
 * 2000 3480
 * 3000 4879
 * 4000 6089
 * 5000 7138
 * 6000 8042
 * 7000 8823
 * 8000 9500
 * 9000 10088
 * 10000 10591
 *
 * The dense representation uses 12288 bytes, so there is a big win up to
 * a cardinality of ~2000-3000. For bigger cardinalities the constant times
 * involved in updating the sparse representation is not justified by the
 * memory savings. The exact maximum length of the sparse representation
 * when this implementation switches to the dense representation is
 * configured via the define server.hll_sparse_max_bytes.
 */
/**
 * Redis HyperLogLog 实现基于以下思想： 使用 [1] 中提出的 64 位散列函数，
 * 以估计大于 10^9 的基数，代价是每个寄存器仅增加 1 位。
 * 使用 16384 个 6 位寄存器以实现高准确度，每个键总共使用 12k。
 * Redis 字符串数据类型的使用。没有引入新类型。没有尝试像 [1] 中那样压缩数据结构。
 * 此外，所使用的算法是 [2] 中的原始 HyperLogLog 算法，
 * 唯一的区别是使用了 64 位散列函数，因此不会像 [1] 中那样对 2^32 附近的值执行更正。
 * [1] Heule, Nunkesser, Hall：HyperLogLog 实践：最先进的基数估计算法的算法工程。
 * [2] P. Flajolet、Éric Fusy、O. Gandouet 和 F. Meunier。 Hyperloglog：一种近乎最优的基数估计算法的分析。
 * Redis 使用两种表示形式：
 * 1）“密集”表示，其中每个条目都由一个 6 位整数表示。
 * 2) 使用运行长度压缩的“稀疏”表示，适用于以内存有效的方式表示具有许多寄存器设置为 0 的 HyperLogLog。
 * HLL 头 === 密集和稀疏表示都有一个 16 字节的头，如下所示：
 * +------+---+-----+----------+
 * | HYLL | E | N/U | Cardin.  |
 * +------+---+-----+----------+
 * 前 4 个字节是设置为字节“HYLL”的魔术字符串。
 * “E”是一字节编码，当前设置为 HLL_DENSE 或 HLL_SPARSE。 NU 是三个未使用的字节。
 * “卡丁”。字段是以小端格式存储的 64 位整数，如果自上次计算后未修改数据结构，
 * 则可以重用计算的最新基数（这很有用，因为 HLLADD 操作很可能不会修改实际数据结构和因此近似的基数）。
 * 当缓存基数的最高有效字节中的最高位被设置时，意味着数据结构被修改了，
 * 我们不能重用必须重新计算的缓存值。
 * 密集表示=== Redis使用的密集表示如下：
 * +--------+--------+--------+------//      //--+
 * |11000000|22221111|33333322|55444444 ....     |
 * +--------+--------+--------+------//      //--+
 * 6 位计数器从LSB 到 MSB，并根据需要使用下一个字节。
 * 稀疏表示 === 稀疏表示使用由三个操作码组成的运行长度编码对寄存器进行编码，两个使用一个字节，
 * 一个使用两个字节。操作码称为 ZERO、XZERO 和 VAL。
 * 零操作码表示为 00xxxxxx。 6位‘xxxxxx’表示的6位整数加1，表示有N个寄存器设置为0。
 * 这个操作码可以表示从1到64个设置为0值的连续寄存器。XZERO操作码用两个表示字节 01xxxxxx yyyyyyyy。
 * 由位'xxxxxx'作为最高有效位和'yyyyyyyy'作为最低有效位表示的14位整数加上1，意味着有N个寄存器设置为0。
 * 这个操作码可以表示从0到16384个连续寄存器设置为值为 0。
 * VAL 操作码表示为 1vvvvvxx。
 * 它包含一个表示寄存器值的 5 位整数和一个表示设置为该值“vvvvv”的连续寄存器数量的 2 位整数。
 * 要获得值和运行长度，整数 vvvvv 和 xx 必须加 1。
 * 此操作码可以表示从 1 到 32 的值，重复 1 到 4 次。
 * 稀疏表示不能表示值大于 32 的寄存器，但是我们不太可能在具有基数的 HLL 中找到这样的寄存器，
 * 其中稀疏表示仍然比密集表示更节省内存。发生这种情况时，HLL 将转换为密集表示。
 * 稀疏表示纯粹是位置的。例如，空 HLL 的稀疏表示就是：
 * XZERO:16384。
 * 在位置 1000、1020、1021 处只有 3 个非零寄存器分别设置为 2、3、3 的 HLL 由以下三个操作码表示：
 * XZERO:1000（寄存器 0-999 设置为 0）VAL:2， 1（1 个寄存器设置为值 2，即寄存器 1000）
 * ZERO:19（寄存器 1001-1019 设置为 0） VAL:3,2（2 个寄存器设置为值 3，即寄存器 1020,1021）
 * XZERO:15362（寄存器 1022-16383 设置为 0）
 * 在示例中，稀疏表示仅使用 7 个字节而不是 12k 来表示 HLL 寄存器。
 * 一般来说，对于低基数，在空间效率方面有很大的优势，用 CPU 时间进行交易，因为稀疏表示访问速度较慢：
 * 下表显示了平均基数与使用的字节数，每个基数 100 个样本（当集合不可表示时由于寄存器的值太大，
 * 因此使用密集表示大小作为样本）。
 * 100 267 200 485 300 678 400 859 500 1033 600 1205 700 1375 800 1544 900 1713 1000 1882 2000 3480 3000 4879 4000 6089 5000 7138 6000 8042 7000 8823 8000 9500 9000 10088 10000 10591
 * 密集表示使用 12288 字节，所以有一个很大的赢得高达〜2000-3000的基数。
 * 对于更大的基数，更新稀疏表示所涉及的恒定时间并不能通过内存节省来证明。
 * 当此实现切换到密集表示时，稀疏表示的确切最大长度是通过定义 server.hll_sparse_max_bytes 配置的。
 * */
struct hllhdr {
    char magic[4];      /* "HYLL" “丑陋的”*/
    uint8_t encoding;   /* HLL_DENSE or HLL_SPARSE. HLL_DENSE 或 HLL_SPARSE。*/
    uint8_t notused[3]; /* Reserved for future use, must be zero. 保留供将来使用，必须为零。*/
    uint8_t card[8];    /* Cached cardinality, little endian. 缓存基数，小端。*/
    uint8_t registers[]; /* Data bytes. 数据字节。*/
};

/* The cached cardinality MSB is used to signal validity of the cached value. */
//缓存基数 MSB 用于表示缓存值的有效性。
#define HLL_INVALIDATE_CACHE(hdr) (hdr)->card[7] |= (1<<7)
#define HLL_VALID_CACHE(hdr) (((hdr)->card[7] & (1<<7)) == 0)

#define HLL_P 14 /* The greater is P, the smaller the error. P越大，误差越小。*/
#define HLL_Q (64-HLL_P) /* The number of bits of the hash value used for
                            determining the number of leading zeros. 用于确定前导零数量的哈希值的位数。*/
#define HLL_REGISTERS (1<<HLL_P) /* With P=14, 16384 registers. 当 P=14 时，有 16384 个寄存器。*/
#define HLL_P_MASK (HLL_REGISTERS-1) /* Mask to index register. 对索引寄存器的掩码。*/
#define HLL_BITS 6 /* Enough to count up to 63 leading zeroes.足以计算多达 63 个前导零。 */
#define HLL_REGISTER_MAX ((1<<HLL_BITS)-1)
#define HLL_HDR_SIZE sizeof(struct hllhdr)
#define HLL_DENSE_SIZE (HLL_HDR_SIZE+((HLL_REGISTERS*HLL_BITS+7)/8))
#define HLL_DENSE 0 /* Dense encoding.密集编码。 */
#define HLL_SPARSE 1 /* Sparse encoding. 稀疏编码。*/
#define HLL_RAW 255 /* Only used internally, never exposed.只在内部使用，从不暴露。 */
#define HLL_MAX_ENCODING 1

static char *invalid_hll_err = "-INVALIDOBJ Corrupted HLL object detected";

/* =========================== Low level bit macros ========================= */

/* Macros to access the dense representation.
 *
 * We need to get and set 6 bit counters in an array of 8 bit bytes.
 * We use macros to make sure the code is inlined since speed is critical
 * especially in order to compute the approximated cardinality in
 * HLLCOUNT where we need to access all the registers at once.
 * For the same reason we also want to avoid conditionals in this code path.
 *
 * +--------+--------+--------+------//
 * |11000000|22221111|33333322|55444444
 * +--------+--------+--------+------//
 *
 * Note: in the above representation the most significant bit (MSB)
 * of every byte is on the left. We start using bits from the LSB to MSB,
 * and so forth passing to the next byte.
 *
 * Example, we want to access to counter at pos = 1 ("111111" in the
 * illustration above).
 *
 * The index of the first byte b0 containing our data is:
 *
 *  b0 = 6 * pos / 8 = 0
 *
 *   +--------+
 *   |11000000|  <- Our byte at b0
 *   +--------+
 *
 * The position of the first bit (counting from the LSB = 0) in the byte
 * is given by:
 *
 *  fb = 6 * pos % 8 -> 6
 *
 * Right shift b0 of 'fb' bits.
 *
 *   +--------+
 *   |11000000|  <- Initial value of b0
 *   |00000011|  <- After right shift of 6 pos.
 *   +--------+
 *
 * Left shift b1 of bits 8-fb bits (2 bits)
 *
 *   +--------+
 *   |22221111|  <- Initial value of b1
 *   |22111100|  <- After left shift of 2 bits.
 *   +--------+
 *
 * OR the two bits, and finally AND with 111111 (63 in decimal) to
 * clean the higher order bits we are not interested in:
 *
 *   +--------+
 *   |00000011|  <- b0 right shifted
 *   |22111100|  <- b1 left shifted
 *   |22111111|  <- b0 OR b1
 *   |  111111|  <- (b0 OR b1) AND 63, our value.
 *   +--------+
 *
 * We can try with a different example, like pos = 0. In this case
 * the 6-bit counter is actually contained in a single byte.
 *
 *  b0 = 6 * pos / 8 = 0
 *
 *   +--------+
 *   |11000000|  <- Our byte at b0
 *   +--------+
 *
 *  fb = 6 * pos % 8 = 0
 *
 *  So we right shift of 0 bits (no shift in practice) and
 *  left shift the next byte of 8 bits, even if we don't use it,
 *  but this has the effect of clearing the bits so the result
 *  will not be affected after the OR.
 *
 * -------------------------------------------------------------------------
 *
 * Setting the register is a bit more complex, let's assume that 'val'
 * is the value we want to set, already in the right range.
 *
 * We need two steps, in one we need to clear the bits, and in the other
 * we need to bitwise-OR the new bits.
 *
 * Let's try with 'pos' = 1, so our first byte at 'b' is 0,
 *
 * "fb" is 6 in this case.
 *
 *   +--------+
 *   |11000000|  <- Our byte at b0
 *   +--------+
 *
 * To create an AND-mask to clear the bits about this position, we just
 * initialize the mask with the value 63, left shift it of "fs" bits,
 * and finally invert the result.
 *
 *   +--------+
 *   |00111111|  <- "mask" starts at 63
 *   |11000000|  <- "mask" after left shift of "ls" bits.
 *   |00111111|  <- "mask" after invert.
 *   +--------+
 *
 * Now we can bitwise-AND the byte at "b" with the mask, and bitwise-OR
 * it with "val" left-shifted of "ls" bits to set the new bits.
 *
 * Now let's focus on the next byte b1:
 *
 *   +--------+
 *   |22221111|  <- Initial value of b1
 *   +--------+
 *
 * To build the AND mask we start again with the 63 value, right shift
 * it by 8-fb bits, and invert it.
 *
 *   +--------+
 *   |00111111|  <- "mask" set at 2&6-1
 *   |00001111|  <- "mask" after the right shift by 8-fb = 2 bits
 *   |11110000|  <- "mask" after bitwise not.
 *   +--------+
 *
 * Now we can mask it with b+1 to clear the old bits, and bitwise-OR
 * with "val" left-shifted by "rs" bits to set the new value.
 */

/* Note: if we access the last counter, we will also access the b+1 byte
 * that is out of the array, but sds strings always have an implicit null
 * term, so the byte exists, and we can skip the conditional (or the need
 * to allocate 1 byte more explicitly). */

/* Store the value of the register at position 'regnum' into variable 'target'.
 * 'p' is an array of unsigned bytes. */
/**
 * ============================ 低级位宏 =================== ======
 * 访问密集表示的宏。
 * 我们需要在 8 位字节数组中获取和设置 6 位计数器。
 * 我们使用宏来确保代码是内联的，因为速度至关重要，尤其是为了计算 HLLCOUNT 中的近似基数，
 * 我们需要一次访问所有寄存器。出于同样的原因，我们还希望在此代码路径中避免使用条件。
 *  * +--------+--------+--------+------//
 * |11000000|22221111|33333322|55444444
 * +--------+--------+--------+------//
 * 注意：在上述表示中，每个字节的最高有效位 (MSB) 位于左侧。
 * 我们开始使用从 LSB 到 MSB 的位，以此类推传递到下一个字节。
 * 例如，我们想要访问 pos = 1 处的计数器（上图中的“111111”）。
 * 包含我们数据的第一个字节 b0 的索引是：
 *  *  b0 = 6 * pos / 8 = 0
 *
 *   +--------+
 *   |11000000|  <- 我们在 b0 的字节
 *   +--------+
 *   字节中第一位（从 LSB = 0 开始计数）的位置由下式给出：
 *    *  fb = 6 * pos % 8 -> 6
 *
 * 将“fb”位的 b0 右移。
 *
 *   +--------+
 *   |11000000|  <- b0 的初始值
 *   |00000011|  <- 右移 6 位后。
 *   +--------+
 *  b1 位左移 8-fb 位（2 位）
 *  *   +--------+
 *   |22221111|  <- Initial value of b1
 *   |22111100|  <- After left shift of 2 bits.
 *   +--------+
 * 或这两个位，最后与 111111（十进制的 63）与以清除我们不感兴趣的高阶位：
 *
 *  *   +--------+
 *   |00000011|  <- b0 right shifted
 *   |22111100|  <- b1 left shifted
 *   |22111111|  <- b0 OR b1
 *   |  111111|  <- (b0 OR b1) AND 63, our value.
 *   +--------+
 *
 *   我们可以尝试不同的示例，例如 pos = 0。在这种情况下，6 位计数器实际上包含在单个字节中。
 *    *  b0 = 6 * pos / 8 = 0
 *   +--------+
 *   |11000000|  <- Our byte at b0
 *   +--------+
 *  fb = 6 * pos % 8 = 0
 *
 *  所以我们右移 0 位（实际上没有移位）并左移 8 位的下一个字节，即使我们不使用它，
 *  但这具有清除位的效果，因此结果不会受到影响或者。
 *  -------------------------------------------------- -----------------------
 *  设置寄存器有点复杂，假设'val'是我们要设置的值，已经在右边范围。
 *  我们需要两个步骤，一个是我们需要清除这些位，另一个是我们需要对新位进行按位或。
 *  让我们尝试使用 'pos' = 1，所以我们在 'b' 处的第一个字节是 0，在这种情况下，“fb”是 6。
 *  *   +--------+
 *   |11000000|  <- Our byte at b0
 *   +--------+
 *
 *   要创建一个 AND 掩码来清除有关该位置的位，我们只需使用值 63 初始化掩码，将其左移“fs”位，最后反转结果。
 *   *   +--------+
 *   |00111111|  <- "mask" starts at 63
 *   |11000000|  <- "mask" after left shift of "ls" bits.
 *   |00111111|  <- "mask" after invert.
 *   +--------+
 *   现在我们可以将“b”处的字节与掩码进行按位与运算，并将“val”与“ls”位左移以设置新位。
 *   现在让我们关注下一个字节 b1：
 *    *   +--------+
 *   |22221111|  <- Initial value of b1
 *   +--------+
 *   为了构建 AND 掩码，我们再次从 63 值开始，将其右移 8-fb 位，然后将其反转。
 *    *   +--------+
 *   |00111111|  <- "mask" set at 2&6-1
 *   |00001111|  <- "mask" after the right shift by 8-fb = 2 bits
 *   |11110000|  <- "mask" after bitwise not.
 *   +--------+
 *   现在我们可以用 b+1 屏蔽它以清除旧位，并与“val”按位或左移“rs”位来设置新值。
 *   注意：如果我们访问最后一个计数器，我们也会访问到数组外的b+1字节，
 *   但是sds字符串总是有一个隐含的空项，所以这个字节是存在的，
 *   我们可以跳过条件（或需要更明确地分配 1 个字节）。
 *   将寄存器'regnum'位置的值存储到变量'target'中。 'p' 是一个无符号字节数组。
 * */

#define HLL_DENSE_GET_REGISTER(target,p,regnum) do { \
    uint8_t *_p = (uint8_t*) p; \
    unsigned long _byte = regnum*HLL_BITS/8; \
    unsigned long _fb = regnum*HLL_BITS&7; \
    unsigned long _fb8 = 8 - _fb; \
    unsigned long b0 = _p[_byte]; \
    unsigned long b1 = _p[_byte+1]; \
    target = ((b0 >> _fb) | (b1 << _fb8)) & HLL_REGISTER_MAX; \
} while(0)

/* Set the value of the register at position 'regnum' to 'val'.
 * 'p' is an array of unsigned bytes. */
//将位置“regnum”的寄存器的值设置为“val”。 'p' 是一个无符号字节数组。
#define HLL_DENSE_SET_REGISTER(p,regnum,val) do { \
    uint8_t *_p = (uint8_t*) p; \
    unsigned long _byte = regnum*HLL_BITS/8; \
    unsigned long _fb = regnum*HLL_BITS&7; \
    unsigned long _fb8 = 8 - _fb; \
    unsigned long _v = val; \
    _p[_byte] &= ~(HLL_REGISTER_MAX << _fb); \
    _p[_byte] |= _v << _fb; \
    _p[_byte+1] &= ~(HLL_REGISTER_MAX >> _fb8); \
    _p[_byte+1] |= _v >> _fb8; \
} while(0)

/* Macros to access the sparse representation.
 * The macros parameter is expected to be an uint8_t pointer. */
//用于访问稀疏表示的宏。宏参数应该是一个 uint8_t 指针。
#define HLL_SPARSE_XZERO_BIT 0x40 /* 01xxxxxx */
#define HLL_SPARSE_VAL_BIT 0x80 /* 1vvvvvxx */
#define HLL_SPARSE_IS_ZERO(p) (((*(p)) & 0xc0) == 0) /* 00xxxxxx */
#define HLL_SPARSE_IS_XZERO(p) (((*(p)) & 0xc0) == HLL_SPARSE_XZERO_BIT)
#define HLL_SPARSE_IS_VAL(p) ((*(p)) & HLL_SPARSE_VAL_BIT)
#define HLL_SPARSE_ZERO_LEN(p) (((*(p)) & 0x3f)+1)
#define HLL_SPARSE_XZERO_LEN(p) (((((*(p)) & 0x3f) << 8) | (*((p)+1)))+1)
#define HLL_SPARSE_VAL_VALUE(p) ((((*(p)) >> 2) & 0x1f)+1)
#define HLL_SPARSE_VAL_LEN(p) (((*(p)) & 0x3)+1)
#define HLL_SPARSE_VAL_MAX_VALUE 32
#define HLL_SPARSE_VAL_MAX_LEN 4
#define HLL_SPARSE_ZERO_MAX_LEN 64
#define HLL_SPARSE_XZERO_MAX_LEN 16384
#define HLL_SPARSE_VAL_SET(p,val,len) do { \
    *(p) = (((val)-1)<<2|((len)-1))|HLL_SPARSE_VAL_BIT; \
} while(0)
#define HLL_SPARSE_ZERO_SET(p,len) do { \
    *(p) = (len)-1; \
} while(0)
#define HLL_SPARSE_XZERO_SET(p,len) do { \
    int _l = (len)-1; \
    *(p) = (_l>>8) | HLL_SPARSE_XZERO_BIT; \
    *((p)+1) = (_l&0xff); \
} while(0)
#define HLL_ALPHA_INF 0.721347520444481703680 /* constant for 0.5/ln(2) */

/* ========================= HyperLogLog algorithm  ========================= */

/* Our hash function is MurmurHash2, 64 bit version.
 * It was modified for Redis in order to provide the same result in
 * big and little endian archs (endian neutral). */
/**
 * 我们的哈希函数是 MurmurHash2，64 位版本。
 * 它针对 Redis 进行了修改，以便在大端和小端拱（端中性）中提供相同的结果。*/
REDIS_NO_SANITIZE("alignment")
uint64_t MurmurHash64A (const void * key, int len, unsigned int seed) {
    const uint64_t m = 0xc6a4a7935bd1e995;
    const int r = 47;
    uint64_t h = seed ^ (len * m);
    const uint8_t *data = (const uint8_t *)key;
    const uint8_t *end = data + (len-(len&7));

    while(data != end) {
        uint64_t k;

#if (BYTE_ORDER == LITTLE_ENDIAN)
    #ifdef USE_ALIGNED_ACCESS
        memcpy(&k,data,sizeof(uint64_t));
    #else
        k = *((uint64_t*)data);
    #endif
#else
        k = (uint64_t) data[0];
        k |= (uint64_t) data[1] << 8;
        k |= (uint64_t) data[2] << 16;
        k |= (uint64_t) data[3] << 24;
        k |= (uint64_t) data[4] << 32;
        k |= (uint64_t) data[5] << 40;
        k |= (uint64_t) data[6] << 48;
        k |= (uint64_t) data[7] << 56;
#endif

        k *= m;
        k ^= k >> r;
        k *= m;
        h ^= k;
        h *= m;
        data += 8;
    }

    switch(len & 7) {
    case 7: h ^= (uint64_t)data[6] << 48; /* fall-thru */
    case 6: h ^= (uint64_t)data[5] << 40; /* fall-thru */
    case 5: h ^= (uint64_t)data[4] << 32; /* fall-thru */
    case 4: h ^= (uint64_t)data[3] << 24; /* fall-thru */
    case 3: h ^= (uint64_t)data[2] << 16; /* fall-thru */
    case 2: h ^= (uint64_t)data[1] << 8; /* fall-thru */
    case 1: h ^= (uint64_t)data[0];
            h *= m; /* fall-thru */
    };

    h ^= h >> r;
    h *= m;
    h ^= h >> r;
    return h;
}

/* Given a string element to add to the HyperLogLog, returns the length
 * of the pattern 000..1 of the element hash. As a side effect 'regp' is
 * set to the register index this element hashes to. */
/**
 * 给定要添加到 HyperLogLog 的字符串元素，返回元素哈希的模式 000..1 的长度。
 * 作为副作用，“regp”被设置为该元素散列到的寄存器索引。
 * */
int hllPatLen(unsigned char *ele, size_t elesize, long *regp) {
    uint64_t hash, bit, index;
    int count;

    /* Count the number of zeroes starting from bit HLL_REGISTERS
     * (that is a power of two corresponding to the first bit we don't use
     * as index). The max run can be 64-P+1 = Q+1 bits.
     *
     * Note that the final "1" ending the sequence of zeroes must be
     * included in the count, so if we find "001" the count is 3, and
     * the smallest count possible is no zeroes at all, just a 1 bit
     * at the first position, that is a count of 1.
     *
     * This may sound like inefficient, but actually in the average case
     * there are high probabilities to find a 1 after a few iterations. */
    /**
     * 从位 HLL_REGISTERS 开始计算零的数量（这是对应于我们不用作索引的第一位的 2 的幂）。
     * 最大运行可以是 64-P+1 = Q+1 位。请注意，结束零序列的最后“1”必须包含在计数中，
     * 因此如果我们找到“001”，则计数为 3，并且可能的最小计数根本没有零，只有第一个位置的 1 位，
     * 即计数为 1。这听起来可能效率低下，但实际上在平均情况下，在几次迭代后找到 1 的概率很高。
     * */
    hash = MurmurHash64A(ele,elesize,0xadc83b19ULL);
    index = hash & HLL_P_MASK; /* Register index. */
    hash >>= HLL_P; /* Remove bits used to address the register. 删除用于寻址寄存器的位。*/
    hash |= ((uint64_t)1<<HLL_Q); /* Make sure the loop terminates
                                     and count will be <= Q+1. 确保循环终止并且计数将 <= Q+1。*/
    bit = 1;
    count = 1; /* Initialized to 1 since we count the "00000...1" pattern.
               初始化为 1，因为我们计算了“00000...1”模式。 */
    while((hash & bit) == 0) {
        count++;
        bit <<= 1;
    }
    *regp = (int) index;
    return count;
}

/* ================== Dense representation implementation  ================== */

/* Low level function to set the dense HLL register at 'index' to the
 * specified value if the current value is smaller than 'count'.
 *
 * 'registers' is expected to have room for HLL_REGISTERS plus an
 * additional byte on the right. This requirement is met by sds strings
 * automatically since they are implicitly null terminated.
 *
 * The function always succeed, however if as a result of the operation
 * the approximated cardinality changed, 1 is returned. Otherwise 0
 * is returned. */
/**
 * ================== 密集表示实现 ===================
 * 将密集 HLL 寄存器设置为 ' 的低级函数如果当前值小于 'count'，则 index' 到指定的值。
 * 'registers' 应该有空间容纳 HLL_REGISTERS 加上右边的一个额外字节。
 * sds 字符串会自动满足此要求，因为它们是隐式以空值终止的。
 * 该函数总是成功的，但是如果作为操作的结果近似基数改变了，则返回 1。否则返回 0。
 * */
int hllDenseSet(uint8_t *registers, long index, uint8_t count) {
    uint8_t oldcount;

    HLL_DENSE_GET_REGISTER(oldcount,registers,index);
    if (count > oldcount) {
        HLL_DENSE_SET_REGISTER(registers,index,count);
        return 1;
    } else {
        return 0;
    }
}

/* "Add" the element in the dense hyperloglog data structure.
 * Actually nothing is added, but the max 0 pattern counter of the subset
 * the element belongs to is incremented if needed.
 *
 * This is just a wrapper to hllDenseSet(), performing the hashing of the
 * element in order to retrieve the index and zero-run count. */
/**
 * “添加”密集超日志数据结构中的元素。
 * 实际上没有添加任何内容，但如果需要，元素所属子集的最大 0 模式计数器会增加。
 * 这只是 hllDenseSet() 的包装器，执行元素的散列以检索索引和零运行计数。
 * */
int hllDenseAdd(uint8_t *registers, unsigned char *ele, size_t elesize) {
    long index;
    uint8_t count = hllPatLen(ele,elesize,&index);
    /* Update the register if this element produced a longer run of zeroes. */
    //如果此元素产生更长的零运行，则更新寄存器。
    return hllDenseSet(registers,index,count);
}

/* Compute the register histogram in the dense representation. */
//计算密集表示中的寄存器直方图。
void hllDenseRegHisto(uint8_t *registers, int* reghisto) {
    int j;

    /* Redis default is to use 16384 registers 6 bits each. The code works
     * with other values by modifying the defines, but for our target value
     * we take a faster path with unrolled loops. */
    /**
     * Redis 默认使用 16384 个寄存器，每个寄存器 6 位。
     * 该代码通过修改定义与其他值一起使用，但对于我们的目标值，我们采用展开循环的更快路径。
     * */
    if (HLL_REGISTERS == 16384 && HLL_BITS == 6) {
        uint8_t *r = registers;
        unsigned long r0, r1, r2, r3, r4, r5, r6, r7, r8, r9,
                      r10, r11, r12, r13, r14, r15;
        for (j = 0; j < 1024; j++) {
            /* Handle 16 registers per iteration. 每次迭代处理 16 个寄存器。*/
            r0 = r[0] & 63;
            r1 = (r[0] >> 6 | r[1] << 2) & 63;
            r2 = (r[1] >> 4 | r[2] << 4) & 63;
            r3 = (r[2] >> 2) & 63;
            r4 = r[3] & 63;
            r5 = (r[3] >> 6 | r[4] << 2) & 63;
            r6 = (r[4] >> 4 | r[5] << 4) & 63;
            r7 = (r[5] >> 2) & 63;
            r8 = r[6] & 63;
            r9 = (r[6] >> 6 | r[7] << 2) & 63;
            r10 = (r[7] >> 4 | r[8] << 4) & 63;
            r11 = (r[8] >> 2) & 63;
            r12 = r[9] & 63;
            r13 = (r[9] >> 6 | r[10] << 2) & 63;
            r14 = (r[10] >> 4 | r[11] << 4) & 63;
            r15 = (r[11] >> 2) & 63;

            reghisto[r0]++;
            reghisto[r1]++;
            reghisto[r2]++;
            reghisto[r3]++;
            reghisto[r4]++;
            reghisto[r5]++;
            reghisto[r6]++;
            reghisto[r7]++;
            reghisto[r8]++;
            reghisto[r9]++;
            reghisto[r10]++;
            reghisto[r11]++;
            reghisto[r12]++;
            reghisto[r13]++;
            reghisto[r14]++;
            reghisto[r15]++;

            r += 12;
        }
    } else {
        for(j = 0; j < HLL_REGISTERS; j++) {
            unsigned long reg;
            HLL_DENSE_GET_REGISTER(reg,registers,j);
            reghisto[reg]++;
        }
    }
}

/* ================== Sparse representation implementation  ================= */

/* Convert the HLL with sparse representation given as input in its dense
 * representation. Both representations are represented by SDS strings, and
 * the input representation is freed as a side effect.
 *
 * The function returns C_OK if the sparse representation was valid,
 * otherwise C_ERR is returned if the representation was corrupted. */
/**
 * ================== 稀疏表示实现 =================
 * 转换具有作为密集输入的稀疏表示的 HLL表示。
 * 两种表示都由 SDS 字符串表示，输入表示作为副作用被释放。
 * 如果稀疏表示有效，则该函数返回 C_OK，否则如果表示已损坏，则返回 C_ERR。
 * */
int hllSparseToDense(robj *o) {
    sds sparse = o->ptr, dense;
    struct hllhdr *hdr, *oldhdr = (struct hllhdr*)sparse;
    int idx = 0, runlen, regval;
    uint8_t *p = (uint8_t*)sparse, *end = p+sdslen(sparse);

    /* If the representation is already the right one return ASAP. */
    hdr = (struct hllhdr*) sparse;
    if (hdr->encoding == HLL_DENSE) return C_OK;

    /* Create a string of the right size filled with zero bytes.
     * Note that the cached cardinality is set to 0 as a side effect
     * that is exactly the cardinality of an empty HLL. */
    dense = sdsnewlen(NULL,HLL_DENSE_SIZE);
    hdr = (struct hllhdr*) dense;
    *hdr = *oldhdr; /* This will copy the magic and cached cardinality. */
    hdr->encoding = HLL_DENSE;

    /* Now read the sparse representation and set non-zero registers
     * accordingly. */
    p += HLL_HDR_SIZE;
    while(p < end) {
        if (HLL_SPARSE_IS_ZERO(p)) {
            runlen = HLL_SPARSE_ZERO_LEN(p);
            idx += runlen;
            p++;
        } else if (HLL_SPARSE_IS_XZERO(p)) {
            runlen = HLL_SPARSE_XZERO_LEN(p);
            idx += runlen;
            p += 2;
        } else {
            runlen = HLL_SPARSE_VAL_LEN(p);
            regval = HLL_SPARSE_VAL_VALUE(p);
            if ((runlen + idx) > HLL_REGISTERS) break; /* Overflow. */
            while(runlen--) {
                HLL_DENSE_SET_REGISTER(hdr->registers,idx,regval);
                idx++;
            }
            p++;
        }
    }

    /* If the sparse representation was valid, we expect to find idx
     * set to HLL_REGISTERS. */
    if (idx != HLL_REGISTERS) {
        sdsfree(dense);
        return C_ERR;
    }

    /* Free the old representation and set the new one. */
    sdsfree(o->ptr);
    o->ptr = dense;
    return C_OK;
}

/* Low level function to set the sparse HLL register at 'index' to the
 * specified value if the current value is smaller than 'count'.
 *
 * The object 'o' is the String object holding the HLL. The function requires
 * a reference to the object in order to be able to enlarge the string if
 * needed.
 *
 * On success, the function returns 1 if the cardinality changed, or 0
 * if the register for this element was not updated.
 * On error (if the representation is invalid) -1 is returned.
 *
 * As a side effect the function may promote the HLL representation from
 * sparse to dense: this happens when a register requires to be set to a value
 * not representable with the sparse representation, or when the resulting
 * size would be greater than server.hll_sparse_max_bytes. */
int hllSparseSet(robj *o, long index, uint8_t count) {
    struct hllhdr *hdr;
    uint8_t oldcount, *sparse, *end, *p, *prev, *next;
    long first, span;
    long is_zero = 0, is_xzero = 0, is_val = 0, runlen = 0;

    /* If the count is too big to be representable by the sparse representation
     * switch to dense representation. */
    if (count > HLL_SPARSE_VAL_MAX_VALUE) goto promote;

    /* When updating a sparse representation, sometimes we may need to
     * enlarge the buffer for up to 3 bytes in the worst case (XZERO split
     * into XZERO-VAL-XZERO). Make sure there is enough space right now
     * so that the pointers we take during the execution of the function
     * will be valid all the time. */
    o->ptr = sdsMakeRoomFor(o->ptr,3);

    /* Step 1: we need to locate the opcode we need to modify to check
     * if a value update is actually needed. */
    sparse = p = ((uint8_t*)o->ptr) + HLL_HDR_SIZE;
    end = p + sdslen(o->ptr) - HLL_HDR_SIZE;

    first = 0;
    prev = NULL; /* Points to previous opcode at the end of the loop. */
    next = NULL; /* Points to the next opcode at the end of the loop. */
    span = 0;
    while(p < end) {
        long oplen;

        /* Set span to the number of registers covered by this opcode.
         *
         * This is the most performance critical loop of the sparse
         * representation. Sorting the conditionals from the most to the
         * least frequent opcode in many-bytes sparse HLLs is faster. */
        oplen = 1;
        if (HLL_SPARSE_IS_ZERO(p)) {
            span = HLL_SPARSE_ZERO_LEN(p);
        } else if (HLL_SPARSE_IS_VAL(p)) {
            span = HLL_SPARSE_VAL_LEN(p);
        } else { /* XZERO. */
            span = HLL_SPARSE_XZERO_LEN(p);
            oplen = 2;
        }
        /* Break if this opcode covers the register as 'index'. */
        if (index <= first+span-1) break;
        prev = p;
        p += oplen;
        first += span;
    }
    if (span == 0 || p >= end) return -1; /* Invalid format. */

    next = HLL_SPARSE_IS_XZERO(p) ? p+2 : p+1;
    if (next >= end) next = NULL;

    /* Cache current opcode type to avoid using the macro again and
     * again for something that will not change.
     * Also cache the run-length of the opcode. */
    if (HLL_SPARSE_IS_ZERO(p)) {
        is_zero = 1;
        runlen = HLL_SPARSE_ZERO_LEN(p);
    } else if (HLL_SPARSE_IS_XZERO(p)) {
        is_xzero = 1;
        runlen = HLL_SPARSE_XZERO_LEN(p);
    } else {
        is_val = 1;
        runlen = HLL_SPARSE_VAL_LEN(p);
    }

    /* Step 2: After the loop:
     *
     * 'first' stores to the index of the first register covered
     *  by the current opcode, which is pointed by 'p'.
     *
     * 'next' ad 'prev' store respectively the next and previous opcode,
     *  or NULL if the opcode at 'p' is respectively the last or first.
     *
     * 'span' is set to the number of registers covered by the current
     *  opcode.
     *
     * There are different cases in order to update the data structure
     * in place without generating it from scratch:
     *
     * A) If it is a VAL opcode already set to a value >= our 'count'
     *    no update is needed, regardless of the VAL run-length field.
     *    In this case PFADD returns 0 since no changes are performed.
     *
     * B) If it is a VAL opcode with len = 1 (representing only our
     *    register) and the value is less than 'count', we just update it
     *    since this is a trivial case. */
    if (is_val) {
        oldcount = HLL_SPARSE_VAL_VALUE(p);
        /* Case A. */
        if (oldcount >= count) return 0;

        /* Case B. */
        if (runlen == 1) {
            HLL_SPARSE_VAL_SET(p,count,1);
            goto updated;
        }
    }

    /* C) Another trivial to handle case is a ZERO opcode with a len of 1.
     * We can just replace it with a VAL opcode with our value and len of 1. */
    if (is_zero && runlen == 1) {
        HLL_SPARSE_VAL_SET(p,count,1);
        goto updated;
    }

    /* D) General case.
     *
     * The other cases are more complex: our register requires to be updated
     * and is either currently represented by a VAL opcode with len > 1,
     * by a ZERO opcode with len > 1, or by an XZERO opcode.
     *
     * In those cases the original opcode must be split into multiple
     * opcodes. The worst case is an XZERO split in the middle resulting into
     * XZERO - VAL - XZERO, so the resulting sequence max length is
     * 5 bytes.
     *
     * We perform the split writing the new sequence into the 'new' buffer
     * with 'newlen' as length. Later the new sequence is inserted in place
     * of the old one, possibly moving what is on the right a few bytes
     * if the new sequence is longer than the older one. */
    uint8_t seq[5], *n = seq;
    int last = first+span-1; /* Last register covered by the sequence. */
    int len;

    if (is_zero || is_xzero) {
        /* Handle splitting of ZERO / XZERO. */
        if (index != first) {
            len = index-first;
            if (len > HLL_SPARSE_ZERO_MAX_LEN) {
                HLL_SPARSE_XZERO_SET(n,len);
                n += 2;
            } else {
                HLL_SPARSE_ZERO_SET(n,len);
                n++;
            }
        }
        HLL_SPARSE_VAL_SET(n,count,1);
        n++;
        if (index != last) {
            len = last-index;
            if (len > HLL_SPARSE_ZERO_MAX_LEN) {
                HLL_SPARSE_XZERO_SET(n,len);
                n += 2;
            } else {
                HLL_SPARSE_ZERO_SET(n,len);
                n++;
            }
        }
    } else {
        /* Handle splitting of VAL. */
        int curval = HLL_SPARSE_VAL_VALUE(p);

        if (index != first) {
            len = index-first;
            HLL_SPARSE_VAL_SET(n,curval,len);
            n++;
        }
        HLL_SPARSE_VAL_SET(n,count,1);
        n++;
        if (index != last) {
            len = last-index;
            HLL_SPARSE_VAL_SET(n,curval,len);
            n++;
        }
    }

    /* Step 3: substitute the new sequence with the old one.
     *
     * Note that we already allocated space on the sds string
     * calling sdsMakeRoomFor(). */
     int seqlen = n-seq;
     int oldlen = is_xzero ? 2 : 1;
     int deltalen = seqlen-oldlen;

     if (deltalen > 0 &&
         sdslen(o->ptr)+deltalen > server.hll_sparse_max_bytes) goto promote;
     if (deltalen && next) memmove(next+deltalen,next,end-next);
     sdsIncrLen(o->ptr,deltalen);
     memcpy(p,seq,seqlen);
     end += deltalen;

updated:
    /* Step 4: Merge adjacent values if possible.
     *
     * The representation was updated, however the resulting representation
     * may not be optimal: adjacent VAL opcodes can sometimes be merged into
     * a single one. */
    p = prev ? prev : sparse;
    int scanlen = 5; /* Scan up to 5 upcodes starting from prev. */
    while (p < end && scanlen--) {
        if (HLL_SPARSE_IS_XZERO(p)) {
            p += 2;
            continue;
        } else if (HLL_SPARSE_IS_ZERO(p)) {
            p++;
            continue;
        }
        /* We need two adjacent VAL opcodes to try a merge, having
         * the same value, and a len that fits the VAL opcode max len. */
        if (p+1 < end && HLL_SPARSE_IS_VAL(p+1)) {
            int v1 = HLL_SPARSE_VAL_VALUE(p);
            int v2 = HLL_SPARSE_VAL_VALUE(p+1);
            if (v1 == v2) {
                int len = HLL_SPARSE_VAL_LEN(p)+HLL_SPARSE_VAL_LEN(p+1);
                if (len <= HLL_SPARSE_VAL_MAX_LEN) {
                    HLL_SPARSE_VAL_SET(p+1,v1,len);
                    memmove(p,p+1,end-p);
                    sdsIncrLen(o->ptr,-1);
                    end--;
                    /* After a merge we reiterate without incrementing 'p'
                     * in order to try to merge the just merged value with
                     * a value on its right. */
                    continue;
                }
            }
        }
        p++;
    }

    /* Invalidate the cached cardinality. */
    hdr = o->ptr;
    HLL_INVALIDATE_CACHE(hdr);
    return 1;

promote: /* Promote to dense representation. */
    if (hllSparseToDense(o) == C_ERR) return -1; /* Corrupted HLL. */
    hdr = o->ptr;

    /* We need to call hllDenseAdd() to perform the operation after the
     * conversion. However the result must be 1, since if we need to
     * convert from sparse to dense a register requires to be updated.
     *
     * Note that this in turn means that PFADD will make sure the command
     * is propagated to slaves / AOF, so if there is a sparse -> dense
     * conversion, it will be performed in all the slaves as well. */
    int dense_retval = hllDenseSet(hdr->registers,index,count);
    serverAssert(dense_retval == 1);
    return dense_retval;
}

/* "Add" the element in the sparse hyperloglog data structure.
 * Actually nothing is added, but the max 0 pattern counter of the subset
 * the element belongs to is incremented if needed.
 *
 * This function is actually a wrapper for hllSparseSet(), it only performs
 * the hashing of the element to obtain the index and zeros run length. */
/**
 * “添加”稀疏超日志数据结构中的元素。
 * 实际上没有添加任何内容，但如果需要，元素所属子集的最大 0 模式计数器会增加。
 * 这个函数实际上是 hllSparseSet() 的一个包装器，它只执行元素的散列以获得索引和零运行长度。
 * */
int hllSparseAdd(robj *o, unsigned char *ele, size_t elesize) {
    long index;
    uint8_t count = hllPatLen(ele,elesize,&index);
    /* Update the register if this element produced a longer run of zeroes. */
    //如果此元素产生更长的零运行，则更新寄存器。
    return hllSparseSet(o,index,count);
}

/* Compute the register histogram in the sparse representation. */
//计算稀疏表示中的寄存器直方图。
void hllSparseRegHisto(uint8_t *sparse, int sparselen, int *invalid, int* reghisto) {
    int idx = 0, runlen, regval;
    uint8_t *end = sparse+sparselen, *p = sparse;

    while(p < end) {
        if (HLL_SPARSE_IS_ZERO(p)) {
            runlen = HLL_SPARSE_ZERO_LEN(p);
            idx += runlen;
            reghisto[0] += runlen;
            p++;
        } else if (HLL_SPARSE_IS_XZERO(p)) {
            runlen = HLL_SPARSE_XZERO_LEN(p);
            idx += runlen;
            reghisto[0] += runlen;
            p += 2;
        } else {
            runlen = HLL_SPARSE_VAL_LEN(p);
            regval = HLL_SPARSE_VAL_VALUE(p);
            idx += runlen;
            reghisto[regval] += runlen;
            p++;
        }
    }
    if (idx != HLL_REGISTERS && invalid) *invalid = 1;
}

/* ========================= HyperLogLog Count ==============================
 * This is the core of the algorithm where the approximated count is computed.
 * The function uses the lower level hllDenseRegHisto() and hllSparseRegHisto()
 * functions as helpers to compute histogram of register values part of the
 * computation, which is representation-specific, while all the rest is common. */

/* Implements the register histogram calculation for uint8_t data type
 * which is only used internally as speedup for PFCOUNT with multiple keys. */
/**
 * ========================== HyperLogLog 计数 ======================= =======
 * 这是计算近似计数的算法的核心。
 * 该函数使用较低级别的 hllDenseRegHisto() 和 hllSparseRegHisto() 函数
 * 作为帮助器来计算寄存器值的直方图，这是计算的一部分，这是特定于表示的，而其余的都是通用的。
 * 实现 uint8_t 数据类型的寄存器直方图计算，该数据类型仅在内部用作具有多个键的 PFCOUNT 的加速。
 * */
void hllRawRegHisto(uint8_t *registers, int* reghisto) {
    uint64_t *word = (uint64_t*) registers;
    uint8_t *bytes;
    int j;

    for (j = 0; j < HLL_REGISTERS/8; j++) {
        if (*word == 0) {
            reghisto[0] += 8;
        } else {
            bytes = (uint8_t*) word;
            reghisto[bytes[0]]++;
            reghisto[bytes[1]]++;
            reghisto[bytes[2]]++;
            reghisto[bytes[3]]++;
            reghisto[bytes[4]]++;
            reghisto[bytes[5]]++;
            reghisto[bytes[6]]++;
            reghisto[bytes[7]]++;
        }
        word++;
    }
}

/* Helper function sigma as defined in
 * "New cardinality estimation algorithms for HyperLogLog sketches"
 * Otmar Ertl, arXiv:1702.01284 */
//辅助函数 sigma 在“HyperLogLog 草图的新基数估计算法”中定义 Otmar Ertl, arXiv:1702.01284
double hllSigma(double x) {
    if (x == 1.) return INFINITY;
    double zPrime;
    double y = 1;
    double z = x;
    do {
        x *= x;
        zPrime = z;
        z += x * y;
        y += y;
    } while(zPrime != z);
    return z;
}

/* Helper function tau as defined in
 * "New cardinality estimation algorithms for HyperLogLog sketches"
 * Otmar Ertl, arXiv:1702.01284 */
//辅助函数 tau 在“HyperLogLog 草图的新基数估计算法”中定义 Otmar Ertl, arXiv:1702.01284
double hllTau(double x) {
    if (x == 0. || x == 1.) return 0.;
    double zPrime;
    double y = 1.0;
    double z = 1 - x;
    do {
        x = sqrt(x);
        zPrime = z;
        y *= 0.5;
        z -= pow(1 - x, 2)*y;
    } while(zPrime != z);
    return z / 3;
}

/* Return the approximated cardinality of the set based on the harmonic
 * mean of the registers values. 'hdr' points to the start of the SDS
 * representing the String object holding the HLL representation.
 *
 * If the sparse representation of the HLL object is not valid, the integer
 * pointed by 'invalid' is set to non-zero, otherwise it is left untouched.
 *
 * hllCount() supports a special internal-only encoding of HLL_RAW, that
 * is, hdr->registers will point to an uint8_t array of HLL_REGISTERS element.
 * This is useful in order to speedup PFCOUNT when called against multiple
 * keys (no need to work with 6-bit integers encoding). */
/**
 * 根据寄存器值的调和平均值返回集合的近似基数。
 * 'hdr' 指向代表保存 HLL 表示的字符串对象的 SDS 的开头。如果 HLL 对象的稀疏表示无效，
 * 则 'invalid' 指向的整数设置为非零，否则保持不变。
 * hllCount() 支持 HLL_RAW 的特殊内部专用编码，即 hdr->registers 将指向 HLL_REGISTERS 元素的 uint8_t 数组。
 * 这对于在针对多个键调用时加速 PFCOUNT 很有用（无需使用 6 位整数编码）。
 * */
uint64_t hllCount(struct hllhdr *hdr, int *invalid) {
    double m = HLL_REGISTERS;
    double E;
    int j;
    /* Note that reghisto size could be just HLL_Q+2, because HLL_Q+1 is
     * the maximum frequency of the "000...1" sequence the hash function is
     * able to return. However it is slow to check for sanity of the
     * input: instead we history array at a safe size: overflows will
     * just write data to wrong, but correctly allocated, places. */
    /**
     * 请注意，reghisto 大小可能只是 HLL_Q+2，因为 HLL_Q+1 是哈希函数能够返回的“000...1”序列的最大频率。
     * 但是检查输入的完整性很慢：相反，我们以安全的大小历史数组：溢出只会将数据写入错误但正确分配的位置。
     * */
    int reghisto[64] = {0};

    /* Compute register histogram */
    //计算寄存器直方图
    if (hdr->encoding == HLL_DENSE) {
        hllDenseRegHisto(hdr->registers,reghisto);
    } else if (hdr->encoding == HLL_SPARSE) {
        hllSparseRegHisto(hdr->registers,
                         sdslen((sds)hdr)-HLL_HDR_SIZE,invalid,reghisto);
    } else if (hdr->encoding == HLL_RAW) {
        hllRawRegHisto(hdr->registers,reghisto);
    } else {
        serverPanic("Unknown HyperLogLog encoding in hllCount()");
    }

    /* Estimate cardinality from register histogram. See:
     * "New cardinality estimation algorithms for HyperLogLog sketches"
     * Otmar Ertl, arXiv:1702.01284 */
    //从寄存器直方图估计基数。请参阅：“HyperLogLog 草图的新基数估计算法”Otmar Ertl，arXiv:1702.01284
    double z = m * hllTau((m-reghisto[HLL_Q+1])/(double)m);
    for (j = HLL_Q; j >= 1; --j) {
        z += reghisto[j];
        z *= 0.5;
    }
    z += m * hllSigma(reghisto[0]/(double)m);
    E = llroundl(HLL_ALPHA_INF*m*m/z);

    return (uint64_t) E;
}

/* Call hllDenseAdd() or hllSparseAdd() according to the HLL encoding. */
//根据 HLL 编码调用 hllDenseAdd() 或 hllSparseAdd()。
int hllAdd(robj *o, unsigned char *ele, size_t elesize) {
    struct hllhdr *hdr = o->ptr;
    switch(hdr->encoding) {
    case HLL_DENSE: return hllDenseAdd(hdr->registers,ele,elesize);
    case HLL_SPARSE: return hllSparseAdd(o,ele,elesize);
    default: return -1; /* Invalid representation. */
    }
}

/* Merge by computing MAX(registers[i],hll[i]) the HyperLogLog 'hll'
 * with an array of uint8_t HLL_REGISTERS registers pointed by 'max'.
 *
 * The hll object must be already validated via isHLLObjectOrReply()
 * or in some other way.
 *
 * If the HyperLogLog is sparse and is found to be invalid, C_ERR
 * is returned, otherwise the function always succeeds. */
/**
 * 通过计算 MAX(registers[i],hll[i]) 将 HyperLogLog 'hll' 与 'max'
 * 指向的 uint8_t HLL_REGISTERS 寄存器数组合并。
 * hll 对象必须已经通过 isHLLObjectOrReply() 或其他方式进行了验证。
 * 如果 HyperLogLog 稀疏且发现无效，则返回 C_ERR，否则函数始终成功。
 * */
int hllMerge(uint8_t *max, robj *hll) {
    struct hllhdr *hdr = hll->ptr;
    int i;

    if (hdr->encoding == HLL_DENSE) {
        uint8_t val;

        for (i = 0; i < HLL_REGISTERS; i++) {
            HLL_DENSE_GET_REGISTER(val,hdr->registers,i);
            if (val > max[i]) max[i] = val;
        }
    } else {
        uint8_t *p = hll->ptr, *end = p + sdslen(hll->ptr);
        long runlen, regval;

        p += HLL_HDR_SIZE;
        i = 0;
        while(p < end) {
            if (HLL_SPARSE_IS_ZERO(p)) {
                runlen = HLL_SPARSE_ZERO_LEN(p);
                i += runlen;
                p++;
            } else if (HLL_SPARSE_IS_XZERO(p)) {
                runlen = HLL_SPARSE_XZERO_LEN(p);
                i += runlen;
                p += 2;
            } else {
                runlen = HLL_SPARSE_VAL_LEN(p);
                regval = HLL_SPARSE_VAL_VALUE(p);
                if ((runlen + i) > HLL_REGISTERS) break; /* Overflow. */
                while(runlen--) {
                    if (regval > max[i]) max[i] = regval;
                    i++;
                }
                p++;
            }
        }
        if (i != HLL_REGISTERS) return C_ERR;
    }
    return C_OK;
}

/* ========================== HyperLogLog commands ========================== */

/* Create an HLL object. We always create the HLL using sparse encoding.
 * This will be upgraded to the dense representation as needed. */
/**
 * =========================== HyperLogLog 命令 ====================== ====
 * 创建一个 HLL 对象。我们总是使用稀疏编码创建 HLL。这将根据需要升级为密集表示。
 * */
robj *createHLLObject(void) {
    robj *o;
    struct hllhdr *hdr;
    sds s;
    uint8_t *p;
    int sparselen = HLL_HDR_SIZE +
                    (((HLL_REGISTERS+(HLL_SPARSE_XZERO_MAX_LEN-1)) /
                     HLL_SPARSE_XZERO_MAX_LEN)*2);
    int aux;

    /* Populate the sparse representation with as many XZERO opcodes as
     * needed to represent all the registers. */
    //使用表示所有寄存器所需的尽可能多的 XZERO 操作码填充稀疏表示。
    aux = HLL_REGISTERS;
    s = sdsnewlen(NULL,sparselen);
    p = (uint8_t*)s + HLL_HDR_SIZE;
    while(aux) {
        int xzero = HLL_SPARSE_XZERO_MAX_LEN;
        if (xzero > aux) xzero = aux;
        HLL_SPARSE_XZERO_SET(p,xzero);
        p += 2;
        aux -= xzero;
    }
    serverAssert((p-(uint8_t*)s) == sparselen);

    /* Create the actual object.创建实际对象。 */
    o = createObject(OBJ_STRING,s);
    hdr = o->ptr;
    memcpy(hdr->magic,"HYLL",4);
    hdr->encoding = HLL_SPARSE;
    return o;
}

/* Check if the object is a String with a valid HLL representation.
 * Return C_OK if this is true, otherwise reply to the client
 * with an error and return C_ERR. */
//检查对象是否是具有有效 HLL 表示的字符串。如果为真则返回 C_OK，否则以错误回复客户端并返回 C_ERR。
int isHLLObjectOrReply(client *c, robj *o) {
    struct hllhdr *hdr;

    /* Key exists, check type 密钥存在，检查类型*/
    if (checkType(c,o,OBJ_STRING))
        return C_ERR; /* Error already sent. 错误已发送。*/

    if (!sdsEncodedObject(o)) goto invalid;
    if (stringObjectLen(o) < sizeof(*hdr)) goto invalid;
    hdr = o->ptr;

    /* Magic should be "HYLL". 魔术应该是“HYLL”。*/
    if (hdr->magic[0] != 'H' || hdr->magic[1] != 'Y' ||
        hdr->magic[2] != 'L' || hdr->magic[3] != 'L') goto invalid;

    if (hdr->encoding > HLL_MAX_ENCODING) goto invalid;

    /* Dense representation string length should match exactly. */
    //密集表示字符串长度应该完全匹配。
    if (hdr->encoding == HLL_DENSE &&
        stringObjectLen(o) != HLL_DENSE_SIZE) goto invalid;

    /* All tests passed. 所有测试都通过了。*/
    return C_OK;

invalid:
    addReplyError(c,"-WRONGTYPE Key is not a valid "
               "HyperLogLog string value.");
    return C_ERR;
}

/* PFADD var ele ele ele ... ele => :0 or :1 */
//PFADD var he he he ... he => :0 或 :1
void pfaddCommand(client *c) {
    robj *o = lookupKeyWrite(c->db,c->argv[1]);
    struct hllhdr *hdr;
    int updated = 0, j;

    if (o == NULL) {
        /* Create the key with a string value of the exact length to
         * hold our HLL data structure. sdsnewlen() when NULL is passed
         * is guaranteed to return bytes initialized to zero. */
        /**
         * 创建具有精确长度的字符串值的键来保存我们的 HLL 数据结构。
         * 当传递 NULL 时，sdsnewlen() 保证返回初始化为零的字节。 */
        o = createHLLObject();
        dbAdd(c->db,c->argv[1],o);
        updated++;
    } else {
        if (isHLLObjectOrReply(c,o) != C_OK) return;
        o = dbUnshareStringValue(c->db,c->argv[1],o);
    }
    /* Perform the low level ADD operation for every element. */
    //对每个元素执行低级 ADD 操作。
    for (j = 2; j < c->argc; j++) {
        int retval = hllAdd(o, (unsigned char*)c->argv[j]->ptr,
                               sdslen(c->argv[j]->ptr));
        switch(retval) {
        case 1:
            updated++;
            break;
        case -1:
            addReplyError(c,invalid_hll_err);
            return;
        }
    }
    hdr = o->ptr;
    if (updated) {
        signalModifiedKey(c,c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_STRING,"pfadd",c->argv[1],c->db->id);
        server.dirty += updated;
        HLL_INVALIDATE_CACHE(hdr);
    }
    addReply(c, updated ? shared.cone : shared.czero);
}

/* PFCOUNT var -> approximated cardinality of set. */
//PFCOUNT var -> 集合的近似基数。
void pfcountCommand(client *c) {
    robj *o;
    struct hllhdr *hdr;
    uint64_t card;

    /* Case 1: multi-key keys, cardinality of the union.
     *
     * When multiple keys are specified, PFCOUNT actually computes
     * the cardinality of the merge of the N HLLs specified. */
    /**
     * 案例 1：多键密钥，联合的基数。当指定多个键时，PFCOUNT 实际上计算指定的 N 个 HLL 合并的基数。
     * */
    if (c->argc > 2) {
        uint8_t max[HLL_HDR_SIZE+HLL_REGISTERS], *registers;
        int j;

        /* Compute an HLL with M[i] = MAX(M[i]_j). */
        //用 M[i] = MAX(M[i]_j) 计算 HLL。
        memset(max,0,sizeof(max));
        hdr = (struct hllhdr*) max;
        hdr->encoding = HLL_RAW; /* Special internal-only encoding. 特殊的内部编码。*/
        registers = max + HLL_HDR_SIZE;
        for (j = 1; j < c->argc; j++) {
            /* Check type and size.检查类型和尺寸。 */
            robj *o = lookupKeyRead(c->db,c->argv[j]);
            if (o == NULL) continue; /* Assume empty HLL for non existing var.假设不存在的 var 为空 HLL。*/
            if (isHLLObjectOrReply(c,o) != C_OK) return;

            /* Merge with this HLL with our 'max' HLL by setting max[i]
             * to MAX(max[i],hll[i]). */
            //通过将 max[i] 设置为 MAX(max[i],hll[i])，将此 HLL 与我们的“max”HLL 合并。
            if (hllMerge(registers,o) == C_ERR) {
                addReplyError(c,invalid_hll_err);
                return;
            }
        }

        /* Compute cardinality of the resulting set. */
        //计算结果集的基数。
        addReplyLongLong(c,hllCount(hdr,NULL));
        return;
    }

    /* Case 2: cardinality of the single HLL.
     *
     * The user specified a single key. Either return the cached value
     * or compute one and update the cache.
     *
     * Since a HLL is a regular Redis string type value, updating the cache does
     * modify the value. We do a lookupKeyRead anyway since this is flagged as a
     * read-only command. The difference is that with lookupKeyWrite, a
     * logically expired key on a replica is deleted, while with lookupKeyRead
     * it isn't, but the lookup returns NULL either way if the key is logically
     * expired, which is what matters here. */
    /**
     * 案例 2：单个 HLL 的基数。用户指定了一个键。
     * 返回缓存的值或计算一个并更新缓存。由于 HLL 是常规的 Redis 字符串类型值，因此更新缓存确实会修改该值。
     * 无论如何，我们都会执行 lookupKeyRead，因为它被标记为只读命令。
     * 不同之处在于，使用lookupKeyWrite 会删除副本上的逻辑过期键，而使用lookupKeyRead 不会删除，
     * 但是如果键逻辑过期，则无论哪种方式查找都会返回NULL，这在这里很重要。
     * */
    o = lookupKeyRead(c->db,c->argv[1]);
    if (o == NULL) {
        /* No key? Cardinality is zero since no element was added, otherwise
         * we would have a key as HLLADD creates it as a side effect. */
        /**
         * 没有钥匙？基数为零，因为没有添加任何元素，否则我们将拥有一个键，因为 HLLADD 创建它作为副作用。
         * */
        addReply(c,shared.czero);
    } else {
        if (isHLLObjectOrReply(c,o) != C_OK) return;
        o = dbUnshareStringValue(c->db,c->argv[1],o);

        /* Check if the cached cardinality is valid. */
        //检查缓存的基数是否有效。
        hdr = o->ptr;
        if (HLL_VALID_CACHE(hdr)) {
            /* Just return the cached value. 只需返回缓存的值。*/
            card = (uint64_t)hdr->card[0];
            card |= (uint64_t)hdr->card[1] << 8;
            card |= (uint64_t)hdr->card[2] << 16;
            card |= (uint64_t)hdr->card[3] << 24;
            card |= (uint64_t)hdr->card[4] << 32;
            card |= (uint64_t)hdr->card[5] << 40;
            card |= (uint64_t)hdr->card[6] << 48;
            card |= (uint64_t)hdr->card[7] << 56;
        } else {
            int invalid = 0;
            /* Recompute it and update the cached value. */
            //重新计算它并更新缓存的值。
            card = hllCount(hdr,&invalid);
            if (invalid) {
                addReplyError(c,invalid_hll_err);
                return;
            }
            hdr->card[0] = card & 0xff;
            hdr->card[1] = (card >> 8) & 0xff;
            hdr->card[2] = (card >> 16) & 0xff;
            hdr->card[3] = (card >> 24) & 0xff;
            hdr->card[4] = (card >> 32) & 0xff;
            hdr->card[5] = (card >> 40) & 0xff;
            hdr->card[6] = (card >> 48) & 0xff;
            hdr->card[7] = (card >> 56) & 0xff;
            /* This is considered a read-only command even if the cached value
             * may be modified and given that the HLL is a Redis string
             * we need to propagate the change. */
            /**
             * 这被认为是只读命令，即使缓存的值可能被修改，并且假设 HLL 是 Redis 字符串，我们需要传播更改。
             * */
            signalModifiedKey(c,c->db,c->argv[1]);
            server.dirty++;
        }
        addReplyLongLong(c,card);
    }
}

/* PFMERGE dest src1 src2 src3 ... srcN => OK */
void pfmergeCommand(client *c) {
    uint8_t max[HLL_REGISTERS];
    struct hllhdr *hdr;
    int j;
    int use_dense = 0; /* Use dense representation as target? 使用密集表示作为目标？*/

    /* Compute an HLL with M[i] = MAX(M[i]_j).
     * We store the maximum into the max array of registers. We'll write
     * it to the target variable later. */
    /**
     * 用 M[i] = MAX(M[i]_j) 计算 HLL。我们将最大值存储到寄存器的最大数组中。稍后我们会将其写入目标变量。
     * */
    memset(max,0,sizeof(max));
    for (j = 1; j < c->argc; j++) {
        /* Check type and size. 检查类型和尺寸。*/
        robj *o = lookupKeyRead(c->db,c->argv[j]);
        if (o == NULL) continue; /* Assume empty HLL for non existing var. 假设不存在的 var 为空 HLL。*/
        if (isHLLObjectOrReply(c,o) != C_OK) return;

        /* If at least one involved HLL is dense, use the dense representation
         * as target ASAP to save time and avoid the conversion step. */
        /**
         * 如果至少一个涉及的 HLL 是密集的，请尽快使用密集表示作为目标，以节省时间并避免转换步骤。
         * */
        hdr = o->ptr;
        if (hdr->encoding == HLL_DENSE) use_dense = 1;

        /* Merge with this HLL with our 'max' HLL by setting max[i]
         * to MAX(max[i],hll[i]). */
        //通过将 max[i] 设置为 MAX(max[i],hll[i])，将此 HLL 与我们的“max”HLL 合并。
        if (hllMerge(max,o) == C_ERR) {
            addReplyError(c,invalid_hll_err);
            return;
        }
    }

    /* Create / unshare the destination key's value if needed. */
    //如果需要，创建取消共享目标键的值。
    robj *o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) {
        /* Create the key with a string value of the exact length to
         * hold our HLL data structure. sdsnewlen() when NULL is passed
         * is guaranteed to return bytes initialized to zero. */
        /**
         * 创建具有精确长度的字符串值的键来保存我们的 HLL 数据结构。
         * 当传递 NULL 时，sdsnewlen() 保证返回初始化为零的字节。
         * */
        o = createHLLObject();
        dbAdd(c->db,c->argv[1],o);
    } else {
        /* If key exists we are sure it's of the right type/size
         * since we checked when merging the different HLLs, so we
         * don't check again. */
        /**
         * 如果 key 存在，我们确定它的类型大小是正确的，因为我们在合并不同的 HLL 时进行了检查，因此我们不再检查。
         * */
        o = dbUnshareStringValue(c->db,c->argv[1],o);
    }

    /* Convert the destination object to dense representation if at least
     * one of the inputs was dense. */
    //如果至少有一个输入是密集的，则将目标对象转换为密集表示。
    if (use_dense && hllSparseToDense(o) == C_ERR) {
        addReplyError(c,invalid_hll_err);
        return;
    }

    /* Write the resulting HLL to the destination HLL registers and
     * invalidate the cached value. */
    //将生成的 HLL 写入目标 HLL 寄存器并使缓存值无效。
    for (j = 0; j < HLL_REGISTERS; j++) {
        if (max[j] == 0) continue;
        hdr = o->ptr;
        switch(hdr->encoding) {
        case HLL_DENSE: hllDenseSet(hdr->registers,j,max[j]); break;
        case HLL_SPARSE: hllSparseSet(o,j,max[j]); break;
        }
    }
    hdr = o->ptr; /* o->ptr may be different now, as a side effect of last
                * hllSparseSet() call.o->ptr 现在可能会有所不同，这是上次调用 hllSparseSet() 的副作用。 */
    HLL_INVALIDATE_CACHE(hdr);

    signalModifiedKey(c,c->db,c->argv[1]);
    /* We generate a PFADD event for PFMERGE for semantical simplicity
     * since in theory this is a mass-add of elements. */
    //我们为 PFMERGE 生成一个 PFADD 事件以简化语义，因为理论上这是元素的大量添加。
    notifyKeyspaceEvent(NOTIFY_STRING,"pfadd",c->argv[1],c->db->id);
    server.dirty++;
    addReply(c,shared.ok);
}

/* ========================== Testing / Debugging  ========================== */

/* PFSELFTEST
 * This command performs a self-test of the HLL registers implementation.
 * Something that is not easy to test from within the outside. */
/**
 * ===========================测试调试====================== ====
 * PFSELFTEST 此命令执行 HLL 寄存器实现的自检。从外部很难测试的东西。*/
#define HLL_TEST_CYCLES 1000
void pfselftestCommand(client *c) {
    unsigned int j, i;
    sds bitcounters = sdsnewlen(NULL,HLL_DENSE_SIZE);
    struct hllhdr *hdr = (struct hllhdr*) bitcounters, *hdr2;
    robj *o = NULL;
    uint8_t bytecounters[HLL_REGISTERS];

    /* Test 1: access registers.
     * The test is conceived to test that the different counters of our data
     * structure are accessible and that setting their values both result in
     * the correct value to be retained and not affect adjacent values. */
    /**
     * 测试 1：访问寄存器。该测试旨在测试我们数据结构的不同计数器是否可访问，
     * 并且设置它们的值会导致正确的值被保留并且不会影响相邻的值。
     * */
    for (j = 0; j < HLL_TEST_CYCLES; j++) {
        /* Set the HLL counters and an array of unsigned byes of the
         * same size to the same set of random values. */
        //将 HLL 计数器和相同大小的无符号字节数组设置为相同的随机值集。
        for (i = 0; i < HLL_REGISTERS; i++) {
            unsigned int r = rand() & HLL_REGISTER_MAX;

            bytecounters[i] = r;
            HLL_DENSE_SET_REGISTER(hdr->registers,i,r);
        }
        /* Check that we are able to retrieve the same values. */
        //检查我们是否能够检索相同的值。
        for (i = 0; i < HLL_REGISTERS; i++) {
            unsigned int val;

            HLL_DENSE_GET_REGISTER(val,hdr->registers,i);
            if (val != bytecounters[i]) {
                addReplyErrorFormat(c,
                    "TESTFAILED Register %d should be %d but is %d",
                    i, (int) bytecounters[i], (int) val);
                goto cleanup;
            }
        }
    }

    /* Test 2: approximation error.
     * The test adds unique elements and check that the estimated value
     * is always reasonable bounds.
     *
     * We check that the error is smaller than a few times than the expected
     * standard error, to make it very unlikely for the test to fail because
     * of a "bad" run.
     *
     * The test is performed with both dense and sparse HLLs at the same
     * time also verifying that the computed cardinality is the same. */
    /**
     * 测试 2：近似误差。该测试添加了唯一元素并检查估计值是否始终是合理的范围。
     * 我们检查误差是否比预期的标准误差小几倍，以使测试不太可能因为“糟糕”的运行而失败。
     * 该测试同时使用密集和稀疏 HLL 执行，还验证计算的基数是否相同。
     * */
    memset(hdr->registers,0,HLL_DENSE_SIZE-HLL_HDR_SIZE);
    o = createHLLObject();
    double relerr = 1.04/sqrt(HLL_REGISTERS);
    int64_t checkpoint = 1;
    uint64_t seed = (uint64_t)rand() | (uint64_t)rand() << 32;
    uint64_t ele;
    for (j = 1; j <= 10000000; j++) {
        ele = j ^ seed;
        hllDenseAdd(hdr->registers,(unsigned char*)&ele,sizeof(ele));
        hllAdd(o,(unsigned char*)&ele,sizeof(ele));

        /* Make sure that for small cardinalities we use sparse
         * encoding. */
        //确保对于小基数我们使用稀疏编码。
        if (j == checkpoint && j < server.hll_sparse_max_bytes/2) {
            hdr2 = o->ptr;
            if (hdr2->encoding != HLL_SPARSE) {
                addReplyError(c, "TESTFAILED sparse encoding not used");
                goto cleanup;
            }
        }

        /* Check that dense and sparse representations agree. */
        //检查密集和稀疏表示是否一致。
        if (j == checkpoint && hllCount(hdr,NULL) != hllCount(o->ptr,NULL)) {
                addReplyError(c, "TESTFAILED dense/sparse disagree");
                goto cleanup;
        }

        /* Check error. 检查错误。*/
        if (j == checkpoint) {
            int64_t abserr = checkpoint - (int64_t)hllCount(hdr,NULL);
            uint64_t maxerr = ceil(relerr*6*checkpoint);

            /* Adjust the max error we expect for cardinality 10
             * since from time to time it is statistically likely to get
             * much higher error due to collision, resulting into a false
             * positive. */
            /**
             * 调整我们对基数 10 的预期最大误差，因为在统计上它有时会由于碰撞而得到更高的误差，从而导致误报。
             * */
            if (j == 10) maxerr = 1;

            if (abserr < 0) abserr = -abserr;
            if (abserr > (int64_t)maxerr) {
                addReplyErrorFormat(c,
                    "TESTFAILED Too big error. card:%llu abserr:%llu",
                    (unsigned long long) checkpoint,
                    (unsigned long long) abserr);
                goto cleanup;
            }
            checkpoint *= 10;
        }
    }

    /* Success! */
    addReply(c,shared.ok);

cleanup:
    sdsfree(bitcounters);
    if (o) decrRefCount(o);
}

/* Different debugging related operations about the HLL implementation.
 *
 * PFDEBUG GETREG <key>
 * PFDEBUG DECODE <key>
 * PFDEBUG ENCODING <key>
 * PFDEBUG TODENSE <key>
 */
void pfdebugCommand(client *c) {
    char *cmd = c->argv[1]->ptr;
    struct hllhdr *hdr;
    robj *o;
    int j;

    o = lookupKeyWrite(c->db,c->argv[2]);
    if (o == NULL) {
        addReplyError(c,"The specified key does not exist");
        return;
    }
    if (isHLLObjectOrReply(c,o) != C_OK) return;
    o = dbUnshareStringValue(c->db,c->argv[2],o);
    hdr = o->ptr;

    /* PFDEBUG GETREG <key> */
    if (!strcasecmp(cmd,"getreg")) {
        if (c->argc != 3) goto arityerr;

        if (hdr->encoding == HLL_SPARSE) {
            if (hllSparseToDense(o) == C_ERR) {
                addReplyError(c,invalid_hll_err);
                return;
            }
            server.dirty++; /* Force propagation on encoding change. */
        }

        hdr = o->ptr;
        addReplyArrayLen(c,HLL_REGISTERS);
        for (j = 0; j < HLL_REGISTERS; j++) {
            uint8_t val;

            HLL_DENSE_GET_REGISTER(val,hdr->registers,j);
            addReplyLongLong(c,val);
        }
    }
    /* PFDEBUG DECODE <key> */
    else if (!strcasecmp(cmd,"decode")) {
        if (c->argc != 3) goto arityerr;

        uint8_t *p = o->ptr, *end = p+sdslen(o->ptr);
        sds decoded = sdsempty();

        if (hdr->encoding != HLL_SPARSE) {
            sdsfree(decoded);
            addReplyError(c,"HLL encoding is not sparse");
            return;
        }

        p += HLL_HDR_SIZE;
        while(p < end) {
            int runlen, regval;

            if (HLL_SPARSE_IS_ZERO(p)) {
                runlen = HLL_SPARSE_ZERO_LEN(p);
                p++;
                decoded = sdscatprintf(decoded,"z:%d ",runlen);
            } else if (HLL_SPARSE_IS_XZERO(p)) {
                runlen = HLL_SPARSE_XZERO_LEN(p);
                p += 2;
                decoded = sdscatprintf(decoded,"Z:%d ",runlen);
            } else {
                runlen = HLL_SPARSE_VAL_LEN(p);
                regval = HLL_SPARSE_VAL_VALUE(p);
                p++;
                decoded = sdscatprintf(decoded,"v:%d,%d ",regval,runlen);
            }
        }
        decoded = sdstrim(decoded," ");
        addReplyBulkCBuffer(c,decoded,sdslen(decoded));
        sdsfree(decoded);
    }
    /* PFDEBUG ENCODING <key> */
    else if (!strcasecmp(cmd,"encoding")) {
        char *encodingstr[2] = {"dense","sparse"};
        if (c->argc != 3) goto arityerr;

        addReplyStatus(c,encodingstr[hdr->encoding]);
    }
    /* PFDEBUG TODENSE <key> */
    else if (!strcasecmp(cmd,"todense")) {
        int conv = 0;
        if (c->argc != 3) goto arityerr;

        if (hdr->encoding == HLL_SPARSE) {
            if (hllSparseToDense(o) == C_ERR) {
                addReplyError(c,invalid_hll_err);
                return;
            }
            conv = 1;
            server.dirty++; /* Force propagation on encoding change. */
        }
        addReply(c,conv ? shared.cone : shared.czero);
    } else {
        addReplyErrorFormat(c,"Unknown PFDEBUG subcommand '%s'", cmd);
    }
    return;

arityerr:
    addReplyErrorFormat(c,
        "Wrong number of arguments for the '%s' subcommand",cmd);
}

