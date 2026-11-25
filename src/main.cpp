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

#include <oled.h>          // button_init / button_clicked (кнопка PRG)
#include "display.h"
#include "secrets.h"       // WIFI_SSID / WIFI_PASSWORD (файл не в git)

// ===========================================================================
//  ПРОГРАММА: погода и часы на OLED.
//
//  Что делает плата:
//    1) включает экран и кнопку PRG;
//    2) подключается к WiFi (SSID/пароль в src/secrets.h);
//    3) раз в 15 минут забирает погоду: сначала с релей-сервера (быстро),
//       если он недоступен — напрямую с api.met.no (медленно, но работает);
//    4) рисует всё на экране (весь рисунок — в src/display.cpp);
//    5) стартовый экран — часы с датой, иконкой текущей погоды и температурой;
//       нажатие PRG открывает второй экран — столбики погоды на неделю;
//       через 10 секунд без действий экран сам возвращается к часам.
//
//  Файлы проекта:
//    src/main.cpp      — WiFi + HTTP + время + расписание и переключение экранов;
//    src/display.cpp   — вся отрисовка (часы с погодой, неделя);
//    include/display.h — функции экрана: displayInit/Message/Clock/Week;
//    lib/SmallEPaper/  — драйвер экрана (SSD1306) + шрифт с кириллицей + кнопка.
// ===========================================================================

// ------------------------------------------------ ---------------------------
//  1. WiFi — ПЛАТА ПОДКЛЮЧАЕТСЯ ИМЕННО К ЭТОЙ СЕТИ
// ---------------------------------------------------------------------------
//  Имя и пароль сети лежат в src/secrets.h (этот файл не попадает в git).
//  Шаблон: src/secrets.example.h — скопируйте его в secrets.h и впишите свои.
//  ВАЖНО: ESP32-S3 умеет только 2.4 ГГц. Сеть 5 ГГц плата не увидит.
//  Если не подключается — проверьте: имя/пароль, диапазон 2.4 ГГц,
//  тип шифрования (WPA2/WPA3-personal) и что сеть не скрытая.

// Текущая сеть видна и на экране: при старте плата пишет "WiFi..." + SSID,
// поэтому по дисплею всегда понятно, к чему она подключается.
const char* WIFI_NAME_ON_SCREEN = WIFI_SSID;

// Сколько ждать подключения к WiFi при старте, мс.
static const uint32_t WIFI_TIMEOUT_MS = 20000;

// Кнопка PRG на плате (GPIO0). Нажатие переключает экран: часы <-> неделя.
static const int PRG_BTN = 0;

// Часовой пояс для часов: Тула = UTC+3 (в России летнего времени нет).
static const long TZ_OFFSET_SEC = 3L * 3600L;

// ---------------------------------------------------------------------------
//  2. Погода — откуда берём и как часто
// ---------------------------------------------------------------------------
//  Источник один — api.met.no (Норвежский метеоинститут): бесплатно, без
//  ключа и регистрации. Отдаёт ~9 дней по часам: и текущую температуру,
//  и прогноз на неделю.
//
//  Раньше текущая погода бралась с wttr.in, но с телефонной точки доступа
//  он стабильно обрывался по таймауту, а api.open-meteo.com здесь вообще
//  недоступен. met.no работает и на мобильной сети.
//
//  Координаты города: Тула = 54.1961, 37.6182.
//  Поменять город — вписать свои (например, из Google Maps).
//
//  Формат ответа (важные места):
//    "timeseries":[
//      {"time":"2026-11-17T09:00:00Z",
//       "data":{"instant":{"details":{"air_temperature":-0.6,...}},
//               "next_1_hours":{"summary":{"symbol_code":"partlycloudy_day"}}}},
//      ...
//    ]
//  Время в ответе UTC, поэтому при разборе недели прибавляется TZ_OFFSET_SEC.
const char* METNO_URL =
    "https://api.met.no/weatherapi/locationforecast/2.0/compact"
    "?lat=54.1961&lon=37.6182";

// Расписание: успешное обновление — раз в 15 минут (одним запросом
// обновляются и текущая погода, и неделя), ошибка — повтор через минуту.
static const uint32_t REFRESH_MS = 15UL * 60UL * 1000UL;
static const uint32_t RETRY_MS   = 60UL * 1000UL;

// Сколько держать экран недели без действий, после чего вернуться к часам.
static const uint32_t WEEK_SCREEN_TIMEOUT_MS = 10UL * 1000UL;

// Как часто перерисовывать иконку погоды (анимация: лучи, капли, снежинки).
static const uint32_t ANIM_FRAME_MS = 180;

// Таймаут HTTP-запроса, мс.
static const uint16_t HTTP_TIMEOUT_MS = 20000;

// ---------------------------------------------------------------------------
//  2b. Релей-сервер (необязательно, но сильно ускоряет работу)
// ---------------------------------------------------------------------------
//  Проблема: api.met.no отдаёт ~500 КБ JSON по HTTPS. На мобильной точке
//  доступа это 20-40 секунд, а иногда обрывается по таймауту.
//
//  Решение: маленький сервер (server/weather_relay.py) сам качает met.no,
//  держит готовый результат и отдаёт плате ~150 байт обычным HTTP без TLS.
//  Плата получает ответ за доли секунды.
//
//  Адрес сервера задаётся в src/secrets.h:
//      #define RELAY_HOST "203.0.113.10"
//      #define RELAY_PORT 8080
//      #define RELAY_PATH "/weather"
//      #define RELAY_TOKEN ""        // если на сервере включён TOKEN
//
//  Если RELAY_HOST пустой — релей выключен и плата ходит прямо в met.no.
//  Если сервер не ответил за 6 секунд — тоже идём напрямую в met.no.
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

// Таймаут релея: он рядом и должен отвечать быстро, ждать дольше нет смысла.
static const uint16_t RELAY_TIMEOUT_MS = 6000;

// Данные старше этого возраста считаем негодными (сервер давно не обновлялся).
static const long RELAY_MAX_AGE_SEC = 6L * 3600L;

// ---------------------------------------------------------------------------
//  3. Разобранные данные текущей погоды
// ---------------------------------------------------------------------------
struct Weather {
    String temp;   // "+5" / "-3"
    String cond;   // "Переменная облачность"
    bool ok = false;   // true — данные пригодны для показа
};

// Какой экран показан сейчас. Кнопка PRG переключает между ними.
enum Screen {
    SCREEN_CLOCK,   // часы + дата + текущая погода (стартовый)
    SCREEN_WEEK     // столбики погоды на неделю
};

// ---------------------------------------------------------------------------
//  3b. Разобранный прогноз на неделю (api.met.no)
// ---------------------------------------------------------------------------
struct Week {
    struct Day {
        String label;   // число месяца: "17"
        int tmax = 0;   // максимум за день
        bool ok = false;
    } day[7];
    bool ok = false;    // есть хотя бы первый день
};

// ---------------------------------------------------------------------------
//  4. Разбор описания погоды
// ---------------------------------------------------------------------------
//  met.no отдаёт погоду кодом (symbol_code), например "partlycloudy_day".
//  Переводим в короткий русский текст: его понимает iconForCondition()
//  в display.cpp и он же виден рядом с иконкой в отладочном логе.
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

// Округление температуры до целого градуса.
static int roundTemp(float v) {
    return (int)(v >= 0 ? v + 0.5f : v - 0.5f);
}

// ---------------------------------------------------------------------------
//  5. WiFi
// ---------------------------------------------------------------------------

// Название типа защиты для лога (видно, WPA3 ли у точки доступа).
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

// Название статуса подключения.
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

// Распечатать, какие сети вообще видит плата. Если нужной сети тут нет —
// дело не в пароле: телефон раздаёт 5 ГГц, точка выключена или далеко.
static void scanNetworks() {
    Serial.println("[WiFi] сканирую эфир...");
    int n = WiFi.scanNetworks(false, true);   // синхронно, включая скрытые
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
    WiFi.persistent(false);        // не писать SSID/пароль во flash
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);          // без энергосбережения связь стабильнее
    WiFi.disconnect();
    delay(100);

    if (doScan) scanNetworks();

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    // Ждём подключения и печатаем каждое изменение статуса — по логу сразу
    // видно, на чём застряли (нет сети / неверный пароль / получили IP).
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

        // Диагностика: разрешается ли имя сервера погоды.
        IPAddress ip;
        if (WiFi.hostByName("api.met.no", ip))
            Serial.printf("[DNS] api.met.no -> %s\n", ip.toString().c_str());
        else
            Serial.println("[DNS] api.met.no не разрешается");

        // Запускаем синхронизацию времени (её использует экран часов).
        // Три сервера: если телефон блокирует один — возьмём время с другого.
        configTime(TZ_OFFSET_SEC, 0, "pool.ntp.org",
                   "time.google.com", "time.cloudflare.com");
        return true;
    }

    Serial.printf("[WiFi] не удалось подключиться к \"%s\", статус: %s\n",
                  WIFI_SSID, wifiStatusName(WiFi.status()));
    return false;
}

// ---------------------------------------------------------------------------
//  6. Запрос погоды: сейчас и на неделю (один JSON api.met.no)
// ---------------------------------------------------------------------------
//  Оба экрана питаются одним ответом — меньше запросов и не бывает
//  ситуации «неделя есть, а текущей погоды нет».
//
//  Нужно из ответа:
//    * первый срез timeseries — текущая температура и код погоды;
//    * все срезы — максимум температуры на каждый местный день (неделя).
//
//  Формат среза:
//    {"time":"2026-11-17T09:00:00Z",
//     "data":{"instant":{"details":{"air_temperature":-0.6,...}},
//             "next_1_hours":{"summary":{"symbol_code":"partlycloudy_day"}}}}
//
//  Полноценный JSON-парсер не тащим: идём по тексту ответа.

// Дней с 1970-01-01 до даты (алгоритм Говарда Хиннанта, без циклов и таблиц).
static long daysFromCivil(int y, int m, int d) {
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (long)era * 146097 + (long)doe - 719468;
}

// Один запрос к met.no. На мобильной точке доступа первый запрос после
// подключения часто отваливается по таймауту — повторяем ещё раз.
static bool metnoRequest(String& payload) {
    for (int attempt = 1; attempt <= 2; attempt++) {
        // setInsecure() — не проверять HTTPS-сертификат.
        // Проверка подписи требует верных даты/времени (NTP), а для виджета
        // погоды это лишняя сложность: нас интересует только текст ответа.
        WiFiClientSecure client;
        client.setInsecure();

        HTTPClient http;
        http.setTimeout(HTTP_TIMEOUT_MS);
        http.setUserAgent("heltec-v4-weather/1.0");   // met.no просит представляться
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

// Всё, что нужно обоим экранам.
struct MetnoData {
    Weather now;
    Week week;
};

// ---------------------------------------------------------------------------
//  6a. Релей-сервер: быстрый путь получения погоды
// ---------------------------------------------------------------------------
//  Сервер отдаёт готовый текст (см. server/weather_relay.py):
//
//      ts 1763900000
//      age 42
//      now +0 partlycloudy_day
//      week 25:+2 26:+4 27:+8 28:+6 29:+5 30:+1 01:+1
//      (в конце каждой строки — '\n')
//
//  ts  — время сервера (unix UTC): если часы платы не выставлены, ставим по нему;
//  age — сколько секунд назад сервер обновлял данные с met.no;
//  now — текущая температура и код погоды;
//  week — число месяца : максимум температуры за день.
static bool relayConfigured() {
    return RELAY_HOST[0] != '\0';
}

// Значение строки, начинающейся с key: "age 42" -> "42".
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

// Один GET к релею. Это обычный HTTP (без TLS) — именно поэтому быстро.
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

// Если часы платы ещё не выставлены (или сильно врут) — берём время сервера.
static void applyServerTime(const String& body) {
    String value = relayLine(body, "ts");
    if (!value.length()) return;

    time_t unixNow = (time_t)strtoul(value.c_str(), nullptr, 10);
    if (unixNow < 1700000000L) return;            // явно не время

    time_t current = time(nullptr);
    bool bad = (current < 1700000000L);
    if (!bad) {
        long diff = (long)current - (long)unixNow;
        if (diff < 0) diff = -diff;
        bad = (diff > 300);                        // больше 5 минут расхождения
    }

    if (bad) {
        struct timeval tv;
        tv.tv_sec  = unixNow;
        tv.tv_usec = 0;
        settimeofday(&tv, nullptr);
        Serial.printf("[время] выставлено по серверу: %ld\n", (long)unixNow);
    }
}

// Разобрать ответ релея. true — данные годятся для показа.
static bool parseRelay(const String& body, MetnoData& d) {
    int ageSec = atoi(relayLine(body, "age").c_str());
    if (ageSec > RELAY_MAX_AGE_SEC) {
        Serial.printf("[релей] данные устарели: %d с\n", ageSec);
        return false;
    }

    // ---- Текущая погода: "now +0 partlycloudy_day" ----------------------
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

    // ---- Неделя: "week 25:+2 26:+4 ..." --------------------------------
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

    // ---- Быстрый путь: свой релей --------------------------------------
    if (relayConfigured()) {
        String body;
        if (relayRequest(body)) {
            applyServerTime(body);
            if (parseRelay(body, d)) return d;
        } else {
            Serial.println("[релей] недоступен — иду напрямую в met.no");
        }
    }

    // ---- Медленный путь: прямой запрос к met.no ------------------------
    String payload;
    if (!metnoRequest(payload)) return d;

    int tsPos = payload.indexOf("\"timeseries\":[");
    if (tsPos < 0) {
        Serial.println("[met.no] в ответе нет timeseries");
        return d;
    }

    // ---- Текущая погода: первый срез timeseries -------------------------
    int tp = payload.indexOf("\"time\":\"", tsPos);
    int te = (tp >= 0) ? payload.indexOf('"', tp + 8) : -1;

    int ap = payload.indexOf("\"air_temperature\":", te >= 0 ? te : tsPos);
    if (ap >= 0) {
        int comma = payload.indexOf(',', ap);
        if (comma < 0) comma = payload.length();
        float t = payload.substring(ap + 18, comma).toFloat();

        // Код погоды ближайшего часа (next_1_hours.summary.symbol_code).
        String code = "";
        int sp = payload.indexOf("\"symbol_code\":\"", te >= 0 ? te : tsPos);
        if (sp >= 0) {
            int ss = sp + 15;               // после "symbol_code":"
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

    // ---- Неделя: максимум температуры на каждый местный день ------------
    Week& w = d.week;
    bool baseSet = false;
    long baseDay = 0;              // местный день первого среза
    int pos = tsPos + 14;
    int shiftHours = (int)(TZ_OFFSET_SEC / 3600);

    while (true) {
        int wtp = payload.indexOf("\"time\":\"", pos);
        if (wtp < 0) break;

        int wte = payload.indexOf('"', wtp + 8);
        if (wte < 0) break;

        String stamp = payload.substring(wtp + 8, wte);   // "2026-11-17T09:00:00Z"

        int wap = payload.indexOf("\"air_temperature\":", wte);
        if (wap < 0) break;

        int comma = payload.indexOf(',', wap);
        if (comma < 0) comma = payload.length();
        float t = payload.substring(wap + 18, comma).toFloat();

        int y = 0, m = 0, dayNum = 0, hh = 0;
        if (sscanf(stamp.c_str(), "%d-%d-%dT%d", &y, &m, &dayNum, &hh) == 4) {
            // Время в ответе UTC — переводим в местный день.
            int localHour = hh + shiftHours;
            long day = daysFromCivil(y, m, dayNum);
            if (localHour >= 24)      { localHour -= 24; day += 1; }
            else if (localHour < 0)   { localHour += 24; day -= 1; }

            if (!baseSet) { baseSet = true; baseDay = day; }

            long idx = day - baseDay;
            if (idx >= 7) break;                    // неделя кончилась
            if (idx >= 0) {
                int tmax = roundTemp(t);
                if (!w.day[idx].ok) {
                    // Первая запись дня — заодно берём число месяца для подписи.
                    time_t tt = (time_t)day * 86400;
                    struct tm dt;
                    gmtime_r(&tt, &dt);

                    char label[8];
                    snprintf(label, sizeof(label), "%02d", dt.tm_mday);

                    w.day[idx].label = label;
                    w.day[idx].tmax  = tmax;
                    w.day[idx].ok    = true;
                } else if (tmax > w.day[idx].tmax) {
                    w.day[idx].tmax = tmax;         // максимум за день
                }
            }
        }

        pos = wte;
    }

    w.ok = w.day[0].ok;

    // Итог: что попадёт на экран недели.
    for (int i = 0; i < 7; i++) {
        if (!w.day[i].ok) break;
        Serial.printf("[неделя] %s-е: %+d\n", w.day[i].label.c_str(), w.day[i].tmax);
    }

    return d;
}

// ---------------------------------------------------------------------------
//  6b. Общая память между сетевой задачей и экраном
// ---------------------------------------------------------------------------
//  Сеть работает в отдельной задаче FreeRTOS (ядро 0). Она может блокироваться
//  на HTTP-запросах сколько угодно — экран и кнопка в loop() (ядро 1) при этом
//  продолжают работать, поэтому нажатия PRG больше не теряются.
//
//  Данные передаются через мьютекс: задача пишет, loop() забирает копию.
static SemaphoreHandle_t g_dataMux = nullptr;

static Weather  g_weather;               // последняя удачная погода
static Week     g_week;                  // последняя удачная неделя
static bool     g_haveWeather = false;
static bool     g_haveWeek = false;
static bool     g_weatherStale = false;  // последняя попытка не удалась
static bool     g_weekStale = false;
static uint32_t g_dataVersion = 0;       // ++ при каждом обновлении

static void netTask(void*) {
    bool firstScan = true;

    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[сеть] подключаюсь к WiFi...");
            if (!connectWiFi(firstScan)) {
                firstScan = false;
                vTaskDelay(pdMS_TO_TICKS(5000));   // пауза и новая попытка
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

// ---------------------------------------------------------------------------
//  7. setup / loop
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(1500);   // дать USB-порту подняться, чтобы увидеть первые строки лога

    // Экран. Если I2C-панель не ответила, рисуем в "никуда",
    // но программа продолжает работать.
    if (!displayInit()) {
        Serial.println("[экран] OLED не отвечает по I2C (адрес 0x3C)");
    }
    button_init(PRG_BTN);    // кнопка PRG: часы <-> неделя

    // На экране видно и SSID — сразу понятно, к какой сети идёт подключение.
    displayMessage("WiFi...", WIFI_NAME_ON_SCREEN);

    // Данные между задачей и экраном.
    g_dataMux = xSemaphoreCreateMutex();

    // Сеть — в отдельной задаче на ядре 0. Экран и кнопка остаются
    // в loop() на ядре 1 и не ждут HTTP-запросы.
    xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, nullptr, 0);
}

void loop() {
    static Weather lastWeather;          // последние удачные данные погоды
    static Week lastWeek;                // последний удачный прогноз на неделю
    static bool haveData = false;        // есть ли что показать (погода)
    static bool haveWeek = false;        // есть ли что показать (неделя)
    static bool stale = false;           // погода не обновилась в этот раз
    static bool weekStale = false;       // неделя не обновилась в этот раз
    static uint32_t seenVersion = 0;     // какую версию данных уже забрали
    static Screen screen = SCREEN_CLOCK; // стартовый экран — часы
    static uint32_t weekShownAt = 0;     // когда открыли экран недели
    static int lastClockMinute = -3;     // -3 = часы ещё ни разу не рисовали
    static bool clockDirty = true;       // данные изменились — перерисовать часы
    static uint8_t animFrame = 0;        // кадр анимации иконки
    static uint32_t lastAnimMs = 0;      // когда анимация рисовалась в прошлый раз
    static bool btnDown = false;         // антидребезг кнопки
    static uint32_t btnChangedMs = 0;
    static bool wifiFailShown = false;
    static bool ntpLogged = false;

    // Показать часы + дату + текущую погоду (frame — кадр анимации иконки).
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
            lastClockMinute = -2;            // время ещё не пришло по NTP
            displayClock(-1, 0, 0, 0, 0, cond, temp, stale, frame);
        }
    };

    // Показать столбики погоды на неделю.
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

    // --- Забрать свежие данные из сетевой задачи --------------------------
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

    // --- Кнопка PRG: антидребезг + переключение экранов --------------------
    // Опрос идёт каждые 20 мс и НЕ блокируется сетью (она в другой задаче).
    bool raw = button_pressed(PRG_BTN);
    if (raw != btnDown && millis() - btnChangedMs >= 25) {
        btnChangedMs = millis();
        btnDown = raw;

        if (raw) {                            // именно нажатие, не отпускание
            screen = (screen == SCREEN_CLOCK) ? SCREEN_WEEK : SCREEN_CLOCK;

            if (screen == SCREEN_WEEK) {
                weekShownAt = millis();       // отсюда начнётся отсчёт 10 секунд
                Serial.println("[экран] неделя (10 с до возврата)");
                drawWeek();
            } else {
                clockDirty = true;            // вернулись к часам — перерисовать
                Serial.println("[экран] часы");
            }
        }
    }

    // Экран недели без действий: через 10 секунд снова показываем часы.
    if (screen == SCREEN_WEEK &&
        millis() - weekShownAt >= WEEK_SCREEN_TIMEOUT_MS) {
        screen = SCREEN_CLOCK;
        clockDirty = true;                    // часы нарисуются блоком ниже
        Serial.println("[экран] 10 с бездействия — вернулись к часам");
    }

    // --- Часы: смена минуты, новые данные и анимация иконки ----------------
    if (screen == SCREEN_CLOCK) {
        struct tm t;
        bool timeOk = getLocalTime(&t, 0);
        int minuteKey = timeOk ? (t.tm_hour * 60 + t.tm_min) : -2;

        // Один раз пишем в лог, когда время реально пришло по NTP.
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
