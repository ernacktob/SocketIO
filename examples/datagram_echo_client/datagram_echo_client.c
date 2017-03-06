#include <stdio.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/errno.h>

#include "SocketIO.h"

#define MAX_DGRAM_LEN	256

/* PROTOTYPES */
static void datagram_recv_error(SocketIODatagramEndpoint_t endpoint, void *arg, int err);
static void datagram_received(SocketIODatagramEndpoint_t endpoint, void *arg, uint8_t *data, size_t len, const struct sockaddr *s_addr, socklen_t s_addrlen, SocketIODatagram_continue_t continued);
static void datagram_sent_error(SocketIODatagramEndpoint_t endpoint, void *arg, int err);
static void datagram_sent(SocketIODatagramEndpoint_t endpoint, void *arg);
/* END PROTOTYPES */

static void datagram_recv_error(SocketIODatagramEndpoint_t endpoint, void *arg, int err)
{
	(void)arg;

	printf("Error while receiving datagram: %d (%s).\n", err, strerror(err));
	SocketIODatagramEndpoint_finished(endpoint);
}

static void datagram_received(SocketIODatagramEndpoint_t endpoint, void *arg, uint8_t *data, size_t len, const struct sockaddr *s_addr, socklen_t s_addrlen, SocketIODatagram_continue_t continued)
{
	const char *hello = "HELLO WORLD!\n";
	const struct sockaddr_in *from_addr, *server_addr;

	if (s_addrlen != sizeof *from_addr) {
		SocketIODatagramEndpoint_continue_listening(continued);
		return;
	}

	from_addr = (const struct sockaddr_in *)s_addr;
	server_addr = (const struct sockaddr_in *)arg;

	if ((server_addr->sin_port != from_addr->sin_port) || (server_addr->sin_addr.s_addr != from_addr->sin_addr.s_addr)) {
		SocketIODatagramEndpoint_continue_listening(continued);
		return;
	}

	printf("Received %lu byte datagram from %s(%hu):\n", len, inet_ntoa(from_addr->sin_addr), ntohs(from_addr->sin_port));
	fwrite(data, 1, len, stdout);
	printf("\n");

	if (len != strlen(hello))
		printf("Wrong data replied.\n");
	else if (memcmp(hello, data, len) != 0)
		printf("Wrong data replied.\n");

	SocketIODatagramEndpoint_finished(endpoint);
}

static void datagram_sent_error(SocketIODatagramEndpoint_t endpoint, void *arg, int err)
{
	(void)endpoint;
	(void)arg;

	printf("Error while sending datagram: %d (%s).\n", err, strerror(err));
	SocketIODatagramEndpoint_finished(endpoint);
}

static void datagram_sent(SocketIODatagramEndpoint_t endpoint, void *arg)
{
	(void)endpoint;
	(void)arg;

	printf("Sent datagram successfully.\n");
}

int main()
{
	SocketIODatagramEndpoint_t endpoint;
	struct sockaddr_in server_addr;
	const char *hello = "HELLO WORLD!\n";

	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(12345);
	server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	memset(&server_addr.sin_zero, 0, sizeof server_addr.sin_zero);

	if (SocketIODatagramEndpoint_create(PF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, &endpoint) != 0) {
		printf("Failed to create datagram endpoint.\n");
		return -1;
	}

	printf("Listening on datagram endpoint.\n");

	if (SocketIODatagramEndpoint_listen(endpoint, datagram_received, datagram_recv_error, MAX_DGRAM_LEN, &server_addr) != 0) {
		printf("Failed to listen on datagram endpoint.\n");
		SocketIODatagramEndpoint_close(endpoint);
		return -1;
	}

	printf("Sending message to server.\n");

	if (SocketIODatagramEndpoint_sendto(endpoint, (const uint8_t *)hello, strlen(hello), (struct sockaddr *)&server_addr, sizeof server_addr, datagram_sent, datagram_sent_error, NULL) != 0) {
		printf("Failed to sendto datagram endpoint.\n");
		SocketIODatagramEndpoint_close(endpoint);
		return -1;
	}

	printf("Blocking on datagram endpoint...\n");

	if (SocketIODatagramEndpoint_block(endpoint) != 0) {
		printf("Failed to block on datagram endpoint.\n");
		SocketIODatagramEndpoint_close(endpoint);
		return -1;
	}

	printf("Closing...\n");
	SocketIODatagramEndpoint_close(endpoint);

	return 0;
}
