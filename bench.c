#define _GNU_SOURCE
#include "kv.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define N 1000000
#define VLEN 100
#define THREADS 8
#define GETS 2000000

static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}

static double load(void) {
  double l = 0;
  FILE *f = fopen("/proc/loadavg", "r");
  if (f) { if (fscanf(f, "%lf", &l) != 1) l = 0; fclose(f); }
  return l;
}

static void key(char *k, uint64_t i) { snprintf(k, 17, "k%015llu", (unsigned long long)i); }

static void val(char *v, uint64_t i) {
  for (int j = 0; j < VLEN; j++) v[j] = (char)('a' + (i * 7 + (uint64_t)j) % 26);
}

static kv *db;
static int failed;

static void *getter(void *arg) {
  uint64_t x = (uint64_t)(uintptr_t)arg * 0x9E3779B97F4A7C15ull + 1;
  char k[17], v[VLEN], want[VLEN];
  uint32_t n;
  for (int i = 0; i < GETS / THREADS; i++) {
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    uint64_t id = x % N;
    key(k, id);
    val(want, id);
    if (kv_get(db, k, 16, v, VLEN, &n) || n != VLEN || memcmp(v, want, VLEN)) failed = 1;
  }
  return NULL;
}

static double puts_synced(const char *path, int every, int count) {
  unlink(path);
  int err;
  kv *d = kv_open(path, NULL, &err);
  char k[17], v[VLEN];
  double t = now();
  for (int i = 0; i < count; i++) {
    key(k, (uint64_t)i);
    val(v, (uint64_t)i);
    kv_put(d, k, 16, v, VLEN);
    if ((i + 1) % every == 0) kv_sync(d);
  }
  kv_sync(d);
  t = now() - t;
  kv_close(d);
  return count / t;
}

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "bench.log";
  int err;
  printf("load %.1f\n", load());
  printf("put, fdatasync every record:      %9.0f records/s\n", puts_synced(path, 1, 2000));
  printf("put, fdatasync every 100 records: %9.0f records/s\n", puts_synced(path, 100, 200000));
  printf("put, fdatasync every 10,000:      %9.0f records/s\n", puts_synced(path, 10000, N));

  kv_stats st;
  double t = now();
  db = kv_open(path, &st, &err);
  t = now() - t;
  printf("open and rebuild index: %llu records, %.1f MB in %.3f s (%.0f MB/s)\n",
         (unsigned long long)st.records, st.bytes / 1e6, t, st.bytes / 1e6 / t);

  pthread_t th[THREADS];
  for (int threads = 1; threads <= THREADS; threads *= 8) {
    t = now();
    for (int i = 0; i < threads; i++) pthread_create(&th[i], NULL, getter, (void *)(uintptr_t)(i + 1));
    for (int i = 0; i < threads; i++) pthread_join(th[i], NULL);
    t = now() - t;
    printf("random get, %d thread%s: %9.0f gets/s (warm page cache)\n", threads, threads > 1 ? "s" : "",
           (double)GETS / THREADS * threads / t);
  }

  t = now();
  kv_scrub(path, &st);
  t = now() - t;
  printf("scrub: %llu records, %llu bad, %.0f MB/s\n", (unsigned long long)st.records, (unsigned long long)st.bad,
         st.bytes / 1e6 / t);

  char k[17], v[VLEN];
  for (uint64_t i = 0; i < N; i += 2) { key(k, i); val(v, i); kv_put(db, k, 16, v, VLEN); }
  kv_sync(db);
  uint64_t before = kv_size(db);
  t = now();
  kv_compact(db);
  t = now() - t;
  printf("compact after overwriting half: %.1f MB to %.1f MB in %.3f s\n", before / 1e6, kv_size(db) / 1e6, t);
  kv_close(db);
  unlink(path);
  printf("load %.1f, %s\n", load(), failed ? "GET MISMATCH" : "all gets matched");
  return failed;
}
