#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <oled.h>
#include "display.h"
#if __has_include("secrets.h")
#include "secrets.h"
#else
#define WIFI_SSID ""
#define WIFI_PASSWORD ""
#endif

const char* WIFI_NAME_ON_SCREEN = WIFI_SSID;

static const uint32_t WIFI_TIMEOUT_MS = 20000;

static const int PRG_BTN = 0;

static const long TZ_OFFSET_SEC = 3L * 3600L;

const char* METNO_URL =
    "https://api.met.no/weatherapi/locationforecast/2.0/compact"
    "?lat=54.1961&lon=37.6182";

static const uint32_t REFRESH_MS = 15UL * 60UL * 1000UL;
static const uint32_t RETRY_MS   = 60UL * 1000UL;

static const uint32_t WEEK_SCREEN_TIMEOUT_MS = 10UL * 1000UL;

static const uint32_t ANIM_FRAME_MS = 180;

static const uint16_t HTTP_TIMEOUT_MS = 20000;

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

static const uint16_t RELAY_TIMEOUT_MS = 6000;

static const long RELAY_MAX_AGE_SEC = 6L * 3600L;

struct Weather {
    String temp;
    String cond;
    bool ok = false;
};

enum Screen {
    SCREEN_CLOCK,
    SCREEN_WEEK
};

struct Week {
    struct Day {
        String label;
        int tmax = 0;
        bool ok = false;
    } day[7];
    bool ok = false;
};

static String conditionRu(const String& code) {
    if (code.indexOf("thunder") >= 0)                      return "Гроза";
    if (code.indexOf("sleet") >= 0)                        return "Мокрый снег";
    if (code.indexOf("snow") >= 0)                         return "Снег";
    if (code.indexOf("rain") >= 0)                         return "Дождь";
    if (code.indexOf("fog") >= 0)                          return "Туман";
    if (code.indexOf("partlycloudy") >= 0)                 return "Переменная облачность";
    if (code.indexOf("cloudy") >= 0)                       return "Облачно";
    if (code.indexOf("fair") >= 0)                         return "Прояснение";
    if (code.indexOf("clearsky") >= 0)                     return "Ясно";
    return "Облачно";
}

static int roundTemp(float v) {
    return (int)(v >= 0 ? v + 0.5f : v - 0.5f);
}

static const char* authName(wifi_auth_mode_t m) {
    switch (m) {
        case WIFI_AUTH_OPEN:            return "open";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
#ifdef WIFI_AUTH_WPA3_PSK
        case WIFI_AUTH_WPA3_PSK:        return "WPA3";
#endif
#ifdef WIFI_AUTH_WPA2_WPA3_PSK
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
#endif
        default:                        return "?";
    }
}

static const char* wifiStatusName(wl_status_t s) {
    switch (s) {
        case WL_IDLE_STATUS:      return "idle";
        case WL_NO_SSID_AVAIL:    return "сеть не найдена (2.4 ГГц? точка включена?)";
        case WL_SCAN_COMPLETED:   return "скан завершён";
        case WL_CONNECTED:        return "подключено";
        case WL_CONNECT_FAILED:   return "отказ (неверный пароль?)";
        case WL_CONNECTION_LOST:  return "связь потеряна";
        case WL_DISCONNECTED:     return "не подключено";
        default:                  return "неизвестно";
    }
}

static void scanNetworks() {
    Serial.println("[WiFi] сканирую эфир...");
    int n = WiFi.scanNetworks(false, true);
    Serial.printf("[WiFi] найдено сетей: %d\n", n);

    bool found = false;
    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        bool ours = (ssid == WIFI_SSID);
        if (ours) found = true;

        Serial.printf("  %-24s %4ld dBm  канал %2ld  %s%s\n",
                      ssid.length() ? ssid.c_str() : "(скрытая)",
                      (long)WiFi.RSSI(i), (long)WiFi.channel(i),
                      authName(WiFi.encryptionType(i)),
                      ours ? "   <== наша сеть" : "");
    }

    if (!found) {
        Serial.printf("[WiFi] ВАЖНО: сети \"%s\" в эфире нет.\n", WIFI_SSID);
        Serial.println("[WiFi] Проверьте: точка доступа включена, телефон рядом,");
        Serial.println("[WiFi] и раздаётся 2.4 ГГц (ESP32 не видит 5 ГГц).");
        Serial.println("[WiFi] iPhone: \"Максимальная совместимость\" = ВКЛ.");
        Serial.println("[WiFi] Android: диапазон точки доступа = 2.4 ГГц.");
    }

    WiFi.scanDelete();
}

static bool connectWiFi(bool doScan = false) {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.disconnect();
    delay(100);

    if (doScan) scanNetworks();

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    uint32_t start = millis();
    int lastStatus = -1;
    while (millis() - start < WIFI_TIMEOUT_MS) {
        int st = (int)WiFi.status();
        if (st != lastStatus) {
            lastStatus = st;
            Serial.printf("[WiFi] статус: %s\n", wifiStatusName((wl_status_t)st));
        }
        if (st == WL_CONNECTED) break;
        delay(250);
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WiFi] подключено к \"%s\", IP: %s\n",
                      WIFI_SSID, WiFi.localIP().toString().c_str());
        Serial.printf("[WiFi] шлюз %s, DNS %s\n",
                      WiFi.gatewayIP().toString().c_str(),
                      WiFi.dnsIP(0).toString().c_str());

        IPAddress ip;
        if (WiFi.hostByName("api.met.no", ip))
            Serial.printf("[DNS] api.met.no -> %s\n", ip.toString().c_str());
        else
            Serial.println("[DNS] api.met.no не разрешается");

        configTime(TZ_OFFSET_SEC, 0, "pool.ntp.org",
                   "time.google.com", "time.cloudflare.com");
        return true;
    }

    Serial.printf("[WiFi] не удалось подключиться к \"%s\", статус: %s\n",
                  WIFI_SSID, wifiStatusName(WiFi.status()));
    return false;
}

static long daysFromCivil(int y, int m, int d) {
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (long)era * 146097 + (long)doe - 719468;
}

static bool metnoRequest(String& payload) {
    for (int attempt = 1; attempt <= 2; attempt++) {
        WiFiClientSecure client;
        client.setInsecure();

        HTTPClient http;
        http.setTimeout(HTTP_TIMEOUT_MS);
        http.setUserAgent("heltec-v4-weather/1.0");
        http.begin(client, METNO_URL);

        int code = http.GET();
        if (code == 200) {
            payload = http.getString();
            http.end();
            return true;
        }

        if (code > 0) Serial.printf("[met.no] попытка %d: код %d\n", attempt, code);
        else          Serial.printf("[met.no] попытка %d: %s\n",
                                    attempt, http.errorToString(code).c_str());
        http.end();

        if (attempt < 2) delay(1500);
    }
    return false;
}

struct MetnoData {
    Weather now;
    Week week;
};

static bool relayConfigured() {
    return RELAY_HOST[0] != '\0';
}

static String relayLine(const String& body, const char* key) {
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

static bool relayRequest(String& payload) {
    WiFiClient client;
    HTTPClient http;

    http.setTimeout(RELAY_TIMEOUT_MS);
    http.setUserAgent("heltec-v4-weather/1.0");

    String path = RELAY_PATH;
    if (RELAY_TOKEN[0] != '\0') path += String("?k=") + RELAY_TOKEN;

    if (!http.begin(client, RELAY_HOST, RELAY_PORT, path)) {
        Serial.println("[релей] не удалось начать запрос");
        return false;
    }

    int code = http.GET();
    if (code != 200) {
        if (code > 0) Serial.printf("[релей] код %d\n", code);
        else          Serial.printf("[релей] %s\n", http.errorToString(code).c_str());
        http.end();
        return false;
    }

    payload = http.getString();
    http.end();

    return payload.length() > 0;
}

static void applyServerTime(const String& body) {
    String value = relayLine(body, "ts");
    if (!value.length()) return;

    time_t unixNow = (time_t)strtoul(value.c_str(), nullptr, 10);
    if (unixNow < 1700000000L) return;

    time_t current = time(nullptr);
    bool bad = (current < 1700000000L);
    if (!bad) {
        long diff = (long)current - (long)unixNow;
        if (diff < 0) diff = -diff;
        bad = (diff > 300);
    }

    if (bad) {
        struct timeval tv;
        tv.tv_sec  = unixNow;
        tv.tv_usec = 0;
        settimeofday(&tv, nullptr);
        Serial.printf("[время] выставлено по серверу: %ld\n", (long)unixNow);
    }
}

static bool parseRelay(const String& body, MetnoData& d) {
    int ageSec = atoi(relayLine(body, "age").c_str());
    if (ageSec > RELAY_MAX_AGE_SEC) {
        Serial.printf("[релей] данные устарели: %d с\n", ageSec);
        return false;
    }

    String nowLine = relayLine(body, "now");
    int sp = nowLine.indexOf(' ');
    if (sp < 0) {
        Serial.println("[релей] нет строки now");
        return false;
    }

    int temp = atoi(nowLine.substring(0, sp).c_str());
    String code = nowLine.substring(sp + 1);
    code.trim();

    char tempText[8];
    snprintf(tempText, sizeof(tempText), "%+d", temp);

    d.now.temp = tempText;
    d.now.cond = conditionRu(code);
    d.now.ok   = true;

    String weekLine = relayLine(body, "week");
    int pos = 0;
    int idx = 0;

    while (pos < (int)weekLine.length() && idx < 7) {
        int sp2 = weekLine.indexOf(' ', pos);
        String token = (sp2 < 0) ? weekLine.substring(pos)
                                 : weekLine.substring(pos, sp2);

        int colon = token.indexOf(':');
        if (colon > 0) {
            d.week.day[idx].label = token.substring(0, colon);
            d.week.day[idx].tmax  = atoi(token.substring(colon + 1).c_str());
            d.week.day[idx].ok    = true;
            idx++;
        }

        if (sp2 < 0) break;
        pos = sp2 + 1;
    }

    d.week.ok = (idx > 0);

    Serial.printf("[релей] %s, %s (%s), дней: %d, возраст: %d с\n",
                  d.now.temp.c_str(), d.now.cond.c_str(), code.c_str(), idx, ageSec);

    return true;
}

static MetnoData fetchMetno() {
    MetnoData d;

    if (relayConfigured()) {
        String body;
        if (relayRequest(body)) {
            applyServerTime(body);
            if (parseRelay(body, d)) return d;
        } else {
            Serial.println("[релей] недоступен — иду напрямую в met.no");
        }
    }

    String payload;
    if (!metnoRequest(payload)) return d;

    int tsPos = payload.indexOf("\"timeseries\":[");
    if (tsPos < 0) {
        Serial.println("[met.no] в ответе нет timeseries");
        return d;
    }

    int tp = payload.indexOf("\"time\":\"", tsPos);
    int te = (tp >= 0) ? payload.indexOf('"', tp + 8) : -1;

    int ap = payload.indexOf("\"air_temperature\":", te >= 0 ? te : tsPos);
    if (ap >= 0) {
        int comma = payload.indexOf(',', ap);
        if (comma < 0) comma = payload.length();
        float t = payload.substring(ap + 18, comma).toFloat();

        String code = "";
        int sp = payload.indexOf("\"symbol_code\":\"", te >= 0 ? te : tsPos);
        if (sp >= 0) {
            int ss = sp + 15;
            int se = payload.indexOf('"', ss);
            if (se > ss) code = payload.substring(ss, se);
        }

        char tempText[8];
        snprintf(tempText, sizeof(tempText), "%+d", roundTemp(t));

        d.now.temp = tempText;
        d.now.cond = conditionRu(code);
        d.now.ok   = true;

        Serial.printf("[погода] %s, %s (%s)\n",
                      d.now.temp.c_str(), d.now.cond.c_str(), code.c_str());
    } else {
        Serial.println("[погода] нет air_temperature");
    }

    Week& w = d.week;
    bool baseSet = false;
    long baseDay = 0;
    int pos = tsPos + 14;
    int shiftHours = (int)(TZ_OFFSET_SEC / 3600);

    while (true) {
        int wtp = payload.indexOf("\"time\":\"", pos);
        if (wtp < 0) break;

        int wte = payload.indexOf('"', wtp + 8);
        if (wte < 0) break;

        String stamp = payload.substring(wtp + 8, wte);

        int wap = payload.indexOf("\"air_temperature\":", wte);
        if (wap < 0) break;

        int comma = payload.indexOf(',', wap);
        if (comma < 0) comma = payload.length();
        float t = payload.substring(wap + 18, comma).toFloat();

        int y = 0, m = 0, dayNum = 0, hh = 0;
        if (sscanf(stamp.c_str(), "%d-%d-%dT%d", &y, &m, &dayNum, &hh) == 4) {
            int localHour = hh + shiftHours;
            long day = daysFromCivil(y, m, dayNum);
            if (localHour >= 24)      { localHour -= 24; day += 1; }
            else if (localHour < 0)   { localHour += 24; day -= 1; }

            if (!baseSet) { baseSet = true; baseDay = day; }

            long idx = day - baseDay;
            if (idx >= 7) break;
            if (idx >= 0) {
                int tmax = roundTemp(t);
                if (!w.day[idx].ok) {
                    time_t tt = (time_t)day * 86400;
                    struct tm dt;
                    gmtime_r(&tt, &dt);

                    char label[8];
                    snprintf(label, sizeof(label), "%02d", dt.tm_mday);

                    w.day[idx].label = label;
                    w.day[idx].tmax  = tmax;
                    w.day[idx].ok    = true;
                } else if (tmax > w.day[idx].tmax) {
                    w.day[idx].tmax = tmax;
                }
            }
        }

        pos = wte;
    }

    w.ok = w.day[0].ok;

    for (int i = 0; i < 7; i++) {
        if (!w.day[i].ok) break;
        Serial.printf("[неделя] %s-е: %+d\n", w.day[i].label.c_str(), w.day[i].tmax);
    }

    return d;
}

static SemaphoreHandle_t g_dataMux = nullptr;

static Weather  g_weather;
static Week     g_week;
static bool     g_haveWeather = false;
static bool     g_haveWeek = false;
static bool     g_weatherStale = false;
static bool     g_weekStale = false;
static uint32_t g_dataVersion = 0;

static void netTask(void*) {
    bool firstScan = true;

    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[сеть] подключаюсь к WiFi...");
            if (!connectWiFi(firstScan)) {
                firstScan = false;
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
            firstScan = false;
        }

        MetnoData d = fetchMetno();

        xSemaphoreTake(g_dataMux, portMAX_DELAY);
        if (d.now.ok)  { g_weather = d.now;   g_haveWeather = true; }
        if (d.week.ok) { g_week = d.week;     g_haveWeek = true; }
        g_weatherStale = !d.now.ok;
        g_weekStale    = !d.week.ok;
        g_dataVersion++;
        xSemaphoreGive(g_dataMux);

        vTaskDelay(pdMS_TO_TICKS((d.now.ok || d.week.ok) ? REFRESH_MS : RETRY_MS));
    }
}

void setup() {
    Serial.begin(115200);
    delay(1500);

    if (!displayInit()) {
        Serial.println("[экран] OLED не отвечает по I2C (адрес 0x3C)");
    }
    button_init(PRG_BTN);

    displayMessage("WiFi...", WIFI_NAME_ON_SCREEN);

    g_dataMux = xSemaphoreCreateMutex();

    xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, nullptr, 0);
}

void loop() {
    static Weather lastWeather;
    static Week lastWeek;
    static bool haveData = false;
    static bool haveWeek = false;
    static bool stale = false;
    static bool weekStale = false;
    static uint32_t seenVersion = 0;
    static Screen screen = SCREEN_CLOCK;
    static uint32_t weekShownAt = 0;
    static int lastClockMinute = -3;
    static bool clockDirty = true;
    static uint8_t animFrame = 0;
    static uint32_t lastAnimMs = 0;
    static bool btnDown = false;
    static uint32_t btnChangedMs = 0;
    static bool wifiFailShown = false;
    static bool ntpLogged = false;

    auto drawClock = [&](uint8_t frame) {
        struct tm t;
        const char* cond = (haveData && lastWeather.cond.length())
                           ? lastWeather.cond.c_str() : nullptr;
        const char* temp = (haveData && lastWeather.temp.length())
                           ? lastWeather.temp.c_str() : nullptr;

        if (getLocalTime(&t, 0)) {
            lastClockMinute = t.tm_hour * 60 + t.tm_min;
            displayClock(t.tm_hour, t.tm_min, t.tm_mday,
                         t.tm_mon + 1, t.tm_year + 1900, cond, temp, stale, frame);
        } else {
            lastClockMinute = -2;
            displayClock(-1, 0, 0, 0, 0, cond, temp, stale, frame);
        }
    };

    auto drawWeek = [&]() {
        if (!haveWeek) {
            displayMessage("Неделя", weekStale ? "нет данных" : "загрузка...");
            return;
        }

        WeekDay view[7];
        int n = 0;
        for (int i = 0; i < 7; i++) {
            if (!lastWeek.day[i].ok) continue;
            view[n].label = lastWeek.day[i].label.c_str();
            view[n].tmax  = lastWeek.day[i].tmax;
            n++;
        }
        displayWeek(view, n, weekStale);
    };

    if (g_dataVersion != seenVersion) {
        xSemaphoreTake(g_dataMux, portMAX_DELAY);
        seenVersion = g_dataVersion;
        lastWeather = g_weather;
        lastWeek    = g_week;
        haveData    = g_haveWeather;
        haveWeek    = g_haveWeek;
        stale       = g_weatherStale;
        weekStale   = g_weekStale;
        xSemaphoreGive(g_dataMux);

        clockDirty = true;
        if (screen == SCREEN_WEEK) drawWeek();
    }

    bool raw = button_pressed(PRG_BTN);
    if (raw != btnDown && millis() - btnChangedMs >= 25) {
        btnChangedMs = millis();
        btnDown = raw;

        if (raw) {
            screen = (screen == SCREEN_CLOCK) ? SCREEN_WEEK : SCREEN_CLOCK;

            if (screen == SCREEN_WEEK) {
                weekShownAt = millis();
                Serial.println("[экран] неделя (10 с до возврата)");
                drawWeek();
            } else {
                clockDirty = true;
                Serial.println("[экран] часы");
            }
        }
    }

    if (screen == SCREEN_WEEK &&
        millis() - weekShownAt >= WEEK_SCREEN_TIMEOUT_MS) {
        screen = SCREEN_CLOCK;
        clockDirty = true;
        Serial.println("[экран] 10 с бездействия — вернулись к часам");
    }

    if (screen == SCREEN_CLOCK) {
        struct tm t;
        bool timeOk = getLocalTime(&t, 0);
        int minuteKey = timeOk ? (t.tm_hour * 60 + t.tm_min) : -2;

        if (timeOk && !ntpLogged) {
            ntpLogged = true;
            Serial.printf("[NTP] время получено: %02d:%02d %02d.%02d.%04d\n",
                          t.tm_hour, t.tm_min, t.tm_mday, t.tm_mon + 1,
                          t.tm_year + 1900);
        }

        if (!timeOk && WiFi.status() != WL_CONNECTED) {
            if (!wifiFailShown) {
                displayMessage("WiFi FAIL", WIFI_NAME_ON_SCREEN);
                wifiFailShown = true;
                lastClockMinute = -4;
            }
        } else {
            wifiFailShown = false;

            bool animate = haveData && (millis() - lastAnimMs >= ANIM_FRAME_MS);
            if (clockDirty || minuteKey != lastClockMinute || animate) {
                clockDirty = false;
                if (animate) {
                    lastAnimMs = millis();
                    animFrame++;
                }
                drawClock(animFrame);
            }
        }
    }

    delay(20);
}
