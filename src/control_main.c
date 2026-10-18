#include <remotedb/control.h>
#include <stdio.h>
#include <string.h>
static int record(void *user, const rdb_control_record *r) {
  (void)user;
  if (strcmp(r->type, "done") == 0) {
    if (strcmp(r->status, "ok") != 0)
      fprintf(stderr, "%s\n", r->message);
  } else if (r->connection_key[0] != 0)
    puts(r->connection_key);
  else
    printf("%s\t%s\t%s\t%s\t%s\t%s\n", r->type, r->definition.name,
           r->definition.driver,
           r->definition.kind == RDB_ABSTRACT ? "abstract" : "pathname",
           r->definition.address, r->connection_key);
  return RDB_OK;
}
static int copy(char *dest, size_t capacity, const char *source) {
  if (strlen(source) >= capacity)
    return 0;
  strcpy(dest, source);
  return 1;
}
int main(int argc, char **argv) {
  rdb_endpoint endpoint;
  rdb_control_request request;
  int i, status;
  memset(&request, 0, sizeof(request));
  endpoint.kind = RDB_PATHNAME;
  i = 1;
  if (argc > 1 && strcmp(argv[1], "--abstract") == 0) {
    endpoint.kind = RDB_ABSTRACT;
    ++i;
  }
  if (argc > 1 && strcmp(argv[1], "--help") == 0) {
    puts("Usage: remotedbctl [--abstract] CONTROL list|sessions [NAME]\n"
         "       remotedbctl [--abstract] CONTROL get|delete|open NAME\n"
         "       remotedbctl [--abstract] CONTROL close < KEY_FILE\n"
         "       remotedbctl [--abstract] CONTROL put NAME DRIVER "
         "pathname|abstract ADDRESS [DSN]\n"
         "Open emits a connection key once; other queries omit keys and "
         "credentials.\n"
         "Secret attributes may be set through the C control API, not command "
         "arguments.");
    return 0;
  }
  if (argc < i + 2)
    goto invalid;
  endpoint.address = argv[i++];
  if (!copy(request.operation, sizeof(request.operation), argv[i++]))
    goto invalid;
  if (strcmp(request.operation, "put") == 0) {
    if (argc != i + 4 && argc != i + 5)
      goto invalid;
    if (!copy(request.definition.name, sizeof(request.definition.name),
              argv[i++]) ||
        !copy(request.definition.driver, sizeof(request.definition.driver),
              argv[i++]))
      goto invalid;
    if (strcmp(argv[i], "abstract") == 0)
      request.definition.kind = RDB_ABSTRACT;
    else if (strcmp(argv[i], "pathname") != 0)
      goto invalid;
    ++i;
    if (!copy(request.definition.address, sizeof(request.definition.address),
              argv[i++]))
      goto invalid;
    if (argc > i &&
        !copy(request.definition.dsn, sizeof(request.definition.dsn), argv[i]))
      goto invalid;
  } else if (strcmp(request.operation, "close") == 0) {
    if (argc != i ||
        fgets(request.connection_key, sizeof(request.connection_key), stdin) ==
            NULL ||
        strlen(request.connection_key) != 64)
      goto invalid;
  } else if (strcmp(request.operation, "get") == 0 ||
             strcmp(request.operation, "delete") == 0 ||
             strcmp(request.operation, "open") == 0) {
    if (argc != i + 1 || !copy(request.definition.name,
                               sizeof(request.definition.name), argv[i]))
      goto invalid;
  } else if (strcmp(request.operation, "list") == 0 ||
             strcmp(request.operation, "sessions") == 0) {
    if (argc > i + 1 ||
        (argc == i + 1 && !copy(request.definition.name,
                                sizeof(request.definition.name), argv[i])))
      goto invalid;
  } else
    goto invalid;
  status = rdb_control_query(&endpoint, &request, record, NULL);
  if (status < 0)
    fprintf(stderr,
            "Control exchange failed (status %d); uncertain mutations are not "
            "retried.\n",
            status);
  return status == RDB_OK ? 0 : 1;
invalid:
  fputs("Invalid control command or field length. Use remotedbctl --help.\n",
        stderr);
  return 2;
}
