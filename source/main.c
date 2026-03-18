/*
 * PivotDS - Pivot Animator for Nintendo DS
 * Enhanced version: bug fixes + professional UI
 */

#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>

/* ── Constants ────────────────────────────────────────────────── */
#define MAX_FRAMES   200
#define NUM_JOINTS    11
#define MAX_OBJS      10
#define UNDO_LEVELS    8
#define CANVAS_H     144   /* touch-screen canvas height */
#define UI_H          48   /* touch-screen UI height     */
#define SCREEN_W     256
#define SCREEN_H     192

/* ── Colours ──────────────────────────────────────────────────── */
#define COL_WHITE   (RGB15(31,31,31)|BIT(15))
#define COL_BLACK   (RGB15( 0, 0, 0)|BIT(15))
#define COL_GRAY    (RGB15(20,20,20)|BIT(15))
#define COL_DGRAY   (RGB15(10,10,10)|BIT(15))
#define COL_RED     (RGB15(31, 0, 0)|BIT(15))
#define COL_HANDLE  (RGB15(31,20, 0)|BIT(15))
#define COL_ONION   (RGB15(15,15,25)|BIT(15))

/* ── Types ────────────────────────────────────────────────────── */
typedef struct { int x, y, parent, length; } Joint;

typedef struct {
    int   type;            /* 0=stickman 1=ball */
    u16   color;
    int   num_joints;
    Joint joints[NUM_JOINTS];
} Object;

typedef struct {
    Object objs[MAX_OBJS];
    int    num_objs;
} Frame;

/* ── State ────────────────────────────────────────────────────── */
static Frame frames[MAX_FRAMES];
static int   num_frames  = 1;
static int   cur_frame   = 0;
static int   play_frame  = 0;
static int   play_timer  = 0;
static bool  is_playing  = false;   /* BUG FIX: start paused */
static int   fps         = 8;

/* Undo ring buffer */
static Frame undo_buf[UNDO_LEVELS];
static int   undo_top    = 0;
static int   undo_count  = 0;

/* Palette */
static const u16 palette[] = {
    RGB15( 0, 0, 0)|BIT(15),
    RGB15(31, 0, 0)|BIT(15),
    RGB15( 0,25, 0)|BIT(15),
    RGB15( 0, 0,31)|BIT(15),
    RGB15(31,15, 0)|BIT(15),
    RGB15(20, 0,25)|BIT(15),
    RGB15( 0,20,20)|BIT(15),
    RGB15(31,31, 0)|BIT(15),
};
static const char* palette_names[] = { "BLK","RED","GRN","BLU","ORG","PRP","TEL","YLW" };
#define NUM_COLORS 8

static int  cur_color_idx  = 0;
static int  cur_shape_type = 0;
static int  cam_x = 0, cam_y = 0;

/* FAT */
static bool fat_ok = false;

/* Notification */
static char  notif_msg[32] = "";
static int   notif_timer   = 0;
#define NOTIF_FRAMES 90

/* Framebuffers */
static u16* fbMain;
static u16* fbSub;

/* ══════════════════════════════════════════════════════════════
   Drawing primitives
   ══════════════════════════════════════════════════════════════ */
static inline void put_pixel(u16* fb, int x, int y, u16 col) {
    if ((unsigned)x < SCREEN_W && (unsigned)y < SCREEN_H)
        fb[y * SCREEN_W + x] = col;
}

static void fill_rect(u16* fb, int x0, int y0, int x1, int y1, u16 col) {
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            put_pixel(fb, x, y, col);
}

static void draw_circle(u16* fb, int cx, int cy, int r, u16 col) {
    int r2 = r * r;
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx*dx + dy*dy <= r2)
                put_pixel(fb, cx+dx, cy+dy, col);
}

static void draw_line(u16* fb, int x0, int y0, int x1, int y1, int thick, u16 col) {
    int dx = abs(x1-x0), sx = x0<x1?1:-1;
    int dy = abs(y1-y0), sy = y0<y1?1:-1;
    int err = (dx>dy?dx:-dy)/2, e2;
    for(;;) {
        draw_circle(fb, x0, y0, thick, col);
        if (x0==x1 && y0==y1) break;
        e2=err;
        if(e2>-dx){err-=dy;x0+=sx;}
        if(e2< dy){err+=dx;y0+=sy;}
    }
}

/* ── Tiny 5x6 pixel font ─────────────────────────────────────── */
static const u8 font5x6[][6] = {
    {0x00,0x00,0x00,0x00,0x00,0x00}, /* ' ' */
    {0x04,0x04,0x04,0x00,0x04,0x00}, /* '!' */
    {0x0A,0x0A,0x00,0x00,0x00,0x00}, /* '"' */
    {0x00,0x00,0x00,0x00,0x00,0x00}, /* '#' placeholder */
    {0x00,0x00,0x00,0x00,0x00,0x00}, /* '$' placeholder */
    {0x00,0x00,0x00,0x00,0x00,0x00}, /* '%' placeholder */
    {0x00,0x00,0x00,0x00,0x00,0x00}, /* '&' placeholder */
    {0x02,0x02,0x00,0x00,0x00,0x00}, /* ''' */
    {0x02,0x04,0x04,0x04,0x02,0x00}, /* '(' */
    {0x04,0x02,0x02,0x02,0x04,0x00}, /* ')' */
    {0x00,0x0A,0x04,0x0A,0x00,0x00}, /* '*' */
    {0x00,0x04,0x0E,0x04,0x00,0x00}, /* '+' */
    {0x00,0x00,0x00,0x02,0x02,0x04}, /* ',' */
    {0x00,0x00,0x0E,0x00,0x00,0x00}, /* '-' */
    {0x00,0x00,0x00,0x00,0x04,0x00}, /* '.' */
    {0x08,0x04,0x04,0x02,0x01,0x00}, /* '/' */
    {0x0E,0x11,0x11,0x11,0x0E,0x00}, /* '0' */
    {0x06,0x02,0x02,0x02,0x07,0x00}, /* '1' */
    {0x0E,0x01,0x06,0x08,0x0F,0x00}, /* '2' */
    {0x0E,0x01,0x06,0x01,0x0E,0x00}, /* '3' */
    {0x06,0x0A,0x1F,0x02,0x02,0x00}, /* '4' */
    {0x1F,0x10,0x1E,0x01,0x1E,0x00}, /* '5' */
    {0x0E,0x10,0x1E,0x11,0x0E,0x00}, /* '6' */
    {0x1F,0x01,0x02,0x04,0x04,0x00}, /* '7' */
    {0x0E,0x11,0x0E,0x11,0x0E,0x00}, /* '8' */
    {0x0E,0x11,0x0F,0x01,0x0E,0x00}, /* '9' */
    {0x00,0x04,0x00,0x04,0x00,0x00}, /* ':' */
    {0x00,0x04,0x00,0x04,0x04,0x00}, /* ';' */
    {0x02,0x04,0x08,0x04,0x02,0x00}, /* '<' */
    {0x00,0x0E,0x00,0x0E,0x00,0x00}, /* '=' */
    {0x08,0x04,0x02,0x04,0x08,0x00}, /* '>' */
    {0x0E,0x01,0x06,0x00,0x04,0x00}, /* '?' */
    {0x0E,0x11,0x17,0x10,0x0E,0x00}, /* '@' */
    {0x04,0x0A,0x1F,0x11,0x11,0x00}, /* 'A' */
    {0x1E,0x11,0x1E,0x11,0x1E,0x00}, /* 'B' */
    {0x0E,0x11,0x10,0x11,0x0E,0x00}, /* 'C' */
    {0x1C,0x12,0x11,0x12,0x1C,0x00}, /* 'D' */
    {0x1F,0x10,0x1C,0x10,0x1F,0x00}, /* 'E' */
    {0x1F,0x10,0x1C,0x10,0x10,0x00}, /* 'F' */
    {0x0E,0x10,0x17,0x11,0x0F,0x00}, /* 'G' */
    {0x11,0x11,0x1F,0x11,0x11,0x00}, /* 'H' */
    {0x0E,0x04,0x04,0x04,0x0E,0x00}, /* 'I' */
    {0x01,0x01,0x01,0x11,0x0E,0x00}, /* 'J' */
    {0x11,0x12,0x1C,0x12,0x11,0x00}, /* 'K' */
    {0x10,0x10,0x10,0x10,0x1F,0x00}, /* 'L' */
    {0x11,0x1B,0x15,0x11,0x11,0x00}, /* 'M' */
    {0x11,0x19,0x15,0x13,0x11,0x00}, /* 'N' */
    {0x0E,0x11,0x11,0x11,0x0E,0x00}, /* 'O' */
    {0x1E,0x11,0x1E,0x10,0x10,0x00}, /* 'P' */
    {0x0E,0x11,0x11,0x13,0x0F,0x00}, /* 'Q' */
    {0x1E,0x11,0x1E,0x12,0x11,0x00}, /* 'R' */
    {0x0F,0x10,0x0E,0x01,0x1E,0x00}, /* 'S' */
    {0x1F,0x04,0x04,0x04,0x04,0x00}, /* 'T' */
    {0x11,0x11,0x11,0x11,0x0E,0x00}, /* 'U' */
    {0x11,0x11,0x11,0x0A,0x04,0x00}, /* 'V' */
    {0x11,0x11,0x15,0x1B,0x11,0x00}, /* 'W' */
    {0x11,0x0A,0x04,0x0A,0x11,0x00}, /* 'X' */
    {0x11,0x0A,0x04,0x04,0x04,0x00}, /* 'Y' */
    {0x1F,0x02,0x04,0x08,0x1F,0x00}, /* 'Z' */
};

static void draw_char(u16* fb, int px, int py, char c, u16 col) {
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c < ' ' || c > 'Z') return;
    const u8* g = font5x6[c - ' '];
    for (int row = 0; row < 6; row++) {
        u8 bits = g[row];
        for (int b = 4; b >= 0; b--)
            if (bits & (1 << b))
                put_pixel(fb, px + (4-b), py + row, col);
    }
}

static void draw_str(u16* fb, int px, int py, const char* s, u16 col) {
    while (*s) { draw_char(fb, px, py, *s, col); px += 6; s++; }
}

static int str_len(const char* s) { int n=0; while(*s++) n++; return n; }

static void draw_str_center(u16* fb, int cx, int py, const char* s, u16 col) {
    draw_str(fb, cx - str_len(s)*3, py, s, col);
}

/* ══════════════════════════════════════════════════════════════
   Object helpers
   ══════════════════════════════════════════════════════════════ */
static void init_object(Object* obj, int type, int x, int y, u16 color) {
    obj->type  = type;
    obj->color = color;
    if (type == 0) {
        obj->num_joints = 11;
        obj->joints[0]  = (Joint){x,    y,     -1, 0};
        obj->joints[1]  = (Joint){x,    y-24,   0, 24};
        obj->joints[2]  = (Joint){x,    y-40,   1, 16};
        obj->joints[3]  = (Joint){x-19, y-20,   1, 19};
        obj->joints[4]  = (Joint){x-39, y-4,    3, 25};
        obj->joints[5]  = (Joint){x+19, y-20,   1, 19};
        obj->joints[6]  = (Joint){x+39, y-4,    5, 25};
        obj->joints[7]  = (Joint){x-11, y+28,   0, 30};
        obj->joints[8]  = (Joint){x-19, y+56,   7, 29};
        obj->joints[9]  = (Joint){x+11, y+28,   0, 30};
        obj->joints[10] = (Joint){x+19, y+56,   9, 29};
    } else {
        obj->num_joints = 2;
        obj->joints[0] = (Joint){x,    y,  -1, 0};
        obj->joints[1] = (Joint){x+15, y,   0, 15};
    }
}

static void init_frames(void) {
    memset(&frames[0], 0, sizeof(Frame));
    frames[0].num_objs = 1;
    init_object(&frames[0].objs[0], 0, 128, 90, palette[0]);
    num_frames = 1;
    cur_frame  = 0;
    play_frame = 0;
    cam_x = 0; cam_y = 0;          /* BUG FIX: reset camera */
    undo_top = 0; undo_count = 0;
}

static bool is_descendant(const Object* obj, int joint, int ancestor) {
    int p = obj->joints[joint].parent;
    while (p != -1) { if (p == ancestor) return true; p = obj->joints[p].parent; }
    return false;
}

static void offset_subtree(Object* obj, int joint, int dx, int dy) {
    obj->joints[joint].x += dx; obj->joints[joint].y += dy;
    for (int i = 0; i < obj->num_joints; i++)
        if (is_descendant(obj, i, joint))
            { obj->joints[i].x += dx; obj->joints[i].y += dy; }
}

/* ── Undo ─────────────────────────────────────────────────────── */
static void save_undo(void) {
    undo_buf[undo_top] = frames[cur_frame];
    undo_top = (undo_top + 1) % UNDO_LEVELS;
    if (undo_count < UNDO_LEVELS) undo_count++;
}

static void do_undo(void) {
    if (undo_count == 0) return;
    undo_top = (undo_top - 1 + UNDO_LEVELS) % UNDO_LEVELS;
    frames[cur_frame] = undo_buf[undo_top];
    undo_count--;
}

/* ── Notification ─────────────────────────────────────────────── */
static void show_notif(const char* msg) {
    strncpy(notif_msg, msg, sizeof(notif_msg)-1);
    notif_msg[sizeof(notif_msg)-1] = '\0';
    notif_timer = NOTIF_FRAMES;
}

/* ══════════════════════════════════════════════════════════════
   Draw objects
   ══════════════════════════════════════════════════════════════ */
static u16 tint_col(u16 c, u16 tint) {
    int r = (((c>>10)&31) + ((tint>>10)&31)) >> 1;
    int g = (((c>> 5)&31) + ((tint>> 5)&31)) >> 1;
    int b = (( c     &31) + ( tint     &31)) >> 1;
    return RGB15(r,g,b)|BIT(15);
}

static void draw_object(u16* fb, const Object* obj, bool is_edit, bool is_onion, int cx, int cy) {
    u16 col = is_onion ? tint_col(obj->color, COL_ONION) : obj->color;

    if (obj->type == 1) {
        int dx = obj->joints[1].x - obj->joints[0].x;
        int dy = obj->joints[1].y - obj->joints[0].y;
        int r  = (int)sqrtf((float)(dx*dx+dy*dy));
        if (r < 1) r = 1;
        draw_circle(fb, obj->joints[0].x-cx, obj->joints[0].y-cy, r, col);
        if (is_edit) {
            draw_circle(fb, obj->joints[0].x-cx, obj->joints[0].y-cy, 4, COL_HANDLE);
            draw_circle(fb, obj->joints[1].x-cx, obj->joints[1].y-cy, 4, COL_HANDLE);
        }
    } else {
        for (int i = 1; i < obj->num_joints; i++) {
            int p = obj->joints[i].parent;
            if (p >= 0)
                draw_line(fb,
                    obj->joints[i].x-cx, obj->joints[i].y-cy,
                    obj->joints[p].x-cx, obj->joints[p].y-cy, 2, col);
        }
        draw_circle(fb, obj->joints[2].x-cx, obj->joints[2].y-cy, 8, col);
        if (!is_edit && !is_onion)
            draw_circle(fb, obj->joints[2].x-cx+2, obj->joints[2].y-cy-2, 1, COL_WHITE);
        if (is_edit) {
            for (int i = 0; i < obj->num_joints; i++)
                draw_circle(fb, obj->joints[i].x-cx, obj->joints[i].y-cy, 3, COL_HANDLE);
        }
    }
}

/* ══════════════════════════════════════════════════════════════
   Export BMP (bug-fixed header)
   ══════════════════════════════════════════════════════════════ */
static void export_to_sd(void) {
    if (!fat_ok) { show_notif("NO FAT CARD"); return; }
    mkdir("fat:/pivot_export", 0777);

    u16* buf = (u16*)malloc(SCREEN_W * SCREEN_H * 2);
    if (!buf) { show_notif("NO MEMORY"); return; }

    for (int f = 0; f < num_frames; f++) {
        for (int i = 0; i < SCREEN_W*SCREEN_H; i++) buf[i] = COL_WHITE;
        for (int o = 0; o < frames[f].num_objs; o++)
            draw_object(buf, &frames[f].objs[o], false, false, cam_x, cam_y);

        char path[64];
        sprintf(path, "fat:/pivot_export/frame_%03d.bmp", f);
        FILE* out = fopen(path, "wb");
        if (out) {
            /* BUG FIX: correct BMP header with proper size fields */
            int data_sz = SCREEN_W * SCREEN_H * 3;
            u8 hdr[54];
            memset(hdr, 0, 54);
            hdr[0]='B'; hdr[1]='M';
            *(u32*)&hdr[ 2] = (u32)(54 + data_sz);
            *(u32*)&hdr[10] = 54;
            *(u32*)&hdr[14] = 40;
            *(s32*)&hdr[18] = SCREEN_W;
            *(s32*)&hdr[22] = SCREEN_H;
            *(u16*)&hdr[26] = 1;
            *(u16*)&hdr[28] = 24;
            *(u32*)&hdr[34] = (u32)data_sz;
            fwrite(hdr, 1, 54, out);

            u8 row[SCREEN_W * 3];
            for (int y = SCREEN_H-1; y >= 0; y--) {
                for (int x = 0; x < SCREEN_W; x++) {
                    u16 c = buf[y*SCREEN_W+x];
                    row[x*3+0] = (u8)(( c     &31)<<3);
                    row[x*3+1] = (u8)(((c>> 5)&31)<<3);
                    row[x*3+2] = (u8)(((c>>10)&31)<<3);
                }
                fwrite(row, 1, SCREEN_W*3, out);
            }
            fclose(out);
        }
    }
    free(buf);
    show_notif("EXPORT OK!");
}

/* ══════════════════════════════════════════════════════════════
   UI rendering
   ══════════════════════════════════════════════════════════════ */
#define BTN_W  42
#define BTN_H  23
#define ROW0_Y 144
#define ROW1_Y 168

static void draw_button(u16* fb, int col_idx, int row, const char* label, bool active) {
    int x0 = col_idx * BTN_W;
    int y0 = (row == 0) ? ROW0_Y : ROW1_Y;
    int x1 = x0 + BTN_W - 1;
    int y1 = y0 + BTN_H - 1;

    u16 bg = active ? RGB15(5,12,2)|BIT(15) : RGB15(8,8,10)|BIT(15);
    u16 hi = active ? RGB15(15,28,8)|BIT(15) : RGB15(22,22,25)|BIT(15);
    u16 sh = RGB15(2,2,3)|BIT(15);

    fill_rect(fb, x0+1, y0+1, x1-1, y1-1, bg);
    for (int x = x0; x <= x1; x++) { put_pixel(fb, x, y0, hi); put_pixel(fb, x, y1, sh); }
    for (int y = y0; y <= y1; y++) { put_pixel(fb, x0, y, hi); put_pixel(fb, x1, y, sh); }

    int lx = x0 + BTN_W/2 - str_len(label)*3;
    int ly = y0 + BTN_H/2 - 3;
    draw_str(fb, lx, ly, label, COL_WHITE);
}

static void draw_ui(u16* fb, int sel_obj) {
    /* Background */
    fill_rect(fb, 0, ROW0_Y, SCREEN_W-1, SCREEN_H-1, COL_DGRAY);

    /* Mini timeline strip between rows */
    {
        int tl_y = ROW1_Y - 2;
        fill_rect(fb, 0, tl_y, SCREEN_W-1, tl_y, RGB15(5,5,5)|BIT(15));
        if (num_frames > 1) {
            for (int i = 0; i < num_frames; i++) {
                int tx = 4 + i * (SCREEN_W-8) / (num_frames-1);
                if (i == cur_frame)
                    draw_circle(fb, tx, tl_y, 2, palette[cur_color_idx]);
                else
                    put_pixel(fb, tx, tl_y, RGB15(18,18,18)|BIT(15));
            }
        }
    }

    /* Row 0: frame controls */
    draw_button(fb, 0, 0, "PREV", false);
    draw_button(fb, 1, 0, "NEXT", false);
    draw_button(fb, 2, 0, "+FRM", false);
    draw_button(fb, 3, 0, "-FRM", false);
    draw_button(fb, 4, 0, "UNDO", undo_count > 0);
    draw_button(fb, 5, 0, "CLR",  false);

    /* Row 1: playback & tools */
    draw_button(fb, 0, 1, is_playing ? "STOP" : "PLAY", is_playing);
    {
        char f[6]; sprintf(f, "%dFPS", fps);
        draw_button(fb, 1, 1, f, false);
    }
    draw_button(fb, 2, 1, palette_names[cur_color_idx], sel_obj != -1);
    /* Colour dot */
    draw_circle(fb, 2*BTN_W + BTN_W/2, ROW1_Y + BTN_H - 6, 4, palette[cur_color_idx]);

    draw_button(fb, 3, 1, cur_shape_type == 0 ? "STKM" : "BALL", false);
    draw_button(fb, 4, 1, "+OBJ", false);
    draw_button(fb, 5, 1, "EXP",  false);

    /* Frame counter in canvas corner */
    {
        char buf[12];
        sprintf(buf, "F%d/%d", cur_frame+1, num_frames);
        fill_rect(fb, 0, ROW0_Y-10, 40, ROW0_Y-1, RGB15(0,0,0)|BIT(15));
        draw_str(fb, 2, ROW0_Y-9, buf, RGB15(20,20,20)|BIT(15));
    }

    /* Notification */
    if (notif_timer > 0) {
        int w = str_len(notif_msg)*6 + 8;
        int x0 = 128 - w/2, y0 = 60;
        fill_rect(fb, x0, y0, x0+w, y0+10, RGB15(0,6,0)|BIT(15));
        for (int x = x0; x <= x0+w; x++) { put_pixel(fb,x,y0,RGB15(0,18,0)|BIT(15)); put_pixel(fb,x,y0+10,RGB15(0,18,0)|BIT(15)); }
        draw_str_center(fb, 128, y0+2, notif_msg, COL_WHITE);
    }
}

/* Main-screen timeline */
static void draw_main_ui(u16* fb) {
    int ty = SCREEN_H - 9;
    fill_rect(fb, 0, ty-2, SCREEN_W-1, SCREEN_H-1, COL_DGRAY);
    draw_line(fb, 6, ty+3, SCREEN_W-10, ty+3, 1, COL_GRAY);
    for (int i = 0; i < num_frames; i++) {
        int tx = 6 + i * (SCREEN_W-16) / (num_frames > 1 ? num_frames-1 : 1);
        if (i == play_frame)
            draw_circle(fb, tx, ty+3, 3, COL_RED);
        else
            draw_line(fb, tx, ty+1, tx, ty+5, 1, RGB15(15,15,15)|BIT(15));
    }
    /* Playing dot */
    if (is_playing)
        draw_circle(fb, SCREEN_W-5, ty+3, 3, RGB15(0,25,0)|BIT(15));
    /* Frame label */
    {
        char buf[10]; sprintf(buf, "F%d", play_frame+1);
        draw_str(fb, 2, ty-1, buf, RGB15(15,15,15)|BIT(15));
    }
}

/* ══════════════════════════════════════════════════════════════
   Main
   ══════════════════════════════════════════════════════════════ */
int main(void) {
    videoSetMode(MODE_5_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    int bgMain = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);

    videoSetModeSub(MODE_5_2D);
    vramSetBankC(VRAM_C_SUB_BG);
    int bgSub = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);

    fbMain = bgGetGfxPtr(bgMain);
    fbSub  = bgGetGfxPtr(bgSub);

    fat_ok = fatInitDefault();  /* BUG FIX: init once at startup */

    init_frames();

    int  sel_obj     = -1, sel_joint = -1;
    int  last_tx     = 0,  last_ty   = 0;
    bool touch_in_ui = false;

    while (1) {
        swiWaitForVBlank();
        scanKeys();
        int keys = keysDown(), held = keysHeld();
        touchPosition touch;
        touchRead(&touch);

        if (notif_timer > 0) notif_timer--;

        /* ── Playback ── */
        if (is_playing && num_frames > 1) {
            play_timer++;
            if (play_timer >= (60 / fps)) {
                play_timer = 0;
                play_frame = (play_frame + 1) % num_frames;
            }
        }

        /* ── Hardware buttons ── */
        if (keys & KEY_START)  {
            is_playing = !is_playing;
            if (is_playing) { play_frame = cur_frame; play_timer = 0; }
        }
        if (!is_playing) {
            if (keys & KEY_L) { if (cur_frame > 0) { cur_frame--; } }
            if (keys & KEY_R) { if (cur_frame < num_frames-1) { cur_frame++; } }
        }
        if (keys & KEY_SELECT) {
            /* Shortcut: duplicate frame */
            if (num_frames < MAX_FRAMES) {
                for (int f = num_frames; f > cur_frame+1; f--) frames[f] = frames[f-1];
                frames[cur_frame+1] = frames[cur_frame];
                cur_frame++; num_frames++;
                show_notif("FRAME ADDED");
            }
        }
        /* D-Pad: fine-tune selected joint */
        if (sel_obj >= 0 && sel_joint >= 0) {
            int ddx = (held & KEY_RIGHT)?1:(held & KEY_LEFT)?-1:0;
            int ddy = (held & KEY_DOWN) ?1:(held & KEY_UP)  ?-1:0;
            if (ddx || ddy) {
                Object* obj = &frames[cur_frame].objs[sel_obj];
                if (sel_joint == 0) {
                    offset_subtree(obj, 0, ddx, ddy);
                } else {
                    int p  = obj->joints[sel_joint].parent;
                    int nx = obj->joints[sel_joint].x + ddx;
                    int ny = obj->joints[sel_joint].y + ddy;
                    float d = sqrtf((float)((nx-obj->joints[p].x)*(nx-obj->joints[p].x)+
                                            (ny-obj->joints[p].y)*(ny-obj->joints[p].y)));
                    if (d > 0.1f) {
                        int len = obj->joints[sel_joint].length;
                        offset_subtree(obj, sel_joint,
                            (int)(obj->joints[p].x+(nx-obj->joints[p].x)*len/d)-obj->joints[sel_joint].x,
                            (int)(obj->joints[p].y+(ny-obj->joints[p].y)*len/d)-obj->joints[sel_joint].y);
                    }
                }
            }
        }

        /* ── Touch: initial press ── */
        if (keys & KEY_TOUCH) {
            touch_in_ui = (touch.py >= CANVAS_H);
            if (touch_in_ui) {
                int row = (touch.py >= ROW1_Y) ? 1 : 0;
                int btn = touch.px / BTN_W; if (btn > 5) btn = 5;
                if (row == 0) {
                    if      (btn==0 && cur_frame>0) cur_frame--;
                    else if (btn==1 && cur_frame<num_frames-1) cur_frame++;
                    else if (btn==2 && num_frames<MAX_FRAMES) {
                        for (int f=num_frames; f>cur_frame+1; f--) frames[f]=frames[f-1];
                        frames[cur_frame+1] = frames[cur_frame]; /* BUG FIX: duplicate */
                        cur_frame++; num_frames++;
                        show_notif("FRAME ADDED");
                    }
                    else if (btn==3 && num_frames>1) {
                        for (int f=cur_frame; f<num_frames-1; f++) frames[f]=frames[f+1];
                        num_frames--; if(cur_frame>=num_frames) cur_frame=num_frames-1;
                        show_notif("FRAME DEL");
                    }
                    else if (btn==4) { do_undo(); show_notif("UNDO"); }
                    else if (btn==5) { init_frames(); sel_obj=-1; show_notif("CLEARED"); }
                } else {
                    if (btn==0) {
                        is_playing = !is_playing;
                        if (is_playing) { play_frame=cur_frame; play_timer=0; }
                    }
                    else if (btn==1) { fps = (fps>=30)?4:fps+4; }
                    else if (btn==2) {
                        cur_color_idx = (cur_color_idx+1) % NUM_COLORS;
                        if (sel_obj>=0) frames[cur_frame].objs[sel_obj].color = palette[cur_color_idx];
                    }
                    else if (btn==3) cur_shape_type = (cur_shape_type+1)%2;
                    else if (btn==4) {
                        if (frames[cur_frame].num_objs < MAX_OBJS) {
                            init_object(&frames[cur_frame].objs[frames[cur_frame].num_objs],
                                cur_shape_type, 128+cam_x, 96+cam_y, palette[cur_color_idx]);
                            frames[cur_frame].num_objs++;
                            show_notif("OBJ ADDED");
                        } else show_notif("MAX OBJS!");
                    }
                    else if (btn==5) export_to_sd();
                }
            } else {
                /* Canvas: pick nearest joint */
                /* BUG FIX: threshold in actual pixel distance squared (20px) */
                sel_obj = -1; sel_joint = -1;
                int min_d = 20*20;
                for (int o=0; o<frames[cur_frame].num_objs; o++) {
                    Object* obj = &frames[cur_frame].objs[o];
                    for (int i=0; i<obj->num_joints; i++) {
                        int dx = (touch.px+cam_x) - obj->joints[i].x;
                        int dy = (touch.py+cam_y) - obj->joints[i].y;
                        int d2 = dx*dx + dy*dy;
                        if (d2 < min_d) { min_d=d2; sel_obj=o; sel_joint=i; }
                    }
                }
                if (sel_obj >= 0) {
                    save_undo();
                    /* Sync palette to selected object */
                    u16 oc = frames[cur_frame].objs[sel_obj].color;
                    for (int c=0; c<NUM_COLORS; c++) if (palette[c]==oc) { cur_color_idx=c; break; }
                } else {
                    last_tx = touch.px; last_ty = touch.py;
                }
            }
        }

        /* ── Touch: held ── */
        if ((held & KEY_TOUCH) && !touch_in_ui && touch.py < CANVAS_H) {
            if (sel_obj >= 0) {
                Object* obj = &frames[cur_frame].objs[sel_obj];
                int wx = touch.px + cam_x, wy = touch.py + cam_y;
                if (sel_joint == 0) {
                    offset_subtree(obj, 0, wx-obj->joints[0].x, wy-obj->joints[0].y);
                } else {
                    int p = obj->joints[sel_joint].parent;
                    float dist = sqrtf((float)((wx-obj->joints[p].x)*(wx-obj->joints[p].x)+
                                               (wy-obj->joints[p].y)*(wy-obj->joints[p].y)));
                    if (dist > 0.1f) {
                        int len = obj->joints[sel_joint].length;
                        offset_subtree(obj, sel_joint,
                            (int)(obj->joints[p].x+(wx-obj->joints[p].x)*len/dist)-obj->joints[sel_joint].x,
                            (int)(obj->joints[p].y+(wy-obj->joints[p].y)*len/dist)-obj->joints[sel_joint].y);
                    }
                }
            } else {
                /* Pan */
                cam_x -= (touch.px - last_tx);
                cam_y -= (touch.py - last_ty);
                last_tx = touch.px; last_ty = touch.py;
            }
        }
        if (!(held & KEY_TOUCH)) sel_obj = sel_joint = -1;

        /* ══════════════════════════════════════════════════════
           Render top screen: playback preview
           ══════════════════════════════════════════════════════ */
        for (int i = 0; i < SCREEN_W*SCREEN_H; i++) fbMain[i] = COL_WHITE;

        /* Subtle dot grid */
        for (int gy = 20; gy < SCREEN_H-12; gy += 20)
            for (int gx = 20; gx < SCREEN_W; gx += 20)
                put_pixel(fbMain, gx, gy, RGB15(27,27,27)|BIT(15));

        /* BUG FIX: show cur_frame when paused, play_frame when playing */
        int show_f = is_playing ? play_frame : cur_frame;
        for (int o = 0; o < frames[show_f].num_objs; o++)
            draw_object(fbMain, &frames[show_f].objs[o], false, false, cam_x, cam_y);

        draw_main_ui(fbMain);

        /* ══════════════════════════════════════════════════════
           Render sub screen: editor
           ══════════════════════════════════════════════════════ */
        /* Canvas background with grid */
        for (int y = 0; y < CANVAS_H; y++)
            for (int x = 0; x < SCREEN_W; x++) {
                bool grid = (x%32==0 || y%32==0);
                fbSub[y*SCREEN_W+x] = grid ? RGB15(28,28,28)|BIT(15) : COL_WHITE;
            }

        /* Onion skin: frame before */
        if (cur_frame > 0)
            for (int o = 0; o < frames[cur_frame-1].num_objs; o++)
                draw_object(fbSub, &frames[cur_frame-1].objs[o], false, true, cam_x, cam_y);

        /* Current frame */
        for (int o = 0; o < frames[cur_frame].num_objs; o++)
            draw_object(fbSub, &frames[cur_frame].objs[o], true, false, cam_x, cam_y);

        /* Selection bounding box */
        if (sel_obj >= 0) {
            const Object* obj = &frames[cur_frame].objs[sel_obj];
            int minx=9999,miny=9999,maxx=-9999,maxy=-9999;
            for (int i=0; i<obj->num_joints; i++) {
                int jx=obj->joints[i].x-cam_x, jy=obj->joints[i].y-cam_y;
                if(jx<minx)minx=jx; if(jx>maxx)maxx=jx;
                if(jy<miny)miny=jy; if(jy>maxy)maxy=jy;
            }
            minx-=10; miny-=10; maxx+=10; maxy+=10;
            if(minx<0)minx=0; if(miny<0)miny=0;
            if(maxx>=SCREEN_W)maxx=SCREEN_W-1;
            if(maxy>=CANVAS_H)maxy=CANVAS_H-1;
            if(maxx>minx && maxy>miny) {
                u16 hl = RGB15(0,18,31)|BIT(15);
                for(int x=minx;x<=maxx;x++){put_pixel(fbSub,x,miny,hl);put_pixel(fbSub,x,maxy,hl);}
                for(int y=miny;y<=maxy;y++){put_pixel(fbSub,minx,y,hl);put_pixel(fbSub,maxx,y,hl);}
            }
        }

        draw_ui(fbSub, sel_obj);
    }

    return 0;
}
