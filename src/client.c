#define _POSIX_C_SOURCE 200809L
#include "endpoint_internal.h"
#include "internal.h"
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct rdb_connection {
  int fd;
  int dead;
  unsigned long limit;
  rdb_reply reply;
};
static int failed(rdb_connection *c, int status, int uncertain) {
  c->dead = 1;
  c->reply.status = status;
  if (uncertain && status == RDB_TRANSPORT_ERROR) {
    c->reply.status = RDB_OUTCOME_UNKNOWN;
    return RDB_OUTCOME_UNKNOWN;
  }
  return status;
}
static int request(rdb_connection *c, unsigned int op, const void *data,
                   size_t len, int uncertain) {
  rdb_header h;
  unsigned char p[16];
  int s;
  unsigned long stmt;
  if (c == NULL || (len != 0 && data == NULL))
    return RDB_ARGUMENT_ERROR;
  if (c->dead)
    return RDB_STATE_ERROR;
  stmt = op >= RDB_SQL_BEGIN ? 1 : 0;
  s = rdb_io_send_header(c->fd, op, 0, stmt, (unsigned long)len);
  if (s == RDB_OK)
    s = rdb_io_write(c->fd, data, len);
  if (s == RDB_OK)
    s = rdb_io_header(c->fd, c->limit, &h);
  if (s != RDB_OK)
    return failed(c, s, uncertain);
  if (h.flags != RDB_RESPONSE_FLAG || h.opcode != op || h.statement != stmt ||
      h.length.low != sizeof(p))
    return failed(c, RDB_PROTOCOL_ERROR, 0);
  s = rdb_io_read(c->fd, p, sizeof(p));
  if (s == RDB_OK)
    s = rdb_reply_decode(p, &c->reply);
  if (s != RDB_OK)
    return failed(c, s, uncertain);
  if (c->reply.status == RDB_NO_DATA ||
      (c->reply.status == RDB_NEED_INPUT &&
       ((op != RDB_EXECUTE && op != RDB_PARAM_END) || c->reply.parameter == 0)))
    return failed(c, RDB_PROTOCOL_ERROR, 0);
  return c->reply.status;
}
int rdb_open_fd(int fd, const rdb_config *config, rdb_connection **out) {
  rdb_connection *c;
  unsigned long limit;
  unsigned char hello[8];
  int s;
  if (out == NULL)
    return RDB_ARGUMENT_ERROR;
  *out = NULL;
  if (fd < 0 || rdb_limit(config, &limit) != RDB_OK)
    return RDB_ARGUMENT_ERROR;
  c = (rdb_connection *)calloc(1, sizeof(*c));
  if (c == NULL)
    return RDB_MEMORY_ERROR;
  c->fd = fd;
  c->limit = limit;
  rdb_put32(hello, 0);
  rdb_put32(hello + 4, limit);
  s = request(c, RDB_HELLO, hello, sizeof(hello), 0);
  if (s == RDB_OK) {
    if (c->reply.value.high != 0 || c->reply.value.low < 32 ||
        c->reply.value.low > limit)
      s = RDB_PROTOCOL_ERROR;
    else
      c->limit = c->reply.value.low;
  }
  if (s != RDB_OK) {
    free(c);
    return s;
  }
  *out = c;
  return RDB_OK;
}
int rdb_open_endpoint(const rdb_endpoint *endpoint, const rdb_config *config,
                      rdb_connection **out) {
  int fd, s;
  if (out == NULL)
    return RDB_ARGUMENT_ERROR;
  *out = NULL;
  s = rdb_endpoint_connect(endpoint, &fd);
  if (s != RDB_OK)
    return s;
  s = rdb_open_fd(fd, config, out);
  if (s != RDB_OK)
    close(fd);
  return s;
}
int rdb_open_key(const rdb_endpoint *endpoint, const char *key,
                 const rdb_config *config, rdb_connection **out) {
  int fd, status;
  size_t i;
  unsigned char accepted;
  if (out == NULL)
    return RDB_ARGUMENT_ERROR;
  *out = NULL;
  if (key == NULL || strlen(key) != 64)
    return RDB_ARGUMENT_ERROR;
  for (i = 0; i < 64; ++i)
    if (!((key[i] >= '0' && key[i] <= '9') || (key[i] >= 'a' && key[i] <= 'f')))
      return RDB_ARGUMENT_ERROR;
  status = rdb_endpoint_connect(endpoint, &fd);
  if (status != RDB_OK)
    return status;
  status = rdb_io_write(fd, key, 64);
  if (status == RDB_OK)
    status = rdb_io_read(fd, &accepted, 1);
  if (status == RDB_OK && accepted != 0)
    status = RDB_ERROR;
  if (status == RDB_OK)
    status = rdb_open_fd(fd, config, out);
  if (status != RDB_OK)
    close(fd);
  return status;
}
int rdb_open(const char *path, const rdb_config *config, rdb_connection **out) {
  rdb_endpoint endpoint;
  endpoint.kind = RDB_PATHNAME;
  endpoint.address = path;
  return rdb_open_endpoint(&endpoint, config, out);
}

void rdb_close(rdb_connection *c) {
  if (c != NULL) {
    close(c->fd);
    free(c);
  }
}
const rdb_reply *rdb_connection_reply(const rdb_connection *c) {
  return c != NULL ? &c->reply : NULL;
}
int rdb_connection_connect(rdb_connection *c) {
  return request(c, RDB_CONNECT, NULL, 0, 0);
}
int rdb_connection_disconnect(rdb_connection *c) {
  int s;
  s = request(c, RDB_DISCONNECT, NULL, 0, 0);
  if (c != NULL)
    c->dead = 1;
  return s;
}
int rdb_connection_sql_begin(rdb_connection *c) {
  return request(c, RDB_SQL_BEGIN, NULL, 0, 0);
}
static int write_chunks(rdb_connection *c, unsigned int op, const void *bytes,
                        size_t length, int uncertain) {
  const unsigned char *p;
  size_t n;
  int s;
  if (c == NULL || (length != 0 && bytes == NULL))
    return RDB_ARGUMENT_ERROR;
  p = (const unsigned char *)bytes;
  while (length != 0) {
    n = length > c->limit ? (size_t)c->limit : length;
    s = request(c, op, p, n, uncertain);
    if (s != RDB_OK)
      return s;
    p += n;
    length -= n;
  }
  return c->dead ? RDB_STATE_ERROR : RDB_OK;
}
int rdb_connection_sql_write(rdb_connection *c, const void *p, size_t n) {
  return write_chunks(c, RDB_SQL_DATA, p, n, 0);
}
int rdb_connection_sql_end(rdb_connection *c) {
  return request(c, RDB_SQL_END, NULL, 0, 0);
}
int rdb_connection_execute(rdb_connection *c) {
  return request(c, RDB_EXECUTE, NULL, 0, 1);
}
int rdb_connection_param_write(rdb_connection *c, const void *p, size_t n) {
  return write_chunks(c, RDB_PARAM_DATA, p, n, 1);
}
int rdb_connection_param_end(rdb_connection *c) {
  return request(c, RDB_PARAM_END, NULL, 0, 1);
}
int rdb_connection_reset(rdb_connection *c) {
  return request(c, RDB_RESET, NULL, 0, 0);
}
int rdb_connection_column_read(rdb_connection *c, void *bytes, size_t capacity,
                               size_t *read, int *eof) {
  rdb_header h;
  unsigned char p[16], count[4];
  size_t n;
  int s;
  if (read != NULL)
    *read = 0;
  if (eof != NULL)
    *eof = 0;
  if (c == NULL || bytes == NULL || capacity == 0 || read == NULL ||
      eof == NULL)
    return RDB_ARGUMENT_ERROR;
  if (c->dead)
    return RDB_STATE_ERROR;
  n = capacity > c->limit - 16 ? (size_t)c->limit - 16 : capacity;
  rdb_put32(count, (unsigned long)n);
  s = rdb_io_send_header(c->fd, RDB_COLUMN_READ, 0, 1, sizeof(count));
  if (s == RDB_OK)
    s = rdb_io_write(c->fd, count, sizeof(count));
  if (s == RDB_OK)
    s = rdb_io_header(c->fd, c->limit, &h);
  if (s != RDB_OK)
    return failed(c, s, 0);
  if (h.opcode != RDB_COLUMN_READ || h.flags != RDB_RESPONSE_FLAG ||
      h.statement != 1 || h.length.low < 16 || h.length.low - 16 > n)
    return failed(c, RDB_PROTOCOL_ERROR, 0);
  s = rdb_io_read(c->fd, p, sizeof(p));
  if (s == RDB_OK)
    s = rdb_reply_decode(p, &c->reply);
  if (s != RDB_OK)
    return failed(c, s, 0);
  n = (size_t)h.length.low - 16;
  if ((c->reply.status != RDB_OK && n != 0) ||
      (c->reply.status == RDB_OK && n == 0))
    return failed(c, RDB_PROTOCOL_ERROR, 0);
  s = rdb_io_read(c->fd, bytes, n);
  if (s != RDB_OK)
    return failed(c, s, 0);
  *read = n;
  *eof = c->reply.status == RDB_NO_DATA;
  return c->reply.status;
}
