/* wm.c - PC11 window manager (UEFI / GOP framebuffer).
 *
 * Features:
 *   - desktop + top taskbar (one entry per window; click to restore/raise)
 *   - windows with title bar, [-] minimize and [x] close buttons
 *   - a working Terminal window: type and see characters appear
 *   - mouse (PS/2): move cursor; click a window to focus + raise it;
 *                   drag a title bar to move; click [-]/[x] buttons
 *   - keyboard (PS/2): TAB cycles focus, arrows move the focused window,
 *                      typing goes to the Terminal when it's focused,
 *                      ESC clears the screen and halts
 *
 * Input is read straight from the PS/2 controller after ExitBootServices
 * (the UEFI pointer protocol is unreliable under OVMF). Everything is
 * software-rendered into a back buffer, then blitted to the framebuffer.
 */
#include "efi.h"
#include "font8x8.h"
#include "png.h"
#include "jpg.h"

#include "crash.h"

static EFI_SYSTEM_TABLE *ST;
static EFI_BOOT_SERVICES *BS;
static EFI_RUNTIME_SERVICES *RT;   /* survives ExitBootServices */
static EFI_GRAPHICS_OUTPUT_PROTOCOL *GOP;

/* framebuffer geometry */
static UINT32 *FB;          /* hardware framebuffer */
static UINT32 *BACK;        /* back buffer */
static UINT32 *DESK;        /* cached scaled desktop (background+solid) */
static int     desk_dirty = 1;  /* rebuild DESK when set (wallpaper/mode change) */

/* freestanding memcpy/memset: GCC/-O2 lowers struct copies and our bulk
 * blits to these symbols, but we link -nostdlib, so provide them. The blit
 * copies are 4-byte aligned and large, so copy 8 bytes at a time. */
__attribute__((used)) void *memcpy(void *d, const void *s, UINTN n){
    UINTN i=0;
    if(!(((UINTN)d|(UINTN)s|n)&7)){            /* all 8-byte aligned/sized */
        UINT64 *dd=(UINT64*)d; const UINT64 *ss=(const UINT64*)s;
        for(UINTN k=n>>3;k;k--) *dd++=*ss++;
        return d;
    }
    { UINT8 *db=(UINT8*)d; const UINT8 *sb=(const UINT8*)s;
      for(;i<n;i++) db[i]=sb[i]; }
    return d;
}
__attribute__((used)) void *memset(void *d, int c, UINTN n){
    UINT8 *db=(UINT8*)d; for(UINTN i=0;i<n;i++) db[i]=(UINT8)c; return d;
}
static UINT32  SW, SH;      /* screen width/height */
static UINT32  PPSL;        /* pixels per scan line (stride) */

/* ---- colors (0x00RRGGBB; GOP BGR/RGB both common, we target BGRX) ---- */
#define RGB(r,g,b) (((UINT32)(r)<<16)|((UINT32)(g)<<8)|(UINT32)(b))
#define C_DESKTOP RGB(0x1e,0x2a,0x40)
#define C_WIN     RGB(0xec,0xec,0xec)
#define C_TITLE   RGB(0x3a,0x6e,0xa5)
#define C_TITLE_A RGB(0x2f,0x9e,0x44)   /* active window title */
#define C_BORDER  RGB(0x10,0x10,0x10)
#define C_TEXT    RGB(0x20,0x20,0x20)
#define C_TITLET  RGB(0xff,0xff,0xff)
#define C_CURSOR  RGB(0xff,0xff,0xff)
#define C_CUR_OUT RGB(0x00,0x00,0x00)


/* ===================== drawing into BACK buffer ===================== */
static inline void px(int x,int y,UINT32 col){
    if(x<0||y<0||x>=(int)SW||y>=(int)SH) return;
    BACK[(UINT32)y*SW + (UINT32)x] = col;
}
/* blend 'col' over the existing back-buffer pixel by alpha a (0..255). */
static inline void px_blend(int x,int y,UINT32 col,int a){
    if(x<0||y<0||x>=(int)SW||y>=(int)SH) return;
    if(a<=0) return;
    if(a>=255){ BACK[(UINT32)y*SW+x]=col; return; }
    UINT32 bg = BACK[(UINT32)y*SW + (UINT32)x];
    int br=(bg>>16)&0xff, bgc=(bg>>8)&0xff, bb=bg&0xff;
    int cr=(col>>16)&0xff, cg=(col>>8)&0xff, cb=col&0xff;
    int r=(cr*a + br*(255-a))/255;
    int g=(cg*a + bgc*(255-a))/255;
    int b=(cb*a + bb*(255-a))/255;
    BACK[(UINT32)y*SW + (UINT32)x] = ((UINT32)r<<16)|((UINT32)g<<8)|(UINT32)b;
}
static void fill_rect(int x,int y,int w,int h,UINT32 col){
    for(int j=0;j<h;j++) for(int i=0;i<w;i++) px(x+i,y+j,col);
}
static void rect_border(int x,int y,int w,int h,UINT32 col){
    for(int i=0;i<w;i++){ px(x+i,y,col); px(x+i,y+h-1,col); }
    for(int j=0;j<h;j++){ px(x,y+j,col); px(x+w-1,y+j,col); }
}

/* is pixel (i,j) inside a rect with rounded corners of radius r?
 * (i,j) are local coords 0..w-1 / 0..h-1. */
static int in_round(int i,int j,int w,int h,int r){
    int cx, cy;
    if(i<r && j<r){ cx=r; cy=r; }                      /* top-left  */
    else if(i>=w-r && j<r){ cx=w-1-r; cy=r; }          /* top-right */
    else if(i<r && j>=h-r){ cx=r; cy=h-1-r; }          /* bot-left  */
    else if(i>=w-r && j>=h-r){ cx=w-1-r; cy=h-1-r; }   /* bot-right */
    else return 1;                                      /* straight edge area */
    int dx=i-cx, dy=j-cy;
    return dx*dx+dy*dy <= r*r;
}
/* filled rectangle with rounded corners (only paints inside the radius). */
static void fill_round_rect(int x,int y,int w,int h,int r,UINT32 col){
    for(int j=0;j<h;j++) for(int i=0;i<w;i++)
        if(in_round(i,j,w,h,r)) px(x+i,y+j,col);
}
/* fill a rect whose TOP corners are rounded (radius r), bottom square. */
static void fill_top_round(int x,int y,int w,int h,int r,UINT32 col){
    for(int j=0;j<h;j++) for(int i=0;i<w;i++){
        int rounded = (j<r) ? in_round(i,j,w,h+r,r) : 1; /* only top corners */
        if(rounded) px(x+i,y+j,col);
    }
}
/* anti-aliased rounded border outline */
static void round_border(int x,int y,int w,int h,int r,UINT32 col){
    for(int i=0;i<w;i++){
        if(in_round(i,0,w,h,r)) px(x+i,y,col);
        if(in_round(i,h-1,w,h,r)) px(x+i,y+h-1,col);
    }
    for(int j=0;j<h;j++){
        if(in_round(0,j,w,h,r)) px(x,y+j,col);
        if(in_round(w-1,j,w,h,r)) px(x+w-1,y+j,col);
    }
}

/* read one bit of a glyph (LSB = left pixel); out of range = 0 */
static inline int glyph_bit(const unsigned char *g,int gx,int gy){
    if(gx<0||gx>7||gy<0||gy>7) return 0;
    return (g[gy]>>gx)&1;
}

/* Anti-aliased glyph drawing at the SAME 8x8 size.
 * For each output pixel we estimate stroke coverage from the glyph bit
 * plus its neighbours, then alpha-blend the text colour. "On" pixels stay
 * fully opaque; the soft coverage rounds off the staircase edges. */
static void draw_char(int x,int y,char c,UINT32 col){
    unsigned char uc = (unsigned char)c;
    if(uc < 0x20 || uc > 0x7E) uc = ' ';
    const unsigned char *g = font8x8_basic[uc - 0x20];
    for(int gy=0;gy<8;gy++){
        for(int gx=0;gx<8;gx++){
            int here = glyph_bit(g,gx,gy);
            if(here){
                px_blend(x+gx,y+gy,col,255);          /* solid stroke */
            } else {
                /* soften: if next to set pixels, add partial coverage.
                 * orthogonal neighbours weigh more than diagonals. */
                int o = glyph_bit(g,gx-1,gy)+glyph_bit(g,gx+1,gy)
                       +glyph_bit(g,gx,gy-1)+glyph_bit(g,gx,gy+1);
                int d = glyph_bit(g,gx-1,gy-1)+glyph_bit(g,gx+1,gy-1)
                       +glyph_bit(g,gx-1,gy+1)+glyph_bit(g,gx+1,gy+1);
                int cov = o*46 + d*18;                /* tuned coverage */
                if(cov>150) cov=150;                  /* keep edges subtle */
                if(cov>0) px_blend(x+gx,y+gy,col,cov);
            }
        }
    }
}
static void draw_text(int x,int y,const char *s,UINT32 col){
    while(*s){ draw_char(x,y,*s,col); x+=8; s++; }
}
/* draw text that may contain '\n' line breaks */
static void draw_text_ml(int x,int y,const char *s,UINT32 col){
    int cx=x;
    for(; *s; s++){
        if(*s=='\n'){ cx=x; y+=12; continue; }
        draw_char(cx,y,*s,col); cx+=8;
    }
}
/* draw text bounded to a box: wraps at maxx, stops at maxy, honors '\n'.
 * Keeps everything inside the window so it never spills onto the desktop. */
static void draw_text_box(int x,int y,int maxx,int maxy,const char *s,UINT32 col){
    int cx=x;
    for(; *s; s++){
        if(*s=='\n'){ cx=x; y+=12; if(y> maxy) return; continue; }
        if(cx+8 > maxx){ cx=x; y+=12; if(y> maxy) return; }   /* wrap */
        if(y> maxy) return;
        draw_char(cx,y,*s,col); cx+=8;
    }
}
/* draw a single line clipped to maxx (truncates, no wrap) */
static void draw_text_clip(int x,int y,int maxx,const char *s,UINT32 col){
    int cx=x;
    for(; *s; s++){
        if(cx+8 > maxx) return;
        draw_char(cx,y,*s,col); cx+=8;
    }
}
/* draw the baked-in PC11 icon (full colour, with transparency mask) at (x,y) */
/* Start-menu icon: loaded from /home/logo.png at boot and scaled down with
 * area averaging when drawn (smooth, like the wallpaper). */
#define ICON_W 28
#define ICON_H 28
static unsigned int *logo_px = NULL;   /* decoded full-res logo (RGBA->RGB) */
static int logo_w = 0, logo_h = 0;

/* draw the start-menu icon at (x,y), area-averaged down to ICON_W x ICON_H.
 * Near-white source pixels are treated as transparent so the round logo
 * floats cleanly on the button. */
static void draw_icon(int x,int y){
    if(!logo_px || logo_w<=0 || logo_h<=0) return;
    for(int j=0;j<ICON_H;j++){
        for(int i=0;i<ICON_W;i++){
            /* the source block that maps to this output pixel */
            int sx0=(int)((long)i*logo_w/ICON_W);
            int sx1=(int)((long)(i+1)*logo_w/ICON_W); if(sx1<=sx0) sx1=sx0+1;
            int sy0=(int)((long)j*logo_h/ICON_H);
            int sy1=(int)((long)(j+1)*logo_h/ICON_H); if(sy1<=sy0) sy1=sy0+1;
            /* Average ONLY the disc (non-white) pixels for colour, and use the
             * fraction of disc pixels as alpha. This avoids white edge pixels
             * polluting the colour and gives smooth anti-aliased edges. */
            long r=0,g=0,b=0,disc=0,total=0;
            for(int sy=sy0;sy<sy1 && sy<logo_h;sy++)
              for(int sx=sx0;sx<sx1 && sx<logo_w;sx++){
                unsigned int c=logo_px[(long)sy*logo_w+sx];
                int cr=(c>>16)&0xff, cg=(c>>8)&0xff, cb=c&0xff;
                total++;
                if(!(cr>236 && cg>236 && cb>236)){      /* a disc (coloured) pixel */
                    r+=cr; g+=cg; b+=cb; disc++;
                }
              }
            if(total==0 || disc==0) continue;            /* fully outside the disc */
            UINT32 col = ((r/disc)<<16)|((g/disc)<<8)|(b/disc);
            int a = (int)(disc*255/total);               /* edge coverage = alpha */
            px_blend(x+i, y+j, col, a);
        }
    }
}

/* ===================== app content ===================== */
static const char *ABOUT_TEXT =
    "welcome to pc11\n"
    "\n"
    "pc11 is an tiny experimental os\n"
    "it is v0.2 it use 64x and uefi\n"
    "to run on modern pc's";

/* ---- Calculator state ---- */
static long calc_acc = 0;        /* accumulated value */
static long calc_cur = 0;        /* number being entered */
static int  calc_has_cur = 0;    /* is a number being entered? */
static char calc_op = 0;         /* pending operator: + - * / or 0 */
static int  calc_err = 0;        /* divide-by-zero etc */
/* layout: 4 cols x 5 rows of buttons; labels: */
static const char calc_keys[5][4] = {
    {'7','8','9','/'},
    {'4','5','6','*'},
    {'1','2','3','-'},
    {'0','C','=','+'},
    {0,0,0,0}
};
static void calc_apply(void){
    if(!calc_op){ calc_acc=calc_cur; return; }
    switch(calc_op){
        case '+': calc_acc += calc_cur; break;
        case '-': calc_acc -= calc_cur; break;
        case '*': calc_acc *= calc_cur; break;
        case '/': if(calc_cur==0){ calc_err=1; } else calc_acc /= calc_cur; break;
    }
}
static void calc_key(char k){
    if(calc_err && k!='C') return;
    if(k>='0'&&k<='9'){
        calc_cur = calc_cur*10 + (k-'0');
        calc_has_cur=1;
    } else if(k=='C'){
        calc_acc=0; calc_cur=0; calc_has_cur=0; calc_op=0; calc_err=0;
    } else if(k=='='){
        if(calc_has_cur){ calc_apply(); }
        calc_op=0; calc_has_cur=0; calc_cur=0;
    } else { /* operator */
        if(calc_has_cur){ calc_apply(); }
        else if(!calc_op) calc_acc=calc_cur;
        calc_op=k; calc_has_cur=0; calc_cur=0;
    }
}
/* the value to show on the calculator display */
static long calc_display(void){ return calc_has_cur ? calc_cur : calc_acc; }

/* ---- Text editor state (edits a real file, saves back) ---- */
#define EDIT_MAX 4096
static char edit_buf[EDIT_MAX];
static int  edit_len = 0;
static char edit_name[64] = "untitled.txt";
static char edit_status[48] = "type; Ctrl not needed - use Save btn";
static int  edit_loaded = 0;     /* did we load a file? */
static int  edit_open_mode = 0;  /* showing the file-picker overlay? */

/* ===================== live file manager state =====================
 * Boot services stay ALIVE, so we browse real drives/folders on demand
 * via the UEFI Simple File System + File protocols.
 */
#define MAX_ENTRIES 128
#define MAX_NAME    64
#define MAX_PREVIEW 2048
#define MAX_VOLS    8

typedef struct {
    char name[MAX_NAME];
    int  is_dir;
    UINT64 size;
} DirEntry;

static EFI_FILE_PROTOCOL *fm_roots[MAX_VOLS];   /* root of each volume */
static int  fm_nvols = 0;                        /* how many volumes found */
static int  fm_vol = -1;                         /* current volume (-1 = drive list) */
static EFI_FILE_PROTOCOL *fm_dir = NULL;         /* current open directory */
static char fm_path[256] = "/";                  /* display path */

static DirEntry fm_entries[MAX_ENTRIES];
static int  fm_count = 0;                         /* entries in current dir */
static int  fm_sel   = 0;                         /* selected entry */
static int  fm_scroll= 0;                         /* list scroll offset */

static char fm_preview[MAX_PREVIEW];              /* file preview text */
static int  fm_preview_len = 0;
static char fm_status[80] = "";                   /* status / message line */

/* ---- clipboard (copy/paste a file) ----
 * We copy the file's BYTES at copy time, not a directory handle (handles
 * get closed when you navigate away, which would dangle and crash). */
static unsigned char *clip_data = NULL;           /* copied file bytes */
static unsigned long  clip_len  = 0;
static char clip_name[MAX_NAME] = "";             /* copied file name ("" = empty) */

/* ---- right-click context menu ---- */
static int  ctx_open = 0;                          /* context menu showing? */
static int  ctx_x, ctx_y;                          /* its top-left position */
static int  ctx_on_file = 0;                       /* right-clicked a file? */
static char ctx_target[MAX_NAME] = "";             /* the file it targets */

static int ends_with(const char *s, const char *suf);   /* fwd decl */
/* is this filename an image we can decode? */
static int is_image_name(const char *n){
    return ends_with(n,".png") || ends_with(n,".bmp")
        || ends_with(n,".jpg") || ends_with(n,".jpeg");
}

/* ---- desktop background image (decoded RGBA, owned in RAM) ---- */
static UINT32 *bg_img = NULL;                       /* W*H pixels, or NULL */
static int bg_w = 0, bg_h = 0;

/* ===================== windows ===================== */
#define WIN_NORMAL   0
#define WIN_TERMINAL 1
#define WIN_POWER    2     /* power-off app: Shutdown / Restart buttons */
#define WIN_ABOUT    3     /* About PC11: multi-line info text */
#define WIN_FILES    4     /* Files: a real file browser */
#define WIN_CALC     5     /* Calculator */
#define WIN_EDIT     6     /* Text editor (loads/saves real files) */
#define WIN_CLOCK    7     /* Clock widget */

typedef struct {
    int  x,y,w,h;
    const char *title;
    const char *body;     /* static body text (normal windows) */
    int  kind;
    int  minimized;       /* 1 = hidden (only on taskbar) */
    int  alive;           /* 0 = closed */
} Win;

#define NWIN 7
#define MIN_W 160         /* minimum window width  */
#define MIN_H 90          /* minimum window height */
#define GRIP  14          /* resize grip size (bottom-right corner) */
/* All windows start CLOSED (alive=0). The user opens them from the
 * PC11 Start menu. The x/y/w/h are their default spawn positions. */
static Win wins[NWIN] = {
    { 80,  70, 360, 170, "About PC11", NULL, WIN_ABOUT,    0, 0 },
    {340, 120, 360, 240, "Files",      NULL, WIN_FILES,    0, 0 },
    {150, 300, 380, 200, "Terminal",   NULL, WIN_TERMINAL, 0, 0 },
    {460, 200, 200, 160, "Power Off",  NULL, WIN_POWER,    0, 0 },
    {120, 120, 200, 250, "Calculator", NULL, WIN_CALC,     0, 0 },
    {200, 100, 420, 280, "Editor",     NULL, WIN_EDIT,     0, 0 },
    {520,  90, 180, 110, "Clock",      NULL, WIN_CLOCK,    0, 0 },
};

/* z-order: zorder[0] = bottom ... zorder[top] = topmost/focused */
static int zorder[NWIN] = {0,1,2,3,4,5,6};

#define TITLE_H 20
#define WIN_R   8          /* window corner radius */
#define BTN_W   16          /* width of each title-bar button */
#define BTN_GAP 4

/* ---- terminal state ---- */
#define TERM_COLS 42
#define TERM_ROWS 18
#define TERM_MAX  (TERM_COLS*TERM_ROWS)
static char term_buf[TERM_MAX];   /* text grid (row-major) */
static int  term_len = 0;         /* number of chars typed so far */

static void term_print(const char *s);  /* fwd */
static void term_prompt(void);           /* fwd */
static void term_init(void){
    term_len = 0;
    term_print("PC11 terminal - type 'help'\n");
    term_prompt();
}
static void term_putc(char c){
    if(c=='\n'){
        int col = term_len % TERM_COLS;
        while(col++ < TERM_COLS && term_len < TERM_MAX) term_buf[term_len++]=' ';
    } else if(c=='\b'){
        if(term_len>0) term_len--;
    } else if(term_len < TERM_MAX){
        term_buf[term_len++]=c;
    }
    /* scroll: if full, drop the first row */
    if(term_len >= TERM_MAX){
        for(int i=0;i<TERM_MAX-TERM_COLS;i++) term_buf[i]=term_buf[i+TERM_COLS];
        term_len = TERM_MAX-TERM_COLS;
    }
}
static void term_print(const char *s){ while(*s) term_putc(*s++); }
static void term_prompt(void){ term_print("# "); }

/* current input line being typed (between prompts) */
#define LINE_MAX 64
static char term_line[LINE_MAX];
static int  term_line_len = 0;

/* compare a typed line against a literal command name */
static int streq(const char *a, const char *b){
    int i=0;
    for(;a[i]&&b[i];i++) if(a[i]!=b[i]) return 0;
    return a[i]==b[i];
}

/* run a command and print its output */
static void term_exec(const char *cmd){
    if(cmd[0]==0){
        /* empty line: nothing */
    } else if(streq(cmd,"help")){
        term_print("commands:\n");
        term_print(" help  - this list\n");
        term_print(" ver   - version\n");
        term_print(" echo X- print X\n");
        term_print(" clear - clear screen\n");
        term_print(" about - about PC11\n");
    } else if(streq(cmd,"ver")){
        term_print("PC11 64-bit GUI v0.2\n");
    } else if(streq(cmd,"about")){
        term_print("PC11: a tiny x86-64 UEFI\n");
        term_print("window manager + terminal.\n");
    } else if(streq(cmd,"clear")){
        term_len = 0;          /* wipe the screen buffer */
    } else if(cmd[0]=='e'&&cmd[1]=='c'&&cmd[2]=='h'&&cmd[3]=='o'&&
              (cmd[4]==' '||cmd[4]==0)){
        const char *arg = cmd[4]? cmd+5 : cmd+4;
        term_print(arg);
        term_putc('\n');
    } else {
        term_print("unknown: ");
        term_print(cmd);
        term_putc('\n');
        term_print("type 'help'\n");
    }
}

/* called when Enter is pressed in the terminal */
static void term_enter(void){
    term_line[term_line_len]=0;
    term_putc('\n');           /* finish the typed line on screen */
    term_exec(term_line);
    term_line_len = 0;
    term_prompt();
}
/* called for a printable char typed in the terminal */
static void term_type(char c){
    if(c=='\b'){
        if(term_line_len>0){ term_line_len--; term_putc('\b'); }
        return;
    }
    if(term_line_len < LINE_MAX-1){
        term_line[term_line_len++]=c;
        term_putc(c);
    }
}

/* focused window = the topmost in z-order */
static int focused_win(void){ return zorder[NWIN-1]; }

/* raise window 'idx' to the top of the z-order (focus it) */
static void raise_win(int idx){
    int at=-1;
    for(int i=0;i<NWIN;i++) if(zorder[i]==idx){ at=i; break; }
    if(at<0) return;
    for(int i=at;i<NWIN-1;i++) zorder[i]=zorder[i+1];
    zorder[NWIN-1]=idx;
}

/* ---- title-bar button rectangles ---- */
/* close button (rightmost) */
static void btn_close_rect(int idx,int *bx,int *by,int *bw,int *bh){
    Win *wn=&wins[idx];
    *bw=BTN_W; *bh=TITLE_H-6;
    *bx=wn->x+wn->w-BTN_W-BTN_GAP;
    *by=wn->y+3;
}
/* minimize button (left of close) */
static void btn_min_rect(int idx,int *bx,int *by,int *bw,int *bh){
    Win *wn=&wins[idx];
    *bw=BTN_W; *bh=TITLE_H-6;
    *bx=wn->x+wn->w-2*BTN_W-2*BTN_GAP;
    *by=wn->y+3;
}
static int pt_in(int px_,int py_,int rx,int ry,int rw,int rh){
    return px_>=rx && px_<rx+rw && py_>=ry && py_<ry+rh;
}

/* Power-app buttons: two big buttons in the body. */
static void pwr_shutdown_rect(int idx,int *bx,int *by,int *bw,int *bh){
    Win *wn=&wins[idx];
    *bx=wn->x+20; *by=wn->y+TITLE_H+20; *bw=wn->w-40; *bh=30;
}
static void pwr_restart_rect(int idx,int *bx,int *by,int *bw,int *bh){
    Win *wn=&wins[idx];
    *bx=wn->x+20; *by=wn->y+TITLE_H+60; *bw=wn->w-40; *bh=30;
}

static void draw_window(int idx){
    Win *wn = &wins[idx];
    if(!wn->alive || wn->minimized) return;
    int active = (idx==focused_win());

    /* body with rounded corners */
    fill_round_rect(wn->x, wn->y, wn->w, wn->h, WIN_R, C_WIN);
    /* title bar (top corners rounded to match) */
    fill_top_round(wn->x, wn->y, wn->w, TITLE_H, WIN_R, active?C_TITLE_A:C_TITLE);
    draw_text(wn->x+8, wn->y+6, wn->title, C_TITLET);

    /* ---- title-bar buttons: [ - ] [ x ] ---- */
    int bx,by,bw,bh;
    /* minimize */
    btn_min_rect(idx,&bx,&by,&bw,&bh);
    fill_rect(bx,by,bw,bh, RGB(0xd0,0xb0,0x30));
    draw_char(bx+bw/2-4, by+bh/2-4, '-', RGB(0,0,0));
    /* close */
    btn_close_rect(idx,&bx,&by,&bw,&bh);
    fill_rect(bx,by,bw,bh, RGB(0xc0,0x3a,0x30));
    draw_char(bx+bw/2-4, by+bh/2-4, 'x', RGB(0xff,0xff,0xff));

    /* ---- body content ---- */
    if(wn->kind==WIN_TERMINAL){
        int tx=wn->x+8, ty=wn->y+TITLE_H+6;
        for(int i=0;i<term_len;i++){
            int row=i/TERM_COLS, col=i%TERM_COLS;
            draw_char(tx+col*8, ty+row*9, term_buf[i], C_TEXT);
        }
        /* blinking-ish caret block at the end */
        int row=term_len/TERM_COLS, col=term_len%TERM_COLS;
        if(active) fill_rect(tx+col*8, ty+row*9, 7, 8, RGB(0x30,0x90,0x30));
    } else if(wn->kind==WIN_POWER){
        int bx,by,bw,bh;
        pwr_shutdown_rect(idx,&bx,&by,&bw,&bh);
        fill_rect(bx,by,bw,bh, RGB(0xc0,0x3a,0x30));
        rect_border(bx,by,bw,bh, C_BORDER);
        draw_text(bx+bw/2-4*8, by+bh/2-4, "Shut Down", C_TITLET);
        pwr_restart_rect(idx,&bx,&by,&bw,&bh);
        fill_rect(bx,by,bw,bh, RGB(0x2f,0x7e,0xc4));
        rect_border(bx,by,bw,bh, C_BORDER);
        draw_text(bx+bw/2-3*8, by+bh/2-4, "Restart", C_TITLET);
    } else if(wn->kind==WIN_ABOUT){
        draw_text_ml(wn->x+12, wn->y+TITLE_H+12, ABOUT_TEXT, C_TEXT);
    } else if(wn->kind==WIN_FILES){
        int x0=wn->x, y0=wn->y+TITLE_H;
        /* --- toolbar: [Up][New][Del]  +  path --- */
        fill_rect(x0, y0, wn->w, 18, RGB(0xdd,0xdd,0xdd));
        /* buttons */
        fill_rect(x0+4,  y0+2, 30,14, RGB(0x3a,0x6e,0xa5)); draw_text(x0+8,  y0+5,"Up", C_TITLET);
        fill_rect(x0+38, y0+2, 36,14, RGB(0x2f,0x9e,0x44)); draw_text(x0+42, y0+5,"New",C_TITLET);
        fill_rect(x0+78, y0+2, 36,14, RGB(0xc0,0x3a,0x30)); draw_text(x0+82, y0+5,"Del",C_TITLET);
        /* path / status */
        if(fm_vol<0) draw_text(x0+122, y0+5, "Drives:", C_TEXT);
        else         draw_text(x0+122, y0+5, fm_path, C_TEXT);

        int ly = y0+22;
        int rows = (wn->h - 22 - TITLE_H - 4) / 14;
        if(rows<1) rows=1;
        int listw = wn->w/2 - 8;

        if(fm_vol<0){
            /* ---- drive list ---- */
            for(int i=0;i<fm_nvols;i++){
                int ry=ly+i*14;
                if(i==fm_sel) fill_rect(x0+4, ry-1, listw, 13, RGB(0x2f,0x9e,0x44));
                char line[24]; int p=0;
                const char *pre="Drive "; while(*pre)line[p++]=*pre++;
                line[p++]='0'+i; line[p]=0;
                draw_text(x0+8, ry, line, i==fm_sel?C_TITLET:C_TEXT);
            }
            if(fm_nvols==0) draw_text(x0+8, ly, "No drives found", C_TEXT);
        } else {
            /* ---- directory listing (left) ---- */
            for(int r=0;r<rows;r++){
                int i=fm_scroll+r;
                if(i>=fm_count) break;
                int ry=ly+r*14;
                if(i==fm_sel) fill_rect(x0+4, ry-1, listw, 13, RGB(0x2f,0x9e,0x44));
                UINT32 c = (i==fm_sel)?C_TITLET : (fm_entries[i].is_dir?RGB(0x20,0x40,0x80):C_TEXT);
                char line[MAX_NAME+2]; int p=0;
                if(fm_entries[i].is_dir){ line[p++]='['; }
                for(int k=0; fm_entries[i].name[k] && p<MAX_NAME; k++) line[p++]=fm_entries[i].name[k];
                if(fm_entries[i].is_dir){ line[p++]=']'; }
                line[p]=0;
                draw_text_clip(x0+8, ry, x0+wn->w/2-4, line, c);  /* clip to left pane */
            }
            /* divider + preview (right) */
            for(int yy=ly-2; yy<wn->y+wn->h-4; yy++) px(x0+wn->w/2, yy, RGB(0xb0,0xb0,0xb0));
            int px0=x0+wn->w/2+8;
            int rmax=wn->x+wn->w-6;            /* right edge (inside border) */
            int bmax=wn->y+wn->h-18;           /* bottom (above status line) */
            if(fm_count>0 && !fm_entries[fm_sel].is_dir){
                draw_text_clip(px0, ly, rmax, fm_entries[fm_sel].name, RGB(0x20,0x40,0x80));
                draw_text_box(px0, ly+16, rmax, bmax, fm_preview, C_TEXT);
            } else {
                draw_text(px0, ly, "(folder)", RGB(0x80,0x80,0x80));
            }
        }
        /* status line at the very bottom */
        draw_text(x0+8, wn->y+wn->h-14, fm_status, RGB(0x60,0x60,0x60));
    } else if(wn->kind==WIN_CALC){
        int x0=wn->x, y0=wn->y+TITLE_H;
        /* display */
        fill_rect(x0+6, y0+6, wn->w-12, 26, RGB(0x10,0x18,0x10));
        char num[24]; long v=calc_display(); int neg=v<0; if(neg)v=-v;
        int p=0; char tmp[24]; if(v==0)tmp[p++]='0'; while(v){tmp[p++]='0'+(v%10);v/=10;}
        int q=0; if(neg)num[q++]='-'; while(p>0)num[q++]=tmp[--p]; num[q]=0;
        if(calc_err){ const char*e="ERR"; q=0; while(*e)num[q++]=*e++; num[q]=0; }
        draw_text(x0+wn->w-12-(int)q*8-4, y0+12, num, RGB(0x40,0xff,0x40));
        /* buttons grid */
        int gx=x0+6, gy=y0+38, bw=(wn->w-12-3*4)/4, bh=28;
        for(int r=0;r<4;r++) for(int c=0;c<4;c++){
            char k=calc_keys[r][c]; if(!k) continue;
            int bxp=gx+c*(bw+4), byp=gy+r*(bh+4);
            UINT32 col = (k=='='||k=='C') ? RGB(0x2f,0x9e,0x44)
                       : ((k>='0'&&k<='9')?RGB(0x40,0x48,0x58):RGB(0x3a,0x6e,0xa5));
            fill_round_rect(bxp,byp,bw,bh,4,col);
            char s[2]={k,0}; draw_text(bxp+bw/2-4, byp+bh/2-4, s, C_TITLET);
        }
    } else if(wn->kind==WIN_EDIT){
        int x0=wn->x, y0=wn->y+TITLE_H;
        /* toolbar: Open + Save + filename */
        fill_rect(x0,y0,wn->w,18, RGB(0xdd,0xdd,0xdd));
        fill_round_rect(x0+4, y0+2,44,14,3,RGB(0x3a,0x6e,0xa5)); draw_text(x0+8, y0+5,"Open",C_TITLET);
        fill_round_rect(x0+52,y0+2,44,14,3,RGB(0x2f,0x9e,0x44)); draw_text(x0+56,y0+5,"Save",C_TITLET);
        draw_text_clip(x0+102,y0+5,x0+wn->w-4, edit_name, C_TEXT);
        /* text area */
        int tx=x0+6, ty=y0+24;
        int rmax=wn->x+wn->w-6, bmax=wn->y+wn->h-16;
        char tmp2[EDIT_MAX+1];
        int n=edit_len<EDIT_MAX?edit_len:EDIT_MAX;
        for(int i=0;i<n;i++) tmp2[i]=edit_buf[i];
        tmp2[n]=0;
        draw_text_box(tx,ty,rmax,bmax,tmp2,C_TEXT);
        /* caret */
        if(active && !edit_open_mode){
            int cx=tx, cy=ty;
            for(int i=0;i<n;i++){ if(edit_buf[i]=='\n'){cx=tx;cy+=12;} else {cx+=8; if(cx+8>rmax){cx=tx;cy+=12;}} }
            if(cy<=bmax) fill_rect(cx,cy,2,10,RGB(0x30,0x90,0x30));
        }
        /* status */
        draw_text(x0+6, wn->y+wn->h-12, edit_status, RGB(0x60,0x60,0x60));
        /* file-picker overlay */
        if(edit_open_mode){
            int ox=x0+20, oy=y0+24, ow=wn->w-40, oh=wn->h-TITLE_H-30;
            fill_round_rect(ox,oy,ow,oh,6, RGB(0xf4,0xf4,0xf4));
            round_border(ox,oy,ow,oh,6, C_BORDER);
            draw_text(ox+8, oy+6, "Pick a file:", RGB(0x20,0x40,0x80));
            if(fm_vol<0){
                draw_text(ox+8, oy+24, "Open a drive in Files first.", C_TEXT);
            } else {
                int rows=(oh-26)/14;
                for(int r=0;r<rows;r++){
                    int i=r; if(i>=fm_count) break;
                    int ry=oy+24+r*14;
                    if(fm_entries[i].is_dir) continue;        /* files only */
                    draw_text_clip(ox+10, ry, ox+ow-6, fm_entries[i].name, C_TEXT);
                }
            }
        }
    } else if(wn->kind==WIN_CLOCK){
        EFI_TIME t;
        char big[16]; int p=0;
        if(RT->GetTime(&t,NULL)==EFI_SUCCESS){
            big[p++]='0'+t.Hour/10; big[p++]='0'+t.Hour%10; big[p++]=':';
            big[p++]='0'+t.Minute/10; big[p++]='0'+t.Minute%10; big[p++]=':';
            big[p++]='0'+t.Second/10; big[p++]='0'+t.Second%10; big[p]=0;
            /* draw 2x-scaled by drawing each char block twice... simple: just text */
            draw_text(wn->x+wn->w/2-32, wn->y+TITLE_H+30, big, C_TEXT);
            char dt[16]; int d=0;
            dt[d++]='0'+(t.Day/10); dt[d++]='0'+(t.Day%10); dt[d++]='/';
            dt[d++]='0'+(t.Month/10); dt[d++]='0'+(t.Month%10); dt[d]=0;
            draw_text(wn->x+wn->w/2-20, wn->y+TITLE_H+50, dt, RGB(0x60,0x60,0x60));
        } else {
            draw_text(wn->x+12, wn->y+TITLE_H+20, "no RTC", C_TEXT);
        }
    } else if(wn->body){
        draw_text(wn->x+10, wn->y+TITLE_H+14, wn->body, C_TEXT);
    }

    /* resize grip (bottom-right): a few diagonal lines */
    {
        int gx=wn->x+wn->w-GRIP, gy=wn->y+wn->h-GRIP;
        for(int d=3; d<GRIP; d+=3)
            for(int k=0;k<=d;k++)
                px(gx+GRIP-1-k, gy+GRIP-1-(d-k), RGB(0x80,0x80,0x80));
    }

    /* border */
    round_border(wn->x, wn->y, wn->w, wn->h, WIN_R, C_BORDER);
}

/* ---- Start ("PC11") button + pop-up app menu (taskbar at BOTTOM) ---- */
static int menu_open = 0;          /* is the start menu showing? */
#define TASKBAR_H 32
#define START_X 1
#define START_W 30
#define START_H 30
#define MENU_W  160
#define MENU_ITEM_H 22
#define TBY() ((int)SH - TASKBAR_H)            /* taskbar top y */
#define STARTY() (TBY() + (TASKBAR_H-START_H)/2)
/* menu pops UP from just above the taskbar */
#define MENU_X 1
#define MENU_GAP 8
#define MENU_H() (NWIN*MENU_ITEM_H + 4 + MENU_GAP)
#define MENU_Y() (TBY() - MENU_H())

static int start_hit(int x,int y){ /* clicked the PC11 button? */
    return x>=START_X && x<START_X+START_W && y>=STARTY() && y<STARTY()+START_H;
}
/* Menu order: all apps in array order EXCEPT Power Off, then Power Off pinned
 * last. menu_win(slot) -> window index for that visual slot. */
static int menu_win(int slot){
    int s=0;
    for(int i=0;i<NWIN;i++){               /* apps first (skip Power Off) */
        if(wins[i].kind==WIN_POWER) continue;
        if(s==slot) return i;
        s++;
    }
    if(slot==NWIN-1){                       /* last slot = Power Off */
        for(int i=0;i<NWIN;i++) if(wins[i].kind==WIN_POWER) return i;
    }
    return -1;
}
static int menu_item_at(int x,int y){ /* -> window index, or -1 */
    if(!menu_open) return -1;
    if(x<MENU_X || x>=MENU_X+MENU_W) return -1;
    int rel = y - (MENU_Y()+2);
    if(rel<0) return -1;
    int apps = NWIN-1;                      /* app slots before the gap */
    int appsH = apps*MENU_ITEM_H;
    if(rel < appsH){                        /* an app row */
        int slot = rel / MENU_ITEM_H;
        return menu_win(slot);
    }
    /* the pinned Power Off row (after the gap) */
    if(rel >= appsH + MENU_GAP && rel < appsH + MENU_GAP + MENU_ITEM_H)
        return menu_win(NWIN-1);
    return -1;
}
static void draw_menu(void){
    if(!menu_open) return;
    int my=MENU_Y(), h=MENU_H();
    fill_round_rect(MENU_X, my, MENU_W, h, 6, RGB(0x22,0x2c,0x3c));
    round_border(MENU_X, my, MENU_W, h, 6, C_BORDER);
    int apps = NWIN-1;
    /* app rows */
    for(int s=0;s<apps;s++){
        int w=menu_win(s); if(w<0) continue;
        int iy = my + 2 + s*MENU_ITEM_H;
        draw_text(MENU_X+12, iy+6, wins[w].title, C_TITLET);
    }
    /* separator + pinned Power Off at the bottom */
    int sepy = my + 2 + apps*MENU_ITEM_H + MENU_GAP/2;
    for(int i=8;i<MENU_W-8;i++) px(MENU_X+i, sepy, RGB(0x44,0x4c,0x5c));
    int pw=menu_win(NWIN-1);
    if(pw>=0){
        int iy = my + 2 + apps*MENU_ITEM_H + MENU_GAP;
        draw_text(MENU_X+12, iy+6, wins[pw].title, RGB(0xff,0x90,0x90));
    }
}

/* ---- right-click context menu ----
 * Items depend on context:
 *   on a file:  Copy, Set as Background, Paste
 *   elsewhere:  Paste
 */
#define CTX_W 150
#define CTX_ITEM_H 20
static const char* ctx_items[6];   /* labels for the current menu */
static int ctx_n = 0;
static void ctx_build(void){
    ctx_n=0;
    if(ctx_on_file){
        ctx_items[ctx_n++]="Copy";
        ctx_items[ctx_n++]="Edit";
        /* only offer "Set as BG" for image files */
        if(is_image_name(ctx_target)) ctx_items[ctx_n++]="Set as BG";
    }
    ctx_items[ctx_n++]="Paste";
}
static void draw_ctx_menu(void){
    if(!ctx_open) return;
    int h = ctx_n*CTX_ITEM_H + 4;
    fill_rect(ctx_x, ctx_y, CTX_W, h, RGB(0xf0,0xf0,0xf0));
    rect_border(ctx_x, ctx_y, CTX_W, h, C_BORDER);
    for(int i=0;i<ctx_n;i++){
        int iy=ctx_y+2+i*CTX_ITEM_H;
        draw_text(ctx_x+10, iy+6, ctx_items[i], C_TEXT);
    }
}
/* which context item is at (x,y)? -1 = none */
static int ctx_item_at(int x,int y){
    if(!ctx_open) return -1;
    if(x<ctx_x || x>=ctx_x+CTX_W) return -1;
    int rel=y-(ctx_y+2);
    if(rel<0) return -1;
    int it=rel/CTX_ITEM_H;
    return (it>=0 && it<ctx_n)? it : -1;
}

/* ---- taskbar at top showing minimized/closed windows ---- */
static void draw_clock(void){
    EFI_TIME t;
    if(RT->GetTime(&t,NULL)!=EFI_SUCCESS) return;
    char buf[6];
    buf[0]='0'+(t.Hour/10);  buf[1]='0'+(t.Hour%10);
    buf[2]=':';
    buf[3]='0'+(t.Minute/10);buf[4]='0'+(t.Minute%10);
    buf[5]=0;
    int cx=(int)SW-46, cy=TBY()+(TASKBAR_H-8)/2;
    draw_text(cx,cy,buf,C_TITLET);
}
static void draw_taskbar(void){
    int ty=TBY();
    fill_rect(0,ty,(int)SW,TASKBAR_H, RGB(0x12,0x18,0x26));
    /* the Start button */
    fill_round_rect(START_X,STARTY(),START_W,START_H,4,
                    menu_open?RGB(0x2f,0x9e,0x44):RGB(0x3a,0x6e,0xa5));
    /* the PC11 icon, centered in the start button */
    draw_icon(START_X+(START_W-ICON_W)/2, STARTY()+(START_H-ICON_H)/2);
    /* open-window buttons */
    int tx=START_X+START_W+8;
    int by=ty+(TASKBAR_H-18)/2;
    for(int i=0;i<NWIN;i++){
        if(!wins[i].alive) continue;
        UINT32 col = wins[i].minimized ? RGB(0x33,0x44,0x55)
                   : (i==focused_win()? RGB(0x2f,0x9e,0x44):RGB(0x3a,0x6e,0xa5));
        fill_round_rect(tx,by,110,18,4,col);
        draw_text_clip(tx+6,by+5,tx+106,wins[i].title,C_TITLET);
        tx += 120;
    }
    draw_clock();
}

/* mouse cursor */
static int mx, my;
static void draw_cursor(void){
    for(int j=0;j<12;j++){
        for(int i=0;i<=j && i<8;i++){
            px(mx+i, my+j, (i==0||i==j||j==11)?C_CUR_OUT:C_CURSOR);
        }
    }
}

static void draw_ctx_menu(void);   /* fwd */

/* Build the scaled desktop into DESK. This is the expensive per-pixel
 * background scale; we only do it when the wallpaper or resolution changes
 * (desk_dirty), NOT every frame. Without this, just moving the mouse forced
 * a full 1920x1080 rescale every frame -> the "heavy" feeling. */
static void build_desktop(void){
    if(!DESK) return;
    if(bg_img && bg_w>0 && bg_h>0){
        for(UINT32 y=0;y<SH;y++){
            int sy = (int)((UINT64)y*bg_h/SH);
            if(sy<0) sy=0;
            if(sy>=bg_h) sy=bg_h-1;                  /* clamp - never OOB */
            UINT32 *row = &DESK[(UINT64)y*SW];
            const UINT32 *srow = &bg_img[(UINT64)sy*bg_w];
            for(UINT32 x=0;x<SW;x++){
                int sx = (int)((UINT64)x*bg_w/SW);
                if(sx<0) sx=0;
                if(sx>=bg_w) sx=bg_w-1;
                row[x] = srow[sx];
            }
        }
    } else {
        for(UINT32 i=0;i<SW*SH;i++) DESK[i]=C_DESKTOP;
    }
    desk_dirty = 0;
}

static void render(void){
    /* desktop: copy the pre-scaled cache (rebuild only when it changed) */
    if(desk_dirty) build_desktop();
    if(DESK) memcpy(BACK, DESK, (UINTN)SW*SH*4);
    else for(UINT32 i=0;i<SW*SH;i++) BACK[i]=C_DESKTOP;

    draw_taskbar();
    /* draw windows bottom-to-top in z-order */
    for(int i=0;i<NWIN;i++) draw_window(zorder[i]);
    draw_menu();          /* start menu floats above windows */
    draw_ctx_menu();      /* right-click context menu floats on top */
    draw_cursor();
    /* blit BACK -> FB (row-at-a-time bulk copy is far faster than scalar) */
    for(UINT32 y=0;y<SH;y++)
        memcpy(&FB[(UINTN)y*PPSL], &BACK[(UINTN)y*SW], (UINTN)SW*4);
}

/* ===================== helpers ===================== */
static EFI_STATUS get_gop(void){
    EFI_GUID g = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    return BS->LocateProtocol(&g, NULL, (VOID**)&GOP);
}

/* Switch the framebuffer to wantW x wantH if the firmware offers it.
 * Falls back silently to the current mode when that resolution is absent. */
static void set_video_mode(UINT32 wantW, UINT32 wantH){
    if(!GOP || !GOP->Mode) return;
    UINT32 best = GOP->Mode->Mode;   /* default: keep current mode */
    UINTN  sz = 0;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
    for(UINT32 m = 0; m < GOP->Mode->MaxMode; m++){
        if(GOP->QueryMode(GOP, m, &sz, &info) != EFI_SUCCESS || !info) continue;
        if(info->HorizontalResolution == wantW &&
           info->VerticalResolution   == wantH){
            best = m;
            break;
        }
    }
    if(best != GOP->Mode->Mode)
        GOP->SetMode(GOP, best);
}

/* ===================== live file manager (UEFI FS) ===================== */

/* ascii -> CHAR16 (for Open) */
static void to_wide(const char *s, CHAR16 *out, int max){
    int i=0; for(; s[i] && i<max-1; i++) out[i]=(CHAR16)(unsigned char)s[i]; out[i]=0;
}
/* CHAR16 -> ascii */
static void to_ascii(const CHAR16 *s, char *out, int max){
    int i=0; for(; s[i] && i<max-1; i++) out[i]=(char)s[i]; out[i]=0;
}
static int sstreq2(const char *a,const char *b){
    int i=0; for(;a[i]&&b[i];i++) if(a[i]!=b[i]) return 0; return a[i]==b[i];
}

/* Enumerate every volume that exposes a filesystem. */
static void fm_enum_volumes(void){
    EFI_GUID fsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_HANDLE *handles=NULL; UINTN n=0;
    fm_nvols=0;
    if(BS->LocateHandleBuffer(2 /*ByProtocol*/, &fsGuid, NULL, &n, &handles)!=EFI_SUCCESS) return;
    for(UINTN i=0;i<n && fm_nvols<MAX_VOLS;i++){
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs=NULL;
        if(BS->HandleProtocol(handles[i], &fsGuid, (VOID**)&fs)!=EFI_SUCCESS || !fs) continue;
        EFI_FILE_PROTOCOL *root=NULL;
        if(fs->OpenVolume(fs, &root)==EFI_SUCCESS && root){
            fm_roots[fm_nvols++] = root;
        }
    }
    if(handles) BS->FreePool(handles);
}

/* Read the currently-open directory (fm_dir) into fm_entries. */
static void fm_read_dir(void){
    fm_count=0; fm_sel=0; fm_scroll=0;
    fm_preview_len=0; fm_preview[0]=0;
    if(!fm_dir) return;
    fm_dir->SetPosition(fm_dir, 0);   /* rewind */
    for(;;){
        UINT8 infobuf[ sizeof(EFI_FILE_INFO)+ (256*2) ];
        UINTN isize=sizeof(infobuf);
        EFI_STATUS s=fm_dir->Read(fm_dir,&isize,infobuf);
        if(s!=EFI_SUCCESS || isize==0) break;
        EFI_FILE_INFO *info=(EFI_FILE_INFO*)infobuf;
        char nm[MAX_NAME]; to_ascii(info->FileName, nm, MAX_NAME);
        if(sstreq2(nm,".")) continue;            /* skip self */
        if(fm_count>=MAX_ENTRIES) break;
        DirEntry *e=&fm_entries[fm_count++];
        to_ascii(info->FileName, e->name, MAX_NAME);
        e->is_dir = (info->Attribute & EFI_FILE_DIRECTORY)?1:0;
        e->size   = info->FileSize;
    }
}

/* Open a file and load up to MAX_PREVIEW bytes into fm_preview. */
static void fm_preview_file(const char *name){
    fm_preview_len=0; fm_preview[0]=0;
    if(!fm_dir) return;
    CHAR16 wn[MAX_NAME]; to_wide(name, wn, MAX_NAME);
    EFI_FILE_PROTOCOL *fh=NULL;
    if(fm_dir->Open(fm_dir,&fh,wn,EFI_FILE_MODE_READ,0)!=EFI_SUCCESS || !fh) return;
    UINTN got=MAX_PREVIEW-1;
    if(fh->Read(fh,&got,fm_preview)==EFI_SUCCESS){
        if(got>(UINTN)(MAX_PREVIEW-1)) got=MAX_PREVIEW-1;
        fm_preview[got]=0; fm_preview_len=(int)got;
    }
    fh->Close(fh);
}

/* ---- pool allocator wrappers for the PNG decoder ---- */
static void* pool_alloc(unsigned long sz){
    VOID *p=NULL;
    if(BS->AllocatePool(2 /*EfiLoaderData*/, (UINTN)sz, &p)!=EFI_SUCCESS) return NULL;
    return p;
}
static void pool_free(void *p){ if(p) BS->FreePool(p); }

/* case-insensitive suffix check, e.g. ends_with(name, ".png") */
static int ends_with(const char *s, const char *suf){
    int ls=0; while(s[ls]) ls++;
    int lf=0; while(suf[lf]) lf++;
    if(lf>ls) return 0;
    for(int i=0;i<lf;i++){
        char a=s[ls-lf+i], b=suf[i];
        if(a>='A'&&a<='Z') a+=32;
        if(b>='A'&&b<='Z') b+=32;
        if(a!=b) return 0;
    }
    return 1;
}

/* Read an entire file from a directory into a freshly-allocated buffer. */
static unsigned char* read_whole_file(EFI_FILE_PROTOCOL *dir, const char *name,
                                      unsigned long *out_len){
    *out_len=0;
    if(!dir) return NULL;
    CHAR16 wn[MAX_NAME]; to_wide(name, wn, MAX_NAME);
    EFI_FILE_PROTOCOL *fh=NULL;
    if(dir->Open(dir,&fh,wn,EFI_FILE_MODE_READ,0)!=EFI_SUCCESS || !fh) return NULL;
    /* get size via EFI_FILE_INFO */
    EFI_GUID fiGuid = EFI_FILE_INFO_GUID;
    UINT8 info[sizeof(EFI_FILE_INFO)+(256*2)]; UINTN isz=sizeof(info);
    UINT64 fsize=0;
    if(fh->GetInfo(fh,&fiGuid,&isz,info)==EFI_SUCCESS){
        fsize=((EFI_FILE_INFO*)info)->FileSize;
    }
    if(fsize==0 || fsize>(8UL*1024*1024)){ fh->Close(fh); return NULL; }  /* cap 8MB */
    unsigned char *buf=(unsigned char*)pool_alloc((unsigned long)fsize);
    if(!buf){ fh->Close(fh); return NULL; }
    UINTN got=(UINTN)fsize;
    if(fh->Read(fh,&got,buf)!=EFI_SUCCESS){ pool_free(buf); fh->Close(fh); return NULL; }
    fh->Close(fh);
    *out_len=(unsigned long)got;
    return buf;
}

/* Decode a simple 24/32-bit uncompressed BMP into RGBA px (top-down). */
static int decode_bmp(const unsigned char *d, unsigned long n,
                      unsigned int **out_px, int *out_w, int *out_h){
    if(n<54 || d[0]!='B' || d[1]!='M') return 0;
    unsigned int off = d[10]|(d[11]<<8)|(d[12]<<16)|((unsigned)d[13]<<24);
    int w = d[18]|(d[19]<<8)|(d[20]<<16)|((unsigned)d[21]<<24);
    int h = d[22]|(d[23]<<8)|(d[24]<<16)|((unsigned)d[25]<<24);
    int bpp = d[28]|(d[29]<<8);
    if(w<=0||h==0||(bpp!=24&&bpp!=32)) return 0;
    int flip = h>0; if(h<0) h=-h;
    int bytespp=bpp/8;
    int row = ((w*bytespp+3)/4)*4;
    unsigned int *px=(unsigned int*)pool_alloc((unsigned long)w*h*4);
    if(!px) return 0;
    for(int y=0;y<h;y++){
        int sy = flip ? (h-1-y) : y;
        const unsigned char *p = d + off + (unsigned long)sy*row;
        if((unsigned long)(p-d)+ (unsigned long)w*bytespp > n) break;
        for(int x=0;x<w;x++){
            int b=p[x*bytespp], g=p[x*bytespp+1], r=p[x*bytespp+2];
            px[y*w+x]=((unsigned)r<<16)|((unsigned)g<<8)|(unsigned)b;
        }
    }
    *out_px=px; *out_w=w; *out_h=h; return 1;
}

/* Set the desktop background from an image file in the current dir. */
static void set_background(const char *name){
    unsigned long len=0;
    unsigned char *data = read_whole_file(fm_dir, name, &len);
    if(!data){ to_ascii(L"can't read image", fm_status, 80); return; }
    unsigned int *px=NULL; int w=0,h=0; int ok=0;
    if(ends_with(name,".png")) ok=decode_png(data,len,pool_alloc,pool_free,&px,&w,&h);
    else if(ends_with(name,".bmp")) ok=decode_bmp(data,len,&px,&w,&h);
    else if(ends_with(name,".jpg")||ends_with(name,".jpeg"))
        ok=decode_jpg(data,len,pool_alloc,pool_free,&px,&w,&h);
    else { to_ascii(L"not an image", fm_status, 80); pool_free(data); return; }
    pool_free(data);
    if(!ok || !px){ to_ascii(L"decode failed", fm_status, 80); return; }
    /* sanity-check the decoded dimensions: a corrupt/huge w or h would make
     * render() read far out of bounds and fault. Reject anything absurd. */
    if(w<=0 || h<=0 || w>8192 || h>8192){
        pool_free(px);
        to_ascii(L"image too large / invalid", fm_status, 80);
        return;
    }
    /* swap in the new background (free the old one first) */
    UINT32 *old = bg_img;
    bg_img = px; bg_w = w; bg_h = h;
    desk_dirty = 1;                  /* rebuild the cached desktop next frame */
    if(old) pool_free(old);          /* free AFTER swap so render never sees a freed ptr */
    to_ascii(L"background set", fm_status, 80);
}

/* Load /home/logo.png from the boot volume at startup and decode it into
 * logo_px (full resolution). draw_icon() scales it down smoothly. */
static void load_logo(EFI_HANDLE ImageHandle){
    EFI_GUID liGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID fsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *li=NULL;
    if(BS->HandleProtocol(ImageHandle,&liGuid,(VOID**)&li)!=EFI_SUCCESS||!li) return;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs=NULL;
    if(BS->HandleProtocol(li->DeviceHandle,&fsGuid,(VOID**)&fs)!=EFI_SUCCESS||!fs) return;
    EFI_FILE_PROTOCOL *root=NULL;
    if(fs->OpenVolume(fs,&root)!=EFI_SUCCESS||!root) return;
    EFI_FILE_PROTOCOL *dir=NULL;
    CHAR16 hn[]=L"PC11";
    if(root->Open(root,&dir,hn,EFI_FILE_MODE_READ,0)==EFI_SUCCESS && dir){
        unsigned long len=0;
        unsigned char *data=read_whole_file(dir, "logo.png", &len);
        if(data){
            unsigned int *px=NULL; int w=0,h=0;
            if(decode_png(data,len,pool_alloc,pool_free,&px,&w,&h) && px
               && w>0 && h>0 && w<=2048 && h<=2048){
                logo_px=px; logo_w=w; logo_h=h;
            } else if(px) pool_free(px);
            pool_free(data);
        }
        dir->Close(dir);
    }
    root->Close(root);
}

/* copy: remember the current dir + selected file name */
static void clip_copy(const char *name){
    /* read the file's bytes NOW and stash them, so paste works even after
     * we navigate to a different folder (which closes the source dir). */
    unsigned long len=0;
    unsigned char *data = read_whole_file(fm_dir, name, &len);
    if(!data){ to_ascii(L"copy failed", fm_status, 80); return; }
    if(clip_data) pool_free(clip_data);
    clip_data = data; clip_len = len;
    int i=0; for(; name[i] && i<MAX_NAME-1; i++) clip_name[i]=name[i];
    clip_name[i]=0;
    to_ascii(L"copied", fm_status, 80);
}

/* paste: read the copied file's bytes and write them into the current dir.
 * If a file with the same name exists here, prefix with 'copy_'. */
static int name_exists(const char *name){
    for(int i=0;i<fm_count;i++) if(sstreq2(fm_entries[i].name, name)) return 1;
    return 0;
}
static void clip_paste(void){
    if(clip_name[0]==0 || !clip_data){ to_ascii(L"nothing copied", fm_status, 80); return; }
    if(fm_vol<0 || !fm_dir){ to_ascii(L"open a folder first", fm_status, 80); return; }

    /* choose a destination name; if it already exists here, prefix copy_ */
    char dst[MAX_NAME]; int p=0;
    if(name_exists(clip_name)){ const char *pre="copy_"; while(*pre && p<MAX_NAME-1) dst[p++]=*pre++; }
    for(int i=0; clip_name[i] && p<MAX_NAME-1; i++) dst[p++]=clip_name[i];
    dst[p]=0;

    CHAR16 wn[MAX_NAME]; to_wide(dst, wn, MAX_NAME);
    EFI_FILE_PROTOCOL *fh=NULL;
    if(fm_dir->Open(fm_dir,&fh,wn, 0x8000000000000003ULL, 0)==EFI_SUCCESS && fh){
        UINTN w=(UINTN)clip_len;
        EFI_STATUS s = fh->Write(fh,&w,clip_data);
        fh->Close(fh);
        if(s==EFI_SUCCESS){ to_ascii(L"pasted", fm_status, 80); fm_read_dir(); }
        else to_ascii(L"paste: write failed", fm_status, 80);
    } else {
        to_ascii(L"paste failed (read-only?)", fm_status, 80);
    }
}

/* Enter a volume's root (vol index). */
static void fm_enter_volume(int vol){
    if(vol<0||vol>=fm_nvols) return;
    if(fm_vol>=0 && fm_dir && fm_dir!=fm_roots[fm_vol]) fm_dir->Close(fm_dir);
    fm_vol=vol;
    fm_dir=fm_roots[vol];
    fm_path[0]='/'; fm_path[1]=0;
    to_ascii(L"", fm_status, 80);
    fm_read_dir();
}

/* Open the selected entry: dir -> descend; file -> preview. */
static void fm_open_selected(void){
    if(fm_vol<0) { fm_enter_volume(fm_sel); return; }   /* drive list */
    if(fm_count==0) return;
    DirEntry *e=&fm_entries[fm_sel];
    if(e->is_dir){
        CHAR16 wn[MAX_NAME]; to_wide(e->name, wn, MAX_NAME);
        EFI_FILE_PROTOCOL *nd=NULL;
        if(fm_dir->Open(fm_dir,&nd,wn,EFI_FILE_MODE_READ,0)==EFI_SUCCESS && nd){
            if(fm_dir!=fm_roots[fm_vol]) fm_dir->Close(fm_dir);
            fm_dir=nd;
            /* update display path */
            if(sstreq2(e->name,"..")){
                /* trim last path component */
                int L=0; while(fm_path[L]) L++;
                if(L>1){ L--; if(fm_path[L]=='/') L--; while(L>0 && fm_path[L]!='/') L--; fm_path[L+1]=0; if(L==0) fm_path[1]=0; }
            } else {
                int L=0; while(fm_path[L]) L++;
                if(L==0||fm_path[L-1]!='/'){ fm_path[L++]='/'; }
                for(int k=0;e->name[k]&&L<250;k++) fm_path[L++]=e->name[k];
                fm_path[L]=0;
            }
            fm_read_dir();
        }
    } else {
        fm_preview_file(e->name);
    }
}

/* Create a new empty file "newfile.txt" in the current dir. */
static void fm_create_file(void){
    if(fm_vol<0 || !fm_dir){ to_ascii(L"open a drive first", fm_status, 80); return; }
    CHAR16 wn[]=L"newfile.txt";
    EFI_FILE_PROTOCOL *fh=NULL;
    /* OpenMode = read|write|create (0x3 | 0x8000000000000000) */
    if(fm_dir->Open(fm_dir,&fh,wn, 0x8000000000000003ULL, 0)==EFI_SUCCESS && fh){
        const char *txt="new file\n"; UINTN w=9;
        fh->Write(fh,&w,(VOID*)txt);
        fh->Close(fh);
        to_ascii(L"created newfile.txt", fm_status, 80);
        fm_read_dir();
    } else {
        to_ascii(L"create failed (read-only?)", fm_status, 80);
    }
}

/* ---- Text editor: load the Files-selected file, and save back ---- */
static void edit_load_selected(void){
    if(fm_vol<0 || fm_count==0 || fm_entries[fm_sel].is_dir){
        /* nothing selected: fresh scratch buffer */
        edit_len=0; edit_buf[0]=0; edit_loaded=0;
        return;
    }
    unsigned long len=0;
    unsigned char *data=read_whole_file(fm_dir, fm_entries[fm_sel].name, &len);
    if(!data){ to_ascii(L"could not open file", edit_status, 48); return; }
    if(len>EDIT_MAX-1) len=EDIT_MAX-1;
    for(unsigned long i=0;i<len;i++) edit_buf[i]=data[i];
    edit_len=(int)len; edit_buf[edit_len]=0;
    pool_free(data);
    int k=0; for(;fm_entries[fm_sel].name[k]&&k<63;k++) edit_name[k]=fm_entries[fm_sel].name[k];
    edit_name[k]=0; edit_loaded=1;
    to_ascii(L"loaded", edit_status, 48);
}
static void edit_save(void){
    if(fm_vol<0 || !fm_dir){ to_ascii(L"open a folder in Files first", edit_status, 48); return; }
    CHAR16 wn[64]; to_wide(edit_name, wn, 64);
    EFI_FILE_PROTOCOL *fh=NULL;
    if(fm_dir->Open(fm_dir,&fh,wn, 0x8000000000000003ULL, 0)==EFI_SUCCESS && fh){
        UINTN w=(UINTN)edit_len;
        EFI_STATUS s=fh->Write(fh,&w,edit_buf);
        fh->Close(fh);
        if(s==EFI_SUCCESS){ to_ascii(L"saved!", edit_status, 48); fm_read_dir(); }
        else to_ascii(L"save: write failed", edit_status, 48);
    } else {
        to_ascii(L"save failed (read-only?)", edit_status, 48);
    }
}

/* Delete the selected file (not dirs). */
static void fm_delete_selected(void){
    if(fm_vol<0 || fm_count==0) return;
    DirEntry *e=&fm_entries[fm_sel];
    if(e->is_dir || sstreq2(e->name,"..")){ to_ascii(L"select a file to delete", fm_status, 80); return; }
    CHAR16 wn[MAX_NAME]; to_wide(e->name, wn, MAX_NAME);
    EFI_FILE_PROTOCOL *fh=NULL;
    if(fm_dir->Open(fm_dir,&fh,wn, 0x8000000000000003ULL, 0)==EFI_SUCCESS && fh){
        /* Delete is a slot in the protocol; call via offset (Delete) */
        EFI_STATUS (EFIAPI *del)(EFI_FILE_PROTOCOL*) =
            (EFI_STATUS (EFIAPI *)(EFI_FILE_PROTOCOL*))fh->Delete;
        if(del) del(fh);
        to_ascii(L"deleted", fm_status, 80);
        fm_read_dir();
    } else {
        to_ascii(L"delete failed", fm_status, 80);
    }
}

/* Initialise the file manager (called once, boot services alive). */
static void fm_init(void){
    fm_enum_volumes();
    fm_vol=-1;            /* start at the drive list */
    fm_dir=NULL;
    fm_count=0; fm_sel=0;
    if(fm_nvols==1) fm_enter_volume(0);   /* one drive: jump right in */
    else to_ascii(L"select a drive", fm_status, 80);
}

static int in_titlebar(int idx,int x,int y){
    Win *wn=&wins[idx];
    return x>=wn->x && x<wn->x+wn->w && y>=wn->y && y<wn->y+TITLE_H;
}
static int in_window(int idx,int x,int y){
    Win *wn=&wins[idx];
    return wn->alive && !wn->minimized &&
           x>=wn->x && x<wn->x+wn->w && y>=wn->y && y<wn->y+wn->h;
}
/* resize grip: a GRIP x GRIP square at the bottom-right corner of a window */
static int in_resize_grip(int idx,int x,int y){
    Win *wn=&wins[idx];
    int gx=wn->x+wn->w-GRIP, gy=wn->y+wn->h-GRIP;
    return x>=gx && x<gx+GRIP && y>=gy && y<gy+GRIP;
}
/* file-manager hit tests (return codes): -1 none, -2 Up, -3 New, -4 Del,
 * >=0 = list row index (already adjusted for scroll). */
#define FM_HIT_NONE -1
#define FM_HIT_UP   -2
#define FM_HIT_NEW  -3
#define FM_HIT_DEL  -4
static int fm_hit(int idx,int x,int y){
    Win *wn=&wins[idx];
    if(wn->kind!=WIN_FILES) return FM_HIT_NONE;
    int x0=wn->x, y0=wn->y+TITLE_H;
    /* toolbar buttons */
    if(y>=y0+2 && y<y0+16){
        if(x>=x0+4  && x<x0+34)  return FM_HIT_UP;
        if(x>=x0+38 && x<x0+74)  return FM_HIT_NEW;
        if(x>=x0+78 && x<x0+114) return FM_HIT_DEL;
    }
    /* list rows */
    int ly=y0+22;
    int rows=(wn->h - 22 - TITLE_H - 4)/14; if(rows<1)rows=1;
    int listw=wn->w/2-8;
    if(x>=x0+4 && x<x0+4+listw){
        for(int r=0;r<rows;r++){
            int ry=ly+r*14;
            if(y>=ry-1 && y<ry+13) return fm_scroll+r;
        }
    }
    return FM_HIT_NONE;
}
/* taskbar entry hit-test: returns window index or -1 */
static int taskbar_hit(int x,int y){
    int by=TBY()+(TASKBAR_H-18)/2;
    if(y<by || y>=by+18) return -1;
    int tx=START_X+START_W+8;
    for(int i=0;i<NWIN;i++){
        if(!wins[i].alive) continue;
        if(x>=tx && x<tx+110) return i;
        tx += 120;
    }
    return -1;
}

/* ===================== PS/2 hardware I/O =====================
 * After ExitBootServices we own the machine and talk to the PS/2
 * controller directly. This is rock-solid under QEMU and real HW,
 * unlike the UEFI pointer protocol (which OVMF leaves NOT_READY).
 */
static inline UINT8 inb(UINT16 port){
    UINT8 v; __asm__ __volatile__("inb %1,%0":"=a"(v):"Nd"(port)); return v;
}
static inline void outb(UINT16 port, UINT8 v){
    __asm__ __volatile__("outb %0,%1"::"a"(v),"Nd"(port));
}

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

static void ps2_wait_in(void){     /* wait until input buffer empty (can write) */
    for(int i=0;i<100000;i++) if(!(inb(PS2_STATUS)&0x02)) return;
}
static void ps2_wait_out(void){    /* wait until output buffer full (can read) */
    for(int i=0;i<100000;i++) if(inb(PS2_STATUS)&0x01) return;
}
static void mouse_write(UINT8 v){  /* send a byte to the mouse (aux device) */
    ps2_wait_in(); outb(PS2_CMD, 0xD4);     /* next byte goes to aux port */
    ps2_wait_in(); outb(PS2_DATA, v);
}
static UINT8 mouse_read(void){
    ps2_wait_out(); return inb(PS2_DATA);
}

/* Initialise the PS/2 mouse: enable aux device, set defaults, enable
 * data reporting. Works with QEMU's i8042 + PS/2 mouse. */
static void ps2_mouse_init(void){
    /* enable auxiliary (mouse) device */
    ps2_wait_in(); outb(PS2_CMD, 0xA8);

    /* read controller config, set "enable IRQ12 / aux" + keep keyboard */
    ps2_wait_in(); outb(PS2_CMD, 0x20);          /* read config byte */
    UINT8 cfg = mouse_read();
    cfg |= 0x02;            /* enable aux interrupt (bit1) */
    cfg &= ~0x20;           /* clear "disable mouse clock" (bit5) */
    ps2_wait_in(); outb(PS2_CMD, 0x60);          /* write config byte */
    ps2_wait_in(); outb(PS2_DATA, cfg);

    mouse_write(0xF6); mouse_read();             /* set defaults, ACK */
    mouse_write(0xF4); mouse_read();             /* enable data reporting, ACK */
}


/* ===================== entry point ===================== */
/* MinGW emits calls to __chkstk_ms for large stack frames (the JPEG
 * decoder has big local arrays). We have ample stack, so provide a no-op
 * probe stub to satisfy the linker. */
__attribute__((used)) void ___chkstk_ms(void){}
__attribute__((used)) void __chkstk_ms(void){}

/* names of the CPU exceptions we handle */
static const char *EXC_NAME[20] = {
 "Divide by zero","Debug","NMI","Breakpoint","Overflow","Bound range",
 "Invalid opcode","Device N/A","Double fault","Coproc overrun","Invalid TSS",
 "Segment not present","Stack fault","General protection","Page fault",
 "Reserved","x87 FP error","Alignment check","Machine check","SIMD FP"};

/* draw a hex number (used on the crash screen) */
static void crash_hex(int x,int y,unsigned long long v,UINT32 col){
    char b[19]; b[0]='0'; b[1]='x';
    const char *h="0123456789ABCDEF";
    for(int i=0;i<16;i++) b[2+i]=h[(v>>((15-i)*4))&0xF];
    b[18]=0; draw_text(x,y,b,col);
}

/* Called by the exception handler on a CPU fault. Paints the crash screen,
 * shows the error, counts down ~5s, then reboots. Never returns. */
void pc11_panic(int vec, unsigned long long err){
    /* paint the whole screen dark red directly to the framebuffer */
    UINT32 bg = RGB(0x60,0x10,0x10);
    if(FB && SW && SH){
        for(UINT32 y=0;y<SH;y++)
            for(UINT32 x=0;x<SW;x++) FB[(UINTN)y*PPSL+x]=bg;
    }
    /* we draw into BACK then blit, reusing the normal text routines */
    if(BACK){
        for(UINT32 i=0;i<SW*SH;i++) BACK[i]=bg;
        int cx=SW/2-180, cy=SH/2-70;
        draw_text(cx, cy,      "PC11 has crashed :(", RGB(0xFF,0xFF,0xFF));
        draw_text(cx, cy+24,   "A fatal error occurred:", RGB(0xFF,0xD0,0xD0));
        /* error number + name */
        char line[64]; int p=0;
        const char *pre="  error #";
        while(*pre) line[p++]=*pre++;
        if(vec>=10){ line[p++]='0'+(vec/10); }
        line[p++]='0'+(vec%10);
        line[p++]=' '; line[p++]='-'; line[p++]=' ';
        const char *nm = (vec>=0&&vec<20)? EXC_NAME[vec] : "Unknown";
        while(*nm && p<63) line[p++]=*nm++;
        line[p]=0;
        draw_text(cx, cy+48, line, RGB(0xFF,0xFF,0x80));
        /* error code (hex) */
        draw_text(cx, cy+68, "error code:", RGB(0xFF,0xD0,0xD0));
        crash_hex(cx+96, cy+68, err, RGB(0xFF,0xFF,0x80));
        draw_text(cx, cy+100, "Restarting in 5 seconds...", RGB(0xFF,0xFF,0xFF));
        /* blit */
        for(UINT32 y=0;y<SH;y++){
            UINT32 *s=&BACK[y*SW], *d=&FB[y*PPSL];
            for(UINT32 x=0;x<SW;x++) d[x]=s[x];
        }
        /* live countdown 5..1 */
        for(int s=5;s>=1;s--){
            char c[32]; int q=0; const char *t="Restarting in ";
            while(*t) c[q++]=*t++;
            c[q++]='0'+s;
            const char *t2=" seconds...   ";
            while(*t2) c[q++]=*t2++;
            c[q]=0;
            /* repaint just that line */
            fill_rect(cx, cy+100, 300, 10, bg);
            draw_text(cx, cy+100, c, RGB(0xFF,0xFF,0xFF));
            for(UINT32 y=(UINT32)(cy+100); y<(UINT32)(cy+112) && y<SH; y++){
                UINT32 *src=&BACK[y*SW], *dst=&FB[y*PPSL];
                for(UINT32 x=0;x<SW;x++) dst[x]=src[x];
            }
            if(BS) BS->Stall(1000000); /* 1 second */
        }
    } else if(BS){
        BS->Stall(5000000);
    }
    /* reboot */
    if(RT) RT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
    for(;;) __asm__ __volatile__("hlt");
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable){
    ST = SystemTable;
    BS = ST->BootServices;
    RT = ST->RuntimeServices;   /* keep for shutdown/restart after ExitBootServices */

    /* Disable the UEFI watchdog timer. The firmware arms a ~5-minute
     * watchdog when it launches a boot app; if we don't disarm it, the
     * firmware FORCE-REBOOTS the machine when it expires -- which looked
     * like "PC11 restarts after a while" regardless of what's open. */
    BS->SetWatchdogTimer(0, 0, 0, NULL);

    ST->ConOut->ClearScreen(ST->ConOut);
    ST->ConOut->OutputString(ST->ConOut, (CHAR16*)L"PC11 GUI: starting...\r\n");

    if(get_gop()!=EFI_SUCCESS){
        ST->ConOut->OutputString(ST->ConOut,(CHAR16*)L"No GOP framebuffer.\r\n");
        for(;;);
    }

    /* prefer 1920x1080 if the firmware provides it */
    set_video_mode(1920, 1080);

    SW   = GOP->Mode->Info->HorizontalResolution;
    SH   = GOP->Mode->Info->VerticalResolution;
    PPSL = GOP->Mode->Info->PixelsPerScanLine;
    FB   = (UINT32*)(UINTN)GOP->Mode->FrameBufferBase;

    /* allocate back buffer (while boot services still exist) */
    if(BS->AllocatePool(2 /*EfiLoaderData*/, (UINTN)SW*SH*4, (VOID**)&BACK)!=EFI_SUCCESS){
        ST->ConOut->OutputString(ST->ConOut,(CHAR16*)L"alloc failed\r\n");
        for(;;);
    }
    /* allocate the cached desktop buffer (pre-scaled wallpaper). If this
     * fails we fall back to scaling per frame in render(). */
    if(BS->AllocatePool(2 /*EfiLoaderData*/, (UINTN)SW*SH*4, (VOID**)&DESK)!=EFI_SUCCESS)
        DESK = NULL;
    desk_dirty = 1;

    /* install CPU exception handlers so faults show a crash screen + reboot
     * instead of silently triple-faulting the machine. */
    crash_init();

    /* load the start-menu logo from /home/logo.png (decoded + scaled smoothly) */
    load_logo(ImageHandle);

    /* NOTE: we intentionally KEEP UEFI boot services alive so the Files
     * app can browse real drives/folders on demand. Input is handled with
     * a hybrid scheme: the keyboard via the UEFI console (ConIn) and the
     * mouse by polling the PS/2 controller ports directly. */
    fm_init();          /* enumerate drives for the file manager */

    /* set up the PS/2 mouse (port I/O works while boot services are alive) */
    ps2_mouse_init();

    mx = SW/2; my = SH/2;
    int dragging = -1;          /* window being dragged by its title bar */
    int resizing = -1;          /* window being resized by its grip */
    int prev_btn = 0;
    int prev_rbtn = 0;

    /* mouse packet assembly */
    int pkt_idx = 0;
    UINT8 pkt[3];

    term_init();
    render();

    for(;;){
        int dirty = 0;
        int activity = 0;

        /* Drain ALL pending PS/2 bytes this tick (mouse can queue 3-byte
         * packets faster than one-per-loop; reading one at a time caused
         * the lag). Cap the drain so we can't spin forever. */
        int guard = 0;
        UINT8 status = inb(PS2_STATUS);
        /* Only consume MOUSE bytes (bit5 set). Leave keyboard bytes in the
         * controller so UEFI's ConIn driver can deliver them to us. */
        while((status & 0x01) && (status & 0x20) && guard++ < 64){
            activity = 1;
            {                              /* this byte is from the mouse */
                UINT8 b = inb(PS2_DATA);
                if(pkt_idx==0 && !(b & 0x08)) {
                    /* not a valid first byte (bit3 must be 1); resync */
                } else {
                    pkt[pkt_idx++] = b;
                    if(pkt_idx==3){
                        pkt_idx=0;
                        UINT8 flags = pkt[0];
                        int dx = (int)pkt[1];
                        int dy = (int)pkt[2];
                        if(flags & 0x10) dx -= 256;   /* X sign bit */
                        if(flags & 0x20) dy -= 256;   /* Y sign bit */
                        mx += dx;
                        my -= dy;                     /* screen Y grows downward */
                        if(mx<0) mx=0;
                        if(my<0) my=0;
                        if(mx>=(int)SW) mx=(int)SW-1;
                        if(my>=(int)SH) my=(int)SH-1;

                        int btn  = (flags & 0x01) ? 1 : 0;  /* left button  */
                        int rbtn = (flags & 0x02) ? 1 : 0;  /* right button */

                        /* ---- right click -> open context menu ---- */
                        if(rbtn && !prev_rbtn){
                            ctx_on_file = 0; ctx_target[0]=0;
                            /* in a focused Files window: target the row clicked,
                             * or fall back to the currently-selected file. */
                            int f = focused_win();
                            if(wins[f].alive && !wins[f].minimized && wins[f].kind==WIN_FILES
                               && fm_vol>=0){
                                int fr = fm_hit(f,mx,my);
                                if(fr>=0 && fr<fm_count) fm_sel=fr;   /* clicked a row */
                                /* use whatever file is selected (if it's a file) */
                                if(fm_sel>=0 && fm_sel<fm_count && !fm_entries[fm_sel].is_dir){
                                    ctx_on_file=1;
                                    int k=0; for(;fm_entries[fm_sel].name[k]&&k<MAX_NAME-1;k++) ctx_target[k]=fm_entries[fm_sel].name[k];
                                    ctx_target[k]=0;
                                }
                            }
                            ctx_build();
                            ctx_x=mx; ctx_y=my; ctx_open=1;
                            dirty=1;
                        }
                        prev_rbtn=rbtn;

                        if(btn && !prev_btn){
                            int handled = 0;

                            /* 0) a context menu is open: handle its items first */
                            if(ctx_open){
                                int it=ctx_item_at(mx,my);
                                if(it>=0){
                                    const char *lbl=ctx_items[it];
                                    if(lbl[0]=='C'){ clip_copy(ctx_target); }            /* Copy */
                                    else if(lbl[0]=='E'){                                /* Edit */
                                        edit_load_selected();
                                        for(int w=0;w<NWIN;w++) if(wins[w].kind==WIN_EDIT){
                                            wins[w].alive=1; wins[w].minimized=0; raise_win(w); break;
                                        }
                                    }
                                    else if(lbl[0]=='S'){ set_background(ctx_target); }   /* Set as BG */
                                    else if(lbl[0]=='P'){ clip_paste(); }                /* Paste */
                                }
                                ctx_open=0; handled=1;
                            }

                            /* 1) Start ("PC11") button toggles the menu */
                            if(start_hit(mx,my)){
                                menu_open = !menu_open; handled=1;
                            }
                            /* 2) clicking a start-menu item opens that app */
                            if(!handled && menu_open){
                                int mi = menu_item_at(mx,my);
                                if(mi>=0){
                                    wins[mi].alive=1; wins[mi].minimized=0;
                                    /* opening the Editor: load the file selected in Files */
                                    if(wins[mi].kind==WIN_EDIT && !edit_loaded) edit_load_selected();
                                    raise_win(mi); menu_open=0; handled=1;
                                } else {
                                    /* clicked outside the menu -> close it */
                                    menu_open=0;
                                }
                            }

                            /* 3) windows (topmost first) */
                            for(int z=NWIN-1; z>=0 && !handled; z--){
                                int i = zorder[z];
                                if(!wins[i].alive || wins[i].minimized) continue;
                                int bx,by,bw,bh;
                                btn_close_rect(i,&bx,&by,&bw,&bh);
                                if(pt_in(mx,my,bx,by,bw,bh)){ wins[i].alive=0; handled=1; break; }
                                btn_min_rect(i,&bx,&by,&bw,&bh);
                                if(pt_in(mx,my,bx,by,bw,bh)){ wins[i].minimized=1; handled=1; break; }

                                /* resize grip -> start resizing */
                                if(in_resize_grip(i,mx,my)){
                                    raise_win(i); resizing=i; handled=1; break;
                                }

                                /* power-app buttons */
                                if(wins[i].kind==WIN_POWER){
                                    pwr_shutdown_rect(i,&bx,&by,&bw,&bh);
                                    if(pt_in(mx,my,bx,by,bw,bh)){
                                        RT->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
                                        for(;;) __asm__ __volatile__("hlt");
                                    }
                                    pwr_restart_rect(i,&bx,&by,&bw,&bh);
                                    if(pt_in(mx,my,bx,by,bw,bh)){
                                        RT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
                                        for(;;) __asm__ __volatile__("hlt");
                                    }
                                }

                                /* Files app: toolbar + list interactions */
                                if(wins[i].kind==WIN_FILES){
                                    int fr = fm_hit(i,mx,my);
                                    if(fr==FM_HIT_UP){
                                        raise_win(i);
                                        if(fm_vol>=0){
                                            /* go to parent via ".." */
                                            int saved=fm_sel; fm_sel=-1;
                                            for(int k=0;k<fm_count;k++) if(sstreq2(fm_entries[k].name,"..")){ fm_sel=k; break; }
                                            if(fm_sel>=0) fm_open_selected();
                                            else { /* at root -> back to drive list */ fm_vol=-1; fm_dir=NULL; fm_sel=0; to_ascii(L"select a drive",fm_status,80); }
                                            (void)saved;
                                        }
                                        handled=1; break;
                                    }
                                    if(fr==FM_HIT_NEW){ raise_win(i); fm_create_file(); handled=1; break; }
                                    if(fr==FM_HIT_DEL){ raise_win(i); fm_delete_selected(); handled=1; break; }
                                    if(fr>=0){
                                        raise_win(i);
                                        fm_sel=fr;
                                        /* single click: drives & folders open;
                                         * files show their preview. */
                                        if(fm_vol<0){ fm_open_selected(); }
                                        else if(fr<fm_count && fm_entries[fr].is_dir){ fm_open_selected(); }
                                        else if(fr<fm_count){ fm_preview_file(fm_entries[fr].name); }
                                        handled=1; break;
                                    }
                                }

                                /* Calculator: click a key */
                                if(wins[i].kind==WIN_CALC && in_window(i,mx,my)){
                                    int x0=wins[i].x, y0=wins[i].y+TITLE_H;
                                    int gx=x0+6, gy=y0+38, bw=(wins[i].w-12-3*4)/4, bh=28;
                                    int hit=0;
                                    for(int r=0;r<4&&!hit;r++) for(int c=0;c<4;c++){
                                        char k=calc_keys[r][c]; if(!k) continue;
                                        int bxp=gx+c*(bw+4), byp=gy+r*(bh+4);
                                        if(pt_in(mx,my,bxp,byp,bw,bh)){ raise_win(i); calc_key(k); hit=1; break; }
                                    }
                                    if(hit){ handled=1; break; }
                                }
                                /* Editor: Open / Save buttons + file picker */
                                if(wins[i].kind==WIN_EDIT){
                                    int x0=wins[i].x, y0=wins[i].y+TITLE_H;
                                    if(pt_in(mx,my,x0+4,y0+2,44,14)){          /* Open */
                                        raise_win(i); edit_open_mode=!edit_open_mode; handled=1; break;
                                    }
                                    if(pt_in(mx,my,x0+52,y0+2,44,14)){         /* Save */
                                        raise_win(i); edit_save(); handled=1; break;
                                    }
                                    if(edit_open_mode){                        /* pick a file from overlay */
                                        int ox=x0+20, oy=y0+24, ow=wins[i].w-40;
                                        if(fm_vol>=0){
                                            for(int r=0;r<fm_count;r++){
                                                if(fm_entries[r].is_dir) continue;
                                                int ry=oy+24+r*14;
                                                if(mx>=ox+6 && mx<ox+ow-4 && my>=ry-1 && my<ry+13){
                                                    fm_sel=r; edit_load_selected();
                                                    edit_open_mode=0; raise_win(i); handled=1; break;
                                                }
                                            }
                                        }
                                        if(handled) break;
                                        /* clicked elsewhere in editor: close picker */
                                        if(in_window(i,mx,my)){ edit_open_mode=0; raise_win(i); handled=1; break; }
                                    }
                                }

                                if(in_titlebar(i,mx,my)){ raise_win(i); dragging=i; handled=1; break; }
                                if(in_window(i,mx,my)){ raise_win(i); handled=1; break; }
                            }
                            /* 4) taskbar entry -> restore/raise */
                            if(!handled){
                                int t = taskbar_hit(mx,my);
                                if(t>=0 && wins[t].alive){
                                    wins[t].minimized = 0;
                                    raise_win(t);
                                }
                            }
                        }
                        if(!btn){ dragging=-1; resizing=-1; }
                        if(dragging>=0 && btn){
                            wins[dragging].x += dx;
                            wins[dragging].y -= dy;
                        }
                        if(resizing>=0 && btn){
                            wins[resizing].w += dx;
                            wins[resizing].h -= dy;   /* screen Y grows down */
                            if(wins[resizing].w < MIN_W) wins[resizing].w = MIN_W;
                            if(wins[resizing].h < MIN_H) wins[resizing].h = MIN_H;
                        }
                        prev_btn = btn;
                        dirty = 1;
                    }
                }
            }
            status = inb(PS2_STATUS);   /* re-check for more queued mouse bytes */
        }

        /* ---- keyboard via the UEFI console (boot services alive) ---- */
        {
            EFI_INPUT_KEY key;
            while(ST->ConIn->ReadKeyStroke(ST->ConIn,&key)==EFI_SUCCESS){
                activity = 1;
                int f = focused_win();
                int is_term = wins[f].alive && !wins[f].minimized
                              && wins[f].kind==WIN_TERMINAL;
                int is_files = wins[f].alive && !wins[f].minimized
                               && wins[f].kind==WIN_FILES;
                int is_edit = wins[f].alive && !wins[f].minimized
                               && wins[f].kind==WIN_EDIT;

                if(key.ScanCode==SCAN_ESC){
                    /* clear the screen and halt */
                    for(UINT32 k=0;k<SW*SH;k++) BACK[k]=0;
                    for(UINT32 y=0;y<SH;y++)
                        for(UINT32 x=0;x<SW;x++) FB[(UINTN)y*PPSL+x]=0;
                    for(;;) __asm__ __volatile__("hlt");
                }
                /* arrow keys: navigate Files list, else move focused window */
                if(key.ScanCode==SCAN_UP){
                    if(is_files){ if(fm_sel>0){fm_sel--; if(fm_sel<fm_scroll)fm_scroll=fm_sel;
                                   if(fm_vol>=0 && !fm_entries[fm_sel].is_dir) fm_preview_file(fm_entries[fm_sel].name);} }
                    else wins[f].y-=10;
                    dirty=1; continue;
                }
                if(key.ScanCode==SCAN_DOWN){
                    if(is_files){ int n=(fm_vol<0)?fm_nvols:fm_count;
                                  if(fm_sel<n-1){fm_sel++; 
                                   if(fm_vol>=0 && !fm_entries[fm_sel].is_dir) fm_preview_file(fm_entries[fm_sel].name);} }
                    else wins[f].y+=10;
                    dirty=1; continue;
                }
                if(key.ScanCode==SCAN_LEFT){  if(!is_files&&!is_edit) wins[f].x-=10; dirty=1; continue; }
                if(key.ScanCode==SCAN_RIGHT){ if(!is_files&&!is_edit) wins[f].x+=10; dirty=1; continue; }

                if(key.UnicodeChar=='\t'){ raise_win(zorder[0]); dirty=1; continue; }

                if(key.UnicodeChar=='\r'){          /* Enter */
                    if(is_term){ term_enter(); dirty=1; }
                    else if(is_files){ fm_open_selected(); dirty=1; }
                    else if(is_edit){ if(edit_len<EDIT_MAX-1) edit_buf[edit_len++]='\n'; dirty=1; }
                    continue;
                }
                if(key.UnicodeChar=='\b'){          /* Backspace */
                    if(is_term){ term_type('\b'); dirty=1; }
                    else if(is_edit){ if(edit_len>0) edit_len--; dirty=1; }
                    continue;
                }
                if(is_term && key.UnicodeChar>=0x20 && key.UnicodeChar<0x7f){
                    term_type((char)key.UnicodeChar); dirty=1;
                }
                if(is_edit && key.UnicodeChar>=0x20 && key.UnicodeChar<0x7f){
                    if(edit_len<EDIT_MAX-1) edit_buf[edit_len++]=(char)key.UnicodeChar;
                    dirty=1;
                }
            }
        }

        if(dirty) render();
        /* Snappy when there's input; idle a little longer otherwise so we
         * don't burn the CPU. This removes the typing/cursor lag. */
        if(!activity) BS->Stall(2000);   /* ~2ms only when idle */
    }
}
