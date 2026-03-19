/*
 * PivotDS v4 — Nintendo DS Pivot Animator
 * - Double-buffered (no flicker)
 * - Dynamic joints (no hard limit per shape)
 * - Shape builder: draw figure first, then place animation joints
 * - Shape library: text on top, touch slots on bottom screen
 */

#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ═══════════════════════════════════════════════════════════════
   Constants
   ═══════════════════════════════════════════════════════════════ */
#define MAX_FRAMES        200
#define MAX_OBJS           10
#define UNDO_LEVELS         8
#define SCREEN_W          256
#define SCREEN_H          192
#define CANVAS_H          144
#define ROW0_Y            144
#define ROW1_Y            168
#define BTN_W              42
#define BTN_H              23
#define NOTIF_FRAMES       90

/* Shape library */
#define MAX_SHAPES         16
#define SHAPE_NAME_LEN      9   /* 8 chars + null */

/* Dynamic joint limits (heap-allocated) */
#define MAX_ANIM_JOINTS   128   /* joints per object for animation */
#define MAX_DRAW_PTS      256   /* ink points when sketching a shape */

/* ═══════════════════════════════════════════════════════════════
   Colours
   ═══════════════════════════════════════════════════════════════ */
#define C15(r,g,b) (RGB15(r,g,b)|BIT(15))
#define COL_WHITE  C15(31,31,31)
#define COL_BLACK  C15( 0, 0, 0)
#define COL_GRAY   C15(20,20,20)
#define COL_DGRAY  C15( 8, 8,10)
#define COL_RED    C15(31, 0, 0)
#define COL_HANDLE C15(31,20, 0)
#define COL_SEL    C15( 0,18,31)
#define COL_ONION  C15(15,15,26)
#define COL_GRID   C15(27,27,29)
#define COL_GREEN  C15( 0,22, 0)

/* ═══════════════════════════════════════════════════════════════
   Types
   ═══════════════════════════════════════════════════════════════ */
typedef struct { int x, y, parent, length; } Joint;

/* An animated object: bones are heap-allocated */
typedef struct {
    int    type;       /* 0=stickman  1=ball  2=custom */
    u16    color;
    int    num_joints;
    Joint* joints;     /* malloc'd array */
    int    shape_id;   /* library index if type==2 */
} Object;

typedef struct {
    Object objs[MAX_OBJS];
    int    num_objs;
} Frame;

/* ── Ink stroke for free-draw ── */
typedef struct { s16 x, y; } Pt;

/* ── Saved custom shape ── */
typedef struct {
    char  name[SHAPE_NAME_LEN];
    bool  used;
    /* ink strokes (the visible drawing) */
    int   num_ink;
    Pt*   ink;         /* malloc'd */
    /* animation joints (relative to first joint) */
    int   num_joints;
    int*  rel_x;       /* malloc'd */
    int*  rel_y;
    int*  parent;
    int*  length;
} CustomShape;

/* ═══════════════════════════════════════════════════════════════
   Global state
   ═══════════════════════════════════════════════════════════════ */
static Frame  frames[MAX_FRAMES];
static int    num_frames = 1;
static int    cur_frame  = 0;
static int    play_frame = 0;
static int    play_timer = 0;
static bool   is_playing = false;
static int    fps        = 8;

/* Undo: we store full frame snapshots — joints are shared read-only
   so for simplicity we deep-copy only joint positions */
typedef struct {
    int    num_objs;
    int    num_joints[MAX_OBJS];
    Joint* joints[MAX_OBJS];   /* malloc'd copies */
    int    type[MAX_OBJS];
    u16    color[MAX_OBJS];
    int    shape_id[MAX_OBJS];
} UndoFrame;
static UndoFrame undo_buf[UNDO_LEVELS];
static int       undo_top   = 0;
static int       undo_count = 0;

static CustomShape lib[MAX_SHAPES];
static int         num_shapes = 0;

/* Palette */
static const u16 palette[] = {
    C15( 0, 0, 0), C15(31, 0, 0), C15( 0,22, 0), C15( 0, 0,31),
    C15(31,15, 0), C15(18, 0,22), C15( 0,18,18), C15(31,31, 0),
};
static const char* pal_name[]={"BLK","RED","GRN","BLU","ORG","PRP","TEL","YLW"};
#define NUM_COLORS 8

static int cur_color = 0;
static int cur_shape = 0;   /* 0=stkm 1=ball 2..=custom */
static int cam_x=0, cam_y=0;

/* ── UI mode ── */
typedef enum {
    MODE_EDIT=0,
    MODE_SHAPE_MENU,
    MODE_SHAPE_DRAW_INK,   /* step 1: free-draw the figure */
    MODE_SHAPE_PLACE_JNT,  /* step 2: tap to place animation joints */
    MODE_SHAPE_NAME        /* step 3: name it */
} UIMode;
static UIMode ui_mode = MODE_EDIT;

/* Shape builder temporaries */
static Pt*   draft_ink     = NULL;
static int   draft_ink_n   = 0;
static int   draft_ink_max = 0;
static bool  ink_drawing   = false;   /* pen held */

static Joint* draft_joints = NULL;
static int    draft_jn     = 0;
static int    draft_jmax   = 0;
static int    draft_sel_j  = -1;     /* joint being dragged in place mode */

static char   name_buf[SHAPE_NAME_LEN] = "";
static int    name_len = 0;
static int    shape_sel = -1;        /* selected slot in menu */

/* Keyboard */
static const char* kbd_rows[]={"ABCDEFGHIJ","KLMNOPQRST","UVWXYZ0123","456789 <OK"};
#define KBD_ROWS 4

/* Notifications */
static char notif_msg[32]="";
static int  notif_timer=0;

/* Back-buffers (double-buffering to kill flicker) */
static u16  bbMain[SCREEN_W*SCREEN_H];
static u16  bbSub [SCREEN_W*SCREEN_H];
static u16* vramMain;
static u16* vramSub;

/* ═══════════════════════════════════════════════════════════════
   Memory helpers
   ═══════════════════════════════════════════════════════════════ */
static void* xmalloc(int sz){ void* p=malloc(sz); if(!p) swiSoftReset(); return p; }
static void* xrealloc(void* p,int sz){ void* q=realloc(p,sz); if(!q) swiSoftReset(); return q; }

/* Deep-copy joint array */
static Joint* joints_clone(const Joint* src, int n){
    Joint* d=(Joint*)xmalloc(n*sizeof(Joint));
    memcpy(d,src,n*sizeof(Joint)); return d;
}

/* Free all objects inside a frame (does NOT free frame itself) */
static void frame_free_joints(Frame* f){
    for(int i=0;i<f->num_objs;i++) if(f->objs[i].joints){ free(f->objs[i].joints); f->objs[i].joints=NULL; }
}

/* ═══════════════════════════════════════════════════════════════
   Undo
   ═══════════════════════════════════════════════════════════════ */
static void undo_free_slot(int slot){
    UndoFrame* u=&undo_buf[slot];
    for(int i=0;i<u->num_objs;i++) if(u->joints[i]){free(u->joints[i]);u->joints[i]=NULL;}
    u->num_objs=0;
}

static void push_undo(void){
    int slot=undo_top;
    undo_free_slot(slot);
    UndoFrame* u=&undo_buf[slot];
    Frame* f=&frames[cur_frame];
    u->num_objs=f->num_objs;
    for(int i=0;i<f->num_objs;i++){
        u->num_joints[i]=f->objs[i].num_joints;
        u->joints[i]=joints_clone(f->objs[i].joints,f->objs[i].num_joints);
        u->type[i]=f->objs[i].type;
        u->color[i]=f->objs[i].color;
        u->shape_id[i]=f->objs[i].shape_id;
    }
    undo_top=(undo_top+1)%UNDO_LEVELS;
    if(undo_count<UNDO_LEVELS) undo_count++;
}

static void pop_undo(void){
    if(!undo_count) return;
    undo_top=(undo_top-1+UNDO_LEVELS)%UNDO_LEVELS;
    UndoFrame* u=&undo_buf[undo_top];
    frame_free_joints(&frames[cur_frame]);
    Frame* f=&frames[cur_frame];
    f->num_objs=u->num_objs;
    for(int i=0;i<u->num_objs;i++){
        f->objs[i].num_joints=u->num_joints[i];
        f->objs[i].joints=joints_clone(u->joints[i],u->num_joints[i]);
        f->objs[i].type=u->type[i];
        f->objs[i].color=u->color[i];
        f->objs[i].shape_id=u->shape_id[i];
    }
    undo_count--;
}

/* ═══════════════════════════════════════════════════════════════
   Object init
   ═══════════════════════════════════════════════════════════════ */
static void obj_init_stickman(Object* o, int x, int y, u16 col){
    o->type=0; o->color=col; o->shape_id=-1; o->num_joints=11;
    o->joints=(Joint*)xmalloc(11*sizeof(Joint));
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
static void obj_init_ball(Object* o, int x, int y, u16 col){
    o->type=1; o->color=col; o->shape_id=-1; o->num_joints=2;
    o->joints=(Joint*)xmalloc(2*sizeof(Joint));
    o->joints[0]=(Joint){x,   y,-1,0};
    o->joints[1]=(Joint){x+15,y, 0,15};
}
static void obj_init_custom(Object* o, int idx, int x, int y, u16 col){
    CustomShape* cs=&lib[idx];
    o->type=2; o->color=col; o->shape_id=idx;
    o->num_joints=cs->num_joints;
    o->joints=(Joint*)xmalloc(cs->num_joints*sizeof(Joint));
    for(int i=0;i<cs->num_joints;i++)
        o->joints[i]=(Joint){x+cs->rel_x[i],y+cs->rel_y[i],cs->parent[i],cs->length[i]};
}

static void init_all_frames(void){
    for(int f=0;f<num_frames;f++) frame_free_joints(&frames[f]);
    memset(frames,0,sizeof(Frame)); /* only clears first */
    frames[0].num_objs=1;
    obj_init_stickman(&frames[0].objs[0],128,90,palette[0]);
    num_frames=1; cur_frame=0; play_frame=0; cam_x=0; cam_y=0;
    undo_top=0;
    for(int i=0;i<undo_count;i++) undo_free_slot((undo_top+i)%UNDO_LEVELS);
    undo_count=0;
}

/* ═══════════════════════════════════════════════════════════════
   Library shape free
   ═══════════════════════════════════════════════════════════════ */
static void lib_free(int i){
    if(lib[i].ink)   {free(lib[i].ink);   lib[i].ink=NULL;}
    if(lib[i].rel_x) {free(lib[i].rel_x); lib[i].rel_x=NULL;}
    if(lib[i].rel_y) {free(lib[i].rel_y); lib[i].rel_y=NULL;}
    if(lib[i].parent){free(lib[i].parent);lib[i].parent=NULL;}
    if(lib[i].length){free(lib[i].length);lib[i].length=NULL;}
    lib[i].used=false; lib[i].num_ink=0; lib[i].num_joints=0;
}

/* Draft dynamic arrays */
static void draft_reset(void){
    if(draft_ink){free(draft_ink);draft_ink=NULL;}
    draft_ink_n=0; draft_ink_max=0;
    if(draft_joints){free(draft_joints);draft_joints=NULL;}
    draft_jn=0; draft_jmax=0;
    draft_sel_j=-1; ink_drawing=false;
}

static void draft_add_ink(int x,int y){
    if(draft_ink_n>=draft_ink_max){
        draft_ink_max=draft_ink_max?draft_ink_max*2:64;
        draft_ink=(Pt*)xrealloc(draft_ink,draft_ink_max*sizeof(Pt));
    }
    draft_ink[draft_ink_n++]=(Pt){(s16)x,(s16)y};
}
/* Sentinel: x==s16_min means "lift pen here, don't draw this segment" */
#define INK_LIFT ((s16)(-32768))
static void draft_lift_pen(void){
    if(draft_ink_n>=draft_ink_max){
        draft_ink_max=draft_ink_max?draft_ink_max*2:64;
        draft_ink=(Pt*)xrealloc(draft_ink,draft_ink_max*sizeof(Pt));
    }
    draft_ink[draft_ink_n++]=(Pt){INK_LIFT,(s16)0};
}

static void draft_add_joint(int x,int y){
    if(draft_jn>=draft_jmax){
        draft_jmax=draft_jmax?draft_jmax*2:16;
        draft_joints=(Joint*)xrealloc(draft_joints,draft_jmax*sizeof(Joint));
    }
    int par=-1,len=0;
    if(draft_jn>0){
        par=draft_jn-1;
        int dx=x-draft_joints[par].x, dy=y-draft_joints[par].y;
        len=(int)sqrtf((float)(dx*dx+dy*dy)); if(len<1)len=1;
    }
    draft_joints[draft_jn++]=(Joint){x,y,par,len};
}

/* ═══════════════════════════════════════════════════════════════
   Skeleton helpers
   ═══════════════════════════════════════════════════════════════ */
static bool is_desc(const Object* o,int j,int anc){
    int p=o->joints[j].parent;
    while(p!=-1){if(p==anc)return true;p=o->joints[p].parent;}
    return false;
}
static void move_tree(Object* o,int j,int dx,int dy){
    o->joints[j].x+=dx; o->joints[j].y+=dy;
    for(int i=0;i<o->num_joints;i++)
        if(is_desc(o,i,j)){o->joints[i].x+=dx;o->joints[i].y+=dy;}
}

/* ═══════════════════════════════════════════════════════════════
   Notification
   ═══════════════════════════════════════════════════════════════ */
static void notif(const char* m){
    strncpy(notif_msg,m,sizeof(notif_msg)-1);
    notif_msg[sizeof(notif_msg)-1]=0;
    notif_timer=NOTIF_FRAMES;
}

/* ═══════════════════════════════════════════════════════════════
   DMA flush
   ═══════════════════════════════════════════════════════════════ */
static void flush(void){
    swiWaitForVBlank();
    dmaCopyWords(3,bbMain,vramMain,SCREEN_W*SCREEN_H*2);
    dmaCopyWords(3,bbSub, vramSub, SCREEN_W*SCREEN_H*2);
}

/* ═══════════════════════════════════════════════════════════════
   Drawing primitives
   ═══════════════════════════════════════════════════════════════ */
static inline void pset(u16* fb,int x,int y,u16 c){
    if((unsigned)x<SCREEN_W&&(unsigned)y<SCREEN_H) fb[y*SCREEN_W+x]=c;
}
static void frect(u16* fb,int x0,int y0,int x1,int y1,u16 c){
    if(x0>x1){int t=x0;x0=x1;x1=t;} if(y0>y1){int t=y0;y0=y1;y1=t;}
    for(int y=y0;y<=y1;y++) for(int x=x0;x<=x1;x++) pset(fb,x,y,c);
}
static void fcircle(u16* fb,int cx,int cy,int r,u16 c){
    int r2=r*r;
    for(int dy=-r;dy<=r;dy++) for(int dx=-r;dx<=r;dx++) if(dx*dx+dy*dy<=r2) pset(fb,cx+dx,cy+dy,c);
}
static void bline(u16* fb,int x0,int y0,int x1,int y1,int th,u16 c){
    int dx=abs(x1-x0),sx=x0<x1?1:-1,dy=abs(y1-y0),sy=y0<y1?1:-1,err=(dx>dy?dx:-dy)/2,e2;
    for(;;){fcircle(fb,x0,y0,th,c);if(x0==x1&&y0==y1)break;e2=err;if(e2>-dx){err-=dy;x0+=sx;}if(e2<dy){err+=dx;y0+=sy;}}
}
static void hrect(u16* fb,int x0,int y0,int x1,int y1,u16 c){
    for(int x=x0;x<=x1;x++){pset(fb,x,y0,c);pset(fb,x,y1,c);}
    for(int y=y0;y<=y1;y++){pset(fb,x0,y,c);pset(fb,x1,y,c);}
}

/* ═══════════════════════════════════════════════════════════════
   Font 5×6
   ═══════════════════════════════════════════════════════════════ */
static const u8 fnt[][6]={
{0,0,0,0,0,0},{4,4,4,0,4,0},{10,10,0,0,0,0},{0,0,0,0,0,0},{0,0,0,0,0,0},{0,0,0,0,0,0},{0,0,0,0,0,0},{2,2,0,0,0,0},
{2,4,4,4,2,0},{4,2,2,2,4,0},{0,10,4,10,0,0},{0,4,14,4,0,0},{0,0,0,2,2,4},{0,0,14,0,0,0},{0,0,0,0,4,0},{8,4,4,2,1,0},
{14,17,17,17,14,0},{6,2,2,2,7,0},{14,1,6,8,15,0},{14,1,6,1,14,0},{6,10,31,2,2,0},{31,16,30,1,30,0},
{14,16,30,17,14,0},{31,1,2,4,4,0},{14,17,14,17,14,0},{14,17,15,1,14,0},{0,4,0,4,0,0},{0,4,0,4,4,0},
{2,4,8,4,2,0},{0,14,0,14,0,0},{8,4,2,4,8,0},{14,1,6,0,4,0},{14,17,23,16,14,0},
{4,10,31,17,17,0},{30,17,30,17,30,0},{14,17,16,17,14,0},{28,18,17,18,28,0},{31,16,28,16,31,0},
{31,16,28,16,16,0},{14,16,23,17,15,0},{17,17,31,17,17,0},{14,4,4,4,14,0},{1,1,1,17,14,0},
{17,18,28,18,17,0},{16,16,16,16,31,0},{17,27,21,17,17,0},{17,25,21,19,17,0},{14,17,17,17,14,0},
{30,17,30,16,16,0},{14,17,17,19,15,0},{30,17,30,18,17,0},{15,16,14,1,30,0},{31,4,4,4,4,0},
{17,17,17,17,14,0},{17,17,17,10,4,0},{17,17,21,27,17,0},{17,10,4,10,17,0},{17,10,4,4,4,0},{31,2,4,8,31,0},
};
static void dchar(u16*fb,int px,int py,char c,u16 col){
    if(c>='a'&&c<='z')c-=32; if(c<' '||c>'Z')return;
    const u8*g=fnt[c-' ']; for(int r=0;r<6;r++){u8 b=g[r];for(int i=4;i>=0;i--)if(b&(1<<i))pset(fb,px+(4-i),py+r,col);}
}
static void dstr(u16*fb,int px,int py,const char*s,u16 col){while(*s){dchar(fb,px,py,*s,col);px+=6;s++;}}
static int sl(const char*s){int n=0;while(*s++)n++;return n;}
static void dstrc(u16*fb,int cx,int py,const char*s,u16 col){dstr(fb,cx-sl(s)*3,py,s,col);}

/* ═══════════════════════════════════════════════════════════════
   Draw object onto fb
   ═══════════════════════════════════════════════════════════════ */
static u16 blend(u16 c,u16 t){
    return C15((((c>>10)&31)+((t>>10)&31))>>1,(((c>>5)&31)+((t>>5)&31))>>1,((c&31)+(t&31))>>1);
}

static void draw_ink(u16*fb,const Pt*ink,int n,u16 col,int cx,int cy){
    for(int i=1;i<n;i++){
        /* skip segment if either endpoint is a lift sentinel */
        if(ink[i-1].x==INK_LIFT||ink[i].x==INK_LIFT) continue;
        bline(fb,ink[i-1].x-cx,ink[i-1].y-cy,ink[i].x-cx,ink[i].y-cy,1,col);
    }
}

static void draw_obj(u16*fb,const Object*o,bool edit,bool onion,int cx,int cy){
    u16 col=onion?blend(o->color,COL_ONION):o->color;

    /* If custom, draw ink strokes as the visual */
    if(o->type==2&&o->shape_id>=0&&lib[o->shape_id].used&&lib[o->shape_id].num_ink>0){
        CustomShape*cs=&lib[o->shape_id];
        /* find offset: joint[0] is anchor */
        int ox=o->joints[0].x-cx - cs->rel_x[0];
        int oy=o->joints[0].y-cy - cs->rel_y[0];
        /* transform ink relative to joint positions */
        /* simple: just offset entire ink by obj root minus stored root */
        int rx=o->joints[0].x-cx, ry=o->joints[0].y-cy;
        int srx=cs->rel_x[0], sry=cs->rel_y[0];
        (void)ox;(void)oy;
        for(int i=1;i<cs->num_ink;i++){
            int ax=(int)cs->ink[i-1].x-srx+rx, ay=(int)cs->ink[i-1].y-sry+ry;
            int bx=(int)cs->ink[i].x  -srx+rx, by=(int)cs->ink[i].y  -sry+ry;
            bline(fb,ax,ay,bx,by,1,col);
        }
    } else if(o->type==1){ /* ball */
        int dx=o->joints[1].x-o->joints[0].x, dy=o->joints[1].y-o->joints[0].y;
        int r=(int)sqrtf((float)(dx*dx+dy*dy)); if(r<1)r=1;
        fcircle(fb,o->joints[0].x-cx,o->joints[0].y-cy,r,col);
    } else { /* stickman */
        for(int i=1;i<o->num_joints;i++){
            int p=o->joints[i].parent;
            if(p>=0) bline(fb,o->joints[i].x-cx,o->joints[i].y-cy,
                               o->joints[p].x-cx,o->joints[p].y-cy,2,col);
        }
        if(o->num_joints>2)
            fcircle(fb,o->joints[2].x-cx,o->joints[2].y-cy,8,col);
        if(!edit&&!onion&&o->num_joints>2)
            fcircle(fb,o->joints[2].x-cx+2,o->joints[2].y-cy-2,1,COL_WHITE);
    }

    /* Edit handles */
    if(edit){
        for(int i=0;i<o->num_joints;i++)
            fcircle(fb,o->joints[i].x-cx,o->joints[i].y-cy,3,COL_HANDLE);
    }
}

/* ═══════════════════════════════════════════════════════════════
   UI helpers
   ═══════════════════════════════════════════════════════════════ */
static void draw_btn(u16*fb,int ci,int row,const char*lbl,bool active){
    int x0=ci*BTN_W,y0=row?ROW1_Y:ROW0_Y,x1=x0+BTN_W-1,y1=y0+BTN_H-1;
    u16 bg=active?C15(4,12,2):C15(6,6,8);
    u16 hi=active?C15(12,26,6):C15(20,20,23);
    frect(fb,x0+1,y0+1,x1-1,y1-1,bg);
    for(int x=x0;x<=x1;x++){pset(fb,x,y0,hi);pset(fb,x,y1,C15(2,2,3));}
    for(int y=y0;y<=y1;y++){pset(fb,x0,y,hi);pset(fb,x1,y,C15(2,2,3));}
    dstr(fb,x0+BTN_W/2-sl(lbl)*3,y0+BTN_H/2-3,lbl,COL_WHITE);
}

static void draw_notif(u16*fb){
    if(notif_timer<=0)return;
    int w=sl(notif_msg)*6+10,x0=128-w/2,y0=58;
    frect(fb,x0,y0,x0+w,y0+12,C15(0,5,0));
    hrect(fb,x0,y0,x0+w,y0+12,C15(0,18,0));
    dstrc(fb,128,y0+3,notif_msg,COL_WHITE);
}

static void draw_timeline_main(u16*fb,int tf){
    int ty=SCREEN_H-9;
    frect(fb,0,ty-2,SCREEN_W-1,SCREEN_H-1,COL_DGRAY);
    bline(fb,6,ty+3,SCREEN_W-10,ty+3,1,COL_GRAY);
    for(int i=0;i<num_frames;i++){
        int tx=6+i*(SCREEN_W-16)/(num_frames>1?num_frames-1:1);
        if(i==tf) fcircle(fb,tx,ty+3,3,COL_RED);
        else bline(fb,tx,ty+1,tx,ty+5,1,C15(14,14,14));
    }
    if(is_playing) fcircle(fb,SCREEN_W-5,ty+3,3,COL_GREEN);
    char b[8];sprintf(b,"F%d",tf+1);dstr(fb,2,ty-1,b,C15(14,14,14));
}

static void draw_timeline_sub(u16*fb,int tf){
    int ty=ROW1_Y-2;
    frect(fb,0,ty,SCREEN_W-1,ty,C15(4,4,5));
    if(num_frames>1) for(int i=0;i<num_frames;i++){
        int tx=4+i*(SCREEN_W-8)/(num_frames-1);
        if(i==tf) fcircle(fb,tx,ty,2,palette[cur_color]);
        else pset(fb,tx,ty,C15(16,16,16));
    }
}

/* ═══════════════════════════════════════════════════════════════
   Render: EDIT MODE
   ═══════════════════════════════════════════════════════════════ */
static void render_edit(int sel_obj){
    int sf=is_playing?play_frame:cur_frame;

    /* Top: preview */
    for(int i=0;i<SCREEN_W*SCREEN_H;i++) bbMain[i]=COL_WHITE;
    for(int gy=20;gy<SCREEN_H-12;gy+=20) for(int gx=20;gx<SCREEN_W;gx+=20) pset(bbMain,gx,gy,COL_GRID);
    for(int o=0;o<frames[sf].num_objs;o++) draw_obj(bbMain,&frames[sf].objs[o],false,false,cam_x,cam_y);
    draw_timeline_main(bbMain,sf);

    /* Bottom: editor */
    for(int y=0;y<CANVAS_H;y++) for(int x=0;x<SCREEN_W;x++)
        bbSub[y*SCREEN_W+x]=(x%32==0||y%32==0)?COL_GRID:COL_WHITE;
    if(cur_frame>0) for(int o=0;o<frames[cur_frame-1].num_objs;o++)
        draw_obj(bbSub,&frames[cur_frame-1].objs[o],false,true,cam_x,cam_y);
    for(int o=0;o<frames[cur_frame].num_objs;o++)
        draw_obj(bbSub,&frames[cur_frame].objs[o],true,false,cam_x,cam_y);

    /* Selection box */
    if(sel_obj>=0){
        const Object*ob=&frames[cur_frame].objs[sel_obj];
        int x0=9999,y0=9999,x1=-9999,y1=-9999;
        for(int i=0;i<ob->num_joints;i++){
            int jx=ob->joints[i].x-cam_x,jy=ob->joints[i].y-cam_y;
            if(jx<x0)x0=jx;if(jx>x1)x1=jx;if(jy<y0)y0=jy;if(jy>y1)y1=jy;
        }
        x0-=10;y0-=10;x1+=10;y1+=10;
        if(x0<0)x0=0;if(y0<0)y0=0;if(x1>=SCREEN_W)x1=SCREEN_W-1;if(y1>=CANVAS_H)y1=CANVAS_H-1;
        if(x1>x0&&y1>y0) hrect(bbSub,x0,y0,x1,y1,COL_SEL);
    }

    /* UI strip */
    frect(bbSub,0,ROW0_Y,SCREEN_W-1,SCREEN_H-1,COL_DGRAY);
    draw_timeline_sub(bbSub,cur_frame);
    draw_btn(bbSub,0,0,"PREV",false); draw_btn(bbSub,1,0,"NEXT",false);
    draw_btn(bbSub,2,0,"+FRM",false); draw_btn(bbSub,3,0,"-FRM",false);
    draw_btn(bbSub,4,0,"UNDO",undo_count>0); draw_btn(bbSub,5,0,"CLR",false);
    draw_btn(bbSub,0,1,is_playing?"STOP":"PLAY",is_playing);
    {char b[6];sprintf(b,"%dFPS",fps);draw_btn(bbSub,1,1,b,false);}
    draw_btn(bbSub,2,1,pal_name[cur_color],sel_obj>=0);
    fcircle(bbSub,2*BTN_W+BTN_W/2,ROW1_Y+BTN_H-6,4,palette[cur_color]);
    /* shape selector label */
    {
        const char*sn;char cb[10];
        if(cur_shape==0)sn="STKM";
        else if(cur_shape==1)sn="BALL";
        else{int idx=cur_shape-2;if(idx<num_shapes&&lib[idx].used){sprintf(cb,"%.4s",lib[idx].name);sn=cb;}else sn="STKM";}
        draw_btn(bbSub,3,1,sn,false);
    }
    draw_btn(bbSub,4,1,"+OBJ",false);
    draw_btn(bbSub,5,1,"SHPE",false);
    {char b[12];sprintf(b,"F%d/%d",cur_frame+1,num_frames);frect(bbSub,0,ROW0_Y-10,50,ROW0_Y-1,COL_BLACK);dstr(bbSub,2,ROW0_Y-9,b,COL_GRAY);}
    draw_notif(bbSub);
}

/* ═══════════════════════════════════════════════════════════════
   Render: SHAPE MENU
   Shape info text on TOP screen, touch slots on BOTTOM screen
   ═══════════════════════════════════════════════════════════════ */
/* Slot grid: 3 columns × 5 rows = 15 slots, big enough to tap comfortably */
#define SLOT_COLS   3
#define SLOT_ROWS   5
#define SLOT_W     84    /* 3*84=252, fits in 256 */
#define SLOT_H     36    /* 5*36=180, fits in 192 with 12px header */

/* Draw a mini thumbnail of a shape into a bounding box */
static void draw_shape_thumb(u16*fb, CustomShape*cs, int bx0, int by0, int bx1, int by1, u16 col){
    if(cs->num_ink<2) return;
    int minx=9999,miny=9999,maxx=-9999,maxy=-9999;
    for(int i=0;i<cs->num_ink;i++){
        int x=(int)cs->ink[i].x, y=(int)cs->ink[i].y;
        if(x<minx)minx=x; if(x>maxx)maxx=x;
        if(y<miny)miny=y; if(y>maxy)maxy=y;
    }
    int pw=maxx-minx+1, ph=maxy-miny+1; if(pw<1)pw=1; if(ph<1)ph=1;
    int tw=bx1-bx0-4, th=by1-by0-4;
    /* uniform scale, keep aspect */
    int scale_num=1, scale_den=1;
    if(pw>tw||ph>th){
        /* fit within box */
        scale_num=1;
        scale_den=(pw>ph)?(pw+tw-1)/tw:(ph+th-1)/th;
        if(scale_den<1)scale_den=1;
    }
    int cx=(bx0+bx1)/2, cy=(by0+by1)/2;
    for(int i=1;i<cs->num_ink;i++){
        int ax=(((int)cs->ink[i-1].x-minx-pw/2)*scale_num/scale_den)+cx;
        int ay=(((int)cs->ink[i-1].y-miny-ph/2)*scale_num/scale_den)+cy;
        int bxx=(((int)cs->ink[i].x  -minx-pw/2)*scale_num/scale_den)+cx;
        int byy=(((int)cs->ink[i].y  -miny-ph/2)*scale_num/scale_den)+cy;
        /* clip to slot */
        if(ax>=bx0&&ax<=bx1&&bxx>=bx0&&bxx<=bx1&&ay>=by0&&ay<=by1&&byy>=by0&&byy<=by1)
            bline(fb,ax,ay,bxx,byy,1,col);
    }
}

static void render_shape_menu(void){
    /* ══ TOP SCREEN: selected shape info + big preview ══ */
    frect(bbMain,0,0,SCREEN_W-1,SCREEN_H-1,C15(3,3,6));
    dstrc(bbMain,128,5,"SHAPE LIBRARY",COL_WHITE);
    bline(bbMain,0,15,SCREEN_W-1,15,1,C15(12,12,15));

    if(shape_sel>=0&&shape_sel<MAX_SHAPES&&lib[shape_sel].used){
        CustomShape*cs=&lib[shape_sel];
        /* Name */
        dstrc(bbMain,128,22,cs->name,C15(20,26,31));
        char b[32];
        sprintf(b,"%d JOINTS",cs->num_joints);
        dstrc(bbMain,128,33,b,COL_GRAY);
        /* Big preview box */
        frect(bbMain,20,44,236,154,C15(6,6,9));
        hrect(bbMain,20,44,236,154,COL_SEL);
        if(cs->num_ink>0) draw_shape_thumb(bbMain,cs,22,46,234,152,palette[cur_color]);
        else dstrc(bbMain,128,95,"(NO INK — JOINTS ONLY)",COL_GRAY);
        /* Joint dots over preview */
        if(cs->num_joints>0&&cs->num_ink>0){
            int minx=9999,miny=9999,maxx=-9999,maxy=-9999;
            for(int i=0;i<cs->num_ink;i++){
                int x=(int)cs->ink[i].x,y=(int)cs->ink[i].y;
                if(x<minx)minx=x;if(x>maxx)maxx=x;if(y<miny)miny=y;if(y>maxy)maxy=y;
            }
            int pw=maxx-minx+1,ph=maxy-miny+1;if(pw<1)pw=1;if(ph<1)ph=1;
            int tw=212,th=106,scale_den=(pw>ph)?(pw+tw-1)/tw:(ph+th-1)/th;
            if(scale_den<1)scale_den=1;
            for(int i=0;i<cs->num_joints;i++){
                int jx=(cs->rel_x[i]-pw/2)/scale_den+128;
                int jy=(cs->rel_y[i]-ph/2)/scale_den+99;
                if(jx>22&&jx<234&&jy>46&&jy<152) fcircle(bbMain,jx,jy,2,COL_HANDLE);
            }
        }
        /* Actions */
        bline(bbMain,0,160,SCREEN_W-1,160,1,C15(10,10,12));
        dstrc(bbMain,128,165,"A OR TAP TWICE = ADD TO FRAME",COL_WHITE);
        dstrc(bbMain,128,175,"Y=DELETE  X=NEW SHAPE  B=BACK",COL_GRAY);
    } else {
        dstrc(bbMain,128,40,"SELECT A SHAPE BELOW",COL_GRAY);
        dstrc(bbMain,128,60,"OR CREATE A NEW ONE",COL_GRAY);
        bline(bbMain,0,160,SCREEN_W-1,160,1,C15(10,10,12));
        dstrc(bbMain,128,168,"X = DRAW NEW SHAPE",COL_WHITE);
        dstrc(bbMain,128,179,"B = BACK TO EDITOR",COL_GRAY);
    }

    /* ══ BOTTOM SCREEN: big tappable slot grid ══ */
    frect(bbSub,0,0,SCREEN_W-1,SCREEN_H-1,C15(5,5,7));

    /* Header row with action buttons */
    frect(bbSub,0,0,SCREEN_W-1,11,C15(3,3,5));
    dstr(bbSub,2,3,"NEW",COL_WHITE);              /* left: new */
    dstrc(bbSub,128,3,"TAP TO SELECT",COL_GRAY);  /* center hint */
    dstr(bbSub,SCREEN_W-20,3,"BCK",COL_GRAY);     /* right: back hint */

    for(int i=0;i<MAX_SHAPES;i++){
        int ci=i%SLOT_COLS, ri=i/SLOT_COLS;
        int x0=1+ci*SLOT_W, y0=12+ri*SLOT_H;
        int x1=x0+SLOT_W-2, y1=y0+SLOT_H-2;
        bool sel=(i==shape_sel);
        bool used=(lib[i].used);
        u16 bg=sel?C15(4,12,20):(used?C15(7,7,10):C15(4,4,5));
        u16 border=sel?COL_SEL:(used?C15(16,16,20):C15(8,8,9));
        frect(bbSub,x0,y0,x1,y1,bg);
        hrect(bbSub,x0,y0,x1,y1,border);
        if(used){
            /* Mini thumbnail */
            draw_shape_thumb(bbSub,&lib[i],x0+2,y0+2,x1-2,y1-14,
                             sel?C15(12,20,31):C15(16,16,20));
            /* Name at bottom of slot */
            frect(bbSub,x0+1,y1-12,x1-1,y1-1,sel?C15(3,9,16):C15(5,5,7));
            dstrc(bbSub,(x0+x1)/2,y1-11,lib[i].name,sel?COL_WHITE:C15(20,20,22));
        } else {
            /* Empty slot: show + and slot number */
            dstrc(bbSub,(x0+x1)/2,(y0+y1)/2-7,"+",C15(14,14,16));
            char nb[4]; sprintf(nb,"%d",i+1);
            dstrc(bbSub,(x0+x1)/2,(y0+y1)/2+3,nb,C15(10,10,11));
        }
    }
}

/* ═══════════════════════════════════════════════════════════════
   Render: SHAPE DRAW INK
   ═══════════════════════════════════════════════════════════════ */
static void render_shape_draw_ink(void){
    /* Top: instructions */
    frect(bbMain,0,0,SCREEN_W-1,SCREEN_H-1,C15(3,3,6));
    dstrc(bbMain,128,8,"STEP 1: DRAW YOUR SHAPE",COL_WHITE);
    bline(bbMain,0,18,SCREEN_W-1,18,1,C15(10,10,13));
    dstrc(bbMain,128,26,"DRAW ON THE BOTTOM SCREEN",COL_GRAY);
    dstrc(bbMain,128,38,"LIFT STYLUS = NEW STROKE",COL_GRAY);
    dstrc(bbMain,128,50,"(DRAW COMPLEX SHAPES THIS WAY)",COL_GRAY);
    bline(bbMain,0,SCREEN_H-48,SCREEN_W-1,SCREEN_H-48,1,C15(10,10,13));
    {char b[24];int actual=draft_ink_n;
     /* count non-sentinel */
     for(int i=0;i<draft_ink_n;i++) if(draft_ink[i].x==INK_LIFT)actual--;
     sprintf(b,"POINTS: %d",actual);
     dstrc(bbMain,128,SCREEN_H-40,b,COL_GRAY);}
    dstrc(bbMain,128,SCREEN_H-28,"SELECT = CLEAR DRAWING",COL_GRAY);
    dstrc(bbMain,128,SCREEN_H-16,"B = NEXT: PLACE JOINTS",COL_WHITE);

    /* Bottom: drawing canvas */
    frect(bbSub,0,0,SCREEN_W-1,SCREEN_H-1,COL_WHITE);
    for(int gy=0;gy<SCREEN_H;gy+=32) for(int gx=0;gx<SCREEN_W;gx+=32) pset(bbSub,gx,gy,COL_GRID);
    if(draft_ink_n>0) draw_ink(bbSub,draft_ink,draft_ink_n,palette[cur_color],0,0);
    if(draft_ink_n==0) dstrc(bbSub,128,SCREEN_H/2-3,"DRAW HERE",COL_GRID);
}

/* ═══════════════════════════════════════════════════════════════
   Render: SHAPE PLACE JOINTS
   ═══════════════════════════════════════════════════════════════ */
static void render_shape_place_jnt(void){
    /* Top: instructions */
    frect(bbMain,0,0,SCREEN_W-1,SCREEN_H-1,C15(4,4,6));
    dstrc(bbMain,128,8,"STEP 2: PLACE JOINTS",COL_WHITE);
    bline(bbMain,0,18,SCREEN_W-1,18,1,COL_GRAY);
    dstrc(bbMain,128,26,"TAP TO ADD ANIMATION JOINTS",COL_GRAY);
    dstrc(bbMain,128,38,"JOINTS CONNECT IN ORDER",COL_GRAY);
    dstrc(bbMain,128,50,"DRAG EXISTING JOINTS TO MOVE",COL_GRAY);
    bline(bbMain,0,SCREEN_H-46,SCREEN_W-1,SCREEN_H-46,1,COL_GRAY);
    {char b[24];sprintf(b,"JOINTS: %d (NO LIMIT)",draft_jn);dstrc(bbMain,128,SCREEN_H-38,b,COL_GRAY);}
    dstrc(bbMain,128,SCREEN_H-26,"Y=REMOVE LAST  SELECT=CLEAR",COL_GRAY);
    dstrc(bbMain,128,SCREEN_H-14,"B=BACK  A=DONE & NAME",COL_WHITE);

    /* Bottom: canvas with ink + joints */
    frect(bbSub,0,0,SCREEN_W-1,SCREEN_H-1,COL_WHITE);
    for(int gy=0;gy<SCREEN_H;gy+=32) for(int gx=0;gx<SCREEN_W;gx+=32) pset(bbSub,gx,gy,COL_GRID);
    /* draw ink as ghost */
    if(draft_ink_n>0) draw_ink(bbSub,draft_ink,draft_ink_n,C15(20,20,22),0,0);
    /* draw joints */
    for(int i=0;i<draft_jn;i++){
        if(i>0){
            int p=draft_joints[i].parent;
            bline(bbSub,draft_joints[i].x,draft_joints[i].y,draft_joints[p].x,draft_joints[p].y,1,C15(0,14,28));
        }
        fcircle(bbSub,draft_joints[i].x,draft_joints[i].y,i==draft_jn-1?5:4,i==0?COL_SEL:COL_HANDLE);
        char nb[4];sprintf(nb,"%d",i);
        dstr(bbSub,draft_joints[i].x+6,draft_joints[i].y-4,nb,COL_BLACK);
    }
    if(draft_jn==0) dstrc(bbSub,128,90,"TAP TO PLACE FIRST JOINT",COL_GRAY);
}

/* ═══════════════════════════════════════════════════════════════
   Render: SHAPE NAME
   ═══════════════════════════════════════════════════════════════ */
static void render_shape_name(void){
    frect(bbMain,0,0,SCREEN_W-1,SCREEN_H-1,C15(4,4,6));
    dstrc(bbMain,128,16,"STEP 3: NAME YOUR SHAPE",COL_WHITE);
    dstrc(bbMain,128,30,"TYPE A NAME (MAX 8 CHARS)",COL_GRAY);
    frect(bbMain,40,48,216,66,C15(2,2,4));hrect(bbMain,40,48,216,66,COL_SEL);
    dstrc(bbMain,128,54,name_len?name_buf:"...",COL_WHITE);
    bline(bbMain,0,SCREEN_H-24,SCREEN_W-1,SCREEN_H-24,1,COL_GRAY);
    dstrc(bbMain,128,SCREEN_H-16,"A OR OK = SAVE   B = CANCEL",COL_GRAY);

    /* Bottom: keyboard */
    frect(bbSub,0,0,SCREEN_W-1,SCREEN_H-1,COL_DGRAY);
    dstrc(bbSub,128,3,"TAP LETTERS TO TYPE",COL_GRAY);
    int kw=24,kh=40;
    for(int r=0;r<KBD_ROWS;r++){
        const char*row=kbd_rows[r]; int nc=sl(row);
        for(int c=0;c<nc;c++){
            int kx=2+c*kw,ky=12+r*kh;
            frect(bbSub,kx,ky,kx+kw-2,ky+kh-4,C15(10,10,13));
            hrect(bbSub,kx,ky,kx+kw-2,ky+kh-4,COL_GRAY);
            char k[3]={row[c],0,0};
            if(row[c]==' '){k[0]='S';k[1]='P';k[2]=0;}
            dstrc(bbSub,kx+(kw-2)/2,ky+(kh-4)/2-3,k,COL_WHITE);
        }
    }
    /* current name bottom */
    frect(bbSub,0,SCREEN_H-12,SCREEN_W-1,SCREEN_H-1,COL_BLACK);
    dstrc(bbSub,128,SCREEN_H-10,name_len?name_buf:"",COL_WHITE);
}

/* ═══════════════════════════════════════════════════════════════
   Save draft → library
   ═══════════════════════════════════════════════════════════════ */
static void save_draft_to_lib(void){
    if(name_len==0){memcpy(name_buf,"SHAPE",6);name_len=5;}
    /* find free slot */
    int slot=-1;
    for(int i=0;i<MAX_SHAPES;i++) if(!lib[i].used){slot=i;break;}
    if(slot<0){notif("LIBRARY FULL");ui_mode=MODE_EDIT;return;}

    lib_free(slot);
    CustomShape*cs=&lib[slot];
    cs->used=true;
    strncpy(cs->name,name_buf,SHAPE_NAME_LEN-1);
    cs->name[SHAPE_NAME_LEN-1]=0;

    /* ink */
    cs->num_ink=draft_ink_n;
    if(draft_ink_n>0){
        cs->ink=(Pt*)xmalloc(draft_ink_n*sizeof(Pt));
        memcpy(cs->ink,draft_ink,draft_ink_n*sizeof(Pt));
    }

    /* joints relative to joint[0] */
    cs->num_joints=draft_jn;
    if(draft_jn>0){
        cs->rel_x=(int*)xmalloc(draft_jn*sizeof(int));
        cs->rel_y=(int*)xmalloc(draft_jn*sizeof(int));
        cs->parent=(int*)xmalloc(draft_jn*sizeof(int));
        cs->length=(int*)xmalloc(draft_jn*sizeof(int));
        int rx=draft_joints[0].x, ry=draft_joints[0].y;
        for(int i=0;i<draft_jn;i++){
            cs->rel_x[i]=draft_joints[i].x-rx;
            cs->rel_y[i]=draft_joints[i].y-ry;
            cs->parent[i]=draft_joints[i].parent;
            cs->length[i]=draft_joints[i].length;
        }
    }

    if(slot>=num_shapes) num_shapes=slot+1;
    notif("SHAPE SAVED!");
    cur_shape=2+slot;
    draft_reset();
    ui_mode=MODE_EDIT;
}

/* ═══════════════════════════════════════════════════════════════
   Main
   ═══════════════════════════════════════════════════════════════ */
int main(void){
    videoSetMode(MODE_5_2D); vramSetBankA(VRAM_A_MAIN_BG);
    int bgM=bgInit(3,BgType_Bmp16,BgSize_B16_256x256,0,0);
    videoSetModeSub(MODE_5_2D); vramSetBankC(VRAM_C_SUB_BG);
    int bgS=bgInitSub(3,BgType_Bmp16,BgSize_B16_256x256,0,0);
    vramMain=bgGetGfxPtr(bgM); vramSub=bgGetGfxPtr(bgS);

    memset(lib,0,sizeof(lib));
    init_all_frames();

    int  sel_obj=-1,sel_joint=-1,last_tx=0,last_ty=0;
    bool touch_ui=false;

    while(1){
        scanKeys();
        int keys=keysDown(),held=keysHeld();
        touchPosition tp; touchRead(&tp);
        if(notif_timer>0) notif_timer--;
        play_timer++;

        /* ══════════════════════════════════════════════════════
           MODE: SHAPE NAME
           ══════════════════════════════════════════════════════ */
        if(ui_mode==MODE_SHAPE_NAME){
            if(keys&KEY_B){draft_reset();ui_mode=MODE_EDIT;}
            if(keys&KEY_A){save_draft_to_lib();}
            if(keys&KEY_TOUCH){
                /* keyboard on bottom screen */
                int kw=24,kh=40;
                int r=(tp.py-12)/kh, c=tp.px/kw;
                if(r>=0&&r<KBD_ROWS&&c>=0){
                    const char*row=kbd_rows[r]; int nc=sl(row);
                    if(c<nc){
                        char ch=row[c];
                        if(ch=='<'){if(name_len>0){name_len--;name_buf[name_len]=0;}}
                        else if(ch=='O'&&r==3&&c==9){save_draft_to_lib();}  /* OK */
                        else if(ch==' '){if(name_len<SHAPE_NAME_LEN-1){name_buf[name_len++]='_';name_buf[name_len]=0;}}
                        else{if(name_len<SHAPE_NAME_LEN-1){name_buf[name_len++]=ch;name_buf[name_len]=0;}}
                    }
                }
            }
            render_shape_name();flush();continue;
        }

        /* ══════════════════════════════════════════════════════
           MODE: SHAPE PLACE JOINTS
           ══════════════════════════════════════════════════════ */
        if(ui_mode==MODE_SHAPE_PLACE_JNT){
            if(keys&KEY_B){ui_mode=MODE_SHAPE_DRAW_INK;}
            if(keys&KEY_A&&draft_jn>=1){
                /* done — go to name */
                memset(name_buf,0,sizeof(name_buf)); name_len=0;
                ui_mode=MODE_SHAPE_NAME;
            }
            if(keys&KEY_Y&&draft_jn>0){
                draft_jn--;
                notif("JOINT REMOVED");
            }
            if(keys&KEY_SELECT){
                /* clear joints */
                draft_jn=0; draft_sel_j=-1;
                notif("JOINTS CLEARED");
            }

            if(keys&KEY_TOUCH){
                /* Check if tapping near existing joint to drag */
                int best=15*15; draft_sel_j=-1;
                for(int i=0;i<draft_jn;i++){
                    int dx=tp.px-draft_joints[i].x, dy=tp.py-draft_joints[i].y;
                    int d2=dx*dx+dy*dy;
                    if(d2<best){best=d2;draft_sel_j=i;}
                }
                if(draft_sel_j<0){
                    /* add new joint */
                    draft_add_joint(tp.px,tp.py);
                }
            }
            if((held&KEY_TOUCH)&&draft_sel_j>=0){
                /* drag joint */
                draft_joints[draft_sel_j].x=tp.px;
                draft_joints[draft_sel_j].y=tp.py;
                /* update length from parent */
                int p=draft_joints[draft_sel_j].parent;
                if(p>=0){
                    int dx=tp.px-draft_joints[p].x, dy=tp.py-draft_joints[p].y;
                    draft_joints[draft_sel_j].length=(int)sqrtf((float)(dx*dx+dy*dy));
                    if(draft_joints[draft_sel_j].length<1) draft_joints[draft_sel_j].length=1;
                }
            }
            if(!(held&KEY_TOUCH)) draft_sel_j=-1;

            render_shape_place_jnt();flush();continue;
        }

        /* ══════════════════════════════════════════════════════
           MODE: SHAPE DRAW INK
           ══════════════════════════════════════════════════════ */
        if(ui_mode==MODE_SHAPE_DRAW_INK){
            if(keys&KEY_SELECT){draft_ink_n=0;ink_drawing=false;notif("DRAWING CLEARED");}
            if(keys&KEY_B){
                /* go to joint placement */
                draft_jn=0; draft_sel_j=-1; ink_drawing=false;
                ui_mode=MODE_SHAPE_PLACE_JNT;
            }
            /* Free-draw with stylus: throttle to ~4px movement to avoid huge arrays */
            if(held&KEY_TOUCH){
                bool add=false;
                if(!ink_drawing){ add=true; ink_drawing=true; }
                else if(draft_ink_n>0){
                    int lx=(int)draft_ink[draft_ink_n-1].x, ly=(int)draft_ink[draft_ink_n-1].y;
                    if(lx==INK_LIFT){ add=true; } /* after lift, always add */
                    else {
                        int dx=tp.px-lx, dy=tp.py-ly;
                        if(dx*dx+dy*dy>=16) add=true; /* 4px threshold */
                    }
                }
                if(add) draft_add_ink(tp.px,tp.py);
            } else {
                if(ink_drawing){ draft_lift_pen(); } /* insert lift sentinel on pen-up */
                ink_drawing=false;
            }
            render_shape_draw_ink();flush();continue;
        }

        /* ══════════════════════════════════════════════════════
           MODE: SHAPE MENU
           ══════════════════════════════════════════════════════ */
        if(ui_mode==MODE_SHAPE_MENU){
            if(keys&KEY_B){ui_mode=MODE_EDIT;}
            if(keys&KEY_X){
                /* start new shape draw */
                draft_reset();
                ui_mode=MODE_SHAPE_DRAW_INK;
            }
            if(keys&KEY_A){
                if(shape_sel>=0&&shape_sel<MAX_SHAPES&&lib[shape_sel].used){
                    if(frames[cur_frame].num_objs<MAX_OBJS){
                        obj_init_custom(&frames[cur_frame].objs[frames[cur_frame].num_objs],
                            shape_sel,128+cam_x,90+cam_y,palette[cur_color]);
                        frames[cur_frame].num_objs++;
                        cur_shape=2+shape_sel;
                        notif("OBJ ADDED");
                        ui_mode=MODE_EDIT;
                    }else notif("MAX OBJS!");
                }else notif("SELECT A SHAPE");
            }
            if(keys&KEY_Y){
                if(shape_sel>=0&&lib[shape_sel].used){
                    lib_free(shape_sel);notif("DELETED");shape_sel=-1;
                }
            }
            /* Touch: slots on bottom screen or header buttons */
            if(keys&KEY_TOUCH){
                if(tp.py<12){
                    /* header row: NEW on left, BACK on right */
                    if(tp.px<30){ draft_reset(); ui_mode=MODE_SHAPE_DRAW_INK; }
                    else if(tp.px>SCREEN_W-30){ ui_mode=MODE_EDIT; }
                } else {
                    int ci=tp.px/SLOT_W, ri=(tp.py-12)/SLOT_H;
                    if(ri>=0&&ri<SLOT_ROWS&&ci>=0&&ci<SLOT_COLS){
                        int idx=ri*SLOT_COLS+ci;
                        if(idx<MAX_SHAPES){
                            if(shape_sel==idx&&lib[idx].used){
                                /* second tap on same used slot = add immediately */
                                if(frames[cur_frame].num_objs<MAX_OBJS){
                                    obj_init_custom(&frames[cur_frame].objs[frames[cur_frame].num_objs],
                                        idx,128+cam_x,90+cam_y,palette[cur_color]);
                                    frames[cur_frame].num_objs++;
                                    cur_shape=2+idx; notif("OBJ ADDED"); ui_mode=MODE_EDIT;
                                } else notif("MAX OBJS!");
                            } else {
                                /* first tap: just select (top screen shows preview) */
                                shape_sel=idx;
                            }
                        }
                    }
                }
            }
            render_shape_menu();draw_notif(bbMain);flush();continue;
        }

        /* ══════════════════════════════════════════════════════
           MODE: EDIT
           ══════════════════════════════════════════════════════ */
        if(is_playing&&num_frames>1&&play_timer>=(60/fps)){
            play_timer=0; play_frame=(play_frame+1)%num_frames;
        }

        /* Hardware buttons */
        if(keys&KEY_START){is_playing=!is_playing;if(is_playing){play_frame=cur_frame;play_timer=0;}}
        if(!is_playing){
            if(keys&KEY_L&&cur_frame>0) cur_frame--;
            if(keys&KEY_R&&cur_frame<num_frames-1) cur_frame++;
        }
        if(keys&KEY_SELECT&&num_frames<MAX_FRAMES){
            /* duplicate frame — deep copy joints */
            for(int f=num_frames;f>cur_frame+1;f--){
                frame_free_joints(&frames[f]);
                frames[f].num_objs=frames[f-1].num_objs;
                for(int o=0;o<frames[f-1].num_objs;o++){
                    frames[f].objs[o]=frames[f-1].objs[o];
                    frames[f].objs[o].joints=joints_clone(frames[f-1].objs[o].joints,frames[f-1].objs[o].num_joints);
                }
            }
            frame_free_joints(&frames[cur_frame+1]);
            frames[cur_frame+1].num_objs=frames[cur_frame].num_objs;
            for(int o=0;o<frames[cur_frame].num_objs;o++){
                frames[cur_frame+1].objs[o]=frames[cur_frame].objs[o];
                frames[cur_frame+1].objs[o].joints=joints_clone(frames[cur_frame].objs[o].joints,frames[cur_frame].objs[o].num_joints);
            }
            cur_frame++; num_frames++; notif("FRAME ADDED");
        }

        /* D-pad nudge */
        if(sel_obj>=0&&sel_joint>=0){
            int ddx=(held&KEY_RIGHT)?1:(held&KEY_LEFT)?-1:0;
            int ddy=(held&KEY_DOWN)?1:(held&KEY_UP)?-1:0;
            if(ddx||ddy){
                Object*ob=&frames[cur_frame].objs[sel_obj];
                if(sel_joint==0){move_tree(ob,0,ddx,ddy);}
                else{
                    int p=ob->joints[sel_joint].parent;
                    int nx=ob->joints[sel_joint].x+ddx,ny=ob->joints[sel_joint].y+ddy;
                    float d=sqrtf((float)((nx-ob->joints[p].x)*(nx-ob->joints[p].x)+(ny-ob->joints[p].y)*(ny-ob->joints[p].y)));
                    if(d>0.1f){
                        int l=ob->joints[sel_joint].length;
                        move_tree(ob,sel_joint,
                            (int)(ob->joints[p].x+(nx-ob->joints[p].x)*l/d)-ob->joints[sel_joint].x,
                            (int)(ob->joints[p].y+(ny-ob->joints[p].y)*l/d)-ob->joints[sel_joint].y);
                    }
                }
            }
        }

        /* Touch: press */
        if(keys&KEY_TOUCH){
            touch_ui=(tp.py>=CANVAS_H);
            if(touch_ui){
                int row=(tp.py>=ROW1_Y)?1:0, btn=tp.px/BTN_W; if(btn>5)btn=5;
                if(row==0){
                    if(btn==0&&cur_frame>0) cur_frame--;
                    else if(btn==1&&cur_frame<num_frames-1) cur_frame++;
                    else if(btn==2&&num_frames<MAX_FRAMES){
                        /* duplicate */
                        for(int f=num_frames;f>cur_frame+1;f--){
                            frame_free_joints(&frames[f]);
                            frames[f].num_objs=frames[f-1].num_objs;
                            for(int o=0;o<frames[f-1].num_objs;o++){
                                frames[f].objs[o]=frames[f-1].objs[o];
                                frames[f].objs[o].joints=joints_clone(frames[f-1].objs[o].joints,frames[f-1].objs[o].num_joints);
                            }
                        }
                        frame_free_joints(&frames[cur_frame+1]);
                        frames[cur_frame+1].num_objs=frames[cur_frame].num_objs;
                        for(int o=0;o<frames[cur_frame].num_objs;o++){
                            frames[cur_frame+1].objs[o]=frames[cur_frame].objs[o];
                            frames[cur_frame+1].objs[o].joints=joints_clone(frames[cur_frame].objs[o].joints,frames[cur_frame].objs[o].num_joints);
                        }
                        cur_frame++;num_frames++;notif("FRAME ADDED");
                    }
                    else if(btn==3&&num_frames>1){
                        frame_free_joints(&frames[cur_frame]);
                        for(int f=cur_frame;f<num_frames-1;f++){
                            frames[f]=frames[f+1];
                        }
                        num_frames--;if(cur_frame>=num_frames)cur_frame=num_frames-1;
                        notif("FRAME DELETED");
                    }
                    else if(btn==4) pop_undo();
                    else if(btn==5){init_all_frames();sel_obj=-1;notif("CLEARED");}
                }else{
                    if(btn==0){is_playing=!is_playing;if(is_playing){play_frame=cur_frame;play_timer=0;}}
                    else if(btn==1){fps=(fps>=30)?4:fps+4;}
                    else if(btn==2){
                        cur_color=(cur_color+1)%NUM_COLORS;
                        if(sel_obj>=0) frames[cur_frame].objs[sel_obj].color=palette[cur_color];
                    }
                    else if(btn==3){
                        cur_shape++;
                        int ms=2+num_shapes; if(cur_shape>=ms)cur_shape=0;
                    }
                    else if(btn==4){
                        if(frames[cur_frame].num_objs<MAX_OBJS){
                            Object*no=&frames[cur_frame].objs[frames[cur_frame].num_objs];
                            if(cur_shape==0)      obj_init_stickman(no,128+cam_x,96+cam_y,palette[cur_color]);
                            else if(cur_shape==1) obj_init_ball(no,128+cam_x,96+cam_y,palette[cur_color]);
                            else{int idx=cur_shape-2;if(idx<num_shapes&&lib[idx].used)obj_init_custom(no,idx,128+cam_x,96+cam_y,palette[cur_color]);else obj_init_stickman(no,128+cam_x,96+cam_y,palette[cur_color]);}
                            frames[cur_frame].num_objs++;notif("OBJ ADDED");
                        }else notif("MAX OBJS!");
                    }
                    else if(btn==5){shape_sel=-1;ui_mode=MODE_SHAPE_MENU;}
                }
            }else{
                /* canvas — pick joint */
                sel_obj=-1;sel_joint=-1;
                int best=20*20;
                for(int o=0;o<frames[cur_frame].num_objs;o++){
                    Object*ob=&frames[cur_frame].objs[o];
                    for(int i=0;i<ob->num_joints;i++){
                        int dx=(tp.px+cam_x)-ob->joints[i].x,dy=(tp.py+cam_y)-ob->joints[i].y;
                        int d2=dx*dx+dy*dy;
                        if(d2<best){best=d2;sel_obj=o;sel_joint=i;}
                    }
                }
                if(sel_obj>=0){
                    push_undo();
                    u16 oc=frames[cur_frame].objs[sel_obj].color;
                    for(int c=0;c<NUM_COLORS;c++) if(palette[c]==oc){cur_color=c;break;}
                }else{last_tx=tp.px;last_ty=tp.py;}
            }
        }
        /* Touch: held */
        if((held&KEY_TOUCH)&&!touch_ui&&tp.py<CANVAS_H){
            if(sel_obj>=0){
                Object*ob=&frames[cur_frame].objs[sel_obj];
                int wx=tp.px+cam_x,wy=tp.py+cam_y;
                if(sel_joint==0){move_tree(ob,0,wx-ob->joints[0].x,wy-ob->joints[0].y);}
                else{
                    int p=ob->joints[sel_joint].parent;
                    float d=sqrtf((float)((wx-ob->joints[p].x)*(wx-ob->joints[p].x)+(wy-ob->joints[p].y)*(wy-ob->joints[p].y)));
                    if(d>0.1f){
                        int l=ob->joints[sel_joint].length;
                        move_tree(ob,sel_joint,
                            (int)(ob->joints[p].x+(wx-ob->joints[p].x)*l/d)-ob->joints[sel_joint].x,
                            (int)(ob->joints[p].y+(wy-ob->joints[p].y)*l/d)-ob->joints[sel_joint].y);
                    }
                }
            }else{cam_x-=(tp.px-last_tx);cam_y-=(tp.py-last_ty);last_tx=tp.px;last_ty=tp.py;}
        }
        if(!(held&KEY_TOUCH)) sel_obj=sel_joint=-1;

        render_edit(sel_obj);
        flush();
    }
    return 0;
}
