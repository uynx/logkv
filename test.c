#define _GNU_SOURCE
#include "kv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

int main(void) {
  if (!mkdtemp(dir)) return 2;
  snprintf(path, sizeof path, "%s/db.log", dir);
  test_put_get_del_survive_reopen();
  printf(fails ? "FAIL %d\n" : "ok\n", fails);
  return fails != 0;
}
