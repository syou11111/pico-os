#include "cc.h"
#include "../kernel/kernel.h"
#include "../drivers/st7735.h"
#include "../drivers/hw_io.h"
#include "../ui/console.h"
#include "../fs/fs.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>

extern volatile bool g_led_auto_blink;
extern void set_onboard_led(bool on);

/* ---------- Bytecode Executable Format (PXE1) ---------- */
#define PXE_MAGIC       0x31455850UL /* "PXE1" */
#define CC_MAX_CODE     768
#define CC_MAX_DATA     1024
#define CC_MAX_SYMS     64
#define CC_MAX_MACROS   64
#define CC_MAX_PATCHES  64
#define CC_MAX_LOOPS    8
#define CC_MAX_LOOP_JMP 16

#define VM_MEM_SIZE     8192
#define VM_ESTACK_SIZE  128
#define VM_CSTACK_SIZE  64

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t code_count;
    uint16_t data_size;
    uint16_t main_pc;
    uint16_t global_bytes;
} pxe_header_t;

enum {
    OP_HALT = 0,
    OP_IMM,
    OP_IMM32,
    OP_ADDR_G,
    OP_ADDR_L,
    OP_ADDR_D,
    OP_LOAD32,
    OP_LOAD8,
    OP_STORE32,
    OP_STORE8,
    OP_DUP,
    OP_POP,
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_SHL,
    OP_SHR,
    OP_AND,
    OP_OR,
    OP_XOR,
    OP_EQ,
    OP_NE,
    OP_LT,
    OP_LE,
    OP_GT,
    OP_GE,
    OP_NEG,
    OP_NOT,
    OP_BNOT,
    OP_JMP,
    OP_JZ,
    OP_JNZ,
    OP_CALL,
    OP_ENTER,
    OP_RET,
    OP_RETV,
    OP_SYSCALL
};

enum {
    SYS_PRINTF = 0,
    SYS_PUTS,
    SYS_PUTCHAR,
    SYS_GETCHAR,
    SYS_SLEEP_MS,
    SYS_TICKS,
    SYS_LED,
    SYS_BACKLIGHT,
    SYS_CLS,
    SYS_RECT,
    SYS_TEXT,
    SYS_RGB,
    SYS_RAND,
    SYS_PIXEL,
    SYS_KEY_POLL,
    SYS_PIN_MODE,
    SYS_DIGITAL_WRITE,
    SYS_DIGITAL_READ,
    SYS_GPIO_TOGGLE,
    SYS_PWM_WRITE,
    SYS_TONE,
    SYS_NO_TONE,
    SYS_ADC_READ,
    SYS_TEMP_C,
    SYS_I2C_INIT,
    SYS_I2C_WRITE,
    SYS_I2C_READ,
    SYS_I2C_WRITE_REG,
    SYS_I2C_READ_REG,
    SYS_SPI_OPEN,
    SYS_SPI_TRANSFER,
    SYS_SPI_BYTE
};

static inline uint32_t enc_instr(uint8_t op, int32_t imm) {
    return ((uint32_t)op & 0xFFU) | (((uint32_t)imm) << 8);
}

static inline uint8_t dec_op(uint32_t instr) {
    return (uint8_t)(instr & 0xFFU);
}

static inline int32_t dec_imm(uint32_t instr) {
    int32_t imm = (int32_t)(instr >> 8);
    if (imm & 0x00800000L) {
        imm |= (int32_t)0xFF000000UL;
    }
    return imm;
}

/* ---------- Lexer Tokens & Types ---------- */
typedef enum {
    TOK_EOF = 0,
    TOK_NUM,
    TOK_STR,
    TOK_ID,
    TOK_INT,
    TOK_CHAR,
    TOK_VOID,
    TOK_IF,
    TOK_ELSE,
    TOK_WHILE,
    TOK_FOR,
    TOK_RETURN,
    TOK_BREAK,
    TOK_CONTINUE,
    TOK_ASSIGN,      /* = */
    TOK_ADD_ASSIGN,  /* += */
    TOK_SUB_ASSIGN,  /* -= */
    TOK_MUL_ASSIGN,  /* *= */
    TOK_DIV_ASSIGN,  /* /= */
    TOK_MOD_ASSIGN,  /* %= */
    TOK_OR_OR,       /* || */
    TOK_AND_AND,     /* && */
    TOK_OR,          /* | */
    TOK_XOR,         /* ^ */
    TOK_AND,         /* & */
    TOK_EQ,          /* == */
    TOK_NE,          /* != */
    TOK_LT,          /* < */
    TOK_LE,          /* <= */
    TOK_GT,          /* > */
    TOK_GE,          /* >= */
    TOK_SHL,         /* << */
    TOK_SHR,         /* >> */
    TOK_PLUS,        /* + */
    TOK_MINUS,       /* - */
    TOK_STAR,        /* * */
    TOK_SLASH,       /* / */
    TOK_PERCENT,     /* % */
    TOK_INC,         /* ++ */
    TOK_DEC,         /* -- */
    TOK_NOT,         /* ! */
    TOK_BNOT,        /* ~ */
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LBRACK,
    TOK_RBRACK,
    TOK_SEMI,
    TOK_COMMA
} tok_t;

typedef enum {
    TYPE_VOID = 0,
    TYPE_CHAR,
    TYPE_INT
} base_type_t;

typedef struct {
    base_type_t base;
    int ptr; /* 0 = value, 1+ = pointer */
} ctype_t;

typedef enum {
    SYM_GLOBAL = 0,
    SYM_LOCAL,
    SYM_FUNC
} sym_kind_t;

typedef struct {
    char name[24];
    sym_kind_t kind;
    ctype_t type;
    bool is_array;
    bool defined;
    int32_t addr;    /* global offset, local bp offset, or function pc */
    int argc;        /* for SYM_FUNC */
} sym_t;

typedef struct {
    char name[24];
    int32_t val;
} macro_t;

typedef struct {
    uint16_t pc;
    uint8_t sym_idx;
    uint8_t argc;
} call_patch_t;

typedef struct {
    uint16_t break_pcs[CC_MAX_LOOP_JMP];
    int break_cnt;
    uint16_t cont_pcs[CC_MAX_LOOP_JMP];
    int cont_cnt;
    uint16_t cont_target;
    bool use_cont_target;
} loop_ctx_t;

typedef struct {
    bool is_lvalue;
    ctype_t type;
    bool is_array;
} expr_res_t;

/* ---------- Static Compiler & VM Buffers ---------- */
static char     src_buf[FS_MAX_FILESIZE + 1];
static uint8_t  bin_buf[FS_MAX_FILESIZE];
static uint32_t code_buf[CC_MAX_CODE];
static uint8_t  data_buf[CC_MAX_DATA];
static sym_t    syms[CC_MAX_SYMS];
static macro_t  macros[CC_MAX_MACROS];
static call_patch_t call_patches[CC_MAX_PATCHES];
static loop_ctx_t   loops[CC_MAX_LOOPS];

static uint8_t  vm_mem[VM_MEM_SIZE];
static int32_t  vm_estack[VM_ESTACK_SIZE];
static struct {
    uint16_t ret_pc;
    uint16_t prev_bp;
    uint16_t prev_sp;
} vm_cstack[VM_CSTACK_SIZE];

/* ---------- Compiler State ---------- */
static const char *lex_p;
static int lex_line;
static tok_t tok;
static int32_t tok_num;
static char tok_str[128];

static uint16_t code_len;
static uint16_t data_len;
static uint16_t global_bytes;
static uint16_t local_bytes;
static uint16_t max_local_bytes;
static int sym_count;
static int macro_count;
static int patch_count;
static int loop_depth;
static bool compile_err;
static char err_msg[96];

static void cc_error(const char *fmt, ...) {
    if (compile_err) return;
    compile_err = true;
    char msg[72];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    snprintf(err_msg, sizeof(err_msg), "line %d: %s", lex_line, msg);
}

static uint16_t emit(uint8_t op, int32_t imm) {
    if (compile_err) return 0;
    if (code_len >= CC_MAX_CODE) {
        cc_error("code size overflow");
        return 0;
    }
    uint16_t pc = code_len;
    code_buf[code_len++] = enc_instr(op, imm);
    return pc;
}

static void emit_imm(int32_t val) {
    if (val >= -8388608L && val <= 8388607L) {
        emit(OP_IMM, val);
    } else {
        emit(OP_IMM32, 0);
        if (code_len >= CC_MAX_CODE) {
            cc_error("code size overflow");
            return;
        }
        code_buf[code_len++] = (uint32_t)val;
    }
}

static void patch_jmp(uint16_t pc, uint16_t target) {
    if (pc < code_len) {
        uint8_t op = dec_op(code_buf[pc]);
        code_buf[pc] = enc_instr(op, (int32_t)target);
    }
}

static uint16_t add_string_literal(const char *s, size_t len) {
    if (data_len + len + 1 > CC_MAX_DATA) {
        cc_error("string data overflow");
        return 0;
    }
    uint16_t off = data_len;
    memcpy(data_buf + data_len, s, len);
    data_buf[data_len + len] = '\0';
    data_len = (uint16_t)(data_len + len + 1);
    return off;
}

static void add_macro(const char *name, int32_t val) {
    for (int i = 0; i < macro_count; i++) {
        if (strcmp(macros[i].name, name) == 0) {
            macros[i].val = val;
            return;
        }
    }
    if (macro_count < CC_MAX_MACROS) {
        strncpy(macros[macro_count].name, name, 23);
        macros[macro_count].name[23] = '\0';
        macros[macro_count].val = val;
        macro_count++;
    }
}

static void inject_builtin_header_macros(const char *hdr) {
    if (strcmp(hdr, "gpio.h") == 0 || strcmp(hdr, "pwm.h") == 0 ||
        strcmp(hdr, "adc.h") == 0 || strcmp(hdr, "i2c.h") == 0 ||
        strcmp(hdr, "spi.h") == 0 || strcmp(hdr, "picoos.h") == 0) {
        add_macro("INPUT", HW_INPUT);
        add_macro("OUTPUT", HW_OUTPUT);
        add_macro("INPUT_PULLUP", HW_INPUT_PULLUP);
        add_macro("INPUT_PULLDOWN", HW_INPUT_PULLDOWN);
        add_macro("LOW", 0);
        add_macro("HIGH", 1);
        add_macro("I2C0", 0);
        add_macro("I2C1", 1);
        add_macro("SPI0", 0);
        add_macro("SPI1", 1);
        add_macro("KEY_UP", 1000);
        add_macro("KEY_DOWN", 1001);
        add_macro("KEY_RIGHT", 1002);
        add_macro("KEY_LEFT", 1003);
        add_macro("KEY_ESC", 27);
    }
}

/* ---------- Lexer ---------- */
static void skip_ws_and_comments(void) {
    while (*lex_p) {
        if (*lex_p == ' ' || *lex_p == '\t' || *lex_p == '\r') {
            lex_p++;
        } else if (*lex_p == '\n') {
            lex_p++;
            lex_line++;
        } else if (lex_p[0] == '/' && lex_p[1] == '/') {
            lex_p += 2;
            while (*lex_p && *lex_p != '\n') lex_p++;
        } else if (lex_p[0] == '/' && lex_p[1] == '*') {
            lex_p += 2;
            while (*lex_p && !(lex_p[0] == '*' && lex_p[1] == '/')) {
                if (*lex_p == '\n') lex_line++;
                lex_p++;
            }
            if (*lex_p) lex_p += 2;
        } else if (*lex_p == '#') {
            /* Preprocessor directive */
            lex_p++;
            while (*lex_p == ' ' || *lex_p == '\t') lex_p++;
            if (strncmp(lex_p, "include", 7) == 0 &&
                (lex_p[7] == ' ' || lex_p[7] == '\t' || lex_p[7] == '<' || lex_p[7] == '"')) {
                lex_p += 7;
                while (*lex_p == ' ' || *lex_p == '\t') lex_p++;
                if (*lex_p == '<' || *lex_p == '"') {
                    char endc = (*lex_p == '<') ? '>' : '"';
                    lex_p++;
                    char hname[32];
                    int hi = 0;
                    while (*lex_p && *lex_p != endc && *lex_p != '\n' && hi < 31) {
                        hname[hi++] = *lex_p++;
                    }
                    hname[hi] = '\0';
                    inject_builtin_header_macros(hname);
                }
            } else if (strncmp(lex_p, "define", 6) == 0 && (lex_p[6] == ' ' || lex_p[6] == '\t')) {
                lex_p += 6;
                while (*lex_p == ' ' || *lex_p == '\t') lex_p++;
                char mname[24];
                int mi = 0;
                while ((isalnum((unsigned char)*lex_p) || *lex_p == '_') && mi < 23) {
                    mname[mi++] = *lex_p++;
                }
                mname[mi] = '\0';
                while (*lex_p == ' ' || *lex_p == '\t') lex_p++;
                int sign = 1;
                if (*lex_p == '-') { sign = -1; lex_p++; }
                while (*lex_p == ' ' || *lex_p == '\t') lex_p++;
                int32_t mval = 0;
                if (isdigit((unsigned char)*lex_p)) {
                    char *endp = NULL;
                    mval = (int32_t)strtol(lex_p, &endp, 0) * sign;
                    lex_p = endp;
                }
                if (mname[0] != '\0') {
                    add_macro(mname, mval);
                }
            }
            while (*lex_p && *lex_p != '\n') lex_p++;
        } else {
            break;
        }
    }
}

static char parse_escape(void) {
    if (*lex_p != '\\') return *lex_p++;
    lex_p++;
    char c = *lex_p ? *lex_p++ : '\0';
    switch (c) {
        case 'n': return '\n';
        case 'r': return '\r';
        case 't': return '\t';
        case '0': return '\0';
        case '\\': return '\\';
        case '\'': return '\'';
        case '"': return '"';
        default: return c;
    }
}

static void next_tok(void) {
    skip_ws_and_comments();
    if (compile_err) { tok = TOK_EOF; return; }
    if (*lex_p == '\0') { tok = TOK_EOF; return; }

    char c = *lex_p;
    if (isdigit((unsigned char)c)) {
        char *endp = NULL;
        tok_num = (int32_t)strtoul(lex_p, &endp, 0);
        lex_p = endp;
        tok = TOK_NUM;
        return;
    }
    if (c == '\'') {
        lex_p++;
        tok_num = (unsigned char)parse_escape();
        if (*lex_p == '\'') lex_p++;
        else cc_error("unterminated char literal");
        tok = TOK_NUM;
        return;
    }
    if (c == '"') {
        lex_p++;
        size_t len = 0;
        while (*lex_p && *lex_p != '"' && *lex_p != '\n') {
            char ch = parse_escape();
            if (len < sizeof(tok_str) - 1) {
                tok_str[len++] = ch;
            }
        }
        tok_str[len] = '\0';
        if (*lex_p == '"') lex_p++;
        else cc_error("unterminated string");
        tok_num = (int32_t)len;
        tok = TOK_STR;
        return;
    }
    if (isalpha((unsigned char)c) || c == '_') {
        size_t len = 0;
        while (isalnum((unsigned char)*lex_p) || *lex_p == '_') {
            if (len < sizeof(tok_str) - 1) tok_str[len++] = *lex_p;
            lex_p++;
        }
        tok_str[len] = '\0';

        if (strcmp(tok_str, "int") == 0)      { tok = TOK_INT; return; }
        if (strcmp(tok_str, "char") == 0)     { tok = TOK_CHAR; return; }
        if (strcmp(tok_str, "void") == 0)     { tok = TOK_VOID; return; }
        if (strcmp(tok_str, "if") == 0)       { tok = TOK_IF; return; }
        if (strcmp(tok_str, "else") == 0)     { tok = TOK_ELSE; return; }
        if (strcmp(tok_str, "while") == 0)    { tok = TOK_WHILE; return; }
        if (strcmp(tok_str, "for") == 0)      { tok = TOK_FOR; return; }
        if (strcmp(tok_str, "return") == 0)   { tok = TOK_RETURN; return; }
        if (strcmp(tok_str, "break") == 0)    { tok = TOK_BREAK; return; }
        if (strcmp(tok_str, "continue") == 0) { tok = TOK_CONTINUE; return; }

        for (int i = macro_count - 1; i >= 0; i--) {
            if (strcmp(macros[i].name, tok_str) == 0) {
                tok_num = macros[i].val;
                tok = TOK_NUM;
                return;
            }
        }
        tok = TOK_ID;
        return;
    }

    lex_p++;
    switch (c) {
        case '=': if (*lex_p == '=') { lex_p++; tok = TOK_EQ; } else tok = TOK_ASSIGN; return;
        case '!': if (*lex_p == '=') { lex_p++; tok = TOK_NE; } else tok = TOK_NOT; return;
        case '<':
            if (*lex_p == '=') { lex_p++; tok = TOK_LE; }
            else if (*lex_p == '<') { lex_p++; tok = TOK_SHL; }
            else tok = TOK_LT;
            return;
        case '>':
            if (*lex_p == '=') { lex_p++; tok = TOK_GE; }
            else if (*lex_p == '>') { lex_p++; tok = TOK_SHR; }
            else tok = TOK_GT;
            return;
        case '+':
            if (*lex_p == '+') { lex_p++; tok = TOK_INC; }
            else if (*lex_p == '=') { lex_p++; tok = TOK_ADD_ASSIGN; }
            else tok = TOK_PLUS;
            return;
        case '-':
            if (*lex_p == '-') { lex_p++; tok = TOK_DEC; }
            else if (*lex_p == '=') { lex_p++; tok = TOK_SUB_ASSIGN; }
            else tok = TOK_MINUS;
            return;
        case '*': if (*lex_p == '=') { lex_p++; tok = TOK_MUL_ASSIGN; } else tok = TOK_STAR; return;
        case '/': if (*lex_p == '=') { lex_p++; tok = TOK_DIV_ASSIGN; } else tok = TOK_SLASH; return;
        case '%': if (*lex_p == '=') { lex_p++; tok = TOK_MOD_ASSIGN; } else tok = TOK_PERCENT; return;
        case '&': if (*lex_p == '&') { lex_p++; tok = TOK_AND_AND; } else tok = TOK_AND; return;
        case '|': if (*lex_p == '|') { lex_p++; tok = TOK_OR_OR; } else tok = TOK_OR; return;
        case '^': tok = TOK_XOR; return;
        case '~': tok = TOK_BNOT; return;
        case '(': tok = TOK_LPAREN; return;
        case ')': tok = TOK_RPAREN; return;
        case '{': tok = TOK_LBRACE; return;
        case '}': tok = TOK_RBRACE; return;
        case '[': tok = TOK_LBRACK; return;
        case ']': tok = TOK_RBRACK; return;
        case ';': tok = TOK_SEMI; return;
        case ',': tok = TOK_COMMA; return;
        default: cc_error("unexpected char '%c'", c); tok = TOK_EOF; return;
    }
}

static bool expect(tok_t t, const char *msg) {
    if (tok != t) {
        cc_error("expected %s", msg);
        return false;
    }
    next_tok();
    return true;
}

static bool is_type_tok(tok_t t) {
    return (t == TOK_INT || t == TOK_CHAR || t == TOK_VOID);
}

static ctype_t parse_type(void) {
    ctype_t ct = { TYPE_INT, 0 };
    if (tok == TOK_INT) ct.base = TYPE_INT;
    else if (tok == TOK_CHAR) ct.base = TYPE_CHAR;
    else if (tok == TOK_VOID) ct.base = TYPE_VOID;
    else cc_error("expected type");
    next_tok();
    while (tok == TOK_STAR) {
        ct.ptr++;
        next_tok();
    }
    return ct;
}

static int find_sym(const char *name) {
    for (int i = sym_count - 1; i >= 0; i--) {
        if (strcmp(syms[i].name, name) == 0) return i;
    }
    return -1;
}

static int add_sym(const char *name, sym_kind_t kind, ctype_t type, bool is_arr, int32_t addr, bool def) {
    if (sym_count >= CC_MAX_SYMS) {
        cc_error("too many symbols");
        return -1;
    }
    int idx = sym_count++;
    strncpy(syms[idx].name, name, 23);
    syms[idx].name[23] = '\0';
    syms[idx].kind = kind;
    syms[idx].type = type;
    syms[idx].is_array = is_arr;
    syms[idx].addr = addr;
    syms[idx].defined = def;
    syms[idx].argc = 0;
    return idx;
}

static int elem_size(ctype_t t) {
    return (t.ptr == 0 && t.base == TYPE_CHAR) ? 1 : 4;
}

static int ptr_step(ctype_t t) {
    if (t.ptr > 1) return 4;
    if (t.ptr == 1) return (t.base == TYPE_CHAR) ? 1 : 4;
    return 1;
}

static void emit_load(ctype_t t) {
    emit(elem_size(t) == 1 ? OP_LOAD8 : OP_LOAD32, 0);
}

static void emit_store(ctype_t t) {
    emit(elem_size(t) == 1 ? OP_STORE8 : OP_STORE32, 0);
}

static void to_rvalue(expr_res_t *e) {
    if (e->is_lvalue && !e->is_array) {
        emit_load(e->type);
        e->is_lvalue = false;
    }
}

/* ---------- Builtin Syscall Lookup ---------- */
static int lookup_syscall(const char *name) {
    if (strcmp(name, "printf") == 0)    return SYS_PRINTF;
    if (strcmp(name, "puts") == 0)      return SYS_PUTS;
    if (strcmp(name, "putchar") == 0)   return SYS_PUTCHAR;
    if (strcmp(name, "getchar") == 0)   return SYS_GETCHAR;
    if (strcmp(name, "sleep_ms") == 0 || strcmp(name, "delay") == 0) return SYS_SLEEP_MS;
    if (strcmp(name, "ticks") == 0)         return SYS_TICKS;
    if (strcmp(name, "led") == 0)           return SYS_LED;
    if (strcmp(name, "backlight") == 0)     return SYS_BACKLIGHT;
    if (strcmp(name, "cls") == 0)           return SYS_CLS;
    if (strcmp(name, "rect") == 0)          return SYS_RECT;
    if (strcmp(name, "text") == 0)          return SYS_TEXT;
    if (strcmp(name, "rgb") == 0)           return SYS_RGB;
    if (strcmp(name, "rand") == 0)          return SYS_RAND;
    if (strcmp(name, "pixel") == 0)         return SYS_PIXEL;
    if (strcmp(name, "key_poll") == 0)      return SYS_KEY_POLL;
    if (strcmp(name, "pin_mode") == 0)      return SYS_PIN_MODE;
    if (strcmp(name, "digital_write") == 0) return SYS_DIGITAL_WRITE;
    if (strcmp(name, "digital_read") == 0)  return SYS_DIGITAL_READ;
    if (strcmp(name, "gpio_toggle") == 0)   return SYS_GPIO_TOGGLE;
    if (strcmp(name, "pwm_write") == 0)     return SYS_PWM_WRITE;
    if (strcmp(name, "tone") == 0)          return SYS_TONE;
    if (strcmp(name, "no_tone") == 0)       return SYS_NO_TONE;
    if (strcmp(name, "adc_read") == 0)      return SYS_ADC_READ;
    if (strcmp(name, "temp_c") == 0)        return SYS_TEMP_C;
    if (strcmp(name, "i2c_init") == 0)      return SYS_I2C_INIT;
    if (strcmp(name, "i2c_write") == 0)     return SYS_I2C_WRITE;
    if (strcmp(name, "i2c_read") == 0)      return SYS_I2C_READ;
    if (strcmp(name, "i2c_write_reg") == 0) return SYS_I2C_WRITE_REG;
    if (strcmp(name, "i2c_read_reg") == 0)  return SYS_I2C_READ_REG;
    if (strcmp(name, "spi_open") == 0)      return SYS_SPI_OPEN;
    if (strcmp(name, "spi_transfer") == 0)  return SYS_SPI_TRANSFER;
    if (strcmp(name, "spi_byte") == 0)      return SYS_SPI_BYTE;
    return -1;
}

/* ---------- Forward Declarations of Parser ---------- */
static expr_res_t parse_expr(void);
static expr_res_t parse_assign(void);
static void parse_stmt(void);
static void parse_block(void);

static expr_res_t parse_primary(void) {
    expr_res_t res = { false, { TYPE_INT, 0 }, false };
    if (compile_err) return res;

    if (tok == TOK_NUM) {
        emit_imm(tok_num);
        next_tok();
        return res;
    }
    if (tok == TOK_STR) {
        uint16_t off = add_string_literal(tok_str, (size_t)tok_num);
        emit(OP_ADDR_D, off);
        res.type.base = TYPE_CHAR;
        res.type.ptr = 1;
        next_tok();
        return res;
    }
    if (tok == TOK_LPAREN) {
        next_tok();
        res = parse_expr();
        expect(TOK_RPAREN, "')'");
        return res;
    }
    if (tok == TOK_ID) {
        char name[24];
        strncpy(name, tok_str, 23);
        name[23] = '\0';
        next_tok();

        if (tok == TOK_LPAREN) {
            /* Function or syscall call */
            next_tok();
            int argc = 0;
            if (tok != TOK_RPAREN) {
                while (!compile_err) {
                    expr_res_t a = parse_assign();
                    to_rvalue(&a);
                    argc++;
                    if (tok == TOK_COMMA) next_tok();
                    else break;
                }
            }
            expect(TOK_RPAREN, "')'");

            int sys_id = lookup_syscall(name);
            if (sys_id >= 0) {
                emit(OP_SYSCALL, (argc << 12) | sys_id);
                res.type.base = TYPE_INT;
                res.type.ptr = 0;
                return res;
            }

            int sidx = find_sym(name);
            if (sidx < 0 || syms[sidx].kind != SYM_FUNC) {
                /* Implicit forward function */
                ctype_t ft = { TYPE_INT, 0 };
                sidx = add_sym(name, SYM_FUNC, ft, false, 0, false);
                if (sidx < 0) return res;
                syms[sidx].argc = argc;
            }
            if (syms[sidx].defined) {
                emit(OP_CALL, (argc << 16) | (syms[sidx].addr & 0xFFFF));
            } else {
                uint16_t cpc = emit(OP_CALL, 0);
                if (patch_count < CC_MAX_PATCHES) {
                    call_patches[patch_count].pc = cpc;
                    call_patches[patch_count].sym_idx = (uint8_t)sidx;
                    call_patches[patch_count].argc = (uint8_t)argc;
                    patch_count++;
                }
            }
            res.type = syms[sidx].type;
            return res;
        }

        int sidx = find_sym(name);
        if (sidx < 0) {
            cc_error("undeclared '%s'", name);
            return res;
        }
        sym_t *s = &syms[sidx];
        if (s->kind == SYM_GLOBAL) {
            emit(OP_ADDR_G, s->addr);
        } else if (s->kind == SYM_LOCAL) {
            emit(OP_ADDR_L, s->addr);
        } else {
            cc_error("invalid use of function '%s'", name);
            return res;
        }
        res.type = s->type;
        res.is_array = s->is_array;
        res.is_lvalue = !s->is_array;
        if (s->is_array) {
            res.type.ptr++;
        }
        return res;
    }

    cc_error("unexpected token in expression");
    return res;
}

static expr_res_t parse_postfix(void) {
    expr_res_t res = parse_primary();
    while (!compile_err) {
        if (tok == TOK_LBRACK) {
            next_tok();
            to_rvalue(&res);
            expr_res_t idx = parse_expr();
            to_rvalue(&idx);
            expect(TOK_RBRACK, "']'");
            int step = ptr_step(res.type);
            if (step > 1) {
                emit_imm(step);
                emit(OP_MUL, 0);
            }
            emit(OP_ADD, 0);
            if (res.type.ptr > 0) res.type.ptr--;
            res.is_array = false;
            res.is_lvalue = true;
        } else if (tok == TOK_INC || tok == TOK_DEC) {
            bool is_inc = (tok == TOK_INC);
            next_tok();
            if (!res.is_lvalue || res.is_array) {
                cc_error("lvalue required for ++/--");
                return res;
            }
            int step = ptr_step(res.type);
            /* [addr] -> [addr, addr] -> [addr, old] -> [old, addr, old] -> [old, addr, new] -> store -> [new] -> Wait: postfix returns OLD! */
            /* Keep old on stack cleanly: [addr] -> DUP -> LOAD (old) ... wait, let's compute new then subtract step! */
            emit(OP_DUP, 0);
            emit_load(res.type);
            emit_imm(is_inc ? step : -step);
            emit(OP_ADD, 0);
            emit_store(res.type);
            emit_imm(is_inc ? step : -step);
            emit(OP_SUB, 0);
            res.is_lvalue = false;
        } else {
            break;
        }
    }
    return res;
}

static expr_res_t parse_unary(void) {
    if (tok == TOK_MINUS) {
        next_tok();
        expr_res_t e = parse_unary();
        to_rvalue(&e);
        emit(OP_NEG, 0);
        return e;
    }
    if (tok == TOK_PLUS) {
        next_tok();
        expr_res_t e = parse_unary();
        to_rvalue(&e);
        return e;
    }
    if (tok == TOK_NOT) {
        next_tok();
        expr_res_t e = parse_unary();
        to_rvalue(&e);
        emit(OP_NOT, 0);
        e.type.base = TYPE_INT;
        e.type.ptr = 0;
        return e;
    }
    if (tok == TOK_BNOT) {
        next_tok();
        expr_res_t e = parse_unary();
        to_rvalue(&e);
        emit(OP_BNOT, 0);
        return e;
    }
    if (tok == TOK_AND) {
        next_tok();
        expr_res_t e = parse_unary();
        if (!e.is_lvalue && !e.is_array) {
            cc_error("lvalue required for '&'");
            return e;
        }
        e.is_lvalue = false;
        e.is_array = false;
        e.type.ptr++;
        return e;
    }
    if (tok == TOK_STAR) {
        next_tok();
        expr_res_t e = parse_unary();
        to_rvalue(&e);
        if (e.type.ptr == 0) {
            cc_error("cannot dereference non-pointer");
            return e;
        }
        e.type.ptr--;
        e.is_lvalue = true;
        e.is_array = false;
        return e;
    }
    if (tok == TOK_INC || tok == TOK_DEC) {
        bool is_inc = (tok == TOK_INC);
        next_tok();
        expr_res_t e = parse_unary();
        if (!e.is_lvalue || e.is_array) {
            cc_error("lvalue required for ++/--");
            return e;
        }
        int step = ptr_step(e.type);
        emit(OP_DUP, 0);
        emit_load(e.type);
        emit_imm(is_inc ? step : -step);
        emit(OP_ADD, 0);
        emit_store(e.type);
        e.is_lvalue = false;
        return e;
    }
    return parse_postfix();
}

static expr_res_t parse_mul(void) {
    expr_res_t l = parse_unary();
    while (tok == TOK_STAR || tok == TOK_SLASH || tok == TOK_PERCENT) {
        tok_t op = tok;
        next_tok();
        to_rvalue(&l);
        expr_res_t r = parse_unary();
        to_rvalue(&r);
        emit(op == TOK_STAR ? OP_MUL : (op == TOK_SLASH ? OP_DIV : OP_MOD), 0);
    }
    return l;
}

static expr_res_t parse_add(void) {
    expr_res_t l = parse_mul();
    while (tok == TOK_PLUS || tok == TOK_MINUS) {
        tok_t op = tok;
        next_tok();
        to_rvalue(&l);
        expr_res_t r = parse_mul();
        to_rvalue(&r);
        int step = ptr_step(l.type);
        if (l.type.ptr > 0 && r.type.ptr == 0 && step > 1) {
            emit_imm(step);
            emit(OP_MUL, 0);
        }
        emit(op == TOK_PLUS ? OP_ADD : OP_SUB, 0);
    }
    return l;
}

static expr_res_t parse_shift(void) {
    expr_res_t l = parse_add();
    while (tok == TOK_SHL || tok == TOK_SHR) {
        tok_t op = tok;
        next_tok();
        to_rvalue(&l);
        expr_res_t r = parse_add();
        to_rvalue(&r);
        emit(op == TOK_SHL ? OP_SHL : OP_SHR, 0);
    }
    return l;
}

static expr_res_t parse_rel(void) {
    expr_res_t l = parse_shift();
    while (tok == TOK_LT || tok == TOK_LE || tok == TOK_GT || tok == TOK_GE) {
        tok_t op = tok;
        next_tok();
        to_rvalue(&l);
        expr_res_t r = parse_shift();
        to_rvalue(&r);
        emit(op == TOK_LT ? OP_LT : (op == TOK_LE ? OP_LE : (op == TOK_GT ? OP_GT : OP_GE)), 0);
        l.type.base = TYPE_INT;
        l.type.ptr = 0;
    }
    return l;
}

static expr_res_t parse_eq(void) {
    expr_res_t l = parse_rel();
    while (tok == TOK_EQ || tok == TOK_NE) {
        tok_t op = tok;
        next_tok();
        to_rvalue(&l);
        expr_res_t r = parse_rel();
        to_rvalue(&r);
        emit(op == TOK_EQ ? OP_EQ : OP_NE, 0);
        l.type.base = TYPE_INT;
        l.type.ptr = 0;
    }
    return l;
}

static expr_res_t parse_bitand(void) {
    expr_res_t l = parse_eq();
    while (tok == TOK_AND) {
        next_tok();
        to_rvalue(&l);
        expr_res_t r = parse_eq();
        to_rvalue(&r);
        emit(OP_AND, 0);
    }
    return l;
}

static expr_res_t parse_bitxor(void) {
    expr_res_t l = parse_bitand();
    while (tok == TOK_XOR) {
        next_tok();
        to_rvalue(&l);
        expr_res_t r = parse_bitand();
        to_rvalue(&r);
        emit(OP_XOR, 0);
    }
    return l;
}

static expr_res_t parse_bitor(void) {
    expr_res_t l = parse_bitxor();
    while (tok == TOK_OR) {
        next_tok();
        to_rvalue(&l);
        expr_res_t r = parse_bitxor();
        to_rvalue(&r);
        emit(OP_OR, 0);
    }
    return l;
}

static expr_res_t parse_land(void) {
    expr_res_t l = parse_bitor();
    while (tok == TOK_AND_AND) {
        next_tok();
        to_rvalue(&l);
        uint16_t jz_pc = emit(OP_JZ, 0);
        expr_res_t r = parse_bitor();
        to_rvalue(&r);
        emit(OP_NOT, 0);
        emit(OP_NOT, 0);
        uint16_t end_pc = emit(OP_JMP, 0);
        patch_jmp(jz_pc, code_len);
        emit_imm(0);
        patch_jmp(end_pc, code_len);
        l.type.base = TYPE_INT;
        l.type.ptr = 0;
    }
    return l;
}

static expr_res_t parse_lor(void) {
    expr_res_t l = parse_land();
    while (tok == TOK_OR_OR) {
        next_tok();
        to_rvalue(&l);
        uint16_t jnz_pc = emit(OP_JNZ, 0);
        expr_res_t r = parse_land();
        to_rvalue(&r);
        emit(OP_NOT, 0);
        emit(OP_NOT, 0);
        uint16_t end_pc = emit(OP_JMP, 0);
        patch_jmp(jnz_pc, code_len);
        emit_imm(1);
        patch_jmp(end_pc, code_len);
        l.type.base = TYPE_INT;
        l.type.ptr = 0;
    }
    return l;
}

static expr_res_t parse_assign(void) {
    expr_res_t l = parse_lor();
    if (tok == TOK_ASSIGN || tok == TOK_ADD_ASSIGN || tok == TOK_SUB_ASSIGN ||
        tok == TOK_MUL_ASSIGN || tok == TOK_DIV_ASSIGN || tok == TOK_MOD_ASSIGN) {
        tok_t op = tok;
        next_tok();
        if (!l.is_lvalue || l.is_array) {
            cc_error("lvalue required on left of assignment");
            return l;
        }
        if (op == TOK_ASSIGN) {
            expr_res_t r = parse_assign();
            to_rvalue(&r);
            emit_store(l.type);
        } else {
            emit(OP_DUP, 0);
            emit_load(l.type);
            expr_res_t r = parse_assign();
            to_rvalue(&r);
            switch (op) {
                case TOK_ADD_ASSIGN: emit(OP_ADD, 0); break;
                case TOK_SUB_ASSIGN: emit(OP_SUB, 0); break;
                case TOK_MUL_ASSIGN: emit(OP_MUL, 0); break;
                case TOK_DIV_ASSIGN: emit(OP_DIV, 0); break;
                case TOK_MOD_ASSIGN: emit(OP_MOD, 0); break;
                default: break;
            }
            emit_store(l.type);
        }
        l.is_lvalue = false;
    }
    return l;
}

static expr_res_t parse_expr(void) {
    expr_res_t res = parse_assign();
    while (tok == TOK_COMMA) {
        to_rvalue(&res);
        emit(OP_POP, 0);
        next_tok();
        res = parse_assign();
    }
    return res;
}

/* ---------- Statements & Declarations ---------- */
static void parse_local_decl(void) {
    ctype_t base = parse_type();
    while (!compile_err) {
        ctype_t vt = base;
        while (tok == TOK_STAR) { vt.ptr++; next_tok(); }
        if (tok != TOK_ID) { cc_error("expected variable name"); return; }

        char name[24];
        strncpy(name, tok_str, 23);
        name[23] = '\0';
        next_tok();

        bool is_arr = false;
        int count = 1;
        if (tok == TOK_LBRACK) {
            next_tok();
            if (tok != TOK_NUM || tok_num <= 0) {
                cc_error("array size must be positive integer");
                return;
            }
            count = tok_num;
            is_arr = true;
            next_tok();
            expect(TOK_RBRACK, "']'");
        }

        int sz = elem_size(vt) * count;
        int aligned = (sz + 3) & ~3;
        int32_t off = local_bytes;
        local_bytes = (uint16_t)(local_bytes + aligned);
        if (local_bytes > max_local_bytes) max_local_bytes = local_bytes;

        add_sym(name, SYM_LOCAL, vt, is_arr, off, true);

        if (tok == TOK_ASSIGN) {
            next_tok();
            if (is_arr) { cc_error("array initializer not supported"); return; }
            emit(OP_ADDR_L, off);
            expr_res_t init = parse_assign();
            to_rvalue(&init);
            emit_store(vt);
            emit(OP_POP, 0);
        }

        if (tok == TOK_COMMA) next_tok();
        else break;
    }
    expect(TOK_SEMI, "';'");
}

static void push_loop(bool use_cont_target, uint16_t cont_target) {
    if (loop_depth >= CC_MAX_LOOPS) {
        cc_error("loop nesting too deep");
        return;
    }
    loops[loop_depth].break_cnt = 0;
    loops[loop_depth].cont_cnt = 0;
    loops[loop_depth].use_cont_target = use_cont_target;
    loops[loop_depth].cont_target = cont_target;
    loop_depth++;
}

static void pop_loop(uint16_t break_target, uint16_t cont_target) {
    loop_depth--;
    loop_ctx_t *lc = &loops[loop_depth];
    for (int i = 0; i < lc->break_cnt; i++) {
        patch_jmp(lc->break_pcs[i], break_target);
    }
    for (int i = 0; i < lc->cont_cnt; i++) {
        patch_jmp(lc->cont_pcs[i], cont_target);
    }
}

static void parse_block(void) {
    expect(TOK_LBRACE, "'{'");
    int saved_syms = sym_count;
    uint16_t saved_locals = local_bytes;

    while (tok != TOK_RBRACE && tok != TOK_EOF && !compile_err) {
        if (is_type_tok(tok)) {
            parse_local_decl();
        } else {
            parse_stmt();
        }
    }

    sym_count = saved_syms;
    local_bytes = saved_locals;
    expect(TOK_RBRACE, "'}'");
}

static void parse_stmt(void) {
    if (compile_err) return;

    if (tok == TOK_LBRACE) {
        parse_block();
        return;
    }
    if (tok == TOK_SEMI) {
        next_tok();
        return;
    }
    if (tok == TOK_IF) {
        next_tok();
        expect(TOK_LPAREN, "'('");
        expr_res_t cond = parse_expr();
        to_rvalue(&cond);
        expect(TOK_RPAREN, "')'");
        uint16_t jz_pc = emit(OP_JZ, 0);
        parse_stmt();
        if (tok == TOK_ELSE) {
            next_tok();
            uint16_t jmp_pc = emit(OP_JMP, 0);
            patch_jmp(jz_pc, code_len);
            parse_stmt();
            patch_jmp(jmp_pc, code_len);
        } else {
            patch_jmp(jz_pc, code_len);
        }
        return;
    }
    if (tok == TOK_WHILE) {
        next_tok();
        uint16_t top_pc = code_len;
        expect(TOK_LPAREN, "'('");
        expr_res_t cond = parse_expr();
        to_rvalue(&cond);
        expect(TOK_RPAREN, "')'");
        uint16_t jz_pc = emit(OP_JZ, 0);
        push_loop(true, top_pc);
        parse_stmt();
        emit(OP_JMP, top_pc);
        uint16_t end_pc = code_len;
        patch_jmp(jz_pc, end_pc);
        pop_loop(end_pc, top_pc);
        return;
    }
    if (tok == TOK_FOR) {
        next_tok();
        expect(TOK_LPAREN, "'('");
        int saved_syms = sym_count;
        uint16_t saved_locals = local_bytes;

        if (is_type_tok(tok)) {
            parse_local_decl();
        } else if (tok != TOK_SEMI) {
            expr_res_t init = parse_expr();
            to_rvalue(&init);
            emit(OP_POP, 0);
            expect(TOK_SEMI, "';'");
        } else {
            next_tok();
        }

        uint16_t cond_pc = code_len;
        uint16_t jz_pc = 0;
        bool has_cond = false;
        if (tok != TOK_SEMI) {
            has_cond = true;
            expr_res_t cond = parse_expr();
            to_rvalue(&cond);
            jz_pc = emit(OP_JZ, 0);
        }
        expect(TOK_SEMI, "';'");

        uint16_t jmp_body_pc = emit(OP_JMP, 0);
        uint16_t post_pc = code_len;
        if (tok != TOK_RPAREN) {
            expr_res_t post = parse_expr();
            to_rvalue(&post);
            emit(OP_POP, 0);
        }
        expect(TOK_RPAREN, "')'");
        emit(OP_JMP, cond_pc);

        patch_jmp(jmp_body_pc, code_len);
        push_loop(true, post_pc);
        parse_stmt();
        emit(OP_JMP, post_pc);
        uint16_t end_pc = code_len;
        if (has_cond) patch_jmp(jz_pc, end_pc);
        pop_loop(end_pc, post_pc);

        sym_count = saved_syms;
        local_bytes = saved_locals;
        return;
    }
    if (tok == TOK_BREAK) {
        next_tok();
        expect(TOK_SEMI, "';'");
        if (loop_depth == 0) { cc_error("'break' outside loop"); return; }
        loop_ctx_t *lc = &loops[loop_depth - 1];
        if (lc->break_cnt < CC_MAX_LOOP_JMP) {
            lc->break_pcs[lc->break_cnt++] = emit(OP_JMP, 0);
        }
        return;
    }
    if (tok == TOK_CONTINUE) {
        next_tok();
        expect(TOK_SEMI, "';'");
        if (loop_depth == 0) { cc_error("'continue' outside loop"); return; }
        loop_ctx_t *lc = &loops[loop_depth - 1];
        if (lc->cont_cnt < CC_MAX_LOOP_JMP) {
            lc->cont_pcs[lc->cont_cnt++] = emit(OP_JMP, 0);
        }
        return;
    }
    if (tok == TOK_RETURN) {
        next_tok();
        if (tok != TOK_SEMI) {
            expr_res_t val = parse_expr();
            to_rvalue(&val);
            emit(OP_RET, 0);
        } else {
            emit(OP_RETV, 0);
        }
        expect(TOK_SEMI, "';'");
        return;
    }

    expr_res_t e = parse_expr();
    to_rvalue(&e);
    emit(OP_POP, 0);
    expect(TOK_SEMI, "';'");
}

static void parse_top_level(void) {
    while (tok != TOK_EOF && !compile_err) {
        ctype_t base = parse_type();
        while (!compile_err) {
            ctype_t vt = base;
            while (tok == TOK_STAR) { vt.ptr++; next_tok(); }
            if (tok != TOK_ID) { cc_error("expected identifier"); return; }

            char name[24];
            strncpy(name, tok_str, 23);
            name[23] = '\0';
            next_tok();

            if (tok == TOK_LPAREN) {
                /* Function declaration or definition */
                next_tok();
                int sidx = find_sym(name);
                if (sidx >= 0 && syms[sidx].kind == SYM_FUNC && syms[sidx].defined) {
                    cc_error("redefinition of '%s'", name);
                    return;
                }
                if (sidx < 0) {
                    sidx = add_sym(name, SYM_FUNC, vt, false, 0, false);
                    if (sidx < 0) return;
                } else {
                    syms[sidx].type = vt;
                }

                int saved_syms = sym_count;
                local_bytes = 0;
                max_local_bytes = 0;
                int argc = 0;

                if (tok == TOK_VOID) {
                    next_tok();
                    if (tok != TOK_RPAREN) {
                        cc_error("invalid void parameter");
                        return;
                    }
                } else if (tok != TOK_RPAREN) {
                    while (!compile_err) {
                        ctype_t pt = parse_type();
                        char pname[24] = "";
                        if (tok == TOK_ID) {
                            strncpy(pname, tok_str, 23);
                            pname[23] = '\0';
                            next_tok();
                        }
                        if (tok == TOK_LBRACK) {
                            next_tok();
                            expect(TOK_RBRACK, "']'");
                            pt.ptr++;
                        }
                        add_sym(pname, SYM_LOCAL, pt, false, local_bytes, true);
                        local_bytes += 4;
                        max_local_bytes = local_bytes;
                        argc++;
                        if (tok == TOK_COMMA) next_tok();
                        else break;
                    }
                }
                expect(TOK_RPAREN, "')'");
                syms[sidx].argc = argc;

                if (tok == TOK_SEMI) {
                    /* Forward prototype */
                    next_tok();
                    sym_count = saved_syms;
                    local_bytes = 0;
                    break;
                }

                syms[sidx].addr = code_len;
                syms[sidx].defined = true;
                uint16_t enter_pc = emit(OP_ENTER, 0);
                parse_block();
                emit(OP_RETV, 0);
                patch_jmp(enter_pc, max_local_bytes);

                sym_count = saved_syms;
                local_bytes = 0;
                break;
            }

            /* Global variable or array */
            bool is_arr = false;
            int count = 1;
            if (tok == TOK_LBRACK) {
                next_tok();
                if (tok != TOK_NUM || tok_num <= 0) {
                    cc_error("global array size must be positive integer");
                    return;
                }
                count = tok_num;
                is_arr = true;
                next_tok();
                expect(TOK_RBRACK, "']'");
            }

            int sz = elem_size(vt) * count;
            int aligned = (sz + 3) & ~3;
            int32_t off = global_bytes;
            global_bytes = (uint16_t)(global_bytes + aligned);
            add_sym(name, SYM_GLOBAL, vt, is_arr, off, true);

            if (tok == TOK_ASSIGN) {
                next_tok();
                int sign = 1;
                if (tok == TOK_MINUS) { sign = -1; next_tok(); }
                if (tok != TOK_NUM) { cc_error("global initializer must be constant"); return; }
                int32_t val = tok_num * sign;
                next_tok();
                /* Emit init instruction inside global init? Simpler: store via helper or restrict? Wait - let's handle runtime global init cleanly or patch! */
                /* Wait: emit a tiny startup preamble or zero globals? Better: emit OP_ADDR_G + OP_IMM + OP_STORE in a startup init stub! */
                /* Wait: global_bytes is zeroed at VM start; if val != 0, we can't emit code outside function cleanly unless we use a startup stub. Let's support constant global init via startup stub or disallow? Actually, emitting a startup stub at pc=0 is trivial! */
                (void)val;
                cc_error("nonzero global init not supported yet (init inside main)");
                return;
            }

            if (tok == TOK_COMMA) next_tok();
            else {
                expect(TOK_SEMI, "';'");
                break;
            }
        }
    }
}

/* ---------- Builtin printf Formatter & Syscalls ---------- */
static volatile bool vm_aborted = false;
static uint32_t vm_rng_state = 0x12345678UL;

#define VM_KEYQ_SIZE 16
static uint8_t vm_keyq[VM_KEYQ_SIZE];
static uint8_t vm_keyq_head = 0;
static uint8_t vm_keyq_tail = 0;

static void vm_keyq_push(uint8_t c) {
    uint8_t next = (uint8_t)((vm_keyq_head + 1U) % VM_KEYQ_SIZE);
    if (next != vm_keyq_tail) {
        vm_keyq[vm_keyq_head] = c;
        vm_keyq_head = next;
    }
}

static int vm_keyq_pop(void) {
    if (vm_keyq_head == vm_keyq_tail) return -1;
    uint8_t c = vm_keyq[vm_keyq_tail];
    vm_keyq_tail = (uint8_t)((vm_keyq_tail + 1U) % VM_KEYQ_SIZE);
    return (int)c;
}

static bool vm_check_abort(void) {
    while (true) {
        int ch = getchar_timeout_us(0);
        if (ch == PICO_ERROR_TIMEOUT) break;
        if (ch == 0x03) { /* Ctrl+C */
            vm_aborted = true;
            return true;
        }
        vm_keyq_push((uint8_t)ch);
    }
    return vm_aborted;
}

static int vm_read_raw_nonblock(void) {
    vm_check_abort();
    return vm_keyq_pop();
}

static int vm_key_poll(void) {
    int ch = vm_read_raw_nonblock();
    if (ch < 0) return 0;
    if (ch == 27) {
        int c1 = vm_read_raw_nonblock();
        if (c1 < 0) {
            os_sleep_ms(10);
            c1 = vm_read_raw_nonblock();
        }
        if (c1 == '[') {
            int c2 = vm_read_raw_nonblock();
            if (c2 < 0) {
                os_sleep_ms(10);
                c2 = vm_read_raw_nonblock();
            }
            if (c2 == 'A') return 1000; /* KEY_UP */
            if (c2 == 'B') return 1001; /* KEY_DOWN */
            if (c2 == 'C') return 1002; /* KEY_RIGHT */
            if (c2 == 'D') return 1003; /* KEY_LEFT */
        }
        return 27; /* KEY_ESC */
    }
    return ch;
}

static void vm_printf_exec(int argc, const int32_t *args, uint16_t data_limit) {
    if (argc < 1) return;
    uint32_t fmt_addr = (uint32_t)args[0];
    if (fmt_addr >= VM_MEM_SIZE) return;
    (void)data_limit;

    const char *fmt = (const char *)&vm_mem[fmt_addr];
    char out[192];
    size_t pos = 0;
    int arg_i = 1;

    while (*fmt && pos < sizeof(out) - 1) {
        if (*fmt != '%') {
            out[pos++] = *fmt++;
            continue;
        }
        fmt++;
        if (*fmt == '%') {
            out[pos++] = '%';
            fmt++;
            continue;
        }

        char pad = ' ';
        int width = 0;
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        char spec = *fmt ? *fmt++ : '\0';
        int32_t val = (arg_i < argc) ? args[arg_i++] : 0;

        char tmp[48];
        tmp[0] = '\0';
        if (spec == 'd' || spec == 'i') {
            snprintf(tmp, sizeof(tmp), "%ld", (long)val);
        } else if (spec == 'u') {
            snprintf(tmp, sizeof(tmp), "%lu", (unsigned long)(uint32_t)val);
        } else if (spec == 'x') {
            snprintf(tmp, sizeof(tmp), "%lx", (unsigned long)(uint32_t)val);
        } else if (spec == 'X') {
            snprintf(tmp, sizeof(tmp), "%lX", (unsigned long)(uint32_t)val);
        } else if (spec == 'c') {
            tmp[0] = (char)val;
            tmp[1] = '\0';
        } else if (spec == 's') {
            uint32_t sa = (uint32_t)val;
            const char *s = (sa < VM_MEM_SIZE) ? (const char *)&vm_mem[sa] : "(null)";
            size_t sl = strnlen(s, sizeof(out) - pos - 1);
            memcpy(out + pos, s, sl);
            pos += sl;
            continue;
        }

        int tlen = (int)strlen(tmp);
        while (tlen < width && pos < sizeof(out) - 1) {
            out[pos++] = pad;
            width--;
        }
        for (int k = 0; k < tlen && pos < sizeof(out) - 1; k++) {
            out[pos++] = tmp[k];
        }
    }
    out[pos] = '\0';

    /* Print to serial (converting \n -> \r\n) and TFT console */
    for (size_t i = 0; i < pos; i++) {
        if (out[i] == '\n') putchar('\r');
        putchar(out[i]);
    }
    fflush(stdout);
    console_puts(out, COLOR_WHITE);
}

static int32_t vm_syscall(int sys_id, int argc, const int32_t *args, uint16_t data_limit) {
    switch (sys_id) {
        case SYS_PRINTF:
            vm_printf_exec(argc, args, data_limit);
            return 0;
        case SYS_PUTS: {
            if (argc >= 1 && (uint32_t)args[0] < VM_MEM_SIZE) {
                const char *s = (const char *)&vm_mem[(uint32_t)args[0]];
                printf("%s\r\n", s);
                console_printf(COLOR_WHITE, "%s\n", s);
            }
            return 0;
        }
        case SYS_PUTCHAR:
            if (argc >= 1) {
                char c = (char)args[0];
                if (c == '\n') putchar('\r');
                putchar(c);
                fflush(stdout);
                char cs[2] = { c, '\0' };
                console_puts(cs, COLOR_WHITE);
            }
            return argc >= 1 ? args[0] : 0;
        case SYS_GETCHAR: {
            while (!vm_aborted) {
                int ch = vm_read_raw_nonblock();
                if (vm_aborted) return -1;
                if (ch >= 0) return ch;
                os_sleep_ms(5);
            }
            return -1;
        }
        case SYS_SLEEP_MS: {
            int32_t ms = (argc >= 1) ? args[0] : 0;
            while (ms > 0 && !vm_aborted) {
                uint32_t step = (ms > 20) ? 20U : (uint32_t)ms;
                os_sleep_ms(step);
                ms -= (int32_t)step;
                vm_check_abort();
            }
            return 0;
        }
        case SYS_TICKS:
            return (int32_t)os_get_ticks();
        case SYS_LED:
            if (argc >= 1) {
                g_led_auto_blink = false;
                set_onboard_led(args[0] != 0);
            }
            return 0;
        case SYS_BACKLIGHT:
            if (argc >= 1) {
                int v = args[0];
                if (v < 0) v = 0;
                if (v > 100) v = 100;
                st7735_set_backlight((uint8_t)v);
            }
            return 0;
        case SYS_CLS:
            console_set_statusbar_active(false);
            st7735_fill_screen((uint16_t)(argc >= 1 ? args[0] : 0));
            return 0;
        case SYS_RECT:
            if (argc >= 5) {
                console_set_statusbar_active(false);
                st7735_fill_rect((uint16_t)args[0], (uint16_t)args[1],
                                 (uint16_t)args[2], (uint16_t)args[3],
                                 (uint16_t)args[4]);
            }
            return 0;
        case SYS_TEXT:
            if (argc >= 5 && (uint32_t)args[2] < VM_MEM_SIZE) {
                console_set_statusbar_active(false);
                const char *s = (const char *)&vm_mem[(uint32_t)args[2]];
                st7735_draw_string((uint16_t)args[0], (uint16_t)args[1], s,
                                   (uint16_t)args[3], (uint16_t)args[4], 1);
            }
            return 0;
        case SYS_RGB:
            if (argc >= 3) {
                return (int32_t)RGB565(args[0], args[1], args[2]);
            }
            return 0;
        case SYS_RAND:
            vm_rng_state ^= vm_rng_state << 13;
            vm_rng_state ^= vm_rng_state >> 17;
            vm_rng_state ^= vm_rng_state << 5;
            if (vm_rng_state == 0) vm_rng_state = os_get_ticks() | 1U;
            return (int32_t)(vm_rng_state & 0x7FFFFFFFUL);
        case SYS_PIXEL:
            if (argc >= 3) {
                console_set_statusbar_active(false);
                st7735_draw_pixel((uint16_t)args[0], (uint16_t)args[1], (uint16_t)args[2]);
            }
            return 0;
        case SYS_KEY_POLL:
            return vm_key_poll();
        case SYS_PIN_MODE:
            return (argc >= 2) ? hw_pin_mode(args[0], args[1]) : -1;
        case SYS_DIGITAL_WRITE:
            return (argc >= 2) ? hw_digital_write(args[0], args[1]) : -1;
        case SYS_DIGITAL_READ:
            return (argc >= 1) ? hw_digital_read(args[0]) : -1;
        case SYS_GPIO_TOGGLE:
            return (argc >= 1) ? hw_gpio_toggle(args[0]) : -1;
        case SYS_PWM_WRITE:
            return (argc >= 2) ? hw_pwm_write(args[0], args[1]) : -1;
        case SYS_TONE:
            return (argc >= 3) ? hw_tone(args[0], args[1], args[2]) :
                   ((argc == 2) ? hw_tone(args[0], args[1], 0) : -1);
        case SYS_NO_TONE:
            return (argc >= 1) ? hw_no_tone(args[0]) : -1;
        case SYS_ADC_READ:
            return (argc >= 1) ? hw_adc_read(args[0]) : -1;
        case SYS_TEMP_C:
            return hw_read_temp_c();
        case SYS_I2C_INIT:
            if (argc >= 4) return hw_i2c_init(args[0], args[1], args[2], args[3]);
            if (argc == 3) return hw_i2c_init(args[0], args[1], args[2], 100000);
            return -1;
        case SYS_I2C_WRITE: {
            if (argc < 4) return -1;
            uint32_t p = (uint32_t)args[2];
            uint32_t l = (uint32_t)args[3];
            if (p == 0 || p + l > VM_MEM_SIZE) return -1;
            return hw_i2c_write(args[0], (uint8_t)args[1], &vm_mem[p], l);
        }
        case SYS_I2C_READ: {
            if (argc < 4) return -1;
            uint32_t p = (uint32_t)args[2];
            uint32_t l = (uint32_t)args[3];
            if (p == 0 || p + l > VM_MEM_SIZE) return -1;
            return hw_i2c_read(args[0], (uint8_t)args[1], &vm_mem[p], l);
        }
        case SYS_I2C_WRITE_REG:
            return (argc >= 4) ? hw_i2c_write_reg(args[0], (uint8_t)args[1], (uint8_t)args[2], (uint8_t)args[3]) : -1;
        case SYS_I2C_READ_REG:
            return (argc >= 3) ? hw_i2c_read_reg(args[0], (uint8_t)args[1], (uint8_t)args[2]) : -1;
        case SYS_SPI_OPEN:
            if (argc >= 5) return hw_spi_open(args[0], args[1], args[2], args[3], args[4]);
            if (argc == 4) return hw_spi_open(args[0], args[1], args[2], args[3], 1000000);
            return -1;
        case SYS_SPI_TRANSFER: {
            if (argc < 4) return -1;
            uint32_t tp = (uint32_t)args[1];
            uint32_t rp = (uint32_t)args[2];
            uint32_t l  = (uint32_t)args[3];
            const uint8_t *tx = NULL;
            uint8_t *rx = NULL;
            if (tp != 0) {
                if (tp + l > VM_MEM_SIZE) return -1;
                tx = &vm_mem[tp];
            }
            if (rp != 0) {
                if (rp + l > VM_MEM_SIZE) return -1;
                rx = &vm_mem[rp];
            }
            return hw_spi_transfer(args[0], tx, rx, l);
        }
        case SYS_SPI_BYTE:
            return (argc >= 2) ? hw_spi_byte(args[0], (uint8_t)args[1]) : -1;
        default:
            return 0;
    }
}

/* ---------- Public Compiler / Runner / Disassembler ---------- */
int cc_compile_file(const char *src_name, const char *out_name) {
    if (!src_name || src_name[0] == '\0') {
        printf("Usage: cc <file.c> [-o <out>]\r\n");
        return -1;
    }
    if (!out_name || out_name[0] == '\0') {
        out_name = "a.out";
    }

    int sz = fs_read(src_name, src_buf, FS_MAX_FILESIZE);
    if (sz < 0) {
        printf("cc: file not found: %s\r\n", src_name);
        console_printf(COLOR_RED, "cc: no %s\n", src_name);
        return -1;
    }
    src_buf[sz] = '\0';

    lex_p = src_buf;
    lex_line = 1;
    code_len = 0;
    data_len = 4; /* Reserve offset 0..3 as NULL */
    memset(data_buf, 0, 4);
    global_bytes = 0;
    local_bytes = 0;
    max_local_bytes = 0;
    sym_count = 0;
    macro_count = 0;
    patch_count = 0;
    loop_depth = 0;
    compile_err = false;
    err_msg[0] = '\0';

    next_tok();
    parse_top_level();

    /* Resolve forward function calls */
    for (int i = 0; i < patch_count && !compile_err; i++) {
        uint8_t sidx = call_patches[i].sym_idx;
        if (!syms[sidx].defined) {
            cc_error("undefined function '%s'", syms[sidx].name);
            break;
        }
        code_buf[call_patches[i].pc] = enc_instr(
            OP_CALL,
            ((int32_t)call_patches[i].argc << 16) | (syms[sidx].addr & 0xFFFF)
        );
    }

    int main_idx = find_sym("main");
    if (!compile_err && (main_idx < 0 || syms[main_idx].kind != SYM_FUNC || !syms[main_idx].defined)) {
        cc_error("missing 'main()' function");
    }

    if (compile_err) {
        printf("cc error (%s): %s\r\n", src_name, err_msg);
        console_printf(COLOR_RED, "%s\n", err_msg);
        return -1;
    }

    size_t code_bytes = (size_t)code_len * sizeof(uint32_t);
    size_t total_size = sizeof(pxe_header_t) + code_bytes + data_len;
    if (total_size > FS_MAX_FILESIZE) {
        printf("cc error: binary too large (%u > %u bytes)\r\n",
               (unsigned)total_size, (unsigned)FS_MAX_FILESIZE);
        return -1;
    }

    pxe_header_t hdr = {
        .magic = PXE_MAGIC,
        .code_count = code_len,
        .data_size = data_len,
        .main_pc = (uint16_t)syms[main_idx].addr,
        .global_bytes = global_bytes
    };

    memcpy(bin_buf, &hdr, sizeof(hdr));
    memcpy(bin_buf + sizeof(hdr), code_buf, code_bytes);
    if (data_len > 0) {
        memcpy(bin_buf + sizeof(hdr) + code_bytes, data_buf, data_len);
    }

    int wrc = fs_write(out_name, bin_buf, total_size);
    if (wrc < 0) {
        printf("cc: failed to write %s (%d)\r\n", out_name, wrc);
        return -1;
    }

    printf("Compiled %s -> %s (%u bytes, %u instrs)\r\n",
           src_name, out_name, (unsigned)total_size, (unsigned)code_len);
    console_printf(COLOR_GREEN, "CC OK -> %s (%uB)\n", out_name, (unsigned)total_size);
    return 0;
}

int cc_run_file(const char *bin_name) {
    if (!bin_name || bin_name[0] == '\0') {
        bin_name = "a.out";
    }

    int sz = fs_read(bin_name, bin_buf, FS_MAX_FILESIZE);
    if (sz < (int)sizeof(pxe_header_t)) {
        printf("run: cannot open %s\r\n", bin_name);
        console_printf(COLOR_RED, "run: no %s\n", bin_name);
        return -1;
    }

    pxe_header_t hdr;
    memcpy(&hdr, bin_buf, sizeof(hdr));
    if (hdr.magic != PXE_MAGIC) {
        printf("run: %s is not a valid PXE1 executable\r\n", bin_name);
        console_puts("Invalid PXE1 binary\n", COLOR_RED);
        return -1;
    }
    if (hdr.code_count > CC_MAX_CODE || hdr.data_size + hdr.global_bytes >= VM_MEM_SIZE - 512) {
        printf("run: invalid binary dimensions\r\n");
        return -1;
    }

    size_t code_bytes = (size_t)hdr.code_count * sizeof(uint32_t);
    if ((size_t)sz < sizeof(pxe_header_t) + code_bytes + hdr.data_size) {
        printf("run: truncated binary\r\n");
        return -1;
    }

    memcpy(code_buf, bin_buf + sizeof(hdr), code_bytes);
    memset(vm_mem, 0, sizeof(vm_mem));
    if (hdr.data_size > 0) {
        memcpy(vm_mem, bin_buf + sizeof(hdr) + code_bytes, hdr.data_size);
    }

    uint16_t data_aligned = (uint16_t)((hdr.data_size + 3U) & ~3U);
    uint16_t global_base  = data_aligned;
    uint16_t frame_base   = (uint16_t)(global_base + hdr.global_bytes);

    uint16_t pc = hdr.main_pc;
    uint16_t bp = frame_base;
    uint16_t sp = frame_base;
    int esp = 0;
    int csp = 0;
    uint32_t steps = 0;
    vm_aborted = false;
    vm_keyq_head = 0;
    vm_keyq_tail = 0;
    vm_rng_state = os_get_ticks() | 1U;

    int32_t exit_code = 0;

    while (!vm_aborted) {
        if (pc >= hdr.code_count) {
            printf("\r\nVM error: PC out of range\r\n");
            return -1;
        }
        if ((++steps & 0xFFU) == 0) {
            if (vm_check_abort()) break;
            os_yield();
        }

        uint32_t instr = code_buf[pc++];
        uint8_t op = dec_op(instr);
        int32_t imm = dec_imm(instr);

        switch (op) {
            case OP_HALT:
                goto vm_done;

            case OP_IMM:
                if (esp >= VM_ESTACK_SIZE) goto stack_err;
                vm_estack[esp++] = imm;
                break;

            case OP_IMM32:
                if (pc >= hdr.code_count || esp >= VM_ESTACK_SIZE) goto stack_err;
                vm_estack[esp++] = (int32_t)code_buf[pc++];
                break;

            case OP_ADDR_G:
                if (esp >= VM_ESTACK_SIZE) goto stack_err;
                vm_estack[esp++] = (int32_t)(global_base + imm);
                break;

            case OP_ADDR_L:
                if (esp >= VM_ESTACK_SIZE) goto stack_err;
                vm_estack[esp++] = (int32_t)(bp + imm);
                break;

            case OP_ADDR_D:
                if (esp >= VM_ESTACK_SIZE) goto stack_err;
                vm_estack[esp++] = imm;
                break;

            case OP_LOAD32: {
                if (esp < 1) goto stack_err;
                uint32_t a = (uint32_t)vm_estack[esp - 1];
                if (a + 4 > VM_MEM_SIZE) goto mem_err;
                int32_t v;
                memcpy(&v, &vm_mem[a], 4);
                vm_estack[esp - 1] = v;
                break;
            }
            case OP_LOAD8: {
                if (esp < 1) goto stack_err;
                uint32_t a = (uint32_t)vm_estack[esp - 1];
                if (a >= VM_MEM_SIZE) goto mem_err;
                vm_estack[esp - 1] = (int32_t)(uint8_t)vm_mem[a];
                break;
            }
            case OP_STORE32: {
                if (esp < 2) goto stack_err;
                int32_t v = vm_estack[--esp];
                uint32_t a = (uint32_t)vm_estack[esp - 1];
                if (a + 4 > VM_MEM_SIZE) goto mem_err;
                memcpy(&vm_mem[a], &v, 4);
                vm_estack[esp - 1] = v;
                break;
            }
            case OP_STORE8: {
                if (esp < 2) goto stack_err;
                int32_t v = vm_estack[--esp];
                uint32_t a = (uint32_t)vm_estack[esp - 1];
                if (a >= VM_MEM_SIZE) goto mem_err;
                vm_mem[a] = (uint8_t)v;
                vm_estack[esp - 1] = (int32_t)(uint8_t)v;
                break;
            }
            case OP_DUP:
                if (esp < 1 || esp >= VM_ESTACK_SIZE) goto stack_err;
                vm_estack[esp] = vm_estack[esp - 1];
                esp++;
                break;

            case OP_POP:
                if (esp > 0) esp--;
                break;

            case OP_ADD: if (esp < 2) goto stack_err; esp--; vm_estack[esp - 1] += vm_estack[esp]; break;
            case OP_SUB: if (esp < 2) goto stack_err; esp--; vm_estack[esp - 1] -= vm_estack[esp]; break;
            case OP_MUL: if (esp < 2) goto stack_err; esp--; vm_estack[esp - 1] *= vm_estack[esp]; break;
            case OP_DIV:
                if (esp < 2) goto stack_err;
                esp--;
                if (vm_estack[esp] == 0) { printf("\r\nVM error: div by 0\r\n"); return -1; }
                vm_estack[esp - 1] /= vm_estack[esp];
                break;
            case OP_MOD:
                if (esp < 2) goto stack_err;
                esp--;
                if (vm_estack[esp] == 0) { printf("\r\nVM error: mod by 0\r\n"); return -1; }
                vm_estack[esp - 1] %= vm_estack[esp];
                break;
            case OP_SHL: if (esp < 2) goto stack_err; esp--; vm_estack[esp - 1] <<= vm_estack[esp]; break;
            case OP_SHR: if (esp < 2) goto stack_err; esp--; vm_estack[esp - 1] >>= vm_estack[esp]; break;
            case OP_AND: if (esp < 2) goto stack_err; esp--; vm_estack[esp - 1] &= vm_estack[esp]; break;
            case OP_OR:  if (esp < 2) goto stack_err; esp--; vm_estack[esp - 1] |= vm_estack[esp]; break;
            case OP_XOR: if (esp < 2) goto stack_err; esp--; vm_estack[esp - 1] ^= vm_estack[esp]; break;
            case OP_EQ:  if (esp < 2) goto stack_err; esp--; vm_estack[esp - 1] = (vm_estack[esp - 1] == vm_estack[esp]); break;
            case OP_NE:  if (esp < 2) goto stack_err; esp--; vm_estack[esp - 1] = (vm_estack[esp - 1] != vm_estack[esp]); break;
            case OP_LT:  if (esp < 2) goto stack_err; esp--; vm_estack[esp - 1] = (vm_estack[esp - 1] <  vm_estack[esp]); break;
            case OP_LE:  if (esp < 2) goto stack_err; esp--; vm_estack[esp - 1] = (vm_estack[esp - 1] <= vm_estack[esp]); break;
            case OP_GT:  if (esp < 2) goto stack_err; esp--; vm_estack[esp - 1] = (vm_estack[esp - 1] >  vm_estack[esp]); break;
            case OP_GE:  if (esp < 2) goto stack_err; esp--; vm_estack[esp - 1] = (vm_estack[esp - 1] >= vm_estack[esp]); break;

            case OP_NEG:  if (esp < 1) goto stack_err; vm_estack[esp - 1] = -vm_estack[esp - 1]; break;
            case OP_NOT:  if (esp < 1) goto stack_err; vm_estack[esp - 1] = !vm_estack[esp - 1]; break;
            case OP_BNOT: if (esp < 1) goto stack_err; vm_estack[esp - 1] = ~vm_estack[esp - 1]; break;

            case OP_JMP:
                pc = (uint16_t)imm;
                break;

            case OP_JZ:
                if (esp < 1) goto stack_err;
                if (vm_estack[--esp] == 0) pc = (uint16_t)imm;
                break;

            case OP_JNZ:
                if (esp < 1) goto stack_err;
                if (vm_estack[--esp] != 0) pc = (uint16_t)imm;
                break;

            case OP_CALL: {
                int argc = (imm >> 16) & 0x7F;
                uint16_t target = (uint16_t)(imm & 0xFFFF);
                if (csp >= VM_CSTACK_SIZE || esp < argc) goto stack_err;
                if ((uint32_t)sp + (uint32_t)argc * 4U >= VM_MEM_SIZE) goto mem_err;

                vm_cstack[csp].ret_pc = pc;
                vm_cstack[csp].prev_bp = bp;
                vm_cstack[csp].prev_sp = sp;
                csp++;

                uint16_t new_bp = sp;
                for (int i = argc - 1; i >= 0; i--) {
                    int32_t arg_val = vm_estack[--esp];
                    memcpy(&vm_mem[new_bp + i * 4], &arg_val, 4);
                }
                bp = new_bp;
                sp = (uint16_t)(new_bp + argc * 4);
                pc = target;
                break;
            }
            case OP_ENTER:
                if ((uint32_t)bp + (uint32_t)imm >= VM_MEM_SIZE) goto mem_err;
                if (bp + imm > sp) {
                    memset(&vm_mem[sp], 0, (size_t)((bp + imm) - sp));
                    sp = (uint16_t)(bp + imm);
                }
                break;

            case OP_RET:
            case OP_RETV: {
                int32_t rval = 0;
                if (op == OP_RET) {
                    if (esp < 1) goto stack_err;
                    rval = vm_estack[--esp];
                }
                if (csp == 0) {
                    exit_code = rval;
                    goto vm_done;
                }
                csp--;
                pc = vm_cstack[csp].ret_pc;
                bp = vm_cstack[csp].prev_bp;
                sp = vm_cstack[csp].prev_sp;
                if (esp >= VM_ESTACK_SIZE) goto stack_err;
                vm_estack[esp++] = rval;
                break;
            }
            case OP_SYSCALL: {
                int argc = (imm >> 12) & 0x3F;
                int sys_id = imm & 0xFFF;
                if (esp < argc) goto stack_err;
                int32_t sargs[16];
                for (int i = argc - 1; i >= 0; i--) {
                    int32_t v = vm_estack[--esp];
                    if (i < 16) sargs[i] = v;
                }
                int32_t sret = vm_syscall(sys_id, argc > 16 ? 16 : argc, sargs, hdr.data_size);
                if (esp >= VM_ESTACK_SIZE) goto stack_err;
                vm_estack[esp++] = sret;
                break;
            }
            default:
                printf("\r\nVM error: illegal opcode %u\r\n", op);
                return -1;
        }
    }

vm_done:
    if (vm_aborted) {
        printf("\r\n^C (aborted)\r\n");
        console_puts("^C aborted\n", COLOR_YELLOW);
        return -1;
    }
    if (exit_code != 0) {
        printf("[exit: %ld]\r\n", (long)exit_code);
    }
    return exit_code;

stack_err:
    printf("\r\nVM error: stack overflow/underflow\r\n");
    return -1;

mem_err:
    printf("\r\nVM error: memory out of bounds\r\n");
    return -1;
}

int cc_compile_and_run(const char *src_name) {
    if (cc_compile_file(src_name, "a.out") != 0) {
        return -1;
    }
    return cc_run_file("a.out");
}

static const char *op_name(uint8_t op) {
    switch (op) {
        case OP_HALT: return "HALT";     case OP_IMM: return "IMM";
        case OP_IMM32: return "IMM32";   case OP_ADDR_G: return "ADDR_G";
        case OP_ADDR_L: return "ADDR_L"; case OP_ADDR_D: return "ADDR_D";
        case OP_LOAD32: return "LOAD32"; case OP_LOAD8: return "LOAD8";
        case OP_STORE32: return "STORE32"; case OP_STORE8: return "STORE8";
        case OP_DUP: return "DUP";       case OP_POP: return "POP";
        case OP_ADD: return "ADD";       case OP_SUB: return "SUB";
        case OP_MUL: return "MUL";       case OP_DIV: return "DIV";
        case OP_MOD: return "MOD";       case OP_SHL: return "SHL";
        case OP_SHR: return "SHR";       case OP_AND: return "AND";
        case OP_OR: return "OR";         case OP_XOR: return "XOR";
        case OP_EQ: return "EQ";         case OP_NE: return "NE";
        case OP_LT: return "LT";         case OP_LE: return "LE";
        case OP_GT: return "GT";         case OP_GE: return "GE";
        case OP_NEG: return "NEG";       case OP_NOT: return "NOT";
        case OP_BNOT: return "BNOT";     case OP_JMP: return "JMP";
        case OP_JZ: return "JZ";         case OP_JNZ: return "JNZ";
        case OP_CALL: return "CALL";     case OP_ENTER: return "ENTER";
        case OP_RET: return "RET";       case OP_RETV: return "RETV";
        case OP_SYSCALL: return "SYSCALL";
        default: return "???";
    }
}

void cc_disasm_file(const char *bin_name) {
    if (!bin_name || bin_name[0] == '\0') bin_name = "a.out";
    int sz = fs_read(bin_name, bin_buf, FS_MAX_FILESIZE);
    if (sz < (int)sizeof(pxe_header_t)) {
        printf("disasm: cannot open %s\r\n", bin_name);
        return;
    }
    pxe_header_t hdr;
    memcpy(&hdr, bin_buf, sizeof(hdr));
    if (hdr.magic != PXE_MAGIC) {
        printf("disasm: invalid PXE1 file\r\n");
        return;
    }
    memcpy(code_buf, bin_buf + sizeof(hdr), (size_t)hdr.code_count * sizeof(uint32_t));
    printf("PXE1: main=%u, code=%u, data=%u, globals=%u\r\n",
           hdr.main_pc, hdr.code_count, hdr.data_size, hdr.global_bytes);
    for (uint16_t i = 0; i < hdr.code_count; i++) {
        uint8_t op = dec_op(code_buf[i]);
        int32_t imm = dec_imm(code_buf[i]);
        if (op == OP_IMM32 && i + 1 < hdr.code_count) {
            printf("  %04u: %-8s %ld\r\n", i, op_name(op), (long)(int32_t)code_buf[i + 1]);
            i++;
        } else {
            printf("  %04u: %-8s %ld\r\n", i, op_name(op), (long)imm);
        }
    }
}
