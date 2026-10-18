#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include <errno.h>
#include <string.h>
#include <unistd.h>

/* Each state denotes which backend calls are legal. SQL failure poisons the
 * statement until reset; data-at-execution prohibits unrelated operations. */
enum state { IDLE, SQL, READY, INPUT, OUTPUT, BROKEN };
static int remote_status(int s) { return s >= RDB_OK && s <= RDB_ERROR; }
static int send_reply(int fd, const rdb_header *h, rdb_reply *r,
                      const void *data, size_t length) {
  unsigned char p[16];
  int s;
  if (!remote_status(r->status) || r->parameter > 0xffffffffUL ||
      r->value.low > 0xffffffffUL || r->value.high > 0xffffffffUL)
    return RDB_PROTOCOL_ERROR;
  rdb_reply_encode(r, p);
  s = rdb_io_send_header(fd, h->opcode, RDB_RESPONSE_FLAG, h->statement,
                         16 + (unsigned long)length);
  if (s == RDB_OK)
    s = rdb_io_write(fd, p, sizeof(p));
  if (s == RDB_OK)
    s = rdb_io_write(fd, data, length);
  return s;
}
static int consume(int fd, unsigned long remaining, const rdb_backend *b,
                   int kind, int *remote) {
  unsigned char data[RDB_CHUNK_SIZE];
  size_t n;
  int s;
  *remote = RDB_OK;
  while (remaining != 0) {
    n = remaining > sizeof(data) ? sizeof(data) : (size_t)remaining;
    s = rdb_io_read(fd, data, n);
    if (s != RDB_OK)
      return s;
    if (*remote == RDB_OK && kind == 1)
      *remote = b->sql_write(b->context, data, n);
    if (*remote == RDB_OK && kind == 2)
      *remote = b->param_write(b->context, data, n);
    if (!remote_status(*remote) || *remote == RDB_NEED_INPUT ||
        *remote == RDB_NO_DATA)
      return RDB_PROTOCOL_ERROR;
    remaining -= (unsigned long)n;
  }
  return RDB_OK;
}
int rdb_server_serve_fd(int fd, const rdb_config *config,
                        const rdb_backend *b) {
  unsigned char header_bytes[RDB_HEADER_SIZE], data[RDB_CHUNK_SIZE];
  rdb_header h;
  rdb_reply r;
  unsigned long limit, requested;
  int hello, connected, s, kind, remote;
  enum state state;
  size_t n, cap;
  ssize_t first;
  if (fd < 0 || b == NULL || b->connect == NULL || b->disconnect == NULL ||
      b->sql_begin == NULL || b->sql_write == NULL || b->sql_end == NULL ||
      b->execute == NULL || b->param_write == NULL || b->param_end == NULL ||
      b->read == NULL || b->reset == NULL ||
      rdb_limit(config, &limit) != RDB_OK)
    return RDB_ARGUMENT_ERROR;
  hello = connected = 0;
  state = IDLE;
  s = RDB_OK;
  for (;;) {
    do {
      first = read(fd, header_bytes, 1);
    } while (first < 0 && errno == EINTR);
    if (first == 0) {
      s = RDB_OK;
      break;
    }
    if (first < 0) {
      s = RDB_TRANSPORT_ERROR;
      break;
    }
    s = rdb_io_read(fd, header_bytes + 1, sizeof(header_bytes) - 1);
    if (s != RDB_OK)
      break;
    s = rdb_header_decode(header_bytes, limit, &h);
    if (s != RDB_OK || h.flags != 0 ||
        h.statement != (h.opcode >= RDB_SQL_BEGIN ? 1UL : 0UL)) {
      s = RDB_PROTOCOL_ERROR;
      break;
    }
    memset(&r, 0, sizeof(r));
    n = 0;
    /* HELLO and CONNECT ordering are protocol invariants. */
    if ((!hello && h.opcode != RDB_HELLO) || (hello && h.opcode == RDB_HELLO)) {
      s = RDB_PROTOCOL_ERROR;
      break;
    }
    if (h.opcode == RDB_SQL_DATA || h.opcode == RDB_PARAM_DATA) {
      kind = h.opcode == RDB_SQL_DATA ? 1 : 2;
      if (!connected || (kind == 1 && state != SQL) ||
          (kind == 2 && state != INPUT))
        kind = 0;
      s = consume(fd, h.length.low, b, kind, &remote);
      if (s != RDB_OK)
        break;
      r.status = kind == 0 ? RDB_ERROR : remote;
      if (kind != 0 && remote != RDB_OK)
        state = BROKEN;
    } else if (h.opcode == RDB_HELLO) {
      if (h.length.low != 8) {
        s = RDB_PROTOCOL_ERROR;
        break;
      }
      s = rdb_io_read(fd, data, 8);
      if (s != RDB_OK)
        break;
      /* uint16 min/max version packed into the first four bytes. */
      requested = rdb_get32(data + 4);
      if (data[0] != 0 || data[1] != 0 || requested < 32 ||
          requested > RDB_DEFAULT_FRAME_LIMIT) {
        s = RDB_PROTOCOL_ERROR;
        break;
      }
      if (requested < limit)
        limit = requested;
      r.value.low = limit;
      hello = 1;
    } else if (h.opcode == RDB_COLUMN_READ) {
      if (h.length.low != 4) {
        s = RDB_PROTOCOL_ERROR;
        break;
      }
      s = rdb_io_read(fd, data, 4);
      if (s != RDB_OK)
        break;
      requested = rdb_get32(data);
      if (!connected || state != OUTPUT || requested == 0 ||
          requested > limit - 16) {
        r.status = RDB_ERROR;
      } else {
        cap = requested > sizeof(data) ? sizeof(data) : (size_t)requested;
        r.status = b->read(b->context, data, cap, &n);
        if (n > cap || (r.status == RDB_OK && n == 0) ||
            (r.status != RDB_OK && n != 0)) {
          s = RDB_PROTOCOL_ERROR;
          break;
        }
        if (r.status == RDB_NO_DATA)
          state = READY;
        else if (r.status != RDB_OK)
          state = BROKEN;
      }
    } else {
      if (h.opcode == RDB_CONNECT || h.opcode == RDB_DISCONNECT ||
          h.opcode == RDB_SQL_BEGIN || h.opcode == RDB_SQL_END ||
          h.opcode == RDB_EXECUTE || h.opcode == RDB_PARAM_END ||
          h.opcode == RDB_RESET) {
        if (h.length.low != 0) {
          s = RDB_PROTOCOL_ERROR;
          break;
        }
      } else {
        s = consume(fd, h.length.low, b, 0, &remote);
        if (s != RDB_OK)
          break;
        r.status = RDB_UNSUPPORTED;
      }
      switch (h.opcode) {
      case RDB_CONNECT:
        r.status = connected ? RDB_ERROR : b->connect(b->context);
        if (r.status == RDB_OK)
          connected = 1;
        break;
      case RDB_DISCONNECT:
        r.status = connected ? RDB_OK : RDB_ERROR;
        break;
      case RDB_SQL_BEGIN:
        if (!connected || state != IDLE)
          r.status = RDB_ERROR;
        else {
          r.status = b->sql_begin(b->context);
          state = r.status == RDB_OK ? SQL : BROKEN;
        }
        break;
      case RDB_SQL_END:
        if (!connected || state != SQL)
          r.status = RDB_ERROR;
        else {
          r.status = b->sql_end(b->context);
          state = r.status == RDB_OK ? READY : BROKEN;
        }
        break;
      case RDB_EXECUTE:
      case RDB_PARAM_END:
        if (!connected || (h.opcode == RDB_EXECUTE && state != READY) ||
            (h.opcode == RDB_PARAM_END && state != INPUT)) {
          r.status = RDB_ERROR;
        } else {
          r.status = h.opcode == RDB_EXECUTE ? b->execute(b->context, &r)
                                             : b->param_end(b->context, &r);
          if (r.status == RDB_NEED_INPUT && r.parameter == 0) {
            s = RDB_PROTOCOL_ERROR;
            break;
          }
          state = r.status == RDB_NEED_INPUT ? INPUT
                  : r.status == RDB_OK       ? OUTPUT
                                             : BROKEN;
        }
        break;
      case RDB_RESET:
        if (!connected)
          r.status = RDB_ERROR;
        else {
          b->reset(b->context);
          state = IDLE;
        }
        break;
      default:
        break;
      }
    }
    if (s != RDB_OK)
      break;
    s = send_reply(fd, &h, &r, data, n);
    if (s != RDB_OK || h.opcode == RDB_DISCONNECT)
      break;
  }
  if (connected)
    b->disconnect(b->context);
  return s;
}
