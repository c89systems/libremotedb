#ifndef RDB_CONTROL_INTERNAL_H
#define RDB_CONTROL_INTERNAL_H
#define LONEJSON_DISABLE_SHORT_NAMES 1
#include <lonejson.h>
#include <remotedb/control.h>
/* JSON maps expose only bounded metadata; no SQL or BLOB fields exist here. */
typedef struct rdb_document {
  char operation[16];
  char type[16];
  char name[RDB_NAME_CAPACITY];
  char driver[RDB_NAME_CAPACITY];
  char dsn[RDB_DSN_CAPACITY];
  char attributes[RDB_ATTRIBUTES_CAPACITY];
  char kind[16];
  char address[RDB_ADDRESS_CAPACITY];
  char connection_key[65];
  char status[32];
  char message[160];
} rdb_document;
const lonejson_map *rdb_json_document_map(void);
const lonejson_map *rdb_json_definition_map(void);
lonejson *rdb_json_new(void);
void rdb_document_set(rdb_document *doc, const rdb_definition *definition,
                      int include_secret);
int rdb_document_definition(const rdb_document *doc,
                            rdb_definition *definition);
int rdb_json_send(lonejson *json, int fd, const rdb_document *doc);
typedef struct rdb_json_input {
  int fd;
  size_t remaining; /* zero is unbounded when bounded == 0 */
  int bounded;
  int resumable; /* EAGAIN yields only for nonblocking stream consumers. */
} rdb_json_input;
lonejson_stream *rdb_json_stream(lonejson *json, rdb_json_input *input);
#endif
