#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/errno.h>

#include "SocketIO.h"

#define MAX_DGRAM_LEN	256

/* STRUCT DEFINITIONS */
struct SendtoInfoToFree {
	struct sockaddr *to_addr;
	uint8_t *data;
};
/* END STRUCT DEFINITIONS */

/* PROTOTYPES */
static void datagram_sent_error(SocketIODatagramEndpoint_t endpoint, void *arg, int err);
static void datagram_sent(SocketIODatagramEndpoint_t endpoint, void *arg);
static void datagram_recv_error(SocketIODatagramEndpoint_t endpoint, void *arg, int err);
static void datagram_received(SocketIODatagramEndpoint_t endpoint, void *arg, uint8_t *data, size_t len, const struct sockaddr *s_addr, socklen_t s_addrlen, SocketIODatagram_continue_t continued);
/* END PROTOTYPES */

static void datagram_sent_error(SocketIODatagramEndpoint_t endpoint, void *arg, int err)
{
	struct SendtoInfoToFree *info;
	(void)endpoint;

	info = (struct SendtoInfoToFree *)arg;

	free(info->data);
	free(info->to_addr);
	free(info);

	printf("Error while sending datagram: %d (%s).\n", err, strerror(err));
}

static void datagram_sent(SocketIODatagramEndpoint_t endpoint, void *arg)
{
	struct SendtoInfoToFree *info;
	(void)endpoint;

	info = (struct SendtoInfoToFree *)arg;

	free(info->data);
	free(info->to_addr);
	free(info);

	printf("Sent datagram successfully.\n");
}

static void datagram_recv_error(SocketIODatagramEndpoint_t endpoint, void *arg, int err)
{
	(void)endpoint;
	(void)arg;

	printf("Error while receiving datagram: %d (%s).\n", err, strerror(err));
}

static void datagram_received(SocketIODatagramEndpoint_t endpoint, void *arg, uint8_t *data, size_t len, const struct sockaddr *s_addr, socklen_t s_addrlen, SocketIODatagram_continue_t continued)
{
	const struct sockaddr_in *from_addr;
	struct sockaddr_in *to_addr;
	struct SendtoInfoToFree *info;
	(void)arg;

	if (s_addrlen != sizeof *from_addr) {
		printf("Wrong length for address.\n");
		SocketIODatagramEndpoint_continue_listening(continued);
		return;
	}

	from_addr = (const struct sockaddr_in *)s_addr;

	printf("Received %lu byte datagram from %s(%hu):\n", len, inet_ntoa(from_addr->sin_addr), ntohs(from_addr->sin_port));
	fwrite(data, 1, len, stdout);
	printf("\n");

	info = malloc(sizeof *info);

	if (info == NULL) {
		perror("malloc");
		free(data);
		SocketIODatagramEndpoint_continue_listening(continued);
		return;
	}

	to_addr = malloc(sizeof *to_addr);

	if (to_addr == NULL) {
		perror("malloc");
		free(info);
		free(data);
		SocketIODatagramEndpoint_continue_listening(continued);
		return;
	}

	memcpy(to_addr, from_addr, sizeof *to_addr);

	info->data = data;
	info->to_addr = (struct sockaddr *)to_addr;

	if (SocketIODatagramEndpoint_sendto(endpoint, data, len, (struct sockaddr *)to_addr, sizeof *to_addr, datagram_sent, datagram_sent_error, info) != 0) {
		printf("Failed to sendto datagram.\n");
		free(to_addr);
		free(info);
		free(data);
	}

	SocketIODatagramEndpoint_continue_listening(continued);
}

int main()
{
	SocketIODatagramEndpoint_t endpoint;
	struct sockaddr_in server_addr;

	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(12345);
	server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	memset(&server_addr.sin_zero, 0, sizeof server_addr.sin_zero);

	if (SocketIODatagramEndpoint_create(PF_INET, SOCK_DGRAM, IPPROTO_UDP, (struct sockaddr *)&server_addr, sizeof server_addr, &endpoint) != 0) {
		printf("Failed to create datagram endpoint.\n");
		return -1;
	}

	printf("Listening on datagram endpoint.\n");

	if (SocketIODatagramEndpoint_listen(endpoint, datagram_received, datagram_recv_error, MAX_DGRAM_LEN, NULL) != 0) {
		printf("Failed to listen on datagram endpoint.\n");
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
