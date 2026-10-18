#ifndef STREAM_CHECKS_H
#define STREAM_CHECKS_H
#include <remotedb/endpoint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#define REQUIRE(x)                                                             \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                  \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)
static void prepare_stream(rdb_connection *c) {
  const char *sql;
  sql = "INSERT INTO documents(payload) VALUES (?)";
  REQUIRE(rdb_connection_connect(c) == RDB_OK);
  REQUIRE(rdb_connection_sql_begin(c) == RDB_OK);
  REQUIRE(rdb_connection_sql_write(c, sql, strlen(sql)) == RDB_OK);
  REQUIRE(rdb_connection_sql_end(c) == RDB_OK);
}
static void upload_stream(rdb_connection *c, unsigned long mib) {
  unsigned char block[65536];
  unsigned long i, j;
  for (i = 0; i < sizeof(block); ++i)
    block[i] = (unsigned char)i;
  for (j = 0; j < mib * 16UL; ++j)
    REQUIRE(rdb_connection_param_write(c, block, sizeof(block)) == RDB_OK);
  REQUIRE(rdb_connection_param_write(c, block, 17) == RDB_OK);
  REQUIRE(rdb_connection_param_end(c) == RDB_OK);
}
static void download_stream(rdb_connection *c, unsigned long mib) {
  unsigned char block[4096];
  size_t n, i;
  int status, eof;
  rdb_u64 received, expected;
  memset(&received, 0, sizeof(received));
  expected.high = mib / 4096UL;
  expected.low = (mib % 4096UL) * 1048576UL + 17UL;
  REQUIRE(rdb_u64_compare(rdb_connection_reply(c)->value, expected) == 0);
  for (;;) {
    status = rdb_connection_column_read(c, block, sizeof(block), &n, &eof);
    if (status == RDB_NO_DATA) {
      REQUIRE(eof && n == 0);
      break;
    }
    REQUIRE(status == RDB_OK && !eof && n > 0 && n <= sizeof(block));
    for (i = 0; i < n; ++i)
      REQUIRE(block[i] ==
              (unsigned char)((received.low + (unsigned long)i) & 255UL));
    REQUIRE(rdb_u64_add(&received, (unsigned long)n) == RDB_OK);
  }
  REQUIRE(rdb_u64_compare(received, expected) == 0);
}
static long rss_kib(void) {
  struct rusage r;
  REQUIRE(getrusage(RUSAGE_SELF, &r) == 0);
#ifdef __APPLE__
  return r.ru_maxrss / 1024;
#else
  return r.ru_maxrss;
#endif
}
static void memory_bound(long before, long after) {
  /* Memcheck owns its own address space and allocation accounting. */
  if (getenv("REMOTEDB_MEMORY_CHECK") == NULL)
    REQUIRE(after - before <= 8192);
}
#endif
