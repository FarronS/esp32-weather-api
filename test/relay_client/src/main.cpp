// ===========================================================================
//  ТЕСТ РЕЛЕЯ НА ESP32-C3 SUPER MINI
//
//  Что делает:
//    1) подключается к WiFi (SSID/пароль из src/secrets.h);
//    2) раз в 30 секунд запрашивает релей-сервер и печатает ответ;
//    3) замеряет время каждого запроса;
//    4) один раз в начале сравнивает релей с прямым запросом в api.met.no
//       (релей ~150 байт по HTTP против ~500 КБ по HTTPS).
//
//  Прошивка:
//      pio run -d test/relay_client -t upload
//      pio device monitor -d test/relay_client -b 115200
//
//  Если RELAY_HOST в src/secrets.h пустой — тест честно об этом скажет
//  и покажет только прямой запрос в met.no.
// ===========================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

#include "secrets.h"

#ifndef RELAY_HOST
#define RELAY_HOST ""
#endif
#ifndef RELAY_PORT
#define RELAY_PORT 8080
#endif
#ifndef RELAY_PATH
#define RELAY_PATH "/weather"
#endif
#ifndef RELAY_TOKEN
#define RELAY_TOKEN ""
#endif

static const char* METNO_URL =
    "https://api.met.no/weatherapi/locationforecast/2.0/compact"
    "?lat=54.1961&lon=37.6182";

static const uint32_t WIFI_TIMEOUT_MS = 20000;
static const uint32_t PERIOD_MS       = 30000;   // как часто дёргать релей
static const uint16_t RELAY_TIMEOUT_MS = 6000;

// ---------------------------------------------------------------------------
//  Мелочи
// ---------------------------------------------------------------------------
static const char* wifiStatusName(wl_status_t s) {
    switch (s) {
        case WL_IDLE_STATUS:     return "idle";
        case WL_NO_SSID_AVAIL:   return "сеть не найдена (2.4 ГГц?)";
        case WL_SCAN_COMPLETED:  return "скан завершён";
        case WL_CONNECTED:       return "подключено";
        case WL_CONNECT_FAILED:  return "отказ (пароль?)";
        case WL_CONNECTION_LOST: return "связь потеряна";
        case WL_DISCONNECTED:    return "не подключено";
        default:                 return "неизвестно";
    }
}

static bool connectWiFi() {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.disconnect();
    delay(100);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    uint32_t start = millis();
    int last = -1;

    while (millis() - start < WIFI_TIMEOUT_MS) {
        int st = (int)WiFi.status();
        if (st != last) {
            last = st;
            Serial.printf("[WiFi] статус: %s\n", wifiStatusName((wl_status_t)st));
        }
        if (st == WL_CONNECTED) break;
        delay(250);
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("[WiFi] не удалось подключиться к \"%s\"\n", WIFI_SSID);
        return false;
    }

    Serial.printf("[WiFi] подключено к \"%s\"\n", WIFI_SSID);
    Serial.printf("[WiFi] IP %s, шлюз %s, RSSI %d dBm, канал %d\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str(),
                  (int)WiFi.RSSI(), (int)WiFi.channel());
    return true;
}

// Значение строки ответа: "age 42" -> "42".
static String lineOf(const String& body, const char* key) {
    String prefix = String(key) + " ";
    int p = body.indexOf(prefix);
    if (p < 0) return "";

    int start = p + prefix.length();
    int end = body.indexOf('\n', start);
    if (end < 0) end = body.length();

    String value = body.substring(start, end);
    value.trim();
    return value;
}

// ---------------------------------------------------------------------------
//  Запрос к релею с замером времени
// ---------------------------------------------------------------------------
static bool requestRelay(String& payload, uint32_t& elapsedMs, int& httpCode) {
    WiFiClient client;
    HTTPClient http;

    http.setTimeout(RELAY_TIMEOUT_MS);
    http.setUserAgent("heltec-v4-weather/1.0");

    String path = RELAY_PATH;
    if (RELAY_TOKEN[0] != '\0') path += String("?k=") + RELAY_TOKEN;

    elapsedMs = 0;
    httpCode = 0;

    if (!http.begin(client, RELAY_HOST, RELAY_PORT, path)) {
        Serial.println("[релей] не удалось начать запрос");
        return false;
    }

    uint32_t t0 = millis();
    httpCode = http.GET();
    elapsedMs = millis() - t0;

    if (httpCode != 200) {
        if (httpCode > 0) Serial.printf("[релей] код %d\n", httpCode);
        else              Serial.printf("[релей] %s\n", http.errorToString(httpCode).c_str());
        http.end();
        return false;
    }

    payload = http.getString();
    http.end();
    return true;
}

// ---------------------------------------------------------------------------
//  Прямой запрос в met.no с замером времени (для сравнения)
// ---------------------------------------------------------------------------
static bool requestMetnoDirect(uint32_t& elapsedMs, int& httpCode, int& bytes) {
    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setTimeout(20000);
    http.setUserAgent("heltec-v4-weather/1.0");

    elapsedMs = 0;
    httpCode = 0;
    bytes = 0;

    if (!http.begin(client, METNO_URL)) return false;

    uint32_t t0 = millis();
    httpCode = http.GET();
    elapsedMs = millis() - t0;

    if (httpCode != 200) {
        if (httpCode > 0) Serial.printf("[met.no] код %d\n", httpCode);
        else              Serial.printf("[met.no] %s\n", http.errorToString(httpCode).c_str());
        http.end();
        return false;
    }

    String payload = http.getString();
    bytes = payload.length();
    http.end();
    return true;
}

// ---------------------------------------------------------------------------
//  Печать одного ответа релея
// ---------------------------------------------------------------------------
static void printRelayPayload(const String& body, uint32_t elapsedMs) {
    Serial.printf("[релей] ответ за %lu мс, %u байт\n",
                  (unsigned long)elapsedMs, (unsigned)body.length());

    for (int i = 0; i < (int)body.length(); i++) {
        char c = body[i];
        Serial.print(c == '\n' ? '\n' : c);
    }
    if (body.length() && body[body.length() - 1] != '\n') Serial.println();

    String now = lineOf(body, "now");       // "+0 partlycloudy_day"
    String week = lineOf(body, "week");
    String age = lineOf(body, "age");

    if (now.length())  Serial.printf("[релей] сейчас: %s\n", now.c_str());
    if (age.length())  Serial.printf("[релей] данным %s с\n", age.c_str());
    if (week.length()) {
        int days = 0;
        for (int i = 0; i < (int)week.length(); i++) if (week[i] == ':') days++;
        Serial.printf("[релей] дней в прогнозе: %d\n", days);
    }
}

// ---------------------------------------------------------------------------
//  Сравнение: релей против прямого met.no
// ---------------------------------------------------------------------------
static void compareOnce() {
    Serial.println();
    Serial.println("=== Сравнение: релей против прямого met.no ===");

    uint32_t relayMs = 0, metnoMs = 0;
    int code = 0, bytes = 0;
    String body;

    bool relayOk = false;
    if (RELAY_HOST[0] != '\0') {
        relayOk = requestRelay(body, relayMs, code);
        if (relayOk)
            Serial.printf("[релей ] %5lu мс, %u байт\n",
                          (unsigned long)relayMs, (unsigned)body.length());
        else
            Serial.println("[релей ] не ответил");
    } else {
        Serial.println("[релей ] RELAY_HOST пустой — адрес сервера не задан");
    }

    bool metnoOk = requestMetnoDirect(metnoMs, code, bytes);
    if (metnoOk)
        Serial.printf("[met.no] %5lu мс, %d байт (напрямую, HTTPS)\n",
                      (unsigned long)metnoMs, bytes);
    else
        Serial.println("[met.no] не ответил (в этой сети TLS может быть заблокирован)");

    if (relayOk && metnoOk && relayMs > 0) {
        Serial.printf("[итог  ] релей быстрее в %.1f раза\n",
                      (double)metnoMs / (double)relayMs);
    }
    Serial.println("=============================================");
    Serial.println();
}

// ---------------------------------------------------------------------------
//  setup / loop
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(2000);                     // USB-CDC на C3 поднимается не сразу

    Serial.println();
    Serial.println("=== Тест релея погоды на ESP32-C3 ===");
    Serial.printf("Релей: %s:%d%s\n",
                  RELAY_HOST[0] ? RELAY_HOST : "(не задан)",
                  RELAY_PORT, RELAY_PATH);

    if (!connectWiFi()) {
        Serial.println("WiFi не поднялся — перезагрузите плату");
        return;
    }

    // Время нужно только для логов; часы C3 не показывают.
    configTime(3 * 3600, 0, "pool.ntp.org", "time.google.com");

    compareOnce();
}

void loop() {
    static uint32_t nextAt = 0;
    static bool first = true;

    if (first) {
        first = false;
        nextAt = millis() + 1000;
    }

    if ((int32_t)(millis() - nextAt) < 0) {
        delay(50);
        return;
    }
    nextAt = millis() + PERIOD_MS;

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] связь потеряна — переподключаюсь");
        if (!connectWiFi()) return;
    }

    if (RELAY_HOST[0] == '\0') {
        Serial.println("[релей] RELAY_HOST пустой: впишите адрес сервера в src/secrets.h");
        return;
    }

    uint32_t elapsedMs = 0;
    int code = 0;
    String body;

    if (requestRelay(body, elapsedMs, code)) {
        printRelayPayload(body, elapsedMs);
    } else {
        Serial.printf("[релей] недоступен (код %d), повтор через 30 с\n", code);
    }

    Serial.printf("[память] свободно %u байт\n\n", (unsigned)ESP.getFreeHeap());
}
