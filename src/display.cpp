#include "display.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <oled.h>

static bool displayOk = false;

static void textCentered(int y, const char *s) {
    if (!s) return;
    int x = (128 - oled_utf8_width(s)) / 2;
    if (x < 0) x = 0;
    oled_draw_utf8(x, y, s);
}

enum IconType {
    ICON_CLEAR,
    ICON_PARTLY,
    ICON_CLOUDY,
    ICON_RAIN,
    ICON_SNOW,
    ICON_THUNDER,
    ICON_FOG
};

static String toLowerRu(const String &src) {
    String r = src;
    for (size_t i = 0; i < r.length(); i++) {
        uint8_t c = (uint8_t)r[i];

        if (c >= 'A' && c <= 'Z') {
            r[i] = (char)(c + 32);
        } else if (c == 0xD0 && i + 1 < r.length()) {
            uint8_t n = (uint8_t)r[i + 1];
            if (n >= 0x90 && n <= 0xAF) {
                r[i + 1] = (char)(n + 0x20);
            } else if (n == 0x81) {
                r[i] = (char)0xD1;
                r[i + 1] = (char)0x91;
            }
            i++;
        }
    }
    return r;
}

static IconType iconForCondition(const String &condition) {
    String c = toLowerRu(condition);

    if (c.indexOf("гроз") >= 0) return ICON_THUNDER;

    if (c.indexOf("снег") >= 0 || c.indexOf("снеж") >= 0 ||
        c.indexOf("метел") >= 0 || c.indexOf("град") >= 0)
        return ICON_SNOW;

    if (c.indexOf("дожд") >= 0 || c.indexOf("ливень") >= 0 ||
        c.indexOf("морос") >= 0)
        return ICON_RAIN;

    if (c.indexOf("туман") >= 0 || c.indexOf("дымка") >= 0 ||
        c.indexOf("мгла") >= 0)
        return ICON_FOG;

    if (c.indexOf("переменная") >= 0 || c.indexOf("проясн") >= 0)
        return ICON_PARTLY;

    if (c.indexOf("ясно") >= 0 || c.indexOf("солнеч") >= 0 ||
        c.indexOf("солнце") >= 0)
        return ICON_CLEAR;

    if (c.indexOf("облач") >= 0 || c.indexOf("пасмур") >= 0) return ICON_CLOUDY;

    return ICON_CLOUDY;
}

static void drawSun(int cx, int cy, int r, uint8_t frame) {
    int rr = ((frame >> 3) & 1) ? r : r - 1;
    oled_fill_circle(cx, cy, rr);

    float base = frame * ((float)PI / 16.0f);

    for (int i = 0; i < 8; i++) {
        float a = base + i * (float)PI / 4.0f;

        int len = (((frame >> 1) + i) & 3) < 2 ? 3 : 5;

        int r0 = rr + 2;
        oled_draw_line(cx + (int)(cos(a) * r0), cy + (int)(sin(a) * r0),
                       cx + (int)(cos(a) * (r0 + len)),
                       cy + (int)(sin(a) * (r0 + len)));
    }

    float ga = -base * 1.6f;
    int gx = cx + (int)(cos(ga) * (rr + 6));
    int gy = cy + (int)(sin(ga) * (rr + 6));
    oled_set_pixel(gx, gy, true);
    oled_set_pixel(gx + 1, gy, true);
}

static void drawCloud(int cx, int cy, uint8_t frame) {
    int bob = (int)lroundf(sinf(frame * 0.35f) * 1.9f);

    oled_fill_circle(cx - 8, cy + 3 + bob, 5);
    oled_fill_circle(cx,      cy - 1 + bob, 7);
    oled_fill_circle(cx + 8,  cy + 3 + bob, 5);
    oled_fill_rect(cx - 13, cy + 3 + bob, 26, 5);
}

static void drawSnowflake(int x, int y) {
    oled_draw_line(x,     y - 2, x,     y + 2);
    oled_draw_line(x - 2, y - 2, x + 2, y + 2);
    oled_draw_line(x + 2, y - 2, x - 2, y + 2);
}

static void drawIcon(IconType type, int cx, int cy, uint8_t frame) {
    switch (type) {
        case ICON_CLEAR:
            drawSun(cx, cy, 6, frame);
            break;

        case ICON_PARTLY:
            drawSun(cx + 6, cy - 6, 4, frame);
            drawCloud(cx - 3, cy + 2, frame);
            break;

        case ICON_CLOUDY:
            drawCloud(cx, cy, frame);
            break;

        case ICON_RAIN: {
            drawCloud(cx, cy - 5, frame);

            for (int i = 0; i < 5; i++) {
                int speed = 1 + (i & 1);
                int phase = (frame * speed + i * 3) % 9;
                int x = cx - 10 + i * 5;
                int y = cy + 1 + phase;

                oled_draw_line(x, y, x - 1, y + 3);

                if (phase >= 7) {
                    oled_set_pixel(x - 2, y + 4, true);
                    oled_set_pixel(x + 1, y + 4, true);
                }
            }
            break;
        }

        case ICON_SNOW:
            drawCloud(cx, cy - 5, frame);
            for (int i = 0; i < 3; i++) {
                int speed = 1 + (i & 1);
                int y = cy + 2 + (int)((frame * speed + i * 4) % 9);
                int wobble = ((frame / 2 + i) & 1) ? 1 : -1;
                drawSnowflake(cx - 8 + i * 8 + wobble, y);
            }
            break;

        case ICON_THUNDER:
            drawCloud(cx, cy - 6, frame);
            if ((frame % 10) < 2 || ((frame % 10) >= 3 && (frame % 10) < 5)) {
                oled_draw_line(cx,      cy + 2,  cx - 4, cy + 8);
                oled_draw_line(cx - 4,  cy + 8,  cx,     cy + 8);
                oled_draw_line(cx,      cy + 8,  cx - 2, cy + 13);
                oled_draw_line(cx - 2,  cy + 13, cx + 6, cy + 6);
                oled_draw_line(cx + 6,  cy + 6,  cx + 2, cy + 6);

                if ((frame % 10) < 1) {
                    oled_set_pixel(cx - 6, cy + 9,  true);
                    oled_set_pixel(cx + 7, cy + 5,  true);
                    oled_set_pixel(cx + 1, cy + 15, true);
                }
            }
            break;

        case ICON_FOG:
            drawCloud(cx, cy - 6, frame);
            for (int i = 0; i < 3; i++) {
                int shift = (int)((frame / 2 + i * 2) % 6) - 2;
                int left  = cx - 12 + shift + (i & 1);
                int right = cx + 10 + shift - (i & 1) * 2;
                oled_draw_line(left, cy + 5 + i * 4, right, cy + 5 + i * 4);
            }
            break;
    }
}

bool displayInit() {
    displayOk = oled_init();
    return displayOk;
}

void displayMessage(const char *line1, const char *line2) {
    if (!displayOk) return;

    oled_clear_buffer();
    textCentered(20, line1);
    textCentered(36, line2);
    oled_update();
}

void displayClock(int hour, int minute, int day, int month, int year,
                  const char* cond, const char* tempC, bool stale,
                  uint8_t frame) {
    if (!displayOk) return;

    oled_clear_buffer();

    char timeText[8];
    if (hour < 0) {
        snprintf(timeText, sizeof(timeText), "--:--");
    } else {
        snprintf(timeText, sizeof(timeText), "%02d:%02d", hour % 24, minute % 60);
    }

    int timeWidth = oled_utf8_len(timeText) * 24;
    oled_draw_utf8_scaled((128 - timeWidth) / 2, 0, timeText, 3);

    if (hour < 0) {
        textCentered(25, "синхронизация...");
    } else {
        char dateText[16];
        snprintf(dateText, sizeof(dateText), "%02d.%02d.%02d",
                 day % 100, month % 100, year % 100);
        textCentered(25, dateText);
    }

    if (cond && *cond) {
        drawIcon(iconForCondition(String(cond)), 24, 49, frame);
    }

    if (tempC && *tempC) {
        int tempWidth = oled_utf8_len(tempC) * 16;
        int tempX = 48 + (76 - tempWidth) / 2;
        if (tempX < 48) tempX = 48;
        oled_draw_utf8_scaled(tempX, 42, tempC, 2);

        int degreeX = tempX + tempWidth + 2;
        if (degreeX > 124) degreeX = 124;
        oled_draw_circle(degreeX, 45, 2);
    }

    if (stale) oled_draw_char(120, 25, '!');

    oled_update();
}

void displayWeek(const WeekDay* days, int count, bool stale) {
    if (!displayOk) return;

    oled_clear_buffer();

    if (count <= 0) {
        textCentered(28, "Нет данных");
        oled_update();
        return;
    }
    if (count > 7) count = 7;

    int minT = days[0].tmax, maxT = days[0].tmax;
    for (int i = 1; i < count; i++) {
        if (days[i].tmax < minT) minT = days[i].tmax;
        if (days[i].tmax > maxT) maxT = days[i].tmax;
    }

    const int baseline = 52;
    const int maxH = 34;
    const int minH = 4;

    for (int i = 0; i < count; i++) {
        int cx = 12 + i * 17;

        int h;
        if (maxT == minT) h = (minH + maxH) / 2;
        else              h = minH + (days[i].tmax - minT) * (maxH - minH) / (maxT - minT);

        oled_fill_rect(cx - 4, baseline - h, 9, h);

        char tempText[8];
        snprintf(tempText, sizeof(tempText), "%+d", days[i].tmax);
        int tw = oled_utf8_width(tempText);
        int tx = cx - tw / 2;
        if (tx < 0) tx = 0;
        if (tx + tw > 128) tx = 128 - tw;
        oled_draw_utf8(tx, (i & 1) ? 9 : 0, tempText);

        if (days[i].label) {
            int lw = oled_utf8_width(days[i].label);
            int lx = cx - lw / 2;
            if (lx < 0) lx = 0;
            if (lx + lw > 128) lx = 128 - lw;
            oled_draw_utf8(lx, baseline + 2, days[i].label);
        }
    }

    oled_draw_line(0, baseline + 1, 127, baseline + 1);

    if (stale) oled_draw_char(120, 44, '!');

    oled_update();
}
