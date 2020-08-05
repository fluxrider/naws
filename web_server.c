// Copyright 2020 David Lareau. This program is free software under the terms of the GPL-3.0-or-later.
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

#define HTTP_404_HEADER "HTTP/1.1 404 Not Found\r\nContent-Type:text/html;charset=utf-8\r\n\r\n"
#define HTTP_404_HEADER_LEN (sizeof(HTTP_404_HEADER) - 1)
#define HTTP_500_HEADER "HTTP/1.1 500 Internal Server Error\r\nContent-Type:text/html;charset=utf-8\r\n\r\n"
#define HTTP_500_HEADER_LEN (sizeof(HTTP_500_HEADER) - 1)
#define HTTP_200_HEADER "HTTP/1.1 200 OK\r\n"
#define HTTP_200_HEADER_LEN (sizeof(HTTP_200_HEADER) - 1)

// C workaround to switch on string (i.e. hash them)

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

// transform children end signal into a file descriptor
int setup_signalfd() {
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGCHLD);

  // block signal
  if(sigprocmask(SIG_BLOCK, &mask, NULL) == -1) { perror("sigprocmask"); exit(EXIT_FAILURE); }

  return signalfd(-1, &mask, SFD_CLOEXEC);
}

int main(int argc, char * argv[]) {
  if(argc != 3) { fprintf(stderr, "usage: naws port root-folder\nexample: naws 8888 .\n"); exit(EXIT_FAILURE); }

  int sigchld_fd = setup_signalfd(); if(sigchld_fd == -1) { perror("signalfd()"); exit(EXIT_FAILURE); }

  uint16_t port = strtol(argv[1], NULL, 10);
  if(chdir(argv[2])) { perror("chdir(root)"); exit(EXIT_FAILURE); }

  // listen for clients
  int server = socket(AF_INET, SOCK_STREAM, 0); if(server == -1) { perror("socket()"); exit(EXIT_FAILURE); }
  if(setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int))) { perror("setsockopt()"); exit(EXIT_FAILURE); }
  if(bind(server, (const struct sockaddr *)&(struct sockaddr_in){AF_INET, htons(port), {INADDR_ANY}}, sizeof(struct sockaddr_in))) {
    perror("bind()");
    if(port < 1024) fprintf(stderr, "for privileged ports, ensure capability is set\nsudo setcap 'cap_net_bind_service=+ep' /path/to/program\n");
    exit(EXIT_FAILURE);
  }
  if(listen(server, 0)) { perror("listen()"); exit(EXIT_FAILURE); }
  struct sockaddr_in client_addr;
  const int buffer_capacity = 8191;
  uint8_t buffer[buffer_capacity + 1]; // room for a null char
  size_t child_stdout_buffer_capacity = 10 * 1024;
  uint8_t * child_stdout_buffer = realloc(NULL, child_stdout_buffer_capacity);
  memcpy(child_stdout_buffer, HTTP_200_HEADER, HTTP_200_HEADER_LEN);
  while(true) {
    int client = accept(server, (struct sockaddr *)&client_addr, &(socklen_t){sizeof(struct sockaddr_in)}); if(client == -1) { perror("accept()"); exit(EXIT_FAILURE); }

    // allow only the usual private IPv4 addresses
    uint8_t * ip = (uint8_t *)&client_addr.sin_addr.s_addr;
    printf("ACCESS client_address: %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]);
    bool allowed_ip = false;
    allowed_ip |= ip[0] == 192 && ip[1] == 168;
    allowed_ip |= ip[0] == 127 && ip[1] == 0 && ip[2] == 0 && ip[3] == 1;
    if(!allowed_ip) goto encountered_problem;
    // TODO would it be possible to behave exactly like if there was no server? filter ip with SO_ATTACH_BPF?
    // TODO if traffic from the internet/tor, turn on HTTPS/AUTH and turn server off on multi failed attempts

    // receive
    ssize_t length = recv(client, &buffer, buffer_capacity, 0); if(length == -1) { perror("recv()"); exit(EXIT_FAILURE); }
    buffer[length] = '\0';
    // printf("%s\n", buffer);
    if(length < 4 || strncmp(buffer, "GET ", 4)) goto encountered_problem;

    // handle GET
    // get uri and query_string
    char * query_string = NULL;
    char * uri = &buffer[4];
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
          fprintf(stderr, "WARNING error parsing request-uri\n%s", buffer);
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
    if(!query_string) query_string = "";
    printf("ACCESS uri: %s query: %s\n", uri, query_string);
    if(uri[0] != '/') goto encountered_problem;
    char * filename = strrchr(uri, '/') + 1;
    do { uri += 1; } while(uri[0] == '/');
    
    // figure out filename extension
    const char * ext = strrchr(filename, '.');
    if(!ext) ext = ""; else ext += 1;
    printf("uri: %s\n", uri);
    printf("filename: %s\n", filename);
    printf("ext: %s\n", ext);

    // verify access of local_uri
    if(access(uri, R_OK)) goto encountered_problem;
    
    // try sending as static file
    const uint32_t hash_djb2_ext = hash_djb2(ext);
    if(send_static_header(client, hash_djb2_ext)) {
      int file = open(uri, O_RDONLY); if(file == -1) { perror("open(uri)"); exit(EXIT_FAILURE); } struct stat file_stat; if(fstat(file, &file_stat)) { perror("fstat(uri)"); exit(EXIT_FAILURE); }
      { ssize_t sent = sendfile(client, file, NULL, file_stat.st_size); if(sent != file_stat.st_size) { if(sent == -1) perror("sendfile(uri)"); else fprintf(stderr, "sendfile(uri): couldn't send whole message, sent only %zu.\n", sent); exit(EXIT_FAILURE); } }
      if(close(file)) { perror("close(uri)"); exit(EXIT_FAILURE); }
    } else {
      // if a program, fork and run
      switch(hash_djb2_ext) {
        case hash_djb2_:
          if(access(uri, X_OK)) goto encountered_problem;
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
        if(close(server)) { perror("CHILD close(server)"); exit(EXIT_FAILURE); }
        if(close(client)) { perror("CHILD close(client)"); exit(EXIT_FAILURE); }
        int cap = 1024 + 13 + 1;
        char query_string_env[cap];
        snprintf(query_string_env, 1024 + 13, "QUERY_STRING=%s", query_string);
        query_string_env[cap - 1] = '\0';
        char * const envp[] = { query_string_env, NULL };
        // change working directory to be where the script resides
        filename[-1] = '\0'; if(uri != filename && chdir(uri)) { perror("CHILD chdir(path)"); fprintf(stderr, "path: %s", uri); exit(EXIT_FAILURE); }
        switch(hash_djb2_ext) {
          case hash_djb2_: {
            char * const args[] = { filename, NULL };
            execve(filename, args, envp);
            perror("CHILD execve()");
            break; }
          case hash_djb2_py: {
            char * const args[] = { "python", "-B", filename, NULL };
            execve("/usr/bin/python", args, envp);
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
            size_t space_left = child_stdout_buffer_capacity - child_stdout_buffer_size;
            ssize_t n = read(fds[1].fd, &child_stdout_buffer[child_stdout_buffer_size], space_left); if(n == -1) { perror("read(child stdout)"); exit(EXIT_FAILURE); }
            if(n == space_left) {
              child_stdout_buffer_capacity *= 2;
              child_stdout_buffer = realloc(child_stdout_buffer, child_stdout_buffer_capacity);
              printf("INFO grew child stdout buffer to %zu\n", child_stdout_buffer_capacity);
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
              printf("WARNING encountered problem, replied 500\n");
              { ssize_t sent = send(client, HTTP_500_HEADER, HTTP_500_HEADER_LEN, MSG_MORE); if(sent != HTTP_500_HEADER_LEN) { if(sent == -1) perror("send()"); else fprintf(stderr, "send(): couldn't send whole message, sent only %zu.\n", sent); exit(EXIT_FAILURE); } }
              int file = open("500.html", O_RDONLY); if(file == -1) { perror("open(500.html)"); exit(EXIT_FAILURE); } struct stat file_stat; if(fstat(file, &file_stat)) { perror("fstat(500.html)"); exit(EXIT_FAILURE); }
              { ssize_t sent = sendfile(client, file, NULL, file_stat.st_size); if(sent != file_stat.st_size) { if(sent == -1) perror("sendfile()"); else fprintf(stderr, "sendfile(): couldn't send whole message, sent only %zu.\n", sent); exit(EXIT_FAILURE); } }
              if(close(file)) { perror("close(500.html)"); exit(EXIT_FAILURE); }
            }
            // child program success
            else {
              ssize_t sent = send(client, child_stdout_buffer, child_stdout_buffer_size, 0); if(sent != child_stdout_buffer_size) { if(sent == -1) perror("send()"); else fprintf(stderr, "send(): couldn't send whole message, sent only %zu.\n", sent); exit(EXIT_FAILURE); }
            }
            break;
          }
        }
      }
    }

    // if any problem arised, do 404 instead
    goto skip_encountered_problem; encountered_problem: {
      printf("WARNING encountered problem, replied 404\n");
      { ssize_t sent = send(client, HTTP_404_HEADER, HTTP_404_HEADER_LEN, MSG_MORE); if(sent != HTTP_404_HEADER_LEN) { if(sent == -1) perror("send()"); else fprintf(stderr, "send(): couldn't send whole message, sent only %zu.\n", sent); exit(EXIT_FAILURE); } }
      int file = open("404.html", O_RDONLY); if(file == -1) { perror("open(404.html)"); exit(EXIT_FAILURE); } struct stat file_stat; if(fstat(file, &file_stat)) { perror("fstat(404.html)"); exit(EXIT_FAILURE); }
      { ssize_t sent = sendfile(client, file, NULL, file_stat.st_size); if(sent != file_stat.st_size) { if(sent == -1) perror("sendfile()"); else fprintf(stderr, "sendfile(): couldn't send whole message, sent only %zu.\n", sent); exit(EXIT_FAILURE); } }
      if(close(file)) { perror("close(404.html)"); exit(EXIT_FAILURE); }
    } skip_encountered_problem:

    if(shutdown(client, SHUT_RDWR)) { perror("WARNING shutdown(client)"); }
    if(close(client)) { perror("WARNING close(client)"); }
  }

  free(child_stdout_buffer);
  if(shutdown(server, SHUT_RDWR)) { perror("WARNING shutdown(server)"); }
  if(close(server)) { perror("WARNING close(server)"); }
  return EXIT_SUCCESS;
}
