#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

#include <getopt.h>
#include <string.h>

#include "client.h"
#include "server.h"

static int
print_usage(const char * const app_name)
{
  fprintf(stderr, "Usage: %s [options]\n"
    "Kermit Server Implementation\n"
    "Options:\n"
    "\t-p --port=<port>\n"
    "\t-s --server\n"
    "\t-c --client=<server_address>\n"
    "\t-d --directory=<directory>\n"
    "\t-D --daemon\n",
    app_name
  );

  return 1;
}

int main(int argc, char **argv)
{
  int      ret          = 0;
  uint16_t port         = 0;
  char     *directory   = NULL;
  bool     is_daemon    = false;
  char     *server_addr = NULL;
  bool     is_server    = false;

  struct option opts[] = {
    { "port"     , 1, 0, 'p' },
    { "directory", 1, 0, 'd' },
    { "client"   , 1, 0, 'c' },
    { "daemon"   , 0, 0, 'D' },
    { "server"   , 0, 0, 's' },
    { NULL       , 0, 0, 0   }
  };

  int opt;
  while ((opt = getopt_long(argc, argv, "p:d:c:Ds", opts, NULL)) != -1) {
    switch(opt) {
      case 'p':
        port = atoi(optarg);
      break;
      case 'd':
        directory = strdup(optarg);
      break;
      case 'c':
        server_addr = strdup(optarg);
      break;
      case 's':
        is_server = true;
      break;
      case 'D':
        is_daemon = true;
      break;
      default:
        return print_usage(argv[0]);
    }
  }

  if (port == 0)
    return print_usage(argv[0]);

  if (server_addr && is_server)
    ret = print_usage(argv[0]);
  else if (server_addr)
    ret = start_client(server_addr, port);
  else if (is_server && directory)
    ret = start_server(directory, port, is_daemon);
  else
    ret = print_usage(argv[0]);

  free(directory);
  free(server_addr);

  return ret;
}
