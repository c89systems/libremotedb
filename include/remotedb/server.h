#ifndef REMOTEDB_SERVER_H
#define REMOTEDB_SERVER_H
#include <remotedb/remotedb.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Session-scoped backend boundary. No ODBC types cross this interface.
 * Host owns context and table for the entire serve call. Callbacks return a
 * remote status (OK, NEED_INPUT, NO_DATA, UNSUPPORTED, ERROR), never local
 * transport errors. Callback buffers are borrowed for that call only.
 * SQL and parameter callbacks must consume each chunk before returning.
 * read must produce at most capacity bytes and may return OK only with data.
 * reset must cancel pending execution and release statement resources.
 * disconnect is called exactly once after a successful backend connect,
 * including on EOF or protocol failure. */
typedef struct rdb_backend {
  void *context;
  int (*connect)(void *context);
  void (*disconnect)(void *context);
  int (*sql_begin)(void *context);
  int (*sql_write)(void *context, const void *bytes, size_t length);
  int (*sql_end)(void *context);
  int (*execute)(void *context, rdb_reply *reply);
  int (*param_write)(void *context, const void *bytes, size_t length);
  int (*param_end)(void *context, rdb_reply *reply);
  int (*read)(void *context, void *bytes, size_t capacity, size_t *read);
  void (*reset)(void *context);
} rdb_backend;

/* Serve one blocking fd sequentially without owning/closing fd. Stack memory
 * is bounded by two RDB_CHUNK_SIZE buffers regardless of input/result length.
 * Returns OK on clean disconnect/EOF, local errors otherwise. */
int rdb_server_serve_fd(int fd, const rdb_config *config,
                        const rdb_backend *backend);
#ifdef __cplusplus
}
#endif
#endif
