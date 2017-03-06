.PHONY: all echo_server echo_fuzzer datagram_echo_server datagram_echo_client clean

all: echo_server echo_fuzzer datagram_echo_server datagram_echo_client

echo_server:
	$(MAKE) -C echo_server

echo_fuzzer:
	$(MAKE) -C echo_fuzzer

datagram_echo_server:
	$(MAKE) -C datagram_echo_server

datagram_echo_client:
	$(MAKE) -C datagram_echo_client

clean:
	$(MAKE) -C echo_server clean
	$(MAKE) -C echo_fuzzer clean
	$(MAKE) -C datagram_echo_server clean
	$(MAKE) -C datagram_echo_client clean
