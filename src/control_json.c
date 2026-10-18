#define _POSIX_C_SOURCE 200809L
#include "control_internal.h"
#include "internal.h"
#include <errno.h>
#include <string.h>
#include <unistd.h>
#define FIELD(m)                                                               \
  LONEJSON_FIELD_STRING_FIXED(rdb_document, m, #m, LONEJSON_OVERFLOW_FAIL)
static const lonejson_field document_fields[] = {
    FIELD(operation), FIELD(type),    FIELD(name),
    FIELD(driver),    FIELD(dsn),     FIELD(attributes),
    FIELD(kind),      FIELD(address), FIELD(connection_key),
    FIELD(status),    FIELD(message)};
LONEJSON_MAP_DEFINE(rdb_document_map, rdb_document, document_fields);
#define REQUIRED(m)                                                            \
  LONEJSON_FIELD_STRING_FIXED_REQ(rdb_document, m, #m, LONEJSON_OVERFLOW_FAIL)
static const lonejson_field definition_fields[] = {
    REQUIRED(type),       REQUIRED(name), REQUIRED(driver), REQUIRED(dsn),
    REQUIRED(attributes), REQUIRED(kind), REQUIRED(address)};
LONEJSON_MAP_DEFINE(rdb_definition_map, rdb_document, definition_fields);

const lonejson_map *rdb_json_document_map(void) { return &rdb_document_map; }
const lonejson_map *rdb_json_definition_map(void) {
  return &rdb_definition_map;
}
lonejson *rdb_json_new(void) {
  lonejson_config config;
  lonejson_error error;
  config = lonejson_default_config();
  config.max_alloc_bytes = 65536;
  config.max_dynamic_string_bytes = RDB_ATTRIBUTES_CAPACITY;
  config.max_depth = 4;
  config.reject_duplicate_keys_by_default = 1;
  config.clear_destination_by_default = 1;
  config.write_pretty = 0;
  lonejson_error_init(&error);
  return lonejson_new(&config, &error);
}
void rdb_document_set(rdb_document *doc, const rdb_definition *d, int secret) {
  memset(doc, 0, sizeof(*doc));
  strcpy(doc->type, "definition");
  strcpy(doc->name, d->name);
  strcpy(doc->driver, d->driver);
  strcpy(doc->dsn, d->dsn);
  if (secret)
    strcpy(doc->attributes, d->attributes);
  strcpy(doc->kind, d->kind == RDB_ABSTRACT ? "abstract" : "pathname");
  strcpy(doc->address, d->address);
}
int rdb_document_definition(const rdb_document *doc, rdb_definition *d) {
  memset(d, 0, sizeof(*d));
  if (strcmp(doc->kind, "abstract") == 0)
    d->kind = RDB_ABSTRACT;
  else if (strcmp(doc->kind, "pathname") != 0)
    return RDB_ARGUMENT_ERROR;
  strcpy(d->name, doc->name);
  strcpy(d->driver, doc->driver);
  strcpy(d->dsn, doc->dsn);
  strcpy(d->attributes, doc->attributes);
  strcpy(d->address, doc->address);
  return RDB_OK;
}
static lonejson_status socket_sink(void *user, const void *bytes, size_t n,
                                   lonejson_error *error) {
  int fd;
  (void)error;
  fd = *(int *)user;
  while (n != 0) {
    size_t chunk;
    chunk = n > RDB_CHUNK_SIZE ? RDB_CHUNK_SIZE : n;
    if (rdb_io_write(fd, bytes, chunk) != RDB_OK)
      return LONEJSON_STATUS_IO_ERROR;
    bytes = (const unsigned char *)bytes + chunk;
    n -= chunk;
  }
  return LONEJSON_STATUS_OK;
}
int rdb_json_send(lonejson *json, int fd, const rdb_document *doc) {
  lonejson_error error;
  lonejson_error_init(&error);
  return lonejson_serialize_jsonl_sink(json, rdb_json_document_map(), doc, 1, 0,
                                       socket_sink, &fd,
                                       &error) == LONEJSON_STATUS_OK
             ? RDB_OK
             : RDB_TRANSPORT_ERROR;
}
static lonejson_read_result socket_reader(void *user, unsigned char *bytes,
                                          size_t capacity) {
  lonejson_read_result result;
  ssize_t n;
  rdb_json_input *input;
  input = (rdb_json_input *)user;
  result = lonejson_default_read_result();
  if (capacity > RDB_CHUNK_SIZE)
    capacity = RDB_CHUNK_SIZE;
  if (input->bounded && capacity > input->remaining)
    capacity = input->remaining;
  /* At the exact limit, distinguish EOF from one excess byte. */
  if (input->bounded && capacity == 0)
    capacity = 1;
  do {
    n = read(input->fd, bytes, capacity);
  } while (n < 0 && errno == EINTR);
  if (n < 0 && input->resumable && (errno == EAGAIN || errno == EWOULDBLOCK))
    result.would_block = 1;
  else if (n < 0)
    result.error_code = errno;
  else if (n == 0)
    result.eof = 1;
  else {
    if (input->bounded && input->remaining == 0) {
      result.error_code = EFBIG;
      return result;
    }
    result.bytes_read = (size_t)n;
    if (input->bounded)
      input->remaining -= (size_t)n;
  }
  return result;
}
lonejson_stream *rdb_json_stream(lonejson *json, rdb_json_input *input) {
  lonejson_error error;
  lonejson_error_init(&error);
  return lonejson_stream_open_reader(json, rdb_json_document_map(),
                                     socket_reader, input, &error);
}
