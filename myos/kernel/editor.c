/*
 * editor.c — TanoojEdit full-screen text editor
 *
 * Layout:
 *   Row 0    : title bar
 *   Rows 1-22: text area (col 0-3 = line numbers, col 4-79 = text)
 *   Row 23   : status bar
 *   Row 24   : system status (unchanged)
 */

#include "editor.h"
#include "vfs.h"
#include "tfs.h"
#include "terminal.h"
#include "heap.h"
#include "mouse.h"
#include <stdint.h>
#include <stddef.h>

#define MAX_LINES  500
#define LINE_MAX   160
#define VIEW_ROWS  22
#define VIEW_COLS  76
#define TEXT_COL   4
#define UNDO_MAX   20
#define VGA_W      80
#define VGA_H      25

static volatile uint16_t * const VGA = (volatile uint16_t *)0xB8000;

/* -----------------------------------------------------------------------
   Helpers
   ----------------------------------------------------------------------- */
static inline uint16_t make_cell(char c, uint8_t color) {
    return (uint16_t)(unsigned char)c | ((uint16_t)color << 8);
}
static int ed_strlen(const char *s) { int n=0; while(s[n]) n++; return n; }
static void ed_strcpy(char *d, const char *s, int max) {
    int i=0; while(i<max-1&&s[i]){d[i]=s[i];i++;} d[i]=0;
}
static void ed_memcpy(char *d, const char *s, int n) {
    for(int i=0;i<n;i++) d[i]=s[i];
}

/* -----------------------------------------------------------------------
   VGA helpers
   ----------------------------------------------------------------------- */
static void vga_str(int row, int col, const char *s, uint8_t color) {
    for(int i=0; s[i]&&col+i<VGA_W; i++)
        VGA[row*VGA_W+col+i] = make_cell(s[i], color);
}
static void vga_fill(int row, int col, int len, char c, uint8_t color) {
    for(int i=0; i<len&&col+i<VGA_W; i++)
        VGA[row*VGA_W+col+i] = make_cell(c, color);
}
static void vga_dec(int row, int col, int val, uint8_t color) {
    char tmp[12]; int p=10; tmp[11]=0;
    if(val==0){VGA[row*VGA_W+col]=make_cell('0',color);return;}
    while(val>0){tmp[p--]='0'+(char)(val%10);val/=10;}
    for(int i=p+1;tmp[i]&&col+(i-p-1)<VGA_W;i++)
        VGA[row*VGA_W+col+(i-p-1)]=make_cell(tmp[i],color);
}

/* -----------------------------------------------------------------------
   Buffer
   ----------------------------------------------------------------------- */
static char buf[MAX_LINES][LINE_MAX+1];
static int  line_len[MAX_LINES];
static int  num_lines = 0;
static int  modified  = 0;
static char filename[48];
static int  is_active = 0;
static int  cur_row=0, cur_col=0, view_row=0;

/* -----------------------------------------------------------------------
   Undo/Redo
   ----------------------------------------------------------------------- */
/*
 * Undo/redo — stores only changed lines per edit, not the full buffer.
 * Each entry saves up to 3 lines (enough for insert/delete/newline).
 * 20 levels × 3 lines × 161 bytes = ~9.6 KiB total. Much leaner.
 */
#define UNDO_SAVE_LINES 3  /* max lines saved per snapshot */

typedef struct {
    /* Saved lines */
    char lines[UNDO_SAVE_LINES][LINE_MAX+1];
    int  lens[UNDO_SAVE_LINES];
    int  start_row;          /* which buffer row the saved lines start at */
    int  saved_count;        /* how many lines saved (1-3)                */
    /* Full buffer metadata (cheap to store) */
    int  num_lines;
    int  cur_row, cur_col;
    int  valid;
} undo_state_t;

static undo_state_t undo_stack[UNDO_MAX];
static int undo_top  = -1;
static int undo_live = -1;

static void snapshot_push(void) {
    undo_live = -1;  /* new edit cancels redo history */
    undo_top = (undo_top + 1) % UNDO_MAX;
    undo_state_t *s = &undo_stack[undo_top];
    s->num_lines   = num_lines;
    s->cur_row     = cur_row;
    s->cur_col     = cur_col;
    s->valid       = 1;
    s->start_row   = cur_row > 0 ? cur_row - 1 : 0;
    /* Save cur_row-1, cur_row, cur_row+1 (covers all edit cases) */
    int count = 0;
    for (int r = s->start_row;
         r <= cur_row + 1 && r < num_lines && count < UNDO_SAVE_LINES;
         r++, count++) {
        s->lens[count] = line_len[r];
        ed_memcpy(s->lines[count], buf[r], line_len[r] + 1);
    }
    s->saved_count = count;
    /* Invalidate the next slot so redo doesn't use stale data */
    int next = (undo_top + 1) % UNDO_MAX;
    undo_stack[next].valid = 0;
}

static void snapshot_restore(undo_state_t *s) {
    /* Restore changed lines back into the buffer */
    num_lines = s->num_lines;
    cur_row   = s->cur_row;
    cur_col   = s->cur_col;
    for (int i = 0; i < s->saved_count; i++) {
        int r = s->start_row + i;
        if (r >= MAX_LINES) break;
        line_len[r] = s->lens[i];
        ed_memcpy(buf[r], s->lines[i], s->lens[i] + 1);
    }
    modified = 1;
}

static void do_undo(void) {
    /* Find the most recent valid snapshot at or before undo_top */
    int search = (undo_live < 0) ? undo_top : (undo_live - 1 + UNDO_MAX) % UNDO_MAX;
    if (search < 0 || !undo_stack[search % UNDO_MAX].valid) {
        vga_str(23,54,"| [Nothing]  ",0x70); return;
    }
    /* Before restoring, save current state one level above for redo */
    if (undo_live < 0) {
        /* First undo — snapshot current state into a temp restore point */
        /* We just move undo_live to undo_top so redo can come back */
        undo_live = (undo_top + 1) % UNDO_MAX;
        /* Save current state there for redo */
        undo_state_t *tmp = &undo_stack[undo_live];
        tmp->num_lines = num_lines;
        tmp->cur_row = cur_row; tmp->cur_col = cur_col;
        tmp->start_row = cur_row > 0 ? cur_row-1 : 0;
        int count=0;
        for(int r=tmp->start_row; r<=cur_row+1&&r<num_lines&&count<UNDO_SAVE_LINES; r++,count++){
            tmp->lens[count]=line_len[r];
            ed_memcpy(tmp->lines[count],buf[r],line_len[r]+1);
        }
        tmp->saved_count=count; tmp->valid=1;
        search = undo_top;
    }
    undo_state_t *s = &undo_stack[search];
    if (!s->valid) { vga_str(23,54,"| [Nothing]  ",0x70); return; }
    snapshot_restore(s);
    undo_live = search;
    vga_str(23,54,"| [Undo]     ",0x70);
}

static void do_redo(void) {
    if (undo_live < 0) { vga_str(23,54,"| [No redo]  ",0x70); return; }
    int next = (undo_live + 1) % UNDO_MAX;
    undo_state_t *s = &undo_stack[next];
    if (!s->valid) { vga_str(23,54,"| [No redo]  ",0x70); return; }
    snapshot_restore(s);
    /* If we just redid back to the live state, exit undo mode */
    int live_pos = (undo_top + 1) % UNDO_MAX;
    undo_live = (next == live_pos) ? -1 : next;
    vga_str(23,54,"| [Redo]     ",0x70);
}

/* -----------------------------------------------------------------------
   Buffer operations
   ----------------------------------------------------------------------- */
static void insert_char(char c) {
    if(num_lines==0){num_lines=1;line_len[0]=0;buf[0][0]=0;}
    if(cur_row>=MAX_LINES||line_len[cur_row]>=LINE_MAX) return;
    snapshot_push();
    int len=line_len[cur_row];
    for(int i=len;i>cur_col;i--) buf[cur_row][i]=buf[cur_row][i-1];
    buf[cur_row][cur_col]=c;
    line_len[cur_row]++;
    buf[cur_row][line_len[cur_row]]=0;
    cur_col++; modified=1;
}

static void delete_char(void) {
    snapshot_push();
    if(cur_col==0){
        if(cur_row==0) return;
        int plen=line_len[cur_row-1], clen=line_len[cur_row];
        if(plen+clen<=LINE_MAX){
            ed_memcpy(buf[cur_row-1]+plen, buf[cur_row], clen);
            line_len[cur_row-1]=plen+clen;
            buf[cur_row-1][line_len[cur_row-1]]=0;
            for(int r=cur_row;r<num_lines-1;r++){
                ed_memcpy(buf[r],buf[r+1],line_len[r+1]+1);
                line_len[r]=line_len[r+1];
            }
            num_lines--; cur_row--; cur_col=plen;
        }
    } else {
        for(int i=cur_col-1;i<line_len[cur_row]-1;i++)
            buf[cur_row][i]=buf[cur_row][i+1];
        line_len[cur_row]--;
        buf[cur_row][line_len[cur_row]]=0;
        cur_col--;
    }
    modified=1;
}

static void insert_newline(void) {
    if(num_lines>=MAX_LINES) return;
    snapshot_push();
    for(int r=num_lines;r>cur_row+1;r--){
        ed_memcpy(buf[r],buf[r-1],line_len[r-1]+1);
        line_len[r]=line_len[r-1];
    }
    int tail=line_len[cur_row]-cur_col;
    ed_memcpy(buf[cur_row+1],buf[cur_row]+cur_col,tail);
    line_len[cur_row+1]=tail; buf[cur_row+1][tail]=0;
    line_len[cur_row]=cur_col; buf[cur_row][cur_col]=0;
    num_lines++; cur_row++; cur_col=0; modified=1;
}

/* -----------------------------------------------------------------------
   Render
   ----------------------------------------------------------------------- */
static void draw_title(void) {
    vga_fill(0,0,VGA_W,' ',0x1F);
    vga_str(0,1,"TanoojEdit",0x1F);
    vga_str(0,12,"| ",0x1B);
    vga_str(0,14,filename[0]?filename:"[new]",0x1F);
    if(modified){int l=14+ed_strlen(filename[0]?filename:"[new]");vga_str(0,l," *",0x1E);}
    const char *h="^S=Save ^Q=Quit ^Z=Undo ^U=Redo ^H=Help";
    vga_str(0,VGA_W-ed_strlen(h)-1,h,0x1B);
}

static void draw_status(void) {
    vga_fill(23,0,VGA_W,' ',0x70);
    vga_str(23,1,"Ln:",0x70); vga_dec(23,4,cur_row+1,0x70);
    vga_str(23,8,"Col:",0x70); vga_dec(23,12,cur_col+1,0x70);
    vga_str(23,18,"|",0x70);
    vga_str(23,20,filename[0]?filename:"[new]",0x70);
    vga_str(23,38,"|",0x70); vga_dec(23,40,num_lines,0x70);
    vga_str(23,45,"lines",0x70);
    if(modified) vga_str(23,52,"| [modified] ",0x78);
    else         vga_str(23,52,"| saved      ",0x70);
}

static void draw_line(int screen_row) {
    int br=view_row+screen_row-1;
    vga_fill(screen_row,0,4,' ',0x08);
    if(br<num_lines) vga_dec(screen_row,0,br+1,0x08);
    VGA[screen_row*VGA_W+3]=make_cell('|',0x08);
    uint8_t color=(br==cur_row)?0x0F:0x07;
    vga_fill(screen_row,TEXT_COL,VIEW_COLS,' ',color);
    if(br<num_lines){
        int show=line_len[br]<VIEW_COLS?line_len[br]:VIEW_COLS;
        for(int c=0;c<show;c++)
            VGA[screen_row*VGA_W+TEXT_COL+c]=make_cell(buf[br][c],color);
    }
}

static void draw_cursor(void) {
    int sr=cur_row-view_row+1;
    if(sr<1||sr>VIEW_ROWS) return;
    int sc=TEXT_COL+cur_col;
    if(sc>=VGA_W) return;
    int idx=sr*VGA_W+sc;
    uint8_t c=(VGA[idx]>>8)&0xFF;
    uint8_t fg=c&0x0F, bg=(c>>4)&0x0F;
    VGA[idx]=(VGA[idx]&0x00FF)|(uint16_t)((bg|(fg<<4))<<8);
}

static void redraw(void) {
    draw_title();
    for(int r=1;r<=VIEW_ROWS;r++) draw_line(r);
    draw_status();
    draw_cursor();
}

static void scroll_to_cursor(void) {
    if(cur_row<view_row) view_row=cur_row;
    if(cur_row>=view_row+VIEW_ROWS) view_row=cur_row-VIEW_ROWS+1;
    if(view_row<0) view_row=0;
}

/* -----------------------------------------------------------------------
   Load / Save
   ----------------------------------------------------------------------- */
static void load_file(const char *name) {
    num_lines=0; cur_row=0; cur_col=0; view_row=0;
    vfs_node_t *f=vfs_resolve(name);
    if(f&&f->type==VFS_FILE&&f->data&&f->size>0){
        const char *p=f->data; uint32_t rem=f->size;
        while(rem>0&&num_lines<MAX_LINES){
            int i=0;
            while(rem>0&&*p!='\n'&&i<LINE_MAX){buf[num_lines][i++]=*p++;rem--;}
            if(rem>0&&*p=='\n'){p++;rem--;}
            buf[num_lines][i]=0; line_len[num_lines]=i; num_lines++;
        }
    }
    if(num_lines==0){num_lines=1;line_len[0]=0;buf[0][0]=0;}
}

static void save_file(void) {
    if(!filename[0]){ vga_str(23,1,"No filename — use ^W to rename        ",0x4F); return; }
    char *flat=(char*)kmalloc(num_lines*(LINE_MAX+1)+1);
    if(!flat) return;
    int pos=0;
    for(int r=0;r<num_lines;r++){
        for(int c=0;c<line_len[r];c++) flat[pos++]=buf[r][c];
        if(r<num_lines-1) flat[pos++]='\n';
    }
    flat[pos]=0;
    vfs_node_t *f=vfs_find(vfs_get_cwd(),filename);
    if(!f) f=vfs_create(vfs_get_cwd(),filename);
    if(f) vfs_write(f,flat,(uint32_t)pos);
    tfs_save_file(filename,flat,(uint32_t)pos);
    kfree(flat);
    modified=0;
}

/* -----------------------------------------------------------------------
   Help overlay
   ----------------------------------------------------------------------- */
static int help_shown=0;
static void show_help(void) {
    const char *lines[]={
        "+------------------------------------------+",
        "|        TanoojEdit  Keybindings           |",
        "+------------------------------------------+",
        "|  Arrows        Move cursor               |",
        "|  Home/End      Line start/end            |",
        "|  PgUp/PgDn     Scroll                    |",
        "|  Backspace     Delete char               |",
        "|  Enter         New line                  |",
        "|  Tab           4 spaces                  |",
        "|  Ctrl+S        Save                      |",
        "|  Ctrl+Q / Esc  Quit                      |",
        "|  Ctrl+Z        Undo                      |",
        "|  Ctrl+U        Redo                      |",
        "|  Ctrl+C        Copy current line         |",
        "|  Ctrl+X        Cut current line          |",
        "|  Ctrl+V        Paste                     |",
        "+------------------------------------------+",
        "|       Press any key to close             |",
        "+------------------------------------------+",
        NULL
    };
    for(int i=0;lines[i];i++){
        int row=3+i; if(row>=23) break;
        vga_fill(row,18,44,' ',0x30);
        for(int c=0;lines[i][c]&&c<44;c++)
            VGA[row*VGA_W+18+c]=make_cell(lines[i][c],0x3F);
    }
}

/* -----------------------------------------------------------------------
   Scancode tables
   ----------------------------------------------------------------------- */
static const char sc_ascii[128]={
 0,0,'1','2','3','4','5','6','7','8','9','0','-','=',0,0,
 'q','w','e','r','t','y','u','i','o','p','[',']',0,0,'a','s',
 'd','f','g','h','j','k','l',';','\'','`',0,'\\','z','x','c','v',
 'b','n','m',',','.','/',0,'*',0,' ',0,0,0,0,0,0,
 0,0,0,0,0,0,0,'7','8','9','-','4','5','6','+','1',
 '2','3','0','.',0,0,0,0,0,0,0,0,0,0,0,0,
 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};
static const char sc_shift[128]={
 0,0,'!','@','#','$','%','^','&','*','(',')','_','+',0,0,
 'Q','W','E','R','T','Y','U','I','O','P','{','}',0,0,'A','S',
 'D','F','G','H','J','K','L',':','"','~',0,'|','Z','X','C','V',
 'B','N','M','<','>','?',0,'*',0,' ',0,0,0,0,0,0,
 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};

/* Extended scancodes arrive with bit 7 set */
#define SC_UP    0xC8
#define SC_DOWN  0xD0
#define SC_LEFT  0xCB
#define SC_RIGHT 0xCD
#define SC_HOME  0xC7
#define SC_END   0xCF
#define SC_PGUP  0xC9
#define SC_PGDN  0xD1
#define SC_BKSP  0x0E
#define SC_ENTER 0x1C
#define SC_ESC   0x01
#define SC_TAB   0x0F

/* -----------------------------------------------------------------------
   editor_key
   ----------------------------------------------------------------------- */
void editor_key(uint8_t scancode, int shift, int ctrl) {
    if(!is_active) return;

    if(help_shown){ help_shown=0; redraw(); return; }

    if(ctrl){
        switch(scancode){
            case 0x1F: save_file(); break;             /* Ctrl+S */
            case 0x10:                                 /* Ctrl+Q */
                is_active=0; terminal_init(); return;
            case 0x31: /* Ctrl+N */
                num_lines=1;line_len[0]=0;buf[0][0]=0;
                cur_row=0;cur_col=0;view_row=0;
                filename[0]=0;modified=0;
                for(int i=0;i<UNDO_MAX;i++) undo_stack[i].valid=0;
                undo_top=-1; undo_live=-1;
                break;
            case 0x23: show_help(); help_shown=1; return; /* Ctrl+H */
            case 0x2C: do_undo(); scroll_to_cursor(); redraw(); return; /* Ctrl+Z */
            case 0x16: do_redo(); scroll_to_cursor(); redraw(); return; /* Ctrl+U */
            case 0x2E: /* Ctrl+C — copy line */
                if(cur_row<num_lines)
                    clipboard_copy(buf[cur_row],line_len[cur_row]);
                break;
            case 0x2D: /* Ctrl+X — cut line */
                if(cur_row<num_lines){
                    snapshot_push();
                    clipboard_copy(buf[cur_row],line_len[cur_row]);
                    line_len[cur_row]=0;buf[cur_row][0]=0;
                    cur_col=0;modified=1;
                }
                break;
            case 0x2F: { /* Ctrl+V — paste */
                const char *clip=clipboard_get();
                int clen=clipboard_len();
                for(int i=0;i<clen;i++){
                    if(clip[i]=='\n') insert_newline();
                    else if(clip[i]>=32&&clip[i]<127) insert_char(clip[i]);
                }
                break;
            }
        }
        scroll_to_cursor(); redraw(); return;
    }

    switch(scancode){
        case SC_ESC:
            is_active=0; terminal_init(); return;
        case SC_UP:
            if(cur_row>0){cur_row--;if(cur_col>line_len[cur_row])cur_col=line_len[cur_row];}
            break;
        case SC_DOWN:
            if(cur_row<num_lines-1){cur_row++;if(cur_col>line_len[cur_row])cur_col=line_len[cur_row];}
            break;
        case SC_LEFT:
            if(cur_col>0) cur_col--;
            else if(cur_row>0){cur_row--;cur_col=line_len[cur_row];}
            break;
        case SC_RIGHT:
            if(cur_col<line_len[cur_row]) cur_col++;
            else if(cur_row<num_lines-1){cur_row++;cur_col=0;}
            break;
        case SC_HOME: cur_col=0; break;
        case SC_END:  cur_col=line_len[cur_row]; break;
        case SC_PGUP: cur_row-=VIEW_ROWS; if(cur_row<0)cur_row=0; cur_col=0; break;
        case SC_PGDN: cur_row+=VIEW_ROWS; if(cur_row>=num_lines)cur_row=num_lines-1; cur_col=0; break;
        case SC_BKSP:  delete_char(); break;
        case SC_ENTER: insert_newline(); break;
        case SC_TAB:   for(int i=0;i<4;i++) insert_char(' '); break;
        default:{
            char c=shift?sc_shift[scancode]:sc_ascii[scancode];
            if(c) insert_char(c);
            break;
        }
    }

    if(cur_row<0) cur_row=0;
    if(cur_row>=num_lines) cur_row=num_lines-1;
    if(cur_col<0) cur_col=0;
    if(cur_col>line_len[cur_row]) cur_col=line_len[cur_row];

    scroll_to_cursor();
    redraw();
}

/* -----------------------------------------------------------------------
   editor_open
   ----------------------------------------------------------------------- */
void editor_open(const char *name) {
    if(name&&name[0]){
        ed_strcpy(filename,name,48);
        load_file(name);
    } else {
        filename[0]=0;
        num_lines=1;line_len[0]=0;buf[0][0]=0;
    }
    cur_row=0;cur_col=0;view_row=0;
    modified=0;help_shown=0;
    undo_top=-1; undo_live=-1;
    for(int i=0;i<UNDO_MAX;i++) undo_stack[i].valid=0;
    is_active=1;
    redraw();
}

int editor_active(void) { return is_active; }
