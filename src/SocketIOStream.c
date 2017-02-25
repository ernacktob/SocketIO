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

struct SocketIOStreamClient {
	SocketIOStreamClient_connect_cb connect_cb;
	SocketIOStreamClient_connect_error_cb error_cb;
	void *arg;
	struct SocketIOStreamConnection *conn;
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

struct StreamBufferRecvRequest {
	uint8_t *buf;
	size_t len;
	size_t pos;
	SocketIOStreamConnection_done_cb done_cb;
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
static int set_nonblocking(int fd);

static struct SocketIOStreamConnection *SocketIOStreamConnection_create(int sockfd);
static void SocketIOStreamConnection_release(SocketIOStreamConnection_t tconn);

static void handle_stream_recv(int fd, asyncio_fdevent_t revents, void *arg, asyncio_continue_t continued);
static void handle_stream_send(int fd, asyncio_fdevent_t revents, void *arg, asyncio_continue_t continued);
static void handle_stream_accept(int fd, asyncio_fdevent_t revents, void *arg, asyncio_continue_t continued);
static void handle_stream_connect(int fd, asyncio_fdevent_t revents, void *arg, asyncio_continue_t continued);

static int create_accept_sock(const struct sockaddr *addr, socklen_t addrlen, int domain, int protocol, int backlog);
/* END PROTOTYPES */

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
	ssize_t rb;

	conn = (struct SocketIOStreamConnection *)arg;

	if (revents & ASYNCIO_FDEVENT_ERROR) {
		if (conn->recvreq->error_cb)
			conn->recvreq->error_cb(conn, conn->recvreq->arg, 0);

		SocketIOStreamConnection_release(conn);	/* Only release when we don't asyncio_continue */
		return;
	}

	rb = recv(fd, conn->recvreq->buf + conn->recvreq->pos, conn->recvreq->len - conn->recvreq->pos, 0);

	if (rb < 0) {
		if (errno == EINTR || errno == EAGAIN) {
			asyncio_continue(continued);
			return;
		}

		if (conn->recvreq->error_cb)
			conn->recvreq->error_cb(conn, conn->recvreq->arg, errno);

		SocketIOStreamConnection_release(conn);
		return;
	} else if (rb == 0) {
		/* Connection closed */
		if (conn->recvreq->error_cb)
			conn->recvreq->error_cb(conn, conn->recvreq->arg, 0);

		SocketIOStreamConnection_release(conn);
		return;
	}

	if (conn->recvreq->sniff_cb)
		conn->recvreq->sniff_cb(conn, conn->recvreq->arg, conn->recvreq->buf + conn->recvreq->pos, rb);

	conn->recvreq->pos += rb;

	if (conn->recvreq->pos == conn->recvreq->len) {
		/* Finished recving all the bytes requested */
		if (conn->recvreq->done_cb)
			conn->recvreq->done_cb(conn, conn->recvreq->arg);

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

	conn = (struct SocketIOStreamConnection *)arg;

	if (revents & ASYNCIO_FDEVENT_ERROR) {
		if (conn->sendreq->error_cb)
			conn->sendreq->error_cb(conn, conn->sendreq->arg, 0);

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

	server = (struct SocketIOStreamServer *)arg;

	if (!server->accepting) {
		/* Release our handle to server (we need refcount because we need to be sure
		 * that server still points to valid memory before we check the accepting value) */
		SocketIOStreamServer_close(server);
		return;	/* Don't continue */
	}

	if (revents & ASYNCIO_FDEVENT_ERROR) {
		SOCKETIO_ERROR("Error event in accept sock.\n");
		SocketIOStreamServer_close(server);
		return;
	}

	client_addrlen = sizeof client_addr;
	client_sock = accept(fd, &client_addr, &client_addrlen);

	while (client_sock >= 0) {
		conn = SocketIOStreamConnection_create(client_sock);

		if (conn == NULL) {
			SOCKETIO_ERROR("Failed to create SocketIOStreamConnection.\n");
		} else {
			server->accept_cb(conn, &client_addr, client_addrlen);	/* accept_cb bettwr not be blocking */
			SocketIOStreamConnection_release(conn);
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
	(void)fd;
	(void)continued;

	client = (struct SocketIOStreamClient *)arg;

	if (revents & ASYNCIO_FDEVENT_ERROR)
		client->error_cb(client->arg);
	else
		client->connect_cb(client->conn, client->arg);

	SocketIOStreamClient_close(client);
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

	req->buf = data;
	req->len = len;
	req->pos = 0;
	req->done_cb = recv_done_cb;
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
int SocketIOStreamClient_connect(const struct sockaddr *addr, socklen_t addrlen, int domain, int protocol, SocketIOStreamClient_connect_cb connect_cb,
					SocketIOStreamClient_connect_error_cb error_cb, void *arg, SocketIOStreamClient_t *tclient)
{
	struct SocketIOStreamClient *client;
	struct SocketIOStreamConnection *conn;
	asyncio_handle_t handle;
	int sockfd;

	client = safe_malloc(sizeof *client);

	if (client == NULL) {
		SOCKETIO_ERROR("Failed to malloc client.\n");
		return -1;
	}

	sockfd = socket(domain, SOCK_STREAM, protocol);

	if (sockfd == -1) {
		SOCKETIO_ERROR("Failed to create client sockfd.\n");
		safe_free(client);
		return -1;
	}

	if (set_nonblocking(sockfd) != 0) {
		SOCKETIO_ERROR("Failed to set sockfd to nonblocking.\n");
		close(sockfd);
		safe_free(client);
		return -1;
	}

	if (connect(sockfd, addr, addrlen) != 0 && errno != EINPROGRESS) {
		SOCKETIO_SYSERROR("connect");
		close(sockfd);
		safe_free(client);
		return -1;
	}

	conn = SocketIOStreamConnection_create(sockfd);

	if (conn == NULL) {
		SOCKETIO_ERROR("Failed to create SocketIOStreamConnection.\n");
		close(sockfd);
		safe_free(client);
		return -1;
	}

	/* We're passing it to the handle_stream_connect thread */
	++(client->conn->refcount);
	++(client->conn->cond_refcount);

	client->connect_cb = connect_cb;
	client->error_cb = error_cb;
	client->arg = arg;
	client->conn = conn;

	if (asyncio_fdevent(sockfd, ASYNCIO_FDEVENT_WRITE, handle_stream_connect, client, ASYNCIO_FLAG_NONE, &handle) != 0) {
		SOCKETIO_ERROR("Failed to register fdevent.\n");
		SocketIOStreamConnection_release(conn);
		close(sockfd);
		safe_free(client);
		return -1;
	}

	asyncio_release(handle);
	*tclient = client;
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

	SocketIOStreamConnection_release(client->conn);
	safe_free(client);
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

	if (asyncio_fdevent(server->accept_sock, ASYNCIO_FDEVENT_READ, handle_stream_accept, server, ASYNCIO_FLAG_CANCELLABLE, &asyncio_handle) != 0) {
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
