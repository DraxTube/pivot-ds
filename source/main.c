/*
 * PivotDS - Pivot Animator for Nintendo DS
 * v3: double-buffered rendering (no flicker), custom shape library
 */

#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ═══════════════════════════════════════════════════════════════
   Constants
   ═══════════════════════════════════════════════════════════════ */
#define MAX_FRAMES      200
#define MAX_JOINTS       20   /* per object — increased for custom shapes */
#define MAX_OBJS         10
#define UNDO_LEVELS       8
#define SCREEN_W        256
#define SCREEN_H        192
#define CANVAS_H        144
#define ROW0_Y          144
#define ROW1_Y          168
#define BTN_W            42
#define BTN_H            23
#define NOTIF_FRAMES     90

/* Custom shape library */
#define MAX_SHAPES       12
#define SHAPE_NAME_LEN   10

/* ═══════════════════════════════════════════════════════════════
   Colours
   ═══════════════════════════════════════════════════════════════ */
#define COL_WHITE  (RGB15(31,31,31)|BIT(15))
#define COL_BLACK  (RGB15( 0, 0, 0)|BIT(15))
#define COL_GRAY   (RGB15(20,20,20)|BIT(15))
#define COL_DGRAY  (RGB15( 8, 8,10)|BIT(15))
#define COL_MGRAY  (RGB15(14,14,16)|BIT(15))
#define COL_RED    (RGB15(31, 0, 0)|BIT(15))
#define COL_HANDLE (RGB15(31,20, 0)|BIT(15))
#define COL_SEL    (RGB15( 0,18,31)|BIT(15))
#define COL_ONION  (RGB15(15,15,26)|BIT(15))
#define COL_GRID   (RGB15(27,27,29)|BIT(15))

/* ═══════════════════════════════════════════════════════════════
   Types
   ═══════════════════════════════════════════════════════════════ */
typedef struct { int x, y, parent, length; } Joint;

typedef struct {
    int   type;          /* 0=stickman  1=ball  2=custom */
    u16   color;
    int   num_joints;
    Joint joints[MAX_JOINTS];
    int   shape_id;      /* index into custom_shapes[] if type==2, else -1 */
} Object;

typedef struct {
    Object objs[MAX_OBJS];
    int    num_objs;
} Frame;

/* A saved custom shape template */
typedef struct {
    char  name[SHAPE_NAME_LEN];
    int   num_joints;
    /* joints stored relative to joint[0] as origin */
    int   rel_x[MAX_JOINTS];
    int   rel_y[MAX_JOINTS];
    int   parent[MAX_JOINTS];
    int   length[MAX_JOINTS];
    bool  used;
} CustomShape;

/* ═══════════════════════════════════════════════════════════════
   Global state
   ═══════════════════════════════════════════════════════════════ */
static Frame       frames[MAX_FRAMES];
static int         num_frames  = 1;
static int         cur_frame   = 0;
static int         play_frame  = 0;
static int         play_timer  = 0;
static bool        is_playing  = false;
static int         fps         = 8;

static Frame       undo_buf[UNDO_LEVELS];
static int         undo_top    = 0;
static int         undo_count  = 0;

static CustomShape custom_shapes[MAX_SHAPES];
static int         num_shapes  = 0;

/* Palette */
static const u16 palette[] = {
    RGB15( 0, 0, 0)|BIT(15),   /* 0 BLK */
    RGB15(31, 0, 0)|BIT(15),   /* 1 RED */
    RGB15( 0,22, 0)|BIT(15),   /* 2 GRN */
    RGB15( 0, 0,31)|BIT(15),   /* 3 BLU */
    RGB15(31,15, 0)|BIT(15),   /* 4 ORG */
    RGB15(18, 0,22)|BIT(15),   /* 5 PRP */
    RGB15( 0,18,18)|BIT(15),   /* 6 TEL */
    RGB15(31,31, 0)|BIT(15),   /* 7 YLW */
};
static const char* pal_name[] = {"BLK","RED","GRN","BLU","ORG","PRP","TEL","YLW"};
#define NUM_COLORS 8

static int  cur_color   = 0;
static int  cur_shape   = 0;   /* 0=stickman 1=ball 2+=custom */
static int  cam_x = 0, cam_y = 0;

/* UI mode */
typedef enum { MODE_EDIT=0, MODE_SHAPE_MENU, MODE_SHAPE_DRAW, MODE_SHAPE_NAME } UIMode;
static UIMode ui_mode = MODE_EDIT;

/* Shape drawing state */
static Object  shape_draft;              /* object being drawn as new shape */
static int     shape_draw_step = 0;      /* 0=place root, 1+=add joints     */
static char    shape_name_buf[SHAPE_NAME_LEN] = "";
static int     shape_name_len = 0;
static int     shape_sel      = -1;      /* selected shape slot in menu      */

/* Keyboard for naming */
static const char* kbd_rows[] = { "ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ0123", "456789 <OK" };
#define KBD_ROWS 4

/* Notifications */
static char notif_msg[32] = "";
static int  notif_timer   = 0;

/* ═══════════════════════════════════════════════════════════════
   Double-buffer: two back-buffers in main RAM, flushed at VBlank
   ═══════════════════════════════════════════════════════════════ */
static u16 bbMain[SCREEN_W * SCREEN_H];   /* back-buffer top    */
static u16 bbSub [SCREEN_W * SCREEN_H];   /* back-buffer bottom */
static u16* vramMain;                      /* VRAM pointers      */
static u16* vramSub;

/* Copy back-buffer → VRAM after VBlank (no mid-frame tearing) */
static void flush_buffers(void) {
    swiWaitForVBlank();
    /* Use DMA for fast copy — channel 3, 16-bit words */
    dmaCopyWords(3, bbMain, vramMain, SCREEN_W * SCREEN_H * 2);
    dmaCopyWords(3, bbSub,  vramSub,  SCREEN_W * SCREEN_H * 2);
}

/* ═══════════════════════════════════════════════════════════════
   Drawing primitives  (write to back-buffer only)
   ═══════════════════════════════════════════════════════════════ */
static inline void pset(u16* fb, int x, int y, u16 c) {
    if ((unsigned)x < SCREEN_W && (unsigned)y < SCREEN_H)
        fb[y * SCREEN_W + x] = c;
}

static void frect(u16* fb, int x0, int y0, int x1, int y1, u16 c) {
    if (x0 > x1) { int t=x0; x0=x1; x1=t; }
    if (y0 > y1) { int t=y0; y0=y1; y1=t; }
    for (int y=y0; y<=y1; y++)
        for (int x=x0; x<=x1; x++)
            pset(fb, x, y, c);
}

static void fcircle(u16* fb, int cx, int cy, int r, u16 c) {
    int r2=r*r;
    for (int dy=-r; dy<=r; dy++)
        for (int dx=-r; dx<=r; dx++)
            if (dx*dx+dy*dy<=r2) pset(fb, cx+dx, cy+dy, c);
}

static void line(u16* fb, int x0, int y0, int x1, int y1, int th, u16 c) {
    int dx=abs(x1-x0), sx=x0<x1?1:-1;
    int dy=abs(y1-y0), sy=y0<y1?1:-1;
    int err=(dx>dy?dx:-dy)/2, e2;
    for(;;){
        fcircle(fb, x0, y0, th, c);
        if(x0==x1&&y0==y1) break;
        e2=err;
        if(e2>-dx){err-=dy;x0+=sx;}
        if(e2< dy){err+=dx;y0+=sy;}
    }
}

static void hrect(u16* fb, int x0, int y0, int x1, int y1, u16 c) {
    for(int x=x0;x<=x1;x++){pset(fb,x,y0,c);pset(fb,x,y1,c);}
    for(int y=y0;y<=y1;y++){pset(fb,x0,y,c);pset(fb,x1,y,c);}
}

/* ═══════════════════════════════════════════════════════════════
   Tiny 5×6 pixel font
   ═══════════════════════════════════════════════════════════════ */
static const u8 fnt[][6]={
{0,0,0,0,0,0},           /* ' ' */
{4,4,4,0,4,0},           /* '!' */
{10,10,0,0,0,0},         /* '"' */
{0,0,0,0,0,0},           /* '#' */
{0,0,0,0,0,0},           /* '$' */
{0,0,0,0,0,0},           /* '%' */
{0,0,0,0,0,0},           /* '&' */
{2,2,0,0,0,0},           /* ''' */
{2,4,4,4,2,0},           /* '(' */
{4,2,2,2,4,0},           /* ')' */
{0,10,4,10,0,0},         /* '*' */
{0,4,14,4,0,0},          /* '+' */
{0,0,0,2,2,4},           /* ',' */
{0,0,14,0,0,0},          /* '-' */
{0,0,0,0,4,0},           /* '.' */
{8,4,4,2,1,0},           /* '/' */
{14,17,17,17,14,0},      /* '0' */
{6,2,2,2,7,0},           /* '1' */
{14,1,6,8,15,0},         /* '2' */
{14,1,6,1,14,0},         /* '3' */
{6,10,31,2,2,0},         /* '4' */
{31,16,30,1,30,0},       /* '5' */
{14,16,30,17,14,0},      /* '6' */
{31,1,2,4,4,0},          /* '7' */
{14,17,14,17,14,0},      /* '8' */
{14,17,15,1,14,0},       /* '9' */
{0,4,0,4,0,0},           /* ':' */
{0,4,0,4,4,0},           /* ';' */
{2,4,8,4,2,0},           /* '<' */
{0,14,0,14,0,0},         /* '=' */
{8,4,2,4,8,0},           /* '>' */
{14,1,6,0,4,0},          /* '?' */
{14,17,23,16,14,0},      /* '@' */
{4,10,31,17,17,0},       /* 'A' */
{30,17,30,17,30,0},      /* 'B' */
{14,17,16,17,14,0},      /* 'C' */
{28,18,17,18,28,0},      /* 'D' */
{31,16,28,16,31,0},      /* 'E' */
{31,16,28,16,16,0},      /* 'F' */
{14,16,23,17,15,0},      /* 'G' */
{17,17,31,17,17,0},      /* 'H' */
{14,4,4,4,14,0},         /* 'I' */
{1,1,1,17,14,0},         /* 'J' */
{17,18,28,18,17,0},      /* 'K' */
{16,16,16,16,31,0},      /* 'L' */
{17,27,21,17,17,0},      /* 'M' */
{17,25,21,19,17,0},      /* 'N' */
{14,17,17,17,14,0},      /* 'O' */
{30,17,30,16,16,0},      /* 'P' */
{14,17,17,19,15,0},      /* 'Q' */
{30,17,30,18,17,0},      /* 'R' */
{15,16,14,1,30,0},       /* 'S' */
{31,4,4,4,4,0},          /* 'T' */
{17,17,17,17,14,0},      /* 'U' */
{17,17,17,10,4,0},       /* 'V' */
{17,17,21,27,17,0},      /* 'W' */
{17,10,4,10,17,0},       /* 'X' */
{17,10,4,4,4,0},         /* 'Y' */
{31,2,4,8,31,0},         /* 'Z' */
};

static void dchar(u16* fb, int px, int py, char c, u16 col){
    if(c>='a'&&c<='z') c-=32;
    if(c<' '||c>'Z') return;
    const u8* g=fnt[c-' '];
    for(int r=0;r<6;r++){u8 b=g[r]; for(int i=4;i>=0;i--) if(b&(1<<i)) pset(fb,px+(4-i),py+r,col);}
}
static void dstr(u16* fb, int px, int py, const char* s, u16 col){
    while(*s){dchar(fb,px,py,*s,col);px+=6;s++;}
}
static int slen(const char* s){int n=0;while(*s++)n++;return n;}
static void dstrc(u16* fb, int cx, int py, const char* s, u16 col){
    dstr(fb,cx-slen(s)*3,py,s,col);
}

/* ═══════════════════════════════════════════════════════════════
   Notifications
   ═══════════════════════════════════════════════════════════════ */
static void notif(const char* m){
    strncpy(notif_msg,m,sizeof(notif_msg)-1);
    notif_msg[sizeof(notif_msg)-1]='\0';
    notif_timer=NOTIF_FRAMES;
}

/* ═══════════════════════════════════════════════════════════════
   Object helpers
   ═══════════════════════════════════════════════════════════════ */
static void init_stickman(Object* o, int x, int y, u16 col){
    o->type=0; o->color=col; o->shape_id=-1; o->num_joints=11;
    o->joints[0] =(Joint){x,   y,    -1,0};
    o->joints[1] =(Joint){x,   y-24,  0,24};
    o->joints[2] =(Joint){x,   y-40,  1,16};
    o->joints[3] =(Joint){x-19,y-20,  1,19};
    o->joints[4] =(Joint){x-39,y-4,   3,25};
    o->joints[5] =(Joint){x+19,y-20,  1,19};
    o->joints[6] =(Joint){x+39,y-4,   5,25};
    o->joints[7] =(Joint){x-11,y+28,  0,30};
    o->joints[8] =(Joint){x-19,y+56,  7,29};
    o->joints[9] =(Joint){x+11,y+28,  0,30};
    o->joints[10]=(Joint){x+19,y+56,  9,29};
}

static void init_ball(Object* o, int x, int y, u16 col){
    o->type=1; o->color=col; o->shape_id=-1; o->num_joints=2;
    o->joints[0]=(Joint){x,   y,-1,0};
    o->joints[1]=(Joint){x+15,y, 0,15};
}

static void init_custom_obj(Object* o, int shape_idx, int x, int y, u16 col){
    CustomShape* cs=&custom_shapes[shape_idx];
    o->type=2; o->color=col; o->shape_id=shape_idx;
    o->num_joints=cs->num_joints;
    for(int i=0;i<cs->num_joints;i++){
        o->joints[i]=(Joint){x+cs->rel_x[i], y+cs->rel_y[i],
                             cs->parent[i], cs->length[i]};
    }
}

static void init_frames_all(void){
    memset(&frames[0],0,sizeof(Frame));
    frames[0].num_objs=1;
    init_stickman(&frames[0].objs[0],128,90,palette[0]);
    num_frames=1; cur_frame=0; play_frame=0;
    cam_x=0; cam_y=0;
    undo_top=0; undo_count=0;
}

static bool is_desc(const Object* o, int j, int anc){
    int p=o->joints[j].parent;
    while(p!=-1){if(p==anc)return true;p=o->joints[p].parent;}
    return false;
}

static void move_tree(Object* o, int j, int dx, int dy){
    o->joints[j].x+=dx; o->joints[j].y+=dy;
    for(int i=0;i<o->num_joints;i++)
        if(is_desc(o,i,j)){o->joints[i].x+=dx;o->joints[i].y+=dy;}
}

/* ─── Undo ─── */
static void push_undo(void){
    undo_buf[undo_top]=frames[cur_frame];
    undo_top=(undo_top+1)%UNDO_LEVELS;
    if(undo_count<UNDO_LEVELS)undo_count++;
}
static void pop_undo(void){
    if(!undo_count)return;
    undo_top=(undo_top-1+UNDO_LEVELS)%UNDO_LEVELS;
    frames[cur_frame]=undo_buf[undo_top];
    undo_count--;
    notif("UNDO");
}

/* ═══════════════════════════════════════════════════════════════
   Draw object  (to any back-buffer)
   ═══════════════════════════════════════════════════════════════ */
static u16 blend(u16 c, u16 t){
    int r=(((c>>10)&31)+((t>>10)&31))>>1;
    int g=(((c>>5)&31)+((t>>5)&31))>>1;
    int b=((c&31)+(t&31))>>1;
    return RGB15(r,g,b)|BIT(15);
}

static void draw_obj(u16* fb, const Object* o, bool edit, bool onion, int cx, int cy){
    u16 col=onion?blend(o->color,COL_ONION):o->color;

    if(o->type==1){ /* ball */
        int dx=o->joints[1].x-o->joints[0].x;
        int dy=o->joints[1].y-o->joints[0].y;
        int r=(int)sqrtf((float)(dx*dx+dy*dy)); if(r<1)r=1;
        fcircle(fb,o->joints[0].x-cx,o->joints[0].y-cy,r,col);
        if(edit){
            fcircle(fb,o->joints[0].x-cx,o->joints[0].y-cy,4,COL_HANDLE);
            fcircle(fb,o->joints[1].x-cx,o->joints[1].y-cy,4,COL_HANDLE);
        }
        return;
    }
    /* stickman or custom: draw bones */
    for(int i=1;i<o->num_joints;i++){
        int p=o->joints[i].parent;
        if(p>=0) line(fb,
            o->joints[i].x-cx,o->joints[i].y-cy,
            o->joints[p].x-cx,o->joints[p].y-cy,2,col);
    }
    if(o->type==0){ /* stickman head */
        fcircle(fb,o->joints[2].x-cx,o->joints[2].y-cy,8,col);
        if(!edit&&!onion)
            fcircle(fb,o->joints[2].x-cx+2,o->joints[2].y-cy-2,1,COL_WHITE);
    }
    if(edit){
        for(int i=0;i<o->num_joints;i++)
            fcircle(fb,o->joints[i].x-cx,o->joints[i].y-cy,3,COL_HANDLE);
    }
}

/* ═══════════════════════════════════════════════════════════════
   Shape builder draw  (draft object with bigger handles)
   ═══════════════════════════════════════════════════════════════ */
static void draw_shape_draft(u16* fb){
    u16 col=palette[cur_color];
    for(int i=1;i<shape_draft.num_joints;i++){
        int p=shape_draft.joints[i].parent;
        if(p>=0) line(fb,
            shape_draft.joints[i].x-cam_x,shape_draft.joints[i].y-cam_y,
            shape_draft.joints[p].x-cam_x,shape_draft.joints[p].y-cam_y,2,col);
    }
    for(int i=0;i<shape_draft.num_joints;i++){
        fcircle(fb,shape_draft.joints[i].x-cam_x,shape_draft.joints[i].y-cam_y,
                4,i==0?COL_SEL:COL_HANDLE);
        /* joint number */
        char n[4]; n[0]='0'+i; n[1]=0;
        dstr(fb,shape_draft.joints[i].x-cam_x+5,shape_draft.joints[i].y-cam_y-4,n,COL_BLACK);
    }
    /* instruction */
    if(shape_draft.num_joints==0)
        dstrc(fb,128,68,"TAP TO PLACE ROOT",COL_GRAY);
    else if(shape_draft.num_joints<MAX_JOINTS)
        dstrc(fb,128,68,"TAP=ADD JOINT  B=DONE",COL_GRAY);
    else
        dstrc(fb,128,68,"MAX JOINTS  B=DONE",COL_GRAY);
}

/* ═══════════════════════════════════════════════════════════════
   UI — button helpers
   ═══════════════════════════════════════════════════════════════ */
static void draw_btn(u16* fb, int ci, int row, const char* lbl, bool active){
    int x0=ci*BTN_W, y0=row?ROW1_Y:ROW0_Y;
    int x1=x0+BTN_W-1, y1=y0+BTN_H-1;
    u16 bg=active?RGB15(4,12,2)|BIT(15):RGB15(6,6,8)|BIT(15);
    u16 hi=active?RGB15(12,26,6)|BIT(15):RGB15(20,20,23)|BIT(15);
    u16 sh=RGB15(2,2,3)|BIT(15);
    frect(fb,x0+1,y0+1,x1-1,y1-1,bg);
    for(int x=x0;x<=x1;x++){pset(fb,x,y0,hi);pset(fb,x,y1,sh);}
    for(int y=y0;y<=y1;y++){pset(fb,x0,y,hi);pset(fb,x1,y,sh);}
    dstr(fb, x0+BTN_W/2-slen(lbl)*3, y0+BTN_H/2-3, lbl, COL_WHITE);
}

/* ═══════════════════════════════════════════════════════════════
   Render: timeline strip
   ═══════════════════════════════════════════════════════════════ */
static void draw_timeline_sub(u16* fb, int track_frame){
    int ty=ROW1_Y-2;
    frect(fb,0,ty,SCREEN_W-1,ty,RGB15(4,4,5)|BIT(15));
    if(num_frames>1){
        for(int i=0;i<num_frames;i++){
            int tx=4+i*(SCREEN_W-8)/(num_frames-1);
            if(i==track_frame) fcircle(fb,tx,ty,2,palette[cur_color]);
            else pset(fb,tx,ty,RGB15(16,16,16)|BIT(15));
        }
    }
}

static void draw_timeline_main(u16* fb, int track_frame){
    int ty=SCREEN_H-9;
    frect(fb,0,ty-2,SCREEN_W-1,SCREEN_H-1,COL_DGRAY);
    line(fb,6,ty+3,SCREEN_W-10,ty+3,1,COL_GRAY);
    for(int i=0;i<num_frames;i++){
        int tx=6+i*(SCREEN_W-16)/(num_frames>1?num_frames-1:1);
        if(i==track_frame) fcircle(fb,tx,ty+3,3,COL_RED);
        else line(fb,tx,ty+1,tx,ty+5,1,RGB15(14,14,14)|BIT(15));
    }
    if(is_playing) fcircle(fb,SCREEN_W-5,ty+3,3,RGB15(0,24,0)|BIT(15));
    char b[8]; sprintf(b,"F%d",track_frame+1);
    dstr(fb,2,ty-1,b,RGB15(14,14,14)|BIT(15));
}

/* ═══════════════════════════════════════════════════════════════
   Render: notification
   ═══════════════════════════════════════════════════════════════ */
static void draw_notif(u16* fb){
    if(notif_timer<=0) return;
    int w=slen(notif_msg)*6+10;
    int x0=128-w/2, y0=58;
    frect(fb,x0,y0,x0+w,y0+12,RGB15(0,5,0)|BIT(15));
    hrect(fb,x0,y0,x0+w,y0+12,RGB15(0,18,0)|BIT(15));
    dstrc(fb,128,y0+3,notif_msg,COL_WHITE);
}

/* ═══════════════════════════════════════════════════════════════
   Render: EDIT MODE UI (sub screen bottom)
   ═══════════════════════════════════════════════════════════════ */
static void draw_edit_ui(u16* fb, int sel_obj){
    frect(fb,0,ROW0_Y,SCREEN_W-1,SCREEN_H-1,COL_DGRAY);
    draw_timeline_sub(fb,cur_frame);

    /* Row 0: frame controls */
    draw_btn(fb,0,0,"PREV",false);
    draw_btn(fb,1,0,"NEXT",false);
    draw_btn(fb,2,0,"+FRM",false);
    draw_btn(fb,3,0,"-FRM",false);
    draw_btn(fb,4,0,"UNDO",undo_count>0);
    draw_btn(fb,5,0,"CLR", false);

    /* Row 1: playback + tools */
    draw_btn(fb,0,1,is_playing?"STOP":"PLAY",is_playing);
    {char b[6];sprintf(b,"%dFPS",fps);draw_btn(fb,1,1,b,false);}
    draw_btn(fb,2,1,pal_name[cur_color],sel_obj>=0);
    /* colour dot */
    fcircle(fb,2*BTN_W+BTN_W/2,ROW1_Y+BTN_H-6,4,palette[cur_color]);

    /* Shape selector — show stickman / ball / custom names */
    {
        const char* sname;
        char cb[SHAPE_NAME_LEN+2];
        if(cur_shape==0)      sname="STKM";
        else if(cur_shape==1) sname="BALL";
        else {
            /* custom shape */
            int idx=cur_shape-2;
            if(idx<num_shapes&&custom_shapes[idx].used){
                sprintf(cb,"%.4s",custom_shapes[idx].name);
                sname=cb;
            } else sname="STKM";
        }
        draw_btn(fb,3,1,sname,false);
    }
    draw_btn(fb,4,1,"+OBJ",false);
    draw_btn(fb,5,1,"SHPE",false);   /* opens shape builder */

    /* frame counter */
    {char b[12];sprintf(b,"F%d/%d",cur_frame+1,num_frames);
     frect(fb,0,ROW0_Y-10,46,ROW0_Y-1,COL_BLACK);
     dstr(fb,2,ROW0_Y-9,b,COL_GRAY);}
}

/* ═══════════════════════════════════════════════════════════════
   Render: SHAPE MENU (choose action)
   ═══════════════════════════════════════════════════════════════ */
static void render_shape_menu(void){
    /* top: list saved shapes */
    frect(bbMain,0,0,SCREEN_W-1,SCREEN_H-1,RGB15(5,5,8)|BIT(15));
    dstrc(bbMain,128,4,"SHAPE LIBRARY",COL_WHITE);
    line(bbMain,0,14,SCREEN_W-1,14,1,COL_GRAY);

    for(int i=0;i<MAX_SHAPES;i++){
        int tx=8+(i%6)*42, ty=20+(i/6)*30;
        u16 bg=(i==shape_sel)?RGB15(4,10,18)|BIT(15):RGB15(10,10,13)|BIT(15);
        frect(bbMain,tx,ty,tx+38,ty+24,bg);
        hrect(bbMain,tx,ty,tx+38,ty+24,i==shape_sel?COL_SEL:COL_GRAY);
        if(i<num_shapes&&custom_shapes[i].used)
            dstrc(bbMain,tx+19,ty+9,custom_shapes[i].name,COL_WHITE);
        else
            dstrc(bbMain,tx+19,ty+9,"---",COL_GRAY);
    }

    dstrc(bbMain,128,SCREEN_H-30,"A=USE  B=BACK  X=NEW  Y=DEL",COL_GRAY);

    /* sub: instructions */
    frect(bbSub,0,0,SCREEN_W-1,SCREEN_H-1,COL_DGRAY);
    dstrc(bbSub,128,30,"TAP SLOT TO SELECT",COL_WHITE);
    dstrc(bbSub,128,50,"A = ADD TO FRAME",COL_GRAY);
    dstrc(bbSub,128,62,"X = DRAW NEW SHAPE",COL_GRAY);
    dstrc(bbSub,128,74,"Y = DELETE SELECTED",COL_GRAY);
    dstrc(bbSub,128,86,"B = BACK TO EDIT",COL_GRAY);
    dstrc(bbSub,128,110,"SHAPES SAVED IN RAM",RGB15(10,10,10)|BIT(15));
    dstrc(bbSub,128,120,"(LOST ON POWER OFF)",RGB15(10,10,10)|BIT(15));
}

/* ═══════════════════════════════════════════════════════════════
   Render: SHAPE DRAW MODE
   ═══════════════════════════════════════════════════════════════ */
static void render_shape_draw(void){
    /* top: preview */
    frect(bbMain,0,0,SCREEN_W-1,SCREEN_H-1,COL_WHITE);
    for(int gy=20;gy<SCREEN_H-12;gy+=20)
        for(int gx=20;gx<SCREEN_W;gx+=20)
            pset(bbMain,gx,gy,COL_GRID);
    draw_shape_draft(bbMain);
    dstrc(bbMain,128,6,"SHAPE BUILDER",RGB15(6,6,8)|BIT(15));
    draw_timeline_main(bbMain,cur_frame);

    /* sub: canvas + instructions */
    frect(bbSub,0,0,SCREEN_W-1,CANVAS_H-1,COL_WHITE);
    for(int gy=0;gy<CANVAS_H;gy+=32)
        for(int gx=0;gx<SCREEN_W;gx+=32)
            pset(bbSub,gx,gy,COL_GRID);
    draw_shape_draft(bbSub);

    frect(bbSub,0,CANVAS_H,SCREEN_W-1,SCREEN_H-1,COL_DGRAY);
    dstrc(bbSub,128,CANVAS_H+4,"TAP=ADD POINT",COL_WHITE);
    {char b[32];sprintf(b,"JOINTS:%d/%d",shape_draft.num_joints,MAX_JOINTS);
     dstrc(bbSub,128,CANVAS_H+14,b,COL_GRAY);}
    dstrc(bbSub,128,CANVAS_H+24,"B=DONE&NAME  SELECT=CLR",COL_GRAY);
}

/* ═══════════════════════════════════════════════════════════════
   Render: SHAPE NAME input
   ═══════════════════════════════════════════════════════════════ */
static void render_shape_name(void){
    frect(bbMain,0,0,SCREEN_W-1,SCREEN_H-1,RGB15(4,4,6)|BIT(15));
    dstrc(bbMain,128,20,"NAME YOUR SHAPE",COL_WHITE);
    /* Name box */
    frect(bbMain,40,50,216,68,RGB15(2,2,4)|BIT(15));
    hrect(bbMain,40,50,216,68,COL_SEL);
    dstrc(bbMain,128,56,shape_name_len?shape_name_buf:"...",COL_WHITE);
    /* cursor blink */
    if((play_timer/15)%2==0&&shape_name_len<SHAPE_NAME_LEN-1){
        int cx=40+8+shape_name_len*6;
        line(bbMain,cx+80,53,cx+80,65,1,COL_WHITE);
    }
    dstrc(bbMain,128,SCREEN_H-20,"A/TAP=OK  B=CANCEL",COL_GRAY);

    /* Sub: on-screen keyboard */
    frect(bbSub,0,0,SCREEN_W-1,SCREEN_H-1,COL_DGRAY);
    dstrc(bbSub,128,4,"KEYBOARD",COL_GRAY);
    for(int r=0;r<KBD_ROWS;r++){
        const char* row=kbd_rows[r];
        int nc=slen(row);
        for(int c=0;c<nc;c++){
            int kx=4+c*25, ky=14+r*42;
            frect(bbSub,kx,ky,kx+22,ky+18,RGB15(10,10,12)|BIT(15));
            hrect(bbSub,kx,ky,kx+22,ky+18,COL_GRAY);
            char k[2]={row[c],0};
            if(row[c]==' ') k[0]='_';
            dstrc(bbSub,kx+11,ky+6,k,COL_WHITE);
        }
    }
    /* display current name on sub too */
    dstrc(bbSub,128,SCREEN_H-12,shape_name_len?shape_name_buf:"",COL_WHITE);
}

/* ═══════════════════════════════════════════════════════════════
   Render: EDIT MODE full frame
   ═══════════════════════════════════════════════════════════════ */
static void render_edit(int sel_obj){
    int show_f=is_playing?play_frame:cur_frame;

    /* ─ TOP: playback preview ─ */
    for(int i=0;i<SCREEN_W*SCREEN_H;i++) bbMain[i]=COL_WHITE;
    for(int gy=20;gy<SCREEN_H-12;gy+=20)
        for(int gx=20;gx<SCREEN_W;gx+=20)
            pset(bbMain,gx,gy,COL_GRID);
    for(int o=0;o<frames[show_f].num_objs;o++)
        draw_obj(bbMain,&frames[show_f].objs[o],false,false,cam_x,cam_y);
    draw_timeline_main(bbMain,show_f);

    /* ─ BOTTOM: editor canvas ─ */
    for(int y=0;y<CANVAS_H;y++)
        for(int x=0;x<SCREEN_W;x++)
            bbSub[y*SCREEN_W+x]=(x%32==0||y%32==0)?COL_GRID:COL_WHITE;

    /* onion skin */
    if(cur_frame>0)
        for(int o=0;o<frames[cur_frame-1].num_objs;o++)
            draw_obj(bbSub,&frames[cur_frame-1].objs[o],false,true,cam_x,cam_y);

    /* current frame */
    for(int o=0;o<frames[cur_frame].num_objs;o++)
        draw_obj(bbSub,&frames[cur_frame].objs[o],true,false,cam_x,cam_y);

    /* selection box */
    if(sel_obj>=0){
        const Object* ob=&frames[cur_frame].objs[sel_obj];
        int x0=9999,y0=9999,x1=-9999,y1=-9999;
        for(int i=0;i<ob->num_joints;i++){
            int jx=ob->joints[i].x-cam_x, jy=ob->joints[i].y-cam_y;
            if(jx<x0)x0=jx;if(jx>x1)x1=jx;
            if(jy<y0)y0=jy;if(jy>y1)y1=jy;
        }
        x0-=10;y0-=10;x1+=10;y1+=10;
        if(x0<0)x0=0;if(y0<0)y0=0;
        if(x1>=SCREEN_W)x1=SCREEN_W-1;
        if(y1>=CANVAS_H)y1=CANVAS_H-1;
        if(x1>x0&&y1>y0) hrect(bbSub,x0,y0,x1,y1,COL_SEL);
    }

    draw_edit_ui(bbSub,sel_obj);
    draw_notif(bbSub);
}

/* ═══════════════════════════════════════════════════════════════
   Main
   ═══════════════════════════════════════════════════════════════ */
int main(void){
    /* Video init */
    videoSetMode(MODE_5_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    int bgM=bgInit(3,BgType_Bmp16,BgSize_B16_256x256,0,0);

    videoSetModeSub(MODE_5_2D);
    vramSetBankC(VRAM_C_SUB_BG);
    int bgS=bgInitSub(3,BgType_Bmp16,BgSize_B16_256x256,0,0);

    vramMain=bgGetGfxPtr(bgM);
    vramSub =bgGetGfxPtr(bgS);

    memset(custom_shapes,0,sizeof(custom_shapes));
    init_frames_all();

    int  sel_obj=-1, sel_joint=-1;
    int  last_tx=0,  last_ty=0;
    bool touch_ui=false;

    while(1){
        scanKeys();
        int keys=keysDown(), held=keysHeld();
        touchPosition tp; touchRead(&tp);

        if(notif_timer>0) notif_timer--;
        play_timer++;

        /* ════════════════════════════════════════════════════════
           SHAPE NAME INPUT MODE
           ════════════════════════════════════════════════════════ */
        if(ui_mode==MODE_SHAPE_NAME){
            if(keys&KEY_B){ ui_mode=MODE_SHAPE_DRAW; }
            if(keys&KEY_A){
                /* confirm name */
                goto save_shape;
            }
            if(keys&KEY_TOUCH){
                /* map touch to keyboard */
                int r=(tp.py-14)/42, c=(tp.px-4)/25;
                if(r>=0&&r<KBD_ROWS&&c>=0){
                    const char* row=kbd_rows[r];
                    int nc=slen(row);
                    if(c<nc){
                        char ch=row[c];
                        if(ch=='<'){ /* backspace */
                            if(shape_name_len>0){ shape_name_len--; shape_name_buf[shape_name_len]=0; }
                        } else if(ch=='O'&&r==3&&c==9){ /* OK (last key of last row) */
                            /* "OK" key — save */
                            goto save_shape;
                        } else {
                            if(shape_name_len<SHAPE_NAME_LEN-1){
                                shape_name_buf[shape_name_len++]=ch;
                                shape_name_buf[shape_name_len]=0;
                            }
                        }
                    }
                }
            }
            /* render */
            render_shape_name();
            flush_buffers();
            continue;

            save_shape:;
            if(shape_name_len==0){ memcpy(shape_name_buf,"SHAPE",6); shape_name_len=5; }
            /* Find free slot */
            int slot=-1;
            for(int i=0;i<MAX_SHAPES;i++) if(!custom_shapes[i].used){slot=i;break;}
            if(slot<0){ notif("SHAPE SLOTS FULL"); ui_mode=MODE_EDIT; continue; }
            CustomShape* cs=&custom_shapes[slot];
            cs->used=true;
            cs->num_joints=shape_draft.num_joints;
            strncpy(cs->name,shape_name_buf,SHAPE_NAME_LEN-1);
            cs->name[SHAPE_NAME_LEN-1]=0;
            int rx=shape_draft.joints[0].x, ry=shape_draft.joints[0].y;
            for(int i=0;i<cs->num_joints;i++){
                cs->rel_x[i]=shape_draft.joints[i].x-rx;
                cs->rel_y[i]=shape_draft.joints[i].y-ry;
                cs->parent[i]=shape_draft.joints[i].parent;
                cs->length[i]=shape_draft.joints[i].length;
            }
            if(slot>=num_shapes) num_shapes=slot+1;
            notif("SHAPE SAVED!");
            ui_mode=MODE_EDIT;
            cur_shape=2+slot;
            continue;
        }

        /* ════════════════════════════════════════════════════════
           SHAPE DRAW MODE
           ════════════════════════════════════════════════════════ */
        if(ui_mode==MODE_SHAPE_DRAW){
            if(keys&KEY_SELECT){ /* clear draft */
                memset(&shape_draft,0,sizeof(Object));
                shape_draft.num_joints=0;
                shape_draw_step=0;
            }
            if(keys&KEY_B){
                /* done — go to name if we have joints */
                if(shape_draft.num_joints>=2){
                    memset(shape_name_buf,0,sizeof(shape_name_buf));
                    shape_name_len=0;
                    ui_mode=MODE_SHAPE_NAME;
                } else {
                    notif("NEED 2+ POINTS");
                }
            }
            if(keys&KEY_TOUCH&&tp.py<CANVAS_H){
                int wx=tp.px+cam_x, wy=tp.py+cam_y;
                int nj=shape_draft.num_joints;
                if(nj<MAX_JOINTS){
                    int px=0, py=0, len=0, par=-1;
                    if(nj>0){
                        par=nj-1; /* connect to previous joint */
                        int dx=wx-shape_draft.joints[par].x;
                        int dy=wy-shape_draft.joints[par].y;
                        len=(int)sqrtf((float)(dx*dx+dy*dy));
                        if(len<1)len=1;
                    }
                    shape_draft.joints[nj]=(Joint){wx,wy,par,len};
                    shape_draft.num_joints++;
                }
            }
            render_shape_draw();
            flush_buffers();
            continue;
        }

        /* ════════════════════════════════════════════════════════
           SHAPE MENU MODE
           ════════════════════════════════════════════════════════ */
        if(ui_mode==MODE_SHAPE_MENU){
            if(keys&KEY_B){ ui_mode=MODE_EDIT; }
            if(keys&KEY_X){
                /* new shape */
                memset(&shape_draft,0,sizeof(Object));
                shape_draft.num_joints=0;
                shape_draw_step=0;
                ui_mode=MODE_SHAPE_DRAW;
            }
            if(keys&KEY_A){
                /* add selected shape to current frame */
                if(shape_sel>=0&&shape_sel<num_shapes&&custom_shapes[shape_sel].used){
                    if(frames[cur_frame].num_objs<MAX_OBJS){
                        init_custom_obj(
                            &frames[cur_frame].objs[frames[cur_frame].num_objs],
                            shape_sel,128+cam_x,90+cam_y,palette[cur_color]);
                        frames[cur_frame].num_objs++;
                        cur_shape=2+shape_sel;
                        notif("OBJ ADDED");
                        ui_mode=MODE_EDIT;
                    } else notif("MAX OBJS!");
                } else notif("SELECT A SHAPE");
            }
            if(keys&KEY_Y){
                /* delete selected shape */
                if(shape_sel>=0&&shape_sel<num_shapes&&custom_shapes[shape_sel].used){
                    custom_shapes[shape_sel].used=false;
                    memset(custom_shapes[shape_sel].name,0,SHAPE_NAME_LEN);
                    notif("SHAPE DELETED");
                    shape_sel=-1;
                }
            }
            if(keys&KEY_TOUCH){
                /* tap slot on top screen — map from sub screen too */
                int r=tp.py/42, c=tp.px/42;
                int idx=r*6+c;
                if(idx<MAX_SHAPES) shape_sel=idx;
            }
            render_shape_menu();
            draw_notif(bbSub);
            flush_buffers();
            continue;
        }

        /* ════════════════════════════════════════════════════════
           EDIT MODE
           ════════════════════════════════════════════════════════ */
        /* Playback */
        if(is_playing&&num_frames>1&&play_timer>=(60/fps)){
            play_timer=0;
            play_frame=(play_frame+1)%num_frames;
        }

        /* Hardware buttons */
        if(keys&KEY_START){
            is_playing=!is_playing;
            if(is_playing){play_frame=cur_frame;play_timer=0;}
        }
        if(!is_playing){
            if(keys&KEY_L&&cur_frame>0) cur_frame--;
            if(keys&KEY_R&&cur_frame<num_frames-1) cur_frame++;
        }
        if(keys&KEY_SELECT&&num_frames<MAX_FRAMES){
            for(int f=num_frames;f>cur_frame+1;f--) frames[f]=frames[f-1];
            frames[cur_frame+1]=frames[cur_frame];
            cur_frame++; num_frames++;
            notif("FRAME ADDED");
        }

        /* D-pad fine nudge */
        if(sel_obj>=0&&sel_joint>=0){
            int ddx=(held&KEY_RIGHT)?1:(held&KEY_LEFT)?-1:0;
            int ddy=(held&KEY_DOWN) ?1:(held&KEY_UP)  ?-1:0;
            if(ddx||ddy){
                Object* ob=&frames[cur_frame].objs[sel_obj];
                if(sel_joint==0){
                    move_tree(ob,0,ddx,ddy);
                } else {
                    int p=ob->joints[sel_joint].parent;
                    int nx=ob->joints[sel_joint].x+ddx;
                    int ny=ob->joints[sel_joint].y+ddy;
                    float d=sqrtf((float)((nx-ob->joints[p].x)*(nx-ob->joints[p].x)+
                                          (ny-ob->joints[p].y)*(ny-ob->joints[p].y)));
                    if(d>0.1f){
                        int l=ob->joints[sel_joint].length;
                        move_tree(ob,sel_joint,
                            (int)(ob->joints[p].x+(nx-ob->joints[p].x)*l/d)-ob->joints[sel_joint].x,
                            (int)(ob->joints[p].y+(ny-ob->joints[p].y)*l/d)-ob->joints[sel_joint].y);
                    }
                }
            }
        }

        /* Touch: initial press */
        if(keys&KEY_TOUCH){
            touch_ui=(tp.py>=CANVAS_H);
            if(touch_ui){
                int row=(tp.py>=ROW1_Y)?1:0;
                int btn=tp.px/BTN_W; if(btn>5)btn=5;
                if(row==0){
                    if(btn==0&&cur_frame>0) cur_frame--;
                    else if(btn==1&&cur_frame<num_frames-1) cur_frame++;
                    else if(btn==2&&num_frames<MAX_FRAMES){
                        for(int f=num_frames;f>cur_frame+1;f--) frames[f]=frames[f-1];
                        frames[cur_frame+1]=frames[cur_frame];
                        cur_frame++;num_frames++;
                        notif("FRAME ADDED");
                    }
                    else if(btn==3&&num_frames>1){
                        for(int f=cur_frame;f<num_frames-1;f++) frames[f]=frames[f+1];
                        num_frames--;
                        if(cur_frame>=num_frames)cur_frame=num_frames-1;
                        notif("FRAME DELETED");
                    }
                    else if(btn==4) pop_undo();
                    else if(btn==5){ init_frames_all(); sel_obj=-1; notif("CLEARED"); }
                } else {
                    if(btn==0){
                        is_playing=!is_playing;
                        if(is_playing){play_frame=cur_frame;play_timer=0;}
                    }
                    else if(btn==1){fps=(fps>=30)?4:fps+4;}
                    else if(btn==2){
                        cur_color=(cur_color+1)%NUM_COLORS;
                        if(sel_obj>=0) frames[cur_frame].objs[sel_obj].color=palette[cur_color];
                    }
                    else if(btn==3){
                        /* cycle shape: stickman→ball→custom shapes→stickman */
                        cur_shape++;
                        int max_s=2+num_shapes;
                        if(cur_shape>=max_s) cur_shape=0;
                    }
                    else if(btn==4){
                        if(frames[cur_frame].num_objs<MAX_OBJS){
                            Object* no=&frames[cur_frame].objs[frames[cur_frame].num_objs];
                            if(cur_shape==0)      init_stickman(no,128+cam_x,96+cam_y,palette[cur_color]);
                            else if(cur_shape==1) init_ball(no,128+cam_x,96+cam_y,palette[cur_color]);
                            else{
                                int idx=cur_shape-2;
                                if(idx<num_shapes&&custom_shapes[idx].used)
                                    init_custom_obj(no,idx,128+cam_x,96+cam_y,palette[cur_color]);
                                else init_stickman(no,128+cam_x,96+cam_y,palette[cur_color]);
                            }
                            frames[cur_frame].num_objs++;
                            notif("OBJ ADDED");
                        } else notif("MAX OBJS!");
                    }
                    else if(btn==5){
                        /* Open shape library */
                        shape_sel=-1;
                        ui_mode=MODE_SHAPE_MENU;
                    }
                }
            } else {
                /* Canvas: pick joint */
                sel_obj=-1;sel_joint=-1;
                int best=20*20;
                for(int o=0;o<frames[cur_frame].num_objs;o++){
                    Object* ob=&frames[cur_frame].objs[o];
                    for(int i=0;i<ob->num_joints;i++){
                        int dx=(tp.px+cam_x)-ob->joints[i].x;
                        int dy=(tp.py+cam_y)-ob->joints[i].y;
                        int d2=dx*dx+dy*dy;
                        if(d2<best){best=d2;sel_obj=o;sel_joint=i;}
                    }
                }
                if(sel_obj>=0){
                    push_undo();
                    u16 oc=frames[cur_frame].objs[sel_obj].color;
                    for(int c=0;c<NUM_COLORS;c++) if(palette[c]==oc){cur_color=c;break;}
                } else {
                    last_tx=tp.px; last_ty=tp.py;
                }
            }
        }

        /* Touch: held — drag joint or pan */
        if((held&KEY_TOUCH)&&!touch_ui&&tp.py<CANVAS_H){
            if(sel_obj>=0){
                Object* ob=&frames[cur_frame].objs[sel_obj];
                int wx=tp.px+cam_x, wy=tp.py+cam_y;
                if(sel_joint==0){
                    move_tree(ob,0,wx-ob->joints[0].x,wy-ob->joints[0].y);
                } else {
                    int p=ob->joints[sel_joint].parent;
                    float d=sqrtf((float)((wx-ob->joints[p].x)*(wx-ob->joints[p].x)+
                                          (wy-ob->joints[p].y)*(wy-ob->joints[p].y)));
                    if(d>0.1f){
                        int l=ob->joints[sel_joint].length;
                        move_tree(ob,sel_joint,
                            (int)(ob->joints[p].x+(wx-ob->joints[p].x)*l/d)-ob->joints[sel_joint].x,
                            (int)(ob->joints[p].y+(wy-ob->joints[p].y)*l/d)-ob->joints[sel_joint].y);
                    }
                }
            } else {
                cam_x-=(tp.px-last_tx); cam_y-=(tp.py-last_ty);
                last_tx=tp.px; last_ty=tp.py;
            }
        }
        if(!(held&KEY_TOUCH)) sel_obj=sel_joint=-1;

        /* Render edit mode */
        render_edit(sel_obj);
        flush_buffers();
    }
    return 0;
}
