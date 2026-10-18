#ifndef REMOTEDB_CONTROL_H
#define REMOTEDB_CONTROL_H
#include <remotedb/endpoint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define RDB_NAME_CAPACITY 64
#define RDB_ADDRESS_CAPACITY 108
#define RDB_DSN_CAPACITY 256
#define RDB_ATTRIBUTES_CAPACITY 1024
#define RDB_DEFINITION_LIMIT 64
#define RDB_SESSION_LIMIT 128
/* Reusable server-side definition. Driver is a logical approved name;
 * attributes can contain secrets and are never returned by control queries.
 * Endpoint is a listener accepting independent sessions. */
typedef struct rdb_definition {
  char name[RDB_NAME_CAPACITY];
  char driver[RDB_NAME_CAPACITY];
  char dsn[RDB_DSN_CAPACITY];
  char attributes[RDB_ATTRIBUTES_CAPACITY];
  int kind;
  char address[RDB_ADDRESS_CAPACITY];
} rdb_definition;
/* Zero-initialize before filling. Operations: put, get, list, delete,
 * sessions, open, close. name selects a definition; connection_key selects a
 * live session. open returns a random 256-bit connection key bound to the
 * creator UID and selected definition; optional dsn/attributes are per-session
 * overrides. Keys are returned once by open to their owner, expire after 60
 * seconds unattached, and are invalidated on disconnect. No session state
 * survives restart. put replaces a complete definition; successful changes are
 * durable. SQL is never accepted by the control socket. */
typedef struct rdb_control_request {
  char operation[16];
  rdb_definition definition;
  char connection_key[65];
} rdb_control_request;
/* Borrowed callback record, valid only during callback. definition omits
 * attributes; connection_key is populated only by open. status is a remote
 * status. Terminal records have type="done". No total response is retained. */
typedef struct rdb_control_record {
  char type[16];
  rdb_definition definition;
  char connection_key[65];
  char status[32];
  char message[160];
} rdb_control_record;
typedef int (*rdb_control_record_fn)(void *user,
                                     const rdb_control_record *record);
/* Synchronous streamed JSONL exchange via lonejson. callback must consume each
 * record before returning OK; cancellation closes transport immediately.
 * Listing records are provisional until done/ok. Concurrent registry changes
 * can end a list with done/error; discard that partial view before retrying.
 * No retries; loss after open/put/delete/close may return OUTCOME_UNKNOWN. */
int rdb_control_query(const rdb_endpoint *endpoint,
                      const rdb_control_request *request,
                      rdb_control_record_fn callback, void *user);
#ifdef __cplusplus
}
#endif
#endif
