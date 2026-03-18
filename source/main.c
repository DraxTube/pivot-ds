#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define MAX_FRAMES 200
#define NUM_JOINTS 11

typedef struct {
    int x, y;
    int parent;
    int length;
} Joint;

Joint frames[MAX_FRAMES][NUM_JOINTS];
u16 frame_colors[MAX_FRAMES];
int num_frames = 1;
int current_frame = 0;
int play_frame = 0;
int play_timer = 0;

Joint joints[NUM_JOINTS];
Joint undo_buffer[NUM_JOINTS];
bool has_undo = false;

u16 palette[] = { 
    RGB15(0,0,0)|BIT(15),     // Black
    RGB15(31,0,0)|BIT(15),    // Red
    RGB15(0,25,0)|BIT(15),    // Green
    RGB15(0,0,31)|BIT(15),    // Blue
    RGB15(31,15,0)|BIT(15)    // Orange
};
int cur_color_idx = 0;

int cam_x = 0;
int cam_y = 0;

void init_joints() {
    // Smoother, slightly scaled down stickman to fit better
    joints[0] = (Joint){128, 90, -1, 0};   // hip
    joints[1] = (Joint){128, 66, 0, 24};   // neck
    joints[2] = (Joint){128, 50, 1, 16};   // head
    joints[3] = (Joint){109, 70, 1, 19};   // L shoulder
    joints[4] = (Joint){89,  86, 3, 25};   // L hand
    joints[5] = (Joint){147, 70, 1, 19};   // R shoulder
    joints[6] = (Joint){167, 86, 5, 25};   // R hand
    joints[7] = (Joint){117, 118, 0, 30};  // L knee
    joints[8] = (Joint){109, 146, 7, 29};  // L foot
    joints[9] = (Joint){139, 118, 0, 30};  // R knee
    joints[10] = (Joint){147, 146, 9, 29}; // R foot
    
    for(int i = 0; i < NUM_JOINTS; i++) frames[0][i] = joints[i];
    frame_colors[0] = palette[0];
    num_frames = 1;
    current_frame = 0;
    cam_x = 0;
    cam_y = 0;
    cur_color_idx = 0;
    has_undo = false;
}

void load_frame(int f) {
    for(int i=0; i<NUM_JOINTS; i++) joints[i] = frames[f][i];
    u16 c = frame_colors[f];
    for(int i=0; i<5; i++) if(palette[i] == c) cur_color_idx = i;
}

void save_frame(int f) {
    for(int i=0; i<NUM_JOINTS; i++) frames[f][i] = joints[i];
    frame_colors[f] = palette[cur_color_idx];
}

void save_undo() {
    for(int i=0; i<NUM_JOINTS; i++) undo_buffer[i] = joints[i];
    has_undo = true;
}

void do_undo() {
    if (has_undo) {
        for(int i=0; i<NUM_JOINTS; i++) joints[i] = undo_buffer[i];
        save_frame(current_frame);
        has_undo = false;
    }
}

bool is_descendant(int joint, int ancestor) {
    int p = joints[joint].parent;
    while (p != -1) {
        if (p == ancestor) return true;
        p = joints[p].parent;
    }
    return false;
}

void offset_subtree(int joint, int dx, int dy) {
    joints[joint].x += dx;
    joints[joint].y += dy;
    for (int i = 0; i < NUM_JOINTS; i++) {
        if (is_descendant(i, joint)) {
            joints[i].x += dx;
            joints[i].y += dy;
        }
    }
}

void draw_solid_circle(u16* fb, int cx, int cy, int r, u16 color) {
    int r2 = r*r;
    for(int y=-r; y<=r; y++) {
        for(int x=-r; x<=r; x++) {
            if (x*x + y*y <= r2) {
                int px = cx+x;
                int py = cy+y;
                if(px>=0 && px<256 && py>=0 && py<192) {
                    fb[py*256+px] = color;
                }
            }
        }
    }
}

void draw_line(u16* fb, int x0, int y0, int x1, int y1, u16 color) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = abs(y1 - y0), sy = y0 < y1 ? 1 : -1; 
    int err = (dx > dy ? dx : -dy) / 2, e2;
    for(;;) {
        if (x0>=0 && x0<256 && y0>=0 && y0<192) fb[y0*256+x0] = color;
        if (x0 == x1 && y0 == y1) break;
        e2 = err;
        if (e2 > -dx) { err -= dy; x0 += sx; }
        if (e2 < dy) { err += dx; y0 += sy; }
    }
}

void draw_thick_line(u16* fb, int x0, int y0, int x1, int y1, int thickness, u16 color) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = abs(y1 - y0), sy = y0 < y1 ? 1 : -1; 
    int err = (dx > dy ? dx : -dy) / 2, e2;
    for(;;) {
        draw_solid_circle(fb, x0, y0, thickness, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = err;
        if (e2 > -dx) { err -= dy; x0 += sx; }
        if (e2 < dy) { err += dx; y0 += sy; }
    }
}

void draw_stickman(u16* fb, Joint* jts, u16 color, bool is_edit, int cx, int cy) {
    for (int i = 1; i < NUM_JOINTS; i++) {
        int p = jts[i].parent;
        if (p != -1) {
            draw_thick_line(fb, jts[i].x - cx, jts[i].y - cy, jts[p].x - cx, jts[p].y - cy, 2, color);
        }
    }
    draw_solid_circle(fb, jts[2].x - cx, jts[2].y - cy, 8, color);

    if (is_edit) {
        for (int i = 0; i < NUM_JOINTS; i++) {
            u16 jc = (i==0) ? (RGB15(31,0,0)|BIT(15)) : (RGB15(31,0,0)|BIT(15));
            draw_solid_circle(fb, jts[i].x - cx, jts[i].y - cy, 3, jc);
        }
    }
}

int main(void) {
    videoSetMode(MODE_5_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    int bgMain = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);

    videoSetModeSub(MODE_5_2D);
    vramSetBankC(VRAM_C_SUB_BG);
    int bgSub = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);

    init_joints();
    int selected_joint = -1;
    int last_tx = 0, last_ty = 0;

    u16* fbMain = bgGetGfxPtr(bgMain);
    u16* fbSub = bgGetGfxPtr(bgSub);

    while (1) {
        swiWaitForVBlank();
        scanKeys();
        int keys = keysDown();
        int held = keysHeld();
        touchPosition touch;
        touchRead(&touch);

        // TOP SCREEN: Live preview
        play_timer++;
        if (play_timer > 6) { // ~10fps
            play_timer = 0;
            if (num_frames > 0) {
                play_frame++;
                if (play_frame >= num_frames) play_frame = 0;
            }
        }

        for(int i = 0; i < 256 * 192; i++) fbMain[i] = RGB15(31,31,31) | BIT(15); // white
        if (num_frames > 0) {
            draw_stickman(fbMain, frames[play_frame], frame_colors[play_frame], false, cam_x, cam_y);
        }

        // BOTTOM SCREEN LOGIC
        if (keys & KEY_TOUCH) {
            if (touch.py >= 168) { // TOOLBAR
                int btn = touch.px / 42;
                if (btn >= 6) btn = 5;
                if (btn == 0 && current_frame > 0) { // PREV
                    current_frame--;
                    load_frame(current_frame);
                } else if (btn == 1 && current_frame < num_frames - 1) { // NEXT
                    current_frame++;
                    load_frame(current_frame);
                } else if (btn == 2 && num_frames < MAX_FRAMES) { // ADD/INSERT
                    // Insert a new frame next to the current one
                    for(int f=num_frames; f>current_frame+1; f--) {
                        for(int i=0; i<NUM_JOINTS; i++) frames[f][i] = frames[f-1][i];
                        frame_colors[f] = frame_colors[f-1];
                    }
                    current_frame++;
                    num_frames++;
                    save_frame(current_frame);
                } else if (btn == 3 && num_frames > 1) { // DELETE
                    for(int f=current_frame; f<num_frames-1; f++) {
                        for(int i=0; i<NUM_JOINTS; i++) frames[f][i] = frames[f+1][i];
                        frame_colors[f] = frame_colors[f+1];
                    }
                    num_frames--;
                    if (current_frame >= num_frames) current_frame = num_frames - 1;
                    load_frame(current_frame);
                } else if (btn == 4) { // UNDO
                    do_undo();
                } else if (btn == 5) { // COLOR
                    cur_color_idx = (cur_color_idx + 1) % 5;
                    save_frame(current_frame);
                }
            } else if (touch.py <= 14) { // TIMELINE
                int f = (touch.px - 10) * num_frames / 236;
                if (f < 0) f = 0;
                if (f >= num_frames) f = num_frames - 1;
                current_frame = f;
                load_frame(current_frame);
            } else { // CANVAS
                selected_joint = -1;
                int min_dist = 400; 
                for (int i=0; i<NUM_JOINTS; i++) {
                    int dx = (touch.px + cam_x) - joints[i].x;
                    int dy = (touch.py + cam_y) - joints[i].y;
                    int d2 = dx*dx + dy*dy;
                    if (d2 < min_dist) {
                        min_dist = d2;
                        selected_joint = i;
                    }
                }
                if (selected_joint != -1) {
                    save_undo();
                } else {
                    last_tx = touch.px;
                    last_ty = touch.py;
                }
            }
        }

        if ((held & KEY_TOUCH) && touch.py < 168 && touch.py > 14) {
            if (selected_joint != -1) {
                int wx = touch.px + cam_x;
                int wy = touch.py + cam_y;
                if (selected_joint == 0) {
                    int dx = wx - joints[0].x;
                    int dy = wy - joints[0].y;
                    offset_subtree(0, dx, dy);
                } else {
                    int p = joints[selected_joint].parent;
                    int tdx = wx - joints[p].x;
                    int tdy = wy - joints[p].y;
                    float dist = sqrt(tdx*tdx + tdy*tdy);
                    if (dist > 0.1f) {
                        int nx = joints[p].x + (tdx * joints[selected_joint].length) / dist;
                        int ny = joints[p].y + (tdy * joints[selected_joint].length) / dist;
                        int dx = nx - joints[selected_joint].x;
                        int dy = ny - joints[selected_joint].y;
                        offset_subtree(selected_joint, dx, dy);
                    }
                }
                save_frame(current_frame);
            } else { // PANNING
                cam_x -= (touch.px - last_tx);
                cam_y -= (touch.py - last_ty);
                last_tx = touch.px;
                last_ty = touch.py;
            }
        } else if (!(held & KEY_TOUCH)) {
            selected_joint = -1;
        }

        // Draw Bottom Screen Map (Canvas)
        for(int y = 0; y < 168; y++) {
            for(int x = 0; x < 256; x++) fbSub[y*256 + x] = RGB15(31,31,31) | BIT(15);
        }
        
        // Draw Onion Skin
        if (current_frame > 0) {
            draw_stickman(fbSub, frames[current_frame-1], RGB15(25,25,25) | BIT(15), false, cam_x, cam_y);
        }
        // Draw Stickman
        draw_stickman(fbSub, joints, palette[cur_color_idx], true, cam_x, cam_y);

        // Draw Timeline (Top)
        for(int y = 0; y < 13; y++) {
            for(int x = 0; x < 256; x++) fbSub[y*256 + x] = RGB15(28,28,28) | BIT(15); // light grey
        }
        for(int x = 0; x < 256; x++) { fbSub[13*256+x] = RGB15(0,0,0)|BIT(15); fbSub[14*256+x] = RGB15(0,0,0)|BIT(15); }

        draw_thick_line(fbSub, 10, 6, 246, 6, 1, RGB15(15,15,15)|BIT(15));
        for(int i = 0; i < num_frames; i++) {
            int tx = 10 + i * 236 / (num_frames > 1 ? num_frames - 1 : 1);
            if (i == current_frame) {
                draw_solid_circle(fbSub, tx, 6, 3, RGB15(31,0,0)|BIT(15));
            } else {
                draw_thick_line(fbSub, tx, 4, tx, 8, 1, RGB15(0,0,0)|BIT(15));
            }
        }

        // Draw Toolbar
        for(int y = 168; y < 192; y++) {
            for(int x = 0; x < 256; x++) fbSub[y*256 + x] = RGB15(10,10,10) | BIT(15);
            if(y == 168) for(int x = 0; x < 256; x++) fbSub[y*256 + x] = RGB15(0,0,0) | BIT(15);
        }
        for(int i = 1; i <= 5; i++) {
            for(int y = 168; y < 192; y++) fbSub[y*256 + i*42] = RGB15(0,0,0) | BIT(15);
        }

        u16 icon_col = RGB15(31,31,31) | BIT(15);
        // 0. PREV (<)
        draw_thick_line(fbSub, 25, 175, 17, 180, 1, icon_col);
        draw_thick_line(fbSub, 17, 180, 25, 185, 1, icon_col);
        // 1. NEXT (>)
        draw_thick_line(fbSub, 59, 175, 67, 180, 1, icon_col);
        draw_thick_line(fbSub, 67, 180, 59, 185, 1, icon_col);
        // 2. ADD (+)
        draw_thick_line(fbSub, 105, 173, 105, 187, 1, icon_col);
        draw_thick_line(fbSub, 98, 180, 112, 180, 1, icon_col);
        // 3. DELETE (-)
        draw_thick_line(fbSub, 140, 180, 154, 180, 1, icon_col);
        // 4. UNDO (Curved Arrow / U)
        draw_thick_line(fbSub, 180, 175, 180, 184, 1, icon_col);
        draw_thick_line(fbSub, 180, 184, 194, 184, 1, icon_col);
        draw_thick_line(fbSub, 194, 184, 194, 175, 1, icon_col);
        draw_thick_line(fbSub, 180, 175, 176, 179, 1, icon_col);
        draw_thick_line(fbSub, 180, 175, 184, 179, 1, icon_col);
        // 5. COLOR (Palette Circle)
        draw_solid_circle(fbSub, 231, 180, 6, palette[cur_color_idx]);
        draw_solid_circle(fbSub, 231, 180, 4, RGB15(31,31,31)|BIT(15));
        draw_solid_circle(fbSub, 231, 180, 2, palette[cur_color_idx]);
    }
    return 0;
}
