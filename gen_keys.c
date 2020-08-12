// Copyright 2020 David Lareau. This source code form is subject to the terms of the Mozilla Public License 2.0.
// gcc gen_keys.c $(pkg-config --libs --cflags libsodium) && ./a.out
#include <stdio.h>
#include <stdint.h>
#include <sodium.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char * argv) {
  // gen keys
  if(sodium_init() < 0) { fprintf(stderr, "sodium_init() failed\n"); exit(EXIT_FAILURE); }
  unsigned char secret_key[crypto_box_SECRETKEYBYTES];
  unsigned char public_key[crypto_box_PUBLICKEYBYTES];
  crypto_box_keypair(public_key, secret_key);

  // store private key in a file
  {
    int file = open("secret.key", O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR); if(file == -1) { perror("open()"); exit(EXIT_FAILURE); }
    ssize_t n = write(file, secret_key, crypto_box_SECRETKEYBYTES); if(n != crypto_box_SECRETKEYBYTES) { if(n == -1) perror("write()"); else fprintf(stderr, "write(): couldn't write whole message, sent only %zu.\n", n); exit(EXIT_FAILURE); }
    if(close(file)) { perror("close()"); exit(EXIT_FAILURE); }
  }

  // store public key as javascript array (because that's the only context I use it)
  {
    int file = open("public.key", O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR); if(file == -1) { perror("open()"); exit(EXIT_FAILURE); }
    dprintf(file, "new Uint8Array([");
    dprintf(file, "%d", public_key[0]);
    for(int i = 1; i < crypto_box_PUBLICKEYBYTES; i++) {
      dprintf(file, ", %d", public_key[i]);
    }
    dprintf(file, "])");
    if(close(file)) { perror("close()"); exit(EXIT_FAILURE); }
  }

  return EXIT_SUCCESS;
}
