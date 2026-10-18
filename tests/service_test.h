#ifndef SERVICE_TEST_H
#define SERVICE_TEST_H
#define _POSIX_C_SOURCE 200809L
#include "control_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #x,        \
              errno);                                                          \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)
extern const char *daemon_path;
extern int endpoint_kind;
extern pid_t service_pid;
extern rdb_document rows[130];
extern int row_count;
double now(void);
void pause_ms(long milliseconds);
int peer(const char *address);
void send_bytes(int fd, const void *data, size_t size);
void send_text(int fd, const char *text);
void receive_bytes(int fd, void *data, size_t size);
void response(int fd, unsigned int operation);
void wire(int fd, unsigned int operation, const void *payload, size_t length);
int attach(const char *address, const char *key);
void bad_key(const char *address, const char *key);
rdb_document document(const char *operation, const char *name,
                      const char *address);
void read_records(int fd);
void query_document(const char *control, const rdb_document *request,
                    int success);
void query_raw(const char *control, const char *request, int fragment,
               int success);
void operation(const char *control, const char *op, const char *name,
               const char *address, int success);
void open_key(const char *control, const char *name, char key[65]);
void empty_sessions(const char *control);
void start_service(const char *control, const char *state,
                   const char *const *extra, int readiness);
void stop_service(void);
int daemon_exit(const char *const *arguments);
void write_file(const char *name, const char *text, unsigned int mode);
long parent_rss(void);
#endif
