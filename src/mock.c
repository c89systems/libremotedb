#include "mock.h"
#include <string.h>

unsigned long rdb_mock_hash(unsigned long h, const void *bytes, size_t n) {
  const unsigned char *p;
  size_t i;
  p = (const unsigned char *)bytes;
  for (i = 0; i < n; ++i)
    h = (h * 33UL + p[i]) & 0xffffffffUL;
  return h;
}
static int connect_mock(void *context) {
  rdb_mock *m;
  m = (rdb_mock *)context;
  ++m->connects;
  return RDB_OK;
}
static void disconnect_mock(void *context) {
  rdb_mock *m;
  m = (rdb_mock *)context;
  ++m->disconnects;
}
static int sql_begin(void *context) {
  rdb_mock *m;
  m = (rdb_mock *)context;
  m->sql_hash = 0;
  memset(&m->sql_bytes, 0, sizeof(m->sql_bytes));
  return RDB_OK;
}
static int sql_write(void *context, const void *p, size_t n) {
  rdb_mock *m;
  m = (rdb_mock *)context;
  m->sql_hash = rdb_mock_hash(m->sql_hash, p, n);
  if (n > m->largest_chunk)
    m->largest_chunk = n;
  return rdb_u64_add(&m->sql_bytes, (unsigned long)n) == RDB_OK ? RDB_OK
                                                                : RDB_ERROR;
}
static int sql_end(void *context) {
  rdb_mock *m;
  m = (rdb_mock *)context;
  return m->sql_bytes.low != 0 || m->sql_bytes.high != 0 ? RDB_OK : RDB_ERROR;
}
static int execute(void *context, rdb_reply *r) {
  rdb_mock *m;
  m = (rdb_mock *)context;
  m->current_parameter = 1;
  m->input_hash = 0;
  memset(&m->input_bytes, 0, sizeof(m->input_bytes));
  memset(&m->output_position, 0, sizeof(m->output_position));
  r->value = m->output_length;
  if (m->input_parameters != 0) {
    r->parameter = 1;
    return RDB_NEED_INPUT;
  }
  return m->execution_status;
}
static int param_write(void *context, const void *p, size_t n) {
  rdb_mock *m;
  m = (rdb_mock *)context;
  m->input_hash = rdb_mock_hash(m->input_hash, p, n);
  if (n > m->largest_chunk)
    m->largest_chunk = n;
  return rdb_u64_add(&m->input_bytes, (unsigned long)n) == RDB_OK ? RDB_OK
                                                                  : RDB_ERROR;
}
static int param_end(void *context, rdb_reply *r) {
  rdb_mock *m;
  m = (rdb_mock *)context;
  if (m->current_parameter < m->input_parameters) {
    r->parameter = ++m->current_parameter;
    return RDB_NEED_INPUT;
  }
  r->value = m->output_length;
  return m->execution_status;
}
static int read_mock(void *context, void *bytes, size_t cap, size_t *read) {
  rdb_mock *m;
  unsigned char *p;
  size_t i;
  m = (rdb_mock *)context;
  p = (unsigned char *)bytes;
  *read = 0;
  if (rdb_u64_compare(m->output_position, m->output_length) >= 0)
    return RDB_NO_DATA;
  for (i = 0;
       i < cap && rdb_u64_compare(m->output_position, m->output_length) < 0;
       ++i) {
    p[i] = (unsigned char)(m->output_position.low & 255UL);
    if (rdb_u64_add(&m->output_position, 1) != RDB_OK)
      return RDB_ERROR;
  }
  *read = i;
  if (i > m->largest_chunk)
    m->largest_chunk = i;
  return RDB_OK;
}
static void reset(void *context) {
  rdb_mock *m;
  m = (rdb_mock *)context;
  memset(&m->sql_bytes, 0, sizeof(m->sql_bytes));
}
void rdb_mock_init(rdb_mock *m) {
  memset(m, 0, sizeof(*m));
  m->backend.context = m;
  m->backend.connect = connect_mock;
  m->backend.disconnect = disconnect_mock;
  m->backend.sql_begin = sql_begin;
  m->backend.sql_write = sql_write;
  m->backend.sql_end = sql_end;
  m->backend.execute = execute;
  m->backend.param_write = param_write;
  m->backend.param_end = param_end;
  m->backend.read = read_mock;
  m->backend.reset = reset;
}
