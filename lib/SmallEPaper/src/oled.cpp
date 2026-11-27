#include "oled.h"

#include <Wire.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <math.h>

#include "oled_font.h"

#define OLED_PAGES (OLED_H / 8)
#define OLED_CHUNK 64

uint8_t oled_fb[OLED_W * OLED_H / 8];

static bool s_ok = false;

static void send_cmds(const uint8_t *cmds, size_t n) {
    Wire.beginTransmission(OLED_ADDR);
    Wire.write((uint8_t)0x00);
    Wire.write(cmds, (uint8_t)n);
    Wire.endTransmission();
}

static void send_cmd(uint8_t c) {
    send_cmds(&c, 1);
}

static void send_page(uint8_t page, const uint8_t *data, size_t n) {
    uint8_t addr[3] = { (uint8_t)(0xB0 | (page & 0x07)), 0x00, 0x10 };
    send_cmds(addr, 3);

    for (size_t off = 0; off < n; off += OLED_CHUNK) {
        size_t chunk = (n - off < OLED_CHUNK) ? (n - off) : OLED_CHUNK;
        Wire.beginTransmission(OLED_ADDR);
        Wire.write((uint8_t)0x40);
        Wire.write(data + off, (uint8_t)chunk);
        Wire.endTransmission();
    }
}

static bool i2c_present(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

static void hw_reset() {
#ifdef OLED_RST
    pinMode(OLED_RST, OUTPUT);
    digitalWrite(OLED_RST, LOW);
    delay(50);
    digitalWrite(OLED_RST, HIGH);
    delay(50);
#endif
}

void oled_power_on() {
#ifdef OLED_VEXT
    pinMode(OLED_VEXT, OUTPUT);
    digitalWrite(OLED_VEXT, LOW);
    delay(100);
#endif
}

bool oled_init() {
    oled_power_on();
    delay(50);

    Wire.begin(OLED_SDA, OLED_SCL);
    Wire.setClock(400000);
    delay(10);

    hw_reset();

    if (!i2c_present(OLED_ADDR)) {
        s_ok = false;
        return false;
    }

    static const uint8_t init_seq[] = {
        0xAE,
        0xD5, 0x80,
        0xA8, 0x3F,
        0xD3, 0x00,
        0x40,
        0x8D, 0x14,
        0x20, 0x00,
        0xA1,
        0xC8,
        0xDA, 0x12,
        0x81, 0xCF,
        0xD9, 0xF1,
        0xDB, 0x40,
        0xA4,
        0xA6,
        0x2E,
        0xAF
    };
    send_cmds(init_seq, sizeof(init_seq));

    memset(oled_fb, 0, sizeof(oled_fb));
    oled_update();

    s_ok = true;
    return true;
}

void oled_update() {
    if (!s_ok) return;
    for (uint8_t page = 0; page < OLED_PAGES; page++) {
        send_page(page, oled_fb + page * OLED_W, OLED_W);
    }
}

void oled_clear_buffer() {
    memset(oled_fb, 0, sizeof(oled_fb));
}

void oled_clear() {
    oled_clear_buffer();
    oled_update();
}

void oled_fill(uint8_t value) {
    memset(oled_fb, value, sizeof(oled_fb));
    oled_update();
}

void oled_set_pixel(int x, int y, bool on) {
    if (x < 0 || x >= OLED_W || y < 0 || y >= OLED_H) return;
    int idx = (y / 8) * OLED_W + x;
    if (on) oled_fb[idx] |= (uint8_t)(1 << (y % 8));
    else    oled_fb[idx] &= (uint8_t)~(1 << (y % 8));
}

bool oled_get_pixel(int x, int y) {
    if (x < 0 || x >= OLED_W || y < 0 || y >= OLED_H) return false;
    return (oled_fb[(y / 8) * OLED_W + x] >> (y % 8)) & 0x01;
}

static size_t utf8_seq_len(uint8_t c) {
    if (c < 0x80) return 1;
    if ((c >> 5) == 0x06) return 2;
    if ((c >> 4) == 0x0E) return 3;
    if ((c >> 3) == 0x1E) return 4;
    return 1;
}

static uint32_t utf8_decode(const char *&s) {
    uint8_t c = (uint8_t)*s;
    if (c == 0) return 0;

    size_t n = utf8_seq_len(c);
    if (n == 1) { s++; return c; }

    uint32_t cp = c & (0xFFu >> (n + 1));
    for (size_t i = 1; i < n; i++) {
        uint8_t cc = (uint8_t)s[i];
        if ((cc & 0xC0) != 0x80) {
            s++;
            return 0x20;
        }
        cp = (cp << 6) | (cc & 0x3F);
    }
    s += n;
    return cp;
}

static void draw_glyph(int x, int y, const uint8_t *g, int scale) {
    if (scale < 1) scale = 1;
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            if (!(g[row] & (0x80 >> col))) continue;
            if (scale == 1) {
                oled_set_pixel(x + col, y + row, true);
            } else {
                for (int dy = 0; dy < scale; dy++)
                    for (int dx = 0; dx < scale; dx++)
                        oled_set_pixel(x + col * scale + dx, y + row * scale + dy, true);
            }
        }
    }
}

void oled_draw_cp(int x, int y, uint32_t cp) {
    draw_glyph(x, y, font_glyph_cp(cp), 1);
}

void oled_draw_cp_scaled(int x, int y, uint32_t cp, int scale) {
    draw_glyph(x, y, font_glyph_cp(cp), scale);
}

void oled_draw_char(int x, int y, char c) {
    draw_glyph(x, y, font_glyph(c), 1);
}

void oled_draw_string(int x, int y, const char *str) {
    while (str && *str) {
        oled_draw_char(x, y, *str);
        x += 8;
        str++;
    }
}

int oled_utf8_len(const char *s) {
    int n = 0;
    while (s && *s) {
        utf8_decode(s);
        n++;
    }
    return n;
}

int oled_utf8_width(const char *s) {
    return oled_utf8_len(s) * 8;
}

int oled_draw_utf8(int x, int y, const char *s) {
    int start = x;
    while (s && *s) {
        uint32_t cp = utf8_decode(s);
        if (cp == 0) break;
        oled_draw_cp(x, y, cp);
        x += 8;
    }
    return x - start;
}

int oled_draw_utf8_scaled(int x, int y, const char *s, int scale) {
    int start = x;
    int step = 8 * (scale < 1 ? 1 : scale);
    while (s && *s) {
        uint32_t cp = utf8_decode(s);
        if (cp == 0) break;
        oled_draw_cp_scaled(x, y, cp, scale);
        x += step;
    }
    return x - start;
}

void oled_printf(int x, int y, const char *fmt, ...) {
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    oled_draw_utf8(x, y, buf);
}

void oled_draw_line(int x0, int y0, int x1, int y1) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        oled_set_pixel(x0, y0, true);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void oled_draw_rect(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    oled_draw_line(x, y, x + w - 1, y);
    oled_draw_line(x, y + h - 1, x + w - 1, y + h - 1);
    oled_draw_line(x, y, x, y + h - 1);
    oled_draw_line(x + w - 1, y, x + w - 1, y + h - 1);
}

void oled_fill_rect(int x, int y, int w, int h) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            oled_set_pixel(x + i, y + j, true);
}

void oled_draw_circle(int cx, int cy, int r) {
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        oled_set_pixel(cx + x, cy + y, true);
        oled_set_pixel(cx + y, cy + x, true);
        oled_set_pixel(cx - y, cy + x, true);
        oled_set_pixel(cx - x, cy + y, true);
        oled_set_pixel(cx - x, cy - y, true);
        oled_set_pixel(cx - y, cy - x, true);
        oled_set_pixel(cx + y, cy - x, true);
        oled_set_pixel(cx + x, cy - y, true);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

void oled_fill_circle(int cx, int cy, int r) {
    for (int dy = -r; dy <= r; dy++) {
        int dx = (int)sqrtf((float)(r * r - dy * dy));
        for (int x = cx - dx; x <= cx + dx; x++)
            oled_set_pixel(x, cy + dy, true);
    }
}

void oled_invert(bool invert) {
    if (!s_ok) return;
    send_cmd(invert ? 0xA7 : 0xA6);
}

void oled_contrast(uint8_t value) {
    if (!s_ok) return;
    uint8_t c[2] = { 0x81, value };
    send_cmds(c, 2);
}

void oled_display(bool on) {
    if (!s_ok) return;
    send_cmd(on ? 0xAF : 0xAE);
}

void button_init(int pin) {
    pinMode(pin, INPUT_PULLUP);
}

bool button_clicked(int pin) {
    static int prev[40];
    static bool init_done = false;
    if (!init_done) {
        for (int i = 0; i < 40; i++) prev[i] = -1;
        init_done = true;
    }
    if (pin < 0 || pin >= 40) return false;
    if (prev[pin] < 0) prev[pin] = digitalRead(pin);

    int cur = digitalRead(pin);
    bool clicked = (prev[pin] == HIGH && cur == LOW);
    prev[pin] = cur;
    return clicked;
}

bool button_pressed(int pin) {
    return digitalRead(pin) == LOW;
}
