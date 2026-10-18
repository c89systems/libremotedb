#define _POSIX_C_SOURCE 200809L
#include "stream_checks.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <remotedb/control.h>
#include <signal.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
typedef struct records {
  char key[65];
  unsigned long count;
  int key_allowed;
} records;
static int record(void *user, const rdb_control_record *r) {
  records *o;
  o = (records *)user;
  if (strcmp(r->type, "done") != 0)
    ++o->count;
  if (r->connection_key[0] != 0)
    strcpy(o->key, r->connection_key);
  if (!o->key_allowed)
    REQUIRE(r->connection_key[0] == 0);
  return RDB_OK;
}
static void delay(void) {
  struct timespec t;
  t.tv_sec = 0;
  t.tv_nsec = 10000000L;
  while (nanosleep(&t, &t) != 0)
    REQUIRE(errno == EINTR);
}
static int query(const rdb_endpoint *ctl, const char *op, const char *key,
                 records *out) {
  rdb_control_request r;
  memset(&r, 0, sizeof(r));
  memset(out, 0, sizeof(*out));
  out->key_allowed = strcmp(op, "open") == 0;
  strcpy(r.operation, op);
  strcpy(r.definition.name, "stream");
  if (key != NULL)
    strcpy(r.connection_key, key);
  return rdb_control_query(ctl, &r, record, out);
}
static void empty(const rdb_endpoint *ctl) {
  records o;
  int i, status;
  for (i = 0; i < 200; ++i) {
    status = query(ctl, "sessions", NULL, &o);
    REQUIRE(status == RDB_OK || status == RDB_ERROR);
    /* Reaping can invalidate a streamed listing; discard that partial view. */
    if (status == RDB_OK && o.count == 0)
      return;
    delay();
  }
  REQUIRE(0);
}
static rdb_connection *attach(const rdb_endpoint *ctl, const rdb_endpoint *data,
                              const char *fault, char key[65]) {
  rdb_control_request r;
  records o;
  rdb_connection *c;
  memset(&r, 0, sizeof(r));
  memset(&o, 0, sizeof(o));
  o.key_allowed = 1;
  strcpy(r.operation, "open");
  strcpy(r.definition.name, "stream");
  if (fault != NULL)
    strcpy(r.definition.attributes, fault);
  REQUIRE(rdb_control_query(ctl, &r, record, &o) == RDB_OK);
  REQUIRE(strlen(o.key) == 64);
  strcpy(key, o.key);
  REQUIRE(rdb_open_key(data, key, NULL, &c) == RDB_OK);
  return c;
}
static void uid_rejected_before_key(const rdb_endpoint *endpoint) {
  struct sockaddr_un address;
  struct pollfd p;
  socklen_t length;
  char byte;
  int fd;
  size_t n;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  n = strlen(endpoint->address);
  REQUIRE(n < sizeof(address.sun_path));
  if (endpoint->kind == RDB_ABSTRACT) {
    memcpy(address.sun_path + 1, endpoint->address, n);
    length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + n + 1);
  } else {
    memcpy(address.sun_path, endpoint->address, n + 1);
    length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + n + 1);
  }
  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  REQUIRE(fd >= 0);
  REQUIRE(connect(fd, (struct sockaddr *)&address, length) == 0);
  p.fd = fd;
  p.events = POLLIN;
  p.revents = 0;
  /* No key or protocol bytes were sent: the native UID gate alone closes it. */
  REQUIRE(poll(&p, 1, 2000) == 1);
  REQUIRE(read(fd, &byte, 1) == 0);
  REQUIRE(close(fd) == 0);
}
static void replay_denied(const rdb_endpoint *data, const char *key) {
  rdb_connection *c;
  c = NULL;
  REQUIRE(rdb_open_key(data, key, NULL, &c) != RDB_OK && c == NULL);
}
static void configure(const rdb_endpoint *ctl, const rdb_endpoint *data,
                      unsigned long mib) {
  rdb_control_request r;
  records o;
  memset(&r, 0, sizeof(r));
  memset(&o, 0, sizeof(o));
  strcpy(r.operation, "put");
  strcpy(r.definition.name, "stream");
  strcpy(r.definition.driver, "mock");
  r.definition.kind = data->kind;
  REQUIRE(strlen(data->address) < sizeof(r.definition.address));
  strcpy(r.definition.address, data->address);
  snprintf(r.definition.dsn, sizeof(r.definition.dsn), "%lu", mib);
  REQUIRE(rdb_control_query(ctl, &r, record, &o) == RDB_OK);
}
static void scenarios(const rdb_endpoint *ctl, const rdb_endpoint *data,
                      unsigned long mib) {
  rdb_connection *c;
  records o;
  char key[65];
  unsigned char block[4096];
  long before;
  int i, status, eof;
  size_t n;
  const char *faults[] = {"crash-execute", "crash-input", "crash-output"};
  before = rss_kib();
  c = attach(ctl, data, NULL, key);
  replay_denied(data, key); /* Exclusive attachment. */
  prepare_stream(c);
  REQUIRE(rdb_connection_execute(c) == RDB_NEED_INPUT);
  upload_stream(c, mib);
  download_stream(c, mib);
  REQUIRE(rdb_connection_disconnect(c) == RDB_OK);
  rdb_close(c);
  empty(ctl);
  replay_denied(data, key);
  memory_bound(before, rss_kib());
  printf("Container transfer: %lu MiB + 17 bytes each direction; client RSS "
         "%ld -> %ld KiB\n",
         mib, before, rss_kib());
  for (i = 0; i < 3; ++i) {
    c = attach(ctl, data, faults[i], key);
    prepare_stream(c);
    status = rdb_connection_execute(c);
    if (i == 0)
      REQUIRE(status == RDB_OUTCOME_UNKNOWN);
    else {
      REQUIRE(status == RDB_NEED_INPUT);
      if (i == 1)
        REQUIRE(rdb_connection_param_write(c, "x", 1) == RDB_OUTCOME_UNKNOWN);
      else {
        upload_stream(c, mib);
        REQUIRE(rdb_connection_column_read(c, block, sizeof(block), &n, &eof) ==
                RDB_TRANSPORT_ERROR);
      }
    }
    REQUIRE(rdb_connection_execute(c) == RDB_STATE_ERROR);
    rdb_close(c);
    empty(ctl);
    replay_denied(data, key);
  }
  /* EOF at SQL, input, and partially consumed output releases backend state. */
  for (i = 0; i < 3; ++i) {
    c = attach(ctl, data, NULL, key);
    if (i == 0) {
      REQUIRE(rdb_connection_connect(c) == RDB_OK);
      REQUIRE(rdb_connection_sql_begin(c) == RDB_OK);
      REQUIRE(rdb_connection_sql_write(c, "INSERT", 6) == RDB_OK);
    } else {
      prepare_stream(c);
      REQUIRE(rdb_connection_execute(c) == RDB_NEED_INPUT);
    }
    if (i == 1) {
      for (n = 0; n < 64; ++n)
        block[n] = (unsigned char)n;
      REQUIRE(rdb_connection_param_write(c, block, 64) == RDB_OK);
    }
    if (i == 2) {
      upload_stream(c, mib);
      REQUIRE(rdb_connection_column_read(c, block, sizeof(block), &n, &eof) ==
              RDB_OK);
    }
    rdb_close(c);
    empty(ctl);
    replay_denied(data, key);
  }
  c = attach(ctl, data, NULL, key);
  prepare_stream(c);
  REQUIRE(rdb_connection_execute(c) == RDB_NEED_INPUT);
  REQUIRE(query(ctl, "close", key, &o) == RDB_OK);
  REQUIRE(rdb_connection_param_write(c, "x", 1) == RDB_OUTCOME_UNKNOWN);
  rdb_close(c);
  empty(ctl);
  replay_denied(data, key);
  /* The parent survived all worker failures and administrative close. */
  REQUIRE(query(ctl, "get", NULL, &o) == RDB_OK && o.count == 1);
  puts("Connection isolation, worker crashes, uncertain operations, EOF and "
       "administrative cleanup passed");
}
static void idle_stop(int sig) {
  (void)sig;
  _exit(0);
}
int main(int argc, char **argv) {
  rdb_endpoint ctl, data;
  rdb_connection *c;
  records o;
  unsigned long mib;
  char key[65], *end;
  int fd, status;
  if (argc == 1 || (argc == 2 && strcmp(argv[1], "idle") == 0)) {
    signal(SIGTERM, idle_stop);
    signal(SIGINT, idle_stop);
    for (;;)
      pause();
  }
  REQUIRE(argc == 6);
  ctl.kind = data.kind =
      strcmp(argv[2], "abstract") == 0 ? RDB_ABSTRACT : RDB_PATHNAME;
  ctl.address = argv[3];
  data.address = argv[4];
  mib = strtoul(argv[5], &end, 10);
  REQUIRE(*end == 0 && mib > 0 && mib <= 8192);
  if (strcmp(argv[1], "probe") == 0)
    return query(&ctl, "list", NULL, &o) == RDB_OK ? 0 : 1;
  if (strcmp(argv[1], "denied") == 0) {
    REQUIRE(query(&ctl, "list", NULL, &o) != RDB_OK);
    uid_rejected_before_key(&data);
    return 0;
  }
  if (strcmp(argv[1], "run") == 0) {
    configure(&ctl, &data, mib);
    scenarios(&ctl, &data, mib);
    return 0;
  }
  if (strcmp(argv[1], "remember") == 0) {
    REQUIRE(query(&ctl, "open", NULL, &o) == RDB_OK);
    fd = open("/work/restart.key", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    REQUIRE(fd >= 0 && write(fd, o.key, 64) == 64 && close(fd) == 0);
    return 0;
  }
  if (strcmp(argv[1], "owner") == 0) {
    REQUIRE(read(STDIN_FILENO, key, 64) == 64);
    key[64] = 0;
    /* This UID is allowed on both sockets but did not create the key. */
    REQUIRE(query(&ctl, "list", NULL, &o) == RDB_OK);
    replay_denied(&data, key);
    REQUIRE(query(&ctl, "close", key, &o) != RDB_OK);
    return 0;
  }
  REQUIRE(strcmp(argv[1], "restart") == 0);
  fd = open("/work/restart.key", O_RDONLY);
  REQUIRE(fd >= 0 && read(fd, key, 64) == 64 && close(fd) == 0);
  key[64] = 0;
  replay_denied(&data, key);
  empty(&ctl);
  c = attach(&ctl, &data, NULL, key);
  prepare_stream(c);
  status = rdb_connection_execute(c);
  REQUIRE(status == RDB_NEED_INPUT);
  upload_stream(c, mib);
  download_stream(c, mib);
  REQUIRE(rdb_connection_disconnect(c) == RDB_OK);
  rdb_close(c);
  empty(&ctl);
  REQUIRE(unlink("/work/restart.key") == 0);
  puts("Restart restored definitions and invalidated the old connection key");
  return 0;
}
