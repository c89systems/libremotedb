#ifndef REMOTEDB_SERVICE_H
#define REMOTEDB_SERVICE_H
#include <remotedb/control.h>
#include <remotedb/server.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Zero selects the daemon UID for each allowlist. UID values are unsigned
 * longs validated against the OS uid_t. Borrowed arrays live until run ends.
 * state_path names durable JSONL definitions; flags/env belong to the host.
 * stop_fd is a readable shutdown pipe, -1 disables it (set explicitly).
 * Limits bound simultaneous sessions/definitions, not SQL or BLOB sizes. */
typedef struct rdb_service_config {
  rdb_endpoint control;
  const char *state_path;
  const unsigned long *control_uids;
  size_t control_uid_count;
  const unsigned long *data_uids;
  size_t data_uid_count;
  int stop_fd;
} rdb_service_config;
/* Called in an isolated child for each accepted, authorized data socket.
 * Populate backend/context; it lives until serve finishes. cleanup runs after
 * serve (even on error). Only approved logical drivers may be instantiated.
 * Host startup must select approved drivers; this slice supports mock only. */
typedef int (*rdb_backend_factory_fn)(void *user,
                                      const rdb_definition *definition,
                                      rdb_backend *backend);
typedef void (*rdb_backend_cleanup_fn)(void *user, rdb_backend *backend);
/* Blocking managed control/data service. Takes a lifetime file lock, loads
 * definitions before binding, never unlinks pre-existing paths, and closes
 * sessions/listeners on stop. Run from a single-threaded host before threads.
 * Parent retains registry; children cannot mutate persisted definitions.
 * Host SIGCHLD auto-reaping/handlers are supported; run does not replace them.
 * Parent socket I/O is nonblocking: 16 control exchanges and 64 key handshakes
 * have separate admission pools and five-second monotonic parent deadlines.
 * A valid key hands off to the worker before backend
 * initialization/acknowledgement. Saturated pools close new peers; stop_fd
 * interrupts incomplete exchanges. Return OUTCOME_UNKNOWN if rename committed
 * but directory fsync failed. Error text is caller-owned; no credentials are
 * written into diagnostics. */
int rdb_service_run(const rdb_service_config *config,
                    rdb_backend_factory_fn factory,
                    rdb_backend_cleanup_fn cleanup, void *user, char *error,
                    size_t error_capacity);
#ifdef __cplusplus
}
#endif
#endif
