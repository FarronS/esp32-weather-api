#pragma once

#include <Arduino.h>

#ifndef OLED_SDA
  #define OLED_SDA 17
#endif

#ifndef OLED_SCL
  #define OLED_SCL 18
#endif

#ifndef OLED_RST
  #define OLED_RST 21
#endif

#ifndef OLED_VEXT
  #define OLED_VEXT 36
#endif

#ifndef OLED_ADDR
  #define OLED_ADDR 0x3C
#endif

#ifndef OLED_W
  #define OLED_W 128
#endif

#ifndef OLED_H
  #define OLED_H 64
#endif

extern uint8_t oled_fb[OLED_W * OLED_H / 8];

bool oled_init();
void oled_power_on();

void oled_update();
void oled_clear_buffer();
void oled_clear();
void oled_fill(uint8_t value);

void oled_set_pixel(int x, int y, bool on);
bool oled_get_pixel(int x, int y);

void oled_draw_char(int x, int y, char c);
void oled_draw_string(int x, int y, const char *str);
void oled_draw_cp(int x, int y, uint32_t cp);
int  oled_draw_utf8(int x, int y, const char *s);
int  oled_utf8_width(const char *s);
int  oled_utf8_len(const char *s);

void oled_draw_cp_scaled(int x, int y, uint32_t cp, int scale);
int  oled_draw_utf8_scaled(int x, int y, const char *s, int scale);

void oled_draw_line(int x0, int y0, int x1, int y1);
void oled_draw_rect(int x, int y, int w, int h);
void oled_fill_rect(int x, int y, int w, int h);
void oled_draw_circle(int cx, int cy, int r);
void oled_fill_circle(int cx, int cy, int r);

void oled_invert(bool invert);
void oled_contrast(uint8_t value);
void oled_display(bool on);

void button_init(int pin);
bool button_clicked(int pin);
bool button_pressed(int pin);
