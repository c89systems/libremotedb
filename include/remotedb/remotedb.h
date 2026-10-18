#ifndef REMOTEDB_H
#define REMOTEDB_H
#include <remotedb/protocol.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Opaque session, synchronous and confined to one caller at a time. */
typedef struct rdb_connection rdb_connection;

/* Zero selects the default. The negotiated frame limit bounds each chunk,
 * never the total streamed value size. Minimum 32, maximum 65536 bytes. */
typedef struct rdb_config {
  unsigned long frame_limit;
} rdb_config;

/* Fixed control response. value is operation-specific (HELLO frame limit,
 * EXECUTE result length); parameter is the requested 1-based input ordinal.
 * Diagnostic text and full result/parameter typing are later v0 slices. */
typedef struct rdb_reply {
  int status;
  unsigned long parameter;
  rdb_u64 value;
} rdb_reply;

/* Adopt a blocking Unix stream fd after successful HELLO. Caller owns fd on
 * failure. On success close destroys the session and closes the fd. */
int rdb_open_fd(int fd, const rdb_config *config, rdb_connection **out);
/* Connect a local Unix socket and negotiate HELLO; clears out on failure. */
int rdb_open(const char *path, const rdb_config *config, rdb_connection **out);
/* NULL-safe destruction. Does not retry uncertain operations. */
void rdb_close(rdb_connection *connection);
/* Read-only borrowed response, valid until the next operation or close. */
const rdb_reply *rdb_connection_reply(const rdb_connection *connection);
/* Open the host-selected backend session. No remote driver path loading. */
int rdb_connection_connect(rdb_connection *connection);
/* Release backend state and end the connection. */
int rdb_connection_disconnect(rdb_connection *connection);
/* Begin one statement's SQL stream. Only one live statement in this slice. */
int rdb_connection_sql_begin(rdb_connection *connection);
/* Synchronously send caller-owned SQL bytes in bounded chunks, unchanged.
 * Buffer is borrowed only for this call; no full SQL text is retained. */
int rdb_connection_sql_write(rdb_connection *connection, const void *bytes,
                             size_t length);
/* Finish SQL text; the backend may prepare here. */
int rdb_connection_sql_end(rdb_connection *connection);
/* Begin execution; NEED_INPUT names the parameter requested by the backend.
 * Transport loss during execution returns OUTCOME_UNKNOWN. */
int rdb_connection_execute(rdb_connection *connection);
/* Supply bytes only for the currently requested parameter, in bounded chunks.
 * No whole-value buffering or spooling. Backend errors remain remote errors. */
int rdb_connection_param_write(rdb_connection *connection, const void *bytes,
                               size_t length);
/* Finish that parameter; returns NEED_INPUT for another or final status.
 * Transport loss can leave execution's database outcome unknown. */
int rdb_connection_param_end(rdb_connection *connection);
/* Pull output into caller-owned buffer. NO_DATA sets read=0 and eof=1.
 * Bootstrap output is an opaque BLOB stream, not a typed row interface. */
int rdb_connection_column_read(rdb_connection *connection, void *bytes,
                               size_t capacity, size_t *read, int *eof);
/* Discard statement and pending execution/streams. */
int rdb_connection_reset(rdb_connection *connection);
#ifdef __cplusplus
}
#endif
#endif
