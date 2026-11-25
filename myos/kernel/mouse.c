/*
 * mouse.c — PS/2 mouse driver (v4 - reliable)
 *
 * Key rules:
 *   1. ALWAYS cursor_restore() before touching VGA
 *   2. ALWAYS cursor_show() after touching VGA
 *   3. sel_valid[] tracks exactly which cells were saved
 *   4. Never touch rows >= ROW_INPUT
 */

#include "mouse.h"
#include "idt.h"
#include "terminal.h"
#include <stdint.h>

static inline void    outb(uint16_t p, uint8_t v) { __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t inb (uint16_t p)             { uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

#define VGA_W      80
#define VGA_H      25
#define ROW_INPUT  23   /* input bar — don't touch below this */
#define ROW_STATUS 24

static volatile uint16_t * const VGA = (volatile uint16_t *)0xB8000;

/* -----------------------------------------------------------------------
   Clipboard
   ----------------------------------------------------------------------- */
#define CLIP_MAX 512
static char clip_buf[CLIP_MAX + 4];
static int  clip_len = 0;

void clipboard_copy(const char *t, int len) {
    if (len > CLIP_MAX) len = CLIP_MAX;
    for (int i = 0; i < len; i++) clip_buf[i] = t[i];
    clip_buf[len] = 0; clip_len = len;
}
const char *clipboard_get(void) { return clip_buf; }
int         clipboard_len(void)  { return clip_len; }

/* -----------------------------------------------------------------------
   Cursor
   ----------------------------------------------------------------------- */
static int      cur_x = 40, cur_y = 5;
static uint16_t cur_saved;
static int      cur_on = 0;

static void cursor_hide(void) {
    if (!cur_on) return;
    VGA[cur_y * VGA_W + cur_x] = cur_saved;
    cur_on = 0;
}

static void cursor_show(void) {
    if (cur_y >= ROW_STATUS) return;
    int idx = cur_y * VGA_W + cur_x;
    cur_saved = VGA[idx];
    uint8_t fg = (cur_saved >> 8) & 0x0F;
    /* Grey background highlight, keep char and fg color */
    uint8_t col = (uint8_t)(0x70 | fg);
    uint16_t ch = cur_saved & 0x00FF;
    if (!ch) ch = ' ';
    VGA[idx] = ch | ((uint16_t)col << 8);
    cur_on = 1;
}

static void cursor_move(int nx, int ny) {
    if (nx < 0) nx = 0;
    if (nx >= VGA_W) nx = VGA_W-1;
    if (ny < 0) ny = 0;
    if (ny >= ROW_STATUS) ny = ROW_STATUS-1;
    cursor_hide();
    cur_x = nx; cur_y = ny;
    cursor_show();
}

/* -----------------------------------------------------------------------
   Selection
   ----------------------------------------------------------------------- */
static int      sel_on = 0;
static int      sel_x0,sel_y0,sel_x1,sel_y1;
static uint16_t sel_orig[ROW_STATUS * 80];
static uint8_t  sel_mark[ROW_STATUS * 80]; /* 1=saved */

static void sel_restore(void) {
    for (int i = 0; i < ROW_STATUS * VGA_W; i++) {
        if (sel_mark[i]) {
            VGA[i] = sel_orig[i];
            sel_mark[i] = 0;
        }
    }
}

static void sel_paint(void) {
    int y0=sel_y0,x0=sel_x0,y1=sel_y1,x1=sel_x1;
    if (y0>y1||(y0==y1&&x0>x1)){int t;t=y0;y0=y1;y1=t;t=x0;x0=x1;x1=t;}
    for (int r=y0; r<=y1&&r<ROW_STATUS; r++) {
        int cs=(r==y0)?x0:0, ce=(r==y1)?x1:VGA_W-1;
        for (int c=cs; c<=ce&&c<VGA_W; c++) {
            int i=r*VGA_W+c;
            if (!sel_mark[i]) { sel_orig[i]=VGA[i]; sel_mark[i]=1; }
            uint16_t ch=VGA[i]&0xFF; if(!ch)ch=' ';
            VGA[i]=ch|(0x1E<<8); /* yellow on blue */
        }
    }
}

static void sel_end(void) {
    cursor_hide();
    sel_restore();
    sel_on = 0;
    cursor_show();
}

static void copy_sel(void) {
    int y0=sel_y0,x0=sel_x0,y1=sel_y1,x1=sel_x1;
    if (y0>y1||(y0==y1&&x0>x1)){int t;t=y0;y0=y1;y1=t;t=x0;x0=x1;x1=t;}
    int pos=0;
    for (int r=y0; r<=y1&&r<ROW_STATUS&&pos<CLIP_MAX; r++) {
        int cs=(r==y0)?x0:0, ce=(r==y1)?x1:VGA_W-1;
        for (int c=cs; c<=ce&&pos<CLIP_MAX; c++) {
            /* Read from orig if we saved it */
            int i=r*VGA_W+c;
            uint16_t cell=sel_mark[i]?sel_orig[i]:VGA[i];
            char ch=(char)(cell&0xFF); if(!ch)ch=' ';
            clip_buf[pos++]=ch;
        }
        if (r<y1&&pos<CLIP_MAX) clip_buf[pos++]='\n';
    }
    while (pos>0&&(clip_buf[pos-1]==' '||clip_buf[pos-1]=='\n')) pos--;
    clip_buf[pos]=0; clip_len=pos;
}

/* -----------------------------------------------------------------------
   Context menu
   ----------------------------------------------------------------------- */
#define CTX_W 14
#define CTX_H 4
static int ctx_on=0, ctx_x=0, ctx_y=0;
static uint16_t ctx_back[CTX_H*CTX_W];
static const char *ctx_items[CTX_H]={
    " Paste (^V) "," Copy  (^C) "," Cut   (^X) "," Clear line "
};

static void ctx_hide(void) {
    if (!ctx_on) return;
    for (int r=0;r<CTX_H;r++)
        for (int c=0;c<CTX_W;c++)
            VGA[(ctx_y+r)*VGA_W+ctx_x+c]=ctx_back[r*CTX_W+c];
    ctx_on=0;
}

static void ctx_show(int x, int y) {
    ctx_hide();
    if (x+CTX_W>=VGA_W) x=VGA_W-CTX_W-1;
    if (y+CTX_H>=ROW_INPUT) y=ROW_INPUT-CTX_H-1;
    ctx_x=x; ctx_y=y;
    for (int r=0;r<CTX_H;r++)
        for (int c=0;c<CTX_W;c++) {
            int vi=(ctx_y+r)*VGA_W+ctx_x+c;
            ctx_back[r*CTX_W+c]=VGA[vi];
            char ch=ctx_items[r][c]?ctx_items[r][c]:' ';
            VGA[vi]=(uint16_t)(unsigned char)ch|(0x70<<8);
        }
    ctx_on=1;
}

static int ctx_click(int x, int y) {
    if (!ctx_on) return 0;
    if (x<ctx_x||x>=ctx_x+CTX_W||y<ctx_y||y>=ctx_y+CTX_H) {
        cursor_hide(); ctx_hide(); cursor_show(); return 0;
    }
    int item=y-ctx_y;
    cursor_hide(); ctx_hide(); cursor_show();
    clip_buf[CLIP_MAX+1]=(char)(item+1);
    return 1;
}

/* -----------------------------------------------------------------------
   Mouse state
   ----------------------------------------------------------------------- */
static mouse_state_t state={0};
mouse_state_t *mouse_get_state(void){return &state;}

static uint8_t pkt[3]; static int pkt_i=0;
static int prev_l=0, prev_r=0;
static int acc_x=0, acc_y=0;
#define SCALE 10

static void mouse_irq(registers_t *regs) {
    (void)regs;
    uint8_t b=inb(PS2_DATA);
    pkt[pkt_i++]=b;
    if (pkt_i<3) return;
    pkt_i=0;

    uint8_t flags=pkt[0];
    if (!(flags&0x08)) return; /* desync */

    int dx=(int)(int8_t)pkt[1];
    int dy=-(int)(int8_t)pkt[2];
    int bl=(flags>>0)&1;
    int br=(flags>>1)&1;

    acc_x+=dx; acc_y+=dy;
    int cdx=acc_x/SCALE; acc_x%=SCALE;
    int cdy=acc_y/SCALE; acc_y%=SCALE;

    if (cdx||cdy) {
        int nx=cur_x+cdx, ny=cur_y+cdy;
        /* If dragging selection, update end point */
        if (bl && sel_on) {
            cursor_hide();
            sel_restore();
            cur_x=nx<0?0:nx>=VGA_W?VGA_W-1:nx;
            cur_y=ny<0?0:ny>=ROW_STATUS?ROW_STATUS-1:ny;
            sel_x1=cur_x; sel_y1=cur_y;
            sel_paint();
            cursor_show();
        } else {
            cursor_move(nx, ny);
        }
        state.x=cur_x; state.y=cur_y;
    }

    /* Left press */
    if (bl&&!prev_l) {
        if (ctx_click(cur_x,cur_y)){prev_l=bl;return;}
        /* Start fresh selection */
        sel_end();
        sel_on=1;
        sel_x0=sel_x1=cur_x;
        sel_y0=sel_y1=cur_y;
        for (int i=0;i<ROW_STATUS*VGA_W;i++) sel_mark[i]=0;
    }

    /* Left release */
    if (!bl&&prev_l) {
        if (sel_x0!=sel_x1||sel_y0!=sel_y1) {
            /* Keep highlight, copy text */
            copy_sel();
            /* Show [Copied] on status bar */
            const char *msg="[Copied]";
            for (int i=0;i<8;i++)
                VGA[ROW_STATUS*VGA_W+71+i]=
                    (uint16_t)(unsigned char)msg[i]|(0x70<<8);
        } else {
            /* Single click — clear selection */
            sel_end();
        }
    }

    /* Right press */
    if (br&&!prev_r) {
        if (ctx_on) { cursor_hide(); ctx_hide(); cursor_show(); }
        else { cursor_hide(); ctx_show(cur_x+1,cur_y); cursor_show(); }
    }

    prev_l=bl; prev_r=br;
    state.buttons=(bl?MOUSE_BTN_LEFT:0)|(br?MOUSE_BTN_RIGHT:0);
}

/* -----------------------------------------------------------------------
   Init
   ----------------------------------------------------------------------- */
static void ps2_wi(void){int t=100000;while(t--&&(inb(PS2_STATUS)&0x02));}
static void ps2_wo(void){int t=100000;while(t--&&!(inb(PS2_STATUS)&0x01));}
static void mcmd(uint8_t c){ps2_wi();outb(PS2_CMD,0xD4);ps2_wi();outb(PS2_DATA,c);}
static uint8_t mread(void){ps2_wo();return inb(PS2_DATA);}

void mouse_init(void) {
    /* Zero arrays */
    for (int i=0;i<ROW_STATUS*VGA_W;i++){sel_orig[i]=0;sel_mark[i]=0;}

    ps2_wi(); outb(PS2_CMD,0xA8);
    ps2_wi(); outb(PS2_CMD,0x20);
    ps2_wo();
    uint8_t cfg=inb(PS2_DATA);
    cfg|=0x02; cfg&=~0x20;
    ps2_wi(); outb(PS2_CMD,0x60);
    ps2_wi(); outb(PS2_DATA,cfg);

    mcmd(0xFF); mread(); mread(); mread();
    mcmd(0xF6); mread();
    mcmd(0xF4); mread();

    irq_install_handler(12, mouse_irq);
    cursor_show();

    terminal_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
    terminal_writeline("  [MOUSE] Ready. Drag=select  RightClick=menu  Ctrl+V=paste");
}
