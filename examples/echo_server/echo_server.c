#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "SocketIOStream.h"

#define MAX_INPUT_LEN	1000

#define TOSTRING(x)	STRINGIFY(x)
#define STRINGIFY(x)	#x

/* PROTOTYPES */
static int server_is_line(const uint8_t *data, size_t len);
static void server_recvd_line(SocketIOStreamConnection_t conn, void *arg, uint8_t *data, size_t len);
static void server_response_sent(SocketIOStreamConnection_t conn, void *arg);
static void server_welcome_sent(SocketIOStreamConnection_t conn, void *arg);
static void server_error_cb(SocketIOStreamConnection_t conn, void *arg, int err);
static void server_accept_cb(SocketIOStreamConnection_t conn, const struct sockaddr *addr, socklen_t addrlen);
/* END PROTOTYPES */

static int server_is_line(const uint8_t *data, size_t len)
{
	if (data[len - 1] == '\n' || len >= MAX_INPUT_LEN)
		return 1;

	return 0;
}

static void server_recvd_line(SocketIOStreamConnection_t conn, void *arg, uint8_t *data, size_t len)
{
	const char *errormsg = "ERROR: Line too long.\n";
	(void)arg;

	if (data[len - 1] != '\n') {
		if (SocketIOStreamConnection_send(conn, (uint8_t *)errormsg, strlen(errormsg), NULL, server_error_cb, NULL) != 0)
			printf("Failed to send to SocketIOStreamConnection.\n");

		free(data);
	} else {
		if (SocketIOStreamConnection_send(conn, data, len, server_response_sent, server_error_cb, data) != 0) {
			printf("Failed to send to SocketIOStreamConnection.\n");
			free(data);
		}
	}
}

static void server_response_sent(SocketIOStreamConnection_t conn, void *arg)
{
	free(arg);

	if (SocketIOStreamConnection_recvuntil(conn, server_is_line, server_recvd_line, server_error_cb, NULL, NULL) != 0)
		printf("Failed to recvuntil from SocketIOStreamConnection.\n");
}

static void server_welcome_sent(SocketIOStreamConnection_t conn, void *arg)
{
	(void)arg;

	if (SocketIOStreamConnection_recvuntil(conn, server_is_line, server_recvd_line, server_error_cb, NULL, NULL) != 0)
		printf("Failed to recvuntil from SocketIOStreamConnection.\n");
}

static void server_error_cb(SocketIOStreamConnection_t conn, void *arg, int err)
{
	(void)conn;
	(void)arg;

	if (err == 0)
		printf("Client disconnected.\n");
	else
		printf("Got stream connection error: %d (%s).\n", err, strerror(err));
}

static void server_accept_cb(SocketIOStreamConnection_t conn, const struct sockaddr *addr, socklen_t addrlen)
{
	const char *welcome = "Welcome to the echo server.\nType a line of at most "TOSTRING(MAX_INPUT_LEN)" characters, and it will be echoed back.\n\n";
	struct sockaddr_in *client_addr;

	if (addrlen != sizeof *client_addr) {
		printf("Wrong size for addrlen!\n");
		return;
	}

	client_addr = (struct sockaddr_in *)addr;
	printf("Got new connection from %s(%hu).\n", inet_ntoa(client_addr->sin_addr), ntohs(client_addr->sin_port));

	if (SocketIOStreamConnection_send(conn, (uint8_t *)welcome, strlen(welcome), server_welcome_sent, server_error_cb, NULL) != 0)
		printf("Failed to send to SocketIOStreamConnection.\n");
}

int main()
{
	struct sockaddr_in server_addr;
	SocketIOStreamServer_t server;

	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(12345);
	server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	memset(&server_addr.sin_zero, 0, sizeof server_addr.sin_zero);

	if (SocketIOStreamServer_create((struct sockaddr *)&server_addr, sizeof server_addr, PF_INET, IPPROTO_TCP, 5, &server) != 0) {
		printf("Failed to create SocketIOStreamServer.\n");
		return -1;
	}

	printf("Listening on loopback port %hu...\n", 12345);

	if (SocketIOStreamServer_run(server, server_accept_cb) != 0) {
		printf("Failed to run SocketIOStreamServer.\n");
		SocketIOStreamServer_close(server);
		return -1;
	}

	printf("Blocking until server stops...\n");

	if (SocketIOStreamServer_block(server) != 0) {
		printf("Failed to block SocketIOStreamServer.\n");
		SocketIOStreamServer_close(server);
		return -1;
	}

	printf("Server stopped. Closing...\n");
	SocketIOStreamServer_close(server);

	return 0;
}
