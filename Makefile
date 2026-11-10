CC ?= gcc
CFLAGS ?= -std=c17 -O2 -Wall -Wextra -g
SAN = -fsanitize=address,undefined -fno-omit-frame-pointer

build/test: kv.c kv.h test.c | build
	$(CC) $(CFLAGS) $(SAN) -o $@ kv.c test.c

build/bench: kv.c kv.h bench.c | build
	$(CC) $(CFLAGS) -O3 -march=native -pthread -o $@ kv.c bench.c

build:
	mkdir -p build

test: build/test
	./build/test

bench: build/bench
	./build/bench

.PHONY: test bench
