#include "robot_ui.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT 64
#define TEXT_GLYPH_HEIGHT 16
#define ASCII_GLYPH_BITMAP_WIDTH 10
#define ASCII_GLYPH_ADVANCE 9
#define CHINESE_GLYPH_WIDTH 16
#define PROVISION_SCROLL_STEP_PIXELS 2
#define PROVISION_SCROLL_INTERVAL_MS 60
#define PROVISION_SCROLL_GAP_PIXELS 24
#define ANIMATION_QUEUE_DEPTH 12

typedef enum {
    CN_PEI,
    CN_WANG,
    CN_MO,
    CN_SHI,
    CN_QING,
    CN_LIAN,
    CN_JIE,
    CN_RAN,
    CN_HOU,
    CN_FANG,
    CN_WEN,
    CN_JIN,
    CN_XING,
    CN_GLYPH_COUNT,
} chinese_glyph_t;

/*
 * Compact 16x16 glyph subset used by the provisioning screen. Keeping only
 * these characters provides XiaoZhi-style Chinese guidance without pulling
 * LVGL and a full CJK font into the lightweight expression renderer.
 */
static const uint16_t CHINESE_GLYPHS[CN_GLYPH_COUNT][TEXT_GLYPH_HEIGHT] = {
    [CN_PEI]  = {0x7F7C, 0x7F7C, 0x1404, 0x7F04, 0x5504, 0x5504, 0x557C, 0x5540,
                 0x6340, 0x4140, 0x7F40, 0x4140, 0x7F42, 0x417E, 0x0000, 0x0000},
    [CN_WANG] = {0x7FFE, 0x6006, 0x611E, 0x6B16, 0x6AD6, 0x6676, 0x6626, 0x6676,
                 0x6F56, 0x68DE, 0x7186, 0x6006, 0x600E, 0x6000, 0x0000, 0x0000},
    [CN_MO]   = {0x10D8, 0x13FE, 0x10D8, 0x7C00, 0x11FC, 0x1104, 0x39FC, 0x3504,
                 0x75FC, 0x5020, 0x13FE, 0x1050, 0x10C8, 0x1707, 0x0002, 0x0000},
    [CN_SHI]  = {0x00C0, 0x00D8, 0x00C8, 0x7FFE, 0x00C0, 0x0040, 0x0040, 0x3F40,
                 0x0C60, 0x0C20, 0x0C20, 0x0FB2, 0x7C1A, 0x200E, 0x0000, 0x0000},
    [CN_QING] = {0x0020, 0x33FE, 0x1820, 0x03FC, 0x0020, 0x77FE, 0x1000, 0x11FC,
                 0x1104, 0x11FC, 0x1504, 0x19FC, 0x3104, 0x010C, 0x0000, 0x0000},
    [CN_LIAN] = {0x0040, 0x6080, 0x37FE, 0x1100, 0x0120, 0x0220, 0x73FC, 0x1020,
                 0x1020, 0x17FE, 0x1020, 0x1020, 0x3820, 0x47FE, 0x0000, 0x0000},
    [CN_JIE]  = {0x1020, 0x1020, 0x13FE, 0x7C88, 0x1088, 0x1010, 0x17FE, 0x1C40,
                 0x73FE, 0x1088, 0x1198, 0x10F0, 0x1078, 0x73C6, 0x0200, 0x0000},
    [CN_RAN]  = {0x1820, 0x1828, 0x1F24, 0x3220, 0x6BFC, 0x4E20, 0x3470, 0x1C50,
                 0x18C8, 0x318E, 0x2100, 0x2048, 0x2644, 0x6226, 0x0000, 0x0000},
    [CN_HOU]  = {0x0008, 0x0FFC, 0x1800, 0x1000, 0x1000, 0x1FFE, 0x1000, 0x1000,
                 0x37FC, 0x340C, 0x240C, 0x240C, 0x67FC, 0x440C, 0x0400, 0x0000},
    [CN_FANG] = {0x2040, 0x3060, 0x1000, 0x17FE, 0x0080, 0x7080, 0x10FC, 0x1084,
                 0x1084, 0x1084, 0x158C, 0x190C, 0x330C, 0x0638, 0x0000, 0x0000},
    [CN_WEN]  = {0x1000, 0x19FC, 0x0804, 0x2004, 0x2004, 0x27E4, 0x2424, 0x2424,
                 0x2424, 0x27E4, 0x2004, 0x2004, 0x2004, 0x200C, 0x6000, 0x0000},
    [CN_JIN]  = {0x0090, 0x3090, 0x1090, 0x07FE, 0x0090, 0x7090, 0x1090, 0x17FE,
                 0x1110, 0x1110, 0x1310, 0x1210, 0x7E00, 0x43FE, 0x0000, 0x0000},
    [CN_XING] = {0x1800, 0x11FC, 0x3000, 0x6000, 0x4C00, 0x1BFE, 0x1010, 0x3010,
                 0x7010, 0x5010, 0x1010, 0x1010, 0x1010, 0x1070, 0x1020, 0x0000},
};

typedef struct {
    char code;
    uint16_t rows[TEXT_GLYPH_HEIGHT];
} ascii_glyph_t;

static const ascii_glyph_t ASCII_GLYPHS[] = {
    {' ', {0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
           0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000}},
    {'-', {0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x0F8, 0x000,
           0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000}},
    {'.', {0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
           0x000, 0x070, 0x070, 0x000, 0x000, 0x000, 0x000, 0x000}},
    {'/', {0x008, 0x018, 0x010, 0x010, 0x030, 0x020, 0x060, 0x040,
           0x040, 0x0C0, 0x080, 0x080, 0x100, 0x000, 0x000, 0x000}},
    {'0', {0x000, 0x070, 0x088, 0x104, 0x10C, 0x134, 0x164, 0x184,
           0x104, 0x088, 0x070, 0x000, 0x000, 0x000, 0x000, 0x000}},
    {'1', {0x000, 0x060, 0x0A0, 0x120, 0x020, 0x020, 0x020, 0x020,
           0x020, 0x020, 0x1FC, 0x000, 0x000, 0x000, 0x000, 0x000}},
    {'2', {0x000, 0x0F0, 0x118, 0x008, 0x008, 0x008, 0x010, 0x020,
           0x040, 0x080, 0x1FC, 0x000, 0x000, 0x000, 0x000, 0x000}},
    {'3', {0x000, 0x1F0, 0x008, 0x008, 0x018, 0x0F0, 0x018, 0x008,
           0x008, 0x010, 0x1E0, 0x000, 0x000, 0x000, 0x000, 0x000}},
    {'4', {0x000, 0x030, 0x070, 0x050, 0x0D0, 0x090, 0x110, 0x310,
           0x3FC, 0x010, 0x010, 0x000, 0x000, 0x000, 0x000, 0x000}},
    {'5', {0x000, 0x1F0, 0x100, 0x100, 0x100, 0x1F0, 0x018, 0x008,
           0x008, 0x010, 0x1E0, 0x000, 0x000, 0x000, 0x000, 0x000}},
    {'6', {0x000, 0x078, 0x0C0, 0x080, 0x100, 0x178, 0x18C, 0x104,
           0x104, 0x08C, 0x070, 0x000, 0x000, 0x000, 0x000, 0x000}},
    {'7', {0x000, 0x1FC, 0x00C, 0x008, 0x018, 0x010, 0x030, 0x020,
           0x060, 0x040, 0x0C0, 0x000, 0x000, 0x000, 0x000, 0x000}},
    {'8', {0x000, 0x0F0, 0x108, 0x108, 0x198, 0x060, 0x090, 0x108,
           0x108, 0x108, 0x0F0, 0x000, 0x000, 0x000, 0x000, 0x000}},
    {'9', {0x000, 0x070, 0x188, 0x104, 0x104, 0x18C, 0x0F4, 0x004,
           0x008, 0x018, 0x0F0, 0x000, 0x000, 0x000, 0x000, 0x000}},
    {':', {0x000, 0x000, 0x000, 0x000, 0x060, 0x060, 0x000, 0x000,
           0x000, 0x060, 0x060, 0x000, 0x000, 0x000, 0x000, 0x000}},
    {'?', {0x0C0, 0x020, 0x010, 0x010, 0x010, 0x0E0, 0x080, 0x080,
           0x000, 0x0C0, 0x0C0, 0x000, 0x000, 0x000, 0x000, 0x000}},
    {'A', {0x000, 0x070, 0x050, 0x050, 0x058, 0x088, 0x088, 0x088,
           0x1FC, 0x104, 0x104, 0x000, 0x000, 0x000, 0x000, 0x000}},
    {'B', {0x000, 0x1F0, 0x108, 0x108, 0x118, 0x1F0, 0x118, 0x108,
           0x108, 0x118, 0x1F0, 0x000, 0x000, 0x000, 0x000, 0x000}},
    {'C', {0x000, 0x078, 0x0C4, 0x080, 0x100, 0x100, 0x100, 0x100,
           0x180, 0x084, 0x078, 0x000, 0x000, 0x000, 0x000, 0x000}},
    {'D', {0x000, 0x1F0, 0x108, 0x104, 0x104, 0x104, 0x104, 0x104,
           0x10C, 0x118, 0x1F0, 0x000, 0x000, 0x000, 0x000, 0x000}},
    {'E', {0x000, 0x0FC, 0x080, 0x080, 0x080, 0x0FC, 0x080, 0x080,
           0x080, 0x080, 0x0FC, 0x000, 0x000, 0x000, 0x000, 0x000}},
    {'F', {0x000, 0x0FC, 0x080, 0x080, 0x080, 0x080, 0x0FC, 0x080,
           0x080, 0x080, 0x080, 0x000, 0x000, 0x000, 0x000, 0x000}},
    {'Z', {0x000, 0x1FC, 0x00C, 0x018, 0x030, 0x060, 0x0C0, 0x180,
           0x100, 0x100, 0x1FC, 0x000, 0x000, 0x000, 0x000, 0x000}},
    {'W', {0x000, 0x204, 0x204, 0x204, 0x224, 0x264, 0x254, 0x2D4,
           0x294, 0x38C, 0x30C, 0x000, 0x000, 0x000, 0x000, 0x000}},
    {'e', {0x000, 0x000, 0x000, 0x000, 0x078, 0x08C, 0x104, 0x1FC,
           0x100, 0x180, 0x07C, 0x000, 0x000, 0x000, 0x000, 0x000}},
    {'h', {0x100, 0x100, 0x100, 0x100, 0x170, 0x188, 0x108, 0x108,
           0x108, 0x108, 0x108, 0x000, 0x000, 0x000, 0x000, 0x000}},
    {'i', {0x0C0, 0x0C0, 0x000, 0x000, 0x1C0, 0x040, 0x040, 0x040,
           0x040, 0x040, 0x1F8, 0x000, 0x000, 0x000, 0x000, 0x000}},
    {'j', {0x030, 0x030, 0x000, 0x000, 0x1F0, 0x010, 0x010, 0x010,
           0x010, 0x010, 0x010, 0x010, 0x130, 0x0E0, 0x000, 0x000}},
    {'k', {0x100, 0x100, 0x100, 0x100, 0x118, 0x120, 0x1C0, 0x140,
           0x120, 0x110, 0x108, 0x000, 0x000, 0x000, 0x000, 0x000}},
    {'m', {0x000, 0x000, 0x000, 0x000, 0x1FC, 0x1B4, 0x124, 0x124,
           0x124, 0x124, 0x124, 0x000, 0x000, 0x000, 0x000, 0x000}},
    {'o', {0x000, 0x000, 0x000, 0x000, 0x078, 0x08C, 0x104, 0x104,
           0x104, 0x188, 0x0F0, 0x000, 0x000, 0x000, 0x000, 0x000}},
    {'p', {0x000, 0x000, 0x000, 0x000, 0x178, 0x18C, 0x104, 0x104,
           0x104, 0x108, 0x1F0, 0x100, 0x100, 0x100, 0x000, 0x000}},
    {'s', {0x000, 0x000, 0x000, 0x000, 0x0F0, 0x100, 0x180, 0x0F0,
           0x008, 0x008, 0x1F0, 0x000, 0x000, 0x000, 0x000, 0x000}},
    {'t', {0x000, 0x040, 0x040, 0x040, 0x1FC, 0x040, 0x040, 0x040,
           0x040, 0x060, 0x03C, 0x000, 0x000, 0x000, 0x000, 0x000}},
};

typedef struct {
    robot_animation_t animation;
    TaskHandle_t waiter;
} robot_animation_message_t;

typedef struct {
    i2c_master_bus_handle_t bus;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
    SemaphoreHandle_t lock;
    QueueHandle_t animation_queue;
    TaskHandle_t animation_task;
    TaskHandle_t provisioning_task;
    uint8_t pixels[DISPLAY_WIDTH * DISPLAY_HEIGHT / 8];
    char provisioning_ssid[33];
    char provisioning_address[64];
    volatile bool provisioning_active;
    volatile bool animation_running;
    volatile bool idle_animation_enabled;
    robot_expression_t current_expression;
    bool initialized;
} robot_ui_state_t;

static const char *TAG = "robot_ui";
static robot_ui_state_t s_ui;

static const chinese_glyph_t PROVISION_TITLE[] = {
    CN_PEI, CN_WANG, CN_MO, CN_SHI,
};
static const chinese_glyph_t PROVISION_CONNECT[] = {
    CN_QING, CN_LIAN, CN_JIE,
};
static const chinese_glyph_t PROVISION_VISIT[] = {
    CN_RAN, CN_HOU, CN_FANG, CN_WEN,
};
static const chinese_glyph_t PROVISION_FINISH[] = {
    CN_JIN, CN_XING, CN_PEI, CN_WANG,
};

static void set_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= DISPLAY_WIDTH || y < 0 || y >= DISPLAY_HEIGHT) {
        return;
    }
    uint8_t *byte = &s_ui.pixels[(y / 8) * DISPLAY_WIDTH + x];
    uint8_t mask = (uint8_t)(1U << (y & 7));
    if (on) {
        *byte |= mask;
    } else {
        *byte &= (uint8_t)~mask;
    }
}

static void fill_rect(int x, int y, int width, int height, bool on)
{
    for (int yy = y; yy < y + height; ++yy) {
        for (int xx = x; xx < x + width; ++xx) {
            set_pixel(xx, yy, on);
        }
    }
}

static void draw_chinese_glyph(int x, int y, chinese_glyph_t glyph)
{
    if (glyph >= CN_GLYPH_COUNT) {
        return;
    }
    for (int row = 0; row < TEXT_GLYPH_HEIGHT; ++row) {
        for (int column = 0; column < CHINESE_GLYPH_WIDTH; ++column) {
            if (CHINESE_GLYPHS[glyph][row] &
                (1U << (CHINESE_GLYPH_WIDTH - 1 - column))) {
                set_pixel(x + column, y + row, true);
            }
        }
    }
}

static void draw_chinese_text(int x, int y,
                              const chinese_glyph_t *glyphs, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        draw_chinese_glyph(x + (int)i * CHINESE_GLYPH_WIDTH, y, glyphs[i]);
    }
}

static void draw_chinese_text_centered(int y,
                                       const chinese_glyph_t *glyphs,
                                       size_t count)
{
    int width = (int)count * CHINESE_GLYPH_WIDTH;
    draw_chinese_text((DISPLAY_WIDTH - width) / 2, y, glyphs, count);
}

static const ascii_glyph_t *find_ascii_glyph(char code)
{
    const size_t count = sizeof(ASCII_GLYPHS) / sizeof(ASCII_GLYPHS[0]);
    const ascii_glyph_t *fallback = NULL;
    for (size_t i = 0; i < count; ++i) {
        if (ASCII_GLYPHS[i].code == '?') {
            fallback = &ASCII_GLYPHS[i];
        }
        if (ASCII_GLYPHS[i].code == code) {
            return &ASCII_GLYPHS[i];
        }
    }
    return fallback;
}

static void draw_ascii_char(int x, int y, char code)
{
    const ascii_glyph_t *glyph = find_ascii_glyph(code);
    if (!glyph) {
        return;
    }
    for (int row = 0; row < TEXT_GLYPH_HEIGHT; ++row) {
        for (int column = 0; column < ASCII_GLYPH_BITMAP_WIDTH; ++column) {
            if (glyph->rows[row] &
                (1U << (ASCII_GLYPH_BITMAP_WIDTH - 1 - column))) {
                set_pixel(x + column, y + row, true);
            }
        }
    }
}

static void draw_ascii_text(int x, int y, const char *text)
{
    for (size_t i = 0; text[i]; ++i) {
        draw_ascii_char(x + (int)i * ASCII_GLYPH_ADVANCE, y, text[i]);
    }
}

static int ascii_text_width(const char *text)
{
    return (int)strlen(text) * ASCII_GLYPH_ADVANCE;
}

static void line(int x0, int y0, int x1, int y1)
{
    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    while (true) {
        set_pixel(x0, y0, true);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int twice = 2 * error;
        if (twice >= dy) {
            error += dy;
            x0 += sx;
        }
        if (twice <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

static void fill_circle(int center_x, int center_y, int radius, bool on)
{
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            if (x * x + y * y <= radius * radius) {
                set_pixel(center_x + x, center_y + y, on);
            }
        }
    }
}

static void draw_heart(int center_x, int center_y)
{
    fill_circle(center_x - 5, center_y - 3, 5, true);
    fill_circle(center_x + 5, center_y - 3, 5, true);
    for (int row = 0; row < 12; ++row) {
        int half = 10 - row;
        if (half >= 0) {
            line(center_x - half, center_y + row - 1,
                 center_x + half, center_y + row - 1);
        }
    }
}

static void fill_rounded_rect(int center_x, int center_y, int width,
                              int height, int radius, bool on)
{
    if (width <= 0 || height <= 0) {
        return;
    }
    radius = radius > width / 2 ? width / 2 : radius;
    radius = radius > height / 2 ? height / 2 : radius;
    int left = center_x - width / 2;
    int top = center_y - height / 2;
    int right = left + width - 1;
    int bottom = top + height - 1;
    for (int y = top; y <= bottom; ++y) {
        for (int x = left; x <= right; ++x) {
            int dx = x < left + radius ? left + radius - x
                     : x > right - radius ? x - (right - radius) : 0;
            int dy = y < top + radius ? top + radius - y
                     : y > bottom - radius ? y - (bottom - radius) : 0;
            if (dx == 0 || dy == 0 || dx * dx + dy * dy <= radius * radius) {
                set_pixel(x, y, on);
            }
        }
    }
}

typedef enum {
    ORIGINAL_EYE_NORMAL,
    ORIGINAL_EYE_HAPPY,
    ORIGINAL_EYE_SAD,
    ORIGINAL_EYE_ANGRY,
} original_eye_shape_t;

static void draw_original_eye(int center_x, int center_y, int width,
                              int height, int radius,
                              original_eye_shape_t shape, bool left_eye)
{
    fill_rounded_rect(center_x, center_y, width, height, radius, true);
    if (shape == ORIGINAL_EYE_NORMAL || height <= 4) {
        return;
    }

    int left = center_x - width / 2;
    int top = center_y - height / 2;
    int right = left + width - 1;
    int bottom = top + height - 1;
    for (int x = left; x <= right; ++x) {
        int progress = (x - left) * 12 / (width > 1 ? width - 1 : 1);
        int slope = left_eye ? progress : 12 - progress;
        int threshold;
        if (shape == ORIGINAL_EYE_HAPPY) {
            /*
             * EmojiController overlays the lower half of the left eye with
             * a +8 degree background mask and the right eye with -8 degrees.
             * Reproduce those mirrored mask edges in the monochrome buffer.
             */
            threshold = center_y + 3 + slope / 2;
            for (int y = threshold; y <= bottom; ++y) {
                set_pixel(x, y, false);
            }
        } else {
            if (shape == ORIGINAL_EYE_ANGRY) {
                slope = 12 - slope;
            }
            threshold = center_y - 5 + slope;
            for (int y = top; y <= threshold; ++y) {
                set_pixel(x, y, false);
            }
        }
    }
}

static void draw_face(robot_expression_t expression, int offset_x,
                      int offset_y, int blink_height)
{
    /* Original EmojiController geometry: two 40x40 rounded eyes with a
     * 10-pixel gap, centered vertically on the 128x64 OLED. */
    int left_x = 39 + offset_x;
    int right_x = 89 + offset_x;
    int left_y = 32 + offset_y;
    int right_y = 32 + offset_y;
    int left_width = 40;
    int right_width = 40;
    int left_height = 40;
    int right_height = 40;
    int radius = 10;
    original_eye_shape_t shape = ORIGINAL_EYE_NORMAL;

    switch (expression) {
        case ROBOT_EXPRESSION_HAPPY:
            shape = ORIGINAL_EYE_HAPPY;
            break;
        case ROBOT_EXPRESSION_SAD:
            shape = ORIGINAL_EYE_SAD;
            break;
        case ROBOT_EXPRESSION_ANGRY:
            shape = ORIGINAL_EYE_ANGRY;
            break;
        case ROBOT_EXPRESSION_SURPRISED:
            left_width = right_width = 44;
            left_height = right_height = 44;
            radius = 11;
            break;
        case ROBOT_EXPRESSION_THINKING:
            right_width = right_height = 12;
            radius = 6;
            break;
        case ROBOT_EXPRESSION_CONFUSED:
            left_height = right_height = 20;
            break;
        case ROBOT_EXPRESSION_AWKWARD:
            left_height = right_height = 20;
            break;
        case ROBOT_EXPRESSION_CRY:
            left_height = right_height = 28;
            shape = ORIGINAL_EYE_SAD;
            break;
        case ROBOT_EXPRESSION_LAUGHING:
            left_height = right_height = 12;
            left_y = right_y = 22 + offset_y;
            radius = 12;
            break;
        case ROBOT_EXPRESSION_FUNNY:
            left_width = left_height = 58;
            right_width = right_height = 24;
            radius = 12;
            break;
        case ROBOT_EXPRESSION_LOVING:
            memset(s_ui.pixels, 0, sizeof(s_ui.pixels));
            draw_heart(left_x, left_y - 2);
            draw_heart(right_x, right_y - 2);
            return;
        case ROBOT_EXPRESSION_EMBARRASSED:
            left_width = right_width = 28;
            left_height = right_height = 28;
            left_y += 5;
            right_y += 5;
            break;
        case ROBOT_EXPRESSION_SHOCKED:
            left_width = right_width = 58;
            left_height = right_height = 58;
            radius = 29;
            break;
        case ROBOT_EXPRESSION_COOL:
            left_height = right_height = 16;
            radius = 10;
            break;
        case ROBOT_EXPRESSION_RELAXED:
            left_width = right_width = 36;
            left_height = right_height = 20;
            break;
        case ROBOT_EXPRESSION_DELICIOUS:
            left_width = right_width = 28;
            left_height = right_height = 28;
            left_y -= 3;
            right_y -= 3;
            break;
        case ROBOT_EXPRESSION_KISSY:
            left_width = right_width = 12;
            left_height = right_height = 12;
            radius = 6;
            break;
        case ROBOT_EXPRESSION_CONFIDENT:
            left_height = right_height = 20;
            left_x += 5;
            right_x += 5;
            break;
        case ROBOT_EXPRESSION_SILLY:
            left_width = left_height = 16;
            radius = 8;
            break;
        case ROBOT_EXPRESSION_SLEEP:
            left_height = right_height = 2;
            radius = 1;
            break;
        case ROBOT_EXPRESSION_NEUTRAL:
        default:
            break;
    }

    if (blink_height < 14) {
        left_height = left_height * blink_height / 14;
        right_height = right_height * blink_height / 14;
        left_height = left_height < 2 ? 2 : left_height;
        right_height = right_height < 2 ? 2 : right_height;
    }

    memset(s_ui.pixels, 0, sizeof(s_ui.pixels));
    draw_original_eye(left_x, left_y, left_width, left_height, radius,
                      shape, true);
    draw_original_eye(right_x, right_y, right_width, right_height, radius,
                      shape, false);

    if (expression == ROBOT_EXPRESSION_AWKWARD) {
        fill_rect(108, 8, 3, 8, true);
        fill_rect(114, 6, 3, 12, true);
        fill_rect(120, 9, 3, 6, true);
    } else if (expression == ROBOT_EXPRESSION_CRY) {
        fill_rounded_rect(left_x, 54, 5, 14, 2, true);
        fill_rounded_rect(right_x, 54, 5, 14, 2, true);
    }
}

static esp_err_t flush(void)
{
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        err = esp_lcd_panel_draw_bitmap(s_ui.panel, 0, 0,
                                        DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                        s_ui.pixels);
        if (err == ESP_OK) {
            if (attempt > 1) {
                ESP_LOGI(TAG, "OLED refresh recovered on attempt %d",
                         attempt);
            }
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(8));
    }
    ESP_LOGW(TAG, "OLED refresh failed after retries: %s",
             esp_err_to_name(err));
    return err;
}

static esp_err_t render_face_frame(robot_expression_t expression,
                                   int offset_x, int offset_y,
                                   int blink_height)
{
    if (!s_ui.initialized || !s_ui.lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_ui.lock, portMAX_DELAY);
    if (s_ui.provisioning_active) {
        xSemaphoreGive(s_ui.lock);
        return ESP_ERR_INVALID_STATE;
    }
    draw_face(expression, offset_x, offset_y, blink_height);
    esp_err_t err = flush();
    xSemaphoreGive(s_ui.lock);
    return err;
}

static esp_err_t render_eye_pair(int left_x, int left_y,
                                 int left_width, int left_height,
                                 int right_x, int right_y,
                                 int right_width, int right_height)
{
    if (!s_ui.initialized || !s_ui.lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_ui.lock, portMAX_DELAY);
    if (s_ui.provisioning_active) {
        xSemaphoreGive(s_ui.lock);
        return ESP_ERR_INVALID_STATE;
    }
    memset(s_ui.pixels, 0, sizeof(s_ui.pixels));
    draw_original_eye(left_x, left_y, left_width, left_height, 10,
                      ORIGINAL_EYE_NORMAL, true);
    draw_original_eye(right_x, right_y, right_width, right_height, 10,
                      ORIGINAL_EYE_NORMAL, false);
    esp_err_t err = flush();
    xSemaphoreGive(s_ui.lock);
    return err;
}

static void execute_look_animation(int direction)
{
    int left_x = 39;
    int right_x = 89;
    int left_width = 40;
    int right_width = 40;
    int left_height = 40;
    int right_height = 40;

    /* Exact MoveEye() geometry from EmojiController. */
    for (int i = 0; i < 3; ++i) {
        left_x += 2 * direction;
        right_x += 2 * direction;
        left_height -= 5;
        right_height -= 5;
        if (direction > 0) {
            ++right_height;
            ++right_width;
        } else {
            ++left_height;
            ++left_width;
        }
        render_eye_pair(left_x, 32, left_width, left_height,
                        right_x, 32, right_width, right_height);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    for (int i = 0; i < 3; ++i) {
        left_x += 2 * direction;
        right_x += 2 * direction;
        left_height += 5;
        right_height += 5;
        if (direction > 0) {
            ++right_height;
            ++right_width;
        } else {
            ++left_height;
            ++left_width;
        }
        render_eye_pair(left_x, 32, left_width, left_height,
                        right_x, 32, right_width, right_height);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    vTaskDelay(pdMS_TO_TICKS(1000));

    /* Exact recovery stages from ExecuteLookLeft/RightAnimation(). */
    for (int i = 0; i < 3; ++i) {
        left_x -= 2 * direction;
        right_x -= 2 * direction;
        left_height -= 5;
        right_height -= 5;
        if (direction > 0) {
            --right_height;
            --right_width;
        } else {
            --left_height;
            --left_width;
        }
        render_eye_pair(left_x, 32, left_width, left_height,
                        right_x, 32, right_width, right_height);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    for (int i = 0; i < 3; ++i) {
        left_x -= 2 * direction;
        right_x -= 2 * direction;
        left_height += 5;
        right_height += 5;
        if (direction > 0) {
            --right_height;
            --right_width;
        } else {
            --left_height;
            --left_width;
        }
        render_eye_pair(left_x, 32, left_width, left_height,
                        right_x, 32, right_width, right_height);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    render_face_frame(ROBOT_EXPRESSION_NEUTRAL, 0, 0, 14);
}

static robot_expression_t expression_for_animation(robot_animation_t animation)
{
    switch (animation) {
        case ROBOT_ANIMATION_HAPPY: return ROBOT_EXPRESSION_HAPPY;
        case ROBOT_ANIMATION_SAD: return ROBOT_EXPRESSION_SAD;
        case ROBOT_ANIMATION_ANGER: return ROBOT_EXPRESSION_ANGRY;
        case ROBOT_ANIMATION_SURPRISE: return ROBOT_EXPRESSION_SURPRISED;
        case ROBOT_ANIMATION_SLEEP: return ROBOT_EXPRESSION_SLEEP;
        case ROBOT_ANIMATION_CONFUSED: return ROBOT_EXPRESSION_CONFUSED;
        case ROBOT_ANIMATION_AWKWARD: return ROBOT_EXPRESSION_AWKWARD;
        case ROBOT_ANIMATION_CRY: return ROBOT_EXPRESSION_CRY;
        case ROBOT_ANIMATION_LAUGHING: return ROBOT_EXPRESSION_LAUGHING;
        case ROBOT_ANIMATION_FUNNY: return ROBOT_EXPRESSION_FUNNY;
        case ROBOT_ANIMATION_LOVING: return ROBOT_EXPRESSION_LOVING;
        case ROBOT_ANIMATION_EMBARRASSED:
            return ROBOT_EXPRESSION_EMBARRASSED;
        case ROBOT_ANIMATION_SHOCKED: return ROBOT_EXPRESSION_SHOCKED;
        case ROBOT_ANIMATION_THINKING: return ROBOT_EXPRESSION_THINKING;
        case ROBOT_ANIMATION_COOL: return ROBOT_EXPRESSION_COOL;
        case ROBOT_ANIMATION_RELAXED: return ROBOT_EXPRESSION_RELAXED;
        case ROBOT_ANIMATION_DELICIOUS: return ROBOT_EXPRESSION_DELICIOUS;
        case ROBOT_ANIMATION_KISSY: return ROBOT_EXPRESSION_KISSY;
        case ROBOT_ANIMATION_CONFIDENT: return ROBOT_EXPRESSION_CONFIDENT;
        case ROBOT_ANIMATION_SILLY: return ROBOT_EXPRESSION_SILLY;
        default: return s_ui.current_expression;
    }
}

static void animate_offset(robot_expression_t expression, int x, int y)
{
    const int steps = 4;
    for (int i = 1; i <= steps; ++i) {
        render_face_frame(expression, x * i / steps, y * i / steps, 14);
        vTaskDelay(pdMS_TO_TICKS(35));
    }
    vTaskDelay(pdMS_TO_TICKS(180));
    for (int i = steps - 1; i >= 0; --i) {
        render_face_frame(expression, x * i / steps, y * i / steps, 14);
        vTaskDelay(pdMS_TO_TICKS(35));
    }
}

static void execute_animation(robot_animation_t animation)
{
    if (s_ui.provisioning_active) {
        return;
    }
    robot_expression_t expression = expression_for_animation(animation);
    switch (animation) {
        case ROBOT_ANIMATION_BLINK:
            /* Original EmojiController: 40% chance of a double blink. */
            for (int blink = 0, count = (esp_random() % 100) < 40 ? 2 : 1;
                 blink < count; ++blink) {
                /* Eight close/open steps, matching ExecuteBlinkAnimation. */
                static const uint8_t closing[] = {
                    12, 11, 9, 7, 5, 4, 2, 0,
                };
                for (size_t i = 0; i < sizeof(closing); ++i) {
                    render_face_frame(s_ui.current_expression, 0, 0,
                                      closing[i]);
                    vTaskDelay(pdMS_TO_TICKS(16));
                }
                vTaskDelay(pdMS_TO_TICKS(blink + 1 < count ? 16 : 48));
                for (int i = (int)sizeof(closing) - 1; i >= 0; --i) {
                    render_face_frame(s_ui.current_expression, 0, 0,
                                      closing[i]);
                    vTaskDelay(pdMS_TO_TICKS(16));
                }
                render_face_frame(s_ui.current_expression, 0, 0, 14);
                if (blink + 1 < count) {
                    vTaskDelay(pdMS_TO_TICKS(32));
                }
            }
            return;
        case ROBOT_ANIMATION_WAKEUP:
            s_ui.current_expression = ROBOT_EXPRESSION_NEUTRAL;
            for (int height = 2; height <= 14; height += 3) {
                render_face_frame(ROBOT_EXPRESSION_NEUTRAL, 0, 0, height);
                vTaskDelay(pdMS_TO_TICKS(65));
            }
            render_face_frame(ROBOT_EXPRESSION_HAPPY, 0, 0, 14);
            vTaskDelay(pdMS_TO_TICKS(240));
            s_ui.current_expression = ROBOT_EXPRESSION_NEUTRAL;
            render_face_frame(s_ui.current_expression, 0, 0, 14);
            return;
        case ROBOT_ANIMATION_SLEEP:
            s_ui.current_expression = ROBOT_EXPRESSION_SLEEP;
            render_face_frame(s_ui.current_expression, 0, 0, 2);
            return;
        case ROBOT_ANIMATION_LOOK_LEFT:
            execute_look_animation(-1);
            return;
        case ROBOT_ANIMATION_LOOK_RIGHT:
            execute_look_animation(1);
            return;
        case ROBOT_ANIMATION_LOOK_UP:
            animate_offset(s_ui.current_expression, 0, -6);
            return;
        case ROBOT_ANIMATION_LOOK_DOWN:
            animate_offset(s_ui.current_expression, 0, 6);
            return;
        case ROBOT_ANIMATION_HEAD_NOD:
            for (int i = 0; i < 3; ++i) {
                render_face_frame(s_ui.current_expression, 0, 5, 10);
                vTaskDelay(pdMS_TO_TICKS(120));
                render_face_frame(s_ui.current_expression, 0, -3, 14);
                vTaskDelay(pdMS_TO_TICKS(120));
            }
            render_face_frame(s_ui.current_expression, 0, 0, 14);
            return;
        case ROBOT_ANIMATION_HEAD_SHAKE:
            for (int i = 0; i < 2; ++i) {
                render_face_frame(s_ui.current_expression, -7, 0, 14);
                vTaskDelay(pdMS_TO_TICKS(110));
                render_face_frame(s_ui.current_expression, 7, 0, 14);
                vTaskDelay(pdMS_TO_TICKS(110));
            }
            render_face_frame(s_ui.current_expression, 0, 0, 14);
            return;
        case ROBOT_ANIMATION_HEAD_ROLL: {
            static const int8_t path[][2] = {
                {0, 5}, {5, 4}, {8, 0}, {5, -4},
                {0, -5}, {-5, -4}, {-8, 0}, {-5, 4}, {0, 0},
            };
            for (size_t i = 0; i < sizeof(path) / sizeof(path[0]); ++i) {
                render_face_frame(s_ui.current_expression,
                                  path[i][0], path[i][1], 14);
                vTaskDelay(pdMS_TO_TICKS(90));
            }
            return;
        }
        case ROBOT_ANIMATION_CONFUSED:
            render_eye_pair(39, 32, 40, 20, 89, 32, 40, 20);
            vTaskDelay(pdMS_TO_TICKS(1000));
            render_face_frame(ROBOT_EXPRESSION_NEUTRAL, 0, 0, 14);
            return;
        case ROBOT_ANIMATION_RANDOM: {
            /* Same distribution as EmojiController::SelectRandomAnimation:
             * blink 60%, left 18%, right 18%, head roll 1%, confused 3%. */
            uint32_t value = esp_random() % 100;
            robot_animation_t idle = value < 60 ? ROBOT_ANIMATION_BLINK
                : value < 78 ? ROBOT_ANIMATION_LOOK_LEFT
                : value < 96 ? ROBOT_ANIMATION_LOOK_RIGHT
                : value < 97 ? ROBOT_ANIMATION_HEAD_ROLL
                             : ROBOT_ANIMATION_CONFUSED;
            execute_animation(idle);
            return;
        }
        case ROBOT_ANIMATION_STOP:
            return;
        default:
            break;
    }

    robot_expression_t restore_expression = s_ui.current_expression;
    if (restore_expression == ROBOT_EXPRESSION_SLEEP) {
        restore_expression = ROBOT_EXPRESSION_NEUTRAL;
    }
    for (int y = 2; y >= 0; --y) {
        render_face_frame(expression, 0, y, 14);
        vTaskDelay(pdMS_TO_TICKS(45));
    }
    /* Happy needs enough dwell time to be readable on the small OLED. */
    const uint32_t expression_hold_ms =
        expression == ROBOT_EXPRESSION_HAPPY ? 1500 : 700;
    vTaskDelay(pdMS_TO_TICKS(expression_hold_ms));
    s_ui.current_expression = restore_expression;
    render_face_frame(restore_expression, 0, 0, 14);
}

static void animation_task(void *arg)
{
    (void)arg;
    while (s_ui.animation_running) {
        robot_animation_message_t message = {0};
        if (xQueueReceive(s_ui.animation_queue, &message,
                          portMAX_DELAY) == pdTRUE) {
            if (message.animation == ROBOT_ANIMATION_STOP) {
                if (message.waiter) {
                    xTaskNotifyGive(message.waiter);
                }
                break;
            }
            execute_animation(message.animation);
            if (message.waiter) {
                xTaskNotifyGive(message.waiter);
            }
        }
    }
    s_ui.animation_task = NULL;
    vTaskDelete(NULL);
}

static int provisioning_connect_width(void)
{
    return (int)(sizeof(PROVISION_CONNECT) / sizeof(PROVISION_CONNECT[0])) *
               CHINESE_GLYPH_WIDTH +
           ascii_text_width(" WiFi: ") +
           ascii_text_width(s_ui.provisioning_ssid);
}

static int provisioning_visit_width(void)
{
    return (int)(sizeof(PROVISION_VISIT) / sizeof(PROVISION_VISIT[0])) *
               CHINESE_GLYPH_WIDTH +
           ascii_text_width(" ") +
           ascii_text_width(s_ui.provisioning_address) +
           ascii_text_width(" ") +
           (int)(sizeof(PROVISION_FINISH) / sizeof(PROVISION_FINISH[0])) *
               CHINESE_GLYPH_WIDTH;
}

static void draw_provisioning_connect_line(int x)
{
    draw_chinese_text(x, 24, PROVISION_CONNECT,
                      sizeof(PROVISION_CONNECT) / sizeof(PROVISION_CONNECT[0]));
    x += (int)(sizeof(PROVISION_CONNECT) / sizeof(PROVISION_CONNECT[0])) *
         CHINESE_GLYPH_WIDTH;
    draw_ascii_text(x, 24, " WiFi: ");
    x += ascii_text_width(" WiFi: ");
    draw_ascii_text(x, 24, s_ui.provisioning_ssid);
}

static void draw_provisioning_visit_line(int x)
{
    draw_chinese_text(x, 45, PROVISION_VISIT,
                      sizeof(PROVISION_VISIT) / sizeof(PROVISION_VISIT[0]));
    x += (int)(sizeof(PROVISION_VISIT) / sizeof(PROVISION_VISIT[0])) *
         CHINESE_GLYPH_WIDTH;
    draw_ascii_text(x, 45, " ");
    x += ascii_text_width(" ");
    draw_ascii_text(x, 45, s_ui.provisioning_address);
    x += ascii_text_width(s_ui.provisioning_address);
    draw_ascii_text(x, 45, " ");
    x += ascii_text_width(" ");
    draw_chinese_text(x, 45, PROVISION_FINISH,
                      sizeof(PROVISION_FINISH) / sizeof(PROVISION_FINISH[0]));
}

static esp_err_t draw_provisioning_frame(int connect_offset, int visit_offset)
{
    int connect_width = provisioning_connect_width();
    int visit_width = provisioning_visit_width();
    int connect_period = connect_width + PROVISION_SCROLL_GAP_PIXELS;
    int visit_period = visit_width + PROVISION_SCROLL_GAP_PIXELS;

    memset(s_ui.pixels, 0, sizeof(s_ui.pixels));
    draw_chinese_text_centered(
        1, PROVISION_TITLE,
        sizeof(PROVISION_TITLE) / sizeof(PROVISION_TITLE[0]));
    line(8, 20, DISPLAY_WIDTH - 9, 20);

    draw_provisioning_connect_line(connect_offset);
    draw_provisioning_connect_line(connect_offset + connect_period);
    draw_provisioning_visit_line(visit_offset);
    draw_provisioning_visit_line(visit_offset + visit_period);
    return flush();
}

static void provisioning_scroll_task(void *arg)
{
    (void)arg;
    int connect_offset = 0;
    int visit_offset = 0;

    while (true) {
        xSemaphoreTake(s_ui.lock, portMAX_DELAY);
        if (!s_ui.provisioning_active) {
            xSemaphoreGive(s_ui.lock);
            break;
        }

        int connect_period =
            provisioning_connect_width() + PROVISION_SCROLL_GAP_PIXELS;
        int visit_period =
            provisioning_visit_width() + PROVISION_SCROLL_GAP_PIXELS;
        esp_err_t err = draw_provisioning_frame(connect_offset, visit_offset);
        xSemaphoreGive(s_ui.lock);

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "provisioning display refresh failed: %s",
                     esp_err_to_name(err));
        }
        connect_offset -= PROVISION_SCROLL_STEP_PIXELS;
        visit_offset -= PROVISION_SCROLL_STEP_PIXELS;
        if (connect_offset <= -connect_period) {
            connect_offset = 0;
        }
        if (visit_offset <= -visit_period) {
            visit_offset = 0;
        }
        ulTaskNotifyTake(pdTRUE,
                         pdMS_TO_TICKS(PROVISION_SCROLL_INTERVAL_MS));
    }

    s_ui.provisioning_task = NULL;
    vTaskDelete(NULL);
}

static void stop_provisioning_scroll(void)
{
    s_ui.provisioning_active = false;
    TaskHandle_t task = s_ui.provisioning_task;
    if (task && task != xTaskGetCurrentTaskHandle()) {
        xTaskNotifyGive(task);
    }
}

esp_err_t robot_ui_init(void)
{
    esp_err_t ret = ESP_OK;
    if (s_ui.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = 41,
        .scl_io_num = 42,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &s_ui.bus), TAG, "new I2C bus");
    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = 0x3c,
        .scl_speed_hz = 400000,
        .control_phase_bytes = 1,
        .dc_bit_offset = 6,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_i2c(s_ui.bus, &io_config, &s_ui.io),
                      fail, TAG, "new panel IO");
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .bits_per_pixel = 1,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_ssd1306(s_ui.io, &panel_config, &s_ui.panel),
                      fail, TAG, "new SSD1306");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(s_ui.panel), fail, TAG, "reset display");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(s_ui.panel), fail, TAG, "init display");
    /* The DeskEmoji OLED is mounted rotated 180 degrees. This matches the
       original XiaoZhi board implementation (mirror_x=true, mirror_y=true). */
    ESP_GOTO_ON_ERROR(esp_lcd_panel_mirror(s_ui.panel, true, true),
                      fail, TAG, "mirror display");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_disp_on_off(s_ui.panel, true), fail, TAG, "display on");
    s_ui.lock = xSemaphoreCreateMutex();
    if (!s_ui.lock) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    s_ui.animation_queue =
        xQueueCreate(ANIMATION_QUEUE_DEPTH,
                     sizeof(robot_animation_message_t));
    if (!s_ui.animation_queue) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    s_ui.current_expression = ROBOT_EXPRESSION_NEUTRAL;
    s_ui.idle_animation_enabled = true;
    s_ui.animation_running = true;
    s_ui.initialized = true;
    ESP_GOTO_ON_ERROR(
        robot_ui_show_expression(ROBOT_EXPRESSION_NEUTRAL),
        fail, TAG, "initial expression");
    if (xTaskCreate(animation_task, "emoji_animation", 4096, NULL, 3,
                    &s_ui.animation_task) != pdPASS) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    return ESP_OK;

fail:
    robot_ui_deinit();
    return ret == ESP_OK ? ESP_FAIL : ret;
}

esp_err_t robot_ui_show_expression(robot_expression_t expression)
{
    if (!s_ui.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (expression < ROBOT_EXPRESSION_NEUTRAL ||
        expression >= ROBOT_EXPRESSION_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    stop_provisioning_scroll();
    xSemaphoreTake(s_ui.lock, portMAX_DELAY);
    s_ui.current_expression = expression;
    draw_face(expression, 0, 0,
              expression == ROBOT_EXPRESSION_SLEEP ? 2 : 14);
    esp_err_t err = flush();
    xSemaphoreGive(s_ui.lock);
    return err;
}

esp_err_t robot_ui_play_animation(robot_animation_t animation)
{
    if (!s_ui.initialized || !s_ui.animation_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    if (animation < ROBOT_ANIMATION_BLINK ||
        animation > ROBOT_ANIMATION_RANDOM) {
        return ESP_ERR_INVALID_ARG;
    }
    robot_animation_message_t message = {
        .animation = animation,
        .waiter = NULL,
    };
    return xQueueSend(s_ui.animation_queue, &message, 0) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

esp_err_t robot_ui_begin_animation(robot_animation_t animation)
{
    if (!s_ui.initialized || !s_ui.animation_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    if (animation < ROBOT_ANIMATION_BLINK ||
        animation > ROBOT_ANIMATION_RANDOM) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Discard a stale completion notification before tracking a new item. */
    ulTaskNotifyTake(pdTRUE, 0);
    robot_animation_message_t message = {
        .animation = animation,
        .waiter = xTaskGetCurrentTaskHandle(),
    };
    return xQueueSend(s_ui.animation_queue, &message, 0) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

esp_err_t robot_ui_wait_animation(uint32_t timeout_ms)
{
    TickType_t timeout = timeout_ms ? pdMS_TO_TICKS(timeout_ms)
                                    : portMAX_DELAY;
    return ulTaskNotifyTake(pdTRUE, timeout) > 0 ? ESP_OK : ESP_ERR_TIMEOUT;
}

void robot_ui_set_idle_animation(bool enabled)
{
    s_ui.idle_animation_enabled = enabled;
}

bool robot_ui_idle_animation_enabled(void)
{
    return s_ui.initialized && s_ui.idle_animation_enabled &&
           !s_ui.provisioning_active &&
           s_ui.current_expression != ROBOT_EXPRESSION_SLEEP;
}

bool robot_ui_i2c_take(uint32_t timeout_ms)
{
    if (!s_ui.lock) {
        return false;
    }
    TickType_t timeout = timeout_ms ? pdMS_TO_TICKS(timeout_ms) : 0;
    return xSemaphoreTake(s_ui.lock, timeout) == pdTRUE;
}

void robot_ui_i2c_give(void)
{
    if (s_ui.lock) {
        xSemaphoreGive(s_ui.lock);
    }
}

esp_err_t robot_ui_show_provisioning(const char *ssid, const char *url)
{
    if (!s_ui.initialized || !ssid || !url) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_ui.lock, portMAX_DELAY);
    snprintf(s_ui.provisioning_ssid, sizeof(s_ui.provisioning_ssid), "%s",
             ssid);
    snprintf(s_ui.provisioning_address, sizeof(s_ui.provisioning_address),
             "%s%s", strstr(url, "://") ? "" : "http://", url);
    s_ui.provisioning_active = true;
    esp_err_t err = draw_provisioning_frame(0, 0);
    bool create_task = s_ui.provisioning_task == NULL;
    xSemaphoreGive(s_ui.lock);
    if (err != ESP_OK) {
        s_ui.provisioning_active = false;
        return err;
    }

    if (create_task &&
        xTaskCreate(provisioning_scroll_task, "ui_provision", 3072, NULL, 2,
                    &s_ui.provisioning_task) != pdPASS) {
        s_ui.provisioning_active = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "provisioning: SSID=%s URL=%s", ssid, url);
    return ESP_OK;
}

void robot_ui_deinit(void)
{
    stop_provisioning_scroll();
    s_ui.idle_animation_enabled = false;
    if (s_ui.animation_running) {
        s_ui.animation_running = false;
        if (s_ui.animation_queue) {
            robot_animation_message_t stop = {
                .animation = ROBOT_ANIMATION_STOP,
                .waiter = NULL,
            };
            xQueueSend(s_ui.animation_queue, &stop, 0);
        }
    }
    for (int i = 0; s_ui.provisioning_task && i < 20; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    for (int i = 0; s_ui.animation_task && i < 200; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_ui.panel) {
        esp_lcd_panel_disp_on_off(s_ui.panel, false);
        esp_lcd_panel_del(s_ui.panel);
    }
    if (s_ui.io) {
        esp_lcd_panel_io_del(s_ui.io);
    }
    if (s_ui.bus) {
        i2c_del_master_bus(s_ui.bus);
    }
    if (s_ui.lock) {
        vSemaphoreDelete(s_ui.lock);
    }
    if (s_ui.animation_queue) {
        vQueueDelete(s_ui.animation_queue);
    }
    memset(&s_ui, 0, sizeof(s_ui));
}
