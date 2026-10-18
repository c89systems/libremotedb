#define _POSIX_C_SOURCE 200809L
#include "stream_fixture.h"
#include <fcntl.h>
#include <remotedb/service.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>
typedef struct instance {
  stream_fixture stream;
  long before;
} instance;
static volatile sig_atomic_t stop_fd = -1;
static void stop(int sig) {
  char byte;
  ssize_t n;
  byte = (char)sig;
  n = write((int)stop_fd, &byte, 1);
  (void)n;
}
static long rss(void) {
  struct rusage r;
  if (getrusage(RUSAGE_SELF, &r) != 0)
    _exit(91);
  return r.ru_maxrss;
}
static int factory(void *user, const rdb_definition *d, rdb_backend *b) {
  instance *s;
  unsigned long mib;
  char *end;
  (void)user;
  mib = strtoul(d->dsn, &end, 10);
  if (*end != 0 || mib == 0 || mib > 8192)
    return RDB_ARGUMENT_ERROR;
  s = (instance *)malloc(sizeof(*s));
  if (s == NULL)
    return RDB_MEMORY_ERROR;
  stream_fixture_init(&s->stream, mib);
  if (strcmp(d->attributes, "crash-execute") == 0)
    s->stream.mode = 1;
  if (strcmp(d->attributes, "crash-input") == 0)
    s->stream.mode = 2;
  if (strcmp(d->attributes, "crash-output") == 0)
    s->stream.mode = 3;
  s->before = rss();
  *b = s->stream.backend;
  return RDB_OK;
}
static void cleanup(void *user, rdb_backend *b) {
  instance *s;
  char line[256];
  int fd, n;
  ssize_t written;
  s = (instance *)b->context; /* stream is the first member */
  n = snprintf(line, sizeof(line), "%lu %lu %lu %lu %d %d %d %lu %ld %ld\n",
               s->stream.input.high, s->stream.input.low, s->stream.output.high,
               s->stream.output.low, s->stream.valid, s->stream.connects,
               s->stream.disconnects, (unsigned long)s->stream.largest,
               s->before, rss());
  fd = open((const char *)user, O_WRONLY | O_CREAT | O_APPEND, 0600);
  if (fd < 0 || n <= 0 || (size_t)n >= sizeof(line))
    _exit(92);
  written = write(fd, line, (size_t)n);
  if (written != n || close(fd) != 0)
    _exit(92);
  free(s);
}
int main(int argc, char **argv) {
  rdb_service_config c;
  struct sigaction sa;
  unsigned long allowed[2];
  int pipes[2], status;
  char error[256];
  if (argc != 5)
    return 2;
  memset(&c, 0, sizeof(c));
  c.control.kind =
      strcmp(argv[1], "abstract") == 0 ? RDB_ABSTRACT : RDB_PATHNAME;
  c.control.address = argv[2];
  c.state_path = argv[3];
  /* The second allowed identity tests that key ownership is stricter than
   * admission. A third identity must fail both socket allowlists. */
  allowed[0] = (unsigned long)geteuid();
  allowed[1] = allowed[0] + 1;
  c.control_uids = c.data_uids = allowed;
  c.control_uid_count = c.data_uid_count = 2;
  if (pipe(pipes) != 0)
    return 1;
  fcntl(pipes[1], F_SETFL, O_NONBLOCK);
  stop_fd = pipes[1];
  c.stop_fd = pipes[0];
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = stop;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  status = rdb_service_run(&c, factory, cleanup, argv[4], error, sizeof(error));
  close(pipes[0]);
  close(pipes[1]);
  if (status != RDB_OK)
    fprintf(stderr, "%s\n", error);
  return status == RDB_OK ? 0 : 1;
}
