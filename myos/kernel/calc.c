/*
 * calc.c — TanoojCalc with decimal support
 *
 * Uses int32_t only (no 64-bit division needed).
 * Fixed-point scale: 1000 = 3 decimal places.
 * e.g. 3.14 stored as 3140, result 6.28 stored as 6280.
 *
 * Operators: + - * / % ^ ( )
 * Precedence: ^ > * / % > + -
 */

#include "terminal.h"
#include <stdint.h>

#define SCALE 1000   /* 3 decimal places */

static const char *src;
static int         pos;
static int         had_error;
static char        errmsg[64];

static void set_err(const char *msg) {
    had_error = 1;
    int i=0; while(msg[i]&&i<63){errmsg[i]=msg[i];i++;} errmsg[i]=0;
}

static void skip_ws(void) {
    while (src[pos]==' '||src[pos]=='\t') pos++;
}
static char peek(void)    { skip_ws(); return src[pos];    }
static char consume(void) { skip_ws(); return src[pos++];  }
static int  is_digit(char c) { return c>='0'&&c<='9'; }

/* Integer power — base and result are fixed-point (scaled by SCALE) */
static int32_t fp_pow(int32_t base, int32_t exp_fp) {
    if (exp_fp % SCALE != 0) { set_err("Fractional exponent not supported"); return 0; }
    int32_t exp = exp_fp / SCALE;
    if (exp < 0)  { set_err("Negative exponent not supported"); return 0; }
    if (exp == 0) return SCALE; /* 1.000 */
    int32_t result = SCALE;
    for (int32_t i = 0; i < exp; i++) {
        result = (result / 100) * (base / 10);  /* keep scaled */
        if (result > 2000000 || result < -2000000) { set_err("Overflow"); return 0; }
    }
    return result;
}

static int32_t parse_expr(void);

/* Parse number → fixed-point */
static int32_t parse_number(void) {
    skip_ws();
    if (!is_digit(src[pos]) && src[pos]!='.') { set_err("Expected number"); return 0; }
    int32_t integer = 0;
    while (is_digit(src[pos])) integer = integer*10 + (src[pos++]-'0');
    int32_t frac=0, fscale=100;
    if (src[pos]=='.') {
        pos++;
        int digits=0;
        while (is_digit(src[pos]) && digits<3) {
            frac += (src[pos++]-'0') * fscale;
            fscale /= 10;
            digits++;
        }
        while (is_digit(src[pos])) pos++; /* skip extra digits */
    }
    return integer*SCALE + frac;
}

static int32_t parse_base(void) {
    char c = peek();
    if (c=='-') { consume(); return -parse_base(); }
    if (c=='+') { consume(); return  parse_base(); }
    if (c=='(') {
        consume();
        int32_t v = parse_expr();
        if (had_error) return 0;
        if (peek()!=')') { set_err("Missing ')'"); return 0; }
        consume(); return v;
    }
    return parse_number();
}

static int32_t parse_factor(void) {
    int32_t v = parse_base();
    if (had_error) return 0;
    while (peek()=='^') {
        consume();
        int32_t e = parse_base();
        if (had_error) return 0;
        v = fp_pow(v, e);
    }
    return v;
}

static int32_t parse_term(void) {
    int32_t v = parse_factor();
    if (had_error) return 0;
    while (1) {
        char c = peek();
        if (c!='*'&&c!='/'&&c!='%') break;
        consume();
        int32_t r = parse_factor();
        if (had_error) return 0;
        if (c=='*') {
            /* (a/SCALE) * (b/SCALE) * SCALE = a*b/SCALE */
            v = (v/10) * (r/10) / 10;
        } else if (c=='/') {
            if (r==0) { set_err("Division by zero"); return 0; }
            v = (v*SCALE) / r;
        } else {
            int32_t a=v/SCALE, b=r/SCALE;
            if (b==0) { set_err("Modulo by zero"); return 0; }
            v = (a%b)*SCALE;
        }
    }
    return v;
}

static int32_t parse_expr(void) {
    int32_t v = parse_term();
    if (had_error) return 0;
    while (1) {
        char c = peek();
        if (c!='+'&&c!='-') break;
        consume();
        int32_t r = parse_term();
        if (had_error) return 0;
        v = (c=='+') ? v+r : v-r;
    }
    return v;
}

static void print_dec(int32_t n) {
    if (n==0){terminal_putchar('0');return;}
    char buf[12]; int p=10; buf[11]=0;
    int32_t t=n<0?-n:n;
    while(t>0){buf[p--]='0'+(char)(t%10);t/=10;}
    terminal_write(&buf[p+1]);
}

static void print_fp(int32_t v) {
    if (v<0){terminal_putchar('-');v=-v;}
    int32_t integer = v/SCALE;
    int32_t frac    = v%SCALE;
    print_dec(integer);
    if (frac!=0) {
        terminal_putchar('.');
        /* print 3 digits, strip trailing zeros */
        char fb[4]; fb[3]=0;
        int32_t f=frac;
        fb[2]='0'+(char)(f%10); f/=10;
        fb[1]='0'+(char)(f%10); f/=10;
        fb[0]='0'+(char)(f%10);
        int last=2;
        while(last>0&&fb[last]=='0') last--;
        fb[last+1]=0;
        terminal_write(fb);
    }
}

void calc_eval(const char *expr) {
    src=expr; pos=0; had_error=0; errmsg[0]=0;
    int32_t result = parse_expr();
    if (had_error) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_write("  Error: "); terminal_writeline(errmsg);
        return;
    }
    skip_ws();
    if (src[pos]!=0) {
        terminal_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        terminal_write("  Unexpected: '");
        terminal_putchar(src[pos]);
        terminal_writeline("'");
        return;
    }
    terminal_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
    terminal_write("  = ");
    print_fp(result);
    terminal_set_color(COLOR_DARK_GREY, COLOR_BLACK);
    terminal_writeline("");
}
