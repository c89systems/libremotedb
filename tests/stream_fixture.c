#define _POSIX_C_SOURCE 200809L
#include "stream_fixture.h"
#include <errno.h>
#include <string.h>
#include <unistd.h>
static void notice(int fd) {
  char byte;
  ssize_t n;
  if (fd < 0)
    return;
  byte = 'x';
  do {
    n = write(fd, &byte, 1);
  } while (n < 0 && errno == EINTR);
  if (n != 1)
    _exit(90);
}
static int connect_stream(void *p) {
  stream_fixture *s;
  s = (stream_fixture *)p;
  ++s->connects;
  return RDB_OK;
}
static void disconnect_stream(void *p) { ++((stream_fixture *)p)->disconnects; }
static int begin(void *p) {
  stream_fixture *s;
  s = (stream_fixture *)p;
  memset(&s->input, 0, sizeof(s->input));
  memset(&s->output, 0, sizeof(s->output));
  return RDB_OK;
}
static int sql(void *p, const void *bytes, size_t n) {
  (void)p;
  (void)bytes;
  (void)n;
  return RDB_OK;
}
static int end(void *p) {
  (void)p;
  return RDB_OK;
}
static int execute(void *p, rdb_reply *r) {
  stream_fixture *s;
  s = (stream_fixture *)p;
  if (s->mode == 1)
    _exit(73); /* Simulated driver worker crash during execute. */
  r->parameter = 1;
  return RDB_NEED_INPUT;
}
static int param(void *p, const void *bytes, size_t n) {
  stream_fixture *s;
  const unsigned char *b;
  size_t i;
  char byte;
  ssize_t k;
  s = (stream_fixture *)p;
  if (s->mode == 2)
    _exit(73);
  notice(s->notice_fd);
  if (s->gate_fd >= 0) {
    do {
      k = read(s->gate_fd, &byte, 1);
    } while (k < 0 && errno == EINTR);
    if (k != 1)
      return RDB_ERROR;
  }
  b = (const unsigned char *)bytes;
  for (i = 0; i < n; ++i)
    if (b[i] != s->pattern[(s->input.low + (unsigned long)i) & 255UL])
      s->valid = 0;
  if (n > s->largest)
    s->largest = n;
  if (rdb_u64_add(&s->input, (unsigned long)n) != RDB_OK)
    return RDB_ERROR;
  return s->valid ? RDB_OK : RDB_ERROR;
}
static int param_end(void *p, rdb_reply *r) {
  stream_fixture *s;
  s = (stream_fixture *)p;
  r->value = s->length;
  return rdb_u64_compare(s->input, s->length) == 0 ? RDB_OK : RDB_ERROR;
}
static int read_stream(void *p, void *bytes, size_t cap, size_t *n) {
  stream_fixture *s;
  size_t i;
  unsigned char *b;
  s = (stream_fixture *)p;
  if (s->mode == 3)
    _exit(73);
  *n = 0;
  if (rdb_u64_compare(s->output, s->length) >= 0)
    return RDB_NO_DATA;
  if (s->output.high == s->length.high &&
      s->length.low - s->output.low < (unsigned long)cap)
    cap = (size_t)(s->length.low - s->output.low);
  b = (unsigned char *)bytes;
  for (i = 0; i < cap; ++i)
    b[i] = s->pattern[(s->output.low + (unsigned long)i) & 255UL];
  if (cap > s->largest)
    s->largest = cap;
  ++s->reads;
  if (rdb_u64_add(&s->output, (unsigned long)cap) != RDB_OK)
    return RDB_ERROR;
  *n = cap;
  notice(s->notice_fd);
  return RDB_OK;
}
static void reset(void *p) { (void)begin(p); }
void stream_fixture_init(stream_fixture *s, unsigned long mib) {
  size_t i;
  memset(s, 0, sizeof(*s));
  s->length.high = mib / 4096UL;
  s->length.low = (mib % 4096UL) * 1048576UL + 17UL;
  s->notice_fd = s->gate_fd = -1;
  s->valid = 1;
  for (i = 0; i < sizeof(s->pattern); ++i)
    s->pattern[i] = (unsigned char)i;
  s->backend.context = s;
  s->backend.connect = connect_stream;
  s->backend.disconnect = disconnect_stream;
  s->backend.sql_begin = begin;
  s->backend.sql_write = sql;
  s->backend.sql_end = end;
  s->backend.execute = execute;
  s->backend.param_write = param;
  s->backend.param_end = param_end;
  s->backend.read = read_stream;
  s->backend.reset = reset;
}
