#define _POSIX_C_SOURCE 200809L
#include "control_exchange.h"
#include <string.h>
#include <unistd.h>

void rdb_exchange_close(rdb_control_exchange *e) {
  if (e->stream != NULL)
    lonejson_stream_close(e->stream);
  if (e->generating)
    lonejson_generator_cleanup(&e->generator);
  if (e->json != NULL)
    lonejson_free(e->json);
  if (e->fd >= 0)
    close(e->fd);
  memset(e, 0, sizeof(*e));
  e->fd = -1;
}
int rdb_exchange_open(rdb_control_exchange *e, int fd) {
  memset(e, 0, sizeof(*e));
  e->fd = fd;
  e->json = rdb_json_new();
  e->input.fd = fd;
  e->input.remaining = 16384;
  e->input.bounded = 1;
  e->input.resumable = 1;
  if (e->json != NULL)
    e->stream = rdb_json_stream(e->json, &e->input);
  if (e->stream == NULL) {
    rdb_exchange_close(e);
    return RDB_MEMORY_ERROR;
  }
  return RDB_OK;
}
int rdb_exchange_receive(rdb_control_exchange *e) {
  lonejson_stream_result result;
  lonejson_error error;
  lonejson_error_init(&error);
  result = lonejson_stream_next(
      e->stream, e->object_seen ? &e->record : &e->request, &error);
  if (result == LONEJSON_STREAM_WOULD_BLOCK)
    return RDB_NO_DATA;
  if (result == LONEJSON_STREAM_OBJECT && !e->object_seen) {
    e->object_seen = 1;
    result = lonejson_stream_next(e->stream, &e->record, &error);
    if (result == LONEJSON_STREAM_WOULD_BLOCK)
      return RDB_NO_DATA;
  }
  lonejson_stream_close(e->stream);
  e->stream = NULL;
  return result == LONEJSON_STREAM_EOF && e->object_seen ? RDB_OK : RDB_ERROR;
}
int rdb_exchange_reply(rdb_control_exchange *e, const rdb_document *record) {
  e->record = *record;
  if (lonejson_generator_init(e->json, &e->generator, rdb_json_document_map(),
                              &e->record) != LONEJSON_STATUS_OK)
    return RDB_MEMORY_ERROR;
  e->generating = 1;
  e->output_eof = e->newline = 0;
  e->used = e->offset = 0;
  return RDB_OK;
}
int rdb_exchange_send(rdb_control_exchange *e) {
  size_t written;
  int status;
  if (e->offset == e->used) {
    e->used = e->offset = 0;
    if (!e->output_eof) {
      if (lonejson_generator_read(&e->generator, e->buffer, sizeof(e->buffer),
                                  &e->used,
                                  &e->output_eof) != LONEJSON_STATUS_OK)
        return RDB_ERROR;
    } else if (!e->newline) {
      e->buffer[0] = '\n';
      e->used = 1;
      e->newline = 1;
    }
  }
  if (e->offset < e->used) {
    status = rdb_io_try_write(e->fd, e->buffer + e->offset, e->used - e->offset,
                              &written);
    if (status != RDB_OK)
      return status;
    e->offset += written;
  }
  if (e->newline && e->offset == e->used) {
    lonejson_generator_cleanup(&e->generator);
    e->generating = 0;
    return RDB_OK;
  }
  return RDB_NO_DATA;
}
