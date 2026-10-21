# Curso de Ciência da Computação
# Prof. Dr. Rubens Barbosa Filho
#Ano: 2015

CC ?= gcc
CFLAGS ?= -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -O2 -I../../include

all: test_protocol

test_protocol: test_protocol.c ../../src/protocol.c
	$(CC) $(CFLAGS) -o test_protocol test_protocol.c ../../src/protocol.c

clean:
	rm -f test_protocol

.PHONY: all clean
