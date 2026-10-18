#define _POSIX_C_SOURCE 200809L
#include "mock.h"
#include <errno.h>
#include <fcntl.h>
#include <remotedb/service.h>
#include <remotedb/version.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static volatile sig_atomic_t signal_fd = -1;
static void stop(int number) {
  int saved;
  unsigned char byte;
  saved = errno;
  byte = (unsigned char)number;
  if (signal_fd >= 0) {
    ssize_t n;
    n = write((int)signal_fd, &byte, 1);
    (void)n;
  }
  errno = saved;
}
static int factory(void *user, const rdb_definition *definition,
                   rdb_backend *backend) {
  rdb_mock *mock;
  (void)user;
  if (strcmp(definition->driver, "mock") != 0)
    return RDB_UNSUPPORTED;
  mock = (rdb_mock *)malloc(sizeof(*mock));
  if (mock == NULL)
    return RDB_MEMORY_ERROR;
  rdb_mock_init(mock);
  *backend = mock->backend;
  return RDB_OK;
}
static void cleanup(void *user, rdb_backend *backend) {
  (void)user;
  free(backend->context);
}
static int uids(const char *text, unsigned long *values, size_t *count) {
  char *end;
  unsigned long value;
  *count = 0;
  if (text == NULL)
    return RDB_OK;
  while (*text != 0 && *count < 64) {
    if (*text < '0' || *text > '9')
      return RDB_ARGUMENT_ERROR;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || value != (unsigned long)(uid_t)value ||
        (end[0] != 0 && end[0] != ','))
      return RDB_ARGUMENT_ERROR;
    values[(*count)++] = value;
    text = end;
    if (*text == ',') {
      ++text;
      if (*text == 0)
        return RDB_ARGUMENT_ERROR;
    }
  }
  return *text == 0 && *count != 0 ? RDB_OK : RDB_ARGUMENT_ERROR;
}
int main(int argc, char **argv) {
  rdb_service_config config;
  struct sigaction action;
  unsigned long control_uids[64], data_uids[64];
  const char *control, *state, *kind, *control_text, *data_text;
  char error[256];
  int i, pipes[2], status;
  control = getenv("REMOTEDB_CONTROL");
  state = getenv("REMOTEDB_STATE");
  kind = getenv("REMOTEDB_SOCKET_KIND");
  control_text = getenv("REMOTEDB_CONTROL_UIDS");
  data_text = getenv("REMOTEDB_DATA_UIDS");
  for (i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--version") == 0) {
      puts("remotedbd " REMOTEDB_VERSION);
      return 0;
    }
    if (strcmp(argv[i], "--help") == 0) {
      puts(
          "Usage: remotedbd [--mock] [--control ADDRESS] [--state FILE]\n"
          "                 [--socket-kind pathname|abstract]\n"
          "                 [--control-uids UID,...] [--data-uids UID,...]\n"
          "Defaults: ./remotedb-control.sock, ./remotedb.jsonl, pathname, "
          "daemon UID.\n"
          "Environment: REMOTEDB_CONTROL, REMOTEDB_STATE, "
          "REMOTEDB_SOCKET_KIND,\n"
          "             REMOTEDB_CONTROL_UIDS, REMOTEDB_DATA_UIDS. Flags win.");
      puts("Only the mock driver is available until the cpktdb iODBC backend "
           "is implemented.\n"
           "Abstract endpoints are Linux-only. State parent directories must "
           "exist.");
      return 0;
    }
    if (strcmp(argv[i], "--mock") == 0)
      continue;
    if (i + 1 == argc)
      goto invalid;
    if (strcmp(argv[i], "--control") == 0)
      control = argv[++i];
    else if (strcmp(argv[i], "--state") == 0)
      state = argv[++i];
    else if (strcmp(argv[i], "--socket-kind") == 0)
      kind = argv[++i];
    else if (strcmp(argv[i], "--control-uids") == 0)
      control_text = argv[++i];
    else if (strcmp(argv[i], "--data-uids") == 0)
      data_text = argv[++i];
    else
      goto invalid;
  }
  memset(&config, 0, sizeof(config));
  config.control.address =
      control != NULL ? control : "./remotedb-control.sock";
  config.state_path = state != NULL ? state : "./remotedb.jsonl";
  if (kind != NULL && strcmp(kind, "abstract") == 0)
    config.control.kind = RDB_ABSTRACT;
  else if (kind != NULL && strcmp(kind, "pathname") != 0)
    goto invalid;
  if (uids(control_text, control_uids, &config.control_uid_count) != RDB_OK ||
      uids(data_text, data_uids, &config.data_uid_count) != RDB_OK)
    goto invalid;
  config.control_uids = control_uids;
  config.data_uids = data_uids;
  if (pipe(pipes) != 0)
    return 1;
  fcntl(pipes[1], F_SETFL, O_NONBLOCK);
  fcntl(pipes[0], F_SETFD, FD_CLOEXEC);
  fcntl(pipes[1], F_SETFD, FD_CLOEXEC);
  config.stop_fd = pipes[0];
  signal_fd = pipes[1];
  memset(&action, 0, sizeof(action));
  action.sa_handler = stop;
  sigemptyset(&action.sa_mask);
  sigaction(SIGTERM, &action, NULL);
  sigaction(SIGINT, &action, NULL);
  status =
      rdb_service_run(&config, factory, cleanup, NULL, error, sizeof(error));
  signal_fd = -1;
  close(pipes[0]);
  close(pipes[1]);
  if (status != RDB_OK)
    fprintf(stderr, "%s\n", error);
  return status == RDB_OK ? 0 : 1;
invalid:
  fputs("Invalid option or UID list. Use remotedbd --help.\n", stderr);
  return 2;
}
