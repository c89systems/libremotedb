#include <remotedb/protocol.h>
#include <stdio.h>
int main(void) {
  unsigned char bytes[RDB_HEADER_SIZE];
  rdb_header header;
  size_t n;
  n = fread(bytes, 1, sizeof(bytes), stdin);
  if (n == sizeof(bytes))
    (void)rdb_header_decode(bytes, RDB_DEFAULT_FRAME_LIMIT, &header);
  return 0;
}
