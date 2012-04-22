#include "client.h"

#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include <dirent.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include <linux/if_ether.h>

#include <arpa/inet.h>

#include <time.h>

#include <sys/time.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/resource.h>

#include "message_encoder.h"

#define KEEPALIVE_TIMEOUT 1000
#define MAX_FD               2
#define IN_BUFF_MAX        128
#define DEFAULT_TIMEOUT    300
#define MAX_TRIES            3

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

struct client_context {
  char               cmd_line[IN_BUFF_MAX];
  struct sockaddr_in dst;
  bool               is_running;
  int                fd;

  uint8_t            sequence;
  uint8_t            last_sequence_received;
  uint8_t            last_cmd[18];
  int                size;

  char               *directory;

  int                cmd_timeout;
  int                timeout_tries;
  bool               is_waiting_response;
  uint8_t            command;
  uint32_t           expected_commands;

  off_t              file_size;
  FILE               *file;
  char               *file_name;

  uint64_t           keepalive_timeout;
  uint64_t           current_time;
};

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

static char *
client_trim(uint8_t *final_size, const char * const line)
{
  size_t len;
  char *begin, *end, *curr;

  len = strlen(line);

  begin = (char *) line;

  while (*begin && *begin == ' ')
    begin++;

  if (!len)
    goto error;

  end = curr = (char *) line + len - 1;
  while (curr > begin) {
    if (*curr == ' ') {
      *curr = 0;
      if (*end != 0)
        goto error;
      end = curr;
    }

    curr--;
  }

  len = end - begin + 1;
  if (len > 15)
    goto error;

  *final_size = len;
  return begin;
error:
  *final_size = 0;
  return NULL;
}

static int
alphasort_desc(const struct dirent **a, const struct dirent **b) {
  return strcmp((*b)->d_name,(*a)->d_name);
}

static void
send_ack(struct client_context *ctx)
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
    fprintf(stdout, "Server not answering. Application aborted\n");
    exit(1);
  }

  ctx->keepalive_timeout = ctx->current_time + KEEPALIVE_TIMEOUT;
}

static void
send_nack(struct client_context *ctx)
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
    fprintf(stdout, "Server not answering. Application aborted\n");
    exit(1);
  }

  ctx->keepalive_timeout = ctx->current_time + KEEPALIVE_TIMEOUT;
}

static void
send_keepalive(struct client_context *ctx)
{
  uint8_t msg_size;
  uint8_t msg[18];

  msg_size = encode_msg(msg, 0, 0, KEEPALIVE, 0);

  ssize_t sent_bytes;
  do {
    sent_bytes = sendto(ctx->fd, msg, msg_size, 0,
      (const struct sockaddr *) &(ctx->dst), sizeof(ctx->dst));
  } while (sent_bytes == -1 && errno == EINTR);

  if (sent_bytes == -1) {
    fprintf(stdout, "Server not answering. Application aborted\n");
    exit(1);
  }

  ctx->keepalive_timeout = ctx->current_time + KEEPALIVE_TIMEOUT;
}

static void
send_command(uint8_t cmd, struct client_context *ctx, const uint8_t * const payload, uint8_t payload_size,
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
    fprintf(stdout, "Server not answering. Application aborted\n");
    exit(1);
  }

  ctx->cmd_timeout         = ctx->current_time + DEFAULT_TIMEOUT;
  ctx->timeout_tries       = MAX_TRIES;
  ctx->is_waiting_response = true;
  ctx->command             = cmd;
  ctx->expected_commands   = expected_commands;

  ctx->keepalive_timeout = ctx->current_time + KEEPALIVE_TIMEOUT;
}

static void
resend_last_command(struct client_context *ctx, bool reset_retries)
{
  if (ctx->is_waiting_response) {
    ssize_t sent_bytes;
    do {
      sent_bytes = sendto(ctx->fd, ctx->last_cmd, ctx->size, 0,
        (const struct sockaddr *) &(ctx->dst), sizeof(ctx->dst));
    } while (sent_bytes == -1 && errno == EINTR);

    if (sent_bytes == -1) {
      fprintf(stdout, "Server not answering. Application aborted\n");
      exit(1);
    }

    ctx->is_waiting_response  = true;
    ctx->cmd_timeout          = ctx->current_time + DEFAULT_TIMEOUT;
    ctx->keepalive_timeout    = ctx->current_time + KEEPALIVE_TIMEOUT;

    if (reset_retries)
      ctx->timeout_tries = MAX_TRIES;
  }
  else
    send_ack(ctx);
}

static void client_out_get(struct client_context *ctx, const char * const line);
static void client_out_put(struct client_context *ctx, const char * const line);
static void client_help(struct client_context *ctx, const char * const line);
static void client_lls(struct client_context *ctx, const char * const line);
static void client_lcd(struct client_context *ctx, const char * const line);
static void client_out_ls(struct client_context *ctx, const char * const line);
static void client_out_cd(struct client_context *ctx, const char * const line);
static void client_quit(struct client_context *ctx, const char * const line);

static struct callbacks {
  char *cmd;
  char *help;
  void (*callback)(struct client_context *, const char * const);
} _cmd_callbacks[] = {
  { "QUIT" , "exit the program"                   , &client_quit    },
  { "CD"   , "change the server working directory", &client_out_cd  },
  { "LS"   , "list server directory contents"     , &client_out_ls  },
  { "PUT"  , "add a file to the server"           , &client_out_put },
  { "GET"  , "get a file from the server"         , &client_out_get },
  { "LLS"  , "list client directory contents"     , &client_lls     },
  { "LCD"  , "change the client working directory", &client_lcd     },
  { "HELP" , "print this information"             , &client_help    },
  { NULL   , NULL                                 , NULL            }
};

static void
client_out_invalid(const char * const line)
{
  fprintf(stdout, "Invalid command: %s\n> ", line);
  fflush(stdout);
}

static void
client_quit(struct client_context *ctx, const char * const line)
{
  if (strlen(line) == 0)
    ctx->is_running = false;
  else
    client_out_invalid(ctx->cmd_line);
}

static void
client_out_cd(struct client_context *ctx, const char * const line)
{
  if (strlen(line) < 2 || line[0] != ' ') {
    client_out_invalid(ctx->cmd_line);
    return;
  }

  uint8_t size;
  char *msg = client_trim(&size, line);

  if (msg)
    send_command(CD, ctx, msg, size, EACK | ENACK | EERROR);
  else
    client_out_invalid(ctx->cmd_line);
}

static void
client_out_ls(struct client_context *ctx, const char * const line)
{
  if (strlen(line) == 0)
    send_command(LS, ctx, 0, 0, ESHOW | ENACK | EEND | EERROR);
  else
    client_out_invalid(ctx->cmd_line);
}

static void
client_lcd(struct client_context *ctx, const char * const line)
{
  if (strlen(line) < 2 || line[0] != ' ') {
    client_out_invalid(ctx->cmd_line);
    return;
  }

  uint8_t size;
  char *msg = client_trim(&size, line);

  if (size == 1 && *msg == '.')
    goto end;

  char *old_dir = strdup(ctx->directory);

  if (size == 2 && msg[0] == '.' && msg[1] == '.') {
    size_t curr_len = strlen(ctx->directory);
    if (curr_len != 1) {
      char *tmp = ctx->directory + curr_len - 2;
      while (tmp != ctx->directory) {
        if (*tmp == '/') {
          *(tmp + 1) = 0;
          break;
        }

        tmp--;
      }

      if (tmp == ctx->directory)
        *(tmp + 1) = 0;
    }
  }
  else {
    size_t curr_len = strlen(ctx->directory);
    ctx->directory = (char *) realloc(ctx->directory, curr_len + size + 2);
    memcpy(ctx->directory + curr_len, msg, size);
    *(ctx->directory + curr_len + size) = '/';
    *(ctx->directory + curr_len + size + 1) = 0;
  }

  if (chdir(ctx->directory) == 0) {
    free(old_dir);
  }
  else {
    fprintf(stdout, "Permission denied\n");
    free(ctx->directory);
    ctx->directory = old_dir;
  }

end: 
  fprintf(stdout, "> ");
  fflush(stdout);
}

static void
client_lls(struct client_context *ctx, const char * const line)
{
  if (strlen(line) == 0) {
    struct dirent **namelist;
    int n;

    n = scandir(".", &namelist, 0, alphasort_desc);
    if (n < 0) {
      fprintf(stdout, "Permission denied\n");
    }
    else {
      while (n--) {
        fprintf(stdout, "%s\n", namelist[n]->d_name);
        free(namelist[n]);
      }

      free(namelist);
    }
    fprintf(stdout, "> ");
    fflush(stdout);
  }
  else
    client_out_invalid(line);
}

static void
client_help(struct client_context *ctx, const char * const line)
{
  if (strlen(line) == 0) {
    struct callbacks *cmds = _cmd_callbacks;
    while (cmds->cmd != NULL) {
      fprintf(stdout, "%s:\t%s\n", cmds->cmd, cmds->help);
      ++cmds;
    }


    fprintf(stdout, "> ");
    fflush(stdout);
  }
  else
    client_out_invalid(line);
}

static void
client_out_put(struct client_context *ctx, const char * const line)
{
  if (strlen(line) < 2 || line[0] != ' ') {
    client_out_invalid(ctx->cmd_line);
    return;
  }

  uint8_t size;
  char *msg = client_trim(&size, line);

  if (msg) {
    size_t curr_len = strlen(msg);
    char *file = (char *) malloc(curr_len + 3);

    memcpy(file, "./", 2);
    memcpy(file + 2, msg, curr_len);
    *(file + curr_len + 2) = 0;

    if (ctx->file = fopen(file, "rb")) {
      ctx->file_name = file;

      send_command(PUT, ctx, msg, size, EACK | ENACK | EERROR);
    }
    else {
      free(file);
      fprintf(stdout, "Permission denied\n");
      fprintf(stdout, "> ");
      fflush(stdout);
    }
  }
  else
    client_out_invalid(ctx->cmd_line);
}

static void
client_out_get(struct client_context *ctx, const char * const line)
{
  if (strlen(line) < 2 || line[0] != ' ') {
    client_out_invalid(ctx->cmd_line);
    return;
  }

  uint8_t size;
  char *msg = client_trim(&size, line);

  if (msg) {
    if (ctx->file = fopen(msg, "wb")) {
      ctx->file_name = strdup(msg);
      send_command(GET, ctx, msg, size, EATTRIBUTES | ENACK | EERROR);
    }
    else {
      fprintf(stdout, "Permission denied\n");
      fprintf(stdout, "> ");
      fflush(stdout);
    }
  }
  else
    client_out_invalid(ctx->cmd_line);
}

static void
process_command(struct client_context *ctx, const char * const cmd_line)
{
  struct callbacks *cmds = _cmd_callbacks;

  memcpy(ctx->cmd_line, cmd_line, IN_BUFF_MAX);

  while (cmds->cmd != NULL) {
    size_t len = strlen(cmds->cmd);
    if (strncasecmp(cmds->cmd, cmd_line, len) == 0) {
      cmds->callback(ctx, cmd_line + len);
      return;
    }

    ++cmds;
  }

  client_out_invalid(ctx->cmd_line);
}

static void
client_in_ack(void *_ctx, uint8_t sequence, const uint8_t * const payload, uint8_t payload_size)
{
  struct client_context *ctx = (struct client_context *) _ctx;

  if (sequence == ctx->last_sequence_received) {
    resend_last_command(ctx, true);
  }
  else if (ctx->expected_commands & EACK) {
    ctx->last_sequence_received = sequence;
    if (ctx->command == PUT) {
      struct stat info;
      if (stat(ctx->file_name, &info) == 0 && S_ISREG(info.st_mode)) {
        send_command(ATTRIBUTES, ctx, (const uint8_t *) &(info.st_size), sizeof(info.st_size), EACK | ENACK | EERROR);
      }
      else {
        if (ctx->file) {
          fclose(ctx->file);
          ctx->file = NULL;
        }

        uint8_t error = PDENIED;
        send_command(ERROR, ctx, &error, 1, EACK | ENACK);
      }
    }
    else if (ctx->command == ATTRIBUTES || ctx->command == DATA) {
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
    else {
      if (ctx->file) {
        fclose(ctx->file);
        ctx->file = NULL;
      }

      ctx->cmd_timeout         = 0;
      ctx->timeout_tries       = 0;
      ctx->is_waiting_response = false;
      ctx->command             = 0;
      ctx->expected_commands   = 0;

      fprintf(stdout, "> ");
      fflush(stdout);
    }
  }
}

static void
client_in_nack(void *_ctx, uint8_t sequence, const uint8_t * const payload, uint8_t payload_size)
{
  struct client_context *ctx = (struct client_context *) _ctx;

  if (ctx->expected_commands & ENACK) {
    ctx->last_sequence_received = sequence;
    resend_last_command(ctx, true);
  }
}

static void
client_in_error(void *_ctx, uint8_t sequence, const uint8_t * const payload, uint8_t payload_size)
{
  struct client_context *ctx = (struct client_context *) _ctx;

  if (sequence == ctx->last_sequence_received) {
    resend_last_command(ctx, true);
  }
  else if (ctx->expected_commands & EERROR) {
    ctx->last_sequence_received = sequence;
    ctx->cmd_timeout         = 0;
    ctx->timeout_tries       = 0;
    ctx->is_waiting_response = false;
    ctx->command             = 0;
    ctx->expected_commands   = 0;

    switch (*payload) {
      case NEXIST:
        fprintf(stdout, "File or directory does not exist\n");
      break;
      case PDENIED:
        fprintf(stdout, "Permission denied\n");
      break;
      case OSPACE:
        fprintf(stdout, "Not enough space to complete the request\n");
      break;
    }

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
    fprintf(stdout, "> ");
    fflush(stdout);
  }
}

static void
client_in_show(void *_ctx, uint8_t sequence, const uint8_t * const payload, uint8_t payload_size)
{
  struct client_context *ctx = (struct client_context *) _ctx;

  if (sequence == ctx->last_sequence_received) {
    resend_last_command(ctx, true);
  }
  else if (ctx->expected_commands & ESHOW) {
    ctx->last_sequence_received = sequence;
    char file[15];
    memcpy(file, payload, payload_size);
    file[payload_size] = 0;

    fprintf(stdout, "%s\n", file);

    send_command(ACK, ctx, 0, 0, ESHOW | ENACK | EEND | EERROR);
  }
}

static void
client_in_attributes(void *_ctx, uint8_t sequence, const uint8_t * const payload, uint8_t payload_size)
{
  struct client_context *ctx = (struct client_context *) _ctx;

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
client_in_data(void *_ctx, uint8_t sequence, const uint8_t * const payload, uint8_t payload_size)
{
  struct client_context *ctx = (struct client_context *) _ctx;

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
client_in_end(void *_ctx, uint8_t sequence, const uint8_t * const payload, uint8_t payload_size)
{
  struct client_context *ctx = (struct client_context *) _ctx;

  if (sequence == ctx->last_sequence_received) {
    resend_last_command(ctx, true);
  }
  else if (ctx->expected_commands & EEND) {
    ctx->last_sequence_received = sequence;
    ctx->cmd_timeout         = 0;
    ctx->timeout_tries       = 0;
    ctx->is_waiting_response = false;
    ctx->command             = 0;
    ctx->expected_commands   = 0;

    if (ctx->file) {
      fclose(ctx->file);
      ctx->file = NULL;
    }

    send_ack(ctx);

    fprintf(stdout, "> ");
    fflush(stdout);
  }
}

static void
client_in_invalid(void *_ctx)
{
  struct client_context *ctx = (struct client_context *) _ctx;
  send_nack(ctx);
}

static void
register_callbacks()
{
  register_decode_callback(ACK        , &client_in_ack       );
  register_decode_callback(NACK       , &client_in_nack      );
  register_decode_callback(SHOW       , &client_in_show      );
  register_decode_callback(ATTRIBUTES , &client_in_attributes);
  register_decode_callback(DATA       , &client_in_data      );
  register_decode_callback(ERROR      , &client_in_error     );
  register_decode_callback(END        , &client_in_end       );

  register_decode_error_callback(&client_in_invalid);
}

static void
perform_keepalive(struct client_context *ctx)
{
  if (ctx->keepalive_timeout <= ctx->current_time)
    send_keepalive(ctx);
}

static void
perform_resend_command(struct client_context *ctx)
{
  if (ctx->is_waiting_response) {
    if (ctx->cmd_timeout <= ctx->current_time) {
      if (ctx->timeout_tries-- == 0) {
        fprintf(stdout, "Server not answering. Application aborted\n");
        exit(1);
      }

      resend_last_command(ctx, false);
    }
  }
}

int
start_client(const char * const server_addr, uint16_t port)
{
  int optval = 1;
  struct client_context ctx;

  struct sockaddr_in srv;
  socklen_t srv_len = sizeof(struct sockaddr_in);

  char buffer[IN_BUFF_MAX];
  int efd, nfds, sock_fd;
  struct epoll_event in_event, sock_event, events[MAX_FD];

  memset(&ctx, 0, sizeof(struct client_context));

  if ((sock_fd = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
    fprintf(stdout, "Problems to start a connection %s", strerror(errno));
    return 1;
  }

  if (!setnonblocking(STDIN_FILENO) || !setnonblocking(sock_fd)) {
    fprintf(stdout, "Problems to start a connection %s", strerror(errno));
    return 1;
  }

  memset(&in_event  , 0, sizeof(struct epoll_event));
  memset(&sock_event, 0, sizeof(struct epoll_event));

  in_event.data.fd = STDIN_FILENO;
  in_event.events  = EPOLLIN | EPOLLET;

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

  if (epoll_ctl(efd, EPOLL_CTL_ADD, STDIN_FILENO, &in_event) == -1) {
    fprintf(stdout, "Problems to start a connection %s", strerror(errno));
    return 1;
  }

  if (epoll_ctl(efd, EPOLL_CTL_ADD, sock_fd, &sock_event) == -1) {
    fprintf(stdout, "Problems to start a connection %s", strerror(errno));
    return 1;
  }

  setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

  srv.sin_family      = AF_INET;
  srv.sin_port        = 0;
  srv.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(sock_fd, (struct sockaddr *) &srv, sizeof(srv)) == -1) {
    fprintf(stdout, "Problems to start a connection %s", strerror(errno));
    return 1;
  }

  ctx.fd = sock_fd;

  ctx.dst.sin_family      = AF_INET;
  ctx.dst.sin_port        = htons(port);
  ctx.dst.sin_addr.s_addr = inet_addr(server_addr);

  ctx.directory = (char *) malloc(PATH_MAX);

  ctx.directory = getcwd(ctx.directory, PATH_MAX);
  size_t dir_len = strlen(ctx.directory);
  if (dir_len < PATH_MAX - 1 && ctx.directory[dir_len -1] != '/') {
    ctx.directory[dir_len]     = '/';
    ctx.directory[dir_len + 1] = 0;
  }

  register_callbacks();

  fprintf(stdout, "> ");
  fflush(stdout);

  ctx.is_running = true;

  static struct timespec ts;
  for (;;) {
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ctx.current_time = TIMESPEC_TO_MS(ts);

    perform_keepalive(&ctx);
    perform_resend_command(&ctx);

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
      if (events[i].data.fd == STDIN_FILENO && events[i].events) {
        int j = 0;
        char ch;
        while (read(STDIN_FILENO, &ch, 1) != -1) {
          if (j < IN_BUFF_MAX - 1 && ch != '\r' && ch != '\n')
            buffer[j++] = ch;
        }

        fflush(stdin);
        buffer[j] = 0;

        if (!ctx.is_waiting_response) {
          process_command(&ctx, buffer);
          if (!ctx.is_running) {
            shutdown(sock_fd, SHUT_WR);
            close(sock_fd);
            close(efd);
            return 0;
          }
        }

        fflush(stdout);
      }
      else if (events[i].data.fd == sock_fd && events[i].events) {
        ssize_t bytes_recv;
        uint16_t header;

        ssize_t msg_size = 0;
        bytes_recv = recvfrom(events[i].data.fd, &header, sizeof(header), MSG_PEEK,
          (struct sockaddr *) &ctx.dst, &srv_len);

        if (bytes_recv != sizeof(header))
          continue;

        msg_size = decode_msg_size_by_header(header);
        if (msg_size == -1)  {
          bytes_recv = recvfrom(events[i].data.fd, &header, 1, MSG_WAITALL,
            (struct sockaddr *) &ctx.dst, &srv_len);
        }

        bytes_recv = recvfrom(events[i].data.fd, buffer, msg_size, MSG_PEEK,
          (struct sockaddr *) &ctx.dst, &srv_len);

        if (bytes_recv != msg_size)
          continue;

        decode_msg(&ctx, buffer);

        bytes_recv = recvfrom(events[i].data.fd, buffer, msg_size, MSG_WAITALL,
          (struct sockaddr *) &ctx.dst, &srv_len);
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
