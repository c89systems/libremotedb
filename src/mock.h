#ifndef RDB_MOCK_H
#define RDB_MOCK_H
#include <remotedb/server.h>
/* Private deterministic test backend, never installed as an ODBC facade.
 * Incremental fingerprints are test observations, not cryptographic hashes. */
typedef struct rdb_mock {
  rdb_backend backend;
  unsigned long sql_hash;
  unsigned long input_hash;
  rdb_u64 sql_bytes;
  rdb_u64 input_bytes;
  rdb_u64 output_length;
  rdb_u64 output_position;
  unsigned long input_parameters;
  unsigned long current_parameter;
  size_t largest_chunk;
  int execution_status;
  int connects;
  int disconnects;
} rdb_mock;
void rdb_mock_init(rdb_mock *mock);
unsigned long rdb_mock_hash(unsigned long hash, const void *bytes, size_t n);
#endif
