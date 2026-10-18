#define _POSIX_C_SOURCE 200809L
#include "stream_checks.h"
#include "stream_fixture.h"
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
typedef struct observation {
  rdb_u64 input, output;
  int valid, disconnects, status;
  size_t largest;
  long before, after;
} observation;
static void io(int fd, void *p, size_t n, int writing) {
  ssize_t k;
  char *b;
  b = (char *)p;
  while (n != 0) {
    k = writing ? write(fd, b, n) : read(fd, b, n);
    if (k < 0 && errno == EINTR)
      continue;
    REQUIRE(k > 0);
    b += k;
    n -= (size_t)k;
  }
}
static int ready(int fd, int ms) {
  struct pollfd p;
  p.fd = fd;
  p.events = POLLIN;
  p.revents = 0;
  return poll(&p, 1, ms);
}
static void waited(pid_t pid) {
  int s;
  REQUIRE(waitpid(pid, &s, 0) == pid);
  REQUIRE(WIFEXITED(s) && WEXITSTATUS(s) == 0);
}
static void transfer(unsigned long mib) {
  int sockets[2], reports[2];
  pid_t pid;
  stream_fixture fixture;
  observation report;
  rdb_connection *c;
  long before, after;
  REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  REQUIRE(pipe(reports) == 0);
  pid = fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    close(sockets[0]);
    close(reports[0]);
    memset(&report, 0, sizeof(report));
    stream_fixture_init(&fixture, mib);
    report.before = rss_kib();
    report.status = rdb_server_serve_fd(sockets[1], NULL, &fixture.backend);
    report.after = rss_kib();
    report.input = fixture.input;
    report.output = fixture.output;
    report.valid = fixture.valid;
    report.largest = fixture.largest;
    report.disconnects = fixture.disconnects;
    io(reports[1], &report, sizeof(report), 1);
    close(sockets[1]);
    close(reports[1]);
    _exit(0);
  }
  close(sockets[1]);
  close(reports[1]);
  before = rss_kib();
  REQUIRE(rdb_open_fd(sockets[0], NULL, &c) == RDB_OK);
  prepare_stream(c);
  REQUIRE(rdb_connection_execute(c) == RDB_NEED_INPUT);
  upload_stream(c, mib);
  download_stream(c, mib);
  REQUIRE(rdb_connection_disconnect(c) == RDB_OK);
  rdb_close(c);
  after = rss_kib();
  io(reports[0], &report, sizeof(report), 0);
  close(reports[0]);
  waited(pid);
  REQUIRE(report.status == RDB_OK && report.valid && report.disconnects == 1);
  REQUIRE(rdb_u64_compare(report.input, report.output) == 0);
  REQUIRE(report.input.high == mib / 4096UL &&
          report.input.low == (mib % 4096UL) * 1048576UL + 17UL);
  REQUIRE(report.largest <= RDB_CHUNK_SIZE);
  memory_bound(before, after);
  memory_bound(report.before, report.after);
  printf("Actual transfer: %lu MiB + 17 bytes each direction; client RSS %ld "
         "-> %ld KiB, server RSS %ld -> %ld KiB; max chunk %lu\n",
         mib, before, after, report.before, report.after,
         (unsigned long)report.largest);
}
static void backpressure(void) {
  int sockets[2], notices[2], gates[2], completed[2], small;
  pid_t server, producer;
  stream_fixture s;
  rdb_connection *c;
  unsigned char block[4096], header[24], request[4], response[40 + 4096];
  rdb_header h;
  char byte;
  size_t i;
  REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  REQUIRE(pipe(notices) == 0 && pipe(gates) == 0 && pipe(completed) == 0);
  small = 1024;
  REQUIRE(setsockopt(sockets[1], SOL_SOCKET, SO_SNDBUF, &small,
                     sizeof(small)) == 0);
  server = fork();
  REQUIRE(server >= 0);
  if (server == 0) {
    close(sockets[0]);
    close(notices[0]);
    close(gates[1]);
    close(completed[0]);
    close(completed[1]);
    stream_fixture_init(&s, 0);
    s.notice_fd = notices[1];
    s.gate_fd = gates[0];
    /* For this pressure test only, input is one chunk, output is one chunk. */
    s.length.low = sizeof(block);
    REQUIRE(rdb_server_serve_fd(sockets[1], NULL, &s.backend) == RDB_OK);
    REQUIRE(s.disconnects == 1 && s.valid && s.reads == 2);
    close(sockets[1]);
    close(notices[1]);
    close(gates[0]);
    _exit(0);
  }
  close(sockets[1]);
  close(notices[1]);
  close(gates[0]);
  REQUIRE(rdb_open_fd(sockets[0], NULL, &c) == RDB_OK);
  prepare_stream(c);
  REQUIRE(rdb_connection_execute(c) == RDB_NEED_INPUT);
  for (i = 0; i < sizeof(block); ++i)
    block[i] = (unsigned char)i;
  producer = fork();
  REQUIRE(producer >= 0);
  if (producer == 0) {
    close(completed[0]);
    close(notices[0]);
    close(gates[1]);
    REQUIRE(rdb_connection_param_write(c, block, sizeof(block)) == RDB_OK);
    byte = 'x';
    io(completed[1], &byte, 1, 1);
    /* Closing this process's duplicate preserves the parent's socket. */
    rdb_close(c);
    close(completed[1]);
    _exit(0);
  }
  close(completed[1]);
  REQUIRE(ready(notices[0], 2000) == 1);
  io(notices[0], &byte, 1, 0);
  REQUIRE(ready(completed[0], 200) ==
          0); /* Consumption gates acknowledgement. */
  io(gates[1], &byte, 1, 1);
  REQUIRE(ready(completed[0], 2000) == 1);
  io(completed[0], &byte, 1, 0);
  waited(producer);
  close(completed[0]);
  REQUIRE(rdb_connection_param_end(c) == RDB_OK);
  /* Pausing output requests must pause the backend producer entirely. */
  REQUIRE(ready(notices[0], 200) == 0);
  memset(&h, 0, sizeof(h));
  h.opcode = RDB_COLUMN_READ;
  h.statement = 1;
  h.length.low = 4;
  REQUIRE(rdb_header_encode(&h, header) == RDB_OK);
  memset(request, 0, sizeof(request));
  request[1] = 8; /* request 2048 */
  io(sockets[0], header, sizeof(header), 1);
  io(sockets[0], request, sizeof(request), 1);
  REQUIRE(ready(notices[0], 2000) == 1);
  io(notices[0], &byte, 1, 0);
  REQUIRE(ready(notices[0], 200) ==
          0); /* No eager generation while consumer stalls. */
  io(sockets[0], response, 40 + 2048, 0);
  REQUIRE(rdb_header_decode(response, 65536, &h) == RDB_OK &&
          h.length.low == 2064);
  for (i = 0; i < 2048; ++i)
    REQUIRE(response[40 + i] == (unsigned char)i);
  /* Resume using the public API; exactly one more production callback. */
  {
    size_t n;
    int eof;
    REQUIRE(rdb_connection_column_read(c, block, 2048, &n, &eof) == RDB_OK &&
            n == 2048);
    REQUIRE(ready(notices[0], 2000) == 1);
    io(notices[0], &byte, 1, 0);
  }
  REQUIRE(rdb_connection_disconnect(c) == RDB_OK);
  rdb_close(c);
  close(notices[0]);
  close(gates[1]);
  waited(server);
  puts("Backpressure: producer blocked until consumption; output generation "
       "stopped while consumer paused and resumed on demand");
}
int main(int argc, char **argv) {
  unsigned long mib;
  char *end;
  REQUIRE(argc == 2 || argc == 3);
  if (strcmp(argv[1], "pressure") == 0) {
    backpressure();
    return 0;
  }
  REQUIRE(strcmp(argv[1], "transfer") == 0);
  mib = argc == 3 ? strtoul(argv[2], &end, 10) : 1;
  if (argc == 3)
    REQUIRE(*end == 0 && mib > 0 && mib <= 8192);
  alarm(300);
  if (mib > 1)
    transfer(1);
  transfer(mib);
  alarm(0);
  return 0;
}
