#define _POSIX_C_SOURCE 200809L
#include "control_exchange.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                  \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

static void pair(int fds[2], rdb_control_exchange *e) {
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
  CHECK(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);
  CHECK(fcntl(fds[1], F_SETFL, O_NONBLOCK) == 0);
  CHECK(rdb_exchange_open(e, fds[0]) == RDB_OK);
}
static void parse_limits(void) {
  rdb_control_exchange e;
  int fds[2], status;
  size_t i;
  static const char object[] = "{\"operation\":\"list\"}";
  unsigned char byte;
  pair(fds, &e);
  for (i = 0; i < sizeof(object) - 1; ++i) {
    CHECK(write(fds[1], object + i, 1) == 1);
    CHECK(rdb_exchange_receive(&e) == RDB_NO_DATA);
  }
  CHECK(strcmp(e.request.operation, "list") == 0);
  /* Exact 16 KiB including whitespace is valid, but needs confirmed EOF. */
  byte = ' ';
  for (i = sizeof(object) - 1; i < 16384; ++i) {
    CHECK(write(fds[1], &byte, 1) == 1);
    CHECK(rdb_exchange_receive(&e) == RDB_NO_DATA);
  }
  CHECK(shutdown(fds[1], SHUT_WR) == 0);
  CHECK(rdb_exchange_receive(&e) == RDB_OK);
  rdb_exchange_close(&e);
  close(fds[1]);
  pair(fds, &e);
  for (i = 0; i < 16385; ++i) {
    CHECK(write(fds[1], &byte, 1) == 1);
    status = rdb_exchange_receive(&e);
    CHECK(status == (i == 16384 ? RDB_ERROR : RDB_NO_DATA));
  }
  rdb_exchange_close(&e);
  close(fds[1]);
  pair(fds, &e);
  CHECK(write(fds[1], object, sizeof(object) - 1) ==
        (ssize_t)(sizeof(object) - 1));
  CHECK(rdb_exchange_receive(&e) == RDB_NO_DATA);
  CHECK(write(fds[1], "{}", 2) == 2);
  CHECK(rdb_exchange_receive(&e) == RDB_ERROR);
  rdb_exchange_close(&e);
  close(fds[1]);
}
static void output_pressure(void) {
  rdb_control_exchange e;
  rdb_document out, received;
  rdb_json_input input;
  lonejson *json;
  lonejson_stream *stream;
  lonejson_error error;
  lonejson_stream_result result;
  int fds[2], small, i, blocked, sent, parsed, status;
  size_t offset, used;
  pair(fds, &e);
  CHECK(write(fds[1], "{}", 2) == 2 && shutdown(fds[1], SHUT_WR) == 0);
  CHECK(rdb_exchange_receive(&e) == RDB_OK);
  small = 1024;
  CHECK(setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);
  memset(&out, 0, sizeof(out));
  strcpy(out.type, "definition");
  memset(out.attributes, 1, sizeof(out.attributes) - 1);
  CHECK(rdb_exchange_reply(&e, &out) == RDB_OK);
  blocked = 0;
  for (i = 0; i < 100 && !blocked; ++i) {
    offset = e.offset;
    used = e.used;
    CHECK(rdb_exchange_send(&e) == RDB_NO_DATA);
    blocked = used == e.used && offset == e.offset && e.used > e.offset;
  }
  CHECK(blocked && e.used <= RDB_CHUNK_SIZE);
  json = rdb_json_new();
  CHECK(json != NULL);
  memset(&input, 0, sizeof(input));
  input.fd = fds[1];
  input.resumable = 1;
  stream = rdb_json_stream(json, &input);
  CHECK(stream != NULL);
  lonejson_error_init(&error);
  sent = parsed = 0;
  for (i = 0; i < 100 && (!sent || !parsed); ++i) {
    if (!sent) {
      status = rdb_exchange_send(&e);
      CHECK(status == RDB_OK || status == RDB_NO_DATA);
      sent = status == RDB_OK;
      CHECK(e.used <= RDB_CHUNK_SIZE);
    }
    if (!parsed) {
      result = lonejson_stream_next(stream, &received, &error);
      CHECK(result == LONEJSON_STREAM_OBJECT ||
            result == LONEJSON_STREAM_WOULD_BLOCK);
      parsed = result == LONEJSON_STREAM_OBJECT;
    }
  }
  CHECK(sent && parsed);
  CHECK(strcmp(received.type, out.type) == 0 &&
        strcmp(received.attributes, out.attributes) == 0);
  rdb_exchange_close(&e);
  CHECK(lonejson_stream_next(stream, &received, &error) == LONEJSON_STREAM_EOF);
  lonejson_stream_close(stream);
  lonejson_free(json);
  close(fds[1]);
  /* Broken readers fail without SIGPIPE terminating the host. */
  pair(fds, &e);
  CHECK(write(fds[1], "{}", 2) == 2 && shutdown(fds[1], SHUT_WR) == 0);
  CHECK(rdb_exchange_receive(&e) == RDB_OK);
  close(fds[1]);
  CHECK(rdb_exchange_reply(&e, &out) == RDB_OK);
  CHECK(rdb_exchange_send(&e) == RDB_TRANSPORT_ERROR);
  rdb_exchange_close(&e);
}
int main(void) {
  parse_limits();
  output_pressure();
  puts("Resumable parsing, exact request limit, bounded output and "
       "backpressure passed");
  return 0;
}
