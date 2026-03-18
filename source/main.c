#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <sys/stat.h>

#define MAX_FRAMES 200
#define NUM_JOINTS 11
#define MAX_OBJS 10

typedef struct {
    int x, y;
    int parent;
    int length;
} Joint;

typedef struct {
    int type; // 0=stickman, 1=ball
    u16 color;
    int num_joints;
    Joint joints[NUM_JOINTS];
} Object;

typedef struct {
    Object objs[MAX_OBJS];
    int num_objs;
} Frame;

Frame frames[MAX_FRAMES];
int num_frames = 1;
int current_frame = 0;
int play_frame = 0;
int play_timer = 0;
bool is_playing = true;
int fps = 10;

Frame undo_buffer;
bool has_undo = false;

u16 palette[] = { 
    RGB15(0,0,0)|BIT(15), RGB15(31,0,0)|BIT(15), RGB15(0,25,0)|BIT(15), 
    RGB15(0,0,31)|BIT(15), RGB15(31,15,0)|BIT(15) 
};
int cur_color_idx = 0;
int cur_shape_type = 0;
int cam_x = 0, cam_y = 0;

void init_object(Object* obj, int type, int x, int y, u16 color) {
    obj->type = type;
    obj->color = color;
    if (type == 0) {
        obj->num_joints = 11;
        obj->joints[0] = (Joint){x, y, -1, 0};
        obj->joints[1] = (Joint){x, y-24, 0, 24};
        obj->joints[2] = (Joint){x, y-40, 1, 16};
        obj->joints[3] = (Joint){x-19, y-20, 1, 19};
        obj->joints[4] = (Joint){x-39, y-4,  3, 25};
        obj->joints[5] = (Joint){x+19, y-20, 1, 19};
        obj->joints[6] = (Joint){x+39, y-4,  5, 25};
        obj->joints[7] = (Joint){x-11, y+28, 0, 30};
        obj->joints[8] = (Joint){x-19, y+56, 7, 29};
        obj->joints[9] = (Joint){x+11, y+28, 0, 30};
        obj->joints[10] = (Joint){x+19, y+56, 9, 29};
    } else {
        obj->num_joints = 2; // ball
        obj->joints[0] = (Joint){x, y, -1, 0};
        obj->joints[1] = (Joint){x+15, y, 0, 15};
    }
}

void init_frames() {
    frames[0].num_objs = 1;
    init_object(&frames[0].objs[0], 0, 128, 90, palette[0]);
    num_frames = 1; current_frame = 0;
}

void save_undo() { undo_buffer = frames[current_frame]; has_undo = true; }
void do_undo() { if(has_undo) { frames[current_frame] = undo_buffer; has_undo = false; } }

bool is_descendant(Object* obj, int joint, int ancestor) {
    int p = obj->joints[joint].parent;
    while (p != -1) {
        if (p == ancestor) return true;
        p = obj->joints[p].parent;
    }
    return false;
}

void offset_subtree(Object* obj, int joint, int dx, int dy) {
    obj->joints[joint].x += dx; obj->joints[joint].y += dy;
    for (int i = 0; i < obj->num_joints; i++) {
        if (is_descendant(obj, i, joint)) {
            obj->joints[i].x += dx; obj->joints[i].y += dy;
        }
    }
}

void draw_solid_circle(u16* fb, int cx, int cy, int r, u16 color) {
    int r2 = r*r;
    for(int y=-r; y<=r; y++) {
        for(int x=-r; x<=r; x++) {
            if (x*x + y*y <= r2) {
                int px=cx+x, py=cy+y;
                if(px>=0 && px<256 && py>=0 && py<192) fb[py*256+px] = color;
            }
        }
    }
}

void draw_thick_line(u16* fb, int x0, int y0, int x1, int y1, int thickness, u16 color) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = abs(y1 - y0), sy = y0 < y1 ? 1 : -1; 
    int err = (dx > dy ? dx : -dy) / 2, e2;
    for(;;) {
        draw_solid_circle(fb, x0, y0, thickness, color);
        if (x0==x1 && y0==y1) break;
        e2 = err;
        if(e2 > -dx) { err -= dy; x0 += sx; }
        if(e2 < dy) { err += dx; y0 += sy; }
    }
}

void draw_object(u16* fb, Object* obj, bool is_edit, int cx, int cy) {
    if (obj->type == 1) { // Ball
        int dx = obj->joints[1].x - obj->joints[0].x;
        int dy = obj->joints[1].y - obj->joints[0].y;
        int r = (int)sqrt(dx*dx+dy*dy);
        draw_solid_circle(fb, obj->joints[0].x - cx, obj->joints[0].y - cy, r, obj->color);
        if (is_edit) {
            draw_solid_circle(fb, obj->joints[0].x-cx, obj->joints[0].y-cy, 3, RGB15(31,0,0)|BIT(15));
            draw_solid_circle(fb, obj->joints[1].x-cx, obj->joints[1].y-cy, 3, RGB15(31,0,0)|BIT(15));
        }
    } else { // Stickman
        for (int i = 1; i < obj->num_joints; i++) {
            int p = obj->joints[i].parent;
            if (p != -1) draw_thick_line(fb, obj->joints[i].x-cx, obj->joints[i].y-cy, obj->joints[p].x-cx, obj->joints[p].y-cy, 2, obj->color);
        }
        draw_solid_circle(fb, obj->joints[2].x-cx, obj->joints[2].y-cy, 8, obj->color);
        if(!is_edit) draw_solid_circle(fb, obj->joints[2].x-cx+2, obj->joints[2].y-cy-2, 1, RGB15(31,31,31)|BIT(15));
        if (is_edit) {
            for (int i = 0; i < obj->num_joints; i++) {
                draw_solid_circle(fb, obj->joints[i].x-cx, obj->joints[i].y-cy, 3, RGB15(31,0,0)|BIT(15));
            }
        }
    }
}

void export_to_sd() {
    if(!fatInitDefault()) return; // Attempt to Mount
    mkdir("fat:/pivot_export", 0777);
    u16* buf = (u16*)malloc(256*192*2);
    if (!buf) return;
    for(int f=0; f<num_frames; f++) {
        for(int i=0; i<256*192; i++) buf[i] = RGB15(31,31,31)|BIT(15);
        for(int o=0; o<frames[f].num_objs; o++) draw_object(buf, &frames[f].objs[o], false, cam_x, cam_y);
        char path[256];
        sprintf(path, "fat:/pivot_export/frame_%03d.bmp", f);
        FILE* out = fopen(path, "wb");
        if(out) {
            u8 header[54] = {'B','M', 0,0,0,0, 0,0,0,0, 54,0,0,0, 40,0,0,0, 0,1,0,0, 192,0,0,0, 1,0, 24,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0};
            *(int*)&header[2] = 54 + 256*192*3;
            fwrite(header, 1, 54, out);
            u8 row[256*3];
            for(int y=191; y>=0; y--) { // Bottom up BMP
                for(int x=0; x<256; x++) {
                    u16 c = buf[y*256+x];
                    row[x*3+0] = ((c>>10)&31)<<3; row[x*3+1] = ((c>>5)&31)<<3; row[x*3+2] = (c&31)<<3;
                }
                fwrite(row, 1, 256*3, out);
            }
            fclose(out);
        }
    }
    free(buf);
}

int main(void) {
    videoSetMode(MODE_5_2D); vramSetBankA(VRAM_A_MAIN_BG);
    int bgMain = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    videoSetModeSub(MODE_5_2D); vramSetBankC(VRAM_C_SUB_BG);
    int bgSub = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);

    init_frames();
    int sel_obj = -1, sel_joint = -1;
    int last_tx = 0, last_ty = 0;
    u16* fbMain = bgGetGfxPtr(bgMain);
    u16* fbSub = bgGetGfxPtr(bgSub);

    while (1) {
        swiWaitForVBlank(); scanKeys();
        int keys = keysDown(), held = keysHeld();
        touchPosition touch; touchRead(&touch);

        if(is_playing) {
            play_timer++;
            if (play_timer > (60/fps)) {
                play_timer = 0; if (num_frames > 0) play_frame = (play_frame + 1) % num_frames;
            }
        }
        
        for(int i = 0; i < 256*192; i++) fbMain[i] = RGB15(31,31,31)|BIT(15);
        if (num_frames > 0) {
            for(int o=0; o<frames[play_frame].num_objs; o++) draw_object(fbMain, &frames[play_frame].objs[o], false, cam_x, cam_y);
            // Draw interactive timeline on the main screen (user doesn't touch it but it stays out of the way)
            for(int y=180; y<192; y++) for(int x=0; x<256; x++) fbMain[y*256+x] = RGB15(20,20,20)|BIT(15);
            draw_thick_line(fbMain, 10, 186, 246, 186, 1, RGB15(15,15,15)|BIT(15));
            for(int i=0; i<num_frames; i++) {
                int tx = 10 + i * 236 / (num_frames > 1 ? num_frames - 1 : 1);
                if (i == play_frame) draw_solid_circle(fbMain, tx, 186, 3, RGB15(31,0,0)|BIT(15));
                else draw_thick_line(fbMain, tx, 184, tx, 188, 1, RGB15(0,0,0)|BIT(15));
            }
        }

        if (keys & KEY_TOUCH) {
            if (touch.py >= 144) {
                int row = (touch.py >= 168) ? 1 : 0;
                int btn = touch.px / 42; if (btn > 5) btn = 5;
                if (row == 0) {
                    if(btn==0 && current_frame>0) current_frame--;
                    else if(btn==1 && current_frame<num_frames-1) current_frame++;
                    else if(btn==2 && num_frames<MAX_FRAMES) {
                        for(int f=num_frames; f>current_frame+1; f--) frames[f] = frames[f-1];
                        current_frame++; num_frames++;
                    } else if(btn==3 && num_frames>1) {
                        for(int f=current_frame; f<num_frames-1; f++) frames[f] = frames[f+1];
                        num_frames--; if(current_frame>=num_frames) current_frame = num_frames-1;
                    } else if(btn==4) do_undo();
                    else if(btn==5) init_frames();
                } else {
                    if(btn==0) is_playing = !is_playing;
                    else if(btn==1) { fps+=5; if(fps>30) fps=5; }
                    else if(btn==2) { cur_color_idx=(cur_color_idx+1)%5; if(sel_obj!=-1) frames[current_frame].objs[sel_obj].color = palette[cur_color_idx]; }
                    else if(btn==3) cur_shape_type = (cur_shape_type+1)%2;
                    else if(btn==4 && frames[current_frame].num_objs < MAX_OBJS) {
                        init_object(&frames[current_frame].objs[frames[current_frame].num_objs], cur_shape_type, 128+cam_x, 96+cam_y, palette[cur_color_idx]);
                        frames[current_frame].num_objs++;
                    } else if(btn==5) export_to_sd(); // Heavy block!
                }
            } else {
                sel_obj = -1; sel_joint = -1;
                int min_dist = 400; 
                for(int o=0; o<frames[current_frame].num_objs; o++) {
                    Object* obj = &frames[current_frame].objs[o];
                    for (int i=0; i<obj->num_joints; i++) {
                        int dx = (touch.px + cam_x) - obj->joints[i].x;
                        int dy = (touch.py + cam_y) - obj->joints[i].y;
                        if (dx*dx+dy*dy < min_dist) { min_dist = dx*dx+dy*dy; sel_obj = o; sel_joint = i; }
                    }
                }
                if (sel_obj != -1) save_undo();
                else { last_tx = touch.px; last_ty = touch.py; }
            }
        }
        if ((held & KEY_TOUCH) && touch.py < 144 && sel_obj != -1) {
            Object* obj = &frames[current_frame].objs[sel_obj];
            int wx = touch.px + cam_x, wy = touch.py + cam_y;
            if (sel_joint == 0) offset_subtree(obj, 0, wx - obj->joints[0].x, wy - obj->joints[0].y);
            else {
                int p = obj->joints[sel_joint].parent;
                float dist = sqrt(pow(wx - obj->joints[p].x, 2) + pow(wy - obj->joints[p].y, 2));
                if (dist > 0.1f) {
                    offset_subtree(obj, sel_joint, (obj->joints[p].x + ((wx-obj->joints[p].x)*obj->joints[sel_joint].length)/dist) - obj->joints[sel_joint].x,
                                                 (obj->joints[p].y + ((wy-obj->joints[p].y)*obj->joints[sel_joint].length)/dist) - obj->joints[sel_joint].y);
                }
            }
        } else if ((held & KEY_TOUCH) && touch.py < 144 && sel_obj == -1) {
            cam_x -= (touch.px - last_tx); cam_y -= (touch.py - last_ty);
            last_tx = touch.px; last_ty = touch.py;
        } else if (!(held & KEY_TOUCH)) sel_obj = -1;

        for(int i=0; i<256*144; i++) fbSub[i] = RGB15(31,31,31)|BIT(15);
        if (current_frame > 0) {
            for(int o=0; o<frames[current_frame-1].num_objs; o++) draw_object(fbSub, &frames[current_frame-1].objs[o], false, cam_x, cam_y);
        }
        for(int o=0; o<frames[current_frame].num_objs; o++) draw_object(fbSub, &frames[current_frame].objs[o], true, cam_x, cam_y);

        for(int y=144; y<192; y++) {
            for(int x=0; x<256; x++) fbSub[y*256+x] = RGB15(15,15,15)|BIT(15);
            if(y==144 || y==168 || y==191) for(int x=0; x<256; x++) fbSub[y*256+x]=RGB15(0,0,0)|BIT(15);
        }
        for(int i=1; i<=5; i++) for(int y=144; y<192; y++) fbSub[y*256+i*42]=RGB15(0,0,0)|BIT(15);
        
        u16 ic = RGB15(31,31,31)|BIT(15);
        // ROW 1: <, >, +, -, U, CLR
        draw_thick_line(fbSub, 25, 151, 15, 156, 1, ic); draw_thick_line(fbSub, 15, 156, 25, 161, 1, ic); // PREV
        draw_thick_line(fbSub, 59, 151, 69, 156, 1, ic); draw_thick_line(fbSub, 69, 156, 59, 161, 1, ic); // NEXT
        draw_thick_line(fbSub, 105, 149, 105, 163, 1, ic); draw_thick_line(fbSub, 98, 156, 112, 156, 1, ic); // ADD
        draw_thick_line(fbSub, 140, 156, 154, 156, 1, ic); // DEL
        draw_thick_line(fbSub, 180, 151, 180, 160, 1, ic); draw_thick_line(fbSub, 180, 160, 194, 160, 1, ic); draw_thick_line(fbSub, 194, 160, 194, 151, 1, ic); // UNDO
        draw_thick_line(fbSub, 222, 149, 238, 163, 1, ic); draw_thick_line(fbSub, 222, 163, 238, 149, 1, ic); // CLR
        
        // ROW 2: PLAY, FPS, COL, SHAPE, ADD OBJ, EXP
        draw_thick_line(fbSub, 17, 175, 17, 185, 1, ic); draw_thick_line(fbSub, 17, 175, 27, 180, 1, ic); draw_thick_line(fbSub, 17, 185, 27, 180, 1, ic); // PLAY
        draw_thick_line(fbSub, 64, 175, 64, 185, 1, ic); draw_thick_line(fbSub, 59, 180, 69, 180, 1, ic); // FPS
        draw_solid_circle(fbSub, 105, 180, 6, palette[cur_color_idx]); // COL
        draw_solid_circle(fbSub, 147, 180, 4, ic); // SHAPE (ball)
        draw_thick_line(fbSub, 189, 175, 189, 185, 1, ic); draw_thick_line(fbSub, 184, 180, 194, 180, 1, ic); // ADD OBJ (+)
        draw_thick_line(fbSub, 222, 185, 238, 185, 2, ic); draw_thick_line(fbSub, 230, 175, 230, 183, 1, ic); draw_thick_line(fbSub, 230, 183, 226, 179, 1, ic); // EXP
    }
    return 0;
}
