#include "service_test.h"
static char names[64][64], addresses[64][108];
static void seed(void) {
  FILE *file;
  lonejson *json;
  lonejson_error error;
  rdb_document d;
  int i;
  size_t length;
  file = fopen("state", "w");
  CHECK(file != NULL);
  CHECK(chmod("state", 0600) == 0);
  json = rdb_json_new();
  CHECK(json != NULL);
  lonejson_error_init(&error);
  for (i = 0; i < 64; ++i) {
    sprintf(names[i], "db%02d", i);
    length = strlen(names[i]);
    memset(names[i] + length, 'n', 63 - length);
    names[i][63] = 0;
    sprintf(addresses[i], "data-%ld-%02d", (long)getpid(), i);
    length = strlen(addresses[i]);
    memset(addresses[i] + length, 'd', 95 - length);
    addresses[i][95] = 0;
    d = document("", names[i], addresses[i]);
    strcpy(d.type, "definition");
    memset(d.dsn, 1, 255);
    d.dsn[255] = 0;
    CHECK(lonejson_serialize_jsonl_filep(json, rdb_json_definition_map(), &d, 1,
                                         0, file,
                                         &error) == LONEJSON_STATUS_OK);
  }
  lonejson_free(json);
  CHECK(fclose(file) == 0);
}
static int closed(int fd) {
  char byte;
  ssize_t count;
  count = read(fd, &byte, 1);
  if (count == 0 || (count < 0 && (errno == ECONNRESET || errno == EPIPE)))
    return 1;
  CHECK(count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
  return 0;
}
static void availability(void) {
  const char *ctl;
  char control[64], key[65], replacement[108], byte;
  unsigned char hello[32];
  rdb_document request;
  int fd, active, slow[2], held[100], count, i, status;
  double started, ended[2], deadline;
  long baseline, peak;
  lonejson *json;
  lonejson_stream *stream;
  lonejson_error error;
  rdb_json_input input;
  lonejson_stream_result result;
  sprintf(control, "control-%ld", (long)getpid());
  ctl = control;
  count = 0;
  seed();
  start_service(ctl, "state", NULL, 1);
  fd = peer(ctl);
  CHECK(fd >= 0);
  send_text(fd, "{\"operation\":\"put\",\"name\":\"");
  send_bytes(fd, names[63], 63);
  send_text(fd, "\",\"dsn\":\"must-not-commit\"}");
  pause_ms(50);
  operation(ctl, "get", names[63], NULL, 1);
  CHECK((unsigned char)rows[0].dsn[0] == 1 && strlen(rows[0].dsn) == 255);
  send_text(fd, "{}");
  CHECK(shutdown(fd, SHUT_WR) == 0);
  read_records(fd);
  CHECK(strcmp(rows[row_count - 1].status, "error") == 0);
  close(fd);
  fd = peer(ctl);
  CHECK(fd >= 0);
  {
    char prefix[400];
    sprintf(prefix,
            "{\"operation\":\"put\",\"name\":\"%s\",\"driver\":\"mock\","
            "\"kind\":\"%s\",\"address\":\"%s\",\"dsn\":\"fixture-",
            names[63], endpoint_kind ? "abstract" : "pathname", addresses[63]);
    send_bytes(fd, prefix, strlen(prefix));
  }
  send_text(fd, "\303");
  pause_ms(30);
  operation(ctl, "get", names[63], NULL, 1);
  CHECK((unsigned char)rows[0].dsn[0] == 1);
  send_text(fd, "\245\\u0");
  pause_ms(30);
  operation(ctl, "get", names[63], NULL, 1);
  CHECK((unsigned char)rows[0].dsn[0] == 1);
  send_text(fd, "041\"}");
  pause_ms(30);
  operation(ctl, "get", names[63], NULL, 1);
  CHECK((unsigned char)rows[0].dsn[0] == 1);
  CHECK(shutdown(fd, SHUT_WR) == 0);
  read_records(fd);
  CHECK(strcmp(rows[row_count - 1].status, "ok") == 0);
  close(fd);
  operation(ctl, "get", names[63], NULL, 1);
  CHECK(strcmp(rows[0].dsn, "fixture-\303\245A") == 0);
  open_key(ctl, names[0], key);
  fd = peer(addresses[0]);
  CHECK(fd >= 0);
  send_bytes(fd, key, 32);
  pause_ms(50);
  operation(ctl, "sessions", NULL, NULL, 1);
  CHECK(strcmp(rows[0].status, "pending") == 0);
  memset(hello, 0, sizeof(hello));
  memcpy(hello, "RDB0", 4);
  hello[6] = 1;
  hello[16] = 8;
  hello[30] = 1;
  send_bytes(fd, key + 32, 32);
  send_bytes(fd, hello, sizeof(hello));
  receive_bytes(fd, &byte, 1);
  CHECK(byte == 0);
  response(fd, 1);
  wire(fd, 2, NULL, 0);
  wire(fd, 3, NULL, 0);
  close(fd);
  empty_sessions(ctl);
  fd = peer(addresses[1]);
  CHECK(fd >= 0);
  pause_ms(50);
  sprintf(replacement, "%.95s-new", addresses[1]);
  operation(ctl, "put", names[1], replacement, 1);
  open_key(ctl, names[1], key);
  send_bytes(fd, key, 64);
  receive_bytes(fd, &byte, 1);
  CHECK(byte == 1 && read(fd, &byte, 1) == 0);
  close(fd);
  fd = attach(replacement, key);
  wire(fd, 3, NULL, 0);
  close(fd);
  empty_sessions(ctl);
  slow[0] = peer(ctl);
  slow[1] = peer(addresses[2]);
  CHECK(slow[0] >= 0 && slow[1] >= 0);
  send_text(slow[0], "{\"operation\":\"list\",\"ignored\":\"");
  send_text(slow[1], "0");
  for (i = 0; i < 2; ++i)
    CHECK(fcntl(slow[i], F_SETFL, O_NONBLOCK) == 0);
  started = now();
  ended[0] = ended[1] = 0;
  while (!ended[0] || !ended[1]) {
    for (i = 0; i < 2; ++i)
      if (!ended[i]) {
        if (closed(slow[i]))
          ended[i] = now() - started;
        else if (write(slow[i], "x", 1) < 0 &&
                 (errno == EPIPE || errno == ECONNRESET))
          ended[i] = now() - started;
      }
    operation(ctl, "get", names[0], NULL, 1);
    CHECK(now() - started < 7);
    pause_ms(250);
  }
  CHECK(ended[0] >= 4.5 && ended[0] <= 6.5 && ended[1] >= 4.5 &&
        ended[1] <= 6.5);
  close(slow[0]);
  close(slow[1]);
  fd = peer(ctl);
  CHECK(fd >= 0);
  send_text(fd, "{\"operation\":\"list\"}");
  CHECK(shutdown(fd, SHUT_WR) == 0);
  pause_ms(50);
  json = rdb_json_new();
  CHECK(json != NULL);
  memset(&input, 0, sizeof(input));
  input.fd = fd;
  stream = rdb_json_stream(json, &input);
  CHECK(stream != NULL);
  lonejson_error_init(&error);
  CHECK(lonejson_stream_next(stream, &request, &error) ==
            LONEJSON_STREAM_OBJECT &&
        strcmp(request.type, "definition") == 0);
  request = document("put", names[62], addresses[62]);
  strcpy(request.dsn, "changed-during-list");
  query_document(ctl, &request, 1);
  do {
    result = lonejson_stream_next(stream, &request, &error);
  } while (result == LONEJSON_STREAM_OBJECT &&
           strcmp(request.type, "done") != 0);
  CHECK(result == LONEJSON_STREAM_OBJECT && strcmp(request.type, "done") == 0 &&
        strcmp(request.status, "error") == 0);
  lonejson_stream_close(stream);
  lonejson_free(json);
  close(fd);
  open_key(ctl, names[0], key);
  active = attach(addresses[0], key);
  baseline = parent_rss();
  for (i = 0; i < 64; ++i) {
    held[count] = peer(addresses[3]);
    CHECK(held[count] >= 0);
    send_text(held[count++], "0");
  }
  fd = peer(addresses[3]);
  CHECK(fd >= 0 && read(fd, &byte, 1) == 0);
  close(fd);
  for (i = 0; i < 16; ++i) {
    held[count] = peer(ctl);
    CHECK(held[count] >= 0);
    send_text(held[count++], "{");
  }
  fd = peer(ctl);
  CHECK(fd >= 0 && read(fd, &byte, 1) == 0);
  close(fd);
  wire(active, 49, NULL, 0);
  for (i = 64; i < count; ++i) {
    close(held[i]);
    held[i] = -1;
  }
  deadline = now() + 3;
  do {
    fd = peer(ctl);
    if (fd >= 0) {
      close(fd);
      break;
    }
    pause_ms(10);
  } while (now() < deadline);
  CHECK(fd >= 0);
  pause_ms(30);
  for (i = 0; i < 8; ++i) {
    held[count] = peer(ctl);
    CHECK(held[count] >= 0);
    send_text(held[count++], "{");
  }
  for (i = 0; i < 4; ++i) {
    held[count] = peer(ctl);
    CHECK(held[count] >= 0);
    send_text(held[count], "{\"operation\":\"list\"}");
    CHECK(shutdown(held[count++], SHUT_WR) == 0);
  }
  pause_ms(100);
  operation(ctl, "get", names[0], NULL, 1);
  wire(active, 49, NULL, 0);
  peak = parent_rss();
  CHECK(!baseline || peak - baseline < 8192);
  started = now();
  stop_service();
  CHECK(now() - started < 2 && read(active, &byte, 1) == 0);
  close(active);
  for (i = 0; i < count; ++i)
    if (held[i] >= 0)
      close(held[i]);
  if (!endpoint_kind) {
    CHECK(access(ctl, F_OK) != 0 && access(replacement, F_OK) != 0);
    for (i = 0; i < 64; ++i)
      CHECK(access(addresses[i], F_OK) != 0);
  }
  printf("%s: absolute deadlines %.2f/%.2f sec; parent RSS %ld -> %ld KiB\n",
         endpoint_kind ? "abstract" : "pathname", ended[0], ended[1], baseline,
         peak);
  status = unlink("state");
  CHECK(status == 0);
  CHECK(unlink("state.lock") == 0);
}
int main(int argc, char **argv) {
  CHECK(argc == 3);
  daemon_path = argv[1];
  CHECK(chdir(argv[2]) == 0);
  signal(SIGPIPE, SIG_IGN);
  availability();
#ifdef __linux__
  endpoint_kind = 1;
  availability();
#endif
  puts("Nonblocking input/output, deadlines, isolation, admission, memory and "
       "shutdown passed");
  return 0;
}
