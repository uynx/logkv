#define _GNU_SOURCE
#include "kv.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define HDR 12u
#define TOMB UINT32_MAX
#define BUFCAP (1u << 20)
#define MAXREC (64u << 20)

typedef struct { uint64_t hash, off; uint32_t klen, vlen; } slot;

struct kv {
  int fd;
  char *path;
  slot *tab;
  uint64_t cap, used, live;
  uint64_t end, flushed;
  uint8_t *buf;
  uint32_t blen;
};

static char *tmp_path(const char *path);
static uint32_t crc_table[8][256];

static void crc_init(void) {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0x82F63B78u : c >> 1;
    crc_table[0][i] = c;
  }
  for (uint32_t i = 0; i < 256; i++)
    for (int t = 1; t < 8; t++) crc_table[t][i] = (crc_table[t - 1][i] >> 8) ^ crc_table[0][crc_table[t - 1][i] & 0xff];
}

static uint32_t crc_sw(uint32_t c, const uint8_t *p, size_t n) {
  static int ready;
  if (!ready) { crc_init(); ready = 1; }
  while (n--) c = (c >> 8) ^ crc_table[0][(c ^ *p++) & 0xff];
  return c;
}

#if defined(__x86_64__)
__attribute__((target("sse4.2"))) static uint32_t crc_hw(uint32_t c, const uint8_t *p, size_t n) {
  uint64_t c64 = c;
  for (; n >= 8; n -= 8, p += 8) {
    uint64_t w;
    memcpy(&w, p, 8);
    c64 = __builtin_ia32_crc32di(c64, w);
  }
  c = (uint32_t)c64;
  while (n--) c = __builtin_ia32_crc32qi(c, *p++);
  return c;
}
#endif

uint32_t kv_crc32c(uint32_t crc, const void *p, size_t n) {
  crc = ~crc;
#if defined(__x86_64__)
  if (__builtin_cpu_supports("sse4.2")) return ~crc_hw(crc, p, n);
#endif
  return ~crc_sw(crc, p, n);
}

static uint64_t hash(const void *k, uint32_t n) {
  const uint8_t *p = k;
  uint64_t h = 0xcbf29ce484222325ull;
  for (uint32_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
  h ^= h >> 33; h *= 0xff51afd7ed558ccdull; h ^= h >> 33;
  return h | 1;
}

static uint64_t reclen(uint32_t klen, uint32_t vlen) { return HDR + klen + (vlen == TOMB ? 0 : vlen); }

static int read_at(kv *db, uint64_t off, void *dst, uint64_t n) {
  if (off >= db->flushed) { memcpy(dst, db->buf + (off - db->flushed), n); return 0; }
  uint8_t *d = dst;
  while (n) {
    ssize_t r = pread(db->fd, d, n, (off_t)off);
    if (r <= 0) { if (r < 0 && errno == EINTR) continue; return KV_EIO; }
    d += r; off += (uint64_t)r; n -= (uint64_t)r;
  }
  return 0;
}

static int write_all(int fd, const uint8_t *p, uint64_t n, uint64_t off) {
  while (n) {
    ssize_t w = pwrite(fd, p, n, (off_t)off);
    if (w < 0) { if (errno == EINTR) continue; return KV_EIO; }
    p += w; off += (uint64_t)w; n -= (uint64_t)w;
  }
  return 0;
}

static int flush(kv *db) {
  if (!db->blen) return 0;
  if (write_all(db->fd, db->buf, db->blen, db->flushed)) return KV_EIO;
  db->flushed += db->blen;
  db->blen = 0;
  return 0;
}

/* Returns the slot holding key, or the empty slot where it would go. */
static slot *find(kv *db, const void *key, uint32_t klen, uint64_t h) {
  uint8_t tmp[256];
  for (uint64_t i = h & (db->cap - 1);; i = (i + 1) & (db->cap - 1)) {
    slot *s = &db->tab[i];
    if (!s->hash) return s;
    if (s->hash != h || s->klen != klen) continue;
    uint8_t *k = klen <= sizeof tmp ? tmp : malloc(klen);
    int same = k && !read_at(db, s->off + HDR, k, klen) && !memcmp(k, key, klen);
    if (k != tmp) free(k);
    if (same) return s;
  }
}

static int grow(kv *db) {
  uint64_t ncap = db->cap ? db->cap * 2 : 1024;
  slot *nt = calloc(ncap, sizeof *nt);
  if (!nt) return KV_ENOMEM;
  for (uint64_t i = 0; i < db->cap; i++) {
    slot s = db->tab[i];
    if (!s.hash) continue;
    uint64_t j = s.hash & (ncap - 1);
    while (nt[j].hash) j = (j + 1) & (ncap - 1);
    nt[j] = s;
  }
  free(db->tab);
  db->tab = nt;
  db->cap = ncap;
  return 0;
}

static int index_rec(kv *db, const void *key, uint32_t klen, uint32_t vlen, uint64_t off) {
  if ((db->used + 1) * 2 > db->cap && grow(db)) return KV_ENOMEM;
  uint64_t h = hash(key, klen);
  slot *s = find(db, key, klen, h);
  if (!s->hash) {
    if (vlen == TOMB) return 0;
    db->used++;
    db->live++;
  } else {
    db->live += (s->vlen == TOMB) - (vlen == TOMB);
  }
  *s = (slot){h, off, klen, vlen};
  return 0;
}

static int append(kv *db, const void *key, uint32_t klen, const void *val, uint32_t vlen) {
  uint64_t n = reclen(klen, vlen);
  if (db->blen + n > BUFCAP && flush(db)) return KV_EIO;
  uint8_t *rec = n <= BUFCAP ? db->buf + db->blen : malloc(n);
  if (!rec) return KV_ENOMEM;
  memcpy(rec + 4, &klen, 4);
  memcpy(rec + 8, &vlen, 4);
  memcpy(rec + HDR, key, klen);
  if (vlen != TOMB) memcpy(rec + HDR + klen, val, vlen);
  uint32_t c = kv_crc32c(0, rec + 4, n - 4);
  memcpy(rec, &c, 4);
  uint64_t off = db->end;
  if (n <= BUFCAP) {
    db->blen += (uint32_t)n;
  } else {
    int r = write_all(db->fd, rec, n, db->flushed);
    free(rec);
    if (r) return r;
    db->flushed += n;
  }
  db->end += n;
  return index_rec(db, key, klen, vlen, off);
}

int kv_put(kv *db, const void *key, uint32_t klen, const void *val, uint32_t vlen) {
  if (!klen || vlen == TOMB || (uint64_t)klen + vlen > MAXREC) return KV_EARG;
  return append(db, key, klen, val, vlen);
}

int kv_del(kv *db, const void *key, uint32_t klen) {
  if (!klen || klen > MAXREC) return KV_EARG;
  slot *s = find(db, key, klen, hash(key, klen));
  if (!s->hash || s->vlen == TOMB) return KV_MISSING;
  return append(db, key, klen, NULL, TOMB);
}

int kv_get(kv *db, const void *key, uint32_t klen, void *buf, uint32_t cap, uint32_t *vlen) {
  if (!klen || !db->cap) return klen ? KV_MISSING : KV_EARG;
  slot *s = find(db, key, klen, hash(key, klen));
  if (!s->hash || s->vlen == TOMB) return KV_MISSING;
  *vlen = s->vlen;
  if (s->vlen > cap) return KV_EARG;
  return read_at(db, s->off + HDR + klen, buf, s->vlen);
}

int kv_sync(kv *db) {
  if (flush(db)) return KV_EIO;
  return fdatasync(db->fd) ? KV_EIO : 0;
}

uint64_t kv_count(const kv *db) { return db->live; }
uint64_t kv_size(const kv *db) { return db->end; }

/* Walks records in [0, size). Returns the offset where the walk stopped. */
static uint64_t walk(const uint8_t *m, uint64_t size, int (*fn)(void *, const uint8_t *, uint64_t), void *arg, uint64_t *count) {
  uint64_t off = 0;
  while (off + HDR <= size) {
    uint32_t c, klen, vlen;
    memcpy(&c, m + off, 4);
    memcpy(&klen, m + off + 4, 4);
    memcpy(&vlen, m + off + 8, 4);
    uint64_t n = reclen(klen, vlen);
    if (!klen || n - HDR > MAXREC || off + n > size || kv_crc32c(0, m + off + 4, n - 4) != c) break;
    if (fn && fn(arg, m + off, off)) break;
    off += n;
    ++*count;
  }
  return off;
}

/* A crash leaves a short last record or zeroed blocks; a bad record with more data after it is corruption. */
static int is_torn(const uint8_t *m, uint64_t off, uint64_t size) {
  if (off + HDR > size) return 1;
  uint32_t klen, vlen;
  memcpy(&klen, m + off + 4, 4);
  memcpy(&vlen, m + off + 8, 4);
  uint64_t n = reclen(klen, vlen);
  if (off + n >= size) return 1;
  for (uint64_t i = off; i < size; i++)
    if (m[i]) return 0;
  return 1;
}

static int load_rec(void *arg, const uint8_t *r, uint64_t off) {
  uint32_t klen, vlen;
  memcpy(&klen, r + 4, 4);
  memcpy(&vlen, r + 8, 4);
  return index_rec(arg, r + HDR, klen, vlen, off);
}

kv *kv_open(const char *path, kv_stats *st, int *err) {
  kv_stats tmp = {0};
  if (!st) st = &tmp;
  memset(st, 0, sizeof *st);
  kv *db = calloc(1, sizeof *db);
  int e = KV_ENOMEM;
  if (!db || !(db->buf = malloc(BUFCAP)) || !(db->path = strdup(path))) goto fail;
  e = KV_EIO;
  char *stale = tmp_path(path);
  if (stale) unlink(stale);
  free(stale);
  if ((db->fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644)) < 0) goto fail;
  struct stat sb;
  if (fstat(db->fd, &sb)) goto fail;
  uint64_t size = (uint64_t)sb.st_size;
  db->flushed = db->end = size;
  if (grow(db)) { e = KV_ENOMEM; goto fail; }
  if (size) {
    uint8_t *m = mmap(NULL, size, PROT_READ, MAP_PRIVATE, db->fd, 0);
    if (m == MAP_FAILED) goto fail;
    madvise(m, size, MADV_SEQUENTIAL);
    uint64_t good = walk(m, size, load_rec, db, &st->records);
    int torn = good < size && is_torn(m, good, size);
    munmap(m, size);
    if (good != size && !torn) { e = KV_ECORRUPT; goto fail; }
    if (torn && (ftruncate(db->fd, (off_t)good) || fsync(db->fd))) goto fail;
    st->bytes = db->flushed = db->end = good;
    st->truncated = size - good;
  }
  return db;
fail:
  if (err) *err = e;
  if (db) { if (db->fd > 0) close(db->fd); free(db->buf); free(db->path); free(db->tab); free(db); }
  return NULL;
}

int kv_close(kv *db) {
  int r = kv_sync(db);
  close(db->fd);
  free(db->buf);
  free(db->path);
  free(db->tab);
  free(db);
  return r;
}

static char *tmp_path(const char *path) {
  size_t n = strlen(path) + sizeof ".compact";
  char *t = malloc(n);
  if (t) snprintf(t, n, "%s.compact", path);
  return t;
}

static int sync_dir(const char *path) {
  char *d = strdup(path);
  if (!d) return KV_ENOMEM;
  int fd = open(dirname(d), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  free(d);
  if (fd < 0) return KV_EIO;
  int r = fsync(fd);
  close(fd);
  return r ? KV_EIO : 0;
}

/* Rewrites live records into a new file and renames it over the log; a crash before the rename leaves the old log intact. */
int kv_compact(kv *db) {
  if (kv_sync(db)) return KV_EIO;
  char *tmp = tmp_path(db->path);
  slot *nt = calloc(db->cap, sizeof *nt);
  int fd = tmp ? open(tmp, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644) : -1;
  int r = KV_EIO;
  if (!nt || fd < 0) goto out;
  uint64_t off = 0, flushed = 0;
  uint32_t blen = 0;
  for (uint64_t i = 0; i < db->cap; i++) {
    slot s = db->tab[i];
    if (!s.hash || s.vlen == TOMB) continue;
    uint64_t n = reclen(s.klen, s.vlen);
    if (blen + n > BUFCAP) {
      if (write_all(fd, db->buf, blen, flushed)) goto out;
      flushed += blen;
      blen = 0;
    }
    uint8_t *rec = n <= BUFCAP ? db->buf + blen : malloc(n);
    if (!rec || read_at(db, s.off, rec, n)) { if (n > BUFCAP) free(rec); goto out; }
    if (n > BUFCAP) {
      int w = write_all(fd, rec, n, flushed);
      free(rec);
      if (w) goto out;
      flushed += n;
    } else {
      blen += (uint32_t)n;
    }
    uint64_t j = s.hash & (db->cap - 1);
    while (nt[j].hash) j = (j + 1) & (db->cap - 1);
    nt[j] = (slot){s.hash, off, s.klen, s.vlen};
    off += n;
  }
  if (write_all(fd, db->buf, blen, flushed) || fdatasync(fd) || rename(tmp, db->path) || sync_dir(db->path)) goto out;
  close(db->fd);
  db->fd = fd;
  fd = -1;
  free(db->tab);
  db->tab = nt;
  nt = NULL;
  db->used = db->live;
  db->end = db->flushed = off;
  r = 0;
out:
  if (fd >= 0) { close(fd); unlink(tmp); }
  free(nt);
  free(tmp);
  return r;
}
int kv_scrub(const char *path, kv_stats *st) {
  memset(st, 0, sizeof *st);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return KV_EIO;
  struct stat sb;
  if (fstat(fd, &sb)) { close(fd); return KV_EIO; }
  uint64_t size = (uint64_t)sb.st_size, off = 0;
  uint8_t *m = size ? mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0) : NULL;
  close(fd);
  if (m == MAP_FAILED) return KV_EIO;
  if (size) madvise(m, size, MADV_SEQUENTIAL);
  while (off < size) {
    uint64_t good = walk(m + off, size - off, NULL, NULL, &st->records);
    st->bytes += good;
    off += good;
    if (off == size) break;
    if (!st->bad++) st->first_bad = off;
    uint32_t klen, vlen;
    if (off + HDR > size) break;
    memcpy(&klen, m + off + 4, 4);
    memcpy(&vlen, m + off + 8, 4);
    uint64_t n = reclen(klen, vlen);
    if (!klen || n - HDR > MAXREC || off + n > size) break; /* header itself is damaged, nothing after it can be framed */
    off += n;
  }
  if (size) munmap(m, size);
  return st->bad ? KV_ECORRUPT : KV_OK;
}
