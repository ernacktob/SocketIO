.PHONY: all echo_server echo_fuzzer clean

all: echo_server echo_fuzzer

echo_server:
	$(MAKE) -C echo_server

echo_fuzzer:
	$(MAKE) -C echo_fuzzer

clean:
	$(MAKE) -C echo_server clean
	$(MAKE) -C echo_fuzzer clean
