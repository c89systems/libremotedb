#include "service_test.h"
static char *contents(const char *name) {
  static char text[16384];
  FILE *file;
  size_t used;
  file = fopen(name, "rb");
  CHECK(file != NULL);
  used = fread(text, 1, sizeof(text) - 1, file);
  CHECK(!ferror(file));
  text[used] = 0;
  CHECK(fclose(file) == 0);
  return text;
}
static void close_key(const char *key) {
  rdb_document request;
  request = document("close", NULL, NULL);
  strcpy(request.connection_key, key);
  query_document("control", &request, 1);
}
static void stored_definition(void) {
  int fd;
  lonejson *json;
  lonejson_stream *stream;
  lonejson_error error;
  rdb_document stored;
  fd = open("state", O_RDONLY);
  CHECK(fd >= 0);
  json = rdb_json_new();
  CHECK(json != NULL);
  lonejson_error_init(&error);
  stream = lonejson_stream_open_fd(json, rdb_json_definition_map(), fd, &error);
  CHECK(stream != NULL);
  CHECK(lonejson_stream_next(stream, &stored, &error) ==
        LONEJSON_STREAM_OBJECT);
  CHECK(strcmp(stored.dsn, "fixture-\303\245") == 0);
  CHECK(strcmp(stored.attributes, "UID=alice;PWD=\"s\\ecret\";") == 0);
  CHECK(lonejson_stream_next(stream, &stored, &error) == LONEJSON_STREAM_EOF);
  lonejson_stream_close(stream);
  lonejson_free(json);
  close(fd);
}
static void run_client(const char *const *args, const char *input) {
  pid_t pid;
  int fd, status;
  write_file("client.input", input, 0600);
  pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    fd = open("client.input", O_RDONLY);
    if (fd < 0)
      _exit(126);
    dup2(fd, 0);
    close(fd);
    fd = open("client.output", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
      _exit(126);
    dup2(fd, 1);
    close(fd);
    execv(args[0], (char *const *)args);
    _exit(127);
  }
  CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0);
}
static void control(const char *ctl, const char *example) {
  char first[65], second[65], token[65], invalid[65], keys[128][65], name[64],
      uid[32], before[16384];
  const char *args[12], *extra[7];
  rdb_document request;
  struct stat st;
  int a, b, i, j;
  memset(invalid, '0', 64);
  invalid[64] = 0;
  args[0] = daemon_path;
  args[1] = "--control";
  args[2] = "state";
  args[3] = "--state";
  args[4] = "state";
  args[5] = NULL;
  CHECK(daemon_exit(args) != 0 && access("state", F_OK) != 0);
  args[2] = "./state";
  CHECK(daemon_exit(args) != 0 && access("state", F_OK) != 0);
  start_service("control", "state", NULL, 1);
  CHECK(stat("control", &st) == 0 && (st.st_mode & 0777) == 0600);
  operation("control", "list", NULL, NULL, 1);
  CHECK(row_count == 1);
  CHECK(symlink(".", "alias") == 0 && mkdir("subdir", 0700) == 0);
  {
    const char *reserved[4];
    reserved[0] = "state";
    reserved[1] = "./state";
    reserved[2] = "subdir/../state";
    reserved[3] = "alias/state";
    for (i = 0; i < 4; ++i) {
      operation("control", "put", "reserved", reserved[i], 0);
      CHECK(access("state", F_OK) != 0);
      operation("control", "list", NULL, NULL, 1);
      CHECK(row_count == 1);
    }
  }
  request = document("put", "primary", "data");
  strcpy(request.dsn, "fixture-\303\245");
  strcpy(request.attributes, "UID=alice;PWD=\"s\\ecret\";");
  query_document("control", &request, 1);
  stored_definition();
  CHECK(strstr(contents("state"), "fixture-") != NULL &&
        stat("state", &st) == 0 && (st.st_mode & 0777) == 0600);
  operation("control", "get", "primary", NULL, 1);
  CHECK(rows[0].attributes[0] == 0 &&
        strcmp(rows[0].dsn, "fixture-\303\245") == 0);
  query_raw("control", "{\"operation\":\"list\"}", 1, 1);
  request = document("open", "primary", NULL);
  strcpy(request.attributes, "UID=bob;PWD=only-this-connection;");
  query_document("control", &request, 1);
  strcpy(first, rows[0].connection_key);
  open_key("control", "primary", second);
  CHECK(strcmp(first, second) != 0);
  operation("control", "sessions", NULL, NULL, 1);
  CHECK(row_count == 3);
  for (i = 0; i < 2; ++i)
    CHECK(rows[i].connection_key[0] == 0 && rows[i].attributes[0] == 0);
  CHECK(strstr(contents("state"), first) == NULL &&
        strstr(contents("state"), "only-this-connection") == NULL);
  operation("control", "put", "primary", "data", 0);
  operation("control", "delete", "primary", NULL, 0);
  request = document("close", NULL, NULL);
  strcpy(request.connection_key, invalid);
  query_document("control", &request, 0);
  bad_key("data", invalid);
  a = peer("data");
  CHECK(a >= 0);
  memset(before, 0, 64);
  memcpy(before, "RDB0", 4);
  before[6] = 1;
  send_bytes(a, before, 64);
  receive_bytes(a, before, 1);
  CHECK(before[0] == 1);
  close(a);
  a = attach("data", first);
  bad_key("data", first);
  b = attach("data", second);
  wire(a, 16, NULL, 0);
  wire(a, 17, "CALL proc(?)", 12);
  wire(a, 17, "; SELECT payload FROM documents", 31);
  wire(a, 18, NULL, 0);
  wire(a, 19, NULL, 0);
  wire(b, 16, NULL, 0);
  operation("control", "sessions", NULL, NULL, 1);
  CHECK(row_count == 3 && strcmp(rows[0].status, "active") == 0 &&
        strcmp(rows[1].status, "active") == 0);
  wire(a, 3, NULL, 0);
  close(a);
  close(b);
  empty_sessions("control");
  bad_key("data", first);
  operation("control", "put", "other", "other", 1);
  open_key("control", "primary", token);
  bad_key("other", token);
  close_key(token);
  bad_key("data", token);
  open_key("control", "primary", token);
  a = attach("data", token);
  close_key(token);
  CHECK(read(a, before, 1) == 0);
  close(a);
  empty_sessions("control");
  bad_key("data", token);
  args[0] = ctl;
  args[1] = "control";
  args[2] = "list";
  args[3] = NULL;
  run_client(args, "");
  args[2] = "open";
  args[3] = "primary";
  args[4] = NULL;
  run_client(args, "");
  strcpy(token, contents("client.output"));
  token[64] = 0;
  if (example[0]) {
    args[0] = example;
    args[1] = "data";
    args[2] = NULL;
    run_client(args, token);
  } else {
    a = attach("data", token);
    wire(a, 3, NULL, 0);
    close(a);
  }
  empty_sessions("control");
  request = document("put", "evil", "evil");
  strcpy(request.driver, "/arbitrary/driver.so");
  query_document("control", &request, 0);
  operation("control", "put", "duplicate", "data", 0);
  operation("control", "put", "bad/name", "bad", 0);
  query_raw("control", "{\"operation\":\"list\",\"operation\":\"delete\"}", 0,
            0);
  query_raw("control", "{\"operation\":\"list\\u0000put\"}", 0, 0);
  query_raw("control", "{\"operation\":\"list\"}{\"operation\":\"delete\"}", 0,
            0);
  query_raw("control", "{broken}", 0, 0);
  memset(name, 'x', 63);
  name[63] = 0;
  sprintf(before,
          "{\"operation\":\"put\",\"name\":\"%sx\",\"driver\":\"mock\","
          "\"kind\":\"pathname\",\"address\":\"bad\"}",
          name);
  query_raw("control", before, 0, 0);
  memset(before, 'x', 108);
  before[108] = 0;
  strcpy(name, "{\"operation\":\"put\",\"name\":\"bad\",\"address\":\"");
  {
    char payload[1400];
    sprintf(payload, "%s%.108s\"}", name, before);
    query_raw("control", payload, 0, 0);
    memset(before, 'x', 1024);
    before[1024] = 0;
    sprintf(
        payload,
        "{\"operation\":\"put\",\"name\":\"bad\",\"attributes\":\"%.1024s\"}",
        before);
    query_raw("control", payload, 0, 0);
  }
  strcpy(before, contents("state"));
  CHECK(rename("state", "backup") == 0 && mkdir("state", 0700) == 0);
  operation("control", "put", "failed", "failed", 0);
  CHECK(access("failed", F_OK) != 0);
  operation("control", "list", NULL, NULL, 1);
  CHECK(row_count == 3);
  CHECK(rmdir("state") == 0 && rename("backup", "state") == 0 &&
        strcmp(contents("state"), before) == 0);
  operation("control", "put", "other", "new", 1);
  CHECK(access("other", F_OK) != 0 && access("new", F_OK) == 0);
  operation("control", "delete", "other", NULL, 1);
  CHECK(access("new", F_OK) != 0);
  for (i = 0; i < 128; ++i) {
    open_key("control", "primary", keys[i]);
    for (j = 0; j < i; ++j)
      CHECK(strcmp(keys[i], keys[j]) != 0);
  }
  operation("control", "open", "primary", NULL, 0);
  for (i = 0; i < 128; ++i)
    close_key(keys[i]);
  for (i = 0; i < 63; ++i) {
    sprintf(name, "limit%d", i);
    operation("control", "put", name, name, 1);
  }
  operation("control", "list", NULL, NULL, 1);
  CHECK(row_count == 65);
  operation("control", "put", "overflow", "overflow", 0);
  CHECK(access("overflow", F_OK) != 0);
  for (i = 0; i < 63; ++i) {
    sprintf(name, "limit%d", i);
    operation("control", "delete", name, NULL, 1);
  }
  args[0] = daemon_path;
  args[1] = "--state";
  args[2] = "state";
  args[3] = "--control";
  args[4] = "duplicate";
  args[5] = NULL;
  CHECK(daemon_exit(args) != 0 && access("duplicate", F_OK) != 0);
  stop_service();
  CHECK(access("control", F_OK) != 0 && access("data", F_OK) != 0);
  start_service("control", "state", NULL, 1);
  operation("control", "list", NULL, NULL, 1);
  CHECK(row_count == 2);
  empty_sessions("control");
  bad_key("data", token);
  operation("control", "delete", "primary", NULL, 1);
  CHECK(contents("state")[0] == 0);
  stop_service();
  write_file("control", "caller-owned", 0600);
  args[4] = "control";
  CHECK(daemon_exit(args) != 0 &&
        strcmp(contents("control"), "caller-owned") == 0);
  CHECK(unlink("control") == 0);
  {
    const char *invalid_stores[4];
    invalid_stores[0] = "{";
    invalid_stores[1] =
        "{\"type\":\"definition\",\"name\":\"db\",\"driver\":\"mock\",\"kind\":"
        "\"pathname\",\"address\":\"data\"}\n{\"type\":\"definition\",\"name\":"
        "\"db\",\"driver\":\"mock\",\"kind\":\"pathname\",\"address\":\"data\"}"
        "\n";
    invalid_stores[2] = "{\"type\":\"definition\"}";
    invalid_stores[3] =
        "{\"type\":\"definition\",\"name\":\"db\",\"driver\":\"unknown\","
        "\"kind\":\"pathname\",\"address\":\"data\"}";
    for (i = 0; i < 4; ++i) {
      write_file("state", invalid_stores[i], 0600);
      CHECK(daemon_exit(args) != 0 && access("control", F_OK) != 0 &&
            access("data", F_OK) != 0);
    }
  }
  write_file("state", "", 0644);
  CHECK(daemon_exit(args) != 0);
  CHECK(unlink("state") == 0 && symlink("nonexistent", "state") == 0);
  CHECK(daemon_exit(args) != 0);
  CHECK(unlink("state") == 0);
  for (i = 0; i < 2; ++i) {
    const char *path;
    path = i ? "state.lock" : "state";
    unlink(path);
    CHECK(mkfifo(path, 0600) == 0);
    CHECK(daemon_exit(args) != 0 && lstat(path, &st) == 0 &&
          S_ISFIFO(st.st_mode) && access("control", F_OK) != 0);
    CHECK(unlink(path) == 0);
  }
  sprintf(uid, "%lu", (unsigned long)getuid() + 1UL);
  extra[0] = "--control-uids";
  extra[1] = uid;
  extra[2] = NULL;
  start_service("control", "state", extra, 0);
  a = peer("control");
  CHECK(a >= 0 && read(a, before, 1) == 0);
  close(a);
  stop_service();
  extra[0] = "--data-uids";
  start_service("control", "state", extra, 1);
  operation("control", "put", "denied", "data", 1);
  open_key("control", "denied", token);
  a = peer("data");
  CHECK(a >= 0 && read(a, before, 1) == 0);
  close(a);
  close_key(token);
  operation("control", "delete", "denied", NULL, 1);
  stop_service();
  CHECK(setenv("REMOTEDB_CONTROL", "env-control", 1) == 0 &&
        setenv("REMOTEDB_STATE", "env-state", 1) == 0 &&
        setenv("REMOTEDB_SOCKET_KIND", "nonsense", 1) == 0 &&
        setenv("REMOTEDB_CONTROL_UIDS", "bad", 1) == 0 &&
        setenv("REMOTEDB_DATA_UIDS", "bad", 1) == 0);
  sprintf(uid, "%lu", (unsigned long)getuid());
  extra[0] = "--socket-kind";
  extra[1] = "pathname";
  extra[2] = "--control-uids";
  extra[3] = uid;
  extra[4] = "--data-uids";
  extra[5] = uid;
  extra[6] = NULL;
  start_service("control", "state", extra, 1);
  stop_service();
  CHECK(access("env-control", F_OK) != 0 && access("env-state", F_OK) != 0);
  unsetenv("REMOTEDB_CONTROL");
  unsetenv("REMOTEDB_STATE");
  unsetenv("REMOTEDB_SOCKET_KIND");
  unsetenv("REMOTEDB_CONTROL_UIDS");
  unsetenv("REMOTEDB_DATA_UIDS");
  args[0] = daemon_path;
  args[1] = "--unknown";
  args[2] = NULL;
  CHECK(daemon_exit(args) == 2);
  args[1] = "--control-uids";
  args[2] = "-1";
  args[3] = NULL;
  CHECK(daemon_exit(args) == 2);
  args[1] = "--data-uids";
  args[2] = "1,";
  CHECK(daemon_exit(args) == 2);
  args[1] = "--socket-kind";
  args[2] = "invalid";
  CHECK(daemon_exit(args) == 2);
}
int main(int argc, char **argv) {
  char control_name[64], data_name[64], key[65];
  const char *args[6];
  int fd;
  CHECK(argc == 5);
  daemon_path = argv[1];
  CHECK(chdir(argv[4]) == 0);
  signal(SIGPIPE, SIG_IGN);
  control(argv[2], argv[3]);
#ifdef __linux__
  endpoint_kind = 1;
  sprintf(control_name, "rdb-control-%ld", (long)getpid());
  sprintf(data_name, "rdb-data-%ld", (long)getpid());
  start_service(control_name, "abstract-state", NULL, 1);
  operation(control_name, "put", "db", data_name, 1);
  open_key(control_name, "db", key);
  fd = attach(data_name, key);
  wire(fd, 3, NULL, 0);
  close(fd);
  empty_sessions(control_name);
  args[0] = argv[2];
  args[1] = "--abstract";
  args[2] = control_name;
  args[3] = "list";
  args[4] = NULL;
  run_client(args, "");
  if (argv[3][0]) {
    open_key(control_name, "db", key);
    args[0] = argv[3];
    args[2] = data_name;
    args[3] = NULL;
    run_client(args, key);
    empty_sessions(control_name);
  }
  operation(control_name, "delete", "db", NULL, 1);
  stop_service();
  CHECK(peer(control_name) < 0);
#else
  (void)control_name;
  (void)data_name;
  (void)key;
  (void)fd;
  (void)args;
#endif
  puts("Managed configuration, persistence, keys, capacities, CLI, UID gates "
       "and endpoints passed");
  return 0;
}
