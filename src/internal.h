#ifndef RDB_INTERNAL_H
#define RDB_INTERNAL_H
#include <remotedb/server.h>
void rdb_put32(unsigned char *bytes, unsigned long value);
unsigned long rdb_get32(const unsigned char *bytes);
int rdb_io_read(int fd, void *bytes, size_t length);
int rdb_io_write(int fd, const void *bytes, size_t length);
int rdb_io_try_write(int fd, const void *bytes, size_t length, size_t *written);
int rdb_io_header(int fd, unsigned long limit, rdb_header *header);
int rdb_io_send_header(int fd, unsigned int opcode, unsigned long flags,
                       unsigned long statement, unsigned long length);
int rdb_limit(const rdb_config *config, unsigned long *limit);
void rdb_reply_encode(const rdb_reply *reply, unsigned char bytes[16]);
int rdb_reply_decode(const unsigned char bytes[16], rdb_reply *reply);
#endif
