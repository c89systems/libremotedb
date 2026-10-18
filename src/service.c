#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif
#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "control_exchange.h"
#include "endpoint_internal.h"
#include "internal.h"
#include "store_internal.h"
#include <errno.h>
#include <fcntl.h>
#if defined(__linux__) || defined(__APPLE__)
#include <sys/random.h>
#endif
#include <poll.h>
#include <remotedb/service.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CONTROL_LIMIT 16
#define HANDSHAKE_LIMIT 64
#define CLIENT_SECONDS 5
enum control_mode {
  CONTROL_READ,
  CONTROL_RECORD,
  CONTROL_DEFINITIONS,
  CONTROL_SESSIONS,
  CONTROL_DONE
};
typedef struct control_client {
  rdb_control_exchange exchange;
  struct timespec deadline;
  unsigned long uid, revision;
  size_t cursor;
  int mode, status, terminal;
  char issued_key[65];
} control_client;
typedef struct key_client {
  int fd;
  struct timespec deadline;
  unsigned long uid, listener_id;
  char name[RDB_NAME_CAPACITY];
  unsigned char key[64];
  size_t used;
  int rejected;
} key_client;
typedef struct listener {
  int fd;
  rdb_endpoint endpoint;
  char address[RDB_ADDRESS_CAPACITY];
  dev_t device;
  ino_t inode;
  unsigned long id;
} listener;
typedef struct session {
  pid_t pid;
  int fd;
  char key[65];
  unsigned long uid;
  time_t expires;
  rdb_definition definition;
} session;
typedef struct service {
  rdb_store store;
  listener control;
  listener data[RDB_DEFINITION_LIMIT];
  session sessions[RDB_SESSION_LIMIT];
  control_client controls[CONTROL_LIMIT];
  key_client handshakes[HANDSHAKE_LIMIT];
  unsigned long next_listener_id, definitions_revision, sessions_revision;
  int fatal;
  const rdb_service_config *config;
  rdb_backend_factory_fn factory;
  rdb_backend_cleanup_fn cleanup;
  void *user;
} service;
static int nonblocking(int fd) {
  int flags;
  flags = fcntl(fd, F_GETFL);
  return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 &&
                 fcntl(fd, F_SETFD, FD_CLOEXEC) == 0
             ? RDB_OK
             : RDB_ERROR;
}
static int expired(const struct timespec *deadline,
                   const struct timespec *now) {
  return now->tv_sec > deadline->tv_sec ||
         (now->tv_sec == deadline->tv_sec && now->tv_nsec >= deadline->tv_nsec);
}
static void key_close(key_client *c) {
  if (c->fd >= 0)
    close(c->fd);
  memset(c, 0, sizeof(*c));
  c->fd = -1;
}
static void control_close(service *s, control_client *c, int completed) {
  size_t i;
  if (!completed && c->issued_key[0] != 0)
    for (i = 0; i < RDB_SESSION_LIMIT; ++i)
      if (s->sessions[i].pid == 0 &&
          strcmp(s->sessions[i].key, c->issued_key) == 0) {
        memset(&s->sessions[i], 0, sizeof(s->sessions[i]));
        ++s->sessions_revision;
        break;
      }
  rdb_exchange_close(&c->exchange);
  memset(c, 0, sizeof(*c));
  c->exchange.fd = -1;
}
static void listener_close(listener *l) {
  struct stat st;
  if (l->fd < 0)
    return;
  close(l->fd);
  l->fd = -1;
  if (l->endpoint.kind == RDB_PATHNAME && lstat(l->address, &st) == 0 &&
      S_ISSOCK(st.st_mode) && st.st_dev == l->device && st.st_ino == l->inode)
    unlink(l->address);
}
static int listener_open(listener *l, int kind, const char *address,
                         int multi_uid) {
  struct stat st;
  int status;
  l->fd = -1;
  l->endpoint.kind = kind;
  if (address == NULL || strlen(address) >= sizeof(l->address))
    return RDB_ARGUMENT_ERROR;
  strcpy(l->address, address);
  l->endpoint.address = l->address;
  status = rdb_endpoint_listen(&l->endpoint, &l->fd);
  if (status != RDB_OK)
    return status;
  if (kind == RDB_PATHNAME) {
    if (lstat(address, &st) != 0) {
      close(l->fd);
      l->fd = -1;
      return RDB_ERROR;
    }
    l->device = st.st_dev;
    l->inode = st.st_ino;
    if (chmod(address, multi_uid ? 0666 : 0600) != 0) {
      listener_close(l);
      return RDB_ERROR;
    }
  }
  if (nonblocking(l->fd) != RDB_OK) {
    listener_close(l);
    return RDB_ERROR;
  }
  return RDB_OK;
}
static int authorized(int fd, const unsigned long *uids, size_t count) {
  unsigned long uid;
  size_t i;
  if (rdb_endpoint_peer_uid(fd, &uid) != RDB_OK)
    return 0;
  if (count == 0)
    return uid == (unsigned long)geteuid();
  for (i = 0; i < count; ++i)
    if (uid == uids[i])
      return 1;
  return 0;
}
static void reap(service *s) {
  size_t i;
  int status, finished;
  pid_t child;
  for (i = 0; i < RDB_SESSION_LIMIT; ++i) {
    finished = s->sessions[i].pid == 0 && s->sessions[i].key[0] != 0 &&
               s->sessions[i].expires <= time(NULL);
    if (s->sessions[i].pid > 0) {
      child = waitpid(s->sessions[i].pid, &status, WNOHANG);
      /* The embedding host may auto-reap or collect workers itself. Never
       * retain their credentials, descriptors, or stale process identities. */
      finished = child == s->sessions[i].pid || (child < 0 && errno == ECHILD);
    }
    if (finished) {
      if (s->sessions[i].pid > 0)
        close(s->sessions[i].fd);
      memset(&s->sessions[i], 0, sizeof(s->sessions[i]));
      ++s->sessions_revision;
    }
  }
}
static int active(service *s, const char *name) {
  size_t i;
  for (i = 0; i < RDB_SESSION_LIMIT; ++i)
    if (s->sessions[i].key[0] != 0 &&
        strcmp(s->sessions[i].definition.name, name) == 0)
      return 1;
  return 0;
}
static int find_definition(service *s, const char *name) {
  size_t i;
  for (i = 0; i < s->store.count; ++i)
    if (strcmp(s->store.definitions[i].name, name) == 0)
      return (int)i;
  return -1;
}
static int put_definition(service *s, const rdb_document *doc) {
  rdb_definition d, previous;
  listener fresh;
  int index, status, changed, added;
  size_t i;
  if (rdb_document_definition(doc, &d) != RDB_OK ||
      rdb_definition_valid(&d) != RDB_OK)
    return RDB_ARGUMENT_ERROR;
  status = rdb_store_endpoint_valid(&s->store, d.kind, d.address);
  if (status != RDB_OK)
    return status;
  index = find_definition(s, d.name);
  if (active(s, d.name))
    return RDB_STATE_ERROR;
  added = index < 0;
  if (added && s->store.count == RDB_DEFINITION_LIMIT)
    return RDB_STATE_ERROR;
  for (i = 0; i < s->store.count; ++i)
    if ((int)i != index && d.kind == s->store.definitions[i].kind &&
        strcmp(d.address, s->store.definitions[i].address) == 0)
      return RDB_ARGUMENT_ERROR;
  changed = added || d.kind != s->store.definitions[index].kind ||
            strcmp(d.address, s->store.definitions[index].address) != 0;
  fresh.fd = -1;
  if (changed) {
    status = listener_open(&fresh, d.kind, d.address,
                           s->config->data_uid_count != 0);
    if (status != RDB_OK)
      return status;
  }
  if (added)
    index = (int)s->store.count++;
  previous = s->store.definitions[index];
  s->store.definitions[index] = d;
  status = rdb_store_save(&s->store);
  if (status != RDB_OK && status != RDB_OUTCOME_UNKNOWN) {
    s->store.definitions[index] = previous;
    if (added)
      --s->store.count;
    listener_close(&fresh);
    return status;
  }
  if (changed) {
    listener_close(&s->data[index]);
    fresh.id = ++s->next_listener_id;
    s->data[index] = fresh;
    s->data[index].endpoint.address = s->data[index].address;
  }
  ++s->definitions_revision;
  if (status == RDB_OUTCOME_UNKNOWN)
    s->fatal = 1;
  return status;
}
static int delete_definition(service *s, const char *name) {
  rdb_definition previous;
  int index, status;
  size_t i;
  index = find_definition(s, name);
  if (index < 0 || active(s, name))
    return RDB_STATE_ERROR;
  previous = s->store.definitions[index];
  for (i = (size_t)index; i + 1 < s->store.count; ++i)
    s->store.definitions[i] = s->store.definitions[i + 1];
  --s->store.count;
  status = rdb_store_save(&s->store);
  if (status != RDB_OK && status != RDB_OUTCOME_UNKNOWN) {
    for (i = s->store.count; i > (size_t)index; --i)
      s->store.definitions[i] = s->store.definitions[i - 1];
    s->store.definitions[index] = previous;
    ++s->store.count;
    return status;
  }
  listener_close(&s->data[index]);
  for (i = (size_t)index; i < s->store.count; ++i) {
    s->data[i] = s->data[i + 1];
    s->data[i].endpoint.address = s->data[i].address;
  }
  s->data[s->store.count].fd = -1;
  memset(&s->store.definitions[s->store.count], 0, sizeof(previous));
  ++s->definitions_revision;
  if (status == RDB_OUTCOME_UNKNOWN)
    s->fatal = 1;
  return status;
}
static int session_open(service *s, control_client *c, rdb_document *reply) {
  unsigned char random[32];
  static const char hex[] = "0123456789abcdef";
  const rdb_document *request;
  session *entry;
  size_t slot, i;
  int index;
  request = &c->exchange.request;
  index = find_definition(s, request->name);
  if (index < 0)
    return RDB_ERROR;
  for (slot = 0; slot < RDB_SESSION_LIMIT; ++slot)
    if (s->sessions[slot].key[0] == 0)
      break;
  if (slot == RDB_SESSION_LIMIT || getentropy(random, sizeof(random)) != 0)
    return RDB_ERROR;
  entry = &s->sessions[slot];
  entry->fd = -1;
  entry->uid = c->uid;
  for (i = 0; i < sizeof(random); ++i) {
    entry->key[i * 2] = hex[random[i] >> 4];
    entry->key[i * 2 + 1] = hex[random[i] & 15];
  }
  entry->key[64] = 0;
  for (i = 0; i < RDB_SESSION_LIMIT; ++i)
    if (i != slot && strcmp(entry->key, s->sessions[i].key) == 0) {
      memset(entry, 0, sizeof(*entry));
      return RDB_ERROR;
    }
  entry->definition = s->store.definitions[index];
  if (request->dsn[0] != 0)
    strcpy(entry->definition.dsn, request->dsn);
  if (request->attributes[0] != 0)
    strcpy(entry->definition.attributes, request->attributes);
  entry->expires = time(NULL) + 60;
  rdb_document_set(reply, &entry->definition, 0);
  strcpy(reply->type, "session");
  strcpy(reply->connection_key, entry->key);
  strcpy(reply->status, "pending");
  strcpy(c->issued_key, entry->key);
  ++s->sessions_revision;
  return RDB_OK;
}
/* Generate only the next mapped record; never snapshot or serialize a list. */
static int control_next(service *s, control_client *c) {
  rdb_document reply;
  size_t i;
  unsigned long revision;
  revision = c->mode == CONTROL_DEFINITIONS ? s->definitions_revision
                                            : s->sessions_revision;
  if ((c->mode == CONTROL_DEFINITIONS || c->mode == CONTROL_SESSIONS) &&
      revision != c->revision) {
    c->status = RDB_STATE_ERROR;
    c->mode = CONTROL_DONE;
  }
  if (c->mode == CONTROL_DEFINITIONS && c->cursor < s->store.count) {
    rdb_document_set(&reply, &s->store.definitions[c->cursor++], 0);
    return rdb_exchange_reply(&c->exchange, &reply);
  }
  if (c->mode == CONTROL_SESSIONS)
    while (c->cursor < RDB_SESSION_LIMIT) {
      i = c->cursor++;
      if (s->sessions[i].key[0] != 0 && s->sessions[i].uid == c->uid &&
          (c->exchange.request.name[0] == 0 ||
           strcmp(c->exchange.request.name, s->sessions[i].definition.name) ==
               0)) {
        rdb_document_set(&reply, &s->sessions[i].definition, 0);
        strcpy(reply.type, "session");
        strcpy(reply.status, s->sessions[i].pid > 0 ? "active" : "pending");
        return rdb_exchange_reply(&c->exchange, &reply);
      }
    }
  c->mode = CONTROL_DONE;
  c->terminal = 1;
  memset(&reply, 0, sizeof(reply));
  strcpy(reply.type, "done");
  strcpy(reply.status, c->status == RDB_OK                ? "ok"
                       : c->status == RDB_OUTCOME_UNKNOWN ? "unknown"
                                                          : "error");
  if (c->status != RDB_OK)
    strcpy(reply.message,
           c->status == RDB_OUTCOME_UNKNOWN
               ? "State replaced but directory sync failed; service stopping."
               : "Request failed or registry changed while listing; check "
                 "operation, "
                 "definition, active sessions, endpoint and state directory.");
  return rdb_exchange_reply(&c->exchange, &reply);
}
static int control_execute(service *s, control_client *c) {
  const rdb_document *doc;
  rdb_document reply;
  size_t i;
  int index;
  doc = &c->exchange.request;
  c->mode = CONTROL_DONE;
  c->status = RDB_ERROR;
  reap(s);
  if (strcmp(doc->operation, "put") == 0)
    c->status = put_definition(s, doc);
  else if (strcmp(doc->operation, "delete") == 0)
    c->status = delete_definition(s, doc->name);
  else if (strcmp(doc->operation, "open") == 0) {
    c->status = session_open(s, c, &reply);
    if (c->status == RDB_OK)
      c->mode = CONTROL_RECORD;
  } else if (strcmp(doc->operation, "get") == 0) {
    index = find_definition(s, doc->name);
    if (index >= 0) {
      rdb_document_set(&reply, &s->store.definitions[index], 0);
      c->status = RDB_OK;
      c->mode = CONTROL_RECORD;
    }
  } else if (strcmp(doc->operation, "list") == 0) {
    c->status = RDB_OK;
    c->mode = CONTROL_DEFINITIONS;
    c->revision = s->definitions_revision;
  } else if (strcmp(doc->operation, "sessions") == 0) {
    c->status = RDB_OK;
    c->mode = CONTROL_SESSIONS;
    c->revision = s->sessions_revision;
  } else if (strcmp(doc->operation, "close") == 0) {
    for (i = 0; i < RDB_SESSION_LIMIT; ++i)
      if (s->sessions[i].key[0] != 0 && s->sessions[i].uid == c->uid &&
          strcmp(s->sessions[i].key, doc->connection_key) == 0) {
        if (s->sessions[i].pid > 0)
          c->status =
              shutdown(s->sessions[i].fd, SHUT_RDWR) == 0 ? RDB_OK : RDB_ERROR;
        else {
          memset(&s->sessions[i], 0, sizeof(s->sessions[i]));
          ++s->sessions_revision;
          c->status = RDB_OK;
        }
        break;
      }
  }
  if (s->fatal) {
    /* Preserve only the uncertain operation's terminal reply until it drains
     * or reaches its deadline; no further requests may mutate this service. */
    for (i = 0; i < CONTROL_LIMIT; ++i)
      if (&s->controls[i] != c && s->controls[i].exchange.fd >= 0)
        control_close(s, &s->controls[i], 0);
    for (i = 0; i < HANDSHAKE_LIMIT; ++i)
      key_close(&s->handshakes[i]);
  }
  return c->mode == CONTROL_RECORD ? rdb_exchange_reply(&c->exchange, &reply)
                                   : control_next(s, c);
}
static void control_step(service *s, control_client *c) {
  int status;
  if (c->mode == CONTROL_READ) {
    status = rdb_exchange_receive(&c->exchange);
    if (status == RDB_NO_DATA)
      return;
    if (status == RDB_OK)
      status = control_execute(s, c);
    else {
      c->mode = CONTROL_DONE;
      c->status = RDB_ERROR;
      status = control_next(s, c);
    }
  } else {
    status = rdb_exchange_send(&c->exchange);
    if (status == RDB_NO_DATA)
      return;
    if (status == RDB_OK && c->terminal) {
      control_close(s, c, 1);
      return;
    }
    if (status == RDB_OK) {
      if (c->mode == CONTROL_RECORD) {
        /* The key record has reached the transport. A peer may consume done
         * before its JSONL newline drains, so terminal-write failure must not
         * revoke a key that has already been published. */
        c->issued_key[0] = 0;
        c->mode = CONTROL_DONE;
      }
      status = control_next(s, c);
    }
  }
  if (status != RDB_OK)
    control_close(s, c, 0);
}
static void accept_control(service *s, const struct timespec *now) {
  control_client *c;
  size_t i;
  int fd;
  fd = accept(s->control.fd, NULL, NULL);
  if (fd < 0)
    return;
  if (!authorized(fd, s->config->control_uids, s->config->control_uid_count)) {
    close(fd);
    return;
  }
  for (i = 0; i < CONTROL_LIMIT; ++i)
    if (s->controls[i].exchange.fd < 0)
      break;
  if (i == CONTROL_LIMIT || nonblocking(fd) != RDB_OK) {
    close(fd);
    return;
  }
  c = &s->controls[i];
  if (rdb_endpoint_peer_uid(fd, &c->uid) != RDB_OK) {
    close(fd);
    return;
  }
  if (rdb_exchange_open(&c->exchange, fd) != RDB_OK)
    return;
  c->deadline = *now;
  c->deadline.tv_sec += CLIENT_SECONDS;
  c->mode = CONTROL_READ;
}
static void accept_key(service *s, size_t definition,
                       const struct timespec *now) {
  key_client *c;
  size_t i;
  int fd;
  fd = accept(s->data[definition].fd, NULL, NULL);
  if (fd < 0)
    return;
  if (!authorized(fd, s->config->data_uids, s->config->data_uid_count)) {
    close(fd);
    return;
  }
  for (i = 0; i < HANDSHAKE_LIMIT; ++i)
    if (s->handshakes[i].fd < 0)
      break;
  if (i == HANDSHAKE_LIMIT || nonblocking(fd) != RDB_OK) {
    close(fd);
    return;
  }
  c = &s->handshakes[i];
  if (rdb_endpoint_peer_uid(fd, &c->uid) != RDB_OK) {
    close(fd);
    return;
  }
  c->fd = fd;
  c->listener_id = s->data[definition].id;
  strcpy(c->name, s->store.definitions[definition].name);
  c->deadline = *now;
  c->deadline.tv_sec += CLIENT_SECONDS;
}

static void start_session(service *s, key_client *c) {
  size_t i, slot;
  pid_t pid;
  rdb_backend backend;
  int status, fd, index, flags;
  unsigned char accepted;
  unsigned int difference;
  fd = c->fd;
  index = find_definition(s, c->name);
  if (index < 0 || s->data[index].id != c->listener_id) {
    c->rejected = 1;
    return;
  }
  for (slot = 0; slot < RDB_SESSION_LIMIT; ++slot) {
    difference = 0;
    for (i = 0; i < sizeof(c->key); ++i)
      difference |= c->key[i] ^ (unsigned char)s->sessions[slot].key[i];
    if (difference == 0 && s->sessions[slot].uid == c->uid &&
        s->sessions[slot].pid == 0 && s->sessions[slot].expires > time(NULL) &&
        strcmp(s->sessions[slot].definition.name, c->name) == 0)
      break;
  }
  if (slot == RDB_SESSION_LIMIT) {
    c->rejected = 1;
    return;
  }
  /* Only the isolated backend transport returns to blocking operation. */
  flags = fcntl(fd, F_GETFL);
  if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) != 0) {
    key_close(c);
    return;
  }
  s->sessions[slot].fd = dup(fd);
  if (s->sessions[slot].fd < 0) {
    key_close(c);
    return;
  }
  pid = fork();
  if (pid < 0) {
    close(s->sessions[slot].fd);
    s->sessions[slot].fd = -1;
    key_close(c);
    return;
  }
  if (pid == 0) {
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    close(s->control.fd);
    for (i = 0; i < s->store.count; ++i)
      close(s->data[i].fd);
    close(s->store.lock);
    if (s->config->stop_fd >= 0)
      close(s->config->stop_fd);
    for (i = 0; i < RDB_SESSION_LIMIT; ++i)
      if (s->sessions[i].pid > 0 || i == slot)
        close(s->sessions[i].fd);
    for (i = 0; i < CONTROL_LIMIT; ++i)
      rdb_exchange_close(&s->controls[i].exchange);
    for (i = 0; i < HANDSHAKE_LIMIT; ++i)
      if (s->handshakes[i].fd != fd)
        key_close(&s->handshakes[i]);
    memset(&backend, 0, sizeof(backend));
    status = s->factory(s->user, &s->sessions[slot].definition, &backend);
    if (status == RDB_OK) {
      accepted = 0;
      status = rdb_io_write(fd, &accepted, 1);
      if (status == RDB_OK)
        status = rdb_server_serve_fd(fd, NULL, &backend);
      if (s->cleanup != NULL)
        s->cleanup(s->user, &backend);
    }
    close(fd);
    /* Child never runs parent cleanup or flushes inherited stdio buffers. */
    _exit(status == RDB_OK ? 0 : 1);
  }
  s->sessions[slot].pid = pid;
  ++s->sessions_revision;
  key_close(c);
}
static void key_step(service *s, key_client *c) {
  ssize_t count;
  size_t written;
  unsigned char rejected;
  int status;
  if (c->rejected) {
    rejected = 1;
    status = rdb_io_try_write(c->fd, &rejected, 1, &written);
    if (status != RDB_NO_DATA)
      key_close(c);
    return;
  }
  count = read(c->fd, c->key + c->used, sizeof(c->key) - c->used);
  if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
    return;
  if (count <= 0) {
    key_close(c);
    return;
  }
  c->used += (size_t)count;
  if (c->used == sizeof(c->key))
    start_session(s, c);
}
static void stop_sessions(service *s) {
  size_t i;
  int status, remaining, attempt;
  struct timespec delay;
  for (i = 0; i < RDB_SESSION_LIMIT; ++i)
    if (s->sessions[i].pid > 0)
      shutdown(s->sessions[i].fd, SHUT_RDWR);
  delay.tv_sec = 0;
  delay.tv_nsec = 10000000L;
  for (attempt = 0; attempt < 100; ++attempt) {
    reap(s);
    remaining = 0;
    for (i = 0; i < RDB_SESSION_LIMIT; ++i)
      remaining += s->sessions[i].pid > 0;
    if (remaining == 0)
      break;
    nanosleep(&delay, NULL);
  }
  for (i = 0; i < RDB_SESSION_LIMIT; ++i)
    if (s->sessions[i].pid > 0) {
      kill(s->sessions[i].pid, SIGKILL);
      while (waitpid(s->sessions[i].pid, &status, 0) < 0 && errno == EINTR) {
      }
      close(s->sessions[i].fd);
    }
}
int rdb_service_run(const rdb_service_config *config,
                    rdb_backend_factory_fn factory,
                    rdb_backend_cleanup_fn cleanup, void *user, char *error,
                    size_t error_capacity) {
  service *s;
  struct pollfd fds[RDB_DEFINITION_LIMIT + 2 + CONTROL_LIMIT + HANDSHAKE_LIMIT];
  struct timespec now;
  size_t i, n, definitions, stop_index, control_index, key_index;
  int status, ready, writers;
  mode_t previous_umask;
  if (error != NULL && error_capacity != 0)
    error[0] = 0;
  if (config == NULL || factory == NULL || config->state_path == NULL ||
      rdb_endpoint_valid(&config->control) != RDB_OK ||
      (config->control_uid_count != 0 && config->control_uids == NULL) ||
      (config->data_uid_count != 0 && config->data_uids == NULL))
    return RDB_ARGUMENT_ERROR;
  if (config->control_uid_count > 64 || config->data_uid_count > 64)
    return RDB_ARGUMENT_ERROR;
  for (i = 0; i < config->control_uid_count; ++i)
    if ((unsigned long)(uid_t)config->control_uids[i] !=
        config->control_uids[i])
      return RDB_ARGUMENT_ERROR;
  for (i = 0; i < config->data_uid_count; ++i)
    if ((unsigned long)(uid_t)config->data_uids[i] != config->data_uids[i])
      return RDB_ARGUMENT_ERROR;
  s = (service *)calloc(1, sizeof(*s));
  if (s == NULL)
    return RDB_MEMORY_ERROR;
  s->control.fd = -1;
  for (i = 0; i < CONTROL_LIMIT; ++i)
    s->controls[i].exchange.fd = -1;
  for (i = 0; i < HANDSHAKE_LIMIT; ++i)
    s->handshakes[i].fd = -1;
  for (i = 0; i < RDB_DEFINITION_LIMIT; ++i)
    s->data[i].fd = -1;
  s->config = config;
  s->factory = factory;
  s->cleanup = cleanup;
  s->user = user;
  previous_umask = umask(0077);
  status = rdb_store_open(&s->store, config->state_path);
  if (status == RDB_OK)
    status = rdb_store_endpoint_valid(&s->store, config->control.kind,
                                      config->control.address);
  for (i = 0; i < s->store.count && status == RDB_OK; ++i)
    status = rdb_store_endpoint_valid(&s->store, s->store.definitions[i].kind,
                                      s->store.definitions[i].address);
  if (status == RDB_OK)
    status =
        listener_open(&s->control, config->control.kind,
                      config->control.address, config->control_uid_count != 0);
  for (i = 0; i < s->store.count && status == RDB_OK; ++i) {
    status = listener_open(&s->data[i], s->store.definitions[i].kind,
                           s->store.definitions[i].address,
                           config->data_uid_count != 0);
    s->data[i].id = ++s->next_listener_id;
  }
  while (status == RDB_OK) {
    reap(s);
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
      status = RDB_ERROR;
      break;
    }
    writers = 0;
    for (i = 0; i < CONTROL_LIMIT; ++i) {
      if (s->controls[i].exchange.fd >= 0 &&
          expired(&s->controls[i].deadline, &now))
        control_close(s, &s->controls[i], 0);
      writers += s->controls[i].exchange.fd >= 0;
    }
    for (i = 0; i < HANDSHAKE_LIMIT; ++i)
      if (s->handshakes[i].fd >= 0 && expired(&s->handshakes[i].deadline, &now))
        key_close(&s->handshakes[i]);
    if (s->fatal && !writers)
      break;
    memset(fds, 0, sizeof(fds));
    definitions = s->store.count;
    fds[0].fd = s->fatal ? -1 : s->control.fd;
    fds[0].events = POLLIN;
    for (i = 0; i < definitions; ++i) {
      fds[i + 1].fd = s->fatal ? -1 : s->data[i].fd;
      fds[i + 1].events = POLLIN;
    }
    stop_index = definitions + 1;
    fds[stop_index].fd = config->stop_fd;
    fds[stop_index].events = POLLIN;
    control_index = stop_index + 1;
    for (i = 0; i < CONTROL_LIMIT; ++i) {
      fds[control_index + i].fd = s->controls[i].exchange.fd;
      fds[control_index + i].events =
          s->controls[i].mode == CONTROL_READ ? POLLIN : POLLOUT;
    }
    key_index = control_index + CONTROL_LIMIT;
    for (i = 0; i < HANDSHAKE_LIMIT; ++i) {
      fds[key_index + i].fd = s->handshakes[i].fd;
      fds[key_index + i].events = s->handshakes[i].rejected ? POLLOUT : POLLIN;
    }
    n = key_index + HANDSHAKE_LIMIT;
    ready = poll(fds, (nfds_t)n, 100);
    if (ready < 0) {
      if (errno == EINTR)
        continue;
      status = RDB_ERROR;
      break;
    }
    if (config->stop_fd >= 0 && fds[stop_index].revents != 0) {
      if ((fds[stop_index].revents & (POLLERR | POLLNVAL)) != 0)
        status = RDB_ERROR;
      break;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
      status = RDB_ERROR;
      break;
    }
    /* Accept against this poll snapshot before control can reorder listeners.
     * New clients have no events in the snapshot and wait for the next poll. */
    for (i = 0; i < definitions && !s->fatal; ++i)
      if ((fds[i + 1].revents & POLLIN) != 0)
        accept_key(s, i, &now);
    if (!s->fatal && (fds[0].revents & POLLIN) != 0)
      accept_control(s, &now);
    for (i = 0; i < HANDSHAKE_LIMIT; ++i)
      if (s->handshakes[i].fd >= 0 &&
          s->handshakes[i].fd == fds[key_index + i].fd) {
        if (expired(&s->handshakes[i].deadline, &now) ||
            (fds[key_index + i].revents & (POLLERR | POLLNVAL)) != 0)
          key_close(&s->handshakes[i]);
        else if (fds[key_index + i].revents != 0)
          key_step(s, &s->handshakes[i]);
      }
    for (i = 0; i < CONTROL_LIMIT; ++i)
      if (s->controls[i].exchange.fd >= 0 &&
          s->controls[i].exchange.fd == fds[control_index + i].fd) {
        if (expired(&s->controls[i].deadline, &now) ||
            (fds[control_index + i].revents & (POLLERR | POLLNVAL)) != 0)
          control_close(s, &s->controls[i], 0);
        else if (fds[control_index + i].revents != 0)
          control_step(s, &s->controls[i]);
      }
  }
  if (s->fatal)
    status = RDB_OUTCOME_UNKNOWN;
  for (i = 0; i < CONTROL_LIMIT; ++i)
    control_close(s, &s->controls[i], 0);
  for (i = 0; i < HANDSHAKE_LIMIT; ++i)
    key_close(&s->handshakes[i]);
  stop_sessions(s);
  listener_close(&s->control);
  for (i = 0; i < RDB_DEFINITION_LIMIT; ++i)
    listener_close(&s->data[i]);
  rdb_store_close(&s->store);
  umask(previous_umask);
  free(s);
  if (status != RDB_OK && error != NULL && error_capacity != 0)
    snprintf(error, error_capacity,
             "Service failed: check private state file/lock, approved driver, "
             "unique endpoints and socket permissions (status %d).",
             status);
  return status;
}
