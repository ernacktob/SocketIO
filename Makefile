UNAME = $(shell uname)

ifeq ($(UNAME), Darwin)
LIBEXTENSION = dylib
TARGET = libsocketio.dylib
LDFLAGS = -dylib -macosx_version_min 10.8 -install_name $(shell pwd)/$(TARGET)
LIBS = -lpthread -lasyncio
else
LIBEXTENSION = so
LDFLAGS = -shared
TARGET = libsocketio.so
EXAMPLES_CFLAGS = -Wl,-rpath=$(shell pwd) -Wl,-rpath=$(shell pwd)/lib	# Tell linker where to look for libsocketio.so and libasyncio.so when linking examples
LIBS = -lpthread -lasyncio
endif

export EXAMPLES_CFLAGS	# Make available for sub-makes
export LIBS

EXTERNAL_IDIR = lib/headers
IDIR = include
SRCDIR = src
LIBDIR = lib
ODIR = obj
CC = gcc
CFLAGS = -Wall -Wextra -Werror -pedantic -fPIC -fvisibility=hidden -DMALLOC_IS_THREAD_SAFE -DFREE_IS_THREAD_SAFE
LD = ld

.PHONY: default all external_libs objdir public_header examples clean

default: $(TARGET)
all: default
external_libs:
	cd lib && ./get_external_libs.sh && cd ..
objdir:
	mkdir -p obj
public_header:
	cp $(IDIR)/SocketIOStream.h $(IDIR)/SocketIODatagram.h $(IDIR)/SocketIO.h .

HEADERS = $(wildcard $(IDIR)/*.h) $(wildcard $(EXTERNAL_IDIR)/*.h)
SOURCES = $(wildcard $(SRCDIR)/*.c)
OBJECTS = $(patsubst $(SRCDIR)/%.c, $(ODIR)/%.o, $(SOURCES))

$(ODIR)/%.o: $(SRCDIR)/%.c $(HEADERS)
	$(CC) -c $(CFLAGS) -I$(IDIR) -I$(EXTERNAL_IDIR) $< -o $@

$(TARGET): external_libs objdir public_header $(OBJECTS)
	$(LD) $(LDFLAGS) $(OBJECTS) -L$(LIBDIR) $(LIBS) -o $@

examples: $(TARGET)
	$(MAKE) -C examples

clean:
	rm -f $(TARGET)
	rm -f SocketIOStream.h SocketIODatagram.h SocketIO.h
	rm -rf obj
	cd lib && ./cleanup_libs.sh && cd ..
	$(MAKE) -C examples clean
