#ifndef STREAM_H
#define STREAM_H

#include "rax.h"
#include "listpack.h"

/* Stream item ID: a 128 bit number composed of a milliseconds time and
 * a sequence counter. IDs generated in the same millisecond (or in a past
 * millisecond if the clock jumped backward) will use the millisecond time
 * of the latest generated ID and an incremented sequence. */
/**流项 ID：一个 128 位的数字，由毫秒时间和序列计数器组成。
 * 在同一毫秒内生成的 ID（如果时钟向后跳，则在过去的毫秒内）将使用最新生成的 ID 的毫秒时间和递增的序列。*/
typedef struct streamID {
    uint64_t ms;        /* Unix time in milliseconds. */
    uint64_t seq;       /* Sequence number. */
} streamID;

typedef struct stream {
    rax *rax;               /* The radix tree holding the stream. 容纳流的基数树。*/
    uint64_t length;        /* Current number of elements inside this stream. 此流中的当前元素数。*/
    streamID last_id;       /* Zero if there are yet no items. 如果还没有项目，则为零。*/
    streamID first_id;      /* The first non-tombstone entry, zero if empty.第一个非墓碑条目，如果为空则为零。 */
    streamID max_deleted_entry_id;  /* The maximal ID that was deleted. 被删除的最大 ID。*/
    uint64_t entries_added; /* All time count of elements added. 添加元素的所有时间计数。*/
    rax *cgroups;           /* Consumer groups dictionary: name -> streamCG 消费者组字典：name -> streamCG*/
} stream;

/* We define an iterator to iterate stream items in an abstract way, without
 * caring about the radix tree + listpack representation. Technically speaking
 * the iterator is only used inside streamReplyWithRange(), so could just
 * be implemented inside the function, but practically there is the AOF
 * rewriting code that also needs to iterate the stream to emit the XADD
 * commands. */
/**我们定义了一个迭代器以抽象的方式迭代流项目，而不关心基数树 + 列表包表示。
 * 从技术上讲，迭代器仅在 streamReplyWithRange() 内部使用，因此可以在函数内部实现，
 * 但实际上存在 AOF 重写代码，也需要迭代流以发出 XADD 命令。*/
typedef struct streamIterator {
    stream *stream;         /* The stream we are iterating. */
    streamID master_id;     /* ID of the master entry at listpack head.列表包头的master的 ID。 */
    uint64_t master_fields_count;       /* Master entries # of fields. */
    unsigned char *master_fields_start; /* Master entries start in listpack. */
    unsigned char *master_fields_ptr;   /* Master field to emit next. */
    int entry_flags;                    /* Flags of entry we are emitting. */
    int rev;                /* True if iterating end to start (reverse). */
    int skip_tombstones;    /* True if not emitting tombstone entries. 如果不发出墓碑条目，则为真。*/
    uint64_t start_key[2];  /* Start key as 128 bit big endian. */
    uint64_t end_key[2];    /* End key as 128 bit big endian. */
    raxIterator ri;         /* Rax iterator. */
    unsigned char *lp;      /* Current listpack. */
    unsigned char *lp_ele;  /* Current listpack cursor. 当前列表包光标。*/
    unsigned char *lp_flags; /* Current entry flags pointer. */
    /* Buffers used to hold the string of lpGet() when the element is
     * integer encoded, so that there is no string representation of the
     * element inside the listpack itself. */
    /**当元素是整数编码时，缓冲区用于保存 lpGet() 的字符串，因此 listpack 本身内部没有元素的字符串表示形式。*/
    unsigned char field_buf[LP_INTBUF_SIZE];
    unsigned char value_buf[LP_INTBUF_SIZE];
} streamIterator;

/* Consumer group. */
typedef struct streamCG {
    streamID last_id;       /* Last delivered (not acknowledged) ID for this
                               group. Consumers that will just ask for more
                               messages will served with IDs > than this.
                               此组的上次交付（未确认）ID。只要求更多消息的消费者将使用 ID > 提供服务。*/
    long long entries_read; /* In a perfect world (CG starts at 0-0, no dels, no
                               XGROUP SETID, ...), this is the total number of
                               group reads. In the real world, the reasoning behind
                               this value is detailed at the top comment of
                               streamEstimateDistanceFromFirstEverEntry().
                               在一个完美的世界中（CG 从 0-0 开始，没有 dels，没有 XGROUP SETID，...），这是组读取的总数。
                               在现实世界中，该值背后的原因在 streamEstimateDistanceFromFirstEverEntry() 的顶部注释中有详细说明。*/
    rax *pel;               /* Pending entries list. This is a radix tree that
                               has every message delivered to consumers (without
                               the NOACK option) that was yet not acknowledged
                               as processed. The key of the radix tree is the
                               ID as a 64 bit big endian number, while the
                               associated value is a streamNACK structure.
                               待处理条目列表。这是一个基数树，它将每条消息传递给尚未确认为已处理的消费者（没有 NOACK 选项）。
                               基数树的键是 64 位大端数的 ID，而关联的值是 streamNACK 结构。*/
    rax *consumers;         /* A radix tree representing the consumers by name
                               and their associated representation in the form
                               of streamConsumer structures.
                               按名称表示消费者的基数树及其以 streamConsumer 结构形式的关联表示。*/
} streamCG;

/* A specific consumer in a consumer group.  消费者组中的特定消费者。*/
typedef struct streamConsumer {
    mstime_t seen_time;         /* Last time this consumer was active. */
    sds name;                   /* Consumer name. This is how the consumer
                                   will be identified in the consumer group
                                   protocol. Case sensitive.
                                   消费者名称。这就是消费者组协议中消费者的识别方式。区分大小写。*/
    rax *pel;                   /* Consumer specific pending entries list: all
                                   the pending messages delivered to this
                                   consumer not yet acknowledged. Keys are
                                   big endian message IDs, while values are
                                   the same streamNACK structure referenced
                                   in the "pel" of the consumer group structure
                                   itself, so the value is shared.
                                   消费者特定的待处理条目列表：传递给此消费者的所有待处理消息尚未确认。
                                   键是大端消息 ID，而值是在消费者组结构本身的“pel”中引用的相同 streamNACK 结构，因此值是共享的。*/
} streamConsumer;

/* Pending (yet not acknowledged) message in a consumer group. */
/**消费者组中的待处理（尚未确认）消息。*/
typedef struct streamNACK {
    mstime_t delivery_time;     /* Last time this message was delivered. 上次发送此消息的时间。*/
    uint64_t delivery_count;    /* Number of times this message was delivered. 此消息的传递次数。*/
    streamConsumer *consumer;   /* The consumer this message was delivered to
                                   in the last delivery.
                                   此消息在上次交付时交付给的消费者。*/
} streamNACK;

/* Stream propagation information, passed to functions in order to propagate
 * XCLAIM commands to AOF and slaves. */
//流传播信息，传递给函数以便将 XCLAIM 命令传播到 AOF 和从属。
typedef struct streamPropInfo {
    robj *keyname;
    robj *groupname;
} streamPropInfo;

/* Prototypes of exported APIs. */
struct client;

/* Flags for streamLookupConsumer */
#define SLC_DEFAULT      0
#define SLC_NO_REFRESH   (1<<0) /* Do not update consumer's seen-time 不更新消费者的观看时间*/

/* Flags for streamCreateConsumer   streamCreateConsumer 的标志*/
#define SCC_DEFAULT       0
#define SCC_NO_NOTIFY     (1<<0) /* Do not notify key space if consumer created 如果消费者创建，则不通知密钥空间*/
#define SCC_NO_DIRTIFY    (1<<1) /* Do not dirty++ if consumer created  如果消费者创建，不要dirty++*/

#define SCG_INVALID_ENTRIES_READ -1

stream *streamNew(void);
void freeStream(stream *s);
unsigned long streamLength(const robj *subject);
size_t streamReplyWithRange(client *c, stream *s, streamID *start, streamID *end, size_t count, int rev, streamCG *group, streamConsumer *consumer, int flags, streamPropInfo *spi);
void streamIteratorStart(streamIterator *si, stream *s, streamID *start, streamID *end, int rev);
int streamIteratorGetID(streamIterator *si, streamID *id, int64_t *numfields);
void streamIteratorGetField(streamIterator *si, unsigned char **fieldptr, unsigned char **valueptr, int64_t *fieldlen, int64_t *valuelen);
void streamIteratorRemoveEntry(streamIterator *si, streamID *current);
void streamIteratorStop(streamIterator *si);
streamCG *streamLookupCG(stream *s, sds groupname);
streamConsumer *streamLookupConsumer(streamCG *cg, sds name, int flags);
streamConsumer *streamCreateConsumer(streamCG *cg, sds name, robj *key, int dbid, int flags);
streamCG *streamCreateCG(stream *s, char *name, size_t namelen, streamID *id, long long entries_read);
streamNACK *streamCreateNACK(streamConsumer *consumer);
void streamDecodeID(void *buf, streamID *id);
int streamCompareID(streamID *a, streamID *b);
void streamFreeNACK(streamNACK *na);
int streamIncrID(streamID *id);
int streamDecrID(streamID *id);
void streamPropagateConsumerCreation(client *c, robj *key, robj *groupname, sds consumername);
robj *streamDup(robj *o);
int streamValidateListpackIntegrity(unsigned char *lp, size_t size, int deep);
int streamParseID(const robj *o, streamID *id);
robj *createObjectFromStreamID(streamID *id);
int streamAppendItem(stream *s, robj **argv, int64_t numfields, streamID *added_id, streamID *use_id, int seq_given);
int streamDeleteItem(stream *s, streamID *id);
void streamGetEdgeID(stream *s, int first, int skip_tombstones, streamID *edge_id);
long long streamEstimateDistanceFromFirstEverEntry(stream *s, streamID *id);
int64_t streamTrimByLength(stream *s, long long maxlen, int approx);
int64_t streamTrimByID(stream *s, streamID minid, int approx);

#endif
