// Copyright 2020 David Lareau. This program is free software under the terms of the GPL-3.0-or-later.
// gcc web_server.c $(pkg-config --libs --cflags libsodium) -lpthread && ./a.out demos/sanity_test 8888 8889
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
#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <errno.h>
#include <stdarg.h>
#include <sodium.h>
#include <time.h>
#include <inttypes.h>
#include <pthread.h>

// -- Utils --

static bool starts_with(const char * s, const char * start) {
  return strncmp(start, s, strlen(start)) == 0;
}

static uint64_t get_time_ns() {
  struct timespec spec;
  if(clock_gettime(CLOCK_REALTIME, &spec)) { perror("clock_gettime"); exit(EXIT_FAILURE); }
  uint64_t ns = spec.tv_nsec; ns += spec.tv_sec * UINT64_C(1000000000); return ns;
}

static void load_file(const char * path, uint8_t * buffer, size_t length, bool securish) {
  int file = open(path, O_RDONLY); if(file == -1) { perror("open()"); fprintf(stderr, "path %s\n", path); exit(EXIT_FAILURE); }
  ssize_t n = read(file, buffer, length); if(n == -1) { perror("read()"); fprintf(stderr, "path %s\n", path); exit(EXIT_FAILURE); }
  if(n != length) { fprintf(stderr, "read(%s) wasn't full length %zu but %zd\n", path, length, n); if(securish) explicit_bzero(buffer, length); exit(EXIT_FAILURE); }
  if(close(file)) { if(securish) explicit_bzero(buffer, length); perror("close()"); fprintf(stderr, "path %s\n", path); exit(EXIT_FAILURE); }
}

// transform children end signal into a file descriptor (so I can use poll() with it)
int setup_signalfd() {
  sigset_t mask; sigemptyset(&mask); sigaddset(&mask, SIGCHLD);
  if(sigprocmask(SIG_BLOCK, &mask, NULL) == -1) { perror("sigprocmask"); exit(EXIT_FAILURE); }
  return signalfd(-1, &mask, SFD_CLOEXEC);
}

int prep_server_socket(struct pollfd * sockets, size_t * sockets_size, uint16_t port, int backlog) {
  int server = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0); if(server == -1) { perror("socket()"); exit(EXIT_FAILURE); }
  if(setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int))) { perror("setsockopt()"); exit(EXIT_FAILURE); }
  if(bind(server, (const struct sockaddr *)&(struct sockaddr_in){AF_INET, htons(port), {INADDR_ANY}}, sizeof(struct sockaddr_in))) {
    perror("bind(server)");
    if(port < 1024) fprintf(stderr, "for privileged ports, ensure capability is set\nsudo setcap 'cap_net_bind_service=+ep' /path/to/program\n");
    exit(EXIT_FAILURE);
  }
  if(listen(server, backlog)) { perror("listen()"); exit(EXIT_FAILURE); }
  sockets[*sockets_size].fd = server;
  sockets[*sockets_size].events = POLLIN;
  (*sockets_size)++;
  return server;
}

// send a file to a socket, but search and replace a few things as we go
// note: for performance reasons, only the first occurence will be replaced
// side effect: the arrays from/to will be modified in place for performance reasons as well
void send_template_file(int socket, int file, const char * from[], const char * to[], int n) {
  size_t max_from_len = 0; for(int i = 0; i < n; i++) { size_t len = strlen(from[i]); if(len > max_from_len) max_from_len = len; }
  size_t K = 1024;
  size_t buffer_cap = K + max_from_len;
  uint8_t buffer[buffer_cap + 1];
  char * found[n];
  size_t readn = read(file, buffer, buffer_cap);
  size_t shifted = 0;
  while(readn) {
    if(readn == -1) { perror("read(send_template_file)"); exit(EXIT_FAILURE); }
    readn += shifted;
    buffer[readn] = '\0';
    // search for the substring
    for(int i = 0; i < n; i++) { found[i] = strstr(buffer, from[i]); }
    // sort the findings (side effect: reorder from/to) [but NULL is considered the highest instead of lowest value]
    for(int i = 1; i < n; i++) {
      char * x = found[i];
      const char * f = from[i];
      const char * t = to[i];
      int j;
      for(j = i - 1; j >= 0 && ((found[j] > x && x) || !found[j]); j--) {
        found[j+1] = found[j];
        from[j+1] = from[j];
        to[j+1] = to[j];
      }
      found[j+1] = x;
      from[j+1] = f;
      to[j+1] = t;
    }
    // send bytes up to each find, and their to[] entry
    uint8_t * head = &buffer[0];
    while(n > 0 && found[0]) {
      size_t count = (uint8_t *)found[0] - head;
      ssize_t sent;
      sent = send(socket, head, count, MSG_MORE); if(sent != count) { if(sent == -1) perror("send(send_template_file)"); else fprintf(stderr, "send(send_template_file): couldn't send whole message, sent only %zu.\n", sent); exit(EXIT_FAILURE); }
      readn -= sent;
      size_t len = strlen(to[0]);
      sent = send(socket, to[0], len, MSG_MORE); if(sent != len) { if(sent == -1) perror("send(send_template_file)"); else fprintf(stderr, "send(send_template_file): couldn't send whole message, sent only %zu.\n", sent); exit(EXIT_FAILURE); }
      len = strlen(from[0]);
      head = found[0] + len;
      readn -= len;
      // erase from/to entry
      for(int i = 1; i < n; i++) {
        found[i-1] = found[i];
        from[i-1] = from[i];
        to[i-1] = to[i];
      }
      n--;
    }
    // send rest of the bytes (except the overflow of partial match if it's still there and we still care)
    if(n == 0) {
      ssize_t sent = send(socket, head, readn, MSG_MORE); if(sent != readn) { if(sent == -1) perror("send(send_template_file)"); else fprintf(stderr, "send(send_template_file): couldn't send whole message, sent only %zu.\n", sent); exit(EXIT_FAILURE); }
      readn = read(file, buffer, buffer_cap);
      shifted = 0;
    } else {
      if(readn > max_from_len) {
        ssize_t sent = send(socket, head, readn - max_from_len, MSG_MORE); if(sent != readn - max_from_len) { if(sent == -1) perror("send(send_template_file)"); else fprintf(stderr, "send(send_template_file): couldn't send whole message, sent only %zu.\n", sent); exit(EXIT_FAILURE); }
        head += readn - max_from_len;
        readn = max_from_len;
      }
      // shift overflow and read up to K more bytes
      if(readn > 0) memmove(buffer, head, readn);
      shifted = readn;
      readn = read(file, buffer + readn, buffer_cap - readn);
    }
  }
  // send what is left in buffer (the max_from_len), or send nothing
  ssize_t sent = send(socket, buffer, shifted, 0); if(sent != shifted) { if(sent == -1) perror("send(send_template_file)"); else fprintf(stderr, "send(send_template_file): couldn't send whole message, sent only %zu.\n", sent); exit(EXIT_FAILURE); }
}

// C workaround to switch on string (i.e. hash them)
uint32_t hash_djb2(const char * s) { uint32_t hash = 5381; while(*s) hash = ((hash << 5) + hash) + *s++; return hash; }
// static files
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
// program files
#define hash_djb2_py 5863726
#define hash_djb2_ 5381
// anything else gets a 404

// -- Web Server --

// mime type for various static files
bool send_static_header(int client, uint32_t hash_djb2_ext) {
  const char * mime;
  switch(hash_djb2_ext) {
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
  ssize_t sent = send(client, buffer, length, MSG_MORE); if(sent != length) { if(sent == -1) perror("send(send_static_header)"); else fprintf(stderr, "send(send_static_header(%u)): couldn't send whole message, sent only %zu.\n", hash_djb2_ext, sent); exit(EXIT_FAILURE); }
  return true;
}

#define HTTP_404_HEADER "HTTP/1.1 404 Not Found\r\nContent-Type:text/html;charset=utf-8\r\n\r\n"
#define HTTP_404_HEADER_LEN (sizeof(HTTP_404_HEADER) - 1)
#define HTTP_500_HEADER "HTTP/1.1 500 Internal Server Error\r\nContent-Type:text/html;charset=utf-8\r\n\r\n"
#define HTTP_500_HEADER_LEN (sizeof(HTTP_500_HEADER) - 1)
#define HTTP_200_HEADER "HTTP/1.1 200 OK\r\n"
#define HTTP_200_HEADER_LEN (sizeof(HTTP_200_HEADER) - 1)

bool do404(int client) {
  ssize_t sent = send(client, HTTP_404_HEADER, HTTP_404_HEADER_LEN, MSG_MORE); if(sent != HTTP_404_HEADER_LEN) { if(sent == -1) perror("send()"); else fprintf(stderr, "send(): couldn't send whole message, sent only %zu.\n", sent); return false; }
  int file = open("naws/404.inc", O_RDONLY); if(file == -1) { perror("open(404.inc)"); exit(EXIT_FAILURE); } struct stat file_stat; if(fstat(file, &file_stat)) { perror("fstat(404.inc)"); exit(EXIT_FAILURE); }
  sent = sendfile(client, file, NULL, file_stat.st_size); if(sent != file_stat.st_size) { if(sent == -1) perror("sendfile()"); else fprintf(stderr, "sendfile(404): couldn't send whole message, sent only %zu.\n", sent); close(file); return false; }
  if(close(file)) { perror("close(404.inc)"); exit(EXIT_FAILURE); }
}

// thread
#define buffer_capacity 8191
#define thread_max 256
struct thread_data {
  uint8_t * buffer;
  size_t child_stdout_buffer_capacity;
  uint8_t * child_stdout_buffer;
  bool in_use;
  int client;
  int thread_id;
  bool private_network_client;
};

static void * thread_routine(void * vargp);
static int sigchld_fd;

// main
int main(int argc, char * argv[]) {
  if(argc < 3) { fprintf(stderr, "usage: naws root-folder private_port [tor_port]\nexample: naws . 8888 8889\n"); exit(EXIT_FAILURE); }
  if(setvbuf(stdout, NULL, _IOLBF, 0)) { perror("setvbuf"); exit(EXIT_FAILURE); };
  sigchld_fd = setup_signalfd(); if(sigchld_fd == -1) { perror("signalfd()"); exit(EXIT_FAILURE); }
  srandom(time(0));
  char * strtol_endptr;

  // args
  if(chdir(argv[1])) { perror("chdir(root)"); exit(EXIT_FAILURE); }
  uint16_t private_port = strtol(argv[2], &strtol_endptr, 10); if(*strtol_endptr) { fprintf(stderr, "could not parse private port %s\n", argv[2]); exit(EXIT_FAILURE); }
  uint16_t tor_port = 0;
  if(argc == 4) { tor_port = strtol(argv[3], &strtol_endptr, 10); if(*strtol_endptr) { fprintf(stderr, "could not parse tor port %s\n", argv[3]); exit(EXIT_FAILURE); } }

  // setup sockets (for private network port and tor network port)
  struct pollfd sockets[2];
  size_t sockets_size = 0;
  // in this context, the private server is meant for local network traffic only, no credentials are asked for traffic on this port
  prep_server_socket(sockets, &sockets_size, private_port, thread_max / 2);
  // in this context, what I call the tor server is a port that only accepts localhost connections
  // as if torrc is setup like: HiddenServicePort 80 127.0.0.1:12345 where 12345 is the tor_port
  // I later assume end-to-end encryption on this port, so that asking for credentials over http is sensical.
  if(tor_port) prep_server_socket(sockets, &sockets_size, tor_port, thread_max / 2);

  // listen for clients
  struct sockaddr_in client_addr;
  struct thread_data * thread_data = calloc(thread_max, sizeof(struct thread_data));
  while(true) {
    int socked_polled = poll(sockets, sockets_size, -1); if(socked_polled == -1) { perror("poll()"); exit(EXIT_FAILURE); }
    int client = -1;
    bool private_network_client = false;
    if(sockets[0].revents & POLLIN) {
      printf("ACCESS private network request\n");
      client = accept(sockets[0].fd, (struct sockaddr *)&client_addr, &(socklen_t){sizeof(struct sockaddr_in)}); if(client == -1) { perror("accept(private)"); continue; }
      private_network_client = true;
    } else if(sockets[1].revents & POLLIN) {
      printf("ACCESS tor network request\n");
      client = accept(sockets[1].fd, (struct sockaddr *)&client_addr, &(socklen_t){sizeof(struct sockaddr_in)}); if(client == -1) { perror("accept(tor)"); continue; }
    }
    if(client == -1) continue;
    
    // allow only the usual private IPv4 addresses
    uint8_t * ip = (uint8_t *)&client_addr.sin_addr.s_addr;
    bool allowed_ip = false;
    allowed_ip |= ip[0] == 127 && ip[1] == 0 && ip[2] == 0 && ip[3] == 1;
    if(private_network_client) allowed_ip |= ip[0] == 192 && ip[1] == 168;
    if(!allowed_ip) {
      fprintf(stderr, "client_address %u.%u.%u.%u was denied access (private=%d)\n", ip[0], ip[1], ip[2], ip[3], private_network_client);
      do404(client);
      close(client);
      continue;
    }
    // TODO would it be possible to behave exactly like if there was no server? filter ip with SO_ATTACH_BPF?

    // pick the free thread data with the largest buffer
    struct thread_data * data = NULL;
    for(int i = 0; i < thread_max; i++) {
      if(!thread_data[i].in_use) {
        if(data == NULL || data->child_stdout_buffer_capacity < thread_data[i].child_stdout_buffer_capacity) {
          data = &thread_data[i];
          data->thread_id = i;
        }
      }
    }
    if(data == NULL) { fprintf(stderr, "could not find any free thread data\n"); close(client); continue; }
    data->in_use = true;
    data->client = client;
    data->private_network_client = private_network_client;

    // start thread
    printf("start thread\n");
    pthread_t thread;
    int ret = pthread_create(&thread, NULL, thread_routine, data); if(ret) { fprintf(stderr, "could not start thread %d %s\n", ret, strerror(ret)); exit(EXIT_FAILURE); }
  }

  return EXIT_SUCCESS;
}

void ensure_scratch_and_child_stdout_buffer(uint8_t ** child_stdout_buffer, size_t * child_stdout_buffer_capacity) {
  if(*child_stdout_buffer_capacity) return;
  *child_stdout_buffer_capacity = 10 * 1024;
  *child_stdout_buffer = realloc(NULL, *child_stdout_buffer_capacity);
  memcpy(*child_stdout_buffer, HTTP_200_HEADER, HTTP_200_HEADER_LEN);
}

static void * thread_routine(void * vargp) {
  struct thread_data * t = vargp;
  printf("thread started %d\n", t->thread_id);
  if(!t->buffer) t->buffer = malloc(buffer_capacity + 1);
  uint8_t * buffer = t->buffer;
  const int client = t->client;

  // receive
  ssize_t length = recv(client, &buffer, buffer_capacity, 0); if(length == -1) { perror("recv()"); goto abort_client; }
  printf("recv() %zd bytes\n", length);
  printf("cap %d\n", buffer_capacity);
  buffer[length] = '\0';
  printf("cap %d\n", buffer_capacity);
  printf("recv() %zd bytes\n", length);
  if(length < 4) { printf("recv() %zd bytes\n", length); goto abort_client; }
  /*
  if(strncmp(buffer, "GET ", 4)) {

    // tls
    int index = 0;
    uint8_t tls_plaintext_type = buffer[index++];
    uint16_t tls_plaintext_legacy_record_version = (buffer[index] << 8) | buffer[index+1]; index += 2;
    uint16_t tls_plaintext_length = (buffer[index] << 8) | buffer[index+1]; index += 2;
    printf("tls_plaintext_type: %u\n", tls_plaintext_type);
    printf("tls_plaintext_legacy_record_version: 0x%x\n", tls_plaintext_legacy_record_version);
    printf("tls_plaintext_length: %u\n", tls_plaintext_length);
    switch(tls_plaintext_type) {
      // tls handshake
      case 22: {
        uint8_t tls_msg_type = buffer[index++];
        uint32_t tls_msg_length = (buffer[index] << 16) | (buffer[index+1] << 8) | buffer[index+2]; index += 3;
        printf("tls_msg_type: %u\n", tls_msg_type);
        printf("tls_msg_length: %u\n", tls_msg_length);
        switch(tls_msg_type) {
          // client hello
          case 1: {
            uint16_t legacy_version = (buffer[index] << 8) | buffer[index+1]; index += 2;
            uint8_t * random = &buffer[index]; index += 32;
            uint8_t legacy_session_id = buffer[index++];
            uint8_t * cipher_suites = &buffer[index]; index += 2;
            uint8_t legacy_compression_methods = buffer[index++];
            uint16_t extensions = (buffer[index] << 8) | buffer[index+1]; index += 2;
            printf("legacy_version: 0x%x\n", legacy_version);
            printf("legacy_session_id: %u\n", legacy_session_id);
            printf("cipher_suites: %u %u\n", cipher_suites[0], cipher_suites[1]);
            printf("legacy_compression_methods: %u\n", legacy_compression_methods);
            printf("extensions: %u\n", extensions);
            break; }
        }
        break; }
    }
  }
  */

  // handle GET
  // get uri and query_string
  char * query_string = NULL;
  char * uri = &buffer[4];
  char * the_rest;
  {
    int i = 4;
    while(buffer[i]) {
      switch(buffer[i]) {
        // no ".." allowed
        case '.':
          if(buffer[i+1] != '.') { i++; break; }
          // fall through
        // unexpected end of line
        case '\r':
        case '\n':
          fprintf(stderr, "WARNING error parsing request-uri\n%s\n", buffer);
          goto encountered_problem;
        case ' ':
          buffer[i] = '\0';
          break;
        case '?':
          if(!query_string) {
            buffer[i] = '\0';
            query_string = &buffer[i+1];
          }
          // fall through
        default:
          i++;
      }
    }
    the_rest = &buffer[i+1];
  }
  if(!query_string) query_string = "";
  printf("ACCESS uri: %s query: %s\n", uri, query_string);
  if(uri[0] != '/') goto encountered_problem;
  
  // decode uri in-place
  {
    int i = 0;
    int j = 0;
    while(uri[i]) {
      int c = uri[i++];
      switch(c) {
        case '+': c = ' '; break;
        case '%':
          if(sscanf(&uri[i], "%2x", &c) != 1) { fprintf(stderr, "WARNING error decoding request-uri\n%s\n", buffer); goto encountered_problem; }
          i += 2;
          break;
      }
      uri[j++] = c;
    }
    uri[j] = '\0';
  }
  
  char * filename = strrchr(uri, '/') + 1;
  do { uri += 1; } while(uri[0] == '/');
  
  // figure out filename extension
  const char * ext = strrchr(filename, '.');
  if(!ext) ext = ""; else ext += 1;
  //printf("uri: %s\n", uri);
  //printf("filename: %s\n", filename);
  //printf("ext: %s\n", ext);
  
  // a few files in naws (e.g. js) would be normally served. Don't allow poking at private files like that
  if(starts_with(uri, "naws/")) goto encountered_problem;
  
  // auth
  if(!t->private_network_client && strcmp(uri, "ricmoo.scrypt.with_libs.js") && strcmp(uri, "sodium.js")) {
    //printf("AUTH\n");
    //printf("%s\n", the_rest);
    // TODO parse cookie
    char * cookie_username_base64 = NULL;
    char * cookie_proof_base64 = NULL;
    char * cookie_nonce_base64 = NULL;
    while(*the_rest) {
      if(starts_with(the_rest, "\nCookie: ")) {
        //printf("Parsing cookie line\n");
        bool last_token = false;
        char * token = the_rest + 9;
        do {
          char * p = token; while(*p && *p != ';' && *p != '\n' && *p != '\r') p++;
          last_token = !*p || *p == '\n' || *p == '\r';
          *p = '\0';
          //printf("cookie token: %s\n", token);
          if(starts_with(token, "nasm_username=")) cookie_username_base64 = strchr(token, '=');
          else if(starts_with(token, "nasm_proof=")) cookie_proof_base64 = strchr(token, '=');
          else if(starts_with(token, "nasm_proof_nonce=")) cookie_nonce_base64 = strchr(token, '=');
          token = p + 1;
          while(*token && *token == ' ') token++;
        } while(!last_token);
        break;
      }
      the_rest++;
    }
    if(cookie_username_base64 && cookie_proof_base64 && cookie_nonce_base64) {
      cookie_username_base64++;
      cookie_proof_base64++;
      cookie_nonce_base64++;
      // decode base64 username
      unsigned char cookie_username[128+1]; size_t cookie_username_len;
      if(sodium_base642bin(cookie_username, 128, cookie_username_base64, strlen(cookie_username_base64), NULL, &cookie_username_len, NULL, sodium_base64_VARIANT_ORIGINAL)) { fprintf(stderr, "ERROR AUTH sodium_base642bin(cookie_username)\n"); goto auth_form; }
      cookie_username[cookie_username_len] = '\0';
      // is username legal? (i.e. no slash allowed)
      if(strchr(cookie_username, '/')) { fprintf(stderr, "ERROR AUTH illegal name %s\n", cookie_username); goto auth_form; }
      // do we have a user by this name?
      ensure_scratch_and_child_stdout_buffer(&t->child_stdout_buffer, &t->child_stdout_buffer_capacity);
      char * tmp_buffer = t->child_stdout_buffer + HTTP_200_HEADER_LEN;
      sprintf(tmp_buffer, "naws/users/%s.key", cookie_username);
      if(access(tmp_buffer, R_OK)) { fprintf(stderr, "ERROR AUTH user does not exist %s\n", cookie_username); goto auth_form; }
      // load user's public key
      unsigned char user_public_key[crypto_box_PUBLICKEYBYTES];
      load_file(tmp_buffer, user_public_key, crypto_box_PUBLICKEYBYTES, false);
      // decode base64 nonce + proof
      unsigned char cookie_nonce[crypto_box_NONCEBYTES]; size_t cookie_nonce_len;
      if(sodium_base642bin(cookie_nonce, crypto_box_NONCEBYTES, cookie_nonce_base64, strlen(cookie_nonce_base64), NULL, &cookie_nonce_len, NULL, sodium_base64_VARIANT_ORIGINAL)) { fprintf(stderr, "ERROR AUTH sodium_base642bin(cookie_nonce_base64) [%s]\n", cookie_nonce_base64); goto auth_form; }
      if(cookie_nonce_len != crypto_box_NONCEBYTES) { fprintf(stderr, "ERROR AUTH nonce wrong length\n"); goto auth_form; }
      size_t coded_proof_size = crypto_box_MACBYTES + crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES + sizeof(uint64_t);
      unsigned char cookie_proof[coded_proof_size]; size_t cookie_proof_len;
      if(sodium_base642bin(cookie_proof, 1024, cookie_proof_base64, strlen(cookie_proof_base64), NULL, &cookie_proof_len, NULL, sodium_base64_VARIANT_ORIGINAL)) { fprintf(stderr, "ERROR AUTH sodium_base642bin(cookie_proof_base64)\n"); goto auth_form; }
      if(cookie_proof_len != coded_proof_size) {
        fprintf(stderr, "ERROR AUTH cookie_proof_len is wrong. Now that is weird. Is %zu not %zu.\n", cookie_proof_len, coded_proof_size);
        goto hacking_attempt_detected;
      }
      // load server's private key
      unsigned char server_secret_key[crypto_box_SECRETKEYBYTES];
      load_file("naws/secret.key", server_secret_key, crypto_box_SECRETKEYBYTES, true);
      // can we decrypt the proof?
      unsigned char coded_ns_with_nonce[crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES + sizeof(uint64_t)];
      if(crypto_box_open_easy(coded_ns_with_nonce, cookie_proof, cookie_proof_len, cookie_nonce, user_public_key, server_secret_key)) { explicit_bzero(server_secret_key, crypto_box_SECRETKEYBYTES); fprintf(stderr, "WARNING could not decrypt proof. Foulplay or did server change key recently?\n"); goto auth_form; }
      explicit_bzero(server_secret_key, crypto_box_SECRETKEYBYTES);
      // can we decrypt the secret server message (i.e. encrypted by server timestamp)?
      unsigned char * nonce = coded_ns_with_nonce;
      unsigned char * coded_ns = coded_ns_with_nonce + crypto_secretbox_NONCEBYTES;
      uint64_t ns;
      unsigned char server_symmetric_key[crypto_secretbox_KEYBYTES];
      load_file("naws/symmetric.key", server_symmetric_key, crypto_secretbox_KEYBYTES, true);
      if(crypto_secretbox_open_easy((unsigned char *)&ns, coded_ns, crypto_secretbox_MACBYTES + sizeof(ns), nonce, server_symmetric_key) != 0) { explicit_bzero(server_symmetric_key, crypto_secretbox_KEYBYTES); fprintf(stderr, "ERROR AUTH Could not decrypt timestamp. Did I change the server symmetric key recently?\n"); goto auth_form; }
      explicit_bzero(server_symmetric_key, crypto_secretbox_KEYBYTES);
      // is the timestamp expired?
      uint64_t two_days_ns = 24 * 60 * 60 * UINT64_C(1000000000);
      if(ns + two_days_ns < get_time_ns()) { printf("Expired time was %" PRIu64 " now is %" PRIu64 "\n", ns, get_time_ns()); goto auth_form; }
      goto good_auth;
    } else {
      printf("no cookie found in header\n");
    }
    goto auth_form;
  }
  good_auth:

  // verify access of local_uri
  if(access(uri, R_OK)) {
    // double check it's not a resource from /naws/401/, allow those
    ensure_scratch_and_child_stdout_buffer(&t->child_stdout_buffer, &t->child_stdout_buffer_capacity);
    sprintf(t->child_stdout_buffer + HTTP_200_HEADER_LEN, "naws/401/%s", uri);
    if(access(t->child_stdout_buffer + HTTP_200_HEADER_LEN, R_OK)) goto encountered_problem;
    uri = t->child_stdout_buffer + HTTP_200_HEADER_LEN;
  }
  
  // try sending as static file
  const uint32_t hash_djb2_ext = hash_djb2(ext);
  if(send_static_header(client, hash_djb2_ext)) {
    int file = open(uri, O_RDONLY); if(file == -1) { perror("open(uri)"); exit(EXIT_FAILURE); } struct stat file_stat; if(fstat(file, &file_stat)) { perror("fstat(uri)"); exit(EXIT_FAILURE); }
    { ssize_t sent = sendfile(client, file, NULL, file_stat.st_size); if(sent != file_stat.st_size) { if(sent == -1) perror("sendfile(uri)"); else fprintf(stderr, "sendfile(uri): couldn't send whole message, sent only %zu.\n", sent); close(file); goto abort_client; } }
    if(close(file)) { perror("close(uri)"); exit(EXIT_FAILURE); }
  } else {
    // if a program, fork and run
    switch(hash_djb2_ext) {
      case hash_djb2_:
        // note: I can't rely on execute permission so I don't bother testing here
        { struct stat uri_stat; if(!stat(uri, &uri_stat) && S_ISDIR(uri_stat.st_mode)) goto encountered_problem; }
        break;
      case hash_djb2_py:
        break;
      default: goto encountered_problem;
    }
    int pipe_err[2], pipe_out[2];
    if(pipe(pipe_err) || pipe(pipe_out)) { perror("pipe()"); exit(EXIT_FAILURE); }
    pid_t pid = fork(); 
    // child
    if(!pid) {
      if(close(0)) { perror("CHILD close(0)"); exit(EXIT_FAILURE); }
      if(close(1)) { perror("CHILD close(1)"); exit(EXIT_FAILURE); }
      if(close(2)) { perror("CHILD close(2)"); exit(EXIT_FAILURE); }
      if(dup2(pipe_out[1], 1) == -1) { perror("CHILD dup2()"); exit(EXIT_FAILURE); }
      if(dup2(pipe_err[1], 2) == -1) { perror("CHILD dup2()"); exit(EXIT_FAILURE); }
      if(close(pipe_out[0])) { perror("CHILD close(pipe_out)"); exit(EXIT_FAILURE); }
      if(close(pipe_out[1])) { perror("CHILD close(pipe_out)"); exit(EXIT_FAILURE); }
      if(close(pipe_err[0])) { perror("CHILD close(pipe_err)"); exit(EXIT_FAILURE); }
      if(close(pipe_err[1])) { perror("CHILD close(pipe_err)"); exit(EXIT_FAILURE); }
      if(close(client)) { perror("CHILD close(client)"); exit(EXIT_FAILURE); }
      int cap = 1024 + 13 + 1;
      char query_string_env[cap];
      snprintf(query_string_env, 1024 + 13, "QUERY_STRING=%s", query_string);
      query_string_env[cap - 1] = '\0';
      char * const envp[] = { query_string_env, NULL };
      // change working directory to be where the script resides
      filename[-1] = '\0'; if(uri != filename && chdir(uri)) { perror("CHILD chdir(path)"); fprintf(stderr, "path: %s\n", uri); exit(EXIT_FAILURE); }
      switch(hash_djb2_ext) {
        case hash_djb2_: {
          // executable
          if(!access(uri, X_OK)) {
            char * const args[] = { filename, NULL };
            execve(filename, args, envp);
          }
          // not executable (manually try to parse first line for #!)
          else {
            int file = open(filename, O_RDONLY); if(file == -1) { perror("CHILD open(non-executable-program)"); exit(EXIT_FAILURE); }
            char hash_bang[1025];
            ssize_t n = read(file, hash_bang, 1024); if(n == -1) { perror("CHILD read(hash_bang)"); exit(EXIT_FAILURE); }
            if(close(file)) { perror("CHILD WARNING close(non-executable-program)"); }
            if(n < 2) { fprintf(stderr, "CHILD read(hash_bang) wasn't big enough for hash bang\n"); exit(EXIT_FAILURE); }
            if(hash_bang[0] != '#' || hash_bang[1] != '!') { fprintf(stderr, "CHILD not hash bang\n"); exit(EXIT_FAILURE); }
            hash_bang[n] = '\0';
            char * line_sep = &hash_bang[2];
            char * hash_bang_line = strsep(&line_sep, "\r\n");
            char * args[] = { NULL, NULL, NULL, NULL };
            char * command_name = strrchr(hash_bang_line, '/');
            if(command_name) command_name += 1; else command_name = &hash_bang[2];
            int i = 0;
            args[i++] = command_name;
            if(!strcmp(command_name, "python3") || !strcmp(command_name, "python")) args[i++] = "-B";
            args[i++] = filename;
            execve(&hash_bang[2], args, envp);
          }
          perror("CHILD execve()");
          break; }
        case hash_djb2_py: {
          char * const args[] = { "python3", "-B", filename, NULL };
          execve("/usr/bin/python3", args, envp);
          perror("CHILD execve()");
          break; }
      }
      exit(EXIT_FAILURE);
    }
    // parent
    if(pid == -1) { perror("fork()"); exit(EXIT_FAILURE); }
    struct pollfd fds[3];
    const int timeout_ms = 1000;
    fds[0].fd = sigchld_fd;
    fds[1].fd = pipe_out[0]; if(close(pipe_out[1])) { perror("close(pipe_out)"); exit(EXIT_FAILURE); }
    fds[2].fd = pipe_err[0]; if(close(pipe_err[1])) { perror("close(pipe_err)"); exit(EXIT_FAILURE); }
    fds[0].events = fds[1].events = fds[2].events = POLLIN;
    bool child_has_stderr = false;
    size_t child_stdout_buffer_size = HTTP_200_HEADER_LEN;
    while(true) {
      int polled = poll(fds, 3, timeout_ms); if(polled == -1) { perror("poll()"); exit(EXIT_FAILURE); }
      if(polled == 0) {
        printf("WARNING poll() timed out\n");
      } else {
        bool read_something = false;
        // buffer stdout
        if(fds[1].revents & POLLIN) {
          ensure_scratch_and_child_stdout_buffer(&t->child_stdout_buffer, &t->child_stdout_buffer_capacity);
          size_t space_left = t->child_stdout_buffer_capacity - child_stdout_buffer_size;
          ssize_t n = read(fds[1].fd, &t->child_stdout_buffer[child_stdout_buffer_size], space_left); if(n == -1) { perror("read(child stdout)"); exit(EXIT_FAILURE); }
          if(n == space_left) {
            t->child_stdout_buffer_capacity *= 2;
            t->child_stdout_buffer = realloc(t->child_stdout_buffer, t->child_stdout_buffer_capacity);
            printf("INFO grew thread %d child stdout buffer to %zu\n", t->thread_id, t->child_stdout_buffer_capacity);
          }
          child_stdout_buffer_size += n;
          read_something = true;
        }
        // spew stderr
        if(fds[2].revents & POLLIN) {
          ssize_t n = read(fds[2].fd, buffer, buffer_capacity); if(n == -1) { perror("read(child stderr)"); exit(EXIT_FAILURE); }
          buffer[n] = '\0';
          printf("WARNING read %zd bytes from child stderr\n%s\n", n, buffer);
          child_has_stderr = true;
          read_something = true;
        }
        // child process ended (only handle this once stdout/stderr are empty)
        if(!read_something && fds[0].revents & POLLIN) {
          struct signalfd_siginfo info;
          ssize_t n = read(sigchld_fd, &info, sizeof(struct signalfd_siginfo)); if(n != sizeof(struct signalfd_siginfo)) { if(n == -1) perror("read(sigchld_fd)"); else fprintf(stderr, "read(sigchld_fd) wasn't whole\n"); exit(EXIT_FAILURE); }
          int child_exit = info.ssi_code == CLD_EXITED? info.ssi_status : EXIT_FAILURE;
          //printf("child exit: %d\n", child_exit);
          if(close(fds[1].fd)) perror("WARNING close(child stdout)");
          if(close(fds[2].fd)) perror("WARNING close(child stderr)");
          // child program failed
          if(child_has_stderr || child_exit != EXIT_SUCCESS) {
            // the child can ask to return 404 instead of 500 using the exit code 4
            if(child_exit == 4) { printf("WARNING child force 404\n"); goto encountered_problem; }
            printf("WARNING encountered problem (stderr=%d exit=%d), replied 500\n", child_has_stderr, child_exit);
            { ssize_t sent = send(client, HTTP_500_HEADER, HTTP_500_HEADER_LEN, MSG_MORE); if(sent != HTTP_500_HEADER_LEN) { if(sent == -1) perror("send()"); else fprintf(stderr, "send(): couldn't send whole message, sent only %zu.\n", sent); goto abort_client; } }
            int file = open("naws/500.inc", O_RDONLY); if(file == -1) { perror("open(500.inc)"); exit(EXIT_FAILURE); } struct stat file_stat; if(fstat(file, &file_stat)) { perror("fstat(500.inc)"); exit(EXIT_FAILURE); }
            { ssize_t sent = sendfile(client, file, NULL, file_stat.st_size); if(sent != file_stat.st_size) { if(sent == -1) perror("sendfile()"); else fprintf(stderr, "sendfile(500): couldn't send whole message, sent only %zu.\n", sent); close(file); goto abort_client; } }
            if(close(file)) { perror("close(500.inc)"); exit(EXIT_FAILURE); }
          }
          // child program success
          else {
            ensure_scratch_and_child_stdout_buffer(&t->child_stdout_buffer, &t->child_stdout_buffer_capacity);
            ssize_t sent = send(client, t->child_stdout_buffer, child_stdout_buffer_size, 0); if(sent != child_stdout_buffer_size) { if(sent == -1) perror("send()"); else fprintf(stderr, "send(): couldn't send whole message, sent only %zu.\n", sent); goto abort_client; }
          }
          break;
        }
      }
    }
  }

  // auth form
  goto skip_auth_form; auth_form: {
    printf("WARNING require authentification\n");
    // read public key (which is already in javascript Uint8Array declaration format)
    int file = open("naws/public.key", O_RDONLY); if(file == -1) { perror("open(public.key)"); exit(EXIT_FAILURE); } struct stat file_stat; if(fstat(file, &file_stat)) { perror("fstat(public.key)"); exit(EXIT_FAILURE); }
    char public_key[file_stat.st_size + 1];
    public_key[file_stat.st_size] = '\0';
    ssize_t n = read(file, public_key, file_stat.st_size); if(n != file_stat.st_size) { if(n == -1) perror("read(public.key)"); else fprintf(stderr, "read(public.key): couldn't read whole key\n"); exit(EXIT_FAILURE); }
    if(close(file)) { perror("close(public.key)"); exit(EXIT_FAILURE); }
    // encode a server message that includes a timestamp of some sort
    uint64_t ns = get_time_ns() + random() % 1000 * UINT64_C(1000000000); // I fudge the time a bit for unpredictability
    unsigned char nonce[crypto_secretbox_NONCEBYTES];
    randombytes_buf(nonce, crypto_secretbox_NONCEBYTES);
    unsigned char ciphertext[crypto_secretbox_MACBYTES + sizeof(ns)];
    unsigned char server_symmetric_key[crypto_secretbox_KEYBYTES];
    load_file("naws/symmetric.key", server_symmetric_key, crypto_secretbox_KEYBYTES, true);
    crypto_secretbox_easy(ciphertext, (const unsigned char *)&ns, sizeof(ns), nonce, server_symmetric_key);
    explicit_bzero(server_symmetric_key, crypto_secretbox_KEYBYTES);
    // convert that to javascript Uint8Array declaration format, (with nonce too)
    ensure_scratch_and_child_stdout_buffer(&t->child_stdout_buffer, &t->child_stdout_buffer_capacity);
    char * tmp_buffer = t->child_stdout_buffer + HTTP_200_HEADER_LEN;
    tmp_buffer += sprintf(tmp_buffer, "new Uint8Array([%d", nonce[0]);
    for(int i = 1; i < crypto_secretbox_NONCEBYTES; i++) {
      tmp_buffer += sprintf(tmp_buffer, ", %d", nonce[i]);
    }
    for(int i = 0; i < crypto_secretbox_MACBYTES + sizeof(ns); i++) {
      tmp_buffer += sprintf(tmp_buffer, ", %d", ciphertext[i]);
    }
    tmp_buffer += sprintf(tmp_buffer, "])");
    // send login page
    send_static_header(client, hash_djb2_html);
    file = open("naws/401.inc", O_RDONLY); if(file == -1) { perror("open(401.inc)"); exit(EXIT_FAILURE); }
    const char * from[2];
    const char * to[2];
    from[0] = "SRV_PUB"; to[0] = public_key;
    from[1] = "SRV_MSG"; to[1] = t->child_stdout_buffer + HTTP_200_HEADER_LEN;
    send_template_file(client, file, from, to, 2);
    if(close(file)) { perror("close(401.inc)"); exit(EXIT_FAILURE); }
  } skip_auth_form:

  // if any problem arised, do 404 instead
  goto skip_encountered_problem; encountered_problem: {
    printf("WARNING encountered problem, replied 404\n");
    if(!do404(client)) goto abort_client;
  } skip_encountered_problem:

  // on detection of hacking attempt, kill server
  goto skip_hack; hacking_attempt_detected: {
    printf("Hacking attempt detected. Over and out.\n");
    exit(EXIT_FAILURE);
  } skip_hack:

  printf("ACCESS done handling client\n");
  abort_client:
  if(shutdown(client, SHUT_RDWR)) { perror("WARNING shutdown(client)"); }
  if(close(client)) { perror("WARNING close(client)"); }

  t->in_use = false;
}
