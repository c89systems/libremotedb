#include "internal.h"
#include <limits.h>
#include <string.h>

void rdb_put32(unsigned char *p, unsigned long v) {
  unsigned int i;
  for (i = 0; i < 4; ++i) {
    p[i] = (unsigned char)(v & 255UL);
    v >>= 8;
  }
}
unsigned long rdb_get32(const unsigned char *p) {
  unsigned long v;
  unsigned int i;
  v = 0;
  for (i = 0; i < 4; ++i)
    v |= (unsigned long)p[i] << (8 * i);
  return v;
}
int rdb_header_encode(const rdb_header *h, unsigned char p[RDB_HEADER_SIZE]) {
  if (h == NULL || p == NULL || CHAR_BIT != 8 || ULONG_MAX < 0xffffffffUL ||
      h->version > 65535U || h->opcode > 65535U || h->flags > 0xffffffffUL ||
      h->statement > 0xffffffffUL || h->length.low > 0xffffffffUL ||
      h->length.high > 0xffffffffUL)
    return RDB_ARGUMENT_ERROR;
  memcpy(p, "RDB0", 4);
  p[4] = (unsigned char)h->version;
  p[5] = (unsigned char)(h->version >> 8);
  p[6] = (unsigned char)h->opcode;
  p[7] = (unsigned char)(h->opcode >> 8);
  rdb_put32(p + 8, h->flags);
  rdb_put32(p + 12, h->statement);
  rdb_put32(p + 16, h->length.low);
  rdb_put32(p + 20, h->length.high);
  return RDB_OK;
}
int rdb_header_decode(const unsigned char p[RDB_HEADER_SIZE],
                      unsigned long limit, rdb_header *h) {
  if (p == NULL || h == NULL)
    return RDB_ARGUMENT_ERROR;
  memset(h, 0, sizeof(*h));
  if (CHAR_BIT != 8 || memcmp(p, "RDB0", 4) != 0 || p[4] != 0 || p[5] != 0)
    return RDB_PROTOCOL_ERROR;
  h->opcode = (unsigned int)p[6] | ((unsigned int)p[7] << 8);
  h->flags = rdb_get32(p + 8);
  h->statement = rdb_get32(p + 12);
  h->length.low = rdb_get32(p + 16);
  h->length.high = rdb_get32(p + 20);
  if (h->length.high != 0 || h->length.low > limit)
    return RDB_PROTOCOL_ERROR;
  return RDB_OK;
}
int rdb_u64_add(rdb_u64 *v, unsigned long n) {
  unsigned long low, carry;
  if (v == NULL || n > 0xffffffffUL || v->low > 0xffffffffUL ||
      v->high > 0xffffffffUL)
    return RDB_ARGUMENT_ERROR;
  carry = n > 0xffffffffUL - v->low;
  if (carry && v->high == 0xffffffffUL)
    return RDB_ARGUMENT_ERROR;
  low = (v->low + n) & 0xffffffffUL;
  v->low = low;
  v->high += carry;
  return RDB_OK;
}
int rdb_u64_compare(rdb_u64 a, rdb_u64 b) {
  if (a.high != b.high)
    return a.high < b.high ? -1 : 1;
  if (a.low != b.low)
    return a.low < b.low ? -1 : 1;
  return 0;
}
void rdb_reply_encode(const rdb_reply *r, unsigned char p[16]) {
  rdb_put32(p, (unsigned long)r->status);
  rdb_put32(p + 4, r->parameter);
  rdb_put32(p + 8, r->value.low);
  rdb_put32(p + 12, r->value.high);
}
int rdb_reply_decode(const unsigned char p[16], rdb_reply *r) {
  unsigned long status;
  status = rdb_get32(p);
  if (status > RDB_ERROR)
    return RDB_PROTOCOL_ERROR;
  r->status = (int)status;
  r->parameter = rdb_get32(p + 4);
  r->value.low = rdb_get32(p + 8);
  r->value.high = rdb_get32(p + 12);
  return RDB_OK;
}
