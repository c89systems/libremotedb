#ifndef REMOTEDB_PROTOCOL_H
#define REMOTEDB_PROTOCOL_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Exact 64-bit wire value without C99 types or C89 compiler extensions.
 * Each limb contains at most 32 bits, including on LP64 systems. */
typedef struct rdb_u64 {
  unsigned long low;
  unsigned long high;
} rdb_u64;

/* Local failures are negative; nonnegative values are remote statuses.
 * NEED_INPUT is an execution continuation, not an operation failure. */
enum rdb_status {
  RDB_OK = 0,
  RDB_NEED_INPUT = 1,
  RDB_NO_DATA = 2,
  RDB_UNSUPPORTED = 3,
  RDB_ERROR = 4,
  RDB_PROTOCOL_ERROR = -1,
  RDB_TRANSPORT_ERROR = -2,
  RDB_ARGUMENT_ERROR = -3,
  RDB_MEMORY_ERROR = -4,
  RDB_STATE_ERROR = -5,
  RDB_OUTCOME_UNKNOWN = -6
};

/* Bootstrap v0 operation assignments. Unknown operations return UNSUPPORTED. */
enum rdb_opcode {
  RDB_HELLO = 1,
  RDB_CONNECT = 2,
  RDB_DISCONNECT = 3,
  RDB_SQL_BEGIN = 16,
  RDB_SQL_DATA = 17,
  RDB_SQL_END = 18,
  RDB_EXECUTE = 19,
  RDB_PARAM_DATA = 32,
  RDB_PARAM_END = 33,
  RDB_COLUMN_READ = 48,
  RDB_RESET = 49
};

#define RDB_HEADER_SIZE 24
#define RDB_RESPONSE_FLAG 1UL
#define RDB_DEFAULT_FRAME_LIMIT 65536UL
#define RDB_CHUNK_SIZE 4096

/* Logical fields only; never write this structure to a socket. */
typedef struct rdb_header {
  unsigned int version;
  unsigned int opcode;
  unsigned long flags;
  unsigned long statement;
  rdb_u64 length;
} rdb_header;

/* Encode/decode exactly 24 bytes; reject out-of-range fields, bad magic,
 * nonzero versions and payloads exceeding limit. No payload is allocated. */
int rdb_header_encode(const rdb_header *header,
                      unsigned char bytes[RDB_HEADER_SIZE]);
int rdb_header_decode(const unsigned char bytes[RDB_HEADER_SIZE],
                      unsigned long limit, rdb_header *header);

/* Add a 32-bit count with overflow detection; value is unchanged on failure. */
int rdb_u64_add(rdb_u64 *value, unsigned long count);
/* Lexicographic unsigned comparison; returns -1, 0, or 1. */
int rdb_u64_compare(rdb_u64 a, rdb_u64 b);
#ifdef __cplusplus
}
#endif
#endif
