#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Погодный релей для ESP32 (Heltec WiFi LoRa 32 V4).

Зачем нужен
-----------
Плата на мобильной точке доступа медленно (20-40 с) и ненадёжно качает JSON
с api.met.no: ~500 КБ, TLS, рукопожатие. Этот сервер качает met.no сам,
раз в 10 минут, держит готовый результат в памяти и отдаёт плате ~150 байт
обычным HTTP без TLS. Плата получает ответ за доли секунды.

Что отдаёт
----------
GET /weather        компактный текст для платы (см. ниже)
GET /weather.json   те же данные в JSON (для отладки и других клиентов)
GET /health         состояние сервера (JSON)

Формат /weather (построчно, '\n'):
    ts 1763900000                       время сервера, unix UTC
    age 42                              сколько секунд назад обновлялись данные
    now +0 partlycloudy_day             текущая температура и код met.no
    week 25:+2 26:+4 27:+8 28:+6 29:+5 30:+1 01:+1
                                        число месяца : максимум дня

Запуск
------
    python3 weather_relay.py

Переменные окружения (все необязательные):
    PORT=8080            порт
    HOST=0.0.0.0         адрес прослушивания
    LAT=54.1961          широта (по умолчанию Тула)
    LON=37.6182          долгота
    TZ_OFFSET=3          часовой пояс города в часах (Москва = +3)
    REFRESH=600          как часто обновлять данные, секунды
    TOKEN=               если задан, запросы должны идти с ?k=TOKEN
                         или заголовком X-Token
    USER_AGENT=...       чем представляться met.no (там этого требуют)

Зависимостей нет — только стандартная библиотека Python 3.8+.
"""

import json
import os
import threading
import time
import urllib.error
import urllib.request
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

# --------------------------------------------------------------------------
#  Настройки
# --------------------------------------------------------------------------
HOST        = os.environ.get("HOST", "0.0.0.0")
PORT        = int(os.environ.get("PORT", "8080"))
LAT         = os.environ.get("LAT", "54.1961")     # Тула
LON         = os.environ.get("LON", "37.6182")
TZ_OFFSET   = int(os.environ.get("TZ_OFFSET", "3"))  # Москва = UTC+3
REFRESH     = int(os.environ.get("REFRESH", "600"))  # 10 минут
TOKEN       = os.environ.get("TOKEN", "").strip()
USER_AGENT  = os.environ.get(
    "USER_AGENT",
    "heltec-weather-relay/1.0 (https://github.com/FarronS/esp32-weather-api)",
)

METNO_URL = os.environ.get("METNO_URL") or (
    "https://api.met.no/weatherapi/locationforecast/2.0/compact"
    f"?lat={LAT}&lon={LON}"
)

# Данные старше этого возраста считаем бесполезными: плата тогда пойдёт
# напрямую в met.no.
MAX_AGE = 6 * 3600

# --------------------------------------------------------------------------
#  Состояние: то, что отдаём плате
# --------------------------------------------------------------------------
_state_lock = threading.Lock()
_state = {
    "text":  "",      # готовый компактный ответ для платы
    "json":  {},      # те же данные для /weather.json
    "ok":    False,   # была ли хоть одна успешная загрузка
    "error": "",      # текст последней ошибки
    "updated": 0.0,   # когда данные реально получены с met.no (unix)
    "fetches": 0,     # сколько раз успешно сходили в met.no
}


def log(msg: str) -> None:
    print(f"{datetime.now():%Y-%m-%d %H:%M:%S} {msg}", flush=True)


def fmt_temp(value: float) -> str:
    """Температура целым числом со знаком: +5, -3, +0."""
    return f"{int(round(value)):+d}"


def log_payload(label: str, text: str) -> None:
    """Показать в логе все строки данных — для проверки глазами."""
    for line in text.rstrip("\n").split("\n"):
        log(f"{label} {line}")


# --------------------------------------------------------------------------
#  Загрузка и разбор ответа met.no
# --------------------------------------------------------------------------
def fetch_upstream() -> dict:
    req = urllib.request.Request(
        METNO_URL,
        headers={"User-Agent": USER_AGENT, "Accept": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=25) as resp:
        return json.loads(resp.read().decode("utf-8"))


def build_payload(raw: dict) -> tuple:
    """Возвращает (now_json, days) — то, что увидит плата."""
    series = raw["properties"]["timeseries"]
    if not series:
        raise ValueError("в ответе met.no пустой timeseries")

    # ---- Текущая погода: первый срез -----------------------------------
    first = series[0]["data"]
    temp_now = first["instant"]["details"]["air_temperature"]
    summary = (
        first.get("next_1_hours")
        or first.get("next_6_hours")
        or first.get("next_12_hours")
        or {}
    )
    code_now = summary.get("summary", {}).get("symbol_code", "")

    # ---- Неделя: максимум температуры на каждый местный день -----------
    tz = timezone(timedelta(hours=TZ_OFFSET))
    order = []          # даты в порядке появления
    tmax = {}           # дата -> максимум

    for entry in series:
        stamp = entry["time"].replace("Z", "+00:00")
        moment = datetime.fromisoformat(stamp).astimezone(tz)
        day = moment.date()

        value = entry["data"]["instant"]["details"].get("air_temperature")
        if value is None:
            continue

        if day not in tmax:
            tmax[day] = value
            order.append(day)
        else:
            tmax[day] = max(tmax[day], value)

    days = [
        {"day": d.day, "tmax": int(round(tmax[d]))}
        for d in order[:7]
    ]

    now_json = {
        "temp": int(round(temp_now)),
        "code": code_now,
    }

    return now_json, days


def store(now_json: dict, days: list) -> None:
    """Собрать компактный текст и положить в общее состояние."""
    updated = time.time()
    text = "\n".join([
        f"ts {int(updated)}",
        "age 0",
        f"now {fmt_temp(now_json['temp'])} {now_json['code']}",
        "week " + " ".join(f"{d['day']:02d}:{fmt_temp(d['tmax'])}" for d in days),
    ]) + "\n"

    with _state_lock:
        _state.update(
            text=text,
            json={"now": now_json, "week": days,
                  "updated": int(updated), "age": 0},
            ok=True,
            error="",
            updated=updated,
            fetches=_state["fetches"] + 1,
        )


def updater() -> None:
    """Фоновый поток: качает met.no и обновляет кэш."""
    backoff = 30
    while True:
        try:
            raw = fetch_upstream()
            now_json, days = build_payload(raw)
            store(now_json, days)

            with _state_lock:
                fetches = _state["fetches"]
                text = _state["text"]

            log(f"[met.no] обновлено: {fmt_temp(now_json['temp'])} "
                f"{now_json['code']}, дней: {len(days)} (всего загрузок: {fetches})")
            log_payload("[данные]", text)   # полный ответ, который получит плата
            backoff = 30
            time.sleep(REFRESH)

        except Exception as exc:                      # noqa: BLE001
            with _state_lock:
                _state["error"] = f"{type(exc).__name__}: {exc}"
            log(f"[met.no] ошибка: {_state['error']} — повтор через {backoff} с")
            time.sleep(backoff)
            backoff = min(backoff * 2, 300)           # 30, 60, 120, 240, 300...


# --------------------------------------------------------------------------
#  HTTP-сервер
# --------------------------------------------------------------------------
def render_text() -> str:
    """Актуальный ответ для платы: с пересчитанным возрастом данных."""
    with _state_lock:
        if not _state["ok"]:
            return ""
        text = _state["text"]
        age = int(time.time() - _state["updated"])

    lines = []
    for line in text.splitlines():
        if line.startswith("age "):
            line = f"age {age}"
        elif line.startswith("ts "):
            line = f"ts {int(time.time())}"
        lines.append(line)
    return "\n".join(lines) + "\n"


def render_json() -> dict:
    with _state_lock:
        data = dict(_state["json"])
        ok = _state["ok"]
        error = _state["error"]
        updated = _state["updated"]

    if ok:
        data["age"] = int(time.time() - updated)
    return {
        "ok": ok,
        "error": error,
        "data": data if ok else None,
    }


class Handler(BaseHTTPRequestHandler):
    server_version = "weather-relay/1.0"

    def log_message(self, fmt, *args):            # тише в логах
        log(f"{self.client_address[0]} {fmt % args}")

    # --- проверка токена (если задан) ---
    def _authorized(self, query) -> bool:
        if not TOKEN:
            return True
        supplied = (query.get("k", [""])[0]
                    or self.headers.get("X-Token", "")
                    or "")
        return supplied == TOKEN

    def _send(self, body: bytes, content_type: str, status: int = 200) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        parsed = urlparse(self.path)
        query = parse_qs(parsed.query)

        if not self._authorized(query):
            self._send(b"forbidden\n", "text/plain; charset=utf-8", 403)
            return

        path = parsed.path.rstrip("/") or "/"

        if path == "/weather":
            text = render_text()
            if not text:
                # Данных ещё нет — плата должна уйти напрямую в met.no.
                self._send(b"", "text/plain; charset=utf-8", 503)
                return
            log_payload(f"[{self.client_address[0]}] ответ плате", text)
            self._send(text.encode("utf-8"), "text/plain; charset=utf-8")
            return

        if path == "/weather.json":
            payload = json.dumps(render_json(), ensure_ascii=False, indent=2)
            self._send(payload.encode("utf-8"), "application/json; charset=utf-8")
            return

        if path == "/health":
            payload = json.dumps(render_json(), ensure_ascii=False, indent=2)
            self._send(payload.encode("utf-8"), "application/json; charset=utf-8")
            return

        if path == "/":
            self._send(
                ("weather-relay\n\n"
                 "  /weather       компактный текст для ESP32\n"
                 "  /weather.json  те же данные в JSON\n"
                 "  /health        состояние\n").encode("utf-8"),
                "text/plain; charset=utf-8",
            )
            return

        self._send(b"not found\n", "text/plain; charset=utf-8", 404)


# --------------------------------------------------------------------------
#  Старт
# --------------------------------------------------------------------------
if __name__ == "__main__":
    log(f"релей запускается: http://{HOST}:{PORT}/weather")
    log(f"город: {LAT}, {LON} (UTC+{TZ_OFFSET}); обновление раз в {REFRESH} с")
    if TOKEN:
        log("доступ по токену: ?k=TOKEN или заголовок X-Token")

    threading.Thread(target=updater, name="updater", daemon=True).start()

    server = ThreadingHTTPServer((HOST, PORT), Handler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log("остановлен")
