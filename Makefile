CC ?= gcc
CFLAGS ?= -std=c17 -O2 -Wall -Wextra -g
SAN = -fsanitize=address,undefined -fno-omit-frame-pointer

build/test: kv.c kv.h test.c | build
	$(CC) $(CFLAGS) $(SAN) -o $@ kv.c test.c

build/bench: kv.c kv.h bench.c | build
	$(CC) $(CFLAGS) -O3 -pthread -o $@ kv.c bench.c

build:
	mkdir -p build

test: build/test
	./build/test

bench: build/bench
	./build/bench

.PHONY: test bench

fio:
	mkdir -p build/fio && cd build/fio && fio --output-format=json ../../fio/nvme.fio > ../fio.json
	jq -r '.jobs[] | (if .read.io_bytes > 0 then .read else .write end) as $$s | "\(.jobname): \($$s.iops|floor) IOPS, \($$s.bw_bytes/1e6|floor) MB/s, latency p50 \($$s.clat_ns.percentile."50.000000"/1000|floor) us, p99 \($$s.clat_ns.percentile."99.000000"/1000|floor) us\(if .sync.lat_ns.percentile then ", fdatasync p50 \(.sync.lat_ns.percentile."50.000000"/1000|floor) us p99 \(.sync.lat_ns.percentile."99.000000"/1000|floor) us" else "" end)"' build/fio.json

.PHONY: fio
