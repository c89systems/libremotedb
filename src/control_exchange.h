#ifndef RDB_CONTROL_EXCHANGE_H
#define RDB_CONTROL_EXCHANGE_H
#include "control_internal.h"
#include "internal.h"
/* Transport state owns one parser, one mapped record and one output chunk.
 * No serialized request, reply, or list is retained. fd is owned after open. */
typedef struct rdb_control_exchange {
  int fd;
  lonejson *json;
  lonejson_stream *stream;
  rdb_json_input input;
  rdb_document request, record;
  int object_seen;
  lonejson_generator generator;
  int generating, output_eof, newline;
  size_t used, offset;
  unsigned char buffer[RDB_CHUNK_SIZE];
} rdb_control_exchange;
int rdb_exchange_open(rdb_control_exchange *exchange, int fd);
void rdb_exchange_close(rdb_control_exchange *exchange);
/* NO_DATA yields to poll; OK means exactly one object followed by EOF. */
int rdb_exchange_receive(rdb_control_exchange *exchange);
int rdb_exchange_reply(rdb_control_exchange *exchange,
                       const rdb_document *record);
/* NO_DATA yields to poll; OK means this JSONL record has fully drained. */
int rdb_exchange_send(rdb_control_exchange *exchange);
#endif
