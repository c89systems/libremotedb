#define _POSIX_C_SOURCE 200809L
#include "control_internal.h"
#include "endpoint_internal.h"
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

int rdb_control_query(const rdb_endpoint *endpoint,
                      const rdb_control_request *request,
                      rdb_control_record_fn callback, void *user) {
  lonejson *json;
  lonejson_stream *stream;
  rdb_json_input input;
  lonejson_error error;
  rdb_document doc;
  rdb_control_record record;
  struct timeval timeout;
  int fd, s, mutation;
  if (request == NULL || callback == NULL ||
      memchr(request->operation, 0, sizeof(request->operation)) == NULL ||
      memchr(request->connection_key, 0, sizeof(request->connection_key)) ==
          NULL ||
      memchr(request->definition.name, 0, sizeof(request->definition.name)) ==
          NULL ||
      memchr(request->definition.driver, 0,
             sizeof(request->definition.driver)) == NULL ||
      memchr(request->definition.dsn, 0, sizeof(request->definition.dsn)) ==
          NULL ||
      memchr(request->definition.attributes, 0,
             sizeof(request->definition.attributes)) == NULL ||
      memchr(request->definition.address, 0,
             sizeof(request->definition.address)) == NULL)
    return RDB_ARGUMENT_ERROR;
  s = rdb_endpoint_connect(endpoint, &fd);
  if (s != RDB_OK)
    return s;
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  mutation = strcmp(request->operation, "put") == 0 ||
             strcmp(request->operation, "open") == 0 ||
             strcmp(request->operation, "delete") == 0 ||
             strcmp(request->operation, "close") == 0;
  json = rdb_json_new();
  if (json == NULL) {
    close(fd);
    return RDB_MEMORY_ERROR;
  }
  rdb_document_set(&doc, &request->definition, 1);
  strcpy(doc.operation, request->operation);
  strcpy(doc.connection_key, request->connection_key);
  s = rdb_json_send(json, fd, &doc);
  stream = NULL;
  lonejson_error_init(&error);
  if (s == RDB_OK) {
    /* One request per connection; finish writing so malformed/truncated
     * requests cannot wait indefinitely for another object. */
    shutdown(fd, SHUT_WR);
    input.fd = fd;
    input.remaining = 0;
    input.bounded = 0;
    input.resumable = 0;
    stream = rdb_json_stream(json, &input);
    if (stream == NULL)
      s = RDB_MEMORY_ERROR;
  }
  while (s == RDB_OK) {
    if (lonejson_stream_next(stream, &doc, &error) != LONEJSON_STREAM_OBJECT) {
      s = mutation ? RDB_OUTCOME_UNKNOWN : RDB_PROTOCOL_ERROR;
      break;
    }
    memset(&record, 0, sizeof(record));
    strcpy(record.type, doc.type);
    strcpy(record.status, doc.status);
    strcpy(record.message, doc.message);
    strcpy(record.connection_key, doc.connection_key);
    if (strcmp(doc.type, "definition") == 0 ||
        strcmp(doc.type, "session") == 0) {
      s = rdb_document_definition(&doc, &record.definition);
      if (s != RDB_OK)
        break;
    } else if (strcmp(doc.type, "done") != 0) {
      s = RDB_PROTOCOL_ERROR;
      break;
    }
    if (strcmp(doc.type, "done") == 0) {
      s = strcmp(doc.status, "ok") == 0        ? RDB_OK
          : strcmp(doc.status, "unknown") == 0 ? RDB_OUTCOME_UNKNOWN
                                               : RDB_ERROR;
      if (callback(user, &record) != RDB_OK && s == RDB_OK)
        s = RDB_STATE_ERROR;
      break;
    }
    s = callback(user, &record);
  }
  if (mutation && s == RDB_TRANSPORT_ERROR)
    s = RDB_OUTCOME_UNKNOWN;
  if (stream != NULL)
    lonejson_stream_close(stream);
  lonejson_free(json);
  close(fd);
  return s;
}
