#include "service_test.h"
const char *daemon_path;
int endpoint_kind;
pid_t service_pid;
rdb_document rows[130];
int row_count;
static void cleanup(void) {
  int status, i;
  if (service_pid <= 0)
    return;
  kill(service_pid, SIGTERM);
  for (i = 0; i < 300; ++i) {
    if (waitpid(service_pid, &status, WNOHANG) == service_pid) {
      service_pid = 0;
      return;
    }
    pause_ms(10);
  }
  kill(service_pid, SIGKILL);
  waitpid(service_pid, &status, 0);
  service_pid = 0;
}
double now(void) {
  struct timespec t;
  CHECK(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
  return (double)t.tv_sec + (double)t.tv_nsec / 1000000000.0;
}
void pause_ms(long milliseconds) {
  struct timespec t;
  t.tv_sec = milliseconds / 1000;
  t.tv_nsec = (milliseconds % 1000) * 1000000L;
  while (nanosleep(&t, &t) < 0 && errno == EINTR) {
  }
}
int peer(const char *address) {
  struct sockaddr_un un;
  struct timeval timeout;
  int fd;
  size_t length;
  memset(&un, 0, sizeof(un));
  un.sun_family = AF_UNIX;
  length = strlen(address);
  CHECK(length < sizeof(un.sun_path));
  memcpy(un.sun_path + (endpoint_kind ? 1 : 0), address, length);
  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  CHECK(fd >= 0);
  timeout.tv_sec = 2;
  timeout.tv_usec = 0;
  CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
        0);
  CHECK(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) ==
        0);
  if (connect(fd, (struct sockaddr *)&un,
              (socklen_t)(offsetof(struct sockaddr_un, sun_path) + length +
                          1)) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}
void send_bytes(int fd, const void *data, size_t size) {
  const unsigned char *p;
  ssize_t n;
  p = (const unsigned char *)data;
  while (size) {
    n = write(fd, p, size);
    CHECK(n > 0);
    p += n;
    size -= (size_t)n;
  }
}
void receive_bytes(int fd, void *data, size_t size) {
  unsigned char *p;
  ssize_t n;
  p = (unsigned char *)data;
  while (size) {
    n = read(fd, p, size);
    CHECK(n > 0);
    p += n;
    size -= (size_t)n;
  }
}
void send_text(int fd, const char *text) { send_bytes(fd, text, strlen(text)); }
void response(int fd, unsigned int operation_code) {
  unsigned char bytes[24], result[16];
  rdb_header header;
  receive_bytes(fd, bytes, sizeof(bytes));
  CHECK(rdb_header_decode(bytes, 65536, &header) == RDB_OK);
  CHECK(header.opcode == operation_code && header.flags == 1 &&
        header.statement == (operation_code >= 16 ? 1UL : 0UL));
  CHECK(header.length.low == 16 && header.length.high == 0);
  receive_bytes(fd, result, sizeof(result));
  CHECK(result[0] == 0 && result[1] == 0 && result[2] == 0 && result[3] == 0);
}
void wire(int fd, unsigned int operation_code, const void *payload,
          size_t length) {
  rdb_header header;
  unsigned char bytes[24];
  memset(&header, 0, sizeof(header));
  header.opcode = operation_code;
  header.statement = operation_code >= 16 ? 1 : 0;
  header.length.low = (unsigned long)length;
  CHECK(rdb_header_encode(&header, bytes) == RDB_OK);
  send_bytes(fd, bytes, sizeof(bytes));
  if (length)
    send_bytes(fd, payload, length);
  response(fd, operation_code);
}
int attach(const char *address, const char *key) {
  int fd;
  unsigned char accepted;
  static const unsigned char hello[8] = {0, 0, 0, 0, 0, 0, 1, 0};
  fd = peer(address);
  CHECK(fd >= 0);
  send_bytes(fd, key, 64);
  receive_bytes(fd, &accepted, 1);
  CHECK(accepted == 0);
  wire(fd, 1, hello, sizeof(hello));
  wire(fd, 2, NULL, 0);
  return fd;
}
void bad_key(const char *address, const char *key) {
  int fd;
  unsigned char byte;
  fd = peer(address);
  CHECK(fd >= 0);
  send_bytes(fd, key, 64);
  receive_bytes(fd, &byte, 1);
  CHECK(byte == 1);
  CHECK(read(fd, &byte, 1) == 0);
  close(fd);
}
rdb_document document(const char *op, const char *name, const char *address) {
  rdb_document request;
  memset(&request, 0, sizeof(request));
  strcpy(request.operation, op);
  if (name)
    strcpy(request.name, name);
  if (address)
    strcpy(request.address, address);
  strcpy(request.driver, "mock");
  strcpy(request.kind, endpoint_kind ? "abstract" : "pathname");
  strcpy(request.dsn, "fixture");
  return request;
}
void read_records(int fd) {
  lonejson *json;
  lonejson_stream *stream;
  lonejson_error error;
  lonejson_stream_result result;
  rdb_json_input input;
  json = rdb_json_new();
  CHECK(json != NULL);
  memset(&input, 0, sizeof(input));
  input.fd = fd;
  stream = rdb_json_stream(json, &input);
  CHECK(stream != NULL);
  lonejson_error_init(&error);
  row_count = 0;
  while ((result = lonejson_stream_next(stream, &rows[row_count], &error)) ==
         LONEJSON_STREAM_OBJECT) {
    ++row_count;
    CHECK(row_count < 130);
  }
  CHECK(result == LONEJSON_STREAM_EOF && row_count > 0 &&
        strcmp(rows[row_count - 1].type, "done") == 0);
  lonejson_stream_close(stream);
  lonejson_free(json);
}
void query_document(const char *control, const rdb_document *request,
                    int success) {
  int fd;
  lonejson *json;
  fd = peer(control);
  CHECK(fd >= 0);
  json = rdb_json_new();
  CHECK(json != NULL);
  CHECK(rdb_json_send(json, fd, request) == RDB_OK);
  lonejson_free(json);
  CHECK(shutdown(fd, SHUT_WR) == 0);
  read_records(fd);
  close(fd);
  if (success >= 0 &&
      strcmp(rows[row_count - 1].status, success ? "ok" : "error") != 0)
    fprintf(stderr, "Control %s name=%s: expected %s, received %s (%s)\n",
            request->operation, request->name, success ? "ok" : "error",
            rows[row_count - 1].status, rows[row_count - 1].message);
  CHECK(success < 0 ||
        strcmp(rows[row_count - 1].status, success ? "ok" : "error") == 0);
}
void query_raw(const char *control, const char *request, int fragment,
               int success) {
  int fd;
  size_t i, length;
  fd = peer(control);
  CHECK(fd >= 0);
  length = strlen(request);
  if (fragment) {
    for (i = 0; i < length; ++i)
      send_bytes(fd, request + i, 1);
  } else
    send_bytes(fd, request, length);
  CHECK(shutdown(fd, SHUT_WR) == 0);
  read_records(fd);
  close(fd);
  CHECK(strcmp(rows[row_count - 1].status, success ? "ok" : "error") == 0);
}
void operation(const char *control, const char *op, const char *name,
               const char *address, int success) {
  rdb_document request;
  request = document(op, name, address);
  query_document(control, &request, success);
}
void open_key(const char *control, const char *name, char key[65]) {
  size_t i;
  operation(control, "open", name, NULL, 1);
  CHECK(row_count == 2 && strcmp(rows[0].type, "session") == 0 &&
        strcmp(rows[0].status, "pending") == 0);
  CHECK(strlen(rows[0].connection_key) == 64 && rows[0].attributes[0] == 0);
  for (i = 0; i < 64; ++i)
    CHECK(strchr("0123456789abcdef", rows[0].connection_key[i]) != NULL);
  strcpy(key, rows[0].connection_key);
}
void empty_sessions(const char *control) {
  double deadline;
  rdb_document request;
  deadline = now() + 3;
  request = document("sessions", NULL, NULL);
  /* Reaping may invalidate an in-progress streamed registry view. Retry only
   * until a complete successful empty view is observed, as the protocol
   * requires. */
  do {
    query_document(control, &request, -1);
    if (row_count == 1 && strcmp(rows[0].status, "ok") == 0)
      return;
    pause_ms(10);
  } while (now() < deadline);
  CHECK(0);
}
void start_service(const char *control, const char *state,
                   const char *const *extra, int readiness) {
  const char *args[32];
  int n, fd, i, status;
  double deadline;
  CHECK(service_pid == 0);
  CHECK(atexit(cleanup) == 0);
  n = 0;
  args[n++] = daemon_path;
  args[n++] = "--control";
  args[n++] = control;
  args[n++] = "--state";
  args[n++] = state;
  if (endpoint_kind) {
    args[n++] = "--socket-kind";
    args[n++] = "abstract";
  }
  if (extra)
    for (i = 0; extra[i]; ++i) {
      CHECK(n < 30);
      args[n++] = extra[i];
    }
  args[n] = NULL;
  service_pid = fork();
  CHECK(service_pid >= 0);
  if (service_pid == 0) {
    fd = open("daemon.log", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
      _exit(126);
    dup2(fd, 1);
    dup2(fd, 2);
    close(fd);
    execv(daemon_path, (char *const *)args);
    _exit(127);
  }
  deadline = now() + 4;
  do {
    CHECK(waitpid(service_pid, &status, WNOHANG) == 0);
    fd = peer(control);
    if (fd >= 0) {
      close(fd);
      if (readiness)
        operation(control, "list", NULL, NULL, 1);
      return;
    }
    pause_ms(10);
  } while (now() < deadline);
  CHECK(0);
}
void stop_service(void) {
  int status, i;
  struct stat st;
  CHECK(service_pid > 0 && kill(service_pid, SIGTERM) == 0);
  for (i = 0; i < 500; ++i) {
    if (waitpid(service_pid, &status, WNOHANG) == service_pid)
      break;
    pause_ms(10);
  }
  CHECK(i < 500 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  service_pid = 0;
  CHECK(stat("daemon.log", &st) == 0 && st.st_size == 0);
}
int daemon_exit(const char *const *arguments) {
  pid_t pid;
  int fd, status, i;
  pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    fd = open("negative.log", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
      _exit(126);
    dup2(fd, 1);
    dup2(fd, 2);
    close(fd);
    execv(daemon_path, (char *const *)arguments);
    _exit(127);
  }
  for (i = 0; i < 200; ++i) {
    if (waitpid(pid, &status, WNOHANG) == pid)
      break;
    pause_ms(10);
  }
  if (i == 200) {
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    CHECK(0);
  }
  CHECK(WIFEXITED(status));
  return WEXITSTATUS(status);
}
void write_file(const char *name, const char *text, unsigned int mode) {
  int fd;
  fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, (mode_t)mode);
  CHECK(fd >= 0);
  send_bytes(fd, text, strlen(text));
  CHECK(close(fd) == 0 && chmod(name, (mode_t)mode) == 0);
}
long parent_rss(void) {
  char name[64], line[256];
  FILE *input;
  long rss;
  sprintf(name, "/proc/%ld/status", (long)service_pid);
  input = fopen(name, "r");
  if (!input)
    return 0;
  rss = 0;
  while (fgets(line, sizeof(line), input))
    if (sscanf(line, "VmRSS: %ld", &rss) == 1)
      break;
  CHECK(fclose(input) == 0);
  return rss;
}
