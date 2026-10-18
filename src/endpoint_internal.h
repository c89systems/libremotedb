#ifndef RDB_ENDPOINT_INTERNAL_H
#define RDB_ENDPOINT_INTERNAL_H
#include <remotedb/endpoint.h>
#include <sys/socket.h>
#include <sys/un.h>
int rdb_endpoint_connect(const rdb_endpoint *endpoint, int *fd);
int rdb_endpoint_listen(const rdb_endpoint *endpoint, int *fd);
int rdb_endpoint_valid(const rdb_endpoint *endpoint);
int rdb_endpoint_peer_uid(int fd, unsigned long *uid);
#endif
