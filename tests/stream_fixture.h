#ifndef STREAM_FIXTURE_H
#define STREAM_FIXTURE_H
#include <remotedb/server.h>
/* Test-only generator/validator: one fixed block, no whole-value storage. */
typedef struct stream_fixture {
  rdb_backend backend;
  rdb_u64 length, input, output;
  unsigned char pattern[RDB_CHUNK_SIZE];
  unsigned long reads;
  size_t largest;
  int connects, disconnects, valid, mode;
  int notice_fd, gate_fd;
} stream_fixture;
void stream_fixture_init(stream_fixture *s, unsigned long mib);
#endif
