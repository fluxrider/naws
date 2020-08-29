#include <stdio.h>
#include <unistd.h>

int main(int argc, char * argv[]) {

  printf(
  "Content-Type:text/html;charset=utf-8\r\n\r\n"
  "<!DOCTYPE html>\n"
  "<html>\n"
  "<head>\n"
  "<meta charset=\"utf-8\" />\n"
  "<title>C program</title>\n"
  "</head><body>\n"
  "C program\n"
  "</body>\n"
  "</html>"
  );

  sleep(2);
  return 0;
}
