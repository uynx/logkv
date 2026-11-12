# logkv

logkv is a small key-value store in C17 that keeps every write in one append-only log file and is built to survive a crash at any line. Each record carries a CRC32C checksum, an in-memory hash index points at the newest copy of each key, and compaction copies only the live records into a new file and renames it over the old log.

## What a crash can and can't do

* A write is durable once `kv_sync` returns, which is one `fdatasync` on the log.
* A crash can leave any prefix of the last write on disk, or zeros the file system allocated but never filled. `kv_open` finds that torn tail and cuts it off.
* A record that fails its checksum with good data after it is not a crash, it is bit rot, so `kv_open` refuses the file with `KV_ECORRUPT` instead of silently skipping it. `kv_scrub` checks every record and reports the offset of the first bad one.
* A crash during `kv_compact` before the rename leaves the old log untouched.

## Tests

`make test` builds with AddressSanitizer and UndefinedBehaviorSanitizer and runs six tests. The main one forks a writer, kills it with SIGKILL at a random point 20 times, and checks that every key the writer reported after a sync is still there and in order. The others cover reopen, torn tails at every byte of the last three records, a flipped byte, the CRC32C check value and compaction.

## Numbers

Measured with `make bench` on an 8-thread laptop with an NVMe drive on November 12, 2026, with the load average between 9 and 12 from other jobs. One million records, each a 16-byte key and a 100-byte value.

| Operation | Result |
| --- | --- |
| Put, `fdatasync` after every record | 212 records/s |
| Put, `fdatasync` every 100 records | 20,634 records/s |
| Put, `fdatasync` every 10,000 records | 691,495 records/s |
| Open and rebuild the index (128 MB) | 0.26 s |
| Random get, 1 thread (warm page cache) | 319,564 gets/s |
| Random get, 8 threads (warm page cache) | 1,216,761 gets/s |
| Scrub every checksum | 6,325 MB/s |
| Compact after overwriting half the keys | 192 MB to 128 MB in 1.8 s |

A sync after every record is limited by the drive, not the code: `make fio` on November 10 measured `fdatasync` on the same drive at 1.22 ms median and 8.29 ms at p99, about 400 synced 4 KB writes per second. Batching 100 records per sync is about 100 times faster for that reason.

## Using it

```c
int err;
kv *db = kv_open("data.log", NULL, &err);
kv_put(db, "a", 1, "one", 3);
kv_sync(db);
char buf[64];
uint32_t n;
kv_get(db, "a", 1, buf, sizeof buf, &n);
kv_close(db);
```

`kv_get` can be called from many threads at once as long as no thread is writing. The full API is in `kv.h`.

## Building

`devenv shell` gives gcc, make, fio and jq. Then `make test`, `make bench` or `make fio`.
