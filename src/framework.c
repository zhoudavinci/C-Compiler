#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <string.h>

int token;            // current token
char *src, *old_src;  // pointer to source code string
int poolsize;         // default size of text/data/stack
int line;             // line number

/*******************************************************************
         virtual memory map for a process
             +------------------+
high address |    stack   |     | function call related vars
             |    ...     v     | like frame parameter or local vars
             |                  |
             |                  |
             |                  |
             |                  |
             |    ...     ^     |
             |    heap    |     | dynamic vars
             +------------------+
             | bss  segment     | uninitialized vars
             +------------------+
             | data segment     | initialized vars
             +------------------+
low address  | text segment     | code/instruct
             +------------------+

simplicity, we care about `text segment`, `data segment` and `stack`,
we don't need `bss segment` because our compiler don't support uninitialized vars, 
beside, we use instruct `MSET` to malloc memory(dynamic)
*******************************************************************/
int *text,            // text segment
    *old_text,        // for dump text segment
    *stack;           // stack
char *data;           // data segment, only for string

/******************************************************************
register for store program running status, we use four registers as follows:
program counter(PC): the next code/instruct memory address
stack pointer(SP): point to the top of stack, increase with address decrease
base pointer(BP): point to the somewhere of stack, used to function call
general register(AX): store the result of one instruct
*******************************************************************/
int *pc, *bp, *sp, ax, cycle;     // virtual machine registers


/******************************************************************
instructions for CPU
*******************************************************************/
enum {LEA,IMM,JMP,CALL,JZ,JNZ,ENT,ADJ,LEV,LI,LC,SI,SC,PUSH,
      OR,XOR,AND,EQ,NE,LT,GT,LE,GE,SHL,SHR,ADD,SUB,
      MUL,DIV,MOD,OPEN,READ,CLOS,PRTF,MALC,MSET,MCMP,EXIT};


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
  int op, *tmp;
  while (op = *pc++) {
    if (op == IMM)      {ax = *pc++;}                   // load immediate value to ax
    else if (op == LC)  {ax = *(char*)ax;}              // load character to ax, address in ax
    else if (op == LI)  {ax = *(int*)ax;}               // load integer to ax, address in ax
    else if (op == SC)  {*(char*)*sp++ = ax;}           // store character as address to stack
    else if (op == SI)  {*(int*)*sp++ = ax;}            // store integer as address to stack
    else if (op == PUSH){*--sp = ax;}                   // push the value of ax onto the stack
    else if (op == JMP) {pc = (int*)*pc;}               // jump to the address
    else if (op == JZ)  {pc = ax ? pc + 1 : (int*)*pc;} // jump if ax is zero
    else if (op == JNZ) {pc = ax ? (int*)*pc : pc + 1;} // jump if ax is not zero
    else if (op == CALL){*--sp = (int)(pc + 1); pc = (int*)*pc;}        // call subroutine
    else if (op == ENT) {*--sp = (int)bp; bp = sp; sp = sp - *pc++;}    // make new stack frame
    else if (op == ADJ) {sp = sp + *pc++;}              // remove arguments from frame
    else if (op == LEV) {sp = bp; bp = (int*)*sp++; pc = (int*)*sp++;}  // restore old call frame
    else if (op == LEA) {ax = (int)(bp + *pc++);}       // load address for arguments
    
    // binary-operations
    else if (op == OR)  ax = *sp++ | ax;
    else if (op == XOR) ax = *sp++ ^ ax;
    else if (op == AND) ax = *sp++ & ax;
    else if (op == EQ)  ax = *sp++ == ax;
    else if (op == NE)  ax = *sp++ != ax;
    else if (op == LT)  ax = *sp++ < ax;
    else if (op == LE)  ax = *sp++ <= ax;
    else if (op == GT)  ax = *sp++ >  ax;
    else if (op == GE)  ax = *sp++ >= ax;
    else if (op == SHL) ax = *sp++ << ax;
    else if (op == SHR) ax = *sp++ >> ax;
    else if (op == ADD) ax = *sp++ + ax;
    else if (op == SUB) ax = *sp++ - ax;
    else if (op == MUL) ax = *sp++ * ax;
    else if (op == DIV) ax = *sp++ / ax;
    else if (op == MOD) ax = *sp++ % ax;

    // inner functions
    else if (op == EXIT) { printf("exit(%d)", *sp); return *sp;}
    else if (op == OPEN) { ax = open((char *)sp[1], sp[0]); }
    else if (op == CLOS) { ax = close(*sp);}
    else if (op == READ) { ax = read(sp[2], (char *)sp[1], *sp); }
    else if (op == PRTF) { tmp = sp + pc[1]; ax = printf((char *)tmp[-1], tmp[-2], tmp[-3], tmp[-4], tmp[-5], tmp[-6]); }
    else if (op == MALC) { ax = (int)malloc(*sp);}
    else if (op == MSET) { ax = (int)memset((char *)sp[2], sp[1], *sp);}
    else if (op == MCMP) { ax = memcmp((char *)sp[2], (char *)sp[1], *sp);}

    // unknown instructions
    else {
      printf("unknown instructions: (%d)\n", op);
      return -1;
    }
  }
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

  // allocate memory for virtual machine
  if (!(text = old_text = malloc(poolsize))) {
    printf("could not malloc (%d) for text area\n", poolsize);
    return -1;
  }
  if (!(data = malloc(poolsize))) {
    printf("could not malloc (%d) for data area\n", poolsize);
    return -1;
  }
  if (!(stack = malloc(poolsize))) {
    printf("could not malloc (%d) for stack area\n", poolsize);
    return -1;
  }

  memset(text, 0, poolsize);
  memset(data, 0, poolsize);
  memset(stack, 0, poolsize);

  bp = sp = (int*)((int)stack + poolsize);
  ax = 0;

  // test code for 10+20
  i = 0;
  text[i++] = IMM;
  text[i++] = 10;
  text[i++] = PUSH;
  text[i++] = IMM;
  text[i++] = 20;
  text[i++] = ADD;
  text[i++] = PUSH;
  text[i++] = EXIT;
  pc = text;

  // program();
  return eval();
}
