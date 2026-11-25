CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2

test: test_serial
	./test_serial

test_serial: test_serial.c ../../main/serial.c ../../main/serial.h
	$(CC) $(CFLAGS) -I../../main -o $@ test_serial.c ../../main/serial.c

clean:
	rm -f test_serial

.PHONY: test clean
