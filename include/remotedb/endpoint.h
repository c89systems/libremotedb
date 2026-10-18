#ifndef REMOTEDB_ENDPOINT_H
#define REMOTEDB_ENDPOINT_H
#include <remotedb/remotedb.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Explicit address kind. Zero means a pathname; abstract is Linux-only.
 * Strings contain no embedded NUL. Address length is platform-validated. */
enum rdb_endpoint_kind { RDB_PATHNAME = 0, RDB_ABSTRACT = 1 };
typedef struct rdb_endpoint {
  int kind;
  const char *address;
} rdb_endpoint;
/* Connect/HELLO with a borrowed endpoint; clears out on failure. */
int rdb_open_endpoint(const rdb_endpoint *endpoint, const rdb_config *config,
                      rdb_connection **out);
/* Attach a managed session with exactly 64 lowercase hexadecimal key bytes.
 * The key and peer UID must match; accepted transport owns the session until
 * disconnect. Do not log keys or pass them in command-line arguments. */
int rdb_open_key(const rdb_endpoint *endpoint, const char *key,
                 const rdb_config *config, rdb_connection **out);
#ifdef __cplusplus
}
#endif
#endif
