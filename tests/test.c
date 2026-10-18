#define _POSIX_C_SOURCE 200809L
#include "mock.h"
#include <remotedb/server.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                  \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)
static void full_read(int fd, void *p, size_t n) {
  ssize_t k;
  unsigned char *b;
  b = (unsigned char *)p;
  while (n) {
    k = read(fd, b, n);
    CHECK(k > 0);
    b += k;
    n -= (size_t)k;
  }
}
static void full_write(int fd, const void *p, size_t n) {
  ssize_t k;
  const unsigned char *b;
  b = (const unsigned char *)p;
  while (n) {
    k = write(fd, b, n);
    CHECK(k > 0);
    b += k;
    n -= (size_t)k;
  }
}
typedef struct report {
  rdb_mock mock;
  int status;
  long rss;
} report;
static int failure_mode;
static int failing_sql(void *context, const void *bytes, size_t length) {
  rdb_mock *mock;
  (void)bytes;
  mock = (rdb_mock *)context;
  CHECK(length <= RDB_CHUNK_SIZE);
  ++mock->sql_hash;
  return RDB_ERROR;
}
static int invalid_read(void *context, void *bytes, size_t capacity,
                        size_t *read) {
  (void)context;
  (void)bytes;
  *read = capacity + 1;
  return RDB_OK;
}
static pid_t start(int *fd, int *observation, unsigned long parameters,
                   unsigned long output, unsigned long limit) {
  int sockets[2], pipefd[2], small;
  pid_t pid;
  report r;
  rdb_config config;
  struct rusage usage;
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  small = 1024;
  CHECK(setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) ==
        0);
  CHECK(setsockopt(sockets[1], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) ==
        0);
  CHECK(pipe(pipefd) == 0);
  pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    close(sockets[0]);
    close(pipefd[0]);
    memset(&r, 0, sizeof(r));
    rdb_mock_init(&r.mock);
    r.mock.input_parameters = parameters;
    r.mock.output_length.low = output;
    if (failure_mode == 1)
      r.mock.backend.sql_write = failing_sql;
    if (failure_mode == 2)
      r.mock.execution_status = RDB_ERROR;
    if (failure_mode == 3)
      r.mock.backend.read = invalid_read;
    config.frame_limit = limit;
    r.status = rdb_server_serve_fd(sockets[1], &config, &r.mock.backend);
    CHECK(getrusage(RUSAGE_SELF, &usage) == 0);
    r.rss = usage.ru_maxrss;
    full_write(pipefd[1], &r, sizeof(r));
    close(sockets[1]);
    close(pipefd[1]);
    exit(0);
  }
  close(sockets[1]);
  close(pipefd[1]);
  *fd = sockets[0];
  *observation = pipefd[0];
  return pid;
}
static report finish(pid_t pid, int observation) {
  report r;
  int status;
  full_read(observation, &r, sizeof(r));
  close(observation);
  CHECK(waitpid(pid, &status, 0) == pid);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  return r;
}
static void header_test(void) {
  rdb_header h, decoded;
  rdb_u64 v, old;
  unsigned char bytes[24];
  static const unsigned char expected[24] = {
      'R',  'D',  'B',  '0',  0,    0,    0x34, 0x12, 1, 0, 0, 0,
      0x78, 0x56, 0x34, 0x12, 0x00, 0x01, 0x00, 0x00, 0, 0, 0, 0};
  memset(&h, 0, sizeof(h));
  h.opcode = 0x1234;
  h.flags = 1;
  h.statement = 0x12345678UL;
  h.length.low = 256;
  CHECK(rdb_header_encode(&h, bytes) == RDB_OK);
  CHECK(memcmp(bytes, expected, sizeof(bytes)) == 0);
  CHECK(rdb_header_decode(bytes, 256, &decoded) == RDB_OK);
  CHECK(decoded.opcode == h.opcode && decoded.statement == h.statement);
  CHECK(rdb_header_decode(bytes, 255, &decoded) == RDB_PROTOCOL_ERROR);
  bytes[0] = 'X';
  CHECK(rdb_header_decode(bytes, 256, &decoded) == RDB_PROTOCOL_ERROR);
  bytes[0] = 'R';
  bytes[4] = 1;
  CHECK(rdb_header_decode(bytes, 256, &decoded) == RDB_PROTOCOL_ERROR);
  bytes[4] = 0;
  bytes[20] = 1;
  CHECK(rdb_header_decode(bytes, 0xffffffffUL, &decoded) == RDB_PROTOCOL_ERROR);
  CHECK(rdb_header_decode(NULL, 0, &decoded) == RDB_ARGUMENT_ERROR);
  v.low = 0xffffffffUL;
  v.high = 0;
  CHECK(rdb_u64_add(&v, 1) == RDB_OK && v.low == 0 && v.high == 1);
  v.low = v.high = 0xffffffffUL;
  old = v;
  CHECK(rdb_u64_add(&v, 1) == RDB_ARGUMENT_ERROR);
  CHECK(rdb_u64_compare(v, old) == 0);
  /* Conceptual >4GiB mock result crosses a limb without allocating its value.
   */
  {
    rdb_mock m;
    unsigned char data[32];
    size_t read;
    rdb_mock_init(&m);
    m.output_position.low = 0xfffffff0UL;
    m.output_length.high = 1;
    m.output_length.low = 16;
    CHECK(m.backend.read(&m, data, sizeof(data), &read) == RDB_OK);
    CHECK(read == 32 && data[0] == 240 && data[16] == 0);
    CHECK(m.output_position.high == 1 && m.output_position.low == 16);
    CHECK(m.backend.read(&m, data, sizeof(data), &read) == RDB_NO_DATA);
  }
}
static void streaming_test(void) {
  int fd, observation, eof, status;
  pid_t pid;
  rdb_connection *c;
  rdb_config config;
  report r;
  unsigned char input[65536], output[3071];
  unsigned long input_hash, sql_hash, offset, loops;
  size_t i, n, total, j;
  static const char *sqls[] = {
      "SELECT * FROM jobs WHERE id = ? FOR UPDATE",
      "INSERT INTO orders(id, amount) VALUES (?, ?)",
      "UPDATE orders SET amount = ? WHERE id = ?",
      "DELETE FROM orders WHERE id = ?",
      "MERGE INTO t USING s ON t.id=s.id WHEN MATCHED THEN UPDATE SET x=s.x",
      "CREATE TABLE t (id INTEGER)",
      "{CALL dbo.process_order(?, ?)}",
      "SELECT jsonb_build_object('a', 1), N'Unicode: \342\230\203'"};
  for (i = 0; i < sizeof(input); ++i)
    input[i] = (unsigned char)i;
  config.frame_limit = 65536;
  sql_hash = input_hash = 0;
  for (j = 0; j < sizeof(sqls) / sizeof(sqls[0]); ++j) {
    pid = start(&fd, &observation, 2, 2UL * 1024UL * 1024UL + 17, 65536);
    CHECK(rdb_open_fd(fd, &config, &c) == RDB_OK);
    CHECK(rdb_connection_connect(c) == RDB_OK);
    CHECK(rdb_connection_sql_begin(c) == RDB_OK);
    sql_hash = 0;
    for (i = 0; i < strlen(sqls[j]); ++i) {
      CHECK(rdb_connection_sql_write(c, sqls[j] + i, 1) == RDB_OK);
      sql_hash = rdb_mock_hash(sql_hash, sqls[j] + i, 1);
    }
    CHECK(rdb_connection_sql_end(c) == RDB_OK);
    CHECK(rdb_connection_execute(c) == RDB_NEED_INPUT);
    CHECK(rdb_connection_reply(c)->parameter == 1);
    input_hash = 0;
    for (loops = 0; loops < 16; ++loops) {
      CHECK(rdb_connection_param_write(c, input, sizeof(input)) == RDB_OK);
      input_hash = rdb_mock_hash(input_hash, input, sizeof(input));
    }
    CHECK(rdb_connection_param_end(c) == RDB_NEED_INPUT);
    CHECK(rdb_connection_reply(c)->parameter == 2);
    /* Empty parameter is legal; the backend gets END with no DATA. */
    CHECK(rdb_connection_param_end(c) == RDB_OK);
    CHECK(rdb_connection_reply(c)->value.low == 2UL * 1024UL * 1024UL + 17);
    offset = 0;
    total = 0;
    for (;;) {
      status = rdb_connection_column_read(c, output, sizeof(output), &n, &eof);
      if (status == RDB_NO_DATA) {
        CHECK(eof && n == 0);
        break;
      }
      CHECK(status == RDB_OK && !eof && n > 0);
      for (i = 0; i < n; ++i)
        CHECK(output[i] == (unsigned char)(offset++ & 255UL));
      total += n;
    }
    CHECK(total == 2UL * 1024UL * 1024UL + 17);
    CHECK(rdb_connection_reset(c) == RDB_OK);
    CHECK(rdb_connection_disconnect(c) == RDB_OK);
    rdb_close(c);
    r = finish(pid, observation);
    CHECK(r.status == RDB_OK && r.mock.connects == 1 &&
          r.mock.disconnects == 1);
    CHECK(r.mock.sql_hash == sql_hash && r.mock.input_hash == input_hash);
    CHECK(r.mock.input_bytes.low == 1024UL * 1024UL &&
          r.mock.input_bytes.high == 0);
    CHECK(r.mock.largest_chunk <= RDB_CHUNK_SIZE);
  }
  /* Under Valgrind RSS belongs to the instrumented process; plain tests also
   * exercise the fixed callback buffer boundary directly above. */
  printf("streaming: 8 SQL variants, 8 MiB input, 16 MiB output; max chunk "
         "%lu, server RSS %ld KiB\n",
         (unsigned long)r.mock.largest_chunk, r.rss);
}
static void runtime_test(const char *self) {
#ifdef __linux__
  const char *sysroot;
  char line[1024];
  FILE *maps;
  int found, status;
  pid_t pid;
  sysroot = getenv("REMOTEDB_TEST_SYSROOT");
  if (sysroot == NULL)
    return;
  maps = fopen("/proc/self/maps", "r");
  CHECK(maps != NULL);
  found = 0;
  while (fgets(line, sizeof(line), maps) != NULL) {
    if (strstr(line, "libc.so") != NULL) {
      CHECK(strstr(line, sysroot) != NULL);
      found = 1;
    }
  }
  CHECK(fclose(maps) == 0 && found);
  if (getenv("REMOTEDB_RUNTIME_CHILD") == NULL) {
    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
      CHECK(setenv("REMOTEDB_RUNTIME_CHILD", "1", 1) == 0);
      /* Under Valgrind, /proc/self/exe names Memcheck rather than the guest.
       * Execute the CTest-provided binary path to check the guest runtime. */
      execl(self, "test-remotedb", "header", (char *)NULL);
      exit(1);
    }
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    /* Link-time runtime selection must not contaminate host subprocesses. */
    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
      execl("/usr/bin/true", "true", (char *)NULL);
      exit(1);
    }
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  }
#endif
  (void)self;
}
static void state_test(void) {
  int fd, observation;
  pid_t pid;
  rdb_connection *c;
  rdb_config config;
  report r;
  config.frame_limit = 32;
  pid = start(&fd, &observation, 1, 0, 32);
  CHECK(rdb_open_fd(fd, &config, &c) == RDB_OK);
  CHECK(rdb_connection_sql_begin(c) == RDB_ERROR);
  CHECK(rdb_connection_connect(c) == RDB_OK);
  CHECK(rdb_connection_connect(c) == RDB_ERROR);
  CHECK(rdb_connection_execute(c) == RDB_ERROR);
  CHECK(rdb_connection_sql_begin(c) == RDB_OK);
  CHECK(rdb_connection_sql_end(c) == RDB_ERROR);
  CHECK(rdb_connection_sql_begin(c) == RDB_ERROR);
  CHECK(rdb_connection_reset(c) == RDB_OK);
  CHECK(rdb_connection_sql_begin(c) == RDB_OK);
  CHECK(rdb_connection_sql_write(c, "INSERT INTO t VALUES (?)", 24) == RDB_OK);
  CHECK(rdb_connection_sql_end(c) == RDB_OK);
  CHECK(rdb_connection_param_write(c, "x", 1) == RDB_ERROR);
  CHECK(rdb_connection_execute(c) == RDB_NEED_INPUT);
  CHECK(rdb_connection_execute(c) == RDB_ERROR);
  CHECK(rdb_connection_sql_begin(c) == RDB_ERROR);
  CHECK(rdb_connection_param_write(c, "abc", 3) == RDB_OK);
  CHECK(rdb_connection_param_end(c) == RDB_OK);
  CHECK(rdb_connection_param_write(c, "x", 1) == RDB_ERROR);
  CHECK(rdb_connection_disconnect(c) == RDB_OK);
  CHECK(rdb_connection_reset(c) == RDB_STATE_ERROR);
  rdb_close(c);
  r = finish(pid, observation);
  CHECK(r.mock.disconnects == 1 && r.status == RDB_OK);
  rdb_close(NULL);
  c = (rdb_connection *)1;
  CHECK(rdb_open_fd(-1, NULL, &c) == RDB_ARGUMENT_ERROR && c == NULL);
  config.frame_limit = 31;
  CHECK(rdb_open_fd(0, &config, &c) == RDB_ARGUMENT_ERROR && c == NULL);
}
static void failure_test(void) {
  int fd, observation, eof, status;
  unsigned char data[65536];
  size_t n;
  pid_t pid;
  rdb_connection *c;
  report r;
  memset(data, 'x', sizeof(data));
  for (failure_mode = 1; failure_mode <= 3; ++failure_mode) {
    pid = start(&fd, &observation, 0, 0, 65536);
    CHECK(rdb_open_fd(fd, NULL, &c) == RDB_OK);
    CHECK(rdb_connection_connect(c) == RDB_OK);
    CHECK(rdb_connection_sql_begin(c) == RDB_OK);
    if (failure_mode == 1) {
      /* The first callback fails, but the remaining frame must be drained
       * without more callbacks so RESET can still be decoded correctly. */
      CHECK(rdb_connection_sql_write(c, data, sizeof(data)) == RDB_ERROR);
      CHECK(rdb_connection_sql_end(c) == RDB_ERROR);
    } else {
      CHECK(rdb_connection_sql_write(c, "SELECT x", 8) == RDB_OK);
      CHECK(rdb_connection_sql_end(c) == RDB_OK);
      status = rdb_connection_execute(c);
      CHECK(status == (failure_mode == 2 ? RDB_ERROR : RDB_OK));
      if (failure_mode == 3) {
        CHECK(rdb_connection_column_read(c, data, 32, &n, &eof) ==
              RDB_TRANSPORT_ERROR);
        CHECK(n == 0 && eof == 0);
      }
    }
    if (failure_mode != 3) {
      CHECK(rdb_connection_reset(c) == RDB_OK);
      CHECK(rdb_connection_disconnect(c) == RDB_OK);
    }
    rdb_close(c);
    r = finish(pid, observation);
    CHECK(r.mock.disconnects == 1);
    CHECK(r.status == (failure_mode == 3 ? RDB_PROTOCOL_ERROR : RDB_OK));
    if (failure_mode == 1)
      CHECK(r.mock.sql_hash == 1);
  }
  failure_mode = 0;
}
static void raw_header(int fd, unsigned int op, unsigned long len,
                       unsigned char *bytes) {
  rdb_header h;
  size_t i;
  memset(&h, 0, sizeof(h));
  h.opcode = op;
  h.statement = op >= RDB_SQL_BEGIN ? 1 : 0;
  h.length.low = len;
  CHECK(rdb_header_encode(&h, bytes) == RDB_OK);
  /* A byte at a time exercises framing independently of client implementation.
   */
  for (i = 0; i < RDB_HEADER_SIZE; ++i)
    full_write(fd, bytes + i, 1);
}
static void raw_reply(int fd, unsigned int op, int status) {
  unsigned char bytes[40];
  rdb_header h;
  full_read(fd, bytes, sizeof(bytes));
  CHECK(rdb_header_decode(bytes, 65536, &h) == RDB_OK);
  CHECK(h.opcode == op && h.flags == 1 && h.length.low == 16);
  CHECK(bytes[24] == status);
}
static void malformed_test(void) {
  int fd, observation;
  pid_t pid;
  rdb_connection *c;
  rdb_header h;
  unsigned char bytes[24], hello[8];
  report r;
  unsigned int scenario;
  for (scenario = 0; scenario < 10; ++scenario) {
    pid = start(&fd, &observation, 0, 0, 65536);
    if (scenario < 3 || scenario >= 7) {
      memset(&h, 0, sizeof(h));
      h.opcode = RDB_HELLO;
      CHECK(rdb_header_encode(&h, bytes) == RDB_OK);
      if (scenario == 0)
        bytes[0] = 'X';
      if (scenario == 1)
        bytes[4] = 1;
      if (scenario == 2)
        bytes[20] = 1;
      if (scenario == 7)
        bytes[18] = 2; /* 131072 exceeds the default frame limit. */
      if (scenario == 8)
        bytes[8] = 1; /* Request marked as a response. */
      if (scenario == 9)
        bytes[12] = 1; /* HELLO must not carry a statement id. */
      full_write(fd, bytes, sizeof(bytes));
    } else if (scenario == 3) {
      raw_header(fd, RDB_HELLO, 7, bytes);
    } else if (scenario == 4) {
      raw_header(fd, RDB_HELLO, 8, bytes);
      memset(hello, 0, sizeof(hello));
      hello[0] = 1;
      hello[6] = 1;
      full_write(fd, hello, sizeof(hello));
    } else {
      CHECK(rdb_open_fd(fd, NULL, &c) == RDB_OK);
      if (scenario == 5) {
        raw_header(fd, 1234, 3, bytes);
        full_write(fd, "abc", 3);
        raw_reply(fd, 1234, RDB_UNSUPPORTED);
        CHECK(rdb_connection_connect(c) == RDB_OK);
        CHECK(rdb_connection_disconnect(c) == RDB_OK);
      } else {
        raw_header(fd, RDB_CONNECT, 1, bytes);
      }
      rdb_close(c);
      fd = -1;
    }
    if (fd >= 0)
      close(fd);
    r = finish(pid, observation);
    CHECK(r.status == (scenario == 5 ? RDB_OK : RDB_PROTOCOL_ERROR));
  }
}
static void transport_test(void) {
  int fd, observation, sockets[2], status;
  pid_t pid;
  rdb_connection *c;
  report r;
  unsigned char bytes[40];
  pid = start(&fd, &observation, 1, 0, 65536);
  CHECK(rdb_open_fd(fd, NULL, &c) == RDB_OK);
  CHECK(rdb_connection_connect(c) == RDB_OK);
  CHECK(rdb_connection_sql_begin(c) == RDB_OK);
  CHECK(rdb_connection_sql_write(c, "INSERT x", 8) == RDB_OK);
  CHECK(rdb_connection_sql_end(c) == RDB_OK);
  CHECK(rdb_connection_execute(c) == RDB_NEED_INPUT);
  rdb_close(c);
  r = finish(pid, observation);
  CHECK(r.status == RDB_OK && r.mock.disconnects == 1);
  /* Loss partway through a parameter frame: no incomplete chunk reaches the
   * backend, and already consumed chunks are never replayed. */
  {
    unsigned char partial[4096], hbytes[24];
    unsigned long cut;
    memset(partial, 'x', sizeof(partial));
    for (cut = 1; cut <= 4096; cut *= 4096) {
      pid = start(&fd, &observation, 1, 0, 65536);
      CHECK(rdb_open_fd(fd, NULL, &c) == RDB_OK);
      CHECK(rdb_connection_connect(c) == RDB_OK);
      CHECK(rdb_connection_sql_begin(c) == RDB_OK);
      CHECK(rdb_connection_sql_write(c, "INSERT x", 8) == RDB_OK);
      CHECK(rdb_connection_sql_end(c) == RDB_OK);
      CHECK(rdb_connection_execute(c) == RDB_NEED_INPUT);
      raw_header(fd, RDB_PARAM_DATA, 8192, hbytes);
      full_write(fd, partial, (size_t)cut);
      rdb_close(c);
      r = finish(pid, observation);
      CHECK(r.status == RDB_TRANSPORT_ERROR && r.mock.disconnects == 1);
      CHECK(r.mock.input_bytes.low == (cut == 4096 ? 4096UL : 0UL));
    }
  }
  /* Server reads a partial header before EOF: not a clean session close. */
  pid = start(&fd, &observation, 0, 0, 65536);
  full_write(fd, "R", 1);
  close(fd);
  r = finish(pid, observation);
  CHECK(r.status == RDB_TRANSPORT_ERROR && r.mock.connects == 0);
  /* Fake peer negotiates, then disappears during EXECUTE. */
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    rdb_header h;
    close(sockets[0]);
    full_read(sockets[1], bytes, 32);
    memset(&h, 0, sizeof(h));
    h.opcode = RDB_HELLO;
    h.flags = 1;
    h.length.low = 16;
    CHECK(rdb_header_encode(&h, bytes) == RDB_OK);
    memset(bytes + 24, 0, 16);
    bytes[34] = 1;
    full_write(sockets[1], bytes, 40);
    full_read(sockets[1], bytes, 24);
    close(sockets[1]);
    exit(0);
  }
  close(sockets[1]);
  CHECK(rdb_open_fd(sockets[0], NULL, &c) == RDB_OK);
  CHECK(rdb_connection_execute(c) == RDB_OUTCOME_UNKNOWN);
  CHECK(rdb_connection_execute(c) == RDB_STATE_ERROR);
  rdb_close(c);
  CHECK(waitpid(pid, &status, 0) == pid && WEXITSTATUS(status) == 0);
  /* EPIPE must be returned without terminating the caller with SIGPIPE. */
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  close(sockets[1]);
  CHECK(rdb_open_fd(sockets[0], NULL, &c) == RDB_TRANSPORT_ERROR);
  CHECK(c == NULL);
  close(sockets[0]);
}
int main(int argc, char **argv) {
  CHECK(argc == 2);
  runtime_test(argv[0]);
  if (strcmp(argv[1], "header") == 0)
    header_test();
  else if (strcmp(argv[1], "streaming") == 0)
    streaming_test();
  else if (strcmp(argv[1], "state") == 0)
    state_test();
  else if (strcmp(argv[1], "malformed") == 0)
    malformed_test();
  else if (strcmp(argv[1], "transport") == 0)
    transport_test();
  else if (strcmp(argv[1], "failure") == 0)
    failure_test();
  else
    CHECK(0);
  return 0;
}
