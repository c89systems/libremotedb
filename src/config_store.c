#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L
#include "endpoint_internal.h"
#include "store_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Compare the containing directory identity as well as the final name.
 * The state file need not exist yet; directory symlinks and relative aliases
 * must still resolve to the same reserved name before any socket is bound. */
static int path_directory(const char *path, struct stat *directory,
                          const char **name) {
  char *copy, *slash;
  const char *leaf;
  int status;
  copy = strdup(path);
  if (copy == NULL)
    return RDB_MEMORY_ERROR;
  leaf = strrchr(path, '/');
  *name = leaf == NULL ? path : leaf + 1;
  slash = strrchr(copy, '/');
  if (slash != NULL) {
    if (slash == copy)
      slash[1] = 0;
    else
      *slash = 0;
  }
  status =
      stat(slash == NULL ? "." : copy, directory) == 0 ? RDB_OK : RDB_ERROR;
  free(copy);
  return status;
}
int rdb_store_endpoint_valid(const rdb_store *s, int kind,
                             const char *address) {
  struct stat state_directory, endpoint_directory;
  const char *state_name, *endpoint_name;
  int status;
  if (kind != RDB_PATHNAME)
    return RDB_OK;
  status = path_directory(s->path, &state_directory, &state_name);
  if (status == RDB_OK)
    status = path_directory(address, &endpoint_directory, &endpoint_name);
  if (status != RDB_OK)
    return status;
  if (state_directory.st_dev == endpoint_directory.st_dev &&
      state_directory.st_ino == endpoint_directory.st_ino &&
      strcmp(state_name, endpoint_name) == 0)
    return RDB_ARGUMENT_ERROR;
  return RDB_OK;
}

int rdb_definition_valid(const rdb_definition *d) {
  rdb_endpoint endpoint;
  const unsigned char *p;
  if (d->name[0] == 0 || strcmp(d->driver, "mock") != 0)
    return RDB_ARGUMENT_ERROR;
  p = (const unsigned char *)d->name;
  while (*p != 0) {
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.'))
      return RDB_ARGUMENT_ERROR;
    ++p;
  }
  endpoint.kind = d->kind;
  endpoint.address = d->address;
  return rdb_endpoint_valid(&endpoint);
}
void rdb_store_close(rdb_store *s) {
  if (s->lock >= 0)
    close(s->lock);
  if (s->json != NULL)
    lonejson_free(s->json);
  s->lock = -1;
  s->json = NULL;
  memset(s->definitions, 0, sizeof(s->definitions));
}
static int owned_file(int fd) {
  struct stat st;
  return fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
         (st.st_mode & 0077) == 0;
}
int rdb_store_open(rdb_store *s, const char *path) {
  char *lock_path;
  struct flock lock;
  lonejson_stream *stream;
  lonejson_stream_result result;
  lonejson_error error;
  rdb_document doc;
  rdb_definition d;
  size_t i;
  int fd, status;
  memset(s, 0, sizeof(*s));
  s->lock = -1;
  s->path = path;
  if (path == NULL || path[0] == 0)
    return RDB_ARGUMENT_ERROR;
  lock_path = (char *)malloc(strlen(path) + 6);
  if (lock_path == NULL)
    return RDB_MEMORY_ERROR;
  sprintf(lock_path, "%s.lock", path);
  s->lock = open(lock_path, O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK, 0600);
  free(lock_path);
  memset(&lock, 0, sizeof(lock));
  lock.l_type = F_WRLCK;
  lock.l_whence = SEEK_SET;
  if (s->lock < 0 || !owned_file(s->lock) || fcntl(s->lock, F_SETLK, &lock) < 0)
    return RDB_ERROR;
  s->json = rdb_json_new();
  if (s->json == NULL)
    return RDB_MEMORY_ERROR;
  fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0)
    return errno == ENOENT ? RDB_OK : RDB_ERROR;
  if (!owned_file(fd)) {
    close(fd);
    return RDB_ERROR;
  }
  lonejson_error_init(&error);
  stream =
      lonejson_stream_open_fd(s->json, rdb_json_definition_map(), fd, &error);
  if (stream == NULL) {
    close(fd);
    return RDB_MEMORY_ERROR;
  }
  status = RDB_OK;
  while ((result = lonejson_stream_next(stream, &doc, &error)) ==
         LONEJSON_STREAM_OBJECT) {
    if (strcmp(doc.type, "definition") != 0 ||
        s->count == RDB_DEFINITION_LIMIT ||
        rdb_document_definition(&doc, &d) != RDB_OK ||
        rdb_definition_valid(&d) != RDB_OK) {
      status = RDB_ERROR;
      break;
    }
    for (i = 0; i < s->count; ++i)
      if (strcmp(d.name, s->definitions[i].name) == 0 ||
          (d.kind == s->definitions[i].kind &&
           strcmp(d.address, s->definitions[i].address) == 0))
        status = RDB_ERROR;
    if (status != RDB_OK)
      break;
    s->definitions[s->count++] = d;
  }
  if (result != LONEJSON_STREAM_EOF)
    status = RDB_ERROR;
  lonejson_stream_close(stream);
  close(fd);
  return status;
}
int rdb_store_save(rdb_store *s) {
  char *temp, *directory, *slash;
  FILE *fp;
  int fd, dirfd, status;
  size_t i;
  lonejson_error error;
  rdb_document doc;
  temp = (char *)malloc(strlen(s->path) + 12);
  directory = (char *)malloc(strlen(s->path) + 2);
  if (temp == NULL || directory == NULL) {
    free(temp);
    free(directory);
    return RDB_MEMORY_ERROR;
  }
  sprintf(temp, "%s.XXXXXX", s->path);
  strcpy(directory, s->path);
  slash = strrchr(directory, '/');
  if (slash == NULL)
    strcpy(directory, ".");
  else if (slash == directory)
    slash[1] = 0;
  else
    *slash = 0;
  dirfd = open(directory, O_RDONLY | O_DIRECTORY);
  free(directory);
  fd = mkstemp(temp);
  fp = fd >= 0 ? fdopen(fd, "w") : NULL;
  status = RDB_ERROR;
  if (fp == NULL || dirfd < 0) {
    if (fp != NULL)
      fclose(fp);
    else if (fd >= 0)
      close(fd);
  } else {
    status = RDB_OK;
    lonejson_error_init(&error);
    for (i = 0; i < s->count && status == RDB_OK; ++i) {
      rdb_document_set(&doc, &s->definitions[i], 1);
      if (lonejson_serialize_jsonl_filep(s->json, rdb_json_definition_map(),
                                         &doc, 1, 0, fp,
                                         &error) != LONEJSON_STATUS_OK)
        status = RDB_ERROR;
    }
    if (fflush(fp) != 0 || fsync(fd) != 0)
      status = RDB_ERROR;
    if (fclose(fp) != 0)
      status = RDB_ERROR;
    if (status == RDB_OK) {
      if (rename(temp, s->path) != 0)
        status = RDB_ERROR;
      else if (fsync(dirfd) != 0)
        status = RDB_OUTCOME_UNKNOWN;
    }
  }
  if (dirfd >= 0)
    close(dirfd);
  unlink(temp);
  free(temp);
  return status;
}
