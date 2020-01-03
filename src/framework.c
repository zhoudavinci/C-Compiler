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


int base_type;                // the type of a declaration
int expr_type;                // the type of an expression
int index_of_bp;              // index of bp pointer on stack

/**
  * parse expression.
  */
void expression(int level) {
  // unit_unary()
  int *id, *addr;
  int tmp;
  if (!token) {
    printf("%d: unexpected token EOF of expression\n", line);
    exit(-1);
  }
  if (token == Num) {
    match(Num);
    // emit code
    *++text = IMM;
    *++text = token_val;
    expr_type = INT;
  }
  else if (token == '"') {
    // emit code
    *++text = IMM;
    *++text = token_val;

    match('"');
    // store the rest strings
    while (token == '"') {
      match('"');
    }

    // append the end of string character '\0', all the data are default
    // to 0, so just move data one position forward
    data = (char*)(((int)data + sizeof(int)) & (-sizeof(int)));
    expr_type = PTR;
  }
  else if (token == Sizeof) {
    match(Sizeof);
    match('(');
    expr_type = INT;

    if (token == Int) {
      match(Int);
    } else if (token == Char) {
      match(Char);
      expr_type = CHAR;
    }

    while (token == Mul) {
      match(Mul);
      expr_type = expr_type + PTR;
    }

    match(')');

    // emit code
    *++text = IMM;
    *++text = (expr_type == CHAR) ? sizeof(char) : sizeof(int);

    expr_type = INT;
  }
  else if (token == Id) {
    match(Id);
    id =  current_id;

    if (token == '(') {
      // function call
      match('(');

      // pass in arguments
      tmp = 0;              // number of arguments
      while (token != ')') {
        expression(Assign);
        *++text = PUSH;
        tmp++;

        if (token == ',') {
          match(',');
        }
      }
      match(')');

      // emit code
      if (id[Class] == Sys) {
        // system functions
        *++text = id[Value];
      }
      else if (id[Class] == Fun) {
        // function call
        *++text = CALL;
        *++text = id[Value];
      } else {
        printf("%d: bad function call\n", line);
        exit(-1);
      }

      // clean the stack for arguments
      if (tmp > 0) {
        *++text = ADJ;
        *++text = tmp;
      }
      expr_type = id[Type];
    }
    else if (id[Class] == Num) {
      // enum variable
      *++text = IMM;
      *++text = id[Value];
      expr_type = INT;
    } else {
      // variable
      if (id[Class] == Loc) {
        *++text = LEA;
        *++text = index_of_bp - id[Value];
      }
      else if (id[Class] == Glo) {
        *++text = IMM;
        *++text = id[Value];
      } else {
        printf("%d: undefined variable\n", line);
        exit(-1);
      }

      // emit code, default behaviour is to liad the value of the 
      // address which is store in `ax`
      expr_type = id[Type];
      *++text = (expr_type == Char) ? LC : LI;
    }
  }
  else if (token == '(') {
    // cast or parenthesis
    match('(');
    if (token == Int || token == Char) {
      tmp = (token == Char) ? CHAR : INT;
      match(token);
      while (token == Mul) {
        match(Mul);
        tmp = tmp + PTR;
      }
      match(')');

      expression(Inc);        // cast has precedence as Inc(++)

      expr_type = tmp;
    } else {
      expression(Assign);
      match(')');
    }
  }
  else if (token == Mul) {
    // dereference *<addr>
    match(Mul);
    expression(Inc);

    if (expr_type >= PTR) {
      expr_type = expr_type - PTR;
    } else {
      printf("%d: bad dereference\n", line);
      exit(-1);
    }

    *++text = (expr_type == CHAR) ? LC : LI;
  }
  else if (token == And) {
    // get the address of variable
    match(And);
    expression(Inc);

    if (*text == LC || *text == LI) {
      text--;
    } else {
      printf("%d: bad address of\n", line);
      exit(-1);
    }

    expr_type = expr_type + PTR;
  }
  else if (token == '!') {
    // logical operate
    match('!');
    expression(Inc);

    // emit code use <expr> == 0
    *++text = PUSH;
    *++text = IMM;
    *++text = 0;
    *++text = EQ;

    expr_type = INT;
  }
  else if (token == '~') {
    // bitwise not
    match('~');
    expression(Inc);

    // emit code use <expr> XOR -1
    *++text = PUSH;
    *++text = IMM;
    *++text = -1;
    *++text = XOR;

    expr_type = INT;
  }
  else if (token == Add) {
    // +var do nothing
    match(Add);
    expression(Inc);

    expr_type = INT;
  }
  else if (token == Sub) {
    // -var
    match(Sub);

    if (token == Num) {
      *++text == IMM;
      *++text == -token_val;
      match(Num);
    } else {
      *++text = IMM;
      *++text = -1;
      *++text = PUSH;
      expression(Inc);
      *++text = MUL;
    }

    expr_type = INT;
  }
  else if (token == Inc || token == Dec) {
    tmp = token;
    match(token);
    expression(Inc);

    if (*text == LC) {
      *text = PUSH;         // to duplicate the address
      *++text = LC;
    } else if (*text == LI) {
      *text = PUSH;
      *++text = LI;
    } else {
      printf("%d: bad lvalue of pre-increment\n", line);
      exit(-1);
    }
    *++text = PUSH;
    *++text = IMM;

    *++text = (expr_type > PTR) ? sizeof(int) : sizeof(char);
    *++text = (tmp == Inc) ? ADD : SUB;
    *++text = (expr_type == CHAR) ? SC : SI;
  }
  else {
    printf("%d: bad expression\n", line);
    exit(-1);
  }

  // binary operate and postfix operators
  while (token >= level) {
    tmp = expr_type;
    if (token == Assign) {
      // var = expr;
      match(Assign);
      if (*text == LC || *text == LI) {
        *text = PUSH;         // save the lvalue pointer
      } else {
        printf("%d: bad lvalue in assignment\n", line);
        exit(-1);
      }
      expression(Assign);

      expr_type = tmp;
      *++text = (expr_type == CHAR) ? SC : SI;
    }
    else if (token == Cond) {
      // expr ? a : b;
      match(Cond);
      *++text = JZ;
      addr = ++text;
      expression(Assign);
      if (token == ':') {
        match(':');
      } else {
        printf("%d: missing colon in conditional\n", line);
        exit(-1);
      }

      *addr = (int)(text + 3);
      *++text = JMP;
      addr = ++text;
      expression(Cond);
      *addr = (int)(text + 1);
    }
    else if (token == Lor) {
      // logical or
      match(Lor);
      *++text = JNZ;
      addr = ++text;
      expression(Lan);
      *addr = (int)(text + 1);
      expr_type = INT;
    }
    else if (token == Lan) {
      // logical and
      match(Lan);
      *++text = JZ;
      addr = ++text;
      expression(Or);
      *addr = (int)(text + 1);
      expr_type = INT;
    }
    else if (token == Or) {
      // bitwise or
      match(Or);
      *++text = PUSH;
      expression(Xor);
      *++text = OR;
      expr_type = INT;
    }
    else if (token == Xor) {
      // bitwise xor
      match(Xor);
      *++text = PUSH;
      expression(And);
      *++text = XOR;
      expr_type = INT;
    }
    else if (token == And) {
      // bitwise and
      match(And);
      *++text = PUSH;
      expression(Eq);
      *++text = AND;
      expr_type = INT;
    }
    else if (token == Eq) {
      // equal ==
      match(Eq);
      *++text = PUSH;
      expression(Ne);
      *++text = EQ;
      expr_type = INT;
    }
    else if (token == Ne) {
      // not equal !=
      match(Ne);
      *++text = PUSH;
      expression(Lt);
      *++text = NE;
      expr_type = INT;
    }
    else if (token == Lt) {
      // less than
      match(Lt);
      *++text = PUSH;
      expression(Shl);
      *++text = LT;
      expr_type = INT;
    }
    else if (token == Gt) {
      // greater than
      match(Gt);
      *++text = PUSH;
      expression(Shl);
      *++text = GT;
      expr_type = INT;
    }
    else if (token == Le) {
      // less or equal
      match(Le);
      *++text = PUSH;
      expression(Shl);
      *++text = LE;
      expr_type = INT;
    }
    else if (token == Ge) {
      // greater or equal
      match(Ge);
      *++text = PUSH;
      expression(Shl);
      *++text = GE;
      expr_type = INT;
    }
    else if (token == Shl) {
      // shift left
      match(Shl);
      *++text = PUSH;
      expression(Add);
      *++text = SHL;
      expr_type = INT;
    }
    else if (token == Shr) {
      // shift right
      match(Shr);
      *++text = PUSH;
      expression(Add);
      *++text = SHR;
      expr_type = INT;
    }
    else if (token == Add) {
      // add
      match(Add);
      *++text = PUSH;
      expression(Mul);

      expr_type = tmp;
      if (expr_type > PTR) {
        // pointer type, and not char *
        *++text = PUSH;
        *++text = IMM;
        *++text = sizeof(int);
        *++text = MUL;
      }
      *++text = ADD;
    }
    else if (token == Sub) {
      // sub
      match(Sub);
      *++text = PUSH;
      expression(Mul);
      if (tmp > PTR && tmp == expr_type) {
        // pointer subtraction
        *++text = SUB;
        *++text = PUSH;
        *++text = IMM;
        *++text = sizeof(int);
        *++text = DIV;
        expr_type = INT;
      }
      else if (expr_type > PTR) {
        // pointer movement
        *++text = PUSH;
        *++text = IMM;
        *++text = sizeof(int);
        *++text = MUL;
        *++text = SUB;
        expr_type = tmp;
      } else {
        // numeral subtraction
        *++text = SUB;
        expr_type = tmp;
      }
    }
    else if (token == Mul) {
      // multiply
      match(Mul);
      *++text = PUSH;
      expression(Inc);
      *++text = MUL;
      expr_type = tmp;
    }
    else if (token == Div) {
      // divide
      match(Div);
      *++text = PUSH;
      expression(Inc);
      *++text = DIV;
      expr_type = tmp;
    }
    else if (token == Mod) {
      // modulo
      match(Mod);
      *++text = PUSH;
      expression(Inc);
      *++text = MOD;
      expr_type = tmp;
    }
    else if (token == Inc || token == Dec) {
      // postfix inc(++) and dec(--)
      // we will increase the value to the variable and decrease it
      // on `ax` to get its original value
      if (*text == LI) {
        *text = PUSH;
        *++text = LI;
      }
      else if (*text == LC) {
        *text = PUSH;
        *++text = LC;
      }
      else {
        printf("%d: bad value in increment\n", line);
        exit(-1);
      }

      *++text = PUSH;
      *++text = IMM;

      *++text = (expr_type > PTR) ? sizeof(int) : sizeof(char);
      *++text = (token == Inc) ? ADD : SUB;
      *++text = (expr_type == CHAR) ? SC : SI;
      *++text = PUSH;
      *++text = IMM;
      *++text = (expr_type > PTR) ? sizeof(int) : sizeof(char);
      *++text = (token == Inc) ? SUB : ADD;\
      match(token);
    }
    else if (token == Brak) {
      // array access var[xx]
      match(Brak);
      *++text = PUSH;
      expression(Assign);
      match(']');

      if (tmp > PTR) {
        // pointer, `not char *`
        *++text = PUSH;
        *++text = IMM;
        *++text = sizeof(int);
        *++text = MUL;
      }
      else if (tmp < PTR) {
        printf("%d: pointer type expected\n", line);
        exit(-1);
      }
      expr_type = tmp - PTR;
      *++text = ADD;
      *++text = (expr_type == CHAR) ? LC : LI;
    }
    else {
      printf("%d: compiler error, token = %d\n", line, token);
      exit(-1);
    }
  }
}

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
void statement() {
  int* a, *b;
  // if (...) <statement> [else <statement>]
  //    if (<cond>)               <cond>
  //                              JZ a
  //      <true_statement>        <true_statement>
  //    else                      JMP b
  //  a:  
  //      <false_statement>       <false_statement>
  //  b:

  if (token == If) {
    match(If);
    match('(');
    expression(Assign);         // parse condition
    match(')');

    *++text = JZ;
    b = ++text;

    statement();                // parse statement
    if (token == Else) {
      match(Else);

      // emit code for JMP b
      *b = (int)(text + 3);
      *++text = JMP;
      b = ++text;

      statement();
    }

    *b = (int)(text + 1);
  }

  // a: 
  //    while (<cond>)              <cond>
  //                                JZ b
  //      <statement>               <statement>
  //                                JMP a
  // b:
  
  else if (token == While) {
    match(While);
    a = text + 1;
    match('(');
    expression(Assign);
    match(')');
    *++text = JZ;
    b = ++text;

    statement();

    *++text = JMP;
    *++text = (int)a;
    *b = (int)(text + 1);
  }

  else if (token == Return) {
    match(Return);
    if (token != ';') {
      expression(Assign);
    }

    match(';');

    //  emit code for return
    *++text = LEV;
  }

  else if (token == '{') {
    // { <statement> ...
    match('{');

    while (token != '}') {
      statement();
    }
    match('}');
  }

  else if (token == ';') {
    // empty statement
    match(';');
  }

  else {
    // a = b; or function_call();
    expression(Assign);
    match(';');
  }
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
    statement();
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
  int *tmp;

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

  // setup stack
  sp = (int*)((int)stack + poolsize);
  *--sp = EXIT;
  *--sp = PUSH;
  tmp = sp;
  *--sp = argc;
  *--sp = (int)argv;
  *--sp = (int) tmp;

  return eval();
}
