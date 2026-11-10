#define _GNU_SOURCE
#include "kv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static char dir[] = "/tmp/logkv-test-XXXXXX";
static char path[256];

static int get_str(kv *db, const char *k, char *out) {
  uint32_t n = 0;
  int r = kv_get(db, k, strlen(k), out, 255, &n);
  if (r == KV_OK) out[n] = 0;
  return r;
}

static void test_put_get_del_survive_reopen(void) {
  unlink(path);
  int err;
  kv *db = kv_open(path, NULL, &err);
  CHECK(db);
  CHECK(kv_put(db, "a", 1, "one", 3) == KV_OK);
  CHECK(kv_put(db, "b", 1, "two", 3) == KV_OK);
  CHECK(kv_put(db, "a", 1, "uno", 3) == KV_OK);
  CHECK(kv_del(db, "b", 1) == KV_OK);
  char v[256];
  CHECK(get_str(db, "a", v) == KV_OK && !strcmp(v, "uno"));
  CHECK(get_str(db, "b", v) == KV_MISSING);
  CHECK(kv_close(db) == KV_OK);

  kv_stats st;
  db = kv_open(path, &st, &err);
  CHECK(db && st.records == 4 && st.truncated == 0);
  CHECK(get_str(db, "a", v) == KV_OK && !strcmp(v, "uno"));
  CHECK(get_str(db, "b", v) == KV_MISSING);
  CHECK(kv_count(db) == 1);
  kv_close(db);
}

static void fill(kv *db, int n) {
  char k[32], v[64];
  for (int i = 0; i < n; i++) {
    int kl = snprintf(k, sizeof k, "key%06d", i), vl = snprintf(v, sizeof v, "value-%06d-%06d", i, i * 7);
    kv_put(db, k, kl, v, vl);
  }
}

static int check_prefix(kv *db, int n) {
  char k[32], v[64], got[256];
  for (int i = 0; i < n; i++) {
    snprintf(k, sizeof k, "key%06d", i);
    snprintf(v, sizeof v, "value-%06d-%06d", i, i * 7);
    if (get_str(db, k, got) != KV_OK || strcmp(got, v)) return 0;
  }
  return 1;
}

static long file_size(void) { FILE *f = fopen(path, "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fclose(f); return n; }

/* A crash can leave any prefix of the last write on disk, or zeros the file system allocated but never filled. */
static void test_torn_tail_is_cut_off(void) {
  unlink(path);
  int err;
  kv *db = kv_open(path, NULL, &err);
  fill(db, 100);
  kv_close(db);
  long full = file_size();
  long rec = full / 100; /* every record here has the same length */
  for (long cut = full - 3 * rec; cut < full; cut++) {
    db = kv_open(path, NULL, &err);
    fill(db, 100);
    kv_close(db);
    CHECK(truncate(path, cut) == 0);
    kv_stats st;
    db = kv_open(path, &st, &err);
    CHECK(db);
    if (!db) return;
    CHECK(st.records == (uint64_t)(cut / rec));
    CHECK(st.truncated == (uint64_t)(cut % rec));
    CHECK(check_prefix(db, (int)(cut / rec)));
    kv_close(db);
    CHECK(file_size() == cut / rec * rec);
    unlink(path);
  }
  db = kv_open(path, NULL, &err);
  fill(db, 10);
  kv_close(db);
  FILE *f = fopen(path, "ab");
  char z[5000] = {0};
  fwrite(z, 1, sizeof z, f);
  fclose(f);
  kv_stats st;
  db = kv_open(path, &st, &err);
  CHECK(db && st.records == 10 && st.truncated == sizeof z && check_prefix(db, 10));
  if (db) kv_close(db);
}

static void flip_byte(long at) {
  FILE *f = fopen(path, "r+b");
  fseek(f, at, SEEK_SET);
  int c = fgetc(f);
  fseek(f, at, SEEK_SET);
  fputc(c ^ 0x10, f);
  fclose(f);
}

static void test_bit_rot_is_refused_and_scrub_finds_it(void) {
  unlink(path);
  int err;
  kv *db = kv_open(path, NULL, &err);
  fill(db, 100);
  kv_close(db);
  long full = file_size(), rec = full / 100;
  kv_stats st;
  CHECK(kv_scrub(path, &st) == KV_OK && st.records == 100 && st.bad == 0 && st.bytes == (uint64_t)full);
  flip_byte(50 * rec + rec - 3);
  err = 0;
  CHECK(kv_open(path, NULL, &err) == NULL && err == KV_ECORRUPT);
  CHECK(file_size() == full);
  CHECK(kv_scrub(path, &st) == KV_ECORRUPT);
  CHECK(st.records == 99 && st.bad == 1 && st.first_bad == (uint64_t)(50 * rec));
}

static void test_crc32c_check_value(void) {
  CHECK(kv_crc32c(0, "123456789", 9) == 0xE3069283u);
  char big[1000];
  for (int i = 0; i < 1000; i++) big[i] = (char)(i * 31);
  CHECK(kv_crc32c(kv_crc32c(0, big, 333), big + 333, 667) == kv_crc32c(0, big, 1000));
}

/* The writer reports each count only after kv_sync returns, so every reported key must survive the kill. */
static void test_kill_mid_write_keeps_every_synced_key(void) {
  srand((unsigned)time(NULL));
  for (int round = 0; round < 20; round++) {
    unlink(path);
    int fds[2];
    CHECK(pipe(fds) == 0);
    pid_t pid = fork();
    if (pid == 0) {
      close(fds[0]);
      int err;
      kv *db = kv_open(path, NULL, &err);
      char k[32], v[64];
      for (int i = 0;; i++) {
        int kl = snprintf(k, sizeof k, "key%06d", i), vl = snprintf(v, sizeof v, "value-%06d-%06d", i, i * 7);
        kv_put(db, k, kl, v, vl);
        if (i % 50 == 49) {
          kv_sync(db);
          int n = i + 1;
          if (write(fds[1], &n, sizeof n) != sizeof n) _exit(1);
        }
      }
    }
    close(fds[1]);
    int acked = 0, n, want = 1 + rand() % 40;
    for (int got = 0; got < want && read(fds[0], &n, sizeof n) == sizeof n; got++) acked = n;
    usleep((useconds_t)(rand() % 3000));
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    while (read(fds[0], &n, sizeof n) == sizeof n) acked = n;
    close(fds[0]);
    int err;
    kv_stats st;
    kv *db = kv_open(path, &st, &err);
    CHECK(db);
    if (!db) return;
    CHECK(acked > 0 && kv_count(db) >= (uint64_t)acked);
    CHECK(check_prefix(db, (int)kv_count(db)));
    kv_close(db);
  }
}

static void test_compact_keeps_live_keys_and_shrinks_the_log(void) {
  unlink(path);
  int err;
  kv *db = kv_open(path, NULL, &err);
  for (int r = 0; r < 5; r++) fill(db, 1000);
  char k[32], v[256];
  for (int i = 0; i < 100; i++) { int kl = snprintf(k, sizeof k, "key%06d", i); kv_del(db, k, kl); }
  CHECK(kv_sync(db) == KV_OK);
  uint64_t before = kv_size(db);
  CHECK(kv_compact(db) == KV_OK);
  uint64_t rec = before / 5100 + 1;
  CHECK(kv_size(db) < 901 * rec && kv_size(db) == (uint64_t)file_size());
  CHECK(kv_count(db) == 900);
  CHECK(get_str(db, "key000050", v) == KV_MISSING);
  kv_put(db, "after", 5, "compact", 7);
  kv_close(db);
  char tmp[300];
  snprintf(tmp, sizeof tmp, "%s.compact", path);
  FILE *f = fopen(tmp, "wb");
  fputs("half-written compaction output", f);
  fclose(f);
  kv_stats st;
  db = kv_open(path, &st, &err);
  CHECK(db && kv_count(db) == 901 && st.truncated == 0);
  CHECK(access(tmp, F_OK) != 0);
  CHECK(get_str(db, "after", v) == KV_OK && !strcmp(v, "compact"));
  int ok = 1;
  for (int i = 100; i < 1000; i++) {
    char want[64];
    snprintf(k, sizeof k, "key%06d", i);
    snprintf(want, sizeof want, "value-%06d-%06d", i, i * 7);
    ok &= get_str(db, k, v) == KV_OK && !strcmp(v, want);
  }
  CHECK(ok);
  kv_close(db);
}

int main(void) {
  if (!mkdtemp(dir)) return 2;
  snprintf(path, sizeof path, "%s/db.log", dir);
  test_put_get_del_survive_reopen();
  test_torn_tail_is_cut_off();
  test_bit_rot_is_refused_and_scrub_finds_it();
  test_crc32c_check_value();
  test_kill_mid_write_keeps_every_synced_key();
  test_compact_keeps_live_keys_and_shrinks_the_log();
  printf(fails ? "FAIL %d\n" : "ok\n", fails);
  return fails != 0;
}
