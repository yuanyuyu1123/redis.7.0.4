
/*
 * Copyright (c) 2019, Redis Labs
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

#ifndef __REDIS_CONNECTION_H
#define __REDIS_CONNECTION_H

#include <errno.h>
#include <sys/uio.h>

#define CONN_INFO_LEN   32

struct aeEventLoop;
typedef struct connection connection;

typedef enum {
    CONN_STATE_NONE = 0,
    CONN_STATE_CONNECTING,
    CONN_STATE_ACCEPTING,
    CONN_STATE_CONNECTED,
    CONN_STATE_CLOSED,
    CONN_STATE_ERROR
} ConnectionState;

#define CONN_FLAG_CLOSE_SCHEDULED   (1<<0)      /* Closed scheduled by a handler 由处理程序安排的关闭*/
#define CONN_FLAG_WRITE_BARRIER     (1<<1)      /* Write barrier requested 请求写屏障*/

#define CONN_TYPE_SOCKET            1
#define CONN_TYPE_TLS               2

typedef void (*ConnectionCallbackFunc)(struct connection *conn);

typedef struct ConnectionType {
    void (*ae_handler)(struct aeEventLoop *el, int fd, void *clientData, int mask);
    int (*connect)(struct connection *conn, const char *addr, int port, const char *source_addr, ConnectionCallbackFunc connect_handler);
    int (*write)(struct connection *conn, const void *data, size_t data_len);
    int (*writev)(struct connection *conn, const struct iovec *iov, int iovcnt);
    int (*read)(struct connection *conn, void *buf, size_t buf_len);
    void (*close)(struct connection *conn);
    int (*accept)(struct connection *conn, ConnectionCallbackFunc accept_handler);
    int (*set_write_handler)(struct connection *conn, ConnectionCallbackFunc handler, int barrier);
    int (*set_read_handler)(struct connection *conn, ConnectionCallbackFunc handler);
    const char *(*get_last_error)(struct connection *conn);
    int (*blocking_connect)(struct connection *conn, const char *addr, int port, long long timeout);
    ssize_t (*sync_write)(struct connection *conn, char *ptr, ssize_t size, long long timeout);
    ssize_t (*sync_read)(struct connection *conn, char *ptr, ssize_t size, long long timeout);
    ssize_t (*sync_readline)(struct connection *conn, char *ptr, ssize_t size, long long timeout);
    int (*get_type)(struct connection *conn);
} ConnectionType;

struct connection {
    ConnectionType *type;
    ConnectionState state;
    short int flags;
    short int refs;
    int last_errno;
    void *private_data;
    ConnectionCallbackFunc conn_handler;
    ConnectionCallbackFunc write_handler;
    ConnectionCallbackFunc read_handler;
    int fd;
};

/* The connection module does not deal with listening and accepting sockets,
 * so we assume we have a socket when an incoming connection is created.
 *
 * The fd supplied should therefore be associated with an already accept()ed
 * socket.
 *
 * connAccept() may directly call accept_handler(), or return and call it
 * at a later time. This behavior is a bit awkward but aims to reduce the need
 * to wait for the next event loop, if no additional handshake is required.
 *
 * IMPORTANT: accept_handler may decide to close the connection, calling connClose().
 * To make this safe, the connection is only marked with CONN_FLAG_CLOSE_SCHEDULED
 * in this case, and connAccept() returns with an error.
 *
 * connAccept() callers must always check the return value and on error (C_ERR)
 * a connClose() must be called.
 */
/**
 * 连接模块不处理侦听和接受套接字，因此我们假设在创建传入连接时我们有一个套接字。
 * 因此，提供的 fd 应该与已经接受 () 的套接字相关联。 connAccept() 可以直接调用accept_handler()，也可以稍后返回调用。
 * 这种行为有点尴尬，但旨在减少等待下一个事件循环的需要，如果不需要额外的握手。
 * 重要提示：accept_handler 可能决定关闭连接，调用 connClose()。
 * 为了安全起见，在这种情况下，连接仅用 CONN_FLAG_CLOSE_SCHEDULED 标记，并且 connAccept() 返回错误。
 * connAccept() 调用者必须始终检查返回值，并且在错误 (C_ERR) 时必须调用 connClose()。
 * */

static inline int connAccept(connection *conn, ConnectionCallbackFunc accept_handler) {
    return conn->type->accept(conn, accept_handler);
}

/* Establish a connection.  The connect_handler will be called when the connection
 * is established, or if an error has occurred.
 *
 * The connection handler will be responsible to set up any read/write handlers
 * as needed.
 *
 * If C_ERR is returned, the operation failed and the connection handler shall
 * not be expected.
 */
/**
 * 建立连接。 connect_handler 将在建立连接或发生错误时调用。连接处理程序将负责根据需要设置任何读写处理程序。
 * 如果返回 C_ERR，则操作失败并且不应期待连接处理程序。
 * */
static inline int connConnect(connection *conn, const char *addr, int port, const char *src_addr,
        ConnectionCallbackFunc connect_handler) {
    return conn->type->connect(conn, addr, port, src_addr, connect_handler);
}

/* Blocking connect.
 *
 * NOTE: This is implemented in order to simplify the transition to the abstract
 * connections, but should probably be refactored out of cluster.c and replication.c,
 * in favor of a pure async implementation.
 */
/**
 * 阻塞连接。注意：这是为了简化到抽象连接的转换而实现的，但可能应该从 cluster.c 和 replication.c 中重构出来，以支持纯异步实现。
 * */
static inline int connBlockingConnect(connection *conn, const char *addr, int port, long long timeout) {
    return conn->type->blocking_connect(conn, addr, port, timeout);
}

/* Write to connection, behaves the same as write(2).
 *
 * Like write(2), a short write is possible. A -1 return indicates an error.
 *
 * The caller should NOT rely on errno. Testing for an EAGAIN-like condition, use
 * connGetState() to see if the connection state is still CONN_STATE_CONNECTED.
 */
/**
 * 写入连接，行为与 write(2) 相同。与 write(2) 一样，可以进行短写入。 -1 返回表示错误。
 * 调用者不应依赖 errno。测试类似 EAGAIN 的条件，使用 connGetState() 查看连接状态是否仍为 CONN_STATE_CONNECTED。
 * */
static inline int connWrite(connection *conn, const void *data, size_t data_len) {
    return conn->type->write(conn, data, data_len);
}

/* Gather output data from the iovcnt buffers specified by the members of the iov
 * array: iov[0], iov[1], ..., iov[iovcnt-1] and write to connection, behaves the same as writev(3).
 *
 * Like writev(3), a short write is possible. A -1 return indicates an error.
 *
 * The caller should NOT rely on errno. Testing for an EAGAIN-like condition, use
 * connGetState() to see if the connection state is still CONN_STATE_CONNECTED.
 */
/**
 * 从由 iov 数组成员指定的 iovcnt 缓冲区收集输出数据：iov[0]、iov[1]、...、iov[iovcnt-1] 并写入连接，其行为与 writev(3) 相同。
 * 像 writev(3) 一样，短写是可能的。 -1 返回表示错误。调用者不应依赖 errno。
 * 测试类似 EAGAIN 的条件，使用 connGetState() 查看连接状态是否仍为 CONN_STATE_CONNECTED。
 * */
static inline int connWritev(connection *conn, const struct iovec *iov, int iovcnt) {
    return conn->type->writev(conn, iov, iovcnt);
}

/* Read from the connection, behaves the same as read(2).
 * 
 * Like read(2), a short read is possible.  A return value of 0 will indicate the
 * connection was closed, and -1 will indicate an error.
 *
 * The caller should NOT rely on errno. Testing for an EAGAIN-like condition, use
 * connGetState() to see if the connection state is still CONN_STATE_CONNECTED.
 */
/**
 * 从连接中读取，行为与 read(2) 相同。像 read(2) 一样，短读也是可能的。返回值 0 表示连接已关闭，-1 表示错误。
 * 调用者不应依赖 errno。测试类似 EAGAIN 的条件，使用 connGetState() 查看连接状态是否仍为 CONN_STATE_CONNECTED。
 * */
static inline int connRead(connection *conn, void *buf, size_t buf_len) {
    int ret = conn->type->read(conn, buf, buf_len);
    return ret;
}

/* Register a write handler, to be called when the connection is writable.
 * If NULL, the existing handler is removed.
 */
//注册一个写处理程序，当连接可写时调用。如果为 NULL，则移除现有的处理程序。
static inline int connSetWriteHandler(connection *conn, ConnectionCallbackFunc func) {
    return conn->type->set_write_handler(conn, func, 0);
}

/* Register a read handler, to be called when the connection is readable.
 * If NULL, the existing handler is removed.
 */
//注册一个读取处理程序，在连接可读时调用。如果为 NULL，则移除现有的处理程序。
static inline int connSetReadHandler(connection *conn, ConnectionCallbackFunc func) {
    return conn->type->set_read_handler(conn, func);
}

/* Set a write handler, and possibly enable a write barrier, this flag is
 * cleared when write handler is changed or removed.
 * With barrier enabled, we never fire the event if the read handler already
 * fired in the same event loop iteration. Useful when you want to persist
 * things to disk before sending replies, and want to do that in a group fashion. */
/**
 * 设置写处理程序，并可能启用写屏障，当写处理程序更改或删除时，此标志被清除。
 * 启用屏障后，如果读取处理程序已经在同一个事件循环迭代中触发，我们永远不会触发事件。
 * 当您想在发送回复之前将内容保存到磁盘并希望以组的方式执行此操作时很有用。
 * */
static inline int connSetWriteHandlerWithBarrier(connection *conn, ConnectionCallbackFunc func, int barrier) {
    return conn->type->set_write_handler(conn, func, barrier);
}

static inline void connClose(connection *conn) {
    conn->type->close(conn);
}

/* Returns the last error encountered by the connection, as a string.  If no error,
 * a NULL is returned.
 */
//以字符串形式返回连接遇到的最后一个错误。如果没有错误，则返回 NULL。
static inline const char *connGetLastError(connection *conn) {
    return conn->type->get_last_error(conn);
}

static inline ssize_t connSyncWrite(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return conn->type->sync_write(conn, ptr, size, timeout);
}

static inline ssize_t connSyncRead(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return conn->type->sync_read(conn, ptr, size, timeout);
}

static inline ssize_t connSyncReadLine(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return conn->type->sync_readline(conn, ptr, size, timeout);
}

/* Return CONN_TYPE_* for the specified connection */
//返回指定连接的 CONN_TYPE_
static inline int connGetType(connection *conn) {
    return conn->type->get_type(conn);
}

static inline int connLastErrorRetryable(connection *conn) {
    return conn->last_errno == EINTR;
}

connection *connCreateSocket();
connection *connCreateAcceptedSocket(int fd);

connection *connCreateTLS();
connection *connCreateAcceptedTLS(int fd, int require_auth);

void connSetPrivateData(connection *conn, void *data);
void *connGetPrivateData(connection *conn);
int connGetState(connection *conn);
int connHasWriteHandler(connection *conn);
int connHasReadHandler(connection *conn);
int connGetSocketError(connection *conn);

/* anet-style wrappers to conns */
//anet 样式的包装器到 conns
int connBlock(connection *conn);
int connNonBlock(connection *conn);
int connEnableTcpNoDelay(connection *conn);
int connDisableTcpNoDelay(connection *conn);
int connKeepAlive(connection *conn, int interval);
int connSendTimeout(connection *conn, long long ms);
int connRecvTimeout(connection *conn, long long ms);
int connPeerToString(connection *conn, char *ip, size_t ip_len, int *port);
int connFormatFdAddr(connection *conn, char *buf, size_t buf_len, int fd_to_str_type);
int connSockName(connection *conn, char *ip, size_t ip_len, int *port);
const char *connGetInfo(connection *conn, char *buf, size_t buf_len);

/* Helpers for tls special considerations */
//tls 特殊注意事项的助手
sds connTLSGetPeerCert(connection *conn);
int tlsHasPendingData();
int tlsProcessPendingData();

#endif  /* __REDIS_CONNECTION_H */
