#include <stdio.h>
#include <stdint.h>

uint32_t hash_djb2(const char * s) {
  uint32_t hash = 5381;
  while(*s) hash = ((hash << 5) + hash) + *s++;
  return hash;
}

void p(const char * s) {
  printf("#define hash_djb2_%s %u\n", s, hash_djb2(s));
}

int main(int argc, char * argv) {
  p("css");
  p("js");
  p("html");
  p("png");
  p("webp");
  p("jpg");
  p("jpeg");
  p("svg");
  p("epub");
  p("mobi");
  p("mp4");
  p("ttf");

  p("py");
  p("");
  return 0;
}
