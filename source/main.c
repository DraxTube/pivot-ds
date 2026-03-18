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
int num_frames = 1;
int current_frame = 0;
int play_frame = 0;
int play_timer = 0;

Joint joints[NUM_JOINTS];

void init_joints() {
    joints[0] = (Joint){128, 80, -1, 0};   // hip
    joints[1] = (Joint){128, 50, 0, 30};   // neck
    joints[2] = (Joint){128, 30, 1, 20};   // head
    joints[3] = (Joint){105, 55, 1, 23};   // L shoulder
    joints[4] = (Joint){80,  75, 3, 31};   // L hand
    joints[5] = (Joint){151, 55, 1, 23};   // R shoulder
    joints[6] = (Joint){176, 75, 5, 31};   // R hand
    joints[7] = (Joint){115, 115, 0, 36};  // L knee
    joints[8] = (Joint){105, 150, 7, 36};  // L foot
    joints[9] = (Joint){141, 115, 0, 36};  // R knee
    joints[10] = (Joint){151, 150, 9, 36}; // R foot
    for(int i = 0; i < NUM_JOINTS; i++) frames[0][i] = joints[i];
    num_frames = 1;
    current_frame = 0;
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

void draw_stickman(u16* fb, Joint* jts, u16 color, bool is_edit) {
    for (int i = 1; i < NUM_JOINTS; i++) {
        int p = jts[i].parent;
        if (p != -1) {
            draw_thick_line(fb, jts[i].x, jts[i].y, jts[p].x, jts[p].y, 2, color);
        }
    }
    draw_solid_circle(fb, jts[2].x, jts[2].y, 8, color);
    
    if (is_edit) {
        for (int i = 0; i < NUM_JOINTS; i++) {
            draw_solid_circle(fb, jts[i].x, jts[i].y, 3, RGB15(31,0,0)|BIT(15));
        }
        draw_solid_circle(fb, jts[2].x, jts[2].y, 3, RGB15(31,0,0)|BIT(15)); // Head joint
    } else {
        // give the face a white dot in playback for style
        draw_solid_circle(fb, jts[2].x+2, jts[2].y-2, 1, RGB15(31,31,31)|BIT(15));
    }
}

void draw_digit(u16* fb, int d, int x, int y, u16 color) {
    int segs[10][7] = {
        {1,1,1,1,1,1,0}, {0,1,1,0,0,0,0}, {1,1,0,1,1,0,1}, {1,1,1,1,0,0,1}, 
        {0,1,1,0,0,1,1}, {1,0,1,1,0,1,1}, {1,0,1,1,1,1,1}, {1,1,1,0,0,0,0}, 
        {1,1,1,1,1,1,1}, {1,1,1,1,0,1,1}
    };
    if (d<0 || d>9) return;
    if (segs[d][0]) draw_line(fb, x, y, x+4, y, color);
    if (segs[d][1]) draw_line(fb, x+4, y, x+4, y+4, color);
    if (segs[d][2]) draw_line(fb, x+4, y+4, x+4, y+8, color);
    if (segs[d][3]) draw_line(fb, x, y+8, x+4, y+8, color);
    if (segs[d][4]) draw_line(fb, x, y+4, x, y+8, color);
    if (segs[d][5]) draw_line(fb, x, y, x, y+4, color);
    if (segs[d][6]) draw_line(fb, x, y+4, x+4, y+4, color);
}

void draw_number(u16* fb, int n, int x, int y, u16 color) {
    if (n==0) { draw_digit(fb, 0, x, y, color); return; }
    int px = x;
    int divisor = 100;
    bool started = false;
    while(divisor > 0) {
        int d = (n / divisor) % 10;
        if (d != 0 || started || divisor == 1) {
            draw_digit(fb, d, px, y, color);
            started = true;
            px += 8;
        }
        divisor /= 10;
    }
}

int main(void) {
    // TOP SCREEN -> LIVE PREVIEW
    videoSetMode(MODE_5_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    int bgMain = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);

    // BOTTOM SCREEN -> EDITOR
    videoSetModeSub(MODE_5_2D);
    vramSetBankC(VRAM_C_SUB_BG);
    int bgSub = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);

    init_joints();
    int selected_joint = -1;

    u16* fbMain = bgGetGfxPtr(bgMain);
    u16* fbSub = bgGetGfxPtr(bgSub);

    while (1) {
        swiWaitForVBlank();
        scanKeys();
        int keys = keysDown();
        int held = keysHeld();
        touchPosition touch;
        touchRead(&touch);

        // TOP SCREEN LOGIC: Looping the animation continuously
        play_timer++;
        if (play_timer > 6) { // ~10fps
            play_timer = 0;
            if (num_frames > 0) {
                play_frame++;
                if (play_frame >= num_frames) play_frame = 0;
            }
        }

        // Draw Top Screen
        for(int i = 0; i < 256 * 192; i++) fbMain[i] = RGB15(31,31,31) | BIT(15);
        if (num_frames > 0) {
            draw_stickman(fbMain, frames[play_frame], RGB15(0,0,0) | BIT(15), false);
        }

        // BOTTOM SCREEN LOGIC: Tools and Input
        if (keys & KEY_TOUCH) {
            if (touch.py >= 168) {
                int btn = touch.px / 51;
                if (btn == 0 && current_frame > 0) { // PREV
                    current_frame--;
                    for(int i=0; i<NUM_JOINTS; i++) joints[i] = frames[current_frame][i];
                } else if (btn == 1 && num_frames < MAX_FRAMES) { // ADD
                    current_frame++;
                    num_frames = current_frame + 1;
                    for(int i=0; i<NUM_JOINTS; i++) frames[current_frame][i] = joints[i];
                } else if (btn == 2 && current_frame < num_frames - 1) { // NEXT
                    current_frame++;
                    for(int i=0; i<NUM_JOINTS; i++) joints[i] = frames[current_frame][i];
                } else if (btn == 3 && num_frames > 1) { // DELETE
                    for(int f=current_frame; f<num_frames-1; f++) {
                        for(int i=0; i<NUM_JOINTS; i++) frames[f][i] = frames[f+1][i];
                    }
                    num_frames--;
                    if (current_frame >= num_frames) current_frame = num_frames - 1;
                    for(int i=0; i<NUM_JOINTS; i++) joints[i] = frames[current_frame][i];
                } else if (btn == 4) {  // CLEAR
                    init_joints();
                }
            } else {
                selected_joint = -1;
                int min_dist = 400; 
                for (int i=0; i<NUM_JOINTS; i++) {
                    int dx = touch.px - joints[i].x;
                    int dy = touch.py - joints[i].y;
                    int d2 = dx*dx + dy*dy;
                    if (d2 < min_dist) {
                        min_dist = d2;
                        selected_joint = i;
                    }
                }
            }
        }

        if ((held & KEY_TOUCH) && selected_joint != -1 && touch.py < 168) {
            if (selected_joint == 0) {
                int dx = touch.px - joints[0].x;
                int dy = touch.py - joints[0].y;
                offset_subtree(0, dx, dy);
            } else {
                int p = joints[selected_joint].parent;
                int tdx = touch.px - joints[p].x;
                int tdy = touch.py - joints[p].y;
                float dist = sqrt(tdx*tdx + tdy*tdy);
                if (dist > 0.1f) {
                    int nx = joints[p].x + (tdx * joints[selected_joint].length) / dist;
                    int ny = joints[p].y + (tdy * joints[selected_joint].length) / dist;
                    int dx = nx - joints[selected_joint].x;
                    int dy = ny - joints[selected_joint].y;
                    offset_subtree(selected_joint, dx, dy);
                }
            }
            for(int i=0; i<NUM_JOINTS; i++) frames[current_frame][i] = joints[i];
        } else if (!(held & KEY_TOUCH)) {
            selected_joint = -1;
        }

        // Draw Bottom Screen
        for(int y = 0; y < 168; y++) {
            for(int x = 0; x < 256; x++) fbSub[y*256 + x] = RGB15(31,31,31) | BIT(15);
        }
        
        // Draw Onion Skin (Previous frame)
        if (current_frame > 0) {
            draw_stickman(fbSub, frames[current_frame-1], RGB15(25,25,25) | BIT(15), false);
        }
        // Draw current Stickman
        draw_stickman(fbSub, joints, RGB15(0,0,0) | BIT(15), true);

        // Frame indicator (Top left corner of editor)
        draw_number(fbSub, current_frame + 1, 5, 5, RGB15(0,0,0) | BIT(15));
        draw_line(fbSub, 18, 17, 24, 5, RGB15(0,0,0) | BIT(15));
        draw_number(fbSub, num_frames, 28, 5, RGB15(0,0,0) | BIT(15));

        // Draw UI Toolbar
        for(int y = 168; y < 192; y++) {
            for(int x = 0; x < 256; x++) fbSub[y*256 + x] = RGB15(10,10,10) | BIT(15);
            if(y == 168) for(int x = 0; x < 256; x++) fbSub[y*256 + x] = RGB15(0,0,0) | BIT(15);
        }
        // Button Separators
        for(int i = 1; i <= 4; i++) {
            for(int y = 168; y < 192; y++) fbSub[y*256 + i*51] = RGB15(0,0,0) | BIT(15);
        }

        u16 icon_color = RGB15(31,31,31) | BIT(15); // white UI icons
        // 1. PREV
        draw_thick_line(fbSub, 32, 175, 20, 180, 1, icon_color);
        draw_thick_line(fbSub, 20, 180, 32, 185, 1, icon_color);
        // 2. ADD (+)
        draw_thick_line(fbSub, 76, 173, 76, 187, 1, icon_color);
        draw_thick_line(fbSub, 69, 180, 83, 180, 1, icon_color);
        // 3. NEXT
        draw_thick_line(fbSub, 120, 175, 132, 180, 1, icon_color);
        draw_thick_line(fbSub, 132, 180, 120, 185, 1, icon_color);
        // 4. DELETE (-)
        draw_thick_line(fbSub, 166, 180, 188, 180, 1, icon_color);
        // 5. CLEAR (X)
        draw_thick_line(fbSub, 222, 173, 238, 187, 1, icon_color);
        draw_thick_line(fbSub, 222, 187, 238, 173, 1, icon_color);
    }
    return 0;
}
