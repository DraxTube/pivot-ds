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
int num_frames = 0;
int current_frame = 0;

Joint joints[NUM_JOINTS];

void init_joints() {
    joints[0] = (Joint){128, 100, -1, 0};  // hip
    joints[1] = (Joint){128, 60, 0, 40};   // neck
    joints[2] = (Joint){128, 40, 1, 20};   // head
    joints[3] = (Joint){108, 65, 1, 20};   // left shoulder
    joints[4] = (Joint){90, 85, 3, 27};    // left hand
    joints[5] = (Joint){148, 65, 1, 20};   // right shoulder
    joints[6] = (Joint){166, 85, 5, 27};   // right hand
    joints[7] = (Joint){115, 135, 0, 35};  // left knee
    joints[8] = (Joint){105, 170, 7, 36};  // left foot
    joints[9] = (Joint){141, 135, 0, 35};  // right knee
    joints[10] = (Joint){151, 170, 9, 36}; // right foot

    for(int i = 0; i < NUM_JOINTS; i++) frames[0][i] = joints[i];
    num_frames = 1;
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

void draw_line(u16* fb, int x0, int y0, int x1, int y1, u16 color) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = abs(y1 - y0), sy = y0 < y1 ? 1 : -1; 
    int err = (dx > dy ? dx : -dy) / 2, e2;
    for(;;) {
        if (x0 >= 0 && x0 < 256 && y0 >= 0 && y0 < 192) {
            fb[y0 * 256 + x0] = color;
        }
        if (x0 == x1 && y0 == y1) break;
        e2 = err;
        if (e2 > -dx) { err -= dy; x0 += sx; }
        if (e2 < dy) { err += dx; y0 += sy; }
    }
}

void draw_stickman(u16* fb, Joint* jts, u16 color, bool highlight_joints) {
    for (int i = 1; i < NUM_JOINTS; i++) {
        int p = jts[i].parent;
        if (p != -1) {
            draw_line(fb, jts[i].x, jts[i].y, jts[p].x, jts[p].y, color);
        }
    }
    if (highlight_joints) {
        for (int i = 0; i < NUM_JOINTS; i++) {
            int cx = jts[i].x, cy = jts[i].y;
            u16 jcolor = (i == 0) ? (RGB15(31,0,0) | BIT(15)) : (RGB15(0,31,0) | BIT(15));
            for (int dy = -2; dy <= 2; dy++) {
                for (int dx = -2; dx <= 2; dx++) {
                    if (dx*dx + dy*dy <= 4) {
                        if (cx+dx >= 0 && cx+dx < 256 && cy+dy >= 0 && cy+dy < 192) {
                            fb[(cy+dy)*256 + (cx+dx)] = jcolor;
                        }
                    }
                }
            }
        }
    }
}

enum { MODE_EDIT, MODE_PLAY };
int current_mode = MODE_EDIT;

int main(void) {
    videoSetModeSub(MODE_5_2D);
    vramSetBankC(VRAM_C_SUB_BG);
    int bg = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    
    consoleDemoInit();
    printf("Pivot DS Animation Clone\n\n");
    printf("Controls:\n");
    printf("TOUCH : Drag joints / root\n");
    printf("A     : Add Frame\n");
    printf("L / R : Previous/Next Frame\n");
    printf("START : Play/Stop Animation\n");
    printf("SELECT: Reset Animation\n");

    init_joints();
    int selected_joint = -1;
    int play_timer = 0;

    while (1) {
        swiWaitForVBlank();
        scanKeys();
        int keys = keysDown();
        int held = keysHeld();
        touchPosition touch;
        touchRead(&touch);

        if (keys & KEY_START) {
            current_mode = (current_mode == MODE_EDIT) ? MODE_PLAY : MODE_EDIT;
            play_timer = 0;
            if (current_mode == MODE_EDIT) {
                for (int i = 0; i < NUM_JOINTS; i++) joints[i] = frames[current_frame][i];
            }
        }

        if (current_mode == MODE_EDIT) {
            if (keys & KEY_TOUCH) {
                selected_joint = -1;
                int min_dist = 400; // 20px radius squared
                for (int i = 0; i < NUM_JOINTS; i++) {
                    int dx = touch.px - joints[i].x;
                    int dy = touch.py - joints[i].y;
                    int d2 = dx*dx + dy*dy;
                    if (d2 < min_dist) {
                        min_dist = d2;
                        selected_joint = i;
                    }
                }
            }

            if ((held & KEY_TOUCH) && selected_joint != -1) {
                if (selected_joint == 0) {
                    // move root
                    int dx = touch.px - joints[0].x;
                    int dy = touch.py - joints[0].y;
                    offset_subtree(0, dx, dy);
                } else {
                    // move joint, maintaining distance to parent and shifting descendants
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
                
                // Immediately save modification to current frame
                for (int i = 0; i < NUM_JOINTS; i++) {
                    frames[current_frame][i] = joints[i];
                }
                
            } else {
                selected_joint = -1;
            }

            if (keys & KEY_A) {
                if (num_frames < MAX_FRAMES) {
                    for (int i = 0; i < NUM_JOINTS; i++) frames[num_frames][i] = joints[i];
                    current_frame = num_frames;
                    num_frames++;
                }
            }

            if (keys & KEY_SELECT) {
                init_joints();
                current_frame = 0;
            }
            
            if (keys & KEY_L) {
                if (current_frame > 0) current_frame--;
                for (int i = 0; i < NUM_JOINTS; i++) joints[i] = frames[current_frame][i];
            }
            
            if (keys & KEY_R) {
                if (current_frame < num_frames - 1) current_frame++;
                for (int i = 0; i < NUM_JOINTS; i++) joints[i] = frames[current_frame][i];
            }
        } else {
            // PLAY MODE
            play_timer++;
            if (play_timer > 6) { // ~10 FPS
                play_timer = 0;
                current_frame++;
                if (current_frame >= num_frames) current_frame = 0;
                for (int i = 0; i < NUM_JOINTS; i++) joints[i] = frames[current_frame][i];
            }
        }

        u16* fb = bgGetGfxPtr(bg);
        // Clear background to white
        for (int i = 0; i < 256 * 192; i++) fb[i] = RGB15(31,31,31) | BIT(15);

        // Draw Onion skin (previous frame)
        if (current_mode == MODE_EDIT && current_frame > 0) {
            draw_stickman(fb, frames[current_frame - 1], RGB15(25,25,25) | BIT(15), false);
        }

        // Draw active stickman
        draw_stickman(fb, joints, RGB15(0,0,0) | BIT(15), current_mode == MODE_EDIT);

        printf("\x1b[10;0HMode: %s    \n", current_mode == MODE_EDIT ? "EDIT" : "PLAY");
        printf("Frame: %d / %d    \n", current_frame + 1, num_frames);
    }
    
    return 0;
}
