#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "mock.h"
#include <errno.h>
#include <remotedb/service.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                  \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)
/* External syscall fault injection, confined to this test executable. */
static int fail_directory_sync;
static void host_reap(int sig) {
  int saved;
  (void)sig;
  saved = errno;
  while (waitpid(-1, NULL, WNOHANG) > 0) {
  }
  errno = saved;
}
int fsync(int fd) {
  struct stat st;
  if (fail_directory_sync && fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
    errno = EIO;
    return -1;
  }
  return (int)syscall(SYS_fsync, fd);
}
typedef struct observed {
  char key[65];
  int count;
  int cancel;
} observed;
static int consume(void *user, const rdb_control_record *record) {
  observed *o;
  o = (observed *)user;
  if (strcmp(record->type, "done") != 0)
    ++o->count;
  if (record->connection_key[0] != 0)
    strcpy(o->key, record->connection_key);
  CHECK(record->definition.attributes[0] == 0);
  return o->cancel ? RDB_STATE_ERROR : RDB_OK;
}
static int create(void *user, const rdb_definition *d, rdb_backend *backend) {
  rdb_mock *m;
  (void)user;
  m = (rdb_mock *)malloc(sizeof(*m));
  if (m == NULL)
    return RDB_MEMORY_ERROR;
  rdb_mock_init(m);
  m->output_length.low = strcmp(d->attributes, "bob") == 0 ? 2 : 1;
  *backend = m->backend;
  return RDB_OK;
}
static void destroy(void *user, rdb_backend *backend) {
  (void)user;
  free(backend->context);
}
static void execute(rdb_connection *connection, unsigned long result_length) {
  unsigned char data[8];
  size_t count;
  int eof;
  CHECK(rdb_connection_connect(connection) == RDB_OK);
  CHECK(rdb_connection_sql_begin(connection) == RDB_OK);
  CHECK(rdb_connection_sql_write(connection, "SELECT ?", 8) == RDB_OK);
  CHECK(rdb_connection_sql_end(connection) == RDB_OK);
  CHECK(rdb_connection_execute(connection) == RDB_OK);
  CHECK(rdb_connection_reply(connection)->value.low == result_length);
  CHECK(rdb_connection_column_read(connection, data, sizeof(data), &count,
                                   &eof) == RDB_OK);
  CHECK(count == result_length && !eof);
  CHECK(rdb_connection_column_read(connection, data, sizeof(data), &count,
                                   &eof) == RDB_NO_DATA);
  CHECK(count == 0 && eof);
}
int main(int argc, char **argv) {
  rdb_endpoint endpoint, data;
  rdb_control_request request;
  rdb_service_config config;
  rdb_connection *a, *b;
  observed o;
  char control[108], path[108], state[256], lock[262], error[256], invalid[65];
  int pipes[2], status, i, reaping;
  pid_t pid;
  struct timespec delay;
  CHECK(argc == 2 || argc == 3);
  fail_directory_sync = argc == 3 && strcmp(argv[2], "uncertain") == 0;
  reaping = argc == 3 && strcmp(argv[2], "ignored") == 0     ? 1
            : argc == 3 && strcmp(argv[2], "nocldwait") == 0 ? 2
            : argc == 3 && strcmp(argv[2], "handler") == 0   ? 3
                                                             : 0;
  CHECK(argc == 2 || fail_directory_sync || reaping != 0);
  CHECK(mkdir(argv[1], 0700) == 0);
  CHECK(chdir(argv[1]) == 0);
  strcpy(control, "control");
  strcpy(path, "data");
  strcpy(state, "definitions");
  sprintf(lock, "%s.lock", state);
  memset(&config, 0, sizeof(config));
  config.control.address = control;
  config.state_path = state;
  {
    rdb_service_config invalid_config;
    invalid_config = config;
    invalid_config.control.address = NULL;
    CHECK(rdb_service_run(&invalid_config, create, destroy, NULL, error,
                          sizeof(error)) == RDB_ARGUMENT_ERROR);
    invalid_config.control.address = control;
    invalid_config.control.kind = 99;
    CHECK(rdb_service_run(&invalid_config, create, destroy, NULL, error,
                          sizeof(error)) == RDB_ARGUMENT_ERROR);
    CHECK(access(lock, F_OK) != 0);
  }
  CHECK(pipe(pipes) == 0);
  config.stop_fd = pipes[0];
  pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    if (reaping != 0) {
      struct sigaction action;
      memset(&action, 0, sizeof(action));
      sigemptyset(&action.sa_mask);
      action.sa_handler = reaping == 1   ? SIG_IGN
                          : reaping == 2 ? SIG_DFL
                                         : host_reap;
      action.sa_flags = reaping == 2 ? SA_NOCLDWAIT : SA_RESTART;
      CHECK(sigaction(SIGCHLD, &action, NULL) == 0);
    }
    close(pipes[1]);
    status =
        rdb_service_run(&config, create, destroy, NULL, error, sizeof(error));
    close(pipes[0]);
    exit(status == (fail_directory_sync ? RDB_OUTCOME_UNKNOWN : RDB_OK) ? 0
                                                                        : 1);
  }
  close(pipes[0]);
  endpoint.kind = RDB_PATHNAME;
  endpoint.address = control;
  data.kind = RDB_PATHNAME;
  data.address = path;
  memset(&request, 0, sizeof(request));
  strcpy(request.operation, "list");
  delay.tv_sec = 0;
  delay.tv_nsec = 10000000L;
  for (i = 0; i < 100; ++i) {
    memset(&o, 0, sizeof(o));
    if (rdb_control_query(&endpoint, &request, consume, &o) == RDB_OK)
      break;
    nanosleep(&delay, NULL);
  }
  CHECK(i != 100 && o.count == 0);
  strcpy(request.operation, "put");
  strcpy(request.definition.name, "db");
  strcpy(request.definition.driver, "mock");
  strcpy(request.definition.address, path);
  strcpy(request.definition.attributes, "alice");
  if (fail_directory_sync) {
    struct stat st;
    CHECK(rdb_control_query(&endpoint, &request, consume, &o) ==
          RDB_OUTCOME_UNKNOWN);
    close(pipes[1]);
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0);
    CHECK(stat(state, &st) == 0 && st.st_size > 0 &&
          (st.st_mode & 0777) == 0600);
    CHECK(access(control, F_OK) != 0 && access(path, F_OK) != 0);
    CHECK(unlink(state) == 0 && unlink(lock) == 0);
    CHECK(chdir("..") == 0 && rmdir(argv[1]) == 0);
    puts("Directory-sync failure reports unknown, retains replaced state and "
         "stops service");
    return 0;
  }
  CHECK(rdb_control_query(&endpoint, &request, consume, &o) == RDB_OK);
  memset(&request, 0, sizeof(request));
  strcpy(request.operation, "open");
  strcpy(request.definition.name, "db");
  strcpy(request.definition.attributes, "bob");
  CHECK(rdb_control_query(&endpoint, &request, consume, &o) == RDB_OK);
  CHECK(strlen(o.key) == 64);
  a = NULL;
  CHECK(rdb_open_key(&data, o.key, NULL, &a) == RDB_OK);
  execute(
      a, 2); /* Per-connection override captured without altering definition. */
  request.definition.attributes[0] = 0;
  CHECK(rdb_control_query(&endpoint, &request, consume, &o) == RDB_OK);
  b = NULL;
  CHECK(rdb_open_key(&data, o.key, NULL, &b) == RDB_OK);
  execute(b, 1);
  CHECK(rdb_connection_reset(a) == RDB_OK);
  CHECK(rdb_connection_sql_begin(a) == RDB_OK);
  CHECK(rdb_connection_sql_write(a, "CALL x()", 8) == RDB_OK);
  CHECK(rdb_connection_sql_end(a) == RDB_OK);
  CHECK(rdb_connection_execute(a) == RDB_OK);
  CHECK(rdb_connection_reply(a)->value.low == 2);
  CHECK(rdb_connection_disconnect(a) == RDB_OK);
  rdb_close(a);
  CHECK(rdb_connection_disconnect(b) == RDB_OK);
  rdb_close(b);
  memset(&request, 0, sizeof(request));
  strcpy(request.operation, "sessions");
  for (i = 0; i < 100; ++i) {
    memset(&o, 0, sizeof(o));
    status = rdb_control_query(&endpoint, &request, consume, &o);
    CHECK(status == RDB_OK || status == RDB_ERROR);
    if (status == RDB_OK && o.count == 0)
      break;
    nanosleep(&delay, NULL);
  }
  CHECK(i < 100);
  memset(invalid, '0', 64);
  invalid[64] = 0;
  a = (rdb_connection *)1;
  CHECK(rdb_open_key(&data, invalid, NULL, &a) == RDB_ERROR && a == NULL);
  CHECK(rdb_open_key(&data, "short", NULL, &a) == RDB_ARGUMENT_ERROR &&
        a == NULL);
  invalid[0] = 'z';
  CHECK(rdb_open_key(&data, invalid, NULL, &a) == RDB_ARGUMENT_ERROR);
  CHECK(rdb_open_endpoint(NULL, NULL, &a) == RDB_ARGUMENT_ERROR && a == NULL);
  memset(&request, 0, sizeof(request));
  strcpy(request.operation, "list");
  o.cancel = 1;
  CHECK(rdb_control_query(&endpoint, &request, consume, &o) == RDB_STATE_ERROR);
  o.cancel = 0;
  strcpy(request.operation, "delete");
  strcpy(request.definition.name, "db");
  CHECK(rdb_control_query(&endpoint, &request, consume, &o) == RDB_OK);
  memset(request.operation, 'x', sizeof(request.operation));
  CHECK(rdb_control_query(&endpoint, &request, consume, &o) ==
        RDB_ARGUMENT_ERROR);
  CHECK(rdb_service_run(NULL, create, destroy, NULL, error, sizeof(error)) ==
        RDB_ARGUMENT_ERROR);
  close(pipes[1]);
  CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0);
  CHECK(access(control, F_OK) != 0 && access(path, F_OK) != 0);
  CHECK(unlink(state) == 0);
  CHECK(unlink(lock) == 0);
  CHECK(chdir("..") == 0);
  CHECK(rmdir(argv[1]) == 0);
  puts("Public control API, credential isolation, cancellation and cleanup "
       "passed");
  return 0;
}
