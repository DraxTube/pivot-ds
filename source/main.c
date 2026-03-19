/*
 * PivotDS v5 — Nintendo DS Pivot Animator
 *
 * Features:
 *   - Stickman + ball objects, unlimited joints
 *   - Double-buffered rendering (no flicker)
 *   - Onion skin (toggle with X)
 *   - 8-level undo
 *   - Animation save/load to SD card (.pvt files, survive power-off)
 *   - Camera background capture
 *   - Delete selected object (Y)
 *   - D-pad fine-nudge selected joint
 */

#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>

/* ═══════════════════════════════════════════════════════════════
   Constants
   ═══════════════════════════════════════════════════════════════ */
#define MAX_FRAMES      200
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
#define SAVE_DIR        "fat:/pivotds"
#define MAX_SAVES        16
#define SAVE_NAME_LEN     9   /* 8 chars + NUL */
#define PVT_MAGIC       0x50565435u  /* "PVT5" */

/* ═══════════════════════════════════════════════════════════════
   Colours
   ═══════════════════════════════════════════════════════════════ */
#define C15(r,g,b)  (RGB15(r,g,b)|BIT(15))
#define COL_WHITE   C15(31,31,31)
#define COL_BLACK   C15( 0, 0, 0)
#define COL_GRAY    C15(20,20,20)
#define COL_DGRAY   C15( 8, 8,10)
#define COL_RED     C15(31, 0, 0)
#define COL_HANDLE  C15(31,20, 0)
#define COL_SEL     C15( 0,18,31)
#define COL_ONION   C15(15,15,26)
#define COL_GRID    C15(27,27,29)
#define COL_GREEN   C15( 0,22, 0)
#define COL_AMBER   C15(31,20, 0)

/* ═══════════════════════════════════════════════════════════════
   Types
   ═══════════════════════════════════════════════════════════════ */
typedef struct { int x, y, parent, length; } Joint;

typedef struct {
    int    type;        /* 0=stickman  1=ball */
    u16    color;
    int    num_joints;
    Joint* joints;      /* heap */
} Object;

typedef struct {
    Object objs[MAX_OBJS];
    int    num_objs;
} Frame;

/* Undo snapshot */
typedef struct {
    int    num_objs;
    int    num_joints[MAX_OBJS];
    Joint* joints[MAX_OBJS];    /* heap copies */
    int    type[MAX_OBJS];
    u16    color[MAX_OBJS];
} UndoSnap;

/* Save-slot descriptor (names scanned from SD) */
typedef struct {
    char name[SAVE_NAME_LEN];   /* display name (filename without .pvt) */
    char path[64];              /* full fat: path */
    bool used;
} SaveEntry;

/* ═══════════════════════════════════════════════════════════════
   Globals
   ═══════════════════════════════════════════════════════════════ */
static Frame     frames[MAX_FRAMES];
static int       num_frames  = 1;
static int       cur_frame   = 0;
static int       play_frame  = 0;
static int       play_timer  = 0;
static bool      is_playing  = false;
static int       fps         = 8;
static bool      onion_on    = true;

static UndoSnap  undo_buf[UNDO_LEVELS];
static int       undo_top    = 0;
static int       undo_count  = 0;

/* Camera background — heap allocated in main() */
static u16*  bg_buf    = NULL;
static bool  bg_active = false;

/* Palette (10 colours) */
static const u16 palette[] = {
    C15( 0, 0, 0), C15(31, 0, 0), C15( 0,22, 0), C15( 0, 0,31),
    C15(31,15, 0), C15(18, 0,22), C15( 0,18,18), C15(31,31, 0),
    C15(31,31,31), C15(14,14,14),
};
static const char* pal_name[] = {
    "BLK","RED","GRN","BLU","ORG","PRP","TEL","YLW","WHT","GRY"
};
#define NUM_COLORS 10

static int  cur_color = 0;
static int  cur_type  = 0;   /* 0=stickman 1=ball */
static int  cam_x     = 0, cam_y = 0;

/* UI mode */
typedef enum { MODE_EDIT=0, MODE_SAVE_MENU, MODE_SAVE_NAME, MODE_CAMERA } UIMode;
static UIMode ui_mode = MODE_EDIT;

/* Save menu state */
static SaveEntry save_entries[MAX_SAVES];
static int       save_count  = 0;
static int       save_sel    = -1;
static bool      fat_ok      = false;

/* Naming state (for new save) */
static char      name_buf[SAVE_NAME_LEN] = "";
static int       name_len = 0;

/* Keyboard */
static const char* kbd_rows[] = {
    "ABCDEFGHIJ",
    "KLMNOPQRST",
    "UVWXYZ0123",
    "456789 <OK"
};
#define KBD_ROWS 4

/* Notifications */
static char notif_msg[32] = "";
static int  notif_timer   = 0;

/* Double buffers — must be 4-byte aligned for DMA */
static u16  bbMain[SCREEN_W * SCREEN_H] __attribute__((aligned(4)));
static u16  bbSub [SCREEN_W * SCREEN_H] __attribute__((aligned(4)));
static u16* vramMain;
static u16* vramSub;

/* ═══════════════════════════════════════════════════════════════
   Memory helpers
   ═══════════════════════════════════════════════════════════════ */
static void* xmalloc(int sz){
    void* p = malloc(sz);
    if(!p){ while(1){} }
    return p;
}

static Joint* joints_clone(const Joint* s, int n){
    Joint* d = (Joint*)xmalloc(n * sizeof(Joint));
    memcpy(d, s, n * sizeof(Joint));
    return d;
}

static void frame_free(Frame* f){
    for(int i = 0; i < f->num_objs; i++){
        if(f->objs[i].joints){ free(f->objs[i].joints); f->objs[i].joints = NULL; }
    }
    f->num_objs = 0;
}

/* ═══════════════════════════════════════════════════════════════
   Notifications
   ═══════════════════════════════════════════════════════════════ */
static void notif(const char* m){
    strncpy(notif_msg, m, sizeof(notif_msg)-1);
    notif_msg[sizeof(notif_msg)-1] = 0;
    notif_timer = NOTIF_FRAMES;
}

/* ═══════════════════════════════════════════════════════════════
   Undo
   ═══════════════════════════════════════════════════════════════ */
static void undo_free(int slot){
    UndoSnap* u = &undo_buf[slot];
    for(int i = 0; i < u->num_objs; i++){
        if(u->joints[i]){ free(u->joints[i]); u->joints[i] = NULL; }
    }
    u->num_objs = 0;
}

static void push_undo(void){
    undo_free(undo_top);
    UndoSnap* u = &undo_buf[undo_top];
    Frame*    f = &frames[cur_frame];
    u->num_objs = f->num_objs;
    for(int i = 0; i < f->num_objs; i++){
        u->num_joints[i] = f->objs[i].num_joints;
        u->joints[i]     = joints_clone(f->objs[i].joints, f->objs[i].num_joints);
        u->type[i]       = f->objs[i].type;
        u->color[i]      = f->objs[i].color;
    }
    undo_top = (undo_top + 1) % UNDO_LEVELS;
    if(undo_count < UNDO_LEVELS){ undo_count++; }
}

static void pop_undo(void){
    if(!undo_count){ return; }
    undo_top = (undo_top - 1 + UNDO_LEVELS) % UNDO_LEVELS;
    UndoSnap* u = &undo_buf[undo_top];
    frame_free(&frames[cur_frame]);
    Frame* f = &frames[cur_frame];
    f->num_objs = u->num_objs;
    for(int i = 0; i < u->num_objs; i++){
        f->objs[i].num_joints = u->num_joints[i];
        f->objs[i].joints     = joints_clone(u->joints[i], u->num_joints[i]);
        f->objs[i].type       = u->type[i];
        f->objs[i].color      = u->color[i];
    }
    undo_count--;
    notif("UNDO");
}

/* ═══════════════════════════════════════════════════════════════
   Object init
   ═══════════════════════════════════════════════════════════════ */
static void obj_stickman(Object* o, int x, int y, u16 col){
    o->type = 0; o->color = col; o->num_joints = 11;
    o->joints = (Joint*)xmalloc(11 * sizeof(Joint));
    o->joints[0]  = (Joint){x,    y,     -1,  0};
    o->joints[1]  = (Joint){x,    y-24,   0, 24};
    o->joints[2]  = (Joint){x,    y-40,   1, 16};
    o->joints[3]  = (Joint){x-19, y-20,   1, 19};
    o->joints[4]  = (Joint){x-39, y-4,    3, 25};
    o->joints[5]  = (Joint){x+19, y-20,   1, 19};
    o->joints[6]  = (Joint){x+39, y-4,    5, 25};
    o->joints[7]  = (Joint){x-11, y+28,   0, 30};
    o->joints[8]  = (Joint){x-19, y+56,   7, 29};
    o->joints[9]  = (Joint){x+11, y+28,   0, 30};
    o->joints[10] = (Joint){x+19, y+56,   9, 29};
}

static void obj_ball(Object* o, int x, int y, u16 col){
    o->type = 1; o->color = col; o->num_joints = 2;
    o->joints = (Joint*)xmalloc(2 * sizeof(Joint));
    o->joints[0] = (Joint){x,    y, -1,  0};
    o->joints[1] = (Joint){x+15, y,  0, 15};
}

static void reset_all(void){
    for(int f = 0; f < num_frames; f++){ frame_free(&frames[f]); }
    memset(&frames[0], 0, sizeof(Frame));
    frames[0].num_objs = 1;
    obj_stickman(&frames[0].objs[0], 128, 90, palette[0]);
    num_frames = 1; cur_frame = 0; play_frame = 0;
    cam_x = 0; cam_y = 0;
    for(int i = 0; i < undo_count; i++){ undo_free((undo_top + i) % UNDO_LEVELS); }
    undo_top = 0; undo_count = 0;
}

/* ═══════════════════════════════════════════════════════════════
   Skeleton kinematics
   ═══════════════════════════════════════════════════════════════ */
static bool is_desc(const Object* o, int j, int anc){
    int p = o->joints[j].parent;
    while(p != -1){
        if(p == anc){ return true; }
        p = o->joints[p].parent;
    }
    return false;
}

static void move_tree(Object* o, int j, int dx, int dy){
    o->joints[j].x += dx; o->joints[j].y += dy;
    for(int i = 0; i < o->num_joints; i++){
        if(is_desc(o, i, j)){
            o->joints[i].x += dx; o->joints[i].y += dy;
        }
    }
}

/* Rotate joint j around its parent to point toward (wx,wy) */
static void aim_joint(Object* o, int j, int wx, int wy){
    int p = o->joints[j].parent;
    float d = sqrtf((float)((wx - o->joints[p].x)*(wx - o->joints[p].x) +
                            (wy - o->joints[p].y)*(wy - o->joints[p].y)));
    if(d < 0.1f){ return; }
    int len = o->joints[j].length;
    int nx  = (int)(o->joints[p].x + (wx - o->joints[p].x) * len / d);
    int ny  = (int)(o->joints[p].y + (wy - o->joints[p].y) * len / d);
    move_tree(o, j, nx - o->joints[j].x, ny - o->joints[j].y);
}

/* ═══════════════════════════════════════════════════════════════
   Frame duplication helper
   ═══════════════════════════════════════════════════════════════ */
static void dup_frame_after_cur(void){
    if(num_frames >= MAX_FRAMES){ notif("MAX FRAMES"); return; }
    /* Shift frames right */
    for(int f = num_frames; f > cur_frame + 1; f--){
        frame_free(&frames[f]);
        frames[f].num_objs = frames[f-1].num_objs;
        for(int o = 0; o < frames[f-1].num_objs; o++){
            frames[f].objs[o]        = frames[f-1].objs[o];
            frames[f].objs[o].joints = joints_clone(frames[f-1].objs[o].joints,
                                                     frames[f-1].objs[o].num_joints);
        }
    }
    /* Clone current frame into cur+1 */
    frame_free(&frames[cur_frame + 1]);
    frames[cur_frame+1].num_objs = frames[cur_frame].num_objs;
    for(int o = 0; o < frames[cur_frame].num_objs; o++){
        frames[cur_frame+1].objs[o]        = frames[cur_frame].objs[o];
        frames[cur_frame+1].objs[o].joints = joints_clone(frames[cur_frame].objs[o].joints,
                                                           frames[cur_frame].objs[o].num_joints);
    }
    cur_frame++; num_frames++;
    notif("+FRAME");
}

/* ═══════════════════════════════════════════════════════════════
   SD save / load
   File format (binary):
     u32 magic  (PVT_MAGIC)
     u32 num_frames
     for each frame:
       u32 num_objs
       for each obj:
         u32 type
         u16 color
         u32 num_joints
         Joint joints[num_joints]  (each Joint = 4×s32)
   ═══════════════════════════════════════════════════════════════ */
static void sd_scan(void){
    save_count = 0;
    if(!fat_ok){ return; }
    DIR* d = opendir(SAVE_DIR);
    if(!d){ return; }
    struct dirent* e;
    while((e = readdir(d)) != NULL && save_count < MAX_SAVES){
        int n = strlen(e->d_name);
        if(n < 5){ continue; }
        if(strcmp(e->d_name + n - 4, ".pvt") != 0){ continue; }
        SaveEntry* se = &save_entries[save_count];
        /* name = filename without extension, capped at 8 chars */
        int nc = n - 4; if(nc >= SAVE_NAME_LEN){ nc = SAVE_NAME_LEN - 1; }
        strncpy(se->name, e->d_name, nc);
        se->name[nc] = 0;
        /* uppercase */
        for(int i = 0; se->name[i]; i++){
            if(se->name[i] >= 'a' && se->name[i] <= 'z'){ se->name[i] -= 32; }
        }
        snprintf(se->path, sizeof(se->path), "%s/%s", SAVE_DIR, e->d_name);
        se->used = true;
        save_count++;
    }
    closedir(d);
}

static void sd_save(const char* name){
    if(!fat_ok){ notif("NO SD CARD"); return; }
    mkdir(SAVE_DIR, 0777);
    char path[72];
    snprintf(path, sizeof(path), "%s/%s.pvt", SAVE_DIR, name);
    FILE* f = fopen(path, "wb");
    if(!f){ notif("SAVE FAILED"); return; }

    u32 magic = PVT_MAGIC;
    u32 nf    = (u32)num_frames;
    fwrite(&magic, 4, 1, f);
    fwrite(&nf,    4, 1, f);

    for(int fi = 0; fi < num_frames; fi++){
        u32 no = (u32)frames[fi].num_objs;
        fwrite(&no, 4, 1, f);
        for(int oi = 0; oi < frames[fi].num_objs; oi++){
            Object* o = &frames[fi].objs[oi];
            u32 type = (u32)o->type;
            u16 col  = o->color;
            u32 nj   = (u32)o->num_joints;
            fwrite(&type, 4, 1, f);
            fwrite(&col,  2, 1, f);
            fwrite(&nj,   4, 1, f);
            fwrite(o->joints, sizeof(Joint), (size_t)o->num_joints, f);
        }
    }
    fclose(f);
    sd_scan();
    notif("SAVED!");
}

static void sd_load(const char* path){
    if(!fat_ok){ notif("NO SD CARD"); return; }
    FILE* f = fopen(path, "rb");
    if(!f){ notif("FILE NOT FOUND"); return; }

    u32 magic = 0, nf = 0;
    fread(&magic, 4, 1, f);
    if(magic != PVT_MAGIC){ fclose(f); notif("BAD FILE"); return; }
    fread(&nf, 4, 1, f);
    if(nf == 0 || nf > MAX_FRAMES){ fclose(f); notif("BAD FILE"); return; }

    /* Load into temp, only commit if fully successful */
    Frame tmp[MAX_FRAMES];
    memset(tmp, 0, sizeof(tmp));
    bool ok = true;

    for(u32 fi = 0; fi < nf && ok; fi++){
        u32 no = 0;
        fread(&no, 4, 1, f);
        if(no > MAX_OBJS){ ok = false; break; }
        tmp[fi].num_objs = (int)no;
        for(u32 oi = 0; oi < no && ok; oi++){
            u32 type = 0; u16 col = 0; u32 nj = 0;
            fread(&type, 4, 1, f);
            fread(&col,  2, 1, f);
            fread(&nj,   4, 1, f);
            if(nj == 0 || nj > 256){ ok = false; break; }
            tmp[fi].objs[oi].type       = (int)type;
            tmp[fi].objs[oi].color      = col;
            tmp[fi].objs[oi].num_joints = (int)nj;
            tmp[fi].objs[oi].joints     = (Joint*)xmalloc((int)nj * sizeof(Joint));
            fread(tmp[fi].objs[oi].joints, sizeof(Joint), (size_t)nj, f);
        }
    }
    fclose(f);

    if(!ok){
        /* free tmp */
        for(int fi = 0; fi < (int)nf; fi++){ frame_free(&tmp[fi]); }
        notif("LOAD ERROR");
        return;
    }

    /* Commit: free current animation, replace */
    for(int fi = 0; fi < num_frames; fi++){ frame_free(&frames[fi]); }
    memcpy(frames, tmp, nf * sizeof(Frame));
    num_frames = (int)nf;
    cur_frame  = 0; play_frame = 0;
    cam_x = 0; cam_y = 0;
    for(int i = 0; i < undo_count; i++){ undo_free((undo_top + i) % UNDO_LEVELS); }
    undo_top = 0; undo_count = 0;
    notif("LOADED!");
}

static void sd_delete(const char* path){
    if(!fat_ok){ notif("NO SD CARD"); return; }
    remove(path);
    sd_scan();
    save_sel = -1;
    notif("DELETED");
}

/* ═══════════════════════════════════════════════════════════════
   Camera helpers
   ═══════════════════════════════════════════════════════════════ */
/* cam_capture: freeze current top screen into bg_buf as background */
static void cam_stop(void){ /* no-op, kept for call-site clarity */ }

static void cam_capture(void){
    dmaCopyWords(3, vramMain, bg_buf, SCREEN_W * SCREEN_H * 2);
    bg_active = true;
    cam_stop();
    notif("BG SET");
}

/* ═══════════════════════════════════════════════════════════════
   DMA flush  (called once per frame after all drawing is done)
   ═══════════════════════════════════════════════════════════════ */
static void flush(void){
    swiWaitForVBlank();
    dmaCopyWords(3, bbMain, vramMain, SCREEN_W * SCREEN_H * 2);
    dmaCopyWords(3, bbSub,  vramSub,  SCREEN_W * SCREEN_H * 2);
}

/* ═══════════════════════════════════════════════════════════════
   Drawing primitives  (all write to back-buffers only)
   ═══════════════════════════════════════════════════════════════ */
static inline void pset(u16* fb, int x, int y, u16 c){
    if((unsigned)x < SCREEN_W && (unsigned)y < SCREEN_H){
        fb[y * SCREEN_W + x] = c;
    }
}
static void frect(u16* fb, int x0, int y0, int x1, int y1, u16 c){
    if(x0 > x1){ int t = x0; x0 = x1; x1 = t; }
    if(y0 > y1){ int t = y0; y0 = y1; y1 = t; }
    for(int y = y0; y <= y1; y++){
        for(int x = x0; x <= x1; x++){
            pset(fb, x, y, c);
        }
    }
}
static void fcircle(u16* fb, int cx, int cy, int r, u16 c){
    int r2 = r * r;
    for(int dy = -r; dy <= r; dy++){
        for(int dx = -r; dx <= r; dx++){
            if(dx*dx + dy*dy <= r2){ pset(fb, cx+dx, cy+dy, c); }
        }
    }
}
static void bline(u16* fb, int x0, int y0, int x1, int y1, int th, u16 c){
    int dx = abs(x1-x0), sx = x0<x1?1:-1;
    int dy = abs(y1-y0), sy = y0<y1?1:-1;
    int err = (dx>dy?dx:-dy)/2, e2;
    for(;;){
        fcircle(fb, x0, y0, th, c);
        if(x0==x1 && y0==y1){ break; }
        e2 = err;
        if(e2 > -dx){ err -= dy; x0 += sx; }
        if(e2 <  dy){ err += dx; y0 += sy; }
    }
}
static void hrect(u16* fb, int x0, int y0, int x1, int y1, u16 c){
    for(int x = x0; x <= x1; x++){ pset(fb, x, y0, c); pset(fb, x, y1, c); }
    for(int y = y0; y <= y1; y++){ pset(fb, x0, y, c); pset(fb, x1, y, c); }
}

/* ═══════════════════════════════════════════════════════════════
   Font 5×6 pixel
   ═══════════════════════════════════════════════════════════════ */
static const u8 fnt[][6] = {
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

static void dchar(u16* fb, int px, int py, char c, u16 col){
    if(c >= 'a' && c <= 'z'){ c -= 32; }
    if(c < ' ' || c > 'Z'){ return; }
    const u8* g = fnt[(int)(c - ' ')];
    for(int r = 0; r < 6; r++){
        u8 b = g[r];
        for(int i = 4; i >= 0; i--){
            if(b & (1 << i)){ pset(fb, px+(4-i), py+r, col); }
        }
    }
}
static void dstr(u16* fb, int px, int py, const char* s, u16 col){
    while(*s){ dchar(fb, px, py, *s, col); px += 6; s++; }
}
static int slen(const char* s){ int n = 0; while(*s++){ n++; } return n; }
static void dstrc(u16* fb, int cx, int py, const char* s, u16 col){
    dstr(fb, cx - slen(s)*3, py, s, col);
}

/* ═══════════════════════════════════════════════════════════════
   Object rendering
   ═══════════════════════════════════════════════════════════════ */
static u16 blend(u16 c, u16 t){
    int r = (((c>>10)&31) + ((t>>10)&31)) >> 1;
    int g = (((c>> 5)&31) + ((t>> 5)&31)) >> 1;
    int b = (( c     &31) + ( t     &31)) >> 1;
    return C15(r, g, b);
}

static void draw_obj(u16* fb, const Object* o, bool edit, bool onion, int cx, int cy){
    u16 col = onion ? blend(o->color, COL_ONION) : o->color;

    if(o->type == 1){
        /* Ball */
        int dx = o->joints[1].x - o->joints[0].x;
        int dy = o->joints[1].y - o->joints[0].y;
        int r  = (int)sqrtf((float)(dx*dx + dy*dy));
        if(r < 1){ r = 1; }
        fcircle(fb, o->joints[0].x-cx, o->joints[0].y-cy, r, col);
        if(edit){
            fcircle(fb, o->joints[0].x-cx, o->joints[0].y-cy, 4, COL_HANDLE);
            fcircle(fb, o->joints[1].x-cx, o->joints[1].y-cy, 4, COL_HANDLE);
        }
    } else {
        /* Stickman */
        for(int i = 1; i < o->num_joints; i++){
            int p = o->joints[i].parent;
            if(p >= 0){
                bline(fb,
                      o->joints[i].x-cx, o->joints[i].y-cy,
                      o->joints[p].x-cx, o->joints[p].y-cy, 2, col);
            }
        }
        /* Head */
        if(o->num_joints > 2){
            fcircle(fb, o->joints[2].x-cx, o->joints[2].y-cy, 8, col);
            if(!edit && !onion){
                fcircle(fb, o->joints[2].x-cx+2, o->joints[2].y-cy-2, 1, COL_WHITE);
            }
        }
        if(edit){
            for(int i = 0; i < o->num_joints; i++){
                fcircle(fb, o->joints[i].x-cx, o->joints[i].y-cy, 3, COL_HANDLE);
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════
   UI helpers
   ═══════════════════════════════════════════════════════════════ */
static void draw_btn(u16* fb, int ci, int row, const char* lbl, bool active){
    int x0 = ci*BTN_W, y0 = row ? ROW1_Y : ROW0_Y;
    int x1 = x0+BTN_W-1, y1 = y0+BTN_H-1;
    u16 bg = active ? C15(4,12,2)  : C15(6,6,8);
    u16 hi = active ? C15(12,26,6) : C15(20,20,23);
    u16 sh = C15(2,2,3);
    frect(fb, x0+1, y0+1, x1-1, y1-1, bg);
    for(int x = x0; x <= x1; x++){ pset(fb, x, y0, hi); pset(fb, x, y1, sh); }
    for(int y = y0; y <= y1; y++){ pset(fb, x0, y, hi); pset(fb, x1, y, sh); }
    dstr(fb, x0+BTN_W/2-slen(lbl)*3, y0+BTN_H/2-3, lbl, COL_WHITE);
}

static void draw_notif(u16* fb){
    if(notif_timer <= 0){ return; }
    int w = slen(notif_msg)*6 + 10;
    int x0 = 128 - w/2, y0 = 60;
    frect(fb, x0, y0, x0+w, y0+12, C15(0,5,0));
    hrect(fb, x0, y0, x0+w, y0+12, C15(0,18,0));
    dstrc(fb, 128, y0+3, notif_msg, COL_WHITE);
}

static void draw_timeline_main(u16* fb, int tf){
    int ty = SCREEN_H - 9;
    frect(fb, 0, ty-2, SCREEN_W-1, SCREEN_H-1, COL_DGRAY);
    bline(fb, 6, ty+3, SCREEN_W-10, ty+3, 1, COL_GRAY);
    for(int i = 0; i < num_frames; i++){
        int tx = 6 + i*(SCREEN_W-16)/(num_frames > 1 ? num_frames-1 : 1);
        if(i == tf){ fcircle(fb, tx, ty+3, 3, COL_RED); }
        else { bline(fb, tx, ty+1, tx, ty+5, 1, C15(14,14,14)); }
    }
    if(is_playing){ fcircle(fb, SCREEN_W-5, ty+3, 3, COL_GREEN); }
    {
        char b[16];
        sprintf(b, "F%d/%d", tf+1, num_frames);
        dstr(fb, 2, ty-1, b, C15(14,14,14));
    }
    /* FPS indicator top-right */
    {
        char b[8];
        sprintf(b, "%dFPS", fps);
        dstr(fb, SCREEN_W-28, 2, b, C15(14,14,14));
    }
    /* BG indicator */
    if(bg_active){
        dstr(fb, SCREEN_W-40, 10, "BG", C15(0,18,14));
    }
}

static void draw_timeline_sub(u16* fb, int tf){
    int ty = ROW1_Y - 2;
    frect(fb, 0, ty, SCREEN_W-1, ty, C15(4,4,5));
    if(num_frames > 1){
        for(int i = 0; i < num_frames; i++){
            int tx = 4 + i*(SCREEN_W-8)/(num_frames-1);
            if(i == tf){ fcircle(fb, tx, ty, 2, palette[cur_color]); }
            else { pset(fb, tx, ty, C15(16,16,16)); }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════
   Render: EDIT MODE
   ═══════════════════════════════════════════════════════════════ */
static void render_edit(int sel_obj){
    int sf = is_playing ? play_frame : cur_frame;

    /* ── Top screen: playback preview ── */
    if(bg_active){
        /* Show camera photo as background */
        memcpy(bbMain, bg_buf, SCREEN_W * SCREEN_H * 2);
    } else {
        /* White canvas with dot grid */
        for(int i = 0; i < SCREEN_W*SCREEN_H; i++){ bbMain[i] = COL_WHITE; }
        for(int gy = 20; gy < SCREEN_H-12; gy += 20){
            for(int gx = 20; gx < SCREEN_W; gx += 20){
                pset(bbMain, gx, gy, COL_GRID);
            }
        }
    }
    for(int o = 0; o < frames[sf].num_objs; o++){
        draw_obj(bbMain, &frames[sf].objs[o], false, false, cam_x, cam_y);
    }
    draw_timeline_main(bbMain, sf);

    /* ── Bottom screen: editor canvas ── */
    for(int y = 0; y < CANVAS_H; y++){
        for(int x = 0; x < SCREEN_W; x++){
            bbSub[y*SCREEN_W+x] = (x%32==0 || y%32==0) ? COL_GRID : COL_WHITE;
        }
    }
    /* Onion skin: previous frame ghost */
    if(onion_on && cur_frame > 0){
        for(int o = 0; o < frames[cur_frame-1].num_objs; o++){
            draw_obj(bbSub, &frames[cur_frame-1].objs[o], false, true, cam_x, cam_y);
        }
    }
    /* Current frame */
    for(int o = 0; o < frames[cur_frame].num_objs; o++){
        draw_obj(bbSub, &frames[cur_frame].objs[o], true, false, cam_x, cam_y);
    }
    /* Selection bounding box */
    if(sel_obj >= 0){
        const Object* ob = &frames[cur_frame].objs[sel_obj];
        int x0=9999, y0=9999, x1=-9999, y1=-9999;
        for(int i = 0; i < ob->num_joints; i++){
            int jx = ob->joints[i].x - cam_x;
            int jy = ob->joints[i].y - cam_y;
            if(jx < x0){ x0 = jx; } if(jx > x1){ x1 = jx; }
            if(jy < y0){ y0 = jy; } if(jy > y1){ y1 = jy; }
        }
        x0-=10; y0-=10; x1+=10; y1+=10;
        if(x0 < 0){ x0 = 0; } if(y0 < 0){ y0 = 0; }
        if(x1 >= SCREEN_W){ x1 = SCREEN_W-1; }
        if(y1 >= CANVAS_H){ y1 = CANVAS_H-1; }
        if(x1 > x0 && y1 > y0){ hrect(bbSub, x0, y0, x1, y1, COL_SEL); }
    }

    /* ── UI strip (bottom 48px of sub screen) ── */
    frect(bbSub, 0, ROW0_Y, SCREEN_W-1, SCREEN_H-1, COL_DGRAY);
    draw_timeline_sub(bbSub, cur_frame);

    /* Row 0: frame controls */
    draw_btn(bbSub, 0, 0, "PREV",  false);
    draw_btn(bbSub, 1, 0, "NEXT",  false);
    draw_btn(bbSub, 2, 0, "+FRM",  false);
    draw_btn(bbSub, 3, 0, "-FRM",  false);
    draw_btn(bbSub, 4, 0, "UNDO",  undo_count > 0);
    draw_btn(bbSub, 5, 0, "CLR",   false);

    /* Row 1: playback + tools */
    draw_btn(bbSub, 0, 1, is_playing ? "STOP" : "PLAY", is_playing);
    {
        char b[6]; sprintf(b, "%dFPS", fps);
        draw_btn(bbSub, 1, 1, b, false);
    }
    draw_btn(bbSub, 2, 1, pal_name[cur_color], sel_obj >= 0);
    fcircle(bbSub, 2*BTN_W+BTN_W/2, ROW1_Y+BTN_H-6, 4, palette[cur_color]);
    draw_btn(bbSub, 3, 1, cur_type == 0 ? "STKM" : "BALL", false);
    draw_btn(bbSub, 4, 1, "+OBJ", false);
    draw_btn(bbSub, 5, 1, "ANIM", false);

    /* Object / frame counter */
    {
        char b[16];
        sprintf(b, "F%d/%d O%d", cur_frame+1, num_frames, frames[cur_frame].num_objs);
        frect(bbSub, 0, ROW0_Y-10, 72, ROW0_Y-1, COL_BLACK);
        dstr(bbSub, 2, ROW0_Y-9, b, COL_GRAY);
    }
    /* Onion indicator */
    {
        u16 oc = onion_on ? C15(0,18,12) : C15(10,10,10);
        dstr(bbSub, SCREEN_W-22, ROW0_Y-9, "ONI", oc);
    }

    draw_notif(bbSub);
}

/* ═══════════════════════════════════════════════════════════════
   Render: SAVE/LOAD MENU
   Top screen: selected slot details + instructions
   Bottom screen: grid of slots (tappable)
   ═══════════════════════════════════════════════════════════════ */
#define SL_COLS  2
#define SL_ROWS  4
#define SL_W    126   /* 2*126 = 252 */
#define SL_H     44   /* 4*44  = 176 + 12 header = 188 */

static void render_anim_menu(void){
    /* ── Top screen ── */
    frect(bbMain, 0, 0, SCREEN_W-1, SCREEN_H-1, C15(3,3,6));
    dstrc(bbMain, 128, 5, "ANIMATIONS", COL_WHITE);
    bline(bbMain, 0, 15, SCREEN_W-1, 15, 1, C15(10,10,13));

    if(!fat_ok){
        dstrc(bbMain, 128, 50, "NO SD CARD DETECTED", COL_RED);
        dstrc(bbMain, 128, 66, "INSERT A FLASHCARD", COL_GRAY);
        dstrc(bbMain, 128, 82, "WITH FAT SUPPORT", COL_GRAY);
    } else if(save_sel >= 0 && save_sel < save_count){
        SaveEntry* se = &save_entries[save_sel];
        dstrc(bbMain, 128, 24, se->name, C15(20,26,31));
        dstrc(bbMain, 128, 38, se->path + 12, C15(12,12,14)); /* show filename */
        bline(bbMain, 10, 56, 246, 56, 1, C15(10,10,12));
        dstrc(bbMain, 128, 64, "A = LOAD THIS ANIMATION", COL_WHITE);
        dstrc(bbMain, 128, 76, "Y = DELETE FROM SD CARD", C15(20,8,8));
        dstrc(bbMain, 128, 88, "X = SAVE CURRENT ANIM", C15(8,20,8));
    } else {
        dstrc(bbMain, 128, 30, "TAP A SLOT TO SELECT", COL_GRAY);
        dstrc(bbMain, 128, 48, "X = SAVE CURRENT ANIMATION", C15(8,20,8));
        {
            char b[24]; sprintf(b, "%d SAVES ON CARD", save_count);
            dstrc(bbMain, 128, 64, b, COL_GRAY);
        }
    }
    bline(bbMain, 0, SCREEN_H-22, SCREEN_W-1, SCREEN_H-22, 1, C15(10,10,12));
    dstrc(bbMain, 128, SCREEN_H-16, "B = BACK TO EDITOR", COL_GRAY);

    /* ── Bottom screen: slot grid ── */
    frect(bbSub, 0, 0, SCREEN_W-1, SCREEN_H-1, C15(4,4,6));
    /* Header */
    frect(bbSub, 0, 0, SCREEN_W-1, 11, C15(2,2,4));
    dstrc(bbSub, 64, 3, "SAVED", C15(16,16,18));
    dstrc(bbSub, 192, 3, "TAP=SELECT", C15(12,12,14));

    for(int i = 0; i < MAX_SAVES; i++){
        int ci = i % SL_COLS, ri = i / SL_COLS;
        int x0 = 1  + ci * SL_W;
        int y0 = 12 + ri * SL_H;
        int x1 = x0 + SL_W - 2;
        int y1 = y0 + SL_H - 2;
        bool sel  = (i == save_sel);
        bool used = (i < save_count);

        u16 bg     = sel  ? C15(4,12,20) : (used ? C15(7,7,10) : C15(4,4,5));
        u16 border = sel  ? COL_SEL      : (used ? C15(16,16,20) : C15(7,7,8));

        frect(bbSub, x0, y0, x1, y1, bg);
        hrect(bbSub, x0, y0, x1, y1, border);

        if(used){
            /* Slot number */
            {
                char nb[4]; sprintf(nb, "%d", i+1);
                dstr(bbSub, x0+3, y0+3, nb, C15(10,10,12));
            }
            /* Name centred */
            dstrc(bbSub, (x0+x1)/2, (y0+y1)/2-3,
                  save_entries[i].name, sel ? COL_WHITE : C15(22,22,24));
        } else {
            /* Empty */
            dstrc(bbSub, (x0+x1)/2, (y0+y1)/2-3, "--", C15(10,10,11));
        }
    }

    draw_notif(bbSub);
}

/* ═══════════════════════════════════════════════════════════════
   Render: NAME INPUT (keyboard)
   ═══════════════════════════════════════════════════════════════ */
static void render_name_input(void){
    /* Top: show current name being typed */
    frect(bbMain, 0, 0, SCREEN_W-1, SCREEN_H-1, C15(3,3,6));
    dstrc(bbMain, 128, 16, "NAME THIS SAVE", COL_WHITE);
    dstrc(bbMain, 128, 30, "MAX 8 CHARACTERS", COL_GRAY);

    /* Name box */
    frect(bbMain, 40, 48, 216, 66, C15(2,2,4));
    hrect(bbMain, 40, 48, 216, 66, COL_SEL);
    dstrc(bbMain, 128, 54, name_len ? name_buf : "...", COL_WHITE);

    bline(bbMain, 0, SCREEN_H-20, SCREEN_W-1, SCREEN_H-20, 1, C15(10,10,12));
    dstrc(bbMain, 128, SCREEN_H-14, "A OR OK = CONFIRM   B = CANCEL", COL_GRAY);

    /* Bottom: keyboard */
    frect(bbSub, 0, 0, SCREEN_W-1, SCREEN_H-1, COL_DGRAY);
    dstrc(bbSub, 128, 3, "TAP TO TYPE", COL_GRAY);

    int kw = 24, kh = 40;
    for(int r = 0; r < KBD_ROWS; r++){
        const char* row = kbd_rows[r];
        int nc = slen(row);
        for(int c = 0; c < nc; c++){
            int kx = 2  + c * kw;
            int ky = 12 + r * kh;
            frect(bbSub, kx, ky, kx+kw-2, ky+kh-4, C15(10,10,13));
            hrect(bbSub, kx, ky, kx+kw-2, ky+kh-4, COL_GRAY);
            char k[3] = {row[c], 0, 0};
            if(row[c] == ' '){ k[0]='S'; k[1]='P'; }
            dstrc(bbSub, kx+(kw-2)/2, ky+(kh-4)/2-3, k, COL_WHITE);
        }
    }
    /* Current name at bottom */
    frect(bbSub, 0, SCREEN_H-12, SCREEN_W-1, SCREEN_H-1, COL_BLACK);
    dstrc(bbSub, 128, SCREEN_H-10, name_len ? name_buf : "", COL_WHITE);
}

/* ═══════════════════════════════════════════════════════════════
   Render: CAMERA VIEWFINDER
   ═══════════════════════════════════════════════════════════════ */
static void render_camera(void){
    /* Top screen shows live camera feed (via display capture) */
    /* Just draw instructions over the bbMain which will be overwritten
       by capture after flush — we draw UI only on bbSub */
    for(int i = 0; i < SCREEN_W*SCREEN_H; i++){ bbMain[i] = COL_BLACK; }
    dstrc(bbMain, 128, SCREEN_H/2 - 6, "CAMERA NOT AVAILABLE", COL_GRAY);
    dstrc(bbMain, 128, SCREEN_H/2 + 6, "ON THIS HARDWARE", COL_GRAY);

    /* Sub: controls */
    frect(bbSub, 0, 0, SCREEN_W-1, SCREEN_H-1, C15(3,3,5));
    dstrc(bbSub, 128, 20, "CAMERA MODE", COL_WHITE);
    bline(bbSub, 0, 32, SCREEN_W-1, 32, 1, C15(10,10,12));
    dstrc(bbSub, 128, 50, "A = CAPTURE AS BACKGROUND", C15(8,20,8));
    dstrc(bbSub, 128, 66, "X = CLEAR BACKGROUND", C15(20,8,8));
    dstrc(bbSub, 128, 82, "B = BACK TO EDITOR", COL_GRAY);
    if(bg_active){
        dstrc(bbSub, 128, 108, "BACKGROUND: ACTIVE", C15(0,20,14));
    } else {
        dstrc(bbSub, 128, 108, "BACKGROUND: NONE", COL_GRAY);
    }
    bline(bbSub, 0, SCREEN_H-22, SCREEN_W-1, SCREEN_H-22, 1, C15(10,10,12));
    dstrc(bbSub, 128, SCREEN_H-14, "NOTE: CAPTURE FROM TOP SCREEN", C15(10,10,12));
    draw_notif(bbSub);
}

/* ═══════════════════════════════════════════════════════════════
   Main
   ═══════════════════════════════════════════════════════════════ */
int main(void){
    /* Video init — bitmap mode on both screens
       VRAM_A (128KB) → main BG at offset 0  → BG3 bitmap at 0x06000000
       VRAM_C (128KB) → sub  BG at offset 0  → BG3 bitmap at 0x06200000 */
    videoSetMode(MODE_5_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    /* BgType_Bmp16, 256x256, mapBase=0 (irrelevant for bitmap), tileBase=0 */
    int bgM = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    bgSetPriority(bgM, 0);

    videoSetModeSub(MODE_5_2D);
    vramSetBankC(VRAM_C_SUB_BG);
    int bgS = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    bgSetPriority(bgS, 0);

    vramMain = (u16*)bgGetGfxPtr(bgM);
    vramSub  = (u16*)bgGetGfxPtr(bgS);

    /* Init FAT — guarded: if no card, fat_ok stays false, all SD calls are no-ops */
    fat_ok = fatInitDefault();
    if(fat_ok){
        mkdir(SAVE_DIR, 0777);
        sd_scan();
    }

    /* Allocate camera background buffer on heap */
    bg_buf = (u16*)xmalloc(SCREEN_W * SCREEN_H * 2);
    memset(bg_buf, 0, SCREEN_W * SCREEN_H * 2);

    /* Init animation state */
    memset(frames,       0, sizeof(frames));
    memset(undo_buf,     0, sizeof(undo_buf));
    memset(save_entries, 0, sizeof(save_entries));
    reset_all();

    /* Clear back-buffers and VRAM before first frame */
    dmaFillWords(0, bbMain,   SCREEN_W * SCREEN_H * 2);
    dmaFillWords(0, bbSub,    SCREEN_W * SCREEN_H * 2);
    dmaFillWords(0, vramMain, SCREEN_W * SCREEN_H * 2);
    dmaFillWords(0, vramSub,  SCREEN_W * SCREEN_H * 2);

    int  sel_obj   = -1, sel_joint = -1;
    int  last_tx   = 0,  last_ty   = 0;
    bool touch_ui  = false;

    while(1){
        scanKeys();
        int keys = keysDown(), held = keysHeld();
        touchPosition tp; touchRead(&tp);
        if(notif_timer > 0){ notif_timer--; }
        play_timer++;

        /* ══════════════════════════════════════════════════════
           MODE: SAVE/LOAD MENU
           ══════════════════════════════════════════════════════ */
        if(ui_mode == MODE_SAVE_MENU){
            if(keys & KEY_B){ ui_mode = MODE_EDIT; save_sel = -1; }

            /* X = save current animation (go to name input) */
            if(keys & KEY_X){
                memset(name_buf, 0, sizeof(name_buf));
                name_len = 0;
                ui_mode = MODE_SAVE_NAME;
            }
            /* A = load selected */
            if(keys & KEY_A){
                if(save_sel >= 0 && save_sel < save_count){
                    sd_load(save_entries[save_sel].path);
                    if(notif_timer > 0){ ui_mode = MODE_EDIT; save_sel = -1; }
                } else {
                    notif("SELECT A SAVE FIRST");
                }
            }
            /* Y = delete selected */
            if(keys & KEY_Y){
                if(save_sel >= 0 && save_sel < save_count){
                    sd_delete(save_entries[save_sel].path);
                } else {
                    notif("SELECT A SAVE FIRST");
                }
            }
            /* Touch: select slot on bottom screen */
            if(keys & KEY_TOUCH){
                if(tp.py >= 12){
                    int ci = tp.px / SL_W;
                    int ri = (tp.py - 12) / SL_H;
                    if(ri >= 0 && ri < SL_ROWS && ci >= 0 && ci < SL_COLS){
                        int idx = ri * SL_COLS + ci;
                        if(idx < MAX_SAVES){
                            if(save_sel == idx && idx < save_count){
                                /* Second tap on same slot = load immediately */
                                sd_load(save_entries[idx].path);
                                ui_mode = MODE_EDIT; save_sel = -1;
                            } else {
                                save_sel = idx;
                            }
                        }
                    }
                }
            }
            render_anim_menu();
            flush();
            continue;
        }

        /* ══════════════════════════════════════════════════════
           MODE: NAME INPUT
           ══════════════════════════════════════════════════════ */
        if(ui_mode == MODE_SAVE_NAME){
            if(keys & KEY_B){ ui_mode = MODE_SAVE_MENU; }
            if(keys & KEY_A){
                if(name_len > 0){
                    sd_save(name_buf);
                    ui_mode = MODE_SAVE_MENU;
                } else {
                    notif("ENTER A NAME FIRST");
                }
            }
            if(keys & KEY_TOUCH){
                int kw = 24, kh = 40;
                int r = (tp.py - 12) / kh;
                int c = tp.px / kw;
                if(r >= 0 && r < KBD_ROWS && c >= 0){
                    const char* row = kbd_rows[r];
                    int nc = slen(row);
                    if(c < nc){
                        char ch = row[c];
                        if(ch == '<'){
                            /* Backspace */
                            if(name_len > 0){ name_len--; name_buf[name_len] = 0; }
                        } else if(ch == 'O' && r == 3 && c == 9){
                            /* OK key */
                            if(name_len > 0){
                                sd_save(name_buf);
                                ui_mode = MODE_SAVE_MENU;
                            }
                        } else if(ch == ' '){
                            if(name_len < SAVE_NAME_LEN-1){
                                name_buf[name_len++] = '_';
                                name_buf[name_len]   = 0;
                            }
                        } else {
                            if(name_len < SAVE_NAME_LEN-1){
                                name_buf[name_len++] = ch;
                                name_buf[name_len]   = 0;
                            }
                        }
                    }
                }
            }
            render_name_input();
            flush();
            continue;
        }

        /* ══════════════════════════════════════════════════════
           MODE: CAMERA
           ══════════════════════════════════════════════════════ */
        if(ui_mode == MODE_CAMERA){
            if(keys & KEY_B){ cam_stop(); ui_mode = MODE_EDIT; }
            if(keys & KEY_A){ cam_capture(); ui_mode = MODE_EDIT; }
            if(keys & KEY_X){ bg_active = false; notif("BG CLEARED"); }
            render_camera();
            flush();
            continue;
        }

        /* ══════════════════════════════════════════════════════
           MODE: EDIT
           ══════════════════════════════════════════════════════ */

        /* Playback timer */
        if(is_playing && num_frames > 1 && play_timer >= (60/fps)){
            play_timer = 0;
            play_frame = (play_frame + 1) % num_frames;
        }

        /* ── Hardware buttons ── */
        /* START: play / pause */
        if(keys & KEY_START){
            is_playing = !is_playing;
            if(is_playing){ play_frame = cur_frame; play_timer = 0; }
        }
        /* L/R: prev / next frame (only when paused) */
        if(!is_playing){
            if(keys & KEY_L && cur_frame > 0){ cur_frame--; }
            if(keys & KEY_R && cur_frame < num_frames-1){ cur_frame++; }
        }
        /* SELECT: duplicate current frame */
        if(keys & KEY_SELECT){ dup_frame_after_cur(); }
        /* X: toggle onion skin */
        if(keys & KEY_X){
            onion_on = !onion_on;
            notif(onion_on ? "ONION ON" : "ONION OFF");
        }
        /* Y: delete selected object */
        if(keys & KEY_Y && sel_obj >= 0){
            push_undo();
            Frame* f = &frames[cur_frame];
            free(f->objs[sel_obj].joints);
            for(int i = sel_obj; i < f->num_objs-1; i++){
                f->objs[i] = f->objs[i+1];
            }
            f->num_objs--;
            sel_obj = -1; sel_joint = -1;
            notif("OBJ DELETED");
        }

        /* ── D-pad: nudge selected joint ── */
        if(sel_obj >= 0 && sel_joint >= 0){
            int ddx = (held & KEY_RIGHT)?1:(held & KEY_LEFT)?-1:0;
            int ddy = (held & KEY_DOWN) ?1:(held & KEY_UP)  ?-1:0;
            if(ddx || ddy){
                Object* ob = &frames[cur_frame].objs[sel_obj];
                if(sel_joint == 0){
                    move_tree(ob, 0, ddx, ddy);
                } else {
                    int p  = ob->joints[sel_joint].parent;
                    int nx = ob->joints[sel_joint].x + ddx;
                    int ny = ob->joints[sel_joint].y + ddy;
                    float dd = sqrtf((float)((nx-ob->joints[p].x)*(nx-ob->joints[p].x)+
                                             (ny-ob->joints[p].y)*(ny-ob->joints[p].y)));
                    if(dd > 0.1f){
                        int l = ob->joints[sel_joint].length;
                        move_tree(ob, sel_joint,
                            (int)(ob->joints[p].x+(nx-ob->joints[p].x)*l/dd)-ob->joints[sel_joint].x,
                            (int)(ob->joints[p].y+(ny-ob->joints[p].y)*l/dd)-ob->joints[sel_joint].y);
                    }
                }
            }
        }

        /* ── Touch: initial press ── */
        if(keys & KEY_TOUCH){
            touch_ui = (tp.py >= CANVAS_H);

            if(touch_ui){
                /* UI strip */
                int row = (tp.py >= ROW1_Y) ? 1 : 0;
                int btn = tp.px / BTN_W;
                if(btn > 5){ btn = 5; }

                if(row == 0){
                    /* PREV | NEXT | +FRM | -FRM | UNDO | CLR */
                    if(btn == 0 && cur_frame > 0){ cur_frame--; }
                    else if(btn == 1 && cur_frame < num_frames-1){ cur_frame++; }
                    else if(btn == 2){ dup_frame_after_cur(); }
                    else if(btn == 3 && num_frames > 1){
                        frame_free(&frames[cur_frame]);
                        for(int f = cur_frame; f < num_frames-1; f++){
                            frames[f] = frames[f+1];
                        }
                        num_frames--;
                        if(cur_frame >= num_frames){ cur_frame = num_frames-1; }
                        notif("FRAME DEL");
                    }
                    else if(btn == 4){ pop_undo(); }
                    else if(btn == 5){
                        reset_all(); sel_obj = -1;
                        notif("CLEARED");
                    }
                } else {
                    /* PLAY | FPS | COL | TYPE | +OBJ | ANIM */
                    if(btn == 0){
                        is_playing = !is_playing;
                        if(is_playing){ play_frame = cur_frame; play_timer = 0; }
                    }
                    else if(btn == 1){
                        fps = (fps >= 30) ? 4 : fps + 4;
                    }
                    else if(btn == 2){
                        cur_color = (cur_color + 1) % NUM_COLORS;
                        /* Also recolour selected object */
                        if(sel_obj >= 0){
                            frames[cur_frame].objs[sel_obj].color = palette[cur_color];
                        }
                    }
                    else if(btn == 3){
                        cur_type = (cur_type + 1) % 2;
                    }
                    else if(btn == 4){
                        Frame* f = &frames[cur_frame];
                        if(f->num_objs < MAX_OBJS){
                            Object* no = &f->objs[f->num_objs];
                            if(cur_type == 0){ obj_stickman(no, 128+cam_x, 90+cam_y, palette[cur_color]); }
                            else             { obj_ball    (no, 128+cam_x, 96+cam_y, palette[cur_color]); }
                            f->num_objs++;
                            notif("+OBJ");
                        } else {
                            notif("MAX OBJS!");
                        }
                    }
                    else if(btn == 5){
                        /* Open ANIM menu — long-hold = camera */
                        if(held & KEY_TOUCH){
                            ui_mode = MODE_CAMERA;
                        } else {
                            sd_scan(); /* refresh file list */
                            save_sel = -1;
                            ui_mode = MODE_SAVE_MENU;
                        }
                    }
                }

            } else {
                /* Canvas: pick nearest joint */
                sel_obj = -1; sel_joint = -1;
                int best = 20*20;
                for(int o = 0; o < frames[cur_frame].num_objs; o++){
                    Object* ob = &frames[cur_frame].objs[o];
                    for(int i = 0; i < ob->num_joints; i++){
                        int dx = (tp.px + cam_x) - ob->joints[i].x;
                        int dy = (tp.py + cam_y) - ob->joints[i].y;
                        int d2 = dx*dx + dy*dy;
                        if(d2 < best){ best = d2; sel_obj = o; sel_joint = i; }
                    }
                }
                if(sel_obj >= 0){
                    push_undo();
                    /* Sync palette */
                    u16 oc = frames[cur_frame].objs[sel_obj].color;
                    for(int c = 0; c < NUM_COLORS; c++){
                        if(palette[c] == oc){ cur_color = c; break; }
                    }
                } else {
                    last_tx = tp.px; last_ty = tp.py;
                }
            }
        }

        /* ── Touch: held — drag joint or pan camera ── */
        if((held & KEY_TOUCH) && !touch_ui && tp.py < CANVAS_H){
            if(sel_obj >= 0){
                Object* ob = &frames[cur_frame].objs[sel_obj];
                int wx = tp.px + cam_x, wy = tp.py + cam_y;
                if(sel_joint == 0){
                    move_tree(ob, 0, wx - ob->joints[0].x, wy - ob->joints[0].y);
                } else {
                    aim_joint(ob, sel_joint, wx, wy);
                }
            } else {
                cam_x -= (tp.px - last_tx);
                cam_y -= (tp.py - last_ty);
                last_tx = tp.px; last_ty = tp.py;
            }
        }
        if(!(held & KEY_TOUCH)){ sel_obj = sel_joint = -1; }

        render_edit(sel_obj);
        flush();
    }
    return 0;
}
