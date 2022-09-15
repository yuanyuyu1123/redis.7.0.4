#ifndef __CLUSTER_H
#define __CLUSTER_H

/*-----------------------------------------------------------------------------
 * Redis cluster data structures, defines, exported API.
 *----------------------------------------------------------------------------*/

#define CLUSTER_SLOTS 16384
#define CLUSTER_OK 0            /* Everything looks ok 一切看起来都不错*/
#define CLUSTER_FAIL 1          /* The cluster can't work */
#define CLUSTER_NAMELEN 40      /* sha1 hex length */
#define CLUSTER_PORT_INCR 10000 /* Cluster port = baseport + PORT_INCR */

/* The following defines are amount of time, sometimes expressed as
 * multiplicators of the node timeout value (when ending with MULT). */
//以下定义是时间量，有时表示为节点超时值的乘数（以 MULT 结尾时）。
#define CLUSTER_FAIL_REPORT_VALIDITY_MULT 2 /* Fail report validity. 失败报告有效性。*/
#define CLUSTER_FAIL_UNDO_TIME_MULT 2 /* Undo fail if master is back. 如果master回来了，撤消失败。*/
#define CLUSTER_MF_TIMEOUT 5000 /* Milliseconds to do a manual failover. 执行手动故障转移的毫秒数。*/
#define CLUSTER_MF_PAUSE_MULT 2 /* Master pause manual failover mult. master暂停手动故障转移多。*/
#define CLUSTER_SLAVE_MIGRATION_DELAY 5000 /* Delay for slave migration. 从属迁移延迟。*/

/* Redirection errors returned by getNodeByQuery(). */
//getNodeByQuery() 返回的重定向错误。
#define CLUSTER_REDIR_NONE 0          /* Node can serve the request. 节点可以服务请求。*/
#define CLUSTER_REDIR_CROSS_SLOT 1    /* -CROSSSLOT request. */
#define CLUSTER_REDIR_UNSTABLE 2      /* -TRYAGAIN redirection required */
#define CLUSTER_REDIR_ASK 3           /* -ASK redirection required. */
#define CLUSTER_REDIR_MOVED 4         /* -MOVED redirection required. -MOVED 需要重定向。*/
#define CLUSTER_REDIR_DOWN_STATE 5    /* -CLUSTERDOWN, global state. */
#define CLUSTER_REDIR_DOWN_UNBOUND 6  /* -CLUSTERDOWN, unbound slot. */
#define CLUSTER_REDIR_DOWN_RO_STATE 7 /* -CLUSTERDOWN, allow reads. */

struct clusterNode;

/* clusterLink encapsulates everything needed to talk with a remote node. */
//clusterLink 封装了与远程节点通信所需的一切。
typedef struct clusterLink {
    mstime_t ctime;             /* Link creation time 链接创建时间*/
    connection *conn;           /* Connection to remote node 连接到远程节点*/
    sds sndbuf;                 /* Packet send buffer 数据包发送缓冲区*/
    char *rcvbuf;               /* Packet reception buffer 数据包接收缓冲区*/
    size_t rcvbuf_len;          /* Used size of rcvbuf 使用的 rcvbuf 大小*/
    size_t rcvbuf_alloc;        /* Allocated size of rcvbuf 分配的rcvbuf大小*/
    struct clusterNode *node;   /* Node related to this link. Initialized to NULL when unknown
 *                                 与此链接相关的节点。未知时初始化为NULL*/
    int inbound;                /* 1 if this link is an inbound link accepted from the related node
 *                                 如果此链接是从相关节点接受的入站链接，则为 1*/
} clusterLink;

/* Cluster node flags and macros. */
//集群节点标志和宏。
#define CLUSTER_NODE_MASTER 1     /* The node is a master */
#define CLUSTER_NODE_SLAVE 2      /* The node is a slave */
#define CLUSTER_NODE_PFAIL 4      /* Failure? Need acknowledge */
#define CLUSTER_NODE_FAIL 8       /* The node is believed to be malfunctioning 该节点被认为出现故障*/
#define CLUSTER_NODE_MYSELF 16    /* This node is myself */
#define CLUSTER_NODE_HANDSHAKE 32 /* We have still to exchange the first ping 我们仍然需要交换第一个 ping*/
#define CLUSTER_NODE_NOADDR   64  /* We don't know the address of this node 我们不知道这个节点的地址*/
#define CLUSTER_NODE_MEET 128     /* Send a MEET message to this node 向该节点发送 MEET 消息*/
#define CLUSTER_NODE_MIGRATE_TO 256 /* Master eligible for replica migration. Master 有资格进行副本迁移。*/
#define CLUSTER_NODE_NOFAILOVER 512 /* Slave will not try to failover. 从站不会尝试故障转移。*/
#define CLUSTER_NODE_NULL_NAME "\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000"

#define nodeIsMaster(n) ((n)->flags & CLUSTER_NODE_MASTER)
#define nodeIsSlave(n) ((n)->flags & CLUSTER_NODE_SLAVE)
#define nodeInHandshake(n) ((n)->flags & CLUSTER_NODE_HANDSHAKE)
#define nodeHasAddr(n) (!((n)->flags & CLUSTER_NODE_NOADDR))
#define nodeWithoutAddr(n) ((n)->flags & CLUSTER_NODE_NOADDR)
#define nodeTimedOut(n) ((n)->flags & CLUSTER_NODE_PFAIL)
#define nodeFailed(n) ((n)->flags & CLUSTER_NODE_FAIL)
#define nodeCantFailover(n) ((n)->flags & CLUSTER_NODE_NOFAILOVER)

/* Reasons why a slave is not able to failover. 从站无法进行故障转移的原因。*/
#define CLUSTER_CANT_FAILOVER_NONE 0
#define CLUSTER_CANT_FAILOVER_DATA_AGE 1
#define CLUSTER_CANT_FAILOVER_WAITING_DELAY 2
#define CLUSTER_CANT_FAILOVER_EXPIRED 3
#define CLUSTER_CANT_FAILOVER_WAITING_VOTES 4
#define CLUSTER_CANT_FAILOVER_RELOG_PERIOD (60*5) /* seconds. */

/* clusterState todo_before_sleep flags. clusterState todo_before_sleep 标志。*/
#define CLUSTER_TODO_HANDLE_FAILOVER (1<<0)
#define CLUSTER_TODO_UPDATE_STATE (1<<1)
#define CLUSTER_TODO_SAVE_CONFIG (1<<2)
#define CLUSTER_TODO_FSYNC_CONFIG (1<<3)
#define CLUSTER_TODO_HANDLE_MANUALFAILOVER (1<<4)

/* Message types.
 *
 * Note that the PING, PONG and MEET messages are actually the same exact
 * kind of packet. PONG is the reply to ping, in the exact format as a PING,
 * while MEET is a special PING that forces the receiver to add the sender
 * as a node (if it is not already in the list). */
/**
 * 消息类型。请注意，PING、PONG 和 MEET 消息实际上是完全相同的数据包类型。
 * PONG 是对 ping 的回复，格式与 PING 完全相同，而 MEET 是一种特殊的 PING，它强制接收者将发送者添加为节点（如果它不在列表中）。
 * */
#define CLUSTERMSG_TYPE_PING 0          /* Ping */
#define CLUSTERMSG_TYPE_PONG 1          /* Pong (reply to Ping) */
#define CLUSTERMSG_TYPE_MEET 2          /* Meet "let's join" message */
#define CLUSTERMSG_TYPE_FAIL 3          /* Mark node xxx as failing */
#define CLUSTERMSG_TYPE_PUBLISH 4       /* Pub/Sub Publish propagation */
#define CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST 5 /* May I failover? */
#define CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK 6     /* Yes, you have my vote */
#define CLUSTERMSG_TYPE_UPDATE 7        /* Another node slots configuration */
#define CLUSTERMSG_TYPE_MFSTART 8       /* Pause clients for manual failover */
#define CLUSTERMSG_TYPE_MODULE 9        /* Module cluster API message. */
#define CLUSTERMSG_TYPE_PUBLISHSHARD 10 /* Pub/Sub Publish shard propagation */
#define CLUSTERMSG_TYPE_COUNT 11        /* Total number of message types. */

/* Flags that a module can set in order to prevent certain Redis Cluster
 * features to be enabled. Useful when implementing a different distributed
 * system on top of Redis Cluster message bus, using modules. */
//模块可以设置的标志，以防止启用某些 Redis 集群功能。在使用模块在 Redis 集群消息总线上实现不同的分布式系统时很有用。
#define CLUSTER_MODULE_FLAG_NONE 0
#define CLUSTER_MODULE_FLAG_NO_FAILOVER (1<<1)
#define CLUSTER_MODULE_FLAG_NO_REDIRECTION (1<<2)

/* This structure represent elements of node->fail_reports. */
//此结构表示 node->fail_reports 的元素。
typedef struct clusterNodeFailReport {
    struct clusterNode *node;  /* Node reporting the failure condition. 节点报告故障情况。*/
    mstime_t time;             /* Time of the last report from this node. 此节点的最后一次报告的时间。*/
} clusterNodeFailReport;

typedef struct clusterNode {
    mstime_t ctime; /* Node object creation time. */
    char name[CLUSTER_NAMELEN]; /* Node name, hex string, sha1-size */
    int flags;      /* CLUSTER_NODE_... */
    uint64_t configEpoch; /* Last configEpoch observed for this node 观察到此节点的最后一个 configEpoch*/
    unsigned char slots[CLUSTER_SLOTS/8]; /* slots handled by this node 此节点处理的槽*/
    uint16_t *slot_info_pairs; /* Slots info represented as (start/end) pair (consecutive index). 插槽信息表示为 (startend) 对（连续索引）。*/
    int slot_info_pairs_count; /* Used number of slots in slot_info_pairs slot_info_pairs 中已使用的槽数*/
    int numslots;   /* Number of slots handled by this node 此节点处理的槽数*/
    int numslaves;  /* Number of slave nodes, if this is a master 从节点的数量，如果这是主节点*/
    struct clusterNode **slaves; /* pointers to slave nodes */
    struct clusterNode *slaveof; /* pointer to the master node. Note that it
                                    may be NULL even if the node is a slave
                                    if we don't have the master node in our
                                    tables.指向主节点的指针。请注意，如果我们的表中没有主节点，
                                    即使该节点是从节点，它也可能为 NULL。 */
    unsigned long long last_in_ping_gossip; /* The number of the last carried in the ping gossip section
 *                                              ping八卦部分最后携带的号码*/
    mstime_t ping_sent;      /* Unix time we sent latest ping 我们发送最新 ping 的 Unix 时间*/
    mstime_t pong_received;  /* Unix time we received the pong */
    mstime_t data_received;  /* Unix time we received any data */
    mstime_t fail_time;      /* Unix time when FAIL flag was set 设置 FAIL 标志时的 Unix 时间*/
    mstime_t voted_time;     /* Last time we voted for a slave of this master 上次我们投票给这个master的slave*/
    mstime_t repl_offset_time;  /* Unix time we received offset for this node 我们收到此节点的偏移量的 Unix 时间*/
    mstime_t orphaned_time;     /* Starting time of orphaned master condition 孤立master条件的开始时间*/
    long long repl_offset;      /* Last known repl offset for this node. 此节点的最后一个已知 repl 偏移量。*/
    char ip[NET_IP_STR_LEN];    /* Latest known IP address of this node 此节点的最新已知 IP 地址*/
    sds hostname;               /* The known hostname for this node 此节点的已知主机名*/
    int port;                   /* Latest known clients port (TLS or plain). 最新的已知客户端端口（TLS 或普通）。*/
    int pport;                  /* Latest known clients plaintext port. Only used
                                   if the main clients port is for TLS.
                                   最新的已知客户端明文端口。仅在主客户端端口用于 TLS 时使用。*/
    int cport;                  /* Latest known cluster port of this node. 此节点的最新已知集群端口。*/
    clusterLink *link;          /* TCP/IP link established toward this node 建立到该节点的 TCP/IP 链接*/
    clusterLink *inbound_link;  /* TCP/IP link accepted from this node 从该节点接受的 TCP/IP 链接*/
    list *fail_reports;         /* List of nodes signaling this as failing  表明此失败的节点列表*/
} clusterNode;

/* Slot to keys for a single slot. The keys in the same slot are linked together
 * using dictEntry metadata. */
//单个插槽的插槽到键。同一槽中的键使用 dictEntry 元数据链接在一起。
typedef struct slotToKeys {
    uint64_t count;             /* Number of keys in the slot. 插槽中的键数。*/
    dictEntry *head;            /* The first key-value entry in the slot. 槽中的第一个键值条目。*/
} slotToKeys;

/* Slot to keys mapping for all slots, opaque outside this file. */
//所有插槽的插槽到键映射，在此文件之外不透明。
struct clusterSlotToKeyMapping {
    slotToKeys by_slot[CLUSTER_SLOTS];
};

/* Dict entry metadata for cluster mode, used for the Slot to Key API to form a
 * linked list of the entries belonging to the same slot. */
//集群模式的字典条目元数据，用于 Slot to Key API 以形成属于同一插槽的条目的链表。
typedef struct clusterDictEntryMetadata {
    dictEntry *prev;            /* Prev entry with key in the same slot 在同一个插槽中使用密钥的上一个条目*/
    dictEntry *next;            /* Next entry with key in the same slot 在同一个插槽中使用密钥的下一个条目*/
} clusterDictEntryMetadata;


typedef struct clusterState {
    clusterNode *myself;  /* This node */
    uint64_t currentEpoch;
    int state;            /* CLUSTER_OK, CLUSTER_FAIL, ... */
    int size;             /* Num of master nodes with at least one slot 至少有一个槽的主节点数*/
    dict *nodes;          /* Hash table of name -> clusterNode structures */
    dict *nodes_black_list; /* Nodes we don't re-add for a few seconds. 我们不会在几秒钟内重新添加节点。*/
    clusterNode *migrating_slots_to[CLUSTER_SLOTS];
    clusterNode *importing_slots_from[CLUSTER_SLOTS];
    clusterNode *slots[CLUSTER_SLOTS];
    rax *slots_to_channels;
    /* The following fields are used to take the slave state on elections. */
    //以下字段用于在选举中采用从属状态。
    mstime_t failover_auth_time; /* Time of previous or next election. 上一次或下一次选举的时间。*/
    int failover_auth_count;    /* Number of votes received so far. 到目前为止收到的票数。*/
    int failover_auth_sent;     /* True if we already asked for votes. 如果我们已经要求投票，则为真。*/
    int failover_auth_rank;     /* This slave rank for current auth request. 当前身份验证请求的此从属级别。*/
    uint64_t failover_auth_epoch; /* Epoch of the current election. 当前选举的时代。*/
    int cant_failover_reason;   /* Why a slave is currently not able to
                                   failover. See the CANT_FAILOVER_* macros.
                                   为什么从站当前无法进行故障转移。请参阅 CANT_FAILOVER_ 宏。*/
    /* Manual failover state in common. 常见的手动故障转移状态。*/
    mstime_t mf_end;            /* Manual failover time limit (ms unixtime).
                                   It is zero if there is no MF in progress.
                                   手动故障转移时间限制（毫秒 unixtime）。如果没有 MF 正在进行，则为零。*/
    /* Manual failover state of master. master 的手动故障转移状态。*/
    clusterNode *mf_slave;      /* Slave performing the manual failover. 从设备执行手动故障转移。*/
    /* Manual failover state of slave. 从站的手动故障转移状态。*/
    long long mf_master_offset; /* Master offset the slave needs to start MF
                                   or -1 if still not received.
                                   主偏移从需要启动 MF 或 -1 如果仍未收到。*/
    int mf_can_start;           /* If non-zero signal that the manual failover
                                   can start requesting masters vote.
                                   如果非零信号表明手动故障转移可以开始请求主节点投票。*/
    /* The following fields are used by masters to take state on elections. */
    //主人使用以下字段来获取选举状态。
    uint64_t lastVoteEpoch;     /* Epoch of the last vote granted. 最后一次投票的时代。*/
    int todo_before_sleep; /* Things to do in clusterBeforeSleep(). 在 clusterBeforeSleep() 中要做的事情。*/
    /* Stats */
    /* Messages received and sent by type. */
    //按类型接收和发送的消息。
    long long stats_bus_messages_sent[CLUSTERMSG_TYPE_COUNT];
    long long stats_bus_messages_received[CLUSTERMSG_TYPE_COUNT];
    long long stats_pfail_nodes;    /* Number of nodes in PFAIL status,
                                       excluding nodes without address.
                                       处于 PFAIL 状态的节点数，不包括没有地址的节点。*/
    unsigned long long stat_cluster_links_buffer_limit_exceeded;  /* Total number of cluster links freed
 *                                                                   due to exceeding buffer limit
 *                                                                   由于超出缓冲区限制而释放的集群链接总数*/
} clusterState;

/* Redis cluster messages header */

/* Initially we don't know our "name", but we'll find it once we connect
 * to the first node, using the getsockname() function. Then we'll use this
 * address for all the next messages. */
/**
 * Redis 集群消息头
 * 最初我们不知道我们的“名称”，但是一旦我们连接到第一个节点，我们就会使用 getsockname() 函数找到它。
 * 然后我们将使用这个地址来接收所有下一条消息。
 * */
typedef struct {
    char nodename[CLUSTER_NAMELEN];
    uint32_t ping_sent;
    uint32_t pong_received;
    char ip[NET_IP_STR_LEN];  /* IP address last time it was seen 上次看到的 IP 地址*/
    uint16_t port;              /* base port last time it was seen 上次看到的基本端口*/
    uint16_t cport;             /* cluster port last time it was seen 上次看到的集群端口*/
    uint16_t flags;             /* node->flags copy */
    uint16_t pport;             /* plaintext-port, when base port is TLS 明文端口，当基本端口为 TLS 时*/
    uint16_t notused1;
} clusterMsgDataGossip;

typedef struct {
    char nodename[CLUSTER_NAMELEN];
} clusterMsgDataFail;

typedef struct {
    uint32_t channel_len;
    uint32_t message_len;
    unsigned char bulk_data[8]; /* 8 bytes just as placeholder. 8 个字节作为占位符。*/
} clusterMsgDataPublish;

typedef struct {
    uint64_t configEpoch; /* Config epoch of the specified instance. 配置指定实例的纪元。*/
    char nodename[CLUSTER_NAMELEN]; /* Name of the slots owner. 插槽所有者的姓名。*/
    unsigned char slots[CLUSTER_SLOTS/8]; /* Slots bitmap. */
} clusterMsgDataUpdate;

typedef struct {
    uint64_t module_id;     /* ID of the sender module. 发送模块的 ID。*/
    uint32_t len;           /* ID of the sender module. */
    uint8_t type;           /* Type from 0 to 255. */
    unsigned char bulk_data[3]; /* 3 bytes just as placeholder. 3 个字节作为占位符。*/
} clusterMsgModule;

/* The cluster supports optional extension messages that can be sent
 * along with ping/pong/meet messages to give additional info in a 
 * consistent manner. */
//集群支持可选的扩展消息，可以与 pingpongmeet 消息一起发送，以一致的方式提供附加信息。
typedef enum {
    CLUSTERMSG_EXT_TYPE_HOSTNAME,
} clusterMsgPingtypes; 

/* Helper function for making sure extensions are eight byte aligned. */
//用于确保扩展是八字节对齐的辅助函数。
#define EIGHT_BYTE_ALIGN(size) ((((size) + 7) / 8) * 8)

typedef struct {
    char hostname[1]; /* The announced hostname, ends with \0. 宣布的主机名以 \0 结尾。 */
} clusterMsgPingExtHostname;

typedef struct {
    uint32_t length; /* Total length of this extension message (including this header) 此扩展消息的总长度（包括此标头）*/
    uint16_t type; /* Type of this extension message (see clusterMsgPingExtTypes) 此扩展消息的类型（请参阅 clusterMsgPingExtTypes） */
    uint16_t unused; /* 16 bits of padding to make this structure 8 byte aligned. 16 位填充使该结构 8 字节对齐。*/
    union {
        clusterMsgPingExtHostname hostname;
    } ext[]; /* Actual extension information, formatted so that the data is 8 
              * byte aligned, regardless of its content.
              * 实际的扩展信息，格式化后数据以 8 字节对齐，无论其内容如何。*/
} clusterMsgPingExt;

union clusterMsgData {
    /* PING, MEET and PONG */
    struct {
        /* Array of N clusterMsgDataGossip structures */
        //N 个 clusterMsgDataGossip 结构的数组
        clusterMsgDataGossip gossip[1];
        /* Extension data that can optionally be sent for ping/meet/pong
         * messages. We can't explicitly define them here though, since
         * the gossip array isn't the real length of the gossip data.
         * 可以选择为 pingmeetpong 消息发送的扩展数据。不过，我们不能在这里明确定义它们，
         * 因为 gossip 数组不是 gossip 数据的真实长度。*/
    } ping;

    /* FAIL */
    struct {
        clusterMsgDataFail about;
    } fail;

    /* PUBLISH */
    struct {
        clusterMsgDataPublish msg;
    } publish;

    /* UPDATE */
    struct {
        clusterMsgDataUpdate nodecfg;
    } update;

    /* MODULE */
    struct {
        clusterMsgModule msg;
    } module;
};

#define CLUSTER_PROTO_VER 1 /* Cluster bus protocol version. 集群总线协议版本。*/

typedef struct {
    char sig[4];        /* Signature "RCmb" (Redis Cluster message bus). 签名“RCmb”（Redis 集群消息总线）。*/
    uint32_t totlen;    /* Total length of this message 此消息的总长度*/
    uint16_t ver;       /* Protocol version, currently set to 1. 协议版本，当前设置为 1。*/
    uint16_t port;      /* TCP base port number. */
    uint16_t type;      /* Message type */
    uint16_t count;     /* Only used for some kind of messages. 仅用于某种消息。*/
    uint64_t currentEpoch;  /* The epoch accordingly to the sending node. 对应发送节点的纪元。*/
    uint64_t configEpoch;   /* The config epoch if it's a master, or the last
                               epoch advertised by its master if it is a
                               slave. 如果它是主服务器，则配置 epoch，或者如果它是从服务器，
                               则它的主服务器发布的最后一个 epoch。*/
    uint64_t offset;    /* Master replication offset if node is a master or
                           processed replication offset if node is a slave.
                           如果节点是主节点，则主复制偏移量；如果节点是从节点，则处理复制偏移量。*/
    char sender[CLUSTER_NAMELEN]; /* Name of the sender node 发送者节点的名称*/
    unsigned char myslots[CLUSTER_SLOTS/8];
    char slaveof[CLUSTER_NAMELEN];
    char myip[NET_IP_STR_LEN];    /* Sender IP, if not all zeroed. 发件人 IP，如果不是全部归零。*/
    uint16_t extensions; /* Number of extensions sent along with this packet. 与此数据包一起发送的扩展数。*/
    char notused1[30];   /* 30 bytes reserved for future usage. 保留 30 个字节以供将来使用。*/
    uint16_t pport;      /* Sender TCP plaintext port, if base port is TLS 发送方 TCP 明文端口，如果基础端口是 TLS*/
    uint16_t cport;      /* Sender TCP cluster bus port 发送方 TCP 集群总线端口*/
    uint16_t flags;      /* Sender node flags 发件人节点标志*/
    unsigned char state; /* Cluster state from the POV of the sender 来自发送者 POV 的集群状态*/
    unsigned char mflags[3]; /* Message flags: CLUSTERMSG_FLAG[012]_... 消息标志：CLUSTERMSG_FLAG[012]_...*/
    union clusterMsgData data;
} clusterMsg;

/* clusterMsg defines the gossip wire protocol exchanged among Redis cluster
 * members, which can be running different versions of redis-server bits,
 * especially during cluster rolling upgrades.
 *
 * Therefore, fields in this struct should remain at the same offset from
 * release to release. The static asserts below ensures that incompatible
 * changes in clusterMsg be caught at compile time.
 */
/**
 * clusterMsg 定义了 Redis 集群成员之间交换的 gossip 有线协议，
 * 可以运行不同版本的 redis-server 位，尤其是在集群滚动升级期间。
 * 因此，此结构中的字段应在不同版本之间保持相同的偏移量。
 * 下面的静态断言可确保在编译时捕获 clusterMsg 中的不兼容更改。
 * */

static_assert(offsetof(clusterMsg, sig) == 0, "unexpected field offset");
static_assert(offsetof(clusterMsg, totlen) == 4, "unexpected field offset");
static_assert(offsetof(clusterMsg, ver) == 8, "unexpected field offset");
static_assert(offsetof(clusterMsg, port) == 10, "unexpected field offset");
static_assert(offsetof(clusterMsg, type) == 12, "unexpected field offset");
static_assert(offsetof(clusterMsg, count) == 14, "unexpected field offset");
static_assert(offsetof(clusterMsg, currentEpoch) == 16, "unexpected field offset");
static_assert(offsetof(clusterMsg, configEpoch) == 24, "unexpected field offset");
static_assert(offsetof(clusterMsg, offset) == 32, "unexpected field offset");
static_assert(offsetof(clusterMsg, sender) == 40, "unexpected field offset");
static_assert(offsetof(clusterMsg, myslots) == 80, "unexpected field offset");
static_assert(offsetof(clusterMsg, slaveof) == 2128, "unexpected field offset");
static_assert(offsetof(clusterMsg, myip) == 2168, "unexpected field offset");
static_assert(offsetof(clusterMsg, extensions) == 2214, "unexpected field offset");
static_assert(offsetof(clusterMsg, notused1) == 2216, "unexpected field offset");
static_assert(offsetof(clusterMsg, pport) == 2246, "unexpected field offset");
static_assert(offsetof(clusterMsg, cport) == 2248, "unexpected field offset");
static_assert(offsetof(clusterMsg, flags) == 2250, "unexpected field offset");
static_assert(offsetof(clusterMsg, state) == 2252, "unexpected field offset");
static_assert(offsetof(clusterMsg, mflags) == 2253, "unexpected field offset");
static_assert(offsetof(clusterMsg, data) == 2256, "unexpected field offset");

#define CLUSTERMSG_MIN_LEN (sizeof(clusterMsg)-sizeof(union clusterMsgData))

/* Message flags better specify the packet content or are used to
 * provide some information about the node state. */
//消息标志更好地指定数据包内容或用于提供有关节点状态的一些信息。
#define CLUSTERMSG_FLAG0_PAUSED (1<<0) /* Master paused for manual failover. Master 暂停以进行手动故障转移。*/
#define CLUSTERMSG_FLAG0_FORCEACK (1<<1) /* Give ACK to AUTH_REQUEST even if
                                            master is up. 即使 master 已启动，也向 AUTH_REQUEST 发送 ACK。*/
#define CLUSTERMSG_FLAG0_EXT_DATA (1<<2) /* Message contains extension data 消息包含扩展数据*/

/* ---------------------- API exported outside cluster.c -------------------- */
void clusterInit(void);
void clusterCron(void);
void clusterBeforeSleep(void);
clusterNode *getNodeByQuery(client *c, struct redisCommand *cmd, robj **argv, int argc, int *hashslot, int *ask);
int verifyClusterNodeId(const char *name, int length);
clusterNode *clusterLookupNode(const char *name, int length);
int clusterRedirectBlockedClientIfNeeded(client *c);
void clusterRedirectClient(client *c, clusterNode *n, int hashslot, int error_code);
void migrateCloseTimedoutSockets(void);
int verifyClusterConfigWithData(void);
unsigned long getClusterConnectionsCount(void);
int clusterSendModuleMessageToTarget(const char *target, uint64_t module_id, uint8_t type, const char *payload, uint32_t len);
void clusterPropagatePublish(robj *channel, robj *message, int sharded);
unsigned int keyHashSlot(char *key, int keylen);
void slotToKeyAddEntry(dictEntry *entry, redisDb *db);
void slotToKeyDelEntry(dictEntry *entry, redisDb *db);
void slotToKeyReplaceEntry(dictEntry *entry, redisDb *db);
void slotToKeyInit(redisDb *db);
void slotToKeyFlush(redisDb *db);
void slotToKeyDestroy(redisDb *db);
void clusterUpdateMyselfFlags(void);
void clusterUpdateMyselfIp(void);
void slotToChannelAdd(sds channel);
void slotToChannelDel(sds channel);
void clusterUpdateMyselfHostname(void);

#endif /* __CLUSTER_H */
