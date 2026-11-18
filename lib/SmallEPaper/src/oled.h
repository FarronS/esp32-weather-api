#pragma once

#include <Arduino.h>

// ===========================================================================
//  SmallEPaper — минималистичный драйвер OLED SSD1306/SSD1315 128x64 по I2C.
//
//  Порт драйвера из репозитория SmallePlayer/blink-main (main/oled.c,
//  main/oled_font.h) с ESP-IDF на Arduino: вместо i2c_master + esp_lcd
//  используется Wire, API и логика фреймбуфера сохранены.
//
//  Что внутри:
//    * свой фреймбуфер 1 КБ в формате страниц SSD1306 (8 строк по 128 байт);
//    * шрифт 8x8: ASCII 0x20..0x7E + кириллица А-Я, а-я, Ё, ё (UTF-8);
//    * примитивы: точка, линия, прямоугольник, круг, строка;
//    * масштабирование текста (x2, x3...) для крупных цифр;
//    * кнопка на GPIO с определением фронта нажатия.
//
//  Модель работы: рисуем в память (oled_*) -> oled_update() отправляет всё
//  на экран одним проходом. Никаких обменов по I2C при рисовании.
// ===========================================================================

// --- Пины (по умолчанию — распиновка Heltec WiFi LoRa 32 V4) ---
// Переопределяются флагами сборки: -DOLED_SDA=... (см. platformio.ini).
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

// Прямой доступ к фреймбуферу (1024 байта).
// Байт = (y / 8) * 128 + x, бит = y % 8.
extern uint8_t oled_fb[OLED_W * OLED_H / 8];

// --- Инициализация ---
// Питание Vext + Wire + сброс + команды SSD1306.
// true — дисплей ответил по I2C и готов.
bool oled_init();
void oled_power_on();                 // только подать питание на Vext

// --- Обмен с экраном ---
void oled_update();                   // отправить фреймбуфер на дисплей
void oled_clear_buffer();             // очистить только фреймбуфер (без I2C)
void oled_clear();                    // очистить фреймбуфер + обновить экран
void oled_fill(uint8_t value);        // залить фреймбуфер 0x00 или 0xFF

// --- Рисование во фреймбуфер (без I2C) ---
void oled_set_pixel(int x, int y, bool on);
bool oled_get_pixel(int x, int y);

// Текст. Высота символа 8 px, шаг по X — 8 px (или 8*scale).
void oled_draw_char(int x, int y, char c);             // 1 байт ASCII
void oled_draw_string(int x, int y, const char *str);  // ASCII-строка
void oled_draw_cp(int x, int y, uint32_t cp);          // 1 символ Unicode
int  oled_draw_utf8(int x, int y, const char *s);      // UTF-8 строка -> ширина
int  oled_utf8_width(const char *s);                   // ширина строки в px
int  oled_utf8_len(const char *s);                     // длина в символах

// Масштабированный текст (scale=2 -> символ 16x16).
void oled_draw_cp_scaled(int x, int y, uint32_t cp, int scale);
int  oled_draw_utf8_scaled(int x, int y, const char *s, int scale);

// Фигуры.
void oled_draw_line(int x0, int y0, int x1, int y1);
void oled_draw_rect(int x, int y, int w, int h);       // рамка
void oled_fill_rect(int x, int y, int w, int h);       // заливка
void oled_draw_circle(int cx, int cy, int r);          // окружность
void oled_fill_circle(int cx, int cy, int r);          // диск

// --- Настройки панели (сразу уходят на экран) ---
void oled_invert(bool invert);        // инверсия изображения
void oled_contrast(uint8_t value);    // яркость 0..255
void oled_display(bool on);           // on/off (спящий режим)

// --- Кнопка (активна в 0, подтяжка к питанию) ---
void button_init(int pin);            // INPUT_PULLUP
bool button_clicked(int pin);         // true один раз на фронт нажатия
bool button_pressed(int pin);         // true, пока держат
