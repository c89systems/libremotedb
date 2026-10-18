#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

int rdb_io_try_write(int fd, const void *bytes, size_t length,
                     size_t *written) {
  ssize_t n;
  *written = 0;
#ifdef SO_NOSIGPIPE
  {
    int enabled;
    enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0)
      return RDB_TRANSPORT_ERROR;
  }
#endif
#ifdef MSG_NOSIGNAL
  n = send(fd, bytes, length, MSG_NOSIGNAL);
#else
  n = send(fd, bytes, length, 0);
#endif
  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
    return RDB_NO_DATA;
  if (n <= 0)
    return RDB_TRANSPORT_ERROR;
  *written = (size_t)n;
  return RDB_OK;
}

int rdb_io_read(int fd, void *bytes, size_t length) {
  unsigned char *p;
  ssize_t n;
  p = (unsigned char *)bytes;
  while (length != 0) {
    n = read(fd, p, length);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return RDB_TRANSPORT_ERROR;
    p += n;
    length -= (size_t)n;
  }
  return RDB_OK;
}
int rdb_io_write(int fd, const void *bytes, size_t length) {
  const unsigned char *p;
  ssize_t n;
  p = (const unsigned char *)bytes;
#ifdef SO_NOSIGPIPE
  {
    int enabled;
    enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0)
      return RDB_TRANSPORT_ERROR;
  }
#endif
  while (length != 0) {
#ifdef MSG_NOSIGNAL
    n = send(fd, p, length, MSG_NOSIGNAL);
#else
    n = send(fd, p, length, 0);
#endif
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return RDB_TRANSPORT_ERROR;
    p += n;
    length -= (size_t)n;
  }
  return RDB_OK;
}
int rdb_io_header(int fd, unsigned long limit, rdb_header *h) {
  unsigned char p[RDB_HEADER_SIZE];
  int status;
  status = rdb_io_read(fd, p, sizeof(p));
  return status == RDB_OK ? rdb_header_decode(p, limit, h) : status;
}
int rdb_io_send_header(int fd, unsigned int op, unsigned long flags,
                       unsigned long stmt, unsigned long len) {
  rdb_header h;
  unsigned char p[RDB_HEADER_SIZE];
  int status;
  h.version = 0;
  h.opcode = op;
  h.flags = flags;
  h.statement = stmt;
  h.length.low = len;
  h.length.high = 0;
  status = rdb_header_encode(&h, p);
  return status == RDB_OK ? rdb_io_write(fd, p, sizeof(p)) : status;
}
int rdb_limit(const rdb_config *c, unsigned long *limit) {
  *limit = c != NULL && c->frame_limit != 0 ? c->frame_limit
                                            : RDB_DEFAULT_FRAME_LIMIT;
  return *limit >= 32 && *limit <= RDB_DEFAULT_FRAME_LIMIT ? RDB_OK
                                                           : RDB_ARGUMENT_ERROR;
}
