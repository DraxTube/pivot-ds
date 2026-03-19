/*
 * PivotDS v6 — Nintendo DS Pivot Animator
 * Stable video: direct VRAM write at VBlank, no DMA copy glitch
 */

#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>

/* ─── screen dimensions ─────────────────────────────────────── */
#define SW   256
#define SH   192
#define CH   144   /* canvas height on sub screen */

/* ─── UI layout ─────────────────────────────────────────────── */
#define R0Y  144   /* row 0 top */
#define R1Y  168   /* row 1 top */
#define BW    42   /* button width */
#define BH    23   /* button height */

/* ─── limits ─────────────────────────────────────────────────── */
#define MAX_FR   200
#define MAX_OB    10
#define UNDO_N     8
#define MAX_SV    16
#define SNLEN      9   /* save name length including NUL */
#define MAGIC  0x50565436u  /* PVT6 */
#define SAVEDIR  "fat:/pivotds"
#define NOTIF_T   90

/* ─── colours ────────────────────────────────────────────────── */
/* BIT(15) must be set on every pixel for the DS to display it */
#define P(r,g,b) ((u16)(RGB15(r,g,b)|BIT(15)))
#define CWHITE   P(31,31,31)
#define CBLACK   P( 0, 0, 0)
#define CGRAY    P(20,20,20)
#define CDGRAY   P( 8, 8,10)
#define CRED     P(31, 0, 0)
#define CHANDLE  P(31,20, 0)
#define CSEL     P( 0,18,31)
#define CONION   P(15,15,26)
#define CGRID    P(27,27,29)
#define CGREEN   P( 0,22, 0)

/* ─── types ──────────────────────────────────────────────────── */
typedef struct { int x,y,parent,length; } Joint;

typedef struct {
    int    type;       /* 0=stickman 1=ball */
    u16    color;
    int    nj;
    Joint* j;          /* malloc */
} Obj;

typedef struct { Obj o[MAX_OB]; int n; } Frame;

typedef struct {
    int n;
    int nj[MAX_OB];
    Joint* j[MAX_OB];
    int type[MAX_OB];
    u16 color[MAX_OB];
} UndoSnap;

typedef struct {
    char name[SNLEN];
    char path[72];
    bool used;
} SaveEnt;

/* ─── palette ────────────────────────────────────────────────── */
static const u16 PAL[] = {
    P( 0, 0, 0), P(31, 0, 0), P( 0,22, 0), P( 0, 0,31),
    P(31,15, 0), P(18, 0,22), P( 0,18,18), P(31,31, 0),
    P(31,31,31), P(14,14,14),
};
static const char* PNAME[] = {
    "BLK","RED","GRN","BLU","ORG","PRP","TEL","YLW","WHT","GRY"
};
#define NPAL 10

/* ─── globals ────────────────────────────────────────────────── */
static Frame     fr[MAX_FR];
static int       nfr=1, cfr=0, pfr=0, ptim=0;
static bool      playing=false;
static int       fps=8;
static bool      onion=true;

static UndoSnap  ubuf[UNDO_N];
static int       utop=0, ucnt=0;

static SaveEnt   sv[MAX_SV];
static int       nsv=0, ssel=-1;
static bool      fat_ok=false;

static int       ccol=0, ctype=0;
static int       camx=0, camy=0;

/* background: freeze of top screen */
static u16*      bg=NULL;
static bool      bgon=false;

static char      notmsg[32]="";
static int       nottim=0;

static char      nbuf[SNLEN]="";
static int       nlen=0;

/* UI mode */
typedef enum { ED=0, SMENU, SNAME, BGMODE } Mode;
static Mode mode=ED;

/* keyboard */
static const char* KBD[]={
    "ABCDEFGHIJ",
    "KLMNOPQRST",
    "UVWXYZ0123",
    "456789 <OK"
};
#define KROWS 4

/* ─── VRAM pointers (set in main) ───────────────────────────── */
static u16* vmain;   /* points directly into VRAM for main screen  */
static u16* vsub;    /* points directly into VRAM for sub screen   */

/* ─── back-buffers (we draw here, then copy at VBlank) ──────── */
static u16 bm[SW*SH];   /* back-buffer main */
static u16 bs[SW*SH];   /* back-buffer sub  */

/* ═══════════════════════════════════════════════════════════════
   Flush: wait for VBlank then CPU-copy both buffers to VRAM.
   CPU copy is safe; DMA on these buffers caused glitches on
   real hardware because DMA doesn't honour BIT(15)/alpha.
   ═══════════════════════════════════════════════════════════════ */
static void flush(void){
    swiWaitForVBlank();
    /* 32-bit word copy — buffers are aligned to 4 bytes (static globals) */
    u32* src; u32* dst; int words = SW*SH/2;
    src=(u32*)bm; dst=(u32*)vmain; for(int i=0;i<words;i++) dst[i]=src[i];
    src=(u32*)bs;  dst=(u32*)vsub;  for(int i=0;i<words;i++) dst[i]=src[i];
}

/* ═══════════════════════════════════════════════════════════════
   Drawing — all to back-buffers only
   ═══════════════════════════════════════════════════════════════ */
static inline void px(u16* b,int x,int y,u16 c){
    if((unsigned)x<SW&&(unsigned)y<SH) b[y*SW+x]=c;
}
static void box(u16* b,int x0,int y0,int x1,int y1,u16 c){
    if(x0>x1){int t=x0;x0=x1;x1=t;}
    if(y0>y1){int t=y0;y0=y1;y1=t;}
    for(int y=y0;y<=y1;y++) for(int x=x0;x<=x1;x++) px(b,x,y,c);
}
static void circ(u16* b,int cx,int cy,int r,u16 c){
    int r2=r*r;
    for(int dy=-r;dy<=r;dy++) for(int dx=-r;dx<=r;dx++)
        if(dx*dx+dy*dy<=r2) px(b,cx+dx,cy+dy,c);
}
static void line(u16* b,int x0,int y0,int x1,int y1,int th,u16 c){
    int dx=abs(x1-x0),sx=x0<x1?1:-1;
    int dy=abs(y1-y0),sy=y0<y1?1:-1;
    int err=(dx>dy?dx:-dy)/2,e2;
    for(;;){
        circ(b,x0,y0,th,c);
        if(x0==x1&&y0==y1) break;
        e2=err;
        if(e2>-dx){err-=dy;x0+=sx;}
        if(e2< dy){err+=dx;y0+=sy;}
    }
}
static void rect(u16* b,int x0,int y0,int x1,int y1,u16 c){
    for(int x=x0;x<=x1;x++){px(b,x,y0,c);px(b,x,y1,c);}
    for(int y=y0;y<=y1;y++){px(b,x0,y,c);px(b,x1,y,c);}
}

/* ─── font 5×6 ───────────────────────────────────────────────── */
static const u8 FNT[][6]={
{0,0,0,0,0,0},{4,4,4,0,4,0},{10,10,0,0,0,0},{0,0,0,0,0,0},
{0,0,0,0,0,0},{0,0,0,0,0,0},{0,0,0,0,0,0},{2,2,0,0,0,0},
{2,4,4,4,2,0},{4,2,2,2,4,0},{0,10,4,10,0,0},{0,4,14,4,0,0},
{0,0,0,2,2,4},{0,0,14,0,0,0},{0,0,0,0,4,0},{8,4,4,2,1,0},
{14,17,17,17,14,0},{6,2,2,2,7,0},{14,1,6,8,15,0},{14,1,6,1,14,0},
{6,10,31,2,2,0},{31,16,30,1,30,0},{14,16,30,17,14,0},{31,1,2,4,4,0},
{14,17,14,17,14,0},{14,17,15,1,14,0},{0,4,0,4,0,0},{0,4,0,4,4,0},
{2,4,8,4,2,0},{0,14,0,14,0,0},{8,4,2,4,8,0},{14,1,6,0,4,0},
{14,17,23,16,14,0},{4,10,31,17,17,0},{30,17,30,17,30,0},
{14,17,16,17,14,0},{28,18,17,18,28,0},{31,16,28,16,31,0},
{31,16,28,16,16,0},{14,16,23,17,15,0},{17,17,31,17,17,0},
{14,4,4,4,14,0},{1,1,1,17,14,0},{17,18,28,18,17,0},
{16,16,16,16,31,0},{17,27,21,17,17,0},{17,25,21,19,17,0},
{14,17,17,17,14,0},{30,17,30,16,16,0},{14,17,17,19,15,0},
{30,17,30,18,17,0},{15,16,14,1,30,0},{31,4,4,4,4,0},
{17,17,17,17,14,0},{17,17,17,10,4,0},{17,17,21,27,17,0},
{17,10,4,10,17,0},{17,10,4,4,4,0},{31,2,4,8,31,0},
};
static void dc(u16* b,int px2,int py,char c,u16 col){
    if(c>='a'&&c<='z'){c-=32;}
    if(c<' '||c>'Z'){return;}
    const u8* g=FNT[(int)(c-' ')];
    for(int r=0;r<6;r++){
        u8 bits=g[r];
        for(int i=4;i>=0;i--){
            if(bits&(1<<i)) px(b,px2+(4-i),py+r,col);
        }
    }
}
static void ds(u16* b,int x,int y,const char* s,u16 c){
    while(*s){dc(b,x,y,*s,c);x+=6;s++;}
}
static int sl(const char* s){int n=0;while(*s++){n++;}return n;}
static void dsc(u16* b,int cx,int y,const char* s,u16 c){
    ds(b,cx-sl(s)*3,y,s,c);
}

/* ─── notification ───────────────────────────────────────────── */
static void notif(const char* m){
    strncpy(notmsg,m,sizeof(notmsg)-1);
    notmsg[sizeof(notmsg)-1]=0;
    nottim=NOTIF_T;
}
static void draw_notif(u16* b){
    if(nottim<=0) return;
    int w=sl(notmsg)*6+10, x0=128-w/2, y0=60;
    box(b,x0,y0,x0+w,y0+12,P(0,5,0));
    rect(b,x0,y0,x0+w,y0+12,P(0,18,0));
    dsc(b,128,y0+3,notmsg,CWHITE);
}

/* ═══════════════════════════════════════════════════════════════
   Memory helpers
   ═══════════════════════════════════════════════════════════════ */
static void* xm(int sz){
    void* p=malloc(sz);
    if(!p){while(1){}}
    return p;
}
static Joint* jclone(const Joint* s,int n){
    Joint* d=(Joint*)xm(n*sizeof(Joint));
    memcpy(d,s,n*sizeof(Joint));
    return d;
}
static void ffree(Frame* f){
    for(int i=0;i<f->n;i++){
        if(f->o[i].j){free(f->o[i].j);f->o[i].j=NULL;}
    }
    f->n=0;
}

/* ═══════════════════════════════════════════════════════════════
   Undo
   ═══════════════════════════════════════════════════════════════ */
static void ufree(int s){
    UndoSnap* u=&ubuf[s];
    for(int i=0;i<u->n;i++){
        if(u->j[i]){free(u->j[i]);u->j[i]=NULL;}
    }
    u->n=0;
}
static void upush(void){
    ufree(utop);
    UndoSnap* u=&ubuf[utop];
    Frame* f=&fr[cfr];
    u->n=f->n;
    for(int i=0;i<f->n;i++){
        u->nj[i]=f->o[i].nj;
        u->j[i]=jclone(f->o[i].j,f->o[i].nj);
        u->type[i]=f->o[i].type;
        u->color[i]=f->o[i].color;
    }
    utop=(utop+1)%UNDO_N;
    if(ucnt<UNDO_N){ucnt++;}
}
static void upop(void){
    if(!ucnt){notif("NOTHING TO UNDO");return;}
    utop=(utop-1+UNDO_N)%UNDO_N;
    UndoSnap* u=&ubuf[utop];
    ffree(&fr[cfr]);
    Frame* f=&fr[cfr];
    f->n=u->n;
    for(int i=0;i<u->n;i++){
        f->o[i].nj=u->nj[i];
        f->o[i].j=jclone(u->j[i],u->nj[i]);
        f->o[i].type=u->type[i];
        f->o[i].color=u->color[i];
    }
    ucnt--;
    notif("UNDO");
}

/* ═══════════════════════════════════════════════════════════════
   Object init
   ═══════════════════════════════════════════════════════════════ */
static void mkstick(Obj* o,int x,int y,u16 col){
    o->type=0;o->color=col;o->nj=11;
    o->j=(Joint*)xm(11*sizeof(Joint));
    o->j[0]=(Joint){x,   y,    -1, 0};
    o->j[1]=(Joint){x,   y-24,  0,24};
    o->j[2]=(Joint){x,   y-40,  1,16};
    o->j[3]=(Joint){x-19,y-20,  1,19};
    o->j[4]=(Joint){x-39,y- 4,  3,25};
    o->j[5]=(Joint){x+19,y-20,  1,19};
    o->j[6]=(Joint){x+39,y- 4,  5,25};
    o->j[7]=(Joint){x-11,y+28,  0,30};
    o->j[8]=(Joint){x-19,y+56,  7,29};
    o->j[9]=(Joint){x+11,y+28,  0,30};
    o->j[10]=(Joint){x+19,y+56, 9,29};
}
static void mkball(Obj* o,int x,int y,u16 col){
    o->type=1;o->color=col;o->nj=2;
    o->j=(Joint*)xm(2*sizeof(Joint));
    o->j[0]=(Joint){x,   y,-1, 0};
    o->j[1]=(Joint){x+15,y, 0,15};
}

/* ─── reset everything ───────────────────────────────────────── */
static void reset(void){
    for(int f=0;f<nfr;f++) ffree(&fr[f]);
    memset(&fr[0],0,sizeof(Frame));
    fr[0].n=1;
    mkstick(&fr[0].o[0],128,90,PAL[0]);
    nfr=1;cfr=0;pfr=0;
    camx=0;camy=0;
    for(int i=0;i<ucnt;i++) ufree((utop+i)%UNDO_N);
    utop=0;ucnt=0;
}

/* ─── kinematics ─────────────────────────────────────────────── */
static bool isdesc(const Obj* o,int j,int a){
    int p=o->j[j].parent;
    while(p!=-1){if(p==a)return true;p=o->j[p].parent;}
    return false;
}
static void movetree(Obj* o,int j,int dx,int dy){
    o->j[j].x+=dx;o->j[j].y+=dy;
    for(int i=0;i<o->nj;i++){
        if(isdesc(o,i,j)){o->j[i].x+=dx;o->j[i].y+=dy;}
    }
}
static void aimjoint(Obj* o,int j,int wx,int wy){
    int p=o->j[j].parent;
    float d=sqrtf((float)((wx-o->j[p].x)*(wx-o->j[p].x)+
                          (wy-o->j[p].y)*(wy-o->j[p].y)));
    if(d<0.5f) return;
    int len=o->j[j].length;
    int nx=(int)(o->j[p].x+(wx-o->j[p].x)*len/d);
    int ny=(int)(o->j[p].y+(wy-o->j[p].y)*len/d);
    movetree(o,j,nx-o->j[j].x,ny-o->j[j].y);
}

/* ─── frame duplicate ────────────────────────────────────────── */
static void dupframe(void){
    if(nfr>=MAX_FR){notif("MAX FRAMES");return;}
    for(int f=nfr;f>cfr+1;f--){
        ffree(&fr[f]);
        fr[f].n=fr[f-1].n;
        for(int o=0;o<fr[f-1].n;o++){
            fr[f].o[o]=fr[f-1].o[o];
            fr[f].o[o].j=jclone(fr[f-1].o[o].j,fr[f-1].o[o].nj);
        }
    }
    ffree(&fr[cfr+1]);
    fr[cfr+1].n=fr[cfr].n;
    for(int o=0;o<fr[cfr].n;o++){
        fr[cfr+1].o[o]=fr[cfr].o[o];
        fr[cfr+1].o[o].j=jclone(fr[cfr].o[o].j,fr[cfr].o[o].nj);
    }
    cfr++;nfr++;
    notif("+FRAME");
}

/* ═══════════════════════════════════════════════════════════════
   SD card
   ═══════════════════════════════════════════════════════════════ */
static void sd_scan(void){
    nsv=0;
    if(!fat_ok) return;
    DIR* d=opendir(SAVEDIR);
    if(!d) return;
    struct dirent* e;
    while((e=readdir(d))!=NULL&&nsv<MAX_SV){
        int n=strlen(e->d_name);
        if(n<5) continue;
        if(strcmp(e->d_name+n-4,".pvt")!=0) continue;
        SaveEnt* s=&sv[nsv];
        int nc=n-4; if(nc>=SNLEN) nc=SNLEN-1;
        strncpy(s->name,e->d_name,nc); s->name[nc]=0;
        for(int i=0;s->name[i];i++){
            if(s->name[i]>='a'&&s->name[i]<='z') s->name[i]-=32;
        }
        snprintf(s->path,sizeof(s->path),"%s/%s",SAVEDIR,e->d_name);
        s->used=true;
        nsv++;
    }
    closedir(d);
}

static void sd_save(const char* name){
    if(!fat_ok){notif("NO SD CARD");return;}
    mkdir(SAVEDIR,0777);
    char path[80];
    snprintf(path,sizeof(path),"%s/%s.pvt",SAVEDIR,name);
    FILE* f=fopen(path,"wb");
    if(!f){notif("WRITE FAILED");return;}
    u32 magic=MAGIC, nf2=(u32)nfr;
    fwrite(&magic,4,1,f);
    fwrite(&nf2,4,1,f);
    for(int fi=0;fi<nfr;fi++){
        u32 no=(u32)fr[fi].n;
        fwrite(&no,4,1,f);
        for(int oi=0;oi<fr[fi].n;oi++){
            Obj* o=&fr[fi].o[oi];
            u32 type=(u32)o->type;
            u16 col=o->color;
            u32 nj=(u32)o->nj;
            fwrite(&type,4,1,f);
            fwrite(&col,2,1,f);
            fwrite(&nj,4,1,f);
            fwrite(o->j,sizeof(Joint),(size_t)o->nj,f);
        }
    }
    fclose(f);
    sd_scan();
    notif("SAVED!");
}

static void sd_load(const char* path){
    if(!fat_ok){notif("NO SD CARD");return;}
    FILE* f=fopen(path,"rb");
    if(!f){notif("NOT FOUND");return;}
    u32 magic=0,nf2=0;
    fread(&magic,4,1,f);
    if(magic!=MAGIC){fclose(f);notif("BAD FILE");return;}
    fread(&nf2,4,1,f);
    if(nf2==0||nf2>MAX_FR){fclose(f);notif("BAD FILE");return;}

    /* load to temp */
    Frame tmp[MAX_FR];
    memset(tmp,0,nf2*sizeof(Frame));
    bool ok=true;
    for(u32 fi=0;fi<nf2&&ok;fi++){
        u32 no=0; fread(&no,4,1,f);
        if(no>MAX_OB){ok=false;break;}
        tmp[fi].n=(int)no;
        for(u32 oi=0;oi<no&&ok;oi++){
            u32 type=0; u16 col=0; u32 nj=0;
            fread(&type,4,1,f);
            fread(&col,2,1,f);
            fread(&nj,4,1,f);
            if(nj==0||nj>256){ok=false;break;}
            tmp[fi].o[oi].type=(int)type;
            tmp[fi].o[oi].color=col;
            tmp[fi].o[oi].nj=(int)nj;
            tmp[fi].o[oi].j=(Joint*)xm((int)nj*sizeof(Joint));
            fread(tmp[fi].o[oi].j,sizeof(Joint),(size_t)nj,f);
        }
    }
    fclose(f);
    if(!ok){
        for(int fi=0;fi<(int)nf2;fi++) ffree(&tmp[fi]);
        notif("LOAD ERROR");
        return;
    }
    /* commit */
    for(int fi=0;fi<nfr;fi++) ffree(&fr[fi]);
    memcpy(fr,tmp,nf2*sizeof(Frame));
    nfr=(int)nf2; cfr=0; pfr=0;
    camx=0; camy=0;
    for(int i=0;i<ucnt;i++) ufree((utop+i)%UNDO_N);
    utop=0; ucnt=0;
    notif("LOADED!");
}

static void sd_del(const char* path){
    if(!fat_ok){notif("NO SD CARD");return;}
    remove(path);
    sd_scan();
    ssel=-1;
    notif("DELETED");
}

/* ═══════════════════════════════════════════════════════════════
   Draw object
   ═══════════════════════════════════════════════════════════════ */
static u16 blnd(u16 c,u16 t){
    int r=(((c>>10)&31)+((t>>10)&31))>>1;
    int g=(((c>>5)&31)+((t>>5)&31))>>1;
    int b=((c&31)+(t&31))>>1;
    return P(r,g,b);
}

static void drawobj(u16* buf,const Obj* o,bool edit,bool ghost,int ox,int oy){
    u16 col=ghost?blnd(o->color,CONION):o->color;
    if(o->type==1){
        int dx=o->j[1].x-o->j[0].x, dy=o->j[1].y-o->j[0].y;
        int r=(int)sqrtf((float)(dx*dx+dy*dy));
        if(r<1) r=1;
        circ(buf,o->j[0].x-ox,o->j[0].y-oy,r,col);
        if(edit){
            circ(buf,o->j[0].x-ox,o->j[0].y-oy,4,CHANDLE);
            circ(buf,o->j[1].x-ox,o->j[1].y-oy,4,CHANDLE);
        }
    } else {
        for(int i=1;i<o->nj;i++){
            int p=o->j[i].parent;
            if(p>=0) line(buf,o->j[i].x-ox,o->j[i].y-oy,
                              o->j[p].x-ox,o->j[p].y-oy,2,col);
        }
        if(o->nj>2){
            circ(buf,o->j[2].x-ox,o->j[2].y-oy,8,col);
            if(!edit&&!ghost)
                circ(buf,o->j[2].x-ox+2,o->j[2].y-oy-2,1,CWHITE);
        }
        if(edit){
            for(int i=0;i<o->nj;i++)
                circ(buf,o->j[i].x-ox,o->j[i].y-oy,3,CHANDLE);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════
   UI widgets
   ═══════════════════════════════════════════════════════════════ */
static void btn(u16* b,int ci,int row,const char* lbl,bool active){
    int x0=ci*BW, y0=row?R1Y:R0Y, x1=x0+BW-1, y1=y0+BH-1;
    u16 bg=active?P(4,12,2):P(6,6,8);
    u16 hi=active?P(12,26,6):P(20,20,23);
    u16 sh=P(2,2,3);
    box(b,x0+1,y0+1,x1-1,y1-1,bg);
    for(int x=x0;x<=x1;x++){px(b,x,y0,hi);px(b,x,y1,sh);}
    for(int y=y0;y<=y1;y++){px(b,x0,y,hi);px(b,x1,y,sh);}
    ds(b,x0+BW/2-sl(lbl)*3,y0+BH/2-3,lbl,CWHITE);
}

static void tline_main(u16* b,int tf){
    int ty=SH-9;
    box(b,0,ty-2,SW-1,SH-1,CDGRAY);
    line(b,6,ty+3,SW-10,ty+3,1,CGRAY);
    for(int i=0;i<nfr;i++){
        int tx=6+i*(SW-16)/(nfr>1?nfr-1:1);
        if(i==tf) circ(b,tx,ty+3,3,CRED);
        else line(b,tx,ty+1,tx,ty+5,1,P(14,14,14));
    }
    if(playing) circ(b,SW-5,ty+3,3,CGREEN);
    char buf[12]; sprintf(buf,"F%d/%d",tf+1,nfr);
    ds(b,2,ty-1,buf,P(14,14,14));
    sprintf(buf,"%dFPS",fps);
    ds(b,SW-28,2,buf,P(14,14,14));
}

static void tline_sub(u16* b,int tf){
    int ty=R1Y-2;
    for(int x=0;x<SW;x++) px(b,x,ty,P(4,4,5));
    if(nfr>1){
        for(int i=0;i<nfr;i++){
            int tx=4+i*(SW-8)/(nfr-1);
            if(i==tf) circ(b,tx,ty,2,PAL[ccol]);
            else px(b,tx,ty,P(16,16,16));
        }
    }
}

/* ═══════════════════════════════════════════════════════════════
   Render: edit mode
   ═══════════════════════════════════════════════════════════════ */
static void render_edit(int so){
    int sf=playing?pfr:cfr;

    /* ── main screen: preview ── */
    if(bgon&&bg){
        /* copy background photo */
        memcpy(bm,bg,SW*SH*2);
    } else {
        /* white + dot grid */
        u16 gval=P(27,27,29);
        u16 wval=CWHITE;
        for(int y=0;y<SH;y++) for(int x=0;x<SW;x++)
            bm[y*SW+x]=((x%20==0)||(y%20==0))?gval:wval;
    }
    for(int o=0;o<fr[sf].n;o++) drawobj(bm,&fr[sf].o[o],false,false,camx,camy);
    tline_main(bm,sf);

    /* ── sub screen: editor ── */
    {
        u16 gval=P(27,27,29), wval=CWHITE;
        for(int y=0;y<CH;y++) for(int x=0;x<SW;x++)
            bs[y*SW+x]=((x%32==0)||(y%32==0))?gval:wval;
    }
    /* onion */
    if(onion&&cfr>0){
        for(int o=0;o<fr[cfr-1].n;o++)
            drawobj(bs,&fr[cfr-1].o[o],false,true,camx,camy);
    }
    /* current */
    for(int o=0;o<fr[cfr].n;o++)
        drawobj(bs,&fr[cfr].o[o],true,false,camx,camy);
    /* selection box */
    if(so>=0){
        const Obj* ob=&fr[cfr].o[so];
        int x0=9999,y0=9999,x1=-9999,y1=-9999;
        for(int i=0;i<ob->nj;i++){
            int jx=ob->j[i].x-camx, jy=ob->j[i].y-camy;
            if(jx<x0) x0=jx; if(jx>x1) x1=jx;
            if(jy<y0) y0=jy; if(jy>y1) y1=jy;
        }
        x0-=10;y0-=10;x1+=10;y1+=10;
        if(x0<0) x0=0; if(y0<0) y0=0;
        if(x1>=SW) x1=SW-1; if(y1>=CH) y1=CH-1;
        if(x1>x0&&y1>y0) rect(bs,x0,y0,x1,y1,CSEL);
    }
    /* UI strip */
    box(bs,0,R0Y,SW-1,SH-1,CDGRAY);
    tline_sub(bs,cfr);
    btn(bs,0,0,"PREV",false); btn(bs,1,0,"NEXT",false);
    btn(bs,2,0,"+FRM",false); btn(bs,3,0,"-FRM",false);
    btn(bs,4,0,"UNDO",ucnt>0); btn(bs,5,0,"CLR",false);
    btn(bs,0,1,playing?"STOP":"PLAY",playing);
    {char b[6];sprintf(b,"%dFPS",fps);btn(bs,1,1,b,false);}
    btn(bs,2,1,PNAME[ccol],so>=0);
    circ(bs,2*BW+BW/2,R1Y+BH-6,4,PAL[ccol]);
    btn(bs,3,1,ctype==0?"STKM":"BALL",false);
    btn(bs,4,1,"+OBJ",false);
    btn(bs,5,1,"ANIM",false);
    /* counters */
    {
        char b[16]; sprintf(b,"F%d/%d O%d",cfr+1,nfr,fr[cfr].n);
        box(bs,0,R0Y-10,72,R0Y-1,CBLACK);
        ds(bs,2,R0Y-9,b,CGRAY);
    }
    {
        u16 oc=onion?P(0,18,12):P(10,10,10);
        ds(bs,SW-22,R0Y-9,"ONI",oc);
    }
    if(bgon) ds(bs,SW-40,R0Y-9,"BG",P(0,18,14));
    draw_notif(bs);
}

/* ═══════════════════════════════════════════════════════════════
   Render: save/load menu
   ═══════════════════════════════════════════════════════════════ */
#define SCW 126
#define SCH2 44
#define SCOLS 2
#define SROWS 4

static void render_smenu(void){
    /* main: info */
    box(bm,0,0,SW-1,SH-1,P(3,3,6));
    dsc(bm,128,5,"ANIMATIONS",CWHITE);
    line(bm,0,15,SW-1,15,1,P(10,10,13));
    if(!fat_ok){
        dsc(bm,128,50,"NO SD CARD",CRED);
        dsc(bm,128,66,"INSERT FLASHCARD",CGRAY);
    } else if(ssel>=0&&ssel<nsv){
        dsc(bm,128,24,sv[ssel].name,P(20,26,31));
        line(bm,10,40,246,40,1,P(8,8,10));
        dsc(bm,128,50,"A = LOAD",CWHITE);
        dsc(bm,128,62,"Y = DELETE",P(22,8,8));
        dsc(bm,128,74,"X = SAVE CURRENT",P(8,22,8));
    } else {
        dsc(bm,128,36,"TAP A SLOT TO SELECT",CGRAY);
        dsc(bm,128,54,"X = SAVE CURRENT ANIM",P(8,22,8));
        {char b[24];sprintf(b,"%d FILE(S) ON CARD",nsv);dsc(bm,128,70,b,CGRAY);}
    }
    line(bm,0,SH-22,SW-1,SH-22,1,P(8,8,10));
    dsc(bm,128,SH-14,"B = BACK",CGRAY);

    /* sub: slot grid */
    box(bs,0,0,SW-1,SH-1,P(4,4,6));
    box(bs,0,0,SW-1,11,P(2,2,4));
    dsc(bs,128,3,"TAP TO SELECT  2ND TAP=LOAD",P(12,12,14));

    for(int i=0;i<MAX_SV;i++){
        int ci=i%SCOLS, ri=i/SCOLS;
        int x0=1+ci*SCW, y0=12+ri*SCH2;
        int x1=x0+SCW-2, y1=y0+SCH2-2;
        bool sel=(i==ssel), used=(i<nsv);
        u16 bg2=sel?P(4,12,20):(used?P(7,7,10):P(4,4,5));
        u16 bd=sel?CSEL:(used?P(16,16,20):P(7,7,8));
        box(bs,x0,y0,x1,y1,bg2);
        rect(bs,x0,y0,x1,y1,bd);
        if(used){
            char nb[4]; sprintf(nb,"%d",i+1);
            ds(bs,x0+3,y0+3,nb,P(10,10,12));
            dsc(bs,(x0+x1)/2,(y0+y1)/2-3,sv[i].name,sel?CWHITE:P(22,22,24));
        } else {
            dsc(bs,(x0+x1)/2,(y0+y1)/2-3,"--",P(10,10,11));
        }
    }
    draw_notif(bs);
}

/* ═══════════════════════════════════════════════════════════════
   Render: name input
   ═══════════════════════════════════════════════════════════════ */
static void render_sname(void){
    box(bm,0,0,SW-1,SH-1,P(3,3,6));
    dsc(bm,128,18,"NAME THIS SAVE",CWHITE);
    box(bm,40,40,216,58,P(2,2,4));
    rect(bm,40,40,216,58,CSEL);
    dsc(bm,128,46,nlen?nbuf:"...",CWHITE);
    line(bm,0,SH-20,SW-1,SH-20,1,P(8,8,10));
    dsc(bm,128,SH-13,"A/OK=SAVE  B=CANCEL",CGRAY);

    box(bs,0,0,SW-1,SH-1,CDGRAY);
    dsc(bs,128,3,"TAP LETTERS",CGRAY);
    int kw=24,kh=40;
    for(int r=0;r<KROWS;r++){
        const char* row=KBD[r]; int nc=sl(row);
        for(int c=0;c<nc;c++){
            int kx=2+c*kw, ky=12+r*kh;
            box(bs,kx,ky,kx+kw-2,ky+kh-4,P(10,10,13));
            rect(bs,kx,ky,kx+kw-2,ky+kh-4,CGRAY);
            char k[3]={row[c],0,0};
            if(row[c]==' '){k[0]='S';k[1]='P';}
            dsc(bs,kx+(kw-2)/2,ky+(kh-4)/2-3,k,CWHITE);
        }
    }
    box(bs,0,SH-12,SW-1,SH-1,CBLACK);
    dsc(bs,128,SH-10,nlen?nbuf:"",CWHITE);
}

/* ═══════════════════════════════════════════════════════════════
   Render: bg freeze mode
   ═══════════════════════════════════════════════════════════════ */
static void render_bgmode(void){
    /* Show current animation on main as preview to freeze */
    int sf=playing?pfr:cfr;
    for(int i=0;i<SW*SH;i++) bm[i]=CWHITE;
    for(int o=0;o<fr[sf].n;o++) drawobj(bm,&fr[sf].o[o],false,false,camx,camy);
    tline_main(bm,sf);

    box(bs,0,0,SW-1,SH-1,P(3,3,5));
    dsc(bs,128,20,"FREEZE BACKGROUND",CWHITE);
    line(bs,0,32,SW-1,32,1,P(8,8,10));
    dsc(bs,128,46,"A = FREEZE TOP SCREEN AS BG",P(8,20,8));
    dsc(bs,128,60,"X = REMOVE BACKGROUND",P(20,8,8));
    dsc(bs,128,74,"B = BACK TO EDITOR",CGRAY);
    dsc(bs,128,96,bgon?"STATUS: ACTIVE":"STATUS: NONE",bgon?P(0,20,14):CGRAY);
    draw_notif(bs);
}

/* ═══════════════════════════════════════════════════════════════
   Main
   ═══════════════════════════════════════════════════════════════ */
int main(void){
    /* ── Video init ──────────────────────────────────────────
       MODE_5_2D on both screens.
       BG3 in mode 5 is a 256×192 16-bit bitmap.
       VRAM_A → main BG,  VRAM_C → sub BG.
       We get a direct pointer to the VRAM and write pixels
       ourselves during VBlank — no sprites, no tile maps.
    ─────────────────────────────────────────────────────────── */
    videoSetMode(MODE_5_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    vmain = (u16*)BG_BMP_RAM(0);      /* 0x06000000 */

    videoSetModeSub(MODE_5_2D);
    vramSetBankC(VRAM_C_SUB_BG);
    bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    vsub  = (u16*)BG_BMP_RAM_SUB(0);  /* 0x06200000 */

    /* pre-fill back-buffers with black (BIT15 set) so no garbage on first frame */
    for(int i=0;i<SW*SH;i++){bm[i]=CBLACK;bs[i]=CBLACK;}

    /* allocate bg buffer on heap */
    bg=(u16*)malloc(SW*SH*2);
    if(bg) memset(bg,0,SW*SH*2);

    /* FAT */
    fat_ok=fatInitDefault();
    if(fat_ok){mkdir(SAVEDIR,0777);sd_scan();}

    /* init state */
    memset(fr,0,sizeof(fr));
    memset(ubuf,0,sizeof(ubuf));
    memset(sv,0,sizeof(sv));
    reset();

    int so=-1,sj=-1,ltx=0,lty=0;
    bool tui=false;

    while(1){
        scanKeys();
        int keys=keysDown(), held=keysHeld();
        touchPosition tp; touchRead(&tp);
        if(nottim>0) nottim--;
        ptim++;

        /* ── SAVE MENU ── */
        if(mode==SMENU){
            if(keys&KEY_B){mode=ED;ssel=-1;}
            if(keys&KEY_X){memset(nbuf,0,sizeof(nbuf));nlen=0;mode=SNAME;}
            if(keys&KEY_A){
                if(ssel>=0&&ssel<nsv){sd_load(sv[ssel].path);mode=ED;ssel=-1;}
                else notif("SELECT A SLOT");
            }
            if(keys&KEY_Y){
                if(ssel>=0&&ssel<nsv) sd_del(sv[ssel].path);
                else notif("SELECT A SLOT");
            }
            if(keys&KEY_TOUCH){
                int ci=tp.px/SCW, ri=(tp.py-12)/SCH2;
                if(tp.py>=12&&ri>=0&&ri<SROWS&&ci>=0&&ci<SCOLS){
                    int idx=ri*SCOLS+ci;
                    if(idx<MAX_SV){
                        if(ssel==idx&&idx<nsv){
                            sd_load(sv[idx].path);mode=ED;ssel=-1;
                        } else {
                            ssel=idx;
                        }
                    }
                }
            }
            render_smenu();flush();continue;
        }

        /* ── NAME INPUT ── */
        if(mode==SNAME){
            if(keys&KEY_B){mode=SMENU;}
            if(keys&KEY_A){
                if(nlen>0){sd_save(nbuf);mode=SMENU;}
                else notif("TYPE A NAME");
            }
            if(keys&KEY_TOUCH){
                int kw=24,kh=40;
                int r=(tp.py-12)/kh, c=tp.px/kw;
                if(r>=0&&r<KROWS&&c>=0){
                    const char* row=KBD[r]; int nc=sl(row);
                    if(c<nc){
                        char ch=row[c];
                        if(ch=='<'){if(nlen>0){nlen--;nbuf[nlen]=0;}}
                        else if(ch=='O'&&r==3&&c==9){
                            if(nlen>0){sd_save(nbuf);mode=SMENU;}
                        }
                        else if(ch==' '){
                            if(nlen<SNLEN-1){nbuf[nlen++]='_';nbuf[nlen]=0;}
                        }
                        else{
                            if(nlen<SNLEN-1){nbuf[nlen++]=ch;nbuf[nlen]=0;}
                        }
                    }
                }
            }
            render_sname();flush();continue;
        }

        /* ── BG FREEZE MODE ── */
        if(mode==BGMODE){
            if(keys&KEY_B){mode=ED;}
            if(keys&KEY_A){
                /* freeze current top-screen back-buffer as background */
                if(bg){
                    int sf=playing?pfr:cfr;
                    /* draw frame to bm first */
                    for(int i=0;i<SW*SH;i++) bm[i]=CWHITE;
                    for(int o=0;o<fr[sf].n;o++)
                        drawobj(bm,&fr[sf].o[o],false,false,camx,camy);
                    memcpy(bg,bm,SW*SH*2);
                    bgon=true;
                    notif("BG SET");
                }
                mode=ED;
            }
            if(keys&KEY_X){bgon=false;if(bg)memset(bg,0,SW*SH*2);notif("BG CLEARED");}
            render_bgmode();flush();continue;
        }

        /* ── EDIT MODE ── */
        if(playing&&nfr>1&&ptim>=(60/fps)){
            ptim=0; pfr=(pfr+1)%nfr;
        }

        if(keys&KEY_START){
            playing=!playing;
            if(playing){pfr=cfr;ptim=0;}
        }
        if(!playing){
            if(keys&KEY_L&&cfr>0) cfr--;
            if(keys&KEY_R&&cfr<nfr-1) cfr++;
        }
        if(keys&KEY_SELECT) dupframe();
        if(keys&KEY_X){onion=!onion;notif(onion?"ONION ON":"ONION OFF");}
        if((keys&KEY_Y)&&so>=0){
            upush();
            Frame* f=&fr[cfr];
            free(f->o[so].j);
            for(int i=so;i<f->n-1;i++) f->o[i]=f->o[i+1];
            f->n--;
            so=-1;sj=-1;
            notif("OBJ DELETED");
        }

        /* D-pad nudge */
        if(so>=0&&sj>=0){
            int ddx=(held&KEY_RIGHT)?1:(held&KEY_LEFT)?-1:0;
            int ddy=(held&KEY_DOWN)?1:(held&KEY_UP)?-1:0;
            if(ddx||ddy){
                Obj* ob=&fr[cfr].o[so];
                if(sj==0){
                    movetree(ob,0,ddx,ddy);
                } else {
                    int p=ob->j[sj].parent;
                    int nx=ob->j[sj].x+ddx, ny=ob->j[sj].y+ddy;
                    float dd=sqrtf((float)((nx-ob->j[p].x)*(nx-ob->j[p].x)+
                                           (ny-ob->j[p].y)*(ny-ob->j[p].y)));
                    if(dd>0.5f){
                        int l=ob->j[sj].length;
                        movetree(ob,sj,
                            (int)(ob->j[p].x+(nx-ob->j[p].x)*l/dd)-ob->j[sj].x,
                            (int)(ob->j[p].y+(ny-ob->j[p].y)*l/dd)-ob->j[sj].y);
                    }
                }
            }
        }

        /* touch press */
        if(keys&KEY_TOUCH){
            tui=(tp.py>=CH);
            if(tui){
                int row=(tp.py>=R1Y)?1:0, b=tp.px/BW;
                if(b>5) b=5;
                if(row==0){
                    if(b==0&&cfr>0) cfr--;
                    else if(b==1&&cfr<nfr-1) cfr++;
                    else if(b==2) dupframe();
                    else if(b==3&&nfr>1){
                        ffree(&fr[cfr]);
                        for(int f2=cfr;f2<nfr-1;f2++) fr[f2]=fr[f2+1];
                        nfr--;
                        if(cfr>=nfr) cfr=nfr-1;
                        notif("-FRAME");
                    }
                    else if(b==4) upop();
                    else if(b==5){reset();so=-1;notif("CLEARED");}
                } else {
                    if(b==0){
                        playing=!playing;
                        if(playing){pfr=cfr;ptim=0;}
                    }
                    else if(b==1) fps=(fps>=30)?4:fps+4;
                    else if(b==2){
                        ccol=(ccol+1)%NPAL;
                        if(so>=0) fr[cfr].o[so].color=PAL[ccol];
                    }
                    else if(b==3) ctype=(ctype+1)%2;
                    else if(b==4){
                        Frame* f=&fr[cfr];
                        if(f->n<MAX_OB){
                            Obj* no=&f->o[f->n];
                            if(ctype==0) mkstick(no,128+camx,90+camy,PAL[ccol]);
                            else         mkball (no,128+camx,96+camy,PAL[ccol]);
                            f->n++;
                            notif("+OBJ");
                        } else notif("MAX OBJS!");
                    }
                    else if(b==5){
                        sd_scan();ssel=-1;mode=SMENU;
                    }
                }
            } else {
                /* pick joint */
                so=-1;sj=-1;
                int best=20*20;
                for(int o=0;o<fr[cfr].n;o++){
                    Obj* ob=&fr[cfr].o[o];
                    for(int i=0;i<ob->nj;i++){
                        int dx=(tp.px+camx)-ob->j[i].x;
                        int dy=(tp.py+camy)-ob->j[i].y;
                        int d2=dx*dx+dy*dy;
                        if(d2<best){best=d2;so=o;sj=i;}
                    }
                }
                if(so>=0){
                    upush();
                    u16 oc=fr[cfr].o[so].color;
                    for(int c=0;c<NPAL;c++) if(PAL[c]==oc){ccol=c;break;}
                } else {
                    ltx=tp.px;lty=tp.py;
                }
            }
        }

        /* touch held */
        if((held&KEY_TOUCH)&&!tui&&tp.py<CH){
            if(so>=0){
                Obj* ob=&fr[cfr].o[so];
                int wx=tp.px+camx, wy=tp.py+camy;
                if(sj==0) movetree(ob,0,wx-ob->j[0].x,wy-ob->j[0].y);
                else       aimjoint(ob,sj,wx,wy);
            } else {
                camx-=(tp.px-ltx);camy-=(tp.py-lty);
                ltx=tp.px;lty=tp.py;
            }
        }
        if(!(held&KEY_TOUCH)){so=-1;sj=-1;}

        /* long-hold on ANIM button opens BG mode instead */
        /* (detected by checking if MODE_CAMERA entered from ANIM btn hold) */

        render_edit(so);
        flush();
    }
    return 0;
}
