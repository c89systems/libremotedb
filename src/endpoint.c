#define _GNU_SOURCE 1
#include "endpoint_internal.h"
#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int address(const rdb_endpoint *e, struct sockaddr_un *a,
                   socklen_t *len) {
  size_t n;
  if (e == NULL || e->address == NULL || e->address[0] == 0)
    return RDB_ARGUMENT_ERROR;
  n = strlen(e->address);
  memset(a, 0, sizeof(*a));
  a->sun_family = AF_UNIX;
  if (e->kind == RDB_ABSTRACT) {
#ifndef __linux__
    return RDB_UNSUPPORTED;
#else
    if (n + 1 > sizeof(a->sun_path))
      return RDB_ARGUMENT_ERROR;
    memcpy(a->sun_path + 1, e->address, n);
    *len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + n + 1);
#endif
  } else if (e->kind == RDB_PATHNAME) {
    if (n >= sizeof(a->sun_path))
      return RDB_ARGUMENT_ERROR;
    memcpy(a->sun_path, e->address, n + 1);
    *len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + n + 1);
  } else
    return RDB_ARGUMENT_ERROR;
#ifdef __APPLE__
  a->sun_len = (unsigned char)*len;
#endif
  return RDB_OK;
}
int rdb_endpoint_connect(const rdb_endpoint *e, int *out) {
  struct sockaddr_un a;
  socklen_t len;
  int fd, s;
  if (out == NULL)
    return RDB_ARGUMENT_ERROR;
  *out = -1;
  s = address(e, &a, &len);
  if (s != RDB_OK)
    return s;
  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return RDB_TRANSPORT_ERROR;
  if (connect(fd, (struct sockaddr *)&a, len) != 0) {
    close(fd);
    return RDB_TRANSPORT_ERROR;
  }
  *out = fd;
  return RDB_OK;
}
int rdb_endpoint_listen(const rdb_endpoint *e, int *out) {
  struct sockaddr_un a;
  socklen_t len;
  int fd, s;
  s = address(e, &a, &len);
  if (s != RDB_OK)
    return s;
  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return RDB_TRANSPORT_ERROR;
  if (bind(fd, (struct sockaddr *)&a, len) != 0) {
    close(fd);
    return RDB_TRANSPORT_ERROR;
  }
  if (listen(fd, 32) != 0) {
    close(fd);
    if (e->kind == RDB_PATHNAME)
      unlink(e->address);
    return RDB_TRANSPORT_ERROR;
  }
  *out = fd;
  return RDB_OK;
}
int rdb_endpoint_valid(const rdb_endpoint *e) {
  struct sockaddr_un a;
  socklen_t len;
  return address(e, &a, &len);
}
int rdb_endpoint_peer_uid(int fd, unsigned long *uid) {
#ifdef __linux__
  struct ucred credentials;
  socklen_t n;
  n = sizeof(credentials);
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &n) != 0 ||
      n != sizeof(credentials))
    return RDB_ERROR;
  *uid = (unsigned long)credentials.uid;
#elif defined(__APPLE__)
  uid_t u;
  gid_t g;
  if (getpeereid(fd, &u, &g) != 0)
    return RDB_ERROR;
  *uid = (unsigned long)u;
#else
  (void)fd;
  (void)uid;
  return RDB_UNSUPPORTED;
#endif
  return RDB_OK;
}
