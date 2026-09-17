CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
LDLIBS ?= -pthread -lz

.PHONY: all clean

all: test_protocol

test_protocol: test_protocol.c ../../network.c ../../network.h \
               ../../protocol.c ../../protocol.h ../../common.h
	$(CC) $(CFLAGS) -pthread -I../.. ../../network.c ../../protocol.c \
		test_protocol.c -o $@ $(LDLIBS)

clean:
	rm -f test_protocol
