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

/******************************************************************
                   +-------+                      +--------+
-- source code --> | lexer | --> token stream --> | parser | --> assembly
                   +-------+                      +--------+
*******************************************************************/
// tokens and classes
enum  {
  Num = 128, Fun, Sys, Glo, Loc, Id,
  Char, Else, Enum, If, Int, Return, Sizeof, While,
  Assign, Cond, Lor, Lan, Or, Xor, And, Eq, Ne, Lt, Gt, Le, Ge, Shl, Shr, Add, Sub, Mul, Div, Mod, Inc, Dec, Brak};

/******************************************************************
define identifier {
  int token;        // token id
  int hash;         // token hash, used for compare
  char *name;       // token name
  int class;        // token class, global var or local var
  int type;         // token type, int/char/int*...
  int value;        // token value, address/integer value...
  int bclass;       // global identifier when local var and global var are the same
  int btype;
  int bvalue;
}

Symbol table:
----+-----+----+----+----+-----+-----+-----+------+------+----
 .. |token|hash|name|type|class|value|btype|bclass|bvalue| ..
----+-----+----+----+----+-----+-----+-----+------+------+----
    |<---       one single identifier                --->|
*******************************************************************/
int token_val;                // value of current token
int *current_id;              // current parsed id
int *symbols;                 // symbol table

// fields of identifier
enum {Token, Hash, Name, Type, Class, Value, BType, BClass, BValue, IdSize};

// types of variable/funtion
enum {CHAR, INT, PTR};
int *idmain;                  // the main function


/**
  * get next token, the function will ignore black character.
  */
void next() {
  char *last_pos;
  int hash;

  // ignore unknown token
  while (token = *src) {
    ++src;

    // parse token
    if (token == '\n') {
      ++line;
    } else if (token == '#') {
      // skip macro
      while (*src != 0 && *src != '\n') {
        src++;
      }
    } else if ((token >= 'a' && token <= 'z') || (token >= 'A' && token <= 'Z') || (token == '_')) {
      // parse identifier
      last_pos = src - 1;
      hash = token;
      
      while ((*src >= 'a' && *src <= 'z') || (*src >= 'A' && *src <= 'Z') || (*src >= '0' && *src <= '9') || (*src == '_')) {
        hash = hash * 147 + *src;
        src++;
      }

      // look for existing identifier, linear search
      current_id = symbols;
      while (current_id[Token]) {
        // find and return
        if (current_id[Hash] == hash && !memcmp((char*)current_id[Name], last_pos, src - last_pos)) {
          token = current_id[Token];
          return;
        }
        current_id = current_id + IdSize;
      }

      // not find and store new id
      current_id[Name] = (int)last_pos;
      current_id[Hash] = hash;
      token = current_id[Token] = Id;
      return;
    } else if (token >= '0' && token <= '9') {
      // parse number, support dec(123) hex(0x123) oct(0123)
      token_val = token - '0';
      if (token_val > 0) {
        // dec, starts with [1-9]
        while (*src >= '0' && *src <= '9') {
          token_val = token_val * 10 + *src++ - '0';
        }
      } else {
        // start with number 0
        if (*src == 'x' || *src == 'X') {
          // hex, starts with x or X
          token = *++src;
          while ((token >= '0' && token <= '9') || (token >= 'a' && token <= 'f') || (token >= 'A' && token <= 'F')) {
            token_val = token_val * 16 + (token & 15) + (token >= 'A' ? 9 : 0);
            token = *++src;
          }
        } else {
          // oct
          while (*src >= '0' && *src <= '7') {
            token_val = token_val * 8 + *src++ - '0';
          }
        }
      }

      token = Num;
      return;
    } else if (token == '"' || token == '\'') {
      // parse string literal, currently only '\n' supporte escape
      // store the string literal into data
      last_pos = data;
      while (*src != 0 && *src != token) {
        token_val = *src++;
        if (token_val == '\\') {
          // escape charater
          token_val = *src++;
          if (token_val == 'n') {
            token_val = '\n';
          }
        }

        if (token == '"') {
          *data++ = token_val;
        }
      }

      src++;
      // if it is a single character, return Num token
      if (token == '"') {
        token_val = (int)last_pos;
      } else {
        token = Num;
      }
      return;
    } else if (token == '/') {
      if (*src == '/') {
        // skip comments, only // type
        while (*src != 0 && *src != '\n') {
          ++src;
        }
      } else {
        // divide operator
        token = Div;
        return;
      }
    } else if (token == '=') {
      // parse '==' and '='
      if (*src == '=') {
        src++;
        token = Eq;
      } else {
        token = Assign;
      }
      return;
    } else if (token == '+') {
      // parse '+' and '++'
      if (*src == '+') {
        src++;
        token = Inc;
      } else {
        token = Add;
      }
      return;
    } else if (token == '-') {
      // parse '-' and '--'
      if (*src == '-') {
        src ++;
        token = Dec;
      } else {
        token = Sub;
      }
      return;
    } else if (token == '!') {
      // parse '!='
      if (*src == '=') {
        src++;
        token = Ne;
      }
      return;
    } else if (token == '<') {
      // parse '<=', '<<' or '<'
      if (*src == '=') {
        src ++;
        token = Le;
      } else if (*src == '<') {
        src ++;
        token = Shl;
      } else {
        token = Lt;
      }
      return;
    } else if (token == '>') {
      // parse '>=', '>>' or '>'
      if (*src == '=') {
        src ++;
        token = Ge;
      } else if (*src == '>') {
        src ++;
        token = Shr;
      } else {
        token = Gt;
      }
      return;
    } else if (token == '|') {
      // parse '|' or '||'
      if (*src == '|') {
        src ++;
        token = Lor;
      } else {
        token = Or;
      }
      return;
    } else if (token == '&') {
      // parse '&' and '&&'
      if (*src == '&') {
        src ++;
        token = Lan;
      } else {
        token = And;
      }
      return;
    } else if (token == '^') {
      token = Xor;
      return;
    } else if (token == '%') {
      token = Mod;
      return;
    } else if (token == '*') {
      token = Mul;
      return;
    } else if (token == '[') {
      token = Brak;
      return;
    } else if (token == '?') {
      token = Cond;
      return;
    } else if (token == '~' || token == ';' || token == '{' || token == '}' || token == '(' || token == ')' || token == ']' || token == ',' || token == ':') {
      // directly return the character as token;
      return;
    }
  }
  return;
}


/********************* useless code ***********************/
int expr();
void match(int tk);
int factor() {
  int value = 0;
  if (token == '(') {
    match('(');
    value = expr();
    match(')');
  } else {
    value = token_val;
    match(Num);
  }
  return value;
}

int term_tail(int lvalue) {
  if (token == '*') {
    match('*');
    int value = lvalue * factor();
    return term_tail(value);
  } else if (token == '/') {
    match('/');
    int value = lvalue / factor();
    return term_tail(value);
  } else {
    return lvalue;
  }
}

int term() {
  int lvalue = factor();
  return term_tail(lvalue);
}

int expr_tail(int lvalue) {
  if (token == '+') {
    match('+');
    int value = lvalue + term();
    return expr_tail(value);
  } else if (token == '-') {
    match('-');
    int value = lvalue - term();
    return expr_tail(value);
  } else {
    return lvalue;
  }
}

int expr() {
  int lvalue = term();
  return expr_tail(lvalue);
}
/*********************** useless code ***********************/

/**
  * parse expression.
  */
void expression(int level) {
  return;
}

int base_type;        // the type of a declaration
int expr_type;        // the type of an expression

void match(int tk) {
  if (token == tk) {
    next();
  } else {
    printf("%d: expected token: %d\n", line, tk);
    exit(-1);
  }
}

/*******************************************************************
int demo(int param_a, int *param_b) {
  int local_1;
  char local_2;

  ...
}
                function stack
             |      ......      |
             +------------------+
high address |    arg: param_a  |   new_bp + 3
             +------------------+
             |    arg: param_b  |   new_bp + 2 
             +------------------+
             |  return address  |   new_bp + 1
             +------------------+
             |      old BP      |<--new BP
             +------------------+
             |      local_1     |   new_bp - 1
             +------------------+
low address  |      local_2     |   new_bp - 2
             +------------------+
             |      ......      |

*******************************************************************/
int index_of_bp;              // index of bp pointer on stack

void statements() {

}

void function_parameter() {
  int type;
  int params = 0;             // index of current parameter
  while (token != ')') {
    // int name, ...
    type = INT;
    if (token == INT) {
      match(Int);
    } else if (token == Char) {
      type = CHAR;
      match(Char);
    }

    // pointer type
    while (token == Mul) {
      match(Mul);
      type = type + PTR;
    }

    // parameter name
    if (token != Id) {
      printf("%d: bad parameter declaration.\n", line);
      exit(-1);
    }
    if (current_id[Class] == Loc) {
      printf("%d: duplicate parameter declaration\n", line);
      exit(-1);
    }

    match(Id);

    // store the local variable
    current_id[BClass] = current_id[Class];
    current_id[Class] = Loc;
    current_id[BType] = current_id[Type];
    current_id[Type] = type;
    current_id[BValue] = current_id[Value];
    current_id[Value] = params++;

    if (token == ',') {
      match(',');
    }
  }

  index_of_bp = params + 1;
}

void function_body() {
  // type func_name (...) {...}

  // ... {
  // 1. local declarations
  // 2. statements
  // }

  int pos_local;              // position of local variables on the stack
  int type;
  pos_local = index_of_bp;

  while (token == Int || token == Char) {
    // local variable declaration, just like global ones
    base_type = (token == Int) ? INT : CHAR;
    match(token);

    while (token != ';') {
      type = base_type;
      while (token == Mul) {
        match(Mul);
        type = type + PTR;
      }

      if (token != Id) {
        // invalid declaration
        printf("%d: bad local declaration\n", line);
        exit(-1);
      }
      if (current_id[Class] == Loc) {
        // identifier exists
        printf("%d: duplicate local declaration\n", line);
        exit(-1);
      }
      match(Id);

      // store the local variable
      current_id[BClass] = current_id[Class];
      current_id[Class] = Loc;
      current_id[BType] = current_id[Type];
      current_id[Type] = type;
      current_id[BValue] = current_id[Value];
      current_id[Value] = ++pos_local;

      if (token == ',') {
        match(',');
      }
    }
    match(';');
  }

  // save the stack size for local variables
  *++text = ENT;
  *++text = pos_local - index_of_bp;

  // statements
  while (token != '}') {
    statements();
  }

  // emit code for leaving the sub function
  *++text = LEV;
}

void function_declaration() {
  // type func_name (...) {...}
  match('(');
  function_parameter();
  match(')');
  match('{');
  function_body();
  
  current_id = symbols;
  while (current_id[Token]) {
    if (current_id[Class] == Loc) {
      current_id[Class] = current_id[BClass];
      current_id[Type] = current_id[BType];
      current_id[Value] = current_id[BValue];
    }
    current_id = current_id + IdSize;
  }
}

void enum_declaration() {
  // parse enum [id] { a = 1, b = 2, ... }
  int i = 0;
  while (token != '}') {
    if (token != Id) {
      printf("%d: bad enum identifier %d\n", line, token);
      exit(-1);
    }
    next();

    if (token == Assign) {
      // like {a = 10}
      next();
      if (token != Num) {
        printf("%d: bad enum initializer\n", line);
        exit(-1);
      }
      i = token_val;
      next();
    }

    current_id[Class] = Num;
    current_id[Type] = INT;
    current_id[Value] = i++;

    if (token == ',') {
      next();
    }

  }
}

void global_declaration() {
  // global_declaration ::= enum_decl | variable_decl | function_decl
  // enum_decl ::= 'enum' [id] '{' id ['=' 'num'] {',' id ['=' 'num'} '}'
  // variable_decl ::= type {'*'} id { ',' {'*'} id } ';'
  // function_decl ::= type {'*'} id '(' parameter_decl ')' '{' body_decl '}'

  int type;           // type for variable
  int i;

  // parse enum, this should be treated alone
  if (token == Enum) {
    // enum [id] {a = 10, b= 20, ... }
    match(Enum);
    if (token != '{') {
      // skip the [id] part
      match(Id);
    }
    if (token == '{') {
      // parse the assign part
      match('{');
      enum_declaration();
      match('}');
    }

    match(';');
    return;
  }

  // parse type information
  if (token == Int) {
    match(Int);
  } else if (token == Char) {
    match(Char);
    base_type = CHAR;
  }

  // parse the comma seperated variable declaration
  while (token != ';' && token != '}') {
    type = base_type;
    // parse pointer type, note that there may exist `int ****x;`
    while (token == Mul) {
      match(Mul);
      type = type + PTR;
    }

    if (token != Id) {
      // invalid declaration
      printf("%d: bad global declaration\n", line);
      exit(-1);
    }

    if (current_id[Class]) {
      // identifier exists
      printf("%d: duplicate global declaration\n", line);
      exit(-1);
    }
    match(Id);
    current_id[Type] = type;

    if (token == '(') {
      current_id[Class] = Fun;
      current_id[Value] = (int)(text + 1);    // the memory address of function
      function_declaration();
    } else {
      current_id[Class] = Glo;
      current_id[Value] = (int)data;          // assign memory address
      data = data + sizeof(int);
    }

    if (token == ',') {
      match(',');
    }
  }

  next();
}

/**
  * entry of grammar parse.
  */
void program() {
  next();
  while (token > 0) {
    global_declaration();
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

  // test token parse
  src = "char else enum if int return sizeof while "
        "open read close printf malloc memset memcmp exit void main";

  // add keywords to symbol table
  i = Char;
  while (i <= While) {
    next();
    current_id[Token] = i++;
  }

  // add library to symbol table
  i = OPEN;
  while (i <= EXIT) {
    next();
    current_id[Class] = Sys;
    current_id[Type] = INT;
    current_id[Value] = i++;
  }

  next(); current_id[Token] = Char;
  next(); idmain = current_id;


  program();
  return eval();
}
