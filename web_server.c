#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>

int main(int argc, char * argv[]) {
  // TODO port arg // NOTE: for privileged ports, sudo setcap 'cap_net_bind_service=+ep' /path/to/program
  uint16_t port = 8888;

  // listen for clients
  int server = socket(AF_INET, SOCK_STREAM, 0); if(server == -1) { perror("socket()"); exit(EXIT_FAILURE); }
  if(setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int))) { perror("setsockopt()"); exit(EXIT_FAILURE); }
  if(bind(server, (const struct sockaddr *)&(struct sockaddr_in){AF_INET, htons(port), {INADDR_ANY}}, sizeof(struct sockaddr_in))) { perror("bind()"); exit(EXIT_FAILURE); }
  if(listen(server, 0)) { perror("listen()"); exit(EXIT_FAILURE); }
  struct sockaddr_in client_addr;
  uint8_t buffer[4096];
  while(true) {
    socklen_t client_addr_len = sizeof(struct sockaddr_in);
    int client = accept(server, (struct sockaddr *)&client_addr, &client_addr_len); if(client == -1) { perror("accept()"); exit(EXIT_FAILURE); }

    // allow only the usual private IPv4 addresses
    uint8_t * ip = (uint8_t *)&client_addr.sin_addr.s_addr;
    printf("client_address: %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]);
    bool allowed_ip = false;
    allowed_ip |= ip[0] == 192 && ip[1] == 168;
    allowed_ip |= ip[0] == 127 && ip[1] == 0 && ip[2] == 0 && ip[3] == 1;
    // TODO if traffic from the internet/tor, turn on HTTPS/AUTH and turn server off on multi failed attempts
    if(!allowed_ip) {
      // TODO how to behave exactly like if there was no server?
      strcpy(buffer, "HTTP/1.1 200 OK\r\n\r\nYou suck.\r\n");
      ssize_t length = strlen(buffer);
      ssize_t sent = send(client, buffer, length, 0); if(sent != length) { if(sent == -1) perror("send()"); else fprintf(stderr, "send(): couldn't send whole message, sent only %zu.\n", sent); exit(EXIT_FAILURE); }
    } else {

      // receive
      ssize_t length = recv(client, &buffer, 4096, 0);
      if(length == -1) { perror("recv()"); exit(EXIT_FAILURE); }
      if(length == 4096) { perror("recv() didn't expect to fill buffer"); exit(EXIT_FAILURE); }

      // reply
      strcpy(buffer, "HTTP/1.1 200 OK\r\n\r\nHello, World!\r\n");
      length = strlen(buffer);
      ssize_t sent = send(client, buffer, length, 0); if(sent != length) { if(sent == -1) perror("send()"); else fprintf(stderr, "send(): couldn't send whole message, sent only %zu.\n", sent); exit(EXIT_FAILURE); }
    }

    if(shutdown(client, SHUT_RDWR)) { perror("shutdown(client)"); exit(EXIT_FAILURE); }
    if(close(client)) { perror("close(client)"); exit(EXIT_FAILURE); }
  }

  if(shutdown(server, SHUT_RDWR)) { perror("shutdown(server)"); exit(EXIT_FAILURE); }
  if(close(server)) { perror("close(server)"); exit(EXIT_FAILURE); }
  return EXIT_SUCCESS;
}
