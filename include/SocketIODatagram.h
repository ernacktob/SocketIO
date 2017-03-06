#ifndef SOCKETIODATAGRAM_H
#define SOCKETIODATAGRAM_H

#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>

typedef void *SocketIODatagramEndpoint_t;
typedef void *SocketIODatagram_continue_t;

typedef void (*SocketIODatagramEndpoint_dgram_recvd_cb)(SocketIODatagramEndpoint_t endpoint, void *arg, uint8_t *data, size_t len, const struct sockaddr *s_addr, socklen_t s_addrlen, SocketIODatagram_continue_t continued);
typedef void (*SocketIODatagramEndpoint_dgram_sent_cb)(SocketIODatagramEndpoint_t endpoint, void *arg);
typedef void (*SocketIODatagramEndpoint_dgram_error_cb)(SocketIODatagramEndpoint_t endpoint, void *arg, int err);

int SocketIODatagramEndpoint_create(int domain, int type, int protocol, const struct sockaddr *bind_addr, socklen_t bind_addrlen, SocketIODatagramEndpoint_t *endpoint);

int SocketIODatagramEndpoint_listen(SocketIODatagramEndpoint_t endpoint, SocketIODatagramEndpoint_dgram_recvd_cb dgram_recvd_cb,
		SocketIODatagramEndpoint_dgram_error_cb dgram_error_cb, size_t max_dgram_len, void *arg);
int SocketIODatagramEndpoint_sendto(SocketIODatagramEndpoint_t endpoint, const uint8_t *data, size_t len, const struct sockaddr *to_addr, socklen_t to_addrlen,
		SocketIODatagramEndpoint_dgram_sent_cb dgram_sent_cb, SocketIODatagramEndpoint_dgram_error_cb dgram_error_cb, void *arg);

int SocketIODatagramEndpoint_block(SocketIODatagramEndpoint_t endpoint);
int SocketIODatagramEndpoint_finished(SocketIODatagramEndpoint_t endpoint);
void SocketIODatagramEndpoint_close(SocketIODatagramEndpoint_t endpoint);

void SocketIODatagramEndpoint_continue_listening(SocketIODatagram_continue_t continued);

#endif
