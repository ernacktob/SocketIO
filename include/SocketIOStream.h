#ifndef SOCKETIOSTREAM_H
#define SOCKETIOSTREAM_H

#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/errno.h>

typedef void *SocketIOStreamConnection_t;
typedef void (*SocketIOStreamConnection_done_cb)(SocketIOStreamConnection_t conn, void *arg);
typedef void (*SocketIOStreamConnection_error_cb)(SocketIOStreamConnection_t conn, void *arg, int err);
typedef void (*SocketIOStreamConnection_sniff_cb)(SocketIOStreamConnection_t conn, void *arg, const uint8_t *data, size_t len);
typedef int (*SocketIOStreamConnection_until_cb)(const uint8_t *data, size_t len);
typedef void (*SocketIOStreamConnection_until_done_cb)(SocketIOStreamConnection_t conn, void *arg, uint8_t *data, size_t len);

int SocketIOStreamConnection_send(SocketIOStreamConnection_t conn, const uint8_t *data, size_t len, SocketIOStreamConnection_done_cb send_done_cb,
		SocketIOStreamConnection_error_cb error_cb, void *arg);
int SocketIOStreamConnection_recv(SocketIOStreamConnection_t conn, uint8_t *data, size_t len, SocketIOStreamConnection_done_cb recv_done_cb,
		SocketIOStreamConnection_error_cb error_cb, SocketIOStreamConnection_sniff_cb sniff_cb, void *arg);
int SocketIOStreamConnection_recvuntil(SocketIOStreamConnection_t conn, SocketIOStreamConnection_until_cb until_cb, SocketIOStreamConnection_until_done_cb until_done_cb,
		SocketIOStreamConnection_error_cb error_cb, SocketIOStreamConnection_sniff_cb sniff_cb, void *arg);

typedef void *SocketIOStreamClient_t;
typedef void (*SocketIOStreamClient_connect_cb)(SocketIOStreamConnection_t conn, void *arg);

int SocketIOStreamClient_create(int domain, int protocol, SocketIOStreamClient_t *client);
int SocketIOStreamClient_connect(SocketIOStreamClient_t client, const struct sockaddr *addr, socklen_t addrlen,
		SocketIOStreamClient_connect_cb connect_cb, SocketIOStreamConnection_error_cb error_cb, void *arg);
int SocketIOStreamClient_block(SocketIOStreamClient_t client);
void SocketIOStreamClient_close(SocketIOStreamClient_t client);

typedef void *SocketIOStreamServer_t;
typedef void (*SocketIOStreamServer_accept_cb)(SocketIOStreamConnection_t conn, const struct sockaddr *addr, socklen_t addrlen);

int SocketIOStreamServer_create(const struct sockaddr *addr, socklen_t addrlen, int domain, int protocol, int backlog, SocketIOStreamServer_t *server);
int SocketIOStreamServer_run(SocketIOStreamServer_t server, SocketIOStreamServer_accept_cb accept_cb);
int SocketIOStreamServer_block(SocketIOStreamServer_t server);
void SocketIOStreamServer_stop(SocketIOStreamServer_t server);
void SocketIOStreamServer_close(SocketIOStreamServer_t server);

#endif
