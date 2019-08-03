#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <string.h>

int token;            // current token
char *src, *old_src;  // pointer to source code string
int poolsize;         // default size of text/data/stack
int line;             // line number

/**
  * get next token, the function will ignore black character.
  */
void next() {
  token = *src++;
  return;
}

/**
  * parse expression.
  */
void expression(int level) {
  return;
}

/**
  * entry of grammar parse.
  */
void program() {
  next();
  while (token > 0) {
    printf("token is: %c\n", token);
    next();
  }
  return;
}

/**
  * entry of virtual machine which used to explain object code.
  */
int eval() {
  return 0;
}

/**
  * the procedure as follows: 1)read a c-code file to main memory
  * 2) token parse for all characters and print it.
  */
int main(int argc, char **argv) {
  int i, fd;
  
  argc--;
  argv++;

  poolsize = 256 * 1024;
  line = 1;

  if ((fd = open(*argv, 0)) < 0) {
    printf("could not open (%s)\n", *argv);
    return -1;
  }

  if (!(src = old_src = malloc(poolsize))) {
    printf("could not malloc (%d) for source area\n", poolsize);
    return -1;
  }

  // read the source file
  if ((i = read(fd, src, poolsize - 1)) <= 0) {
    printf("read return %d\n", i);
    return -1;
  }

  src[i] = 0;           // add EOF character
  close(fd);

  program();
  return eval();
}
