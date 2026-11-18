# SmallEPaper

Минималистичный драйвер OLED **SSD1306 / SSD1315 128x64 по I2C** для ESP32 (Arduino).
Порт драйвера из репозитория [SmallePlayer/blink-main](https://github.com/SmallePlayer/blink-main)
(`main/oled.c` + `main/oled_font.h`, ESP-IDF) на Arduino `Wire`, плюс кириллица
в шрифте и масштабирование текста.

## Что изменилось при порте

| Оригинал (ESP-IDF)            | Здесь (Arduino)                          |
|-------------------------------|------------------------------------------|
| `i2c_master_*`                | `Wire` (400 кГц)                          |
| `esp_lcd_panel_ssd1306`       | прямые команды SSD1306 через `Wire`       |
| `esp_lcd_panel_draw_bitmap`   | `oled_update()` (8 страниц по 128 байт)   |
| `vTaskDelay`                  | `delay()`                                 |
| `gpio_get_level`              | `digitalRead()`                           |

Логика и API сохранены: тот же фреймбуфер, тот же шрифт 8x8, те же имена функций.

Дополнительно (чего не было в оригинале):

* **кириллица** А-Я, а-я, Ё, ё (UTF-8) в том же шрифте 8x8;
* `oled_draw_utf8()` — строки в UTF-8, `oled_utf8_width()` / `oled_utf8_len()`;
* **масштаб текста** x1..x4 (`oled_draw_utf8_scaled()`) — крупные цифры;
* фигуры `oled_draw_circle()` / `oled_fill_circle()`;
* `oled_get_pixel()`, `oled_printf()`.

## Состав

```
lib/SmallEPaper/
├── library.json          манифест PlatformIO
├── README.md             этот файл
├── src/
│   ├── oled.h            API
│   ├── oled.cpp          реализация (Wire + SSD1306)
│   └── oled_font.h       шрифт 8x8: ASCII + кириллица
└── examples/demo/demo.ino  пример
```

PlatformIO подхватывает `lib/` автоматически — в `platformio.ini` ничего
добавлять не нужно (в этом проекте так и сделано, `lib_deps` пустой).

## Модель работы

Рисование идёт **в память**, на экран данные уходят только по `oled_update()`:

```cpp
#include <oled.h>

void setup() {
    if (!oled_init()) { /* экран не ответил по I2C */ }

    oled_clear();                              // буфер = 0
    oled_draw_utf8(0, 0, "Погода");             // кириллица
    oled_draw_utf8_scaled(0, 12, "+5", 2);      // крупно, 16x16 на символ
    oled_draw_line(0, 10, 127, 10);
    oled_update();                             // отправить кадр на дисплей
}
```

Один кадр = один обмен по I2C. Перерисовка = `oled_clear()` и рисование заново:
у пикселя нет «цвета фона», есть только «включён / выключен».

## API

### Инициализация

| Функция | Что делает |
|---|---|
| `bool oled_init()` | Vext (питание), `Wire.begin(SDA,SCL)`, сброс RST, команды SSD1306. `false` — дисплей не ответил на адрес `0x3C`. |
| `void oled_power_on()` | только включить питание дисплея (Vext = LOW) |

### Вывод кадра

| Функция | Что делает |
|---|---|
| `oled_update()` | отправить фреймбуфер на экран |
| `oled_clear()` | очистить буфер и сразу обновить экран |
| `oled_clear_buffer()` | очистить только буфер (без I2C) — перерисовка без мигания |
| `oled_fill(v)` | залить буфер `0x00` (чёрный) или `0xFF` (белый) и обновить |

### Текст

| Функция | Что делает |
|---|---|
| `oled_draw_utf8(x, y, "текст")` | UTF-8 строка (латиница + кириллица), 8x8, шаг 8 px. Возвращает ширину в px |
| `oled_draw_utf8_scaled(x, y, s, scale)` | то же, символ 8*scale px (`scale=2` → 16x16) |
| `oled_draw_cp(x, y, cp)` | один символ по коду Unicode |
| `oled_draw_string(x, y, s)` | только ASCII (шаг 8 px) |
| `oled_printf(x, y, "fmt", ...)` | `printf` в строку (до 63 символов) |
| `int oled_utf8_width(s)` | ширина строки в пикселях |
| `int oled_utf8_len(s)` | длина строки в символах |

Координата `y` у текста — **верх** символа (в U8g2 была базовая линия).
Шаг строки — 8 px (для scale=2 — 16 px).

### Фигуры

| Функция | Что делает |
|---|---|
| `oled_set_pixel(x, y, on)` | поставить/снять пиксель |
| `bool oled_get_pixel(x, y)` | прочитать пиксель |
| `oled_draw_line(x0,y0,x1,y1)` | линия (Брезенхем) |
| `oled_draw_rect(x,y,w,h)` | рамка |
| `oled_fill_rect(x,y,w,h)` | заливка |
| `oled_draw_circle(cx,cy,r)` | окружность |
| `oled_fill_circle(cx,cy,r)` | диск |

Левый верхний угол экрана — `(0,0)`, правый нижний — `(127,63)`.

### Настройки панели

| Функция | Что делает |
|---|---|
| `oled_invert(true/false)` | инверсия изображения |
| `oled_contrast(0..255)` | яркость |
| `oled_display(true/false)` | включить/погасить |

### Кнопка

| Функция | Что делает |
|---|---|
| `button_init(pin)` | `INPUT_PULLUP` |
| `bool button_clicked(pin)` | `true` один раз в момент нажатия |
| `bool button_pressed(pin)` | `true`, пока кнопка удерживается |

## Шрифт и кириллица

`src/oled_font.h` содержит:

* `FONT8X8[96][8]` — ASCII 0x20..0x7E (как в оригинале);
* `FONT8X8_CYR_U[32][8]` — А..Я (U+0410..U+042F);
* `FONT8X8_CYR_L[32][8]` — а..я (U+0430..U+044F);
* `FONT8X8_YO_U` / `FONT8X8_YO_L` — Ё и ё.

Кириллица отрисована из Consolas 10px в сетку 8x8: заглавные 6 px высотой
против 7 px у латиницы, базовая линия общая, поэтому строки в смешанном
тексте не «прыгают». У Й и Ё (й, ё) знаки над буквой дорисованы вручную —
в 10px они не пропечатывались.

**Как добавить свой символ** (например, градус `°` = U+00B0):

1. Нарисуйте 8 байт вручную или сгенерируйте из шрифта.
2. Добавьте массив в `oled_font.h`:

```c
static const uint8_t FONT_DEGREE[8] = {0x00,0x0E,0x11,0x11,0x0E,0x00,0x00,0x00};
```

3. Впишите его в `font_glyph_cp()`:

```c
if (cp == 0x00B0) return FONT_DEGREE;
```

Ограничение шрифта: символ всегда 8 px в ширину (моноширинный), поэтому
в строку 128 px влезает ровно 16 символов.

## Настройка пинов

По умолчанию пины соответствуют Heltec WiFi LoRa 32 V4: SDA 17, SCL 18,
RST 21, Vext 36. Переопределить можно флагами сборки в `platformio.ini`:

```ini
build_flags =
    -DOLED_SDA=17
    -DOLED_SCL=18
    -DOLED_RST=21
    -DOLED_VEXT=36
    -DOLED_ADDR=0x3C
```

## Как это подключено в проекте

* `src/display.cpp` — весь экран погоды на этой библиотеке:
  город, иконка из примитивов, температура x2, описание в две строки
  с переносом по словам, влажность/ветер, индикатор устаревших данных `!`.
* `src/main.cpp` — логика WiFi + wttr.in, экран не трогает, вызывает
  `displayInit()`, `displayMessage()`, `displayWeather()`.
* `platformio.ini` — `lib_deps` пустой, U8g2 больше не нужен.
