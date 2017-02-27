TARGET = echo_fuzzer
ROOTDIR = ../../
IDIR = $(ROOTDIR)
LIBDIR = $(ROOTDIR)
ASYNCIO_LIBDIR = $(ROOTDIR)/lib
ASYNCIO_INCDIR = $(ROOTDIR)/lib/headers
LIBS = -lsocketio -lasyncio

CC = gcc
CFLAGS = -Wall -Wextra -Werror -pedantic $(EXAMPLES_CFLAGS)

.PHONY: default all clean

default: $(TARGET)
all: default

SOURCES = $(wildcard *.c)

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) -I$(IDIR) -I$(ASYNCIO_INCDIR) -L$(LIBDIR) -L$(ASYNCIO_LIBDIR) $(SOURCES) $(LIBS) -o $(TARGET)

clean:
	rm -f $(TARGET)
