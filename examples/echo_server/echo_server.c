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

/* STRUCT DEFINITIONS */
struct connection_state {
	char line[MAX_INPUT_LEN + 1];
	size_t len;
};
/* END STRUCT DEFINITIONS */

/* PROTOTYPES */
static void server_byte_recvd(SocketIOStreamConnection_t conn, void *arg);
static void server_response_sent(SocketIOStreamConnection_t conn, void *arg);
static void server_sniff_cb(SocketIOStreamConnection_t conn, void *arg, const uint8_t *data, size_t len);
static void server_error_cb(SocketIOStreamConnection_t conn, void *arg, int err);
static void server_accept_cb(SocketIOStreamConnection_t conn, const struct sockaddr *addr, socklen_t addrlen);
/* END PROTOTYPES */

static void server_byte_recvd(SocketIOStreamConnection_t conn, void *arg)
{
	struct connection_state *state;

	state = (struct connection_state *)arg;
	++state->len;

	if (state->line[state->len - 1] == '\n') {
		if (SocketIOStreamConnection_send(conn, (uint8_t *)state->line, state->len, server_response_sent, server_error_cb, state) != 0) {
			printf("Failed to send to SocketIOStreamConnection.\n");
			free(state);
		}

		return;
	}

	if (state->len == MAX_INPUT_LEN + 1) {
		free(state);
		return;
	}

	if (SocketIOStreamConnection_recv(conn, (uint8_t *)state->line + state->len, 1, server_byte_recvd, server_error_cb, server_sniff_cb, state) != 0) {
		printf("Failed to recv from SocketIOStreamConnection.\n");
		free(state);
	}
}

static void server_response_sent(SocketIOStreamConnection_t conn, void *arg)
{
	struct connection_state *state;

	state = (struct connection_state *)arg;
	state->len = 0;

	if (SocketIOStreamConnection_recv(conn, (uint8_t *)state->line, 1, server_byte_recvd, server_error_cb, server_sniff_cb, state) != 0) {
		printf("Failed to recv from SocketIOStreamConnection.\n");
		free(state);
	}
}

static void server_sniff_cb(SocketIOStreamConnection_t conn, void *arg, const uint8_t *data, size_t len)
{
	size_t i;
	(void)conn;
	(void)arg;

	printf("Got %lu bytes of data:\n", len);

	for (i = 0; i < len; i++)
		printf("%c", data[i]);

	printf("\n");
}

static void server_error_cb(SocketIOStreamConnection_t conn, void *arg, int err)
{
	struct connection_state *state;
	(void)conn;

	state = (struct connection_state *)arg;

	if (err == 0)
		printf("Client disconnected.\n");
	else
		printf("Got stream connection error: %d (%s).\n", err, strerror(err));

	free(state);
}

static void server_accept_cb(SocketIOStreamConnection_t conn, const struct sockaddr *addr, socklen_t addrlen)
{
	const char *welcome = "Welcome to the echo server.\nType a line of at most "TOSTRING(MAX_INPUT_LEN)" characters, and it will be echoed back.\n\n";
	struct sockaddr_in *client_addr;
	struct connection_state *state;

	if (addrlen != sizeof *client_addr) {
		printf("Wrong size for addrlen!\n");
		return;
	}

	client_addr = (struct sockaddr_in *)addr;
	printf("Got new connection from %s(%hu).\n", inet_ntoa(client_addr->sin_addr), ntohs(client_addr->sin_port));

	state = malloc(sizeof *state);

	if (state == NULL) {
		perror("malloc");
		return;
	}

	state->line[0] = '\0';
	state->len = 0;

	if (SocketIOStreamConnection_send(conn, (uint8_t *)welcome, strlen(welcome), server_response_sent, server_error_cb, state) != 0) {
		printf("Failed to send to SocketIOStreamConnection.\n");
		free(state);
	}
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
