#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "SSD1306Wire.h"

const char* ssid     = "raspi";
const char* password = "87654321";

const String serverPath = "http://openweathermap.org" + cityName + "&appid=" + ap

static const uint8_t OLED_ADDR = 0x3C;

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(
    U8G2_R0, RST_OLED, SCL_OLED, SDA_OLED);

static bool displayOk = false;

static bool i2cPresent(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

static void pulseDisplayReset() {
    pinMode(RST_OLED, OUTPUT);
    digitalWrite(RST_OLED, LOW);
    delay(50);
    digitalWrite(RST_OLED, HIGH);
    delay(50);
}

static bool initDisplay() {
    pinMode(Vext, OUTPUT);
    digitalWrite(Vext, LOW);
    delay(100);

    Wire.begin(SDA_OLED, SCL_OLED);
    Wire.setClock(400000);
    delay(10);

    pulseDisplayReset();

    if (!i2cPresent(OLED_ADDR)) {
        return false;
    }

    u8g2.begin();
    u8g2.setPowerSave(0);
    u8g2.setContrast(255);
    return true;
}

static void drawScreen() {
    if (!displayOk) return;

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_10x20_tr);
    u8g2.drawStr((128 - u8g2.getStrWidth("Hi")) / 2, 39, "Hi");
    u8g2.sendBuffer();
}

void setup() {
    Serial.begin(115200);
    delay(1500);

    displayOk = initDisplay();
    if (displayOk) {
        drawScreen();
    } else {
        Serial.println("OLED: not found");
    }

    pinMode(LED_BUILTIN, OUTPUT);
}

void loop() {
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
    delay(1000);
}
