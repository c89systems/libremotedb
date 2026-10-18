#include <remotedb/remotedb.h>
#include <remotedb/service.h>
#include <remotedb/version.h>
int main(void) {
  rdb_close(0);
  return rdb_server_serve_fd(-1, 0, 0) != RDB_ARGUMENT_ERROR ||
         rdb_control_query(0, 0, 0, 0) != RDB_ARGUMENT_ERROR ||
         rdb_service_run(0, 0, 0, 0, 0, 0) != RDB_ARGUMENT_ERROR ||
         REMOTEDB_VERSION[0] == 0;
}
