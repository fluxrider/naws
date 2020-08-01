#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/sendfile.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#define HTTP_404_HEADER "HTTP/1.1 404 Not Found\r\nContent-Type:text/html;charset=utf-8\r\n\r\n"
#define HTTP_404_HEADER_LEN (sizeof(HTTP_404_HEADER) - 1)

uint32_t hash_djb2(const char * s) {
  uint32_t hash = 5381;
  while(*s) hash = ((hash << 5) + hash) + *s++;
  return hash;
}

#define hash_djb2_css 193488718
#define hash_djb2_js 5863522
#define hash_djb2_html 2090341082
#define hash_djb2_png 193502698
#define hash_djb2_webp 2090863443
#define hash_djb2_jpg 193496230
#define hash_djb2_jpeg 2090408331
#define hash_djb2_svg 193506229
#define hash_djb2_epub 2090229169
#define hash_djb2_mobi 2090514956
#define hash_djb2_mp4 193499446
#define hash_djb2_ttf 193507251
#define hash_djb2_py 5863726
#define hash_djb2_ 5381

bool send_static_header(int client, const char * ext) {
  const char * mime;
  switch(hash_djb2(ext)) {
    case hash_djb2_css: mime = "text/css"; break;
    case hash_djb2_js: mime = "application/javascript"; break;
    case hash_djb2_html: mime = "text/html;charset=utf-8"; break;
    case hash_djb2_png: mime = "image/png"; break;
    case hash_djb2_webp: mime = "image/webp"; break;
    case hash_djb2_jpg:
    case hash_djb2_jpeg: mime = "image/jpeg"; break;
    case hash_djb2_svg: mime = "image/svg+xml"; break;
    case hash_djb2_epub: mime = "application/epub+zip"; break;
    case hash_djb2_mobi: mime = "application/x-mobipocket-ebook"; break;
    case hash_djb2_mp4: mime = "video/mp4"; break;
    case hash_djb2_ttf: mime = "application/x-font-ttf"; break;
    default: return false;
  }
  char buffer[96];
  size_t length = sprintf(buffer, "HTTP/1.1 200 OK\r\nContent-Type:%s\r\n\r\n", mime);
  ssize_t sent = send(client, buffer, length, MSG_MORE); if(sent != length) { if(sent == -1) perror("send(send_static_header)"); else fprintf(stderr, "send(send_static_header(%s)): couldn't send whole message, sent only %zu.\n", ext, sent); exit(EXIT_FAILURE); }
  return true;
}

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
    int client = accept(server, (struct sockaddr *)&client_addr, &(socklen_t){sizeof(struct sockaddr_in)}); if(client == -1) { perror("accept()"); exit(EXIT_FAILURE); }

    // allow only the usual private IPv4 addresses
    uint8_t * ip = (uint8_t *)&client_addr.sin_addr.s_addr;
    printf("ACCESS client_address: %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]);
    bool allowed_ip = false;
    allowed_ip |= ip[0] == 192 && ip[1] == 168;
    allowed_ip |= ip[0] == 127 && ip[1] == 0 && ip[2] == 0 && ip[3] == 1;
    // TODO if traffic from the internet/tor, turn on HTTPS/AUTH and turn server off on multi failed attempts
    if(!allowed_ip) {
      // TODO would it be possible to behave exactly like if there was no server? filter ip with SO_ATTACH_BPF?
      { ssize_t sent = send(client, HTTP_404_HEADER, HTTP_404_HEADER_LEN, MSG_MORE); if(sent != HTTP_404_HEADER_LEN) { if(sent == -1) perror("send()"); else fprintf(stderr, "send(): couldn't send whole message, sent only %zu.\n", sent); exit(EXIT_FAILURE); } }
      int file = open("404.html", O_RDONLY); if(file == -1) { perror("open(404.html)"); exit(EXIT_FAILURE); } struct stat file_stat; if(fstat(file, &file_stat)) { perror("fstat(404.html)"); exit(EXIT_FAILURE); }
      { ssize_t sent = sendfile(client, file, NULL, file_stat.st_size); if(sent != file_stat.st_size) { if(sent == -1) perror("sendfile()"); else fprintf(stderr, "sendfile(): couldn't send whole message, sent only %zu.\n", sent); exit(EXIT_FAILURE); } }
      if(close(file)) { perror("close(404.html)"); exit(EXIT_FAILURE); }
    } else {

      // receive
      ssize_t length = recv(client, &buffer, 4096, 0); if(length == -1) { perror("recv()"); exit(EXIT_FAILURE); } if(length == 4096) { perror("recv() didn't expect to fill buffer"); exit(EXIT_FAILURE); }

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
