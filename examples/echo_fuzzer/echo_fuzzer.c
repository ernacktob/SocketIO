#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "SocketIOStream.h"
#include "asyncio.h"

#define NUM_CLIENTS		500
#define CONNECTION_PERIOD	1000	/* All connections launched during 1ms */

#define MAX_REPLY_SIZE		1000

/* PROTOTYPES */
static int client_is_line(const uint8_t *data, size_t len);
static void client_recvd_reply(SocketIOStreamConnection_t conn, void *arg, uint8_t *data, size_t len);
static void client_recvd_welcome1(SocketIOStreamConnection_t conn, void *arg, uint8_t *data, size_t len);
static void client_recvd_welcome2(SocketIOStreamConnection_t conn, void *arg, uint8_t *data, size_t len);
static void client_recvd_welcome3(SocketIOStreamConnection_t conn, void *arg, uint8_t *data, size_t len);
static void client_connect_cb(SocketIOStreamConnection_t conn, void *arg);
static void client_error_cb(SocketIOStreamConnection_t conn, void *arg, int err);

static void run_client(void *arg, asyncio_continue_t continued);
/* END PROTOTYPES */

static int client_is_line(const uint8_t *data, size_t len)
{
	if (data[len - 1] == '\n' || len >= MAX_REPLY_SIZE)
		return 1;

	return 0;
}

static void client_recvd_reply(SocketIOStreamConnection_t conn, void *arg, uint8_t *data, size_t len)
{
	const char *hello = "HELLO\n";
	(void)conn;
	(void)arg;

	if ((len != strlen(hello)) || (memcmp(data, hello, len) != 0))
		printf("Did not receive correct reply.\n");

	free(data);
}

static void client_recvd_welcome1(SocketIOStreamConnection_t conn, void *arg, uint8_t *data, size_t len)
{
	const char *welcome = "Welcome to the echo server.\n";
	(void)conn;
	(void)arg;

	if ((len != strlen(welcome)) || (memcmp(data, welcome, len) != 0))
		printf("Did not receive correct welcome.\n");

	free(data);
}

static void client_recvd_welcome2(SocketIOStreamConnection_t conn, void *arg, uint8_t *data, size_t len)
{
	const char *welcome = "Type a line of at most 1000 characters, and it will be echoed back.\n";
	(void)conn;
	(void)arg;

	if ((len != strlen(welcome)) || (memcmp(data, welcome, len) != 0))
		printf("Did not receive correct welcome.\n");

	free(data);
}

static void client_recvd_welcome3(SocketIOStreamConnection_t conn, void *arg, uint8_t *data, size_t len)
{
	const char *welcome = "\n";
	(void)conn;
	(void)arg;

	if ((len != strlen(welcome)) || (memcmp(data, welcome, len) != 0))
		printf("Did not receive correct welcome.\n");

	free(data);
}

static void client_sent_cb(SocketIOStreamConnection_t conn, void *arg)
{
	(void)arg;

	if (SocketIOStreamConnection_recvuntil(conn, client_is_line, client_recvd_reply, client_error_cb, NULL, NULL) != 0)
		printf("Failed to recvuntil from SocketIOStreamConnection.\n");
}

static void client_connect_cb(SocketIOStreamConnection_t conn, void *arg)
{
	const char *hello = "HELLO\n";
	(void)arg;

	if (SocketIOStreamConnection_recvuntil(conn, client_is_line, client_recvd_welcome1, client_error_cb, NULL, NULL) != 0)
		printf("Failed to recvuntil from SocketIOStreamConnection.\n");

	if (SocketIOStreamConnection_recvuntil(conn, client_is_line, client_recvd_welcome2, client_error_cb, NULL, NULL) != 0)
		printf("Failed to recvuntil from SocketIOStreamConnection.\n");

	if (SocketIOStreamConnection_recvuntil(conn, client_is_line, client_recvd_welcome3, client_error_cb, NULL, NULL) != 0)
		printf("Failed to recvuntil from SocketIOStreamConnection.\n");

	if (SocketIOStreamConnection_send(conn, (uint8_t *)hello, strlen(hello), client_sent_cb, client_error_cb, NULL) != 0)
		printf("Failed to send to SocketIOStreamConnection.\n");
}

static void client_error_cb(SocketIOStreamConnection_t conn, void *arg, int err)
{
	(void)conn;
	(void)arg;
	printf("Client error: %d (%s).\n", err, strerror(err));
}

static void run_client(void *arg, asyncio_continue_t continued)
{
	SocketIOStreamClient_t client;
	struct sockaddr_in addr;
	(void)arg;
	(void)continued;

	addr.sin_family = AF_INET;
	addr.sin_port = htons(12345);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	memset(&addr.sin_zero, 0, sizeof addr.sin_zero);

	if (SocketIOStreamClient_connect((struct sockaddr *)&addr, sizeof addr, PF_INET, IPPROTO_TCP, client_connect_cb, client_error_cb, NULL, &client) != 0) {
		printf("Failed to connect.\n");
		return;
	}

	if (SocketIOStreamClient_block(client) != 0)
		printf("Failed to block for client.\n");

	SocketIOStreamClient_close(client);
}

int main()
{
	asyncio_handle_t handles[NUM_CLIENTS];
	int i;

	srand(time(NULL));

	for (i = 0; i < NUM_CLIENTS; i++) {
		if (asyncio_timevent(rand() % CONNECTION_PERIOD, run_client, NULL, ASYNCIO_FLAG_CONTRACTOR, &handles[i]) != 0) {
			printf("Failed to register timevent.\n");
			return -1;
		}
	}

	for (i = 0; i < NUM_CLIENTS; i++) {
		if (asyncio_join(handles[i]) != 0) {
			printf("Failed to join timevent.\n");
			return -1;
		}
	}

	for (i = 0; i < NUM_CLIENTS; i++)
		asyncio_release(handles[i]);

	return 0;
}
