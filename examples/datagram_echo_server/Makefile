TARGET = datagram_echo_server
ROOTDIR = ../../
IDIR = $(ROOTDIR)
LIBDIR = $(ROOTDIR)
LIBS = -lsocketio

CC = gcc
CFLAGS = -Wall -Wextra -Werror -pedantic $(EXAMPLES_CFLAGS)

.PHONY: default all clean

default: $(TARGET)
all: default

SOURCES = $(wildcard *.c)

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) -I$(IDIR) -L$(LIBDIR) $(SOURCES) $(LIBS) -o $(TARGET)

clean:
	rm -f $(TARGET)
