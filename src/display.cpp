#include "display.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <oled.h>

// ===========================================================================
//  ЭКРАНЫ: ЧАСЫ С ПОГОДОЙ И НЕДЕЛЬНЫЙ ГРАФИК
//
//  Железо: OLED SSD1306/SSD1315, 128x64, шина I2C (адрес 0x3C).
//  Драйвер: lib/SmallEPaper (порт oled.c из SmallePlayer/blink-main).
//
//  КАК ЭТО РАБОТАЕТ (важно понять один раз — дальше всё просто)
//  ---------------------------------------------------------------------
//  У экрана нет "цвета фона": каждый пиксель либо горит, либо нет.
//  Рисование идёт НЕ в экран, а в память — во фреймбуфер (1024 байта).
//  Поэтому кадр всегда строится так:
//
//      oled_clear_buffer(); // 1. стереть буфер (oled_clear() ещё и шлёт кадр)
//      ... рисование ...    // 2. нарисовать всё заново
//      oled_update();       // 3. одной передачей отправить буфер на экран
//
//  Если что-то нарисовать без очистки буфера в начале, оно просто
//  добавится поверх старого кадра — на экране останется "мусор".
//
//  КООРДИНАТЫ
//  ---------------------------------------------------------------------
//      (0,0) ────────── X ──────────> (127,0)
//        │
//        Y   левый верхний угол = (0,0)
//        │   правый нижний      = (127,63)
//        v
//      (0,63)
//
//  ШРИФТ
//  ---------------------------------------------------------------------
//  Моноширинный 8x8: символ занимает 8x8 px, шаг по X = 8 px.
//  Кириллица и латиница есть; по высоте кириллица 6 px, латиница 7 px,
//  низ строки общий — смешанный текст не "прыгает".
//  В строку 128 px влезает 16 символов.
//  Координата y — ВЕРХ символа (а не базовая линия, как в U8g2).
//  Шаг между строками текста — 8 px.
//  Крупный текст: oled_draw_utf8_scaled(x, y, s, 2) — символ 16x16.
//
//  ЭКРАНЫ
//  ---------------------------------------------------------------------
//    displayClock() — стартовый: время x3, дата, иконка погоды, температура x2;
//    displayWeek()  — по кнопке: 7 вертикальных столбиков на неделю.
//  Разметка каждого экрана описана прямо над его функцией ниже.
//
//  ЧТО МЕНЯТЬ
//  ---------------------------------------------------------------------
//    * координаты — прямо в displayClock() / displayWeek();
//    * иконки — drawIcon(); какая иконка для какого текста погоды —
//      iconForCondition();
//    * крупный/мелкий шрифт — последний аргумент oled_draw_utf8_scaled().
// ===========================================================================

static bool displayOk = false;   // false — экран не ответил по I2C

// ===========================================================================
//  ТЕКСТ
// ===========================================================================

// Вывести строку по центру экрана (по горизонтали).
static void textCentered(int y, const char *s) {
    if (!s) return;
    int x = (128 - oled_utf8_width(s)) / 2;
    if (x < 0) x = 0;
    oled_draw_utf8(x, y, s);
}

// ===========================================================================
//  ИКОНКИ ПОГОДЫ
//
//  Рисуются не картинками, а примитивами драйвера:
//  круги (oled_fill_circle) и линии (oled_draw_line).
//  Так иконки занимают считанные байты flash и легко правятся.
// ===========================================================================
enum IconType {
    ICON_CLEAR,     // ясно, солнце
    ICON_PARTLY,    // переменная облачность
    ICON_CLOUDY,    // облачно, пасмурно
    ICON_RAIN,      // дождь
    ICON_SNOW,      // снег
    ICON_THUNDER,   // гроза
    ICON_FOG        // туман
};

// Привести русский текст к нижнему регистру, чтобы искать по корням слов
// ("Дождь", "дождь", "ДОЖДЬ" — всё должно находиться).
// UTF-8: "А".."Я" = 0xD0 0x90..0xAF, "а".."я" = 0xD0 0xB0..0xBF + 0xD1 0x80..0x8F.
static String toLowerRu(const String &src) {
    String r = src;
    for (size_t i = 0; i < r.length(); i++) {
        uint8_t c = (uint8_t)r[i];

        if (c >= 'A' && c <= 'Z') {
            r[i] = (char)(c + 32);
        } else if (c == 0xD0 && i + 1 < r.length()) {
            uint8_t n = (uint8_t)r[i + 1];
            if (n >= 0x90 && n <= 0xAF) {          // А-Я -> а-я
                r[i + 1] = (char)(n + 0x20);
            } else if (n == 0x81) {                // Ё -> ё
                r[i] = (char)0xD1;
                r[i + 1] = (char)0x91;
            }
            i++;
        }
    }
    return r;
}

// Какую иконку рисовать для текста описания.
// Порядок проверок важен: "переменная облачность" должна попасть
// в ICON_PARTLY раньше, чем сработает проверка на "облач".
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

    return ICON_CLOUDY;   // неизвестный текст — нейтральное облако
}

// Солнце: диск + 8 лучей. Диск чуть «дышит», лучи медленно вращаются,
// а по кругу бежит волна длины луча — солнце выглядит живым.
static void drawSun(int cx, int cy, int r, uint8_t frame) {
    int rr = ((frame >> 3) & 1) ? r : r - 1;    // радиус «дышит» 5 <-> 6
    oled_fill_circle(cx, cy, rr);

    float base = frame * ((float)PI / 16.0f);   // полный оборот за 32 кадра

    for (int i = 0; i < 8; i++) {
        float a = base + i * (float)PI / 4.0f;

        // Длина луча чередуется (2 коротких, 2 длинных) и «волна» бежит
        // по кругу: солнце мерцает, но не дёргается.
        int len = (((frame >> 1) + i) & 3) < 2 ? 3 : 5;

        int r0 = rr + 2;                        // начало луча — от края диска
        oled_draw_line(cx + (int)(cos(a) * r0), cy + (int)(sin(a) * r0),
                       cx + (int)(cos(a) * (r0 + len)),
                       cy + (int)(sin(a) * (r0 + len)));
    }

    // Один блик обходит солнце по кругу — как отражение на глянце.
    float ga = -base * 1.6f;
    int gx = cx + (int)(cos(ga) * (rr + 6));
    int gy = cy + (int)(sin(ga) * (rr + 6));
    oled_set_pixel(gx, gy, true);
    oled_set_pixel(gx + 1, gy, true);
}

// Облако: три диска разного размера + прямоугольник-основание.
// Плавно покачивается вверх-вниз на пиксель.
static void drawCloud(int cx, int cy, uint8_t frame) {
    int bob = (int)lroundf(sinf(frame * 0.35f) * 1.9f);   // -2..+2

    oled_fill_circle(cx - 8, cy + 3 + bob, 5);
    oled_fill_circle(cx,      cy - 1 + bob, 7);
    oled_fill_circle(cx + 8,  cy + 3 + bob, 5);
    oled_fill_rect(cx - 13, cy + 3 + bob, 26, 5);
}

// Снежинка 5x5: три линии, проходящие через центр (6 лучей-«звёздочка»).
// На такой маленькой иконке это читается лучше, чем точка.
static void drawSnowflake(int x, int y) {
    oled_draw_line(x,     y - 2, x,     y + 2);   // вертикаль
    oled_draw_line(x - 2, y - 2, x + 2, y + 2);   // диагональ "\"
    oled_draw_line(x + 2, y - 2, x - 2, y + 2);   // диагональ /
}

// Отрисовать иконку с центром (cx, cy). frame — кадр анимации.
static void drawIcon(IconType type, int cx, int cy, uint8_t frame) {
    switch (type) {
        case ICON_CLEAR:
            drawSun(cx, cy, 6, frame);
            break;

        case ICON_PARTLY:
            drawSun(cx + 6, cy - 6, 4, frame);  // солнце выглядывает сверху справа
            drawCloud(cx - 3, cy + 2, frame);
            break;

        case ICON_CLOUDY:
            drawCloud(cx, cy, frame);
            break;

        case ICON_RAIN: {
            drawCloud(cx, cy - 5, frame);

            // Пять капель: у каждой своя скорость и фаза, поэтому дождь
            // не выглядит строем палочек. Внизу капля «всплёскивается».
            for (int i = 0; i < 5; i++) {
                int speed = 1 + (i & 1);              // 1 или 2 пикселя за кадр
                int phase = (frame * speed + i * 3) % 9;
                int x = cx - 10 + i * 5;
                int y = cy + 1 + phase;

                oled_draw_line(x, y, x - 1, y + 3);   // сама капля (косая)

                if (phase >= 7) {                     // всплеск у нижней точки
                    oled_set_pixel(x - 2, y + 4, true);
                    oled_set_pixel(x + 1, y + 4, true);
                }
            }
            break;
        }

        case ICON_SNOW:
            drawCloud(cx, cy - 5, frame);
            // Три снежинки падают с разной скоростью и «виляют» в стороны.
            for (int i = 0; i < 3; i++) {
                int speed = 1 + (i & 1);
                int y = cy + 2 + (int)((frame * speed + i * 4) % 9);
                int wobble = ((frame / 2 + i) & 1) ? 1 : -1;
                drawSnowflake(cx - 8 + i * 8 + wobble, y);
            }
            break;

        case ICON_THUNDER:
            drawCloud(cx, cy - 6, frame);
            // Молния вспыхивает дважды и гаснет: 2 кадра есть, 1 нет,
            // 2 кадра есть, 5 нет — получается «зигзаг в грозе».
            if ((frame % 10) < 2 || ((frame % 10) >= 3 && (frame % 10) < 5)) {
                oled_draw_line(cx,      cy + 2,  cx - 4, cy + 8);
                oled_draw_line(cx - 4,  cy + 8,  cx,     cy + 8);
                oled_draw_line(cx,      cy + 8,  cx - 2, cy + 13);
                oled_draw_line(cx - 2,  cy + 13, cx + 6, cy + 6);
                oled_draw_line(cx + 6,  cy + 6,  cx + 2, cy + 6);

                // Свечение вокруг молнии — только на пике вспышки.
                if ((frame % 10) < 1) {
                    oled_set_pixel(cx - 6, cy + 9,  true);
                    oled_set_pixel(cx + 7, cy + 5,  true);
                    oled_set_pixel(cx + 1, cy + 15, true);
                }
            }
            break;

        case ICON_FOG:
            drawCloud(cx, cy - 6, frame);
            // Полосы тумана «плывут» вправо-влево с разной длиной.
            for (int i = 0; i < 3; i++) {
                int shift = (int)((frame / 2 + i * 2) % 6) - 2;
                int left  = cx - 12 + shift + (i & 1);
                int right = cx + 10 + shift - (i & 1) * 2;
                oled_draw_line(left, cy + 5 + i * 4, right, cy + 5 + i * 4);
            }
            break;
    }
}

// ===========================================================================
//  ПУБЛИЧНЫЕ ФУНКЦИИ ЭКРАНА (см. include/display.h)
// ===========================================================================

// Включить экран. Вызывать один раз в setup().
bool displayInit() {
    displayOk = oled_init();     // питание Vext + Wire + команды SSD1306
    return displayOk;
}

// Две строки по центру: статусы "WiFi...", "WiFi FAIL", "No weather data".
void displayMessage(const char *line1, const char *line2) {
    if (!displayOk) return;

    oled_clear_buffer();         // новый кадр с чистого листа (без мигания)
    textCentered(20, line1);
    textCentered(36, line2);
    oled_update();               // отправить кадр на экран
}

// ===========================================================================
//  ЧАСЫ + ПОГОДА СЕЙЧАС (стартовый экран)
//
//  Разметка (128x64):
//      (0,0)     12:34          время крупно, scale 3 (символ 24x24)
//      (0,25)   17.11.26        дата по центру
//      (24,49)  ☀ / ☁ / ...     иконка текущей погоды (центр, анимируется)
//      (48..124)      +5°        температура x2 справа
// ===========================================================================
void displayClock(int hour, int minute, int day, int month, int year,
                  const char* cond, const char* tempC, bool stale,
                  uint8_t frame) {
    if (!displayOk) return;

    oled_clear_buffer();

    // Время крупно: scale 3 -> символ 24x24, "ЧЧ:ММ" = 120 px по ширине.
    char timeText[8];
    if (hour < 0) {
        snprintf(timeText, sizeof(timeText), "--:--");
    } else {
        snprintf(timeText, sizeof(timeText), "%02d:%02d", hour % 24, minute % 60);
    }

    int timeWidth = oled_utf8_len(timeText) * 24;
    oled_draw_utf8_scaled((128 - timeWidth) / 2, 0, timeText, 3);

    // Под временем: дата, а пока NTP не ответил — надпись о синхронизации.
    if (hour < 0) {
        textCentered(25, "синхронизация...");
    } else {
        char dateText[16];
        snprintf(dateText, sizeof(dateText), "%02d.%02d.%02d",
                 day % 100, month % 100, year % 100);
        textCentered(25, dateText);
    }

    // Текущая погода: иконка слева, температура x2 справа.
    if (cond && *cond) {
        drawIcon(iconForCondition(String(cond)), 24, 49, frame);
    }

    if (tempC && *tempC) {
        int tempWidth = oled_utf8_len(tempC) * 16;
        int tempX = 48 + (76 - tempWidth) / 2;   // зона x=48..124
        if (tempX < 48) tempX = 48;
        oled_draw_utf8_scaled(tempX, 42, tempC, 2);

        // Кружок градуса после цифр (символа "°" в шрифте нет).
        int degreeX = tempX + tempWidth + 2;
        if (degreeX > 124) degreeX = 124;
        oled_draw_circle(degreeX, 45, 2);
    }

    // Погода не обновилась — "!" справа от даты.
    if (stale) oled_draw_char(120, 25, '!');

    oled_update();
}

// ===========================================================================
//  ПОГОДА НА НЕДЕЛЮ: 7 СТОЛБИКОВ
//
//  Разметка (128x64):
//
//       +2        +5        +7       ...   максимальная температура дня
//      ████      █████     ██████         высота столбика — та же температура
//      ████      █████     ██████         (масштаб общий на всю неделю)
//      ...
//      17        18        19        ...   число месяца
//
//  Шаг столбцов 17 px. Подписи температур у соседних столбцов стоят на
//  разной высоте (y=0 и y=9): двухзначная отрицательная "-10" шире
//  столбца, и так подписи не наезжают друг на друга.
//
//  Высота столбика — относительная (от самого холодного дня к самому
//  тёплому), а точные значения написаны цифрами над столбиками.
// ===========================================================================
void displayWeek(const WeekDay* days, int count, bool stale) {
    if (!displayOk) return;

    oled_clear_buffer();

    if (count <= 0) {
        textCentered(28, "Нет данных");
        oled_update();
        return;
    }
    if (count > 7) count = 7;

    // Диапазон максимумов за неделю — чтобы столбики заметно отличались.
    int minT = days[0].tmax, maxT = days[0].tmax;
    for (int i = 1; i < count; i++) {
        if (days[i].tmax < minT) minT = days[i].tmax;
        if (days[i].tmax > maxT) maxT = days[i].tmax;
    }

    const int baseline = 52;   // низ столбиков
    const int maxH = 34;       // самый высокий столбик начинается с y=18
    const int minH = 4;

    for (int i = 0; i < count; i++) {
        int cx = 12 + i * 17;  // центр столбика

        int h;
        if (maxT == minT) h = (minH + maxH) / 2;
        else              h = minH + (days[i].tmax - minT) * (maxH - minH) / (maxT - minT);

        // Столбик.
        oled_fill_rect(cx - 4, baseline - h, 9, h);

        // Максимум дня над столбиком (у соседних столбцов — на разной высоте).
        char tempText[8];
        snprintf(tempText, sizeof(tempText), "%+d", days[i].tmax);
        int tw = oled_utf8_width(tempText);
        int tx = cx - tw / 2;
        if (tx < 0) tx = 0;
        if (tx + tw > 128) tx = 128 - tw;
        oled_draw_utf8(tx, (i & 1) ? 9 : 0, tempText);

        // Число месяца под столбиком.
        if (days[i].label) {
            int lw = oled_utf8_width(days[i].label);
            int lx = cx - lw / 2;
            if (lx < 0) lx = 0;
            if (lx + lw > 128) lx = 128 - lw;
            oled_draw_utf8(lx, baseline + 2, days[i].label);
        }
    }

    // Линия-основание под столбиками.
    oled_draw_line(0, baseline + 1, 127, baseline + 1);

    // Прогноз не обновился — "!" справа, рядом с последним столбиком.
    if (stale) oled_draw_char(120, 44, '!');

    oled_update();
}
