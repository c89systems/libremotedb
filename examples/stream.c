#include <remotedb/endpoint.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
  rdb_connection *connection;
  const char *sql;
  int status, index;
  rdb_endpoint endpoint;
  char key[65];
  index = 1;
  endpoint.kind = RDB_PATHNAME;
  if (argc > 1 && strcmp(argv[1], "--abstract") == 0) {
    endpoint.kind = RDB_ABSTRACT;
    ++index;
  }
  if (argc != index + 1) {
    fputs("Usage: stream-example [--abstract] SOCKET < CONNECTION_KEY_FILE\n",
          stderr);
    return 2;
  }
  connection = NULL;
  sql = "SELECT payload FROM documents";
  if (fgets(key, sizeof(key), stdin) == NULL || strlen(key) != 64)
    return 2;
  endpoint.address = argv[index];
  status = rdb_open_key(&endpoint, key, NULL, &connection);
  if (status == RDB_OK)
    status = rdb_connection_connect(connection);
  if (status == RDB_OK)
    status = rdb_connection_sql_begin(connection);
  if (status == RDB_OK)
    status = rdb_connection_sql_write(connection, sql, strlen(sql));
  if (status == RDB_OK)
    status = rdb_connection_sql_end(connection);
  if (status == RDB_OK)
    status = rdb_connection_execute(connection);
  if (status == RDB_OK)
    status = rdb_connection_disconnect(connection);
  rdb_close(connection);
  return status == RDB_OK ? 0 : 1;
}
