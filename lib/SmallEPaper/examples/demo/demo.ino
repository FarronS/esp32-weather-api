
#include <Arduino.h>
#include <oled.h>

#define BUTTON_PIN 0

bool ready = false;
uint32_t last = 0;
int counter = 0;
bool clickedFlag = false;

void setup() {
    Serial.begin(115200);

    ready = oled_init();
    button_init(BUTTON_PIN);

    if (!ready) {
        Serial.println("OLED not found");
        return;
    }

    oled_fill(0xFF);
    delay(500);
    oled_clear();

    oled_draw_utf8(0, 0, "Привет, мир!");
    oled_draw_line(0, 10, 127, 10);
    oled_draw_utf8_scaled(2, 14, "+5", 2);
    oled_draw_circle(58, 17, 2);
    oled_draw_utf8(70, 18, "ясно");
    oled_draw_utf8(0, 34, "Кириллица: А-Я а-я Ёё");
    oled_draw_utf8(0, 44, "Влаж 45%  3 км/ч");

    oled_draw_rect(0, 52, 128, 12);
    oled_draw_utf8(6, 54, "SmallEPaper");
    oled_update();
    delay(2500);
}

void loop() {
    if (!ready) {
        delay(1000);
        return;
    }

    if (button_clicked(BUTTON_PIN)) {
        clickedFlag = true;
        counter++;
    }

    if (millis() - last >= 1000) {
        last = millis();

        oled_clear();
        oled_draw_utf8(0, 0, "SmallEPaper");
        oled_draw_line(0, 10, 127, 10);
        oled_printf(0, 16, "uptime: %lu s", (unsigned long)(millis() / 1000));
        oled_printf(0, 26, "clicks: %d", counter);
        oled_draw_utf8(0, 36, button_pressed(BUTTON_PIN) ? "кнопка: нажата"
                                                        : "кнопка: отпущена");

        if (clickedFlag) {
            oled_draw_rect(0, 48, 128, 15);
            oled_draw_utf8(6, 52, "НАЖАТИЕ!");
            clickedFlag = false;
        }

        oled_update();
    }
}
