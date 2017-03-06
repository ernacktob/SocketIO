#include <string.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/errno.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>

#include "SocketIOStream.h"
#include "asyncio.h"
#include "queue.h"
#include "safe_malloc.h"
#include "logging.h"

#define SIZET_MAX	((size_t)(-1))	/* Get rid of compiler warning about C99 long long integer constants */

struct SocketIOStreamClient {
	int sockfd;
	SocketIOStreamClient_connect_cb connect_cb;
	SocketIOStreamConnection_error_cb error_cb;
	void *arg;
	struct SocketIOStreamConnection *conn;
	unsigned int refcount;	/* Protected by mtx */
	pthread_mutex_t mtx;
};

struct SocketIOStreamServer {
	int accept_sock;
	asyncio_handle_t asyncio_handle;
	SocketIOStreamServer_accept_cb accept_cb;
	volatile int accepting;	/* Volatile to prevent compiler optimization removing checks for accepting (it gets written in one thread, read in another) */
	unsigned int refcount;	/* Protected by mtx */
	pthread_mutex_t mtx;
};

struct StreamBufferSendRequest {
	const uint8_t *buf;
	size_t len;
	size_t pos;
	SocketIOStreamConnection_done_cb done_cb;
	SocketIOStreamConnection_error_cb error_cb;
	void *arg;

	struct StreamBufferSendRequest *prev;
	struct StreamBufferSendRequest *next;
};

#define DEFAULT_RECV_BUF_SIZE	100

struct StreamBufferRecvRequest {
	int type;
#define RECV_LENGTH_TYPE	0
#define RECV_UNTIL_TYPE		1

/* For convenience */
#define lengthreq		RequestData.LengthRequest
#define untilreq		RequestData.UntilRequest

	union {
		struct {
			uint8_t *buf;
			size_t len;
			size_t pos;
			SocketIOStreamConnection_done_cb done_cb;
		} LengthRequest;

		struct {
			uint8_t *buf;
			size_t mem;
			size_t pos;
			SocketIOStreamConnection_until_cb until_cb;
			SocketIOStreamConnection_until_done_cb until_done_cb;
		} UntilRequest;
	} RequestData;

	SocketIOStreamConnection_error_cb error_cb;
	SocketIOStreamConnection_sniff_cb sniff_cb;
	void *arg;

	struct StreamBufferRecvRequest *prev;
	struct StreamBufferRecvRequest *next;
};

struct SocketIOStreamConnection {
	int sockfd;

	struct StreamBufferSendRequest *sendreq;		/* Protected by mtx */
	int sending;						/* Protected by mtx */

	struct StreamBufferRecvRequest *recvreq;		/* Protected by mtx */
	int recving;						/* Protected by mtx */

	decl_queue(struct StreamBufferSendRequest, sendq);	/* Protected by mtx */
	decl_queue(struct StreamBufferRecvRequest, recvq);	/* Protected by mtx */

	/* Used for SocketiOStreamClient to indicate connection finished */
	int finished;						/* Protected by mtx */
	unsigned int cond_refcount;				/* Protected by mtx */
	pthread_cond_t cond;					/* Protected by mtx */

	unsigned int refcount;					/* Protexted by mtx */
	pthread_mutex_t mtx;
};
/* END STRUCT DEFINITIONS */

/* PROTOTYPES */
static int get_socket_errno(int fd);
static int set_nonblocking(int fd);

static struct SocketIOStreamConnection *SocketIOStreamConnection_create(int sockfd);
static void SocketIOStreamConnection_release(SocketIOStreamConnection_t tconn);

static void handle_stream_recv(int fd, asyncio_fdevent_t revents, void *arg, asyncio_continue_t continued);
static void handle_stream_send(int fd, asyncio_fdevent_t revents, void *arg, asyncio_continue_t continued);
static void handle_stream_accept(int fd, asyncio_fdevent_t revents, void *arg, asyncio_continue_t continued);
static void handle_stream_connect(int fd, asyncio_fdevent_t revents, void *arg, asyncio_continue_t continued);

static int create_accept_sock(const struct sockaddr *addr, socklen_t addrlen, int domain, int protocol, int backlog);
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

static struct SocketIOStreamConnection *SocketIOStreamConnection_create(int sockfd)
{
	struct SocketIOStreamConnection *conn;

	conn = safe_malloc(sizeof *conn);

	if (conn == NULL)
		return NULL;

	if (pthread_mutex_init(&conn->mtx, NULL) != 0) {
		SOCKETIO_SYSERROR("pthread_mutex_init");
		safe_free(conn);
		return NULL;
	}

	if (pthread_cond_init(&conn->cond, NULL) != 0) {
		SOCKETIO_SYSERROR("pthread_cond_init");

		if (pthread_mutex_destroy(&conn->mtx) != 0)
			SOCKETIO_SYSERROR("pthread_mutex_destroy");

		safe_free(conn);
		return NULL;
	}

	conn->sockfd = sockfd;

	conn->sendreq = NULL;
	conn->sending = 0;

	conn->recvreq = NULL;
	conn->recving = 0;

	queue_init(&conn->sendq);
	queue_init(&conn->recvq);

	conn->finished = 0;
	conn->cond_refcount = 0;	/* This only counts the handles held by dispatched threads */
	conn->refcount = 1;

	return conn;
}

static void SocketIOStreamConnection_release(SocketIOStreamConnection_t tconn)
{
	struct SocketIOStreamConnection *conn;
	struct StreamBufferSendRequest *sendnode, *sendnext;
	struct StreamBufferRecvRequest *recvnode, *recvnext;

	conn = (struct SocketIOStreamConnection *)tconn;

	if (pthread_mutex_lock(&conn->mtx) != 0) {
		SOCKETIO_SYSERROR("pthread_mutex_lock");
		return;
	}

	if (conn->cond_refcount == 1) {
		/* We are the last thread that kept connection, so indicate connection is over */
		--conn->cond_refcount;
		conn->finished = 1;

		if (pthread_cond_broadcast(&conn->cond) != 0)
			SOCKETIO_SYSERROR("pthread_cond_broadcast");
	}

	if (conn->refcount == 0) {
		SOCKETIO_ERROR("SocketIOStreamConnection refcount already 0 before close.\n");

		if (pthread_mutex_unlock(&conn->mtx) != 0)
			SOCKETIO_SYSERROR("pthread_mutex_unlock");

		return;
	}

	--conn->refcount;

	if (conn->refcount == 0) {
		if (pthread_mutex_unlock(&conn->mtx) != 0)
			SOCKETIO_SYSERROR("pthread_mutex_unlock");

		if (pthread_mutex_destroy(&conn->mtx) != 0)
			SOCKETIO_SYSERROR("pthread_mutex_destroy");

		if (pthread_cond_destroy(&conn->cond) != 0)
			SOCKETIO_SYSERROR("pthread_cond_destroy");

		queue_foreach(&conn->sendq, sendnode, sendnext) {
			sendnext = sendnode->next;
			safe_free(sendnode);
		}

		queue_foreach(&conn->recvq, recvnode, recvnext) {
			recvnext = recvnode->next;
			safe_free(recvnode);
		}

		if (close(conn->sockfd) != 0)
			SOCKETIO_SYSERROR("close");

		return;
	}

	if (pthread_mutex_unlock(&conn->mtx) != 0)
		SOCKETIO_SYSERROR("pthread_mutex_unlock");
}

static void handle_stream_recv(int fd, asyncio_fdevent_t revents, void *arg, asyncio_continue_t continued)
{
	struct SocketIOStreamConnection *conn;
	struct StreamBufferRecvRequest *recvreq;
	ssize_t rb;
	int done = 0;
	int err;

	conn = (struct SocketIOStreamConnection *)arg;
	recvreq = conn->recvreq;

	if (revents & ASYNCIO_FDEVENT_ERROR) {
		if (recvreq->error_cb) {
			err = get_socket_errno(fd);
			recvreq->error_cb(conn, recvreq->arg, err);
		}

		SocketIOStreamConnection_release(conn);	/* Only release when we don't asyncio_continue */
		return;
	}

	switch (recvreq->type) {
		case RECV_LENGTH_TYPE:
			rb = recv(fd, recvreq->lengthreq.buf + recvreq->lengthreq.pos, recvreq->lengthreq.len - recvreq->lengthreq.pos, 0);
			break;

		case RECV_UNTIL_TYPE:
			rb = recv(fd, recvreq->untilreq.buf + recvreq->untilreq.pos, 1, 0);
			break;

		default:
			SOCKETIO_ERROR("Invalid StreamBufferRecvRequest type.\n");
			break;
	}

	if (rb < 0) {
		if (errno == EINTR || errno == EAGAIN) {
			asyncio_continue(continued);
			return;
		}

		if (recvreq->error_cb)
			recvreq->error_cb(conn, recvreq->arg, errno);

		SocketIOStreamConnection_release(conn);
		return;
	} else if (rb == 0) {
		/* Connection closed */
		if (recvreq->error_cb)
			recvreq->error_cb(conn, recvreq->arg, 0);

		SocketIOStreamConnection_release(conn);
		return;
	}

	switch (recvreq->type) {
		case RECV_LENGTH_TYPE:
			if (recvreq->sniff_cb)
				recvreq->sniff_cb(conn, recvreq->arg, recvreq->lengthreq.buf + recvreq->lengthreq.pos, rb);

			recvreq->lengthreq.pos += rb;

			if (recvreq->lengthreq.pos == recvreq->lengthreq.len)
				done = 1;

			break;

		case RECV_UNTIL_TYPE:
			if (recvreq->sniff_cb)
				recvreq->sniff_cb(conn, recvreq->arg, recvreq->untilreq.buf + recvreq->untilreq.pos, rb);

			recvreq->untilreq.pos += rb;

			if (recvreq->untilreq.until_cb(recvreq->untilreq.buf, recvreq->untilreq.pos)) {
				done = 1;
			} else if (recvreq->untilreq.pos >= recvreq->untilreq.mem) {
				/* Realloc space for buffer */
				uint8_t *tmp;

				if (recvreq->untilreq.mem >= SIZET_MAX / 2) {
					if (recvreq->error_cb)
						recvreq->error_cb(conn, recvreq->arg, ENOMEM);

					SocketIOStreamConnection_release(conn);
					return;
				}

				recvreq->untilreq.mem *= 2;
				tmp = safe_realloc(recvreq->untilreq.buf, recvreq->untilreq.mem);

				if (tmp == NULL) {
					if (recvreq->error_cb)
						recvreq->error_cb(conn, recvreq->arg, ENOMEM);

					SocketIOStreamConnection_release(conn);
					return;
				}

				recvreq->untilreq.buf = tmp;
			}

			break;

		default:
			SOCKETIO_ERROR("Invalid StreamBufferRecvRequest type.\n");
			break;
	}

	if (done) {
		switch (recvreq->type) {
			case RECV_LENGTH_TYPE:
				/* Finished recving all the bytes requested */
				if (recvreq->lengthreq.done_cb)
					recvreq->lengthreq.done_cb(conn, recvreq->arg);

				break;

			case RECV_UNTIL_TYPE:
				/* Read until the user is satisfied */
				if (recvreq->untilreq.until_done_cb)
					recvreq->untilreq.until_done_cb(conn, recvreq->arg, recvreq->untilreq.buf, recvreq->untilreq.pos);

				break;

			default:
				SOCKETIO_ERROR("Invalid StreamBufferRecvRequest type.\n");
				break;
		}

		/* We don't need lock on mtx up to now, because no other thread
		 * can be reading or writing the recvreq. The recvreq is only
		 * modified the first time in SocketIOStreamConnection_recv when recving = 0,
		 * and recving is then set to 1. Since we got here, that means recvreq is valid.
		 * But after that we need to access recving so we now need to lock the mutex. */
		if (pthread_mutex_lock(&conn->mtx) != 0) {
			/* Shouldn't happen. If it does, we're kinda screwed because the request is done
			 * but we can't move on to another one, so the whole connection is blocked.
			 * But the man page says the only possible errors are EDEADLK and EINVAL, both
			 * of which cannot happen if this code is written properly. */
			SOCKETIO_SYSERROR("pthread_mutex_lock");
			SocketIOStreamConnection_release(conn);
			return;
		}

		conn->recving = 0;
		conn->recvreq = NULL;

		if (!queue_empty(&conn->recvq)) {
			queue_pop(&conn->recvq, &conn->recvreq);
			conn->recving = 1;
			asyncio_continue(continued);

			if (pthread_mutex_unlock(&conn->mtx) != 0)
				SOCKETIO_SYSERROR("pthread_mutex_unlock");
		} else {
			if (pthread_mutex_unlock(&conn->mtx) != 0)
				SOCKETIO_SYSERROR("pthread_mutex_unlock");

			SocketIOStreamConnection_release(conn);
		}
	} else {
		/* Keep recving bytes */
		asyncio_continue(continued);
	}
}

static void handle_stream_send(int fd, asyncio_fdevent_t revents, void *arg, asyncio_continue_t continued)
{
	struct SocketIOStreamConnection *conn;
	ssize_t sb;
	int err;

	conn = (struct SocketIOStreamConnection *)arg;

	if (revents & ASYNCIO_FDEVENT_ERROR) {
		if (conn->sendreq->error_cb) {
			err = get_socket_errno(fd);
			conn->sendreq->error_cb(conn, conn->sendreq->arg, err);
		}

		SocketIOStreamConnection_release(conn);
		return;
	}

	sb = send(fd, conn->sendreq->buf + conn->sendreq->pos, conn->sendreq->len - conn->sendreq->pos, 0);

	if (sb < 0) {
		if (errno == EINTR || errno == EAGAIN) {
			asyncio_continue(continued);
			return;
		}

		if (conn->sendreq->error_cb)
			conn->sendreq->error_cb(conn, conn->sendreq->arg, errno);

		SocketIOStreamConnection_release(conn);
		return;
	}

	conn->sendreq->pos += sb;

	if (conn->sendreq->pos == conn->sendreq->len) {
		/* Finished sending all the bytes requested */
		if (conn->sendreq->done_cb)
			conn->sendreq->done_cb(conn, conn->sendreq->arg);

		/* We don't need lock on mtx up to now, because no other thread
		 * can be reading or writing the sendreq. The sendreq is only
		 * modified the first time in SocketIOStreamConnection_send when sending = 0,
		 * and sending is then set to 1. Since we got here, that means sendreq is valid.
		 * But after that we need to access sending so we now need to lock the mutex. */
		if (pthread_mutex_lock(&conn->mtx) != 0) {
			/* Shouldn't happen. If it does, we're kinda screwed because the request is done
			 * but we can't move on to another one, so the whole connection is blocked.
			 * But the man page says the only possible errors are EDEADLK and EINVAL, both
			 * of which cannot happen if this code is written properly. */
			SOCKETIO_SYSERROR("pthread_mutex_lock");
			SocketIOStreamConnection_release(conn);
			return;
		}

		conn->sending = 0;
		conn->sendreq = NULL;

		if (!queue_empty(&conn->sendq)) {
			queue_pop(&conn->sendq, &conn->sendreq);
			conn->sending = 1;
			asyncio_continue(continued);

			if (pthread_mutex_unlock(&conn->mtx) != 0)
				SOCKETIO_SYSERROR("pthread_mutex_unlock");
		} else {
			if (pthread_mutex_unlock(&conn->mtx) != 0)
				SOCKETIO_SYSERROR("pthread_mutex_unlock");

			SocketIOStreamConnection_release(conn);
		}
	} else {
		/* Keep sending bytes */
		asyncio_continue(continued);
	}
}

static void handle_stream_accept(int fd, asyncio_fdevent_t revents, void *arg, asyncio_continue_t continued)
{
	struct SocketIOStreamServer *server;
	struct SocketIOStreamConnection *conn;
	int client_sock;
	struct sockaddr client_addr;
	socklen_t client_addrlen;
	int err;

	server = (struct SocketIOStreamServer *)arg;

	if (!server->accepting) {
		/* Release our handle to server (we need refcount because we need to be sure
		 * that server still points to valid memory before we check the accepting value) */
		SocketIOStreamServer_close(server);
		return;	/* Don't continue */
	}

	if (revents & ASYNCIO_FDEVENT_ERROR) {
		err = get_socket_errno(fd);
		SOCKETIO_ERROR("Error event in accept sock: %d (%s)\n", err, strerror(err));
		SocketIOStreamServer_close(server);
		return;
	}

	client_addrlen = sizeof client_addr;
	client_sock = accept(fd, &client_addr, &client_addrlen);

	while (client_sock >= 0) {
		if (set_nonblocking(client_sock) == 0) {
			conn = SocketIOStreamConnection_create(client_sock);

			if (conn == NULL) {
				SOCKETIO_ERROR("Failed to create SocketIOStreamConnection.\n");
			} else {
				server->accept_cb(conn, &client_addr, client_addrlen);	/* accept_cb better not be blocking */
				SocketIOStreamConnection_release(conn);
			}
		} else {
			SOCKETIO_ERROR("Failed to set client_sock to nonblocking after accept.\n");
		}

		if (!server->accepting) {
			SocketIOStreamServer_close(server);
			return;
		}

		client_sock = accept(fd, &client_addr, &client_addrlen);
	}

	if (errno != EWOULDBLOCK) {
		SOCKETIO_SYSERROR("accept");
		SocketIOStreamServer_close(server);
		return;
	}

	asyncio_continue(continued);
}

static void handle_stream_connect(int fd, asyncio_fdevent_t revents, void *arg, asyncio_continue_t continued)
{
	struct SocketIOStreamClient *client;
	int err;
	(void)fd;
	(void)continued;

	client = (struct SocketIOStreamClient *)arg;

	if (revents & ASYNCIO_FDEVENT_ERROR) {
		err = get_socket_errno(fd);
		client->error_cb(client->conn, client->arg, err);
	} else {
		client->connect_cb(client->conn, client->arg);
	}

	SocketIOStreamConnection_release(client->conn);
	SocketIOStreamClient_close(client); /* Releases our reference to client */
}

static int create_accept_sock(const struct sockaddr *addr, socklen_t addrlen, int domain, int protocol, int backlog)
{
	int accept_sock;
	int one = 1;

	accept_sock = socket(domain, SOCK_STREAM, protocol);

	if (accept_sock == -1) {
		SOCKETIO_SYSERROR("socket");
		return -1;
	}

	if (set_nonblocking(accept_sock) != 0) {
		SOCKETIO_ERROR("Failed to set accept_sock to nonblocking.\n");
		close(accept_sock);
		return -1;
	}

	/* Set REUSEADDR on accept_sock to be able to bind to address next time we run immediately. */
	if (setsockopt(accept_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0) {
		SOCKETIO_SYSERROR("setsockopt");
		close(accept_sock);
		return -1;
	}

	if (bind(accept_sock, addr, addrlen) != 0) {
		SOCKETIO_SYSERROR("bind");
		close(accept_sock);
		return -1;
	}

	if (listen(accept_sock, backlog) != 0) {
		SOCKETIO_SYSERROR("listen");
		close(accept_sock);
		return -1;
	}

	return accept_sock;
}

__attribute__((visibility("default")))
int SocketIOStreamConnection_recv(SocketIOStreamConnection_t tconn, uint8_t *data, size_t len, SocketIOStreamConnection_done_cb recv_done_cb,
					SocketIOStreamConnection_error_cb error_cb, SocketIOStreamConnection_sniff_cb sniff_cb, void *arg)
{
	struct SocketIOStreamConnection *conn;
	struct StreamBufferRecvRequest *req;
	asyncio_handle_t handle;
	int rc;

	conn = (struct SocketIOStreamConnection *)tconn;

	req = safe_malloc(sizeof *req);

	if (req == NULL)
		return -1;

	req->type = RECV_LENGTH_TYPE;
	req->lengthreq.buf = data;
	req->lengthreq.len = len;
	req->lengthreq.pos = 0;
	req->lengthreq.done_cb = recv_done_cb;
	req->error_cb = error_cb;
	req->sniff_cb = sniff_cb;
	req->arg = arg;

	if (pthread_mutex_lock(&conn->mtx) != 0) {
		SOCKETIO_SYSERROR("pthread_mutex_lock");
		safe_free(req);
		return -1;
	}

	rc = 0;

	if (conn->recving) {
		queue_push(&conn->recvq, req);
	} else {
		conn->recvreq = req;
		++conn->refcount;

		if (asyncio_fdevent(conn->sockfd, ASYNCIO_FDEVENT_READ | ASYNCIO_FDEVENT_ERROR, handle_stream_recv, conn, ASYNCIO_FLAG_NONE, &handle) == 0) {
			asyncio_release(handle);
			conn->recving = 1;
		} else {
			SOCKETIO_ERROR("Failed to register fdevent.\n");
			--conn->refcount; /* Undo refcount increment */
			conn->recvreq = NULL;
			rc = -1;
		}
	}

	if (pthread_mutex_unlock(&conn->mtx) != 0)
		SOCKETIO_SYSERROR("pthread_mutex_unlock");

	if (rc != 0)
		safe_free(req);

	return rc;
}

__attribute__((visibility("default")))
int SocketIOStreamConnection_recvuntil(SocketIOStreamConnection_t tconn, SocketIOStreamConnection_until_cb until_cb, SocketIOStreamConnection_until_done_cb until_done_cb,
					SocketIOStreamConnection_error_cb error_cb, SocketIOStreamConnection_sniff_cb sniff_cb, void *arg)
{
	struct SocketIOStreamConnection *conn;
	struct StreamBufferRecvRequest *req;
	asyncio_handle_t handle;
	uint8_t *buf;
	int rc;

	conn = (struct SocketIOStreamConnection *)tconn;

	req = safe_malloc(sizeof *req);

	if (req == NULL)
		return -1;

	buf = safe_malloc(DEFAULT_RECV_BUF_SIZE);

	if (buf == NULL) {
		safe_free(req);
		return -1;
	}

	req->type = RECV_UNTIL_TYPE;
	req->untilreq.buf = buf;
	req->untilreq.mem = DEFAULT_RECV_BUF_SIZE;
	req->untilreq.pos = 0;
	req->untilreq.until_cb = until_cb;
	req->untilreq.until_done_cb = until_done_cb;
	req->error_cb = error_cb;
	req->sniff_cb = sniff_cb;
	req->arg = arg;

	if (pthread_mutex_lock(&conn->mtx) != 0) {
		SOCKETIO_SYSERROR("pthread_mutex_lock");
		safe_free(buf);
		safe_free(req);
		return -1;
	}

	rc = 0;

	if (conn->recving) {
		queue_push(&conn->recvq, req);
	} else {
		conn->recvreq = req;
		++conn->refcount;

		if (asyncio_fdevent(conn->sockfd, ASYNCIO_FDEVENT_READ | ASYNCIO_FDEVENT_ERROR, handle_stream_recv, conn, ASYNCIO_FLAG_NONE, &handle) == 0) {
			asyncio_release(handle);
			conn->recving = 1;
		} else {
			SOCKETIO_ERROR("Failed to register fdevent.\n");
			--conn->refcount; /* Undo refcount increment */
			conn->recvreq = NULL;
			rc = -1;
		}
	}

	if (pthread_mutex_unlock(&conn->mtx) != 0)
		SOCKETIO_SYSERROR("pthread_mutex_unlock");

	if (rc != 0) {
		safe_free(buf);
		safe_free(req);
	}

	return rc;
}

__attribute__((visibility("default")))
int SocketIOStreamConnection_send(SocketIOStreamConnection_t tconn, const uint8_t *data, size_t len, SocketIOStreamConnection_done_cb send_done_cb, SocketIOStreamConnection_error_cb error_cb, void *arg)
{
	struct SocketIOStreamConnection *conn;
	struct StreamBufferSendRequest *req;
	asyncio_handle_t handle;
	int rc;

	conn = (struct SocketIOStreamConnection *)tconn;

	req = safe_malloc(sizeof *req);

	if (req == NULL)
		return -1;

	req->buf = data;
	req->len = len;
	req->pos = 0;
	req->done_cb = send_done_cb;
	req->error_cb = error_cb;
	req->arg = arg;

	if (pthread_mutex_lock(&conn->mtx) != 0) {
		SOCKETIO_SYSERROR("pthread_mutex_lock");
		safe_free(req);
		return -1;
	}

	rc = 0;

	if (conn->sending) {
		queue_push(&conn->sendq, req);
	} else {
		conn->sendreq = req;
		++conn->refcount;

		if (asyncio_fdevent(conn->sockfd, ASYNCIO_FDEVENT_WRITE | ASYNCIO_FDEVENT_ERROR, handle_stream_send, conn, ASYNCIO_FLAG_NONE, &handle) == 0) {
			asyncio_release(handle);
			conn->sending = 1;
		} else {
			SOCKETIO_ERROR("Failed to register fdevent.\n");
			--conn->refcount; /* Undo refcount increment */
			conn->sendreq = NULL;
			rc = -1;
		}
	}

	if (pthread_mutex_unlock(&conn->mtx) != 0)
		SOCKETIO_SYSERROR("pthread_mutex_unlock");

	if (rc != 0)
		safe_free(req);

	return rc;
}

__attribute__((visibility("default")))
int SocketIOStreamClient_create(int domain, int protocol, SocketIOStreamClient_t *tclient)
{
	struct SocketIOStreamClient *client;
	int sockfd;

	client = safe_malloc(sizeof *client);

	if (client == NULL) {
		SOCKETIO_ERROR("Failed to malloc client.\n");
		return -1;
	}

	sockfd = socket(domain, SOCK_STREAM, protocol);

	if (sockfd == -1) {
		SOCKETIO_SYSERROR("socket");
		safe_free(client);
		return -1;
	}

	if (set_nonblocking(sockfd) != 0) {
		SOCKETIO_ERROR("Failed to set sockfd to nonblocking.\n");
		close(sockfd);
		safe_free(client);
		return -1;
	}

	if (pthread_mutex_init(&client->mtx, NULL) != 0) {
		SOCKETIO_SYSERROR("pthread_mutex_init");
		close(sockfd);
		safe_free(client);
		return -1;
	}

	client->sockfd = sockfd;
	client->conn = NULL;
	client->refcount = 1;
	*tclient = client;

	return 0;
}

__attribute__((visibility("default")))
int SocketIOStreamClient_connect(SocketIOStreamClient_t tclient, const struct sockaddr *addr, socklen_t addrlen,
		SocketIOStreamClient_connect_cb connect_cb, SocketIOStreamConnection_error_cb error_cb, void *arg)
{
	struct SocketIOStreamConnection *conn;
	struct SocketIOStreamClient *client;
	asyncio_handle_t handle;

	client = (struct SocketIOStreamClient *)tclient;

	if (connect(client->sockfd, addr, addrlen) != 0 && errno != EINPROGRESS) {
		SOCKETIO_SYSERROR("connect");
		return -1;
	}

	conn = SocketIOStreamConnection_create(client->sockfd);

	if (conn == NULL) {
		SOCKETIO_ERROR("Failed to create SocketIOStreamConnection.\n");
		return -1;
	}

	/* We're passing it to the handle_stream_connect thread */
	++(conn->refcount);
	++(conn->cond_refcount);

	client->connect_cb = connect_cb;
	client->error_cb = error_cb;
	client->arg = arg;
	client->conn = conn;
	++(client->refcount);

	if (asyncio_fdevent(client->sockfd, ASYNCIO_FDEVENT_WRITE, handle_stream_connect, client, ASYNCIO_FLAG_NONE, &handle) != 0) {
		SOCKETIO_ERROR("Failed to register fdevent.\n");
		SocketIOStreamConnection_release(conn);
		close(client->sockfd);
		safe_free(client);
		return -1;
	}

	client->sockfd = -1;	/* sockfd stored in the conn from now on... */
	asyncio_release(handle);

	return 0;
}

__attribute__((visibility("default")))
int SocketIOStreamClient_block(SocketIOStreamClient_t tclient)
{
	struct SocketIOStreamClient *client;

	client = (struct SocketIOStreamClient *)tclient;

	if (pthread_mutex_lock(&client->conn->mtx) != 0) {
		SOCKETIO_SYSERROR("pthread_mutex_lock");
		return -1;
	}

	while (!client->conn->finished) {
		if (pthread_cond_wait(&client->conn->cond, &client->conn->mtx) != 0) {
			SOCKETIO_SYSERROR("pthread_cond_wait");

			if (pthread_mutex_unlock(&client->conn->mtx) != 0)
				SOCKETIO_SYSERROR("pthread_mutex_unlock");

			return -1;
		}
	}

	if (pthread_mutex_unlock(&client->conn->mtx) != 0)
		SOCKETIO_SYSERROR("pthread_mutex_unlock");

	return 0;
}

__attribute__((visibility("default")))
void SocketIOStreamClient_close(SocketIOStreamClient_t tclient)
{
	struct SocketIOStreamClient *client;

	client = (struct SocketIOStreamClient *)tclient;

	if (pthread_mutex_lock(&client->mtx) != 0) {
		SOCKETIO_SYSERROR("pthread_mutex_lock");
		return;
	}

	if (client->refcount == 0) {
		SOCKETIO_ERROR("SocketIOStreamClient refcount is already 0 before close.\n");

		if (pthread_mutex_unlock(&client->mtx) != 0)
			SOCKETIO_SYSERROR("pthread_mutex_unlock");

		return;
	}

	--(client->refcount);

	if (client->refcount == 0) {
		if (pthread_mutex_unlock(&client->mtx) != 0)
			SOCKETIO_SYSERROR("pthread_mutex_unlock");

		if (pthread_mutex_destroy(&client->mtx) != 0)
			SOCKETIO_SYSERROR("pthread_mutex_destroy");

		if (client->conn == NULL)
			close(client->sockfd);
		else
			SocketIOStreamConnection_release(client->conn);

		safe_free(client);
		return;
	}

	if (pthread_mutex_unlock(&client->mtx) != 0)
		SOCKETIO_SYSERROR("pthread_mutex_unlock");
}

__attribute__((visibility("default")))
int SocketIOStreamServer_create(const struct sockaddr *addr, socklen_t addrlen, int domain, int protocol, int backlog, SocketIOStreamServer_t *tserver)
{
	struct SocketIOStreamServer *server;
	int accept_sock;

	server = safe_malloc(sizeof *server);

	if (server == NULL) {
		SOCKETIO_ERROR("Failed to create stream server.\n");
		return -1;
	}

	accept_sock = create_accept_sock(addr, addrlen, domain, protocol, backlog);

	if (accept_sock == -1) {
		SOCKETIO_ERROR("Failed to create accept sock.\n");
		safe_free(server);
		return -1;
	}

	if (pthread_mutex_init(&server->mtx, NULL) != 0) {
		SOCKETIO_SYSERROR("pthread_mutex_init");
		close(accept_sock);
		safe_free(server);
		return -1;
	}

	server->accept_sock = accept_sock;
	server->accepting = 0;
	server->refcount = 1;
	*tserver = (SocketIOStreamServer_t)server;

	return 0;
}

__attribute__((visibility("default")))
int SocketIOStreamServer_run(SocketIOStreamServer_t tserver, SocketIOStreamServer_accept_cb accept_cb)
{
	struct SocketIOStreamServer *server;
	asyncio_handle_t asyncio_handle;

	server = (struct SocketIOStreamServer *)tserver;

	if (server->accepting)
		return -1;

	server->refcount = 2;	/* At this point only one thread has access to the server handle so no need for mutex */
	server->accept_cb = accept_cb;
	server->accepting = 1;

	if (asyncio_fdevent(server->accept_sock, ASYNCIO_FDEVENT_READ, handle_stream_accept, server, ASYNCIO_FLAG_NONE, &asyncio_handle) != 0) {
		SOCKETIO_ERROR("Failed to register fdevent.\n");
		server->accepting = 0;
		return -1;
	}

	server->asyncio_handle = asyncio_handle;

	return 0;
}

__attribute__((visibility("default")))
int SocketIOStreamServer_block(SocketIOStreamServer_t tserver)
{
	struct SocketIOStreamServer *server;

	server = (struct SocketIOStreamServer *)tserver;

	if (asyncio_join(server->asyncio_handle) != 0) {
		SOCKETIO_ERROR("Failed to join asyncio handle.\n");
		return -1;
	}

	return 0;
}

__attribute__((visibility("default")))
void SocketIOStreamServer_stop(SocketIOStreamServer_t tserver)
{
	struct SocketIOStreamServer *server;

	/* No need for lock here, because race conditions won't
	 * affect us negatively. At worst the accept thread will
	 * miss one pass of server->accepting becoming 0 and stop
	 * the next time. */
	server = (struct SocketIOStreamServer *)tserver;
	server->accepting = 0;
}

__attribute__((visibility("default")))
void SocketIOStreamServer_close(SocketIOStreamServer_t tserver)
{
	struct SocketIOStreamServer *server;

	server = (struct SocketIOStreamServer *)tserver;

	if (pthread_mutex_lock(&server->mtx) != 0) {
		SOCKETIO_SYSERROR("pthread_mutex_lock");
		return;
	}

	if (server->refcount == 0) {
		SOCKETIO_ERROR("SocketIOStreamServer refcount already 0 before close.\n");

		if (pthread_mutex_unlock(&server->mtx) != 0)
			SOCKETIO_SYSERROR("pthread_mutex_unlock");

		return;
	}

	--server->refcount;

	if (server->refcount == 0) {
		if (pthread_mutex_destroy(&server->mtx) != 0)
			SOCKETIO_SYSERROR("pthread_mutex_destroy");

		if (close(server->accept_sock) != 0)
			SOCKETIO_SYSERROR("close");

		safe_free(server);
		return;
	}

	if (pthread_mutex_unlock(&server->mtx) != 0)
		SOCKETIO_SYSERROR("pthread_mutex_unlock");
}
