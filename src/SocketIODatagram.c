#include <stdlib.h>
#include <stdint.h>
#include <limits.h>

#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/errno.h>

#include "SocketIODatagram.h"
#include "asyncio.h"
#include "safe_malloc.h"
#include "logging.h"

/* STRUCT DEFINITIONS */
struct SocketIODatagramEndpoint {
	int sockfd;
	int finished;		/* Protected by mtx */
	unsigned int refcount;	/* Protected by mtx */
	pthread_cond_t cond;
	pthread_mutex_t mtx;
};

struct SocketIODatagramEndpointListenCallbackInfo {
	struct SocketIODatagramEndpoint *endpoint;
	size_t max_dgram_len;
	const struct sockaddr *from_addr;
	socklen_t from_addrlen;
	SocketIODatagramEndpoint_dgram_recvd_cb dgram_recvd_cb;
	SocketIODatagramEndpoint_dgram_error_cb dgram_error_cb;
	void *arg;
};

struct SocketIODatagramEndpointSendtoCallbackInfo {
	struct SocketIODatagramEndpoint *endpoint;
	const uint8_t *data;
	size_t len;
	const struct sockaddr *to_addr;
	socklen_t to_addrlen;
	SocketIODatagramEndpoint_dgram_sent_cb dgram_sent_cb;
	SocketIODatagramEndpoint_dgram_error_cb dgram_error_cb;
	void *arg;
};
/* END STRUCT DEFINITIONS */

/* PROTOTYPES */
static int get_socket_errno(int fd);
static int set_nonblocking(int fd);

static int lock_endpoint(struct SocketIODatagramEndpoint *endpoint);
static void unlock_endpoint(struct SocketIODatagramEndpoint *endpoint);

static void handle_readable_event(int fd, asyncio_fdevent_t revents, void *arg, asyncio_continue_t continued);
static void handle_writable_event(int fd, asyncio_fdevent_t revents, void *arg, asyncio_continue_t continued);

static int create_sock(int domain, int type, int protocol, const struct sockaddr *bind_addr, socklen_t bind_addrlen);
/* END PROTOTYPES */

static int get_socket_errno(int fd)
{
	socklen_t len;
	int err = 0;

	len = sizeof err;

	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
		SOCKETIO_SYSERROR("getsockopt");
		return 0;
	}

	return err;
}

static int set_nonblocking(int fd)
{
	int oldflags;

	oldflags = fcntl(fd, F_GETFL);

	if (fcntl(fd, F_SETFL, oldflags | O_NONBLOCK) == -1) {
		SOCKETIO_SYSERROR("fcntl");
		return -1;
	}

	return 0;
}

static int lock_endpoint(struct SocketIODatagramEndpoint *endpoint)
{
	if (pthread_mutex_lock(&endpoint->mtx) != 0) {
		SOCKETIO_SYSERROR("pthread_mutex_lock");
		return -1;
	}

	return 0;
}

static void unlock_endpoint(struct SocketIODatagramEndpoint *endpoint)
{
	if (pthread_mutex_unlock(&endpoint->mtx) != 0)
		SOCKETIO_SYSERROR("pthread_mutex_unlock");
}

static void handle_readable_event(int fd, asyncio_fdevent_t revents, void *arg, asyncio_continue_t continued)
{
	struct SocketIODatagramEndpointListenCallbackInfo *info;
	int continue_receiving = 0;
	struct sockaddr from_addr;
	socklen_t from_addrlen;
	uint8_t *datagram;
	ssize_t rb;
	int err;

	info = (struct SocketIODatagramEndpointListenCallbackInfo *)arg;

	if (revents & ASYNCIO_FDEVENT_ERROR) {
		err = get_socket_errno(fd);

		if (info->dgram_error_cb)
			info->dgram_error_cb(info->endpoint, info->arg, err);

		SocketIODatagramEndpoint_close(info->endpoint);
		safe_free(info);
		return;
	}

	datagram = safe_malloc(info->max_dgram_len);

	if (datagram == NULL) {
		SOCKETIO_ERROR("Failed to allocate memory for received datagram.\n");
		asyncio_continue(continued);
		return;
	}

	from_addrlen = sizeof from_addr;
	rb = recvfrom(fd, datagram, info->max_dgram_len, 0, &from_addr, &from_addrlen);

	if (rb < 0) {
		if (errno == EAGAIN || errno == EINTR) {
			asyncio_continue(continued);
		} else {
			SOCKETIO_SYSERROR("recvfrom");

			if (info->dgram_error_cb)
				info->dgram_error_cb(info->endpoint, info->arg, errno);

			SocketIODatagramEndpoint_close(info->endpoint);
			safe_free(info);
		}

		safe_free(datagram);
		return;
	}

	info->dgram_recvd_cb(info->endpoint, info->arg, datagram, rb, &from_addr, from_addrlen, (SocketIODatagram_continue_t)&continue_receiving);

	if (continue_receiving) {
		asyncio_continue(continued);
	} else {
		SocketIODatagramEndpoint_close(info->endpoint);
		safe_free(info);
	}
}

static void handle_writable_event(int fd, asyncio_fdevent_t revents, void *arg, asyncio_continue_t continued)
{
	struct SocketIODatagramEndpointSendtoCallbackInfo *info;
	ssize_t sb;
	int err;

	info = (struct SocketIODatagramEndpointSendtoCallbackInfo *)arg;

	if (revents & ASYNCIO_FDEVENT_ERROR) {
		err = get_socket_errno(fd);

		if (info->dgram_error_cb)
			info->dgram_error_cb(info->endpoint, info->arg, err);

		SocketIODatagramEndpoint_close(info->endpoint);
		safe_free(info);
		return;
	}

	sb = sendto(fd, info->data, info->len, 0, info->to_addr, info->to_addrlen);

	if (sb < 0) {
		if (errno == EAGAIN || errno == EINTR) {
			asyncio_continue(continued);
		} else {
			SOCKETIO_SYSERROR("sendto");

			if (info->dgram_error_cb)
				info->dgram_error_cb(info->endpoint, info->arg, errno);

			SocketIODatagramEndpoint_close(info->endpoint);
			safe_free(info);
			return;
		}
	}

	if ((size_t)sb != info->len) {
		SOCKETIO_ERROR("Did not sendto the full length.\n");

		if (info->dgram_error_cb)
			info->dgram_error_cb(info->endpoint, info->arg, 0);

		SocketIODatagramEndpoint_close(info->endpoint);
		safe_free(info);
		return;
	}

	info->dgram_sent_cb(info->endpoint, info->arg);

	SocketIODatagramEndpoint_close(info->endpoint);
	safe_free(info);
}

static int create_sock(int domain, int type, int protocol, const struct sockaddr *bind_addr, socklen_t bind_addrlen)
{
	int sockfd;
	int one = 1;

	sockfd = socket(domain, type, protocol);

	if (sockfd == -1) {
		SOCKETIO_SYSERROR("socket");
		return -1;
	}

	if (set_nonblocking(sockfd) != 0) {
		SOCKETIO_ERROR("Failed to set sockfd to nonblocking.\n");
		close(sockfd);
		return -1;
	}

	if (bind_addr != NULL) {
		/* Set REUSEADDR on sockfd to be able to bind to address next time we run immediately. */
		if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0) {
			SOCKETIO_SYSERROR("setsockopt");
			close(sockfd);
			return -1;
		}

		if (bind(sockfd, bind_addr, bind_addrlen) != 0) {
			SOCKETIO_SYSERROR("bind");
			close(sockfd);
			return -1;
		}
	}

	return sockfd;
}

__attribute__((visibility("default")))
int SocketIODatagramEndpoint_create(int domain, int type, int protocol, const struct sockaddr *bind_addr, socklen_t bind_addrlen, SocketIODatagramEndpoint_t *tendpoint)
{
	struct SocketIODatagramEndpoint *endpoint;
	int sockfd;
	pthread_mutex_t mtx;
	pthread_cond_t cond;

	endpoint = safe_malloc(sizeof *endpoint);

	if (endpoint == NULL) {
		SOCKETIO_ERROR("Failed to create datagram endpoint.\n");
		return -1;
	}

	sockfd = create_sock(domain, type, protocol, bind_addr, bind_addrlen);

	if (sockfd == -1) {
		SOCKETIO_ERROR("Failed to create endpoint sock.\n");
		safe_free(endpoint);
		return -1;
	}

	if (pthread_mutex_init(&mtx, NULL) != 0) {
		SOCKETIO_SYSERROR("pthread_mutex_init");
		close(sockfd);
		safe_free(endpoint);
		return -1;
	}

	if (pthread_cond_init(&cond, NULL) != 0) {
		SOCKETIO_SYSERROR("pthread_cond_init");

		if (pthread_mutex_destroy(&mtx) != 0)
			SOCKETIO_SYSERROR("pthread_mutex_destroy");

		close(sockfd);
		safe_free(endpoint);
		return -1;
	}

	endpoint->sockfd = sockfd;
	endpoint->finished = 0;
	endpoint->refcount = 1;
	endpoint->cond = cond;
	endpoint->mtx = mtx;

	*tendpoint = (SocketIODatagramEndpoint_t)endpoint;

	return 0;
}

__attribute__((visibility("default")))
int SocketIODatagramEndpoint_listen(SocketIODatagramEndpoint_t tendpoint, SocketIODatagramEndpoint_dgram_recvd_cb dgram_recvd_cb,
		SocketIODatagramEndpoint_dgram_error_cb dgram_error_cb, size_t max_dgram_len, void *arg)
{
	struct SocketIODatagramEndpoint *endpoint;
	struct SocketIODatagramEndpointListenCallbackInfo *info;
	asyncio_handle_t asyncio_handle;
	endpoint = (struct SocketIODatagramEndpoint *)tendpoint;
	info = safe_malloc(sizeof *info);

	if (info == NULL) {
		SOCKETIO_SYSERROR("Failed to malloc SocketIODatagramEndpointListenCallbackInfo.\n");
		return -1;
	}

	info->endpoint = endpoint;
	info->max_dgram_len = max_dgram_len;
	info->dgram_recvd_cb = dgram_recvd_cb;
	info->dgram_error_cb = dgram_error_cb;
	info->arg = arg;

	if (lock_endpoint(endpoint) != 0) {
		SOCKETIO_ERROR("Failed to lock endpoint.\n");
		safe_free(info);
		return -1;
	}

	/* Check for overflow */
	if (endpoint->refcount == UINT_MAX) {
		SOCKETIO_ERROR("SocketIODatagramEndpoint refcount reached maximum value.\n");
		unlock_endpoint(endpoint);
		safe_free(info);
		return -1;
	}

	++(endpoint->refcount);

	if (asyncio_fdevent(endpoint->sockfd, ASYNCIO_FDEVENT_READ, handle_readable_event, info, ASYNCIO_FLAG_NONE, &asyncio_handle) != 0) {
		SOCKETIO_ERROR("Failed to register fdevent.\n");
		--(endpoint->refcount);
		unlock_endpoint(endpoint);
		safe_free(info);
		return -1;
	}

	unlock_endpoint(endpoint);
	asyncio_release(asyncio_handle);

	return 0;
}

__attribute__((visibility("default")))
int SocketIODatagramEndpoint_sendto(SocketIODatagramEndpoint_t tendpoint, const uint8_t *data, size_t len, const struct sockaddr *to_addr, socklen_t to_addrlen,
		SocketIODatagramEndpoint_dgram_sent_cb dgram_sent_cb, SocketIODatagramEndpoint_dgram_error_cb dgram_error_cb, void *arg)
{
	struct SocketIODatagramEndpoint *endpoint;
	struct SocketIODatagramEndpointSendtoCallbackInfo *info;
	asyncio_handle_t asyncio_handle;

	endpoint = (struct SocketIODatagramEndpoint *)tendpoint;
	info = safe_malloc(sizeof *info);

	if (info == NULL) {
		SOCKETIO_SYSERROR("Failed to malloc SocketIODatagramEndpointSendtoCallbackInfo.\n");
		return -1;
	}

	info->endpoint = endpoint;
	info->data = data;
	info->len = len;
	info->to_addr = to_addr;	/* Note: the address must not be allocated in the stack. If malloc'd, user must free through the send callback, with arg. */
	info->to_addrlen = to_addrlen;
	info->dgram_sent_cb = dgram_sent_cb;
	info->dgram_error_cb = dgram_error_cb;
	info->arg = arg;

	if (lock_endpoint(endpoint) != 0) {
		SOCKETIO_ERROR("Failed to lock endpoint.\n");
		safe_free(info);
		return -1;
	}

	/* Check for overflow */
	if (endpoint->refcount == UINT_MAX) {
		SOCKETIO_ERROR("SocketIODatagramEndpoint refcount reached maximum value.\n");
		unlock_endpoint(endpoint);
		safe_free(info);
		return -1;
	}

	++(endpoint->refcount);

	if (asyncio_fdevent(endpoint->sockfd, ASYNCIO_FDEVENT_WRITE, handle_writable_event, info, ASYNCIO_FLAG_NONE, &asyncio_handle) != 0) {
		SOCKETIO_ERROR("Failed to register fdevent.\n");
		--(endpoint->refcount);
		unlock_endpoint(endpoint);
		safe_free(info);
		return -1;
	}

	unlock_endpoint(endpoint);
	asyncio_release(asyncio_handle);

	return 0;
}

__attribute__((visibility("default")))
int SocketIODatagramEndpoint_block(SocketIODatagramEndpoint_t tendpoint)
{
	struct SocketIODatagramEndpoint *endpoint;

	endpoint = (struct SocketIODatagramEndpoint *)tendpoint;

	if (lock_endpoint(endpoint) != 0) {
		SOCKETIO_ERROR("Failed to lock SocketIODatagramEndpoint.\n");
		return -1;
	}

	while (!(endpoint->finished)) {
		if (pthread_cond_wait(&endpoint->cond, &endpoint->mtx) != 0) {
			SOCKETIO_SYSERROR("pthread_cond_wait");
			unlock_endpoint(endpoint);
			return -1;
		}
	}

	unlock_endpoint(endpoint);
	return 0;
}

__attribute__((visibility("default")))
int SocketIODatagramEndpoint_finished(SocketIODatagramEndpoint_t tendpoint)
{
	struct SocketIODatagramEndpoint *endpoint;

	endpoint = (struct SocketIODatagramEndpoint *)tendpoint;

	if (lock_endpoint(endpoint) != 0) {
		SOCKETIO_ERROR("Failed to lock SocketIODatagramEndpoint.\n");
		return -1;
	}

	endpoint->finished = 1;

	if (pthread_cond_broadcast(&endpoint->cond) != 0) {
		SOCKETIO_SYSERROR("pthread_cond_broadcast");
		unlock_endpoint(endpoint);
		return -1;
	}

	unlock_endpoint(endpoint);
	return 0;
}

__attribute__((visibility("default")))
void SocketIODatagramEndpoint_close(SocketIODatagramEndpoint_t tendpoint)
{
	struct SocketIODatagramEndpoint *endpoint;

	endpoint = (struct SocketIODatagramEndpoint *)tendpoint;

	if (lock_endpoint(endpoint) != 0) {
		SOCKETIO_ERROR("Failed to lock SocketIODatagramEndpoint.\n");
		return;
	}

	if (endpoint->refcount == 0) {
		SOCKETIO_ERROR("Refcount for SocketIODatagramError is 0 before close.\n");
		unlock_endpoint(endpoint);
		return;
	}

	--(endpoint->refcount);

	if (endpoint->refcount == 0) {
		unlock_endpoint(endpoint);

		if (close(endpoint->sockfd) != 0)
			SOCKETIO_SYSERROR("close");

		if (pthread_mutex_destroy(&endpoint->mtx) != 0)
			SOCKETIO_SYSERROR("pthread_mutex_destroy");

		if (pthread_cond_destroy(&endpoint->cond) != 0)
			SOCKETIO_SYSERROR("pthread_cond_destroy");

		safe_free(endpoint);
		return;
	}

	unlock_endpoint(endpoint);
}

__attribute__((visibility("default")))
void SocketIODatagramEndpoint_continue_listening(SocketIODatagram_continue_t tcontinued)
{
	int *continued;

	continued = (int *)tcontinued;
	*continued = 1;
}
