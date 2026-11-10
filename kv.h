#ifndef KV_H
#define KV_H
#include <stddef.h>
#include <stdint.h>

typedef struct kv kv;

enum { KV_OK = 0, KV_MISSING = 1, KV_EIO = -1, KV_ECORRUPT = -2, KV_EARG = -3, KV_ENOMEM = -4 };

typedef struct {
  uint64_t records;   /* valid records read during open or scrub */
  uint64_t bytes;     /* bytes kept */
  uint64_t truncated; /* torn-tail bytes cut off by open */
  uint64_t bad;       /* records failing their checksum (scrub) */
  uint64_t first_bad; /* offset of the first bad record, valid when bad > 0 */
} kv_stats;

/* Readers may call kv_get from many threads at once while no thread writes. */
kv *kv_open(const char *path, kv_stats *st, int *err);
int kv_put(kv *db, const void *key, uint32_t klen, const void *val, uint32_t vlen);
int kv_del(kv *db, const void *key, uint32_t klen);
int kv_get(kv *db, const void *key, uint32_t klen, void *buf, uint32_t cap, uint32_t *vlen);
int kv_sync(kv *db);
int kv_compact(kv *db);
uint64_t kv_count(const kv *db);
uint64_t kv_size(const kv *db);
int kv_close(kv *db);

int kv_scrub(const char *path, kv_stats *st);
uint32_t kv_crc32c(uint32_t crc, const void *p, size_t n);
#endif
