#include <lib/linearfb/linearfb.h>
#include <string.h>

static struct limine_framebuffer *fb = NULL;
static linearfb_mode_t fb_mode = FB_MODE_CONSOLE;
static linearfb_font_t fb_font = {0};
static uint32_t font_glyph_count = 0;
static uint32_t font_glyph_w = 0, font_glyph_h = 0;

// --- Console state ---
static uint32_t console_col = 0, console_row = 0;
static uint32_t console_cols = 0, console_rows = 0;

static uint32_t console_bg = 0x00000000;

void linearfb_console_set_cursor(uint32_t col, uint32_t row) {
    if (col < console_cols) __atomic_store_n(&console_col, col, __ATOMIC_SEQ_CST);
    if (row < console_rows) __atomic_store_n(&console_row, row, __ATOMIC_SEQ_CST);
}

void linearfb_console_get_cursor(uint32_t *col, uint32_t *row) {
    if (col) *col = console_col;
    if (row) *row = console_row;
}

static void putpixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!fb || x >= fb->width || y >= fb->height) return;
    uint8_t *p = (uint8_t*)fb->address + y * fb->pitch + x * (fb->bpp / 8);
    memcpy(p, &color, fb->bpp / 8);
}

void linearfb_console_clear(uint32_t color) {
    if (!fb) return;
    for (uint32_t y = 0; y < fb->height; ++y) {
        for (uint32_t x = 0; x < fb->width; ++x) {
            putpixel(x, y, color);
        }
    }
    console_col = 0;
    console_row = 0;
    console_bg = color;
}

static void console_scroll(void) {
    if (!fb || !fb_font.data) return;
    uint32_t row_bytes = fb->pitch * font_glyph_h;
    uint8_t *dst = (uint8_t*)fb->address;
    uint8_t *src = dst + row_bytes;
    memmove(dst, src, row_bytes * (console_rows - 1));
    // Clear last row
    for (uint32_t y = (console_rows - 1) * font_glyph_h; y < fb->height; ++y) {
        for (uint32_t x = 0; x < fb->width; ++x) {
            putpixel(x, y, console_bg);
        }
    }
    console_row = console_rows - 1;
}

void linearfb_console_putc(char c) {
    if (fb_mode != FB_MODE_CONSOLE || !fb_font.data) return;
    if (c == '\n') {
        console_col = 0;
        if (++console_row >= console_rows) console_scroll();
        return;
    } if (c == '\r') {
        console_col = 0;
        return;
    } if (c == '\b') {
        if (console_col > 0) --console_col;
        return;
    }
    // Draw glyph
    uint8_t ch = (uint8_t)c;
    if (ch >= font_glyph_count) ch = '?';
    const uint8_t *glyph = fb_font.data + ch * font_glyph_h;
    uint32_t px = console_col * font_glyph_w;
    uint32_t py = console_row * font_glyph_h;
    // Draw background
    for (uint32_t row = 0; row < font_glyph_h; ++row) {
        for (uint32_t col = 0; col < font_glyph_w; ++col) {
            putpixel(px + col, py + row, console_bg);
        }
    }
    // Draw glyph
    for (uint32_t row = 0; row < font_glyph_h; ++row) {
        uint8_t bits = glyph[row];
        for (uint32_t col = 0; col < font_glyph_w; ++col) {
            if (bits & (1 << (7 - col)))
                putpixel(px + col, py + row, 0xFFFFFFFF);
        }
    }
    if (++console_col >= console_cols) {
        console_col = 0;
        if (++console_row >= console_rows) console_scroll();
    }
}

void linearfb_console_puts(const char *s) {
    while (*s) linearfb_console_putc(*s++);
}

#define abs(x) ((x) >= 0 ? (x) : -(x))

int linearfb_init(struct limine_framebuffer_request *fb_req) {
    if (!fb_req || !fb_req->response || fb_req->response->framebuffer_count == 0)
        return -1;
    fb = fb_req->response->framebuffers[0];
    // Update console cols/rows if font is ready
    if (fb && font_glyph_w && font_glyph_h) {
        console_cols = fb->width / font_glyph_w;
        console_rows = fb->height / font_glyph_h;
    }
    return 0;
}

void linearfb_set_mode(const linearfb_mode_t mode) {
    __atomic_store_n(&fb_mode, mode, __ATOMIC_SEQ_CST);
}

int linearfb_load_font(const linearfb_font_t* font, const uint32_t count) {
    if (!font)
        return -1;
    fb_font = *font;
    font_glyph_w = font->width;
    font_glyph_h = font->height;
    font_glyph_count = count;
    // Update console cols/rows if fb is ready
    if (fb && font_glyph_w && font_glyph_h) {
        console_cols = fb->width / font_glyph_w;
        console_rows = fb->height / font_glyph_h;
    }
    return 0;
}

void linearfb_draw_text(const char *text, uint32_t x, uint32_t y) {
    if (fb_mode == FB_MODE_CONSOLE) return; // No drawing in console mode
    if (!fb_font.data) return;
    for (size_t i = 0; text[i]; ++i) {
        uint8_t ch = (uint8_t)text[i];
        if (ch >= font_glyph_count) ch = '?';
        const uint8_t *glyph = fb_font.data + ch * font_glyph_h;
        for (uint32_t row = 0; row < font_glyph_h; ++row) {
            uint8_t bits = glyph[row];
            for (uint32_t col = 0; col < font_glyph_w; ++col) {
                if (bits & (1 << (7 - col)))
                    putpixel(x + col, y + row, 0xFFFFFFFF);
            }
        }
        x += font_glyph_w;
    }
}

// Bresenham line
static void fb_draw_line(int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    while (1) {
        putpixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Simple polygon fill (scanline, convex only)
static void fb_fill_polygon(const int *x, const int *y, size_t n, uint32_t color) {
    int min_y = y[0], max_y = y[0];
    for (size_t i = 1; i < n; ++i) {
        if (y[i] < min_y) min_y = y[i];
        if (y[i] > max_y) max_y = y[i];
    }
    for (int yy = min_y; yy <= max_y; ++yy) {
        int nodes[64], nodes_n = 0;
        for (size_t i = 0, j = n - 1; i < n; j = i++) {
            if ((y[i] < yy && y[j] >= yy) || (y[j] < yy && y[i] >= yy)) {
                int xval = x[i] + (yy - y[i]) * (x[j] - x[i]) / (y[j] - y[i]);
                if (nodes_n < 64) nodes[nodes_n++] = xval;
            }
        }
        for (int k = 0; k + 1 < nodes_n; k += 2) {
            if (nodes[k] > nodes[k + 1]) {
                int tmp = nodes[k]; nodes[k] = nodes[k + 1]; nodes[k + 1] = tmp;
            }
            for (int xx = nodes[k]; xx <= nodes[k + 1]; ++xx)
                putpixel(xx, yy, color);
        }
    }
}

void linearfb_draw_polygon(const int *x, const int *y, size_t n, uint32_t color, int filled) {
    if (fb_mode == FB_MODE_CONSOLE) return;
    if (n < 2) return;
    if (filled) fb_fill_polygon(x, y, n, color);
    for (size_t i = 0; i < n; ++i) {
        size_t j = (i + 1) % n;
        fb_draw_line(x[i], y[i], x[j], y[j], color);
    }
}
