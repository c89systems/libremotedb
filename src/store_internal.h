#ifndef RDB_STORE_INTERNAL_H
#define RDB_STORE_INTERNAL_H
#include "control_internal.h"
typedef struct rdb_store {
  rdb_definition definitions[RDB_DEFINITION_LIMIT];
  size_t count;
  const char *path;
  int lock;
  lonejson *json;
} rdb_store;
int rdb_store_open(rdb_store *store, const char *path);
int rdb_store_save(rdb_store *store);
int rdb_store_endpoint_valid(const rdb_store *store, int kind,
                             const char *address);
void rdb_store_close(rdb_store *store);
int rdb_definition_valid(const rdb_definition *definition);
#endif
