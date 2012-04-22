#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include <linux/if_ether.h>

#include <arpa/inet.h>

#include <time.h> 
#include <dirent.h>

#include <sys/time.h> 
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <sys/syscall.h> 
#include <sys/resource.h> 

#include "list.h"
#include "message_encoder.h"

#define MAX_FD               50
#define MAX_HASH          10000
#define IN_BUFF_MAX         128
#define DEFAULT_TIMEOUT     500
#define KEEPALIVE_TIMEOUT 10000
#define MAX_TRIES             3

#define MAX_CONNECTION (MAX_FD - 1)

#define ECD          0x1
#define EACK         0x2
#define ENACK        0x4
#define ELS          0x8
#define ESHOW        0x10
#define EPUT         0x20
#define EGET         0x40
#define EATTRIBUTES  0x80
#define EDATA        0x100
#define EERROR       0x200
#define EEND         0x400

#define TIMESPEC_TO_MS(ts) ((ts.tv_sec * 1000) + (ts.tv_nsec / 1000000))

struct connection_context {
  struct epoll_event sock_event;
  struct sockaddr_in dst;
  int                efd;
  int                fd;

  uint8_t            sequence;
  uint8_t            last_sequence_received;
  uint8_t            last_cmd[18];
  int                size;

  int                cmd_timeout;
  int                timeout_tries;
  bool               is_waiting_response;
  uint8_t            command;
  uint32_t           expected_commands;

  char               *directory;
  struct dirent      **namelist;
  int                namelist_n;

  off_t              file_size;
  FILE               *file;
  char               *file_name;

  uint64_t           key;

  uint64_t           keepalive_limit_time;

  struct hlist_node  hash_ctx_node;

  struct list_head   retries_list;
  struct list_head   keepalive_list;
};

uint8_t _connection_count = 0;

struct connection_context _retries_head;
struct connection_context _keepalive_head;

static struct {
  struct hlist_head ctx_head;
} _ctx_hash[MAX_HASH];

static bool
daemonize()
{
  int   fd, i;
  pid_t pid;

  if (getppid() == 1)
    return false;

  pid = fork();
  if (pid < 0)
    return false;
  
  if (pid > 0) 
    exit(0);

  setsid();

  for (i = getdtablesize(); i >= 0; --i)
    close(i);

  if ((fd = open("/dev/null", O_RDWR, 0)) != -1) {
    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > 2)
      close(fd);
  }

  return true;
}

static bool
setnonblocking(int fd)
{
  int opts;
  if ((opts = fcntl(fd, F_GETFL)) == -1)
    return false;

  opts = opts | O_NONBLOCK;
  if (fcntl(fd, F_SETFL, opts) == -1)
    return false;

  return true;
}

static struct connection_context *
allocate_context(uint64_t key, const struct sockaddr_in * const dst, int efd)
{
  int optval = 1;
  if (_connection_count + 1 == MAX_CONNECTION)
    return NULL;

  struct connection_context *ctx = (struct connection_context *) malloc(sizeof(struct connection_context));

  memset(ctx, 0, sizeof(struct connection_context));

  if ((ctx->fd = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
    fprintf(stdout, "Problems to start a connection %s", strerror(errno));
    free(ctx);
    return NULL;
  }

  if (!setnonblocking(ctx->fd)) {
    fprintf(stdout, "Problems to start a connection %s", strerror(errno));
    free(ctx);
    return NULL; 
  }

  ctx->efd                = efd;
  ctx->sock_event.data.fd = ctx->fd;
  ctx->sock_event.events  = EPOLLIN | EPOLLET;

  setsockopt(ctx->fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

  ctx->dst.sin_family      = AF_INET;
  ctx->dst.sin_port        = 0;
  ctx->dst.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(ctx->fd, (struct sockaddr *) &ctx->dst, sizeof(ctx->dst)) == -1) {
    fprintf(stdout, "Problems to start a connection %s", strerror(errno));
    free(ctx);
    return NULL;
  }

  if (epoll_ctl(efd, EPOLL_CTL_ADD, ctx->fd, &ctx->sock_event) == -1) {
    fprintf(stdout, "Problems to start a connection %s", strerror(errno));
    free(ctx);
    return NULL; 
  }

  ctx->key                 = key;
  ctx->directory           = strdup("./");
  ctx->expected_commands   = ELS | ECD | EPUT | EGET;
  memcpy(&ctx->dst, dst, sizeof(struct sockaddr_in));

  INIT_LIST_HEAD(&ctx->retries_list);
  INIT_LIST_HEAD(&ctx->keepalive_list);

  _connection_count++;

  return ctx;
}

static int
alphasort_desc(const struct dirent **a, const struct dirent **b) {
  return strcmp((*b)->d_name,(*a)->d_name);
}

static struct connection_context *
find_context(const struct sockaddr_in * const dst, int efd)
{
  uint64_t key = (dst->sin_addr.s_addr << 2) + dst->sin_port;

  int bucket = key % MAX_HASH;

  struct hlist_node *node = NULL, *tmp;

  static struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  struct connection_context *ctx = NULL; 
  hlist_for_each_entry_safe(ctx, node, tmp, &(_ctx_hash[bucket].ctx_head), hash_ctx_node) {
    if (ctx->key == key) {
      ctx->keepalive_limit_time = KEEPALIVE_TIMEOUT + TIMESPEC_TO_MS(ts);

      list_del_init(&ctx->retries_list);
      list_move_tail(&ctx->keepalive_list, &_keepalive_head.keepalive_list);

      return ctx;
    }
  }

  ctx = allocate_context(key, dst, efd);
  if (ctx) {
    ctx->keepalive_limit_time = KEEPALIVE_TIMEOUT + TIMESPEC_TO_MS(ts);
    list_add_tail(&ctx->keepalive_list, &_keepalive_head.keepalive_list);
    hlist_add_head(&ctx->hash_ctx_node, &(_ctx_hash[bucket].ctx_head));
  }

  return ctx;
}

static void
remove_context(struct connection_context *ctx)
{
  hlist_del_init(&ctx->hash_ctx_node);
  list_del_init(&ctx->retries_list);
  list_del_init(&ctx->keepalive_list);

  if (ctx->file)
    fclose(ctx->file);

  free(ctx->file_name);

  while (ctx->namelist_n && ctx->namelist_n--)
    free(ctx->namelist[ctx->namelist_n]);

  _connection_count--;

  epoll_ctl(ctx->efd, EPOLL_CTL_DEL, ctx->fd, NULL);

  close(ctx->fd);

  free(ctx->namelist);
  free(ctx);
}

static void
send_ack(struct connection_context *ctx)
{
  uint8_t msg_size;
  uint8_t msg[18];

  ctx->sequence = ctx->sequence == 3 ? 0 : ctx->sequence + 1;
  msg_size = encode_msg(msg, 0, ctx->sequence, ACK, 0);

  ssize_t sent_bytes;
  do {
    sent_bytes = sendto(ctx->fd, msg, msg_size, 0, 
      (const struct sockaddr *) &(ctx->dst), sizeof(ctx->dst));
  } while (sent_bytes == -1 && errno == EINTR);
  
  if (sent_bytes == -1) {
    remove_context(ctx);
  }
}

static void
send_nack(struct connection_context *ctx)
{
  uint8_t msg_size;
  uint8_t msg[18];

  ctx->sequence = ctx->sequence == 3 ? 0 : ctx->sequence + 1;
  msg_size = encode_msg(msg, 0, ctx->sequence, NACK, 0);

  ssize_t sent_bytes;
  do {
    sent_bytes = sendto(ctx->fd, msg, msg_size, 0, 
      (const struct sockaddr *) &(ctx->dst), sizeof(ctx->dst));
  } while (sent_bytes == -1 && errno == EINTR);

  if (sent_bytes == -1) {
    remove_context(ctx);
  }
}

static void
send_command(uint8_t cmd, struct connection_context *ctx, const uint8_t * const payload, uint8_t payload_size, 
  uint32_t expected_commands)
{
  ctx->sequence = ctx->sequence == 3 ? 0 : ctx->sequence + 1;
  ctx->size = encode_msg(ctx->last_cmd, payload_size, ctx->sequence, cmd, payload);
  
  ssize_t sent_bytes;
  do {
    sent_bytes = sendto(ctx->fd, ctx->last_cmd, ctx->size, 0, 
      (const struct sockaddr *) &(ctx->dst), sizeof(ctx->dst));
  } while (sent_bytes == -1 && errno == EINTR);
 
  if (sent_bytes == -1) {
    remove_context(ctx);
    return;
  }

  static struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  ctx->cmd_timeout         = DEFAULT_TIMEOUT + TIMESPEC_TO_MS(ts);
  ctx->timeout_tries       = MAX_TRIES;
  ctx->is_waiting_response = true;
  ctx->command             = cmd;
  ctx->expected_commands   = expected_commands;

  list_add_tail(&ctx->retries_list, &_retries_head.retries_list);
}

static void
resend_last_command(struct connection_context *ctx, bool reset_retries)
{
  if (ctx->is_waiting_response) {
    ssize_t sent_bytes;
    do {
      sent_bytes = sendto(ctx->fd, ctx->last_cmd, ctx->size, 0, 
        (const struct sockaddr *) &(ctx->dst), sizeof(ctx->dst));
    } while (sent_bytes == -1 && errno == EINTR);
  
    if (sent_bytes == -1) {
      remove_context(ctx);
      return;
    }
  
    static struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ctx->is_waiting_response  = true;
    ctx->cmd_timeout          = DEFAULT_TIMEOUT + TIMESPEC_TO_MS(ts);

    list_add_tail(&ctx->retries_list, &_retries_head.retries_list);
  }
  else
    send_ack(ctx);
}

static void
server_in_ack(void *_ctx, uint8_t sequence, const uint8_t * const payload, uint8_t payload_size)
{
  struct connection_context *ctx = (struct connection_context *) _ctx;

  if (sequence == ctx->last_sequence_received) {
    resend_last_command(ctx, true);
  }
  else if (ctx->expected_commands & EACK) {
    ctx->last_sequence_received = sequence;
    if (ctx->command == ATTRIBUTES || ctx->command == DATA) {
      if (ctx->file) {
        if (feof(ctx->file)) {
          send_command(END, ctx, 0, 0, EACK | ENACK);
        }
        else {
          uint8_t data[15];
          size_t size = fread(data, 1, 15, ctx->file);
          if (size == 0)
            send_command(END, ctx, 0, 0, EACK | ENACK);
          else
            send_command(DATA, ctx, data, size, EACK | ENACK | EERROR);
        }
      }
      else {
        uint8_t error = PDENIED;
        send_command(ERROR, ctx, &error, 1, EACK | ENACK);
      }
    }
    else if (ctx->command == SHOW) {
      if (ctx->namelist_n) {
        ctx->namelist_n--;
        size_t curr_len = strlen(ctx->namelist[ctx->namelist_n]->d_name);
        if (curr_len > 15) {
          ctx->namelist[ctx->namelist_n]->d_name[14] = '~';
          curr_len = 15;
        }

        send_command(SHOW, ctx, ctx->namelist[ctx->namelist_n]->d_name, curr_len, EACK | ENACK);
        free(ctx->namelist[ctx->namelist_n]);
      }
      else {
        send_command(END, ctx, 0, 0, EACK | ENACK);
        free(ctx->namelist);
        ctx->namelist = NULL;
      }
    }
    else {
      if (ctx->file) {
        fclose(ctx->file);
        ctx->file = NULL;
      }

      ctx->cmd_timeout         = 0;
      ctx->timeout_tries       = 0;
      ctx->is_waiting_response = false;
      ctx->command             = 0;
      ctx->expected_commands   = ELS | ECD | EPUT | EGET;
    }
  }
}

static void
server_in_nack(void *_ctx, uint8_t sequence, const uint8_t * const payload, uint8_t payload_size)
{
  struct connection_context *ctx = (struct connection_context *) _ctx;

  if (ctx->expected_commands & ENACK) {
    ctx->last_sequence_received = sequence;
    resend_last_command(ctx, true);
  }
}

static void
server_in_cd(void *_ctx, uint8_t sequence, const uint8_t * const payload, uint8_t payload_size)
{
  struct connection_context *ctx = (struct connection_context *) _ctx;

  if (sequence == ctx->last_sequence_received) {
    resend_last_command(ctx, true);
  }
  else if (ctx->expected_commands & ECD) {
    ctx->last_sequence_received = sequence;
    if (payload_size == 1 && *payload == '.') {
      send_ack(ctx);
    }
    else if (payload_size == 2 && payload[0] == '.' && payload[1] == '.') {
      char *tmp = ctx->directory + strlen(ctx->directory) - 2;
      while (tmp != ctx->directory) {
        if (*tmp == '/') {
          *(tmp + 1) = 0;
          break;
        }

        tmp--;
      }

      send_ack(ctx);
    }
    else {
      // TODO: Check if the first element is a "/" if it is, just replace everything

      char *old_dir = strdup(ctx->directory);

      size_t curr_len = strlen(ctx->directory);
      ctx->directory = (char *) realloc(ctx->directory, curr_len + payload_size + 2);
      memcpy(ctx->directory + curr_len, payload, payload_size);
      *(ctx->directory + curr_len + payload_size) = '/';
      *(ctx->directory + curr_len + payload_size + 1) = 0;

      struct stat info;
      if (stat(ctx->directory, &info) == 0 && S_ISDIR(info.st_mode)) {
        send_ack(ctx);
        free(old_dir);
      } 
      else {
        free(ctx->directory);
        ctx->directory = old_dir;
        uint8_t error  = NEXIST;
        send_command(ERROR, ctx, &error, 1, EACK | ENACK);
      }
    }
  }
}

static void
server_in_ls(void *_ctx, uint8_t sequence, const uint8_t * const payload, uint8_t payload_size)
{
  struct connection_context *ctx = (struct connection_context *) _ctx;

  if (sequence == ctx->last_sequence_received) {
    resend_last_command(ctx, true);
  }
  else if (ctx->expected_commands & ELS) {
    ctx->last_sequence_received = sequence;

    if ((ctx->namelist_n = scandir(ctx->directory, &ctx->namelist, 0, alphasort_desc)) > 0) {
      ctx->namelist_n--;
      size_t curr_len = strlen(ctx->namelist[ctx->namelist_n]->d_name);
      if (curr_len > 15) {
        ctx->namelist[ctx->namelist_n]->d_name[14] = '~';
        curr_len = 15;
      }

      send_command(SHOW, ctx, ctx->namelist[ctx->namelist_n]->d_name, curr_len, EACK | ENACK);
      free(ctx->namelist[ctx->namelist_n]);
    }
    else {
      uint8_t error = PDENIED;
      send_command(ERROR, ctx, &error, 1, EACK | ENACK);
    }
  }
}

static void
server_in_put(void *_ctx, uint8_t sequence, const uint8_t * const payload, uint8_t payload_size)
{
  struct connection_context *ctx = (struct connection_context *) _ctx;

  if (sequence == ctx->last_sequence_received) {
    resend_last_command(ctx, true);
  }
  else if (ctx->expected_commands & EPUT) {
    ctx->last_sequence_received = sequence;

      send_command(ACK, ctx, 0, 0, EATTRIBUTES | ENACK | EERROR);

    size_t curr_len = strlen(ctx->directory);
    char *file = (char *) malloc(curr_len + payload_size + 2);
    memcpy(file, ctx->directory, curr_len);
    memcpy(file + curr_len, payload, payload_size);
    *(file + curr_len + payload_size) = 0;

    if (ctx->file = fopen(file, "wb")) {
      ctx->file_name = file;
      send_command(EACK, ctx, 0, 0, EATTRIBUTES | ENACK | EERROR);
    }
    else {
      free(file);
      uint8_t error = PDENIED;
      send_command(ERROR, ctx, &error, 1, EACK | ENACK);
    } 
  }
}

static void
server_in_get(void *_ctx, uint8_t sequence, const uint8_t * const payload, uint8_t payload_size)
{
  struct connection_context *ctx = (struct connection_context *) _ctx;

  if (sequence == ctx->last_sequence_received) {
    resend_last_command(ctx, true);
  }
  else if (ctx->expected_commands & EGET) {
    ctx->last_sequence_received = sequence;

    size_t curr_len = strlen(ctx->directory);
    char *file = (char *) malloc(curr_len + payload_size + 2);
    memcpy(file, ctx->directory, curr_len);
    memcpy(file + curr_len, payload, payload_size);
    *(file + curr_len + payload_size) = 0;

    struct stat info;
    if (stat(file, &info) == 0 && S_ISREG(info.st_mode)) {
      if (ctx->file = fopen(file, "rb")) {
        ctx->file_name = file;
        send_command(ATTRIBUTES, ctx, (const uint8_t *) &(info.st_size), sizeof(info.st_size), EACK | ENACK | EERROR);
      }
      else {
        free(file);
        uint8_t error = PDENIED;
        send_command(ERROR, ctx, &error, 1, EACK | ENACK);
      } 
    }
    else {
      if (ctx->file) {
        fclose(ctx->file);
        ctx->file = NULL;
      }

      free(file);
      uint8_t error = NEXIST;
      send_command(ERROR, ctx, &error, 1, EACK | ENACK);
    }
  }
}

static void
server_in_error(void *_ctx, uint8_t sequence, const uint8_t * const payload, uint8_t payload_size)
{
  struct connection_context *ctx = (struct connection_context *) _ctx;

  if (sequence == ctx->last_sequence_received) {
    resend_last_command(ctx, true);
  }
  else if (ctx->expected_commands & EERROR) {
    ctx->last_sequence_received = sequence;
    ctx->cmd_timeout         = 0;
    ctx->timeout_tries       = 0;
    ctx->is_waiting_response = false;
    ctx->command             = 0;
    ctx->expected_commands   = ELS | ECD | EPUT | EGET;

    if (ctx->file) {
      fclose(ctx->file);
      ctx->file = NULL;

      if (ctx->file_name) {
        unlink(ctx->file_name);
        free(ctx->file_name);
        ctx->file_name = NULL;
      }
    }

    send_ack(ctx);
  }
} 

static void
server_in_attributes(void *_ctx, uint8_t sequence, const uint8_t * const payload, uint8_t payload_size)
{
  struct connection_context *ctx = (struct connection_context *) _ctx;

  if (sequence == ctx->last_sequence_received) {
    resend_last_command(ctx, true);
  }
  else if (ctx->expected_commands & EATTRIBUTES) {
    ctx->last_sequence_received = sequence;
    ctx->file_size = *((off_t *) payload);

    send_command(ACK, ctx, 0, 0, EDATA | ENACK | EEND | EERROR);
  }
}

static void
server_in_data(void *_ctx, uint8_t sequence, const uint8_t * const payload, uint8_t payload_size)
{
  struct connection_context *ctx = (struct connection_context *) _ctx;

  if (sequence == ctx->last_sequence_received) {
    resend_last_command(ctx, true);
  }
  else if (ctx->expected_commands & EDATA) {
    ctx->last_sequence_received = sequence;
    size_t size = fwrite(payload, 1, payload_size, ctx->file);

    if (size == 0) {
      fclose(ctx->file);
      ctx->file = NULL;

      if (ctx->file_name) {
        unlink(ctx->file_name);
        free(ctx->file_name);
        ctx->file_name = NULL;
      }

      uint8_t error = OSPACE;
      send_command(ERROR, ctx, &error, 1, EACK | ENACK);
    }
    else
      send_command(ACK, ctx, 0, 0, EDATA | ENACK | EEND | EERROR);
  }
}

static void
server_in_end(void *_ctx, uint8_t sequence, const uint8_t * const payload, uint8_t payload_size)
{
  struct connection_context *ctx = (struct connection_context *) _ctx;

  if (sequence == ctx->last_sequence_received) {
    resend_last_command(ctx, true);
  }
  else if (ctx->expected_commands & EEND) {
    ctx->last_sequence_received = sequence;
    ctx->cmd_timeout         = 0;
    ctx->timeout_tries       = 0;
    ctx->is_waiting_response = false;
    ctx->command             = 0;
    ctx->expected_commands   = ELS | ECD | EPUT | EGET;

    if (ctx->file) {
      fclose(ctx->file);
      ctx->file = NULL;
    }

    send_ack(ctx);
  }
}

static void
server_in_invalid(void *_ctx)
{
  struct connection_context *ctx = (struct connection_context *) _ctx;
  send_nack(ctx);
}

static void
register_callbacks()
{
  register_decode_callback(ACK        , &server_in_ack       );
  register_decode_callback(NACK       , &server_in_nack      );
  register_decode_callback(CD         , &server_in_cd        );
  register_decode_callback(PUT        , &server_in_put       );
  register_decode_callback(GET        , &server_in_get       );
  register_decode_callback(LS         , &server_in_ls        );
  register_decode_callback(ATTRIBUTES , &server_in_attributes);
  register_decode_callback(DATA       , &server_in_data      );
  register_decode_callback(ERROR      , &server_in_error     );
  register_decode_callback(END        , &server_in_end       );

  register_decode_error_callback(&server_in_invalid);
}

static void
perform_keepalive(uint64_t timestamp)
{
  struct connection_context *ctx, *tmp;

  list_for_each_entry_safe(ctx, tmp, &_keepalive_head.keepalive_list, keepalive_list) {
    if (ctx->keepalive_limit_time > timestamp)
      break;

    remove_context(ctx);
  }
}

static void
perform_resend_command(uint64_t timestamp)
{
  struct connection_context *ctx, *tmp;

  list_for_each_entry_safe(ctx, tmp, &_keepalive_head.keepalive_list, keepalive_list) {
    if (ctx->cmd_timeout > timestamp)
      break;

    if (ctx->timeout_tries-- == 0) {
      remove_context(ctx);
    }
    else
      resend_last_command(ctx, false);
  }
}

int
start_server(const char * const directory, uint16_t port, bool is_daemon)
{
  int optval = 1;

  struct sockaddr_in srv;
  socklen_t srv_len = sizeof(struct sockaddr_in);
 
  char buffer[IN_BUFF_MAX];
  int efd, nfds, sock_fd;
  struct epoll_event sock_event, events[MAX_FD];

  if (is_daemon && !daemonize()) {
    fprintf(stdout, "Internal error. Application cannot continue\n");
    return 1;
  }

  if (chdir(directory) != 0) {
    fprintf(stdout, "Problems to change the directory %s", strerror(errno));
    return 1;
  }
  
  if (chroot(directory) != 0) {
    fprintf(stdout, "Problems to change the directory %s", strerror(errno));
    return 1;
  }

  if ((sock_fd = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
    fprintf(stdout, "Problems to start a connection %s", strerror(errno));
    return 1;
  }

  if (!setnonblocking(sock_fd)) {
    fprintf(stdout, "Problems to start a connection %s", strerror(errno));
    return 1;
  }

  memset(&_ctx_hash, 0, sizeof(_ctx_hash));
  memset(&sock_event, 0, sizeof(struct epoll_event));

  sock_event.data.fd = sock_fd;
  sock_event.events  = EPOLLIN | EPOLLET;

#ifdef EPOLLRDHUP
  if ((efd = epoll_create1(0)) == -1) {
#else
  if ((efd = epoll_create(MAX_FD)) == -1) {
#endif
    fprintf(stdout, "Problems to start a connection %s", strerror(errno));
    return 1;
  }

  setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

  srv.sin_family      = AF_INET;
  srv.sin_port        = htons(port);
  srv.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(sock_fd, (struct sockaddr *) &srv, sizeof(srv)) == -1) {
    fprintf(stdout, "Problems to start a connection %s", strerror(errno));
    return 1;
  }

  if (epoll_ctl(efd, EPOLL_CTL_ADD, sock_fd, &sock_event) == -1) {
    fprintf(stdout, "Problems to start a connection %s", strerror(errno));
    return 1;
  }

  register_callbacks();

  INIT_LIST_HEAD(&_retries_head.retries_list);
  INIT_LIST_HEAD(&_keepalive_head.keepalive_list);

  static struct timespec ts;
  for (;;) {
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t timestamp = TIMESPEC_TO_MS(ts);

    perform_keepalive(timestamp);
    perform_resend_command(timestamp);

    if ((nfds = epoll_wait(efd, events, MAX_FD, 200)) == -1) {
      if (errno == EINTR || errno == EAGAIN)
        continue;
      else {
        fprintf(stdout, "Internal error. Application cannot continue.\n");
        return 1;
      }
    }

    int i;
    for (i = 0; i < nfds; ++i) {
      if (events[i].events) {
        ssize_t bytes_recv;
        uint16_t header;

        ssize_t msg_size = 0;
        bytes_recv = recvfrom(events[i].data.fd, &header, sizeof(header), MSG_PEEK,
          (struct sockaddr *) &srv, &srv_len);

        if (bytes_recv != sizeof(header))
          continue;

        msg_size = decode_msg_size_by_header(header);
        if (msg_size == -1)  {
          bytes_recv = recvfrom(events[i].data.fd, &header, 1, MSG_WAITALL,
            (struct sockaddr *) &srv, &srv_len);
        }

        bytes_recv = recvfrom(events[i].data.fd, buffer, msg_size, MSG_PEEK,
          (struct sockaddr *) &srv, &srv_len);

        if (bytes_recv != msg_size)
          continue;

        uint64_t to_check_key = (srv.sin_addr.s_addr << 2) + srv.sin_port;
        struct connection_context *ctx = find_context(&srv, efd);
        if (ctx)
          decode_msg(ctx, buffer);

        bytes_recv = recvfrom(events[i].data.fd, buffer, msg_size, MSG_WAITALL,
          (struct sockaddr *) &srv, &srv_len);
      }
      else {
#ifdef EPOLLRDHUP
        if (events[i].events & EPOLLRDHUP) {
#else
        if (events[i].events & EPOLLHUP) {
#endif
          epoll_ctl(efd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
          close(events[i].data.fd);
          continue;
        }
      }
    }
  }

  return 1;
}
