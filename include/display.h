#pragma once

#include <Arduino.h>

bool displayInit();

void displayMessage(const char* line1, const char* line2 = "");

void displayClock(int hour, int minute, int day, int month, int year,
                  const char* cond, const char* tempC, bool stale,
                  uint8_t frame = 0);

struct WeekDay {
    const char* label;
    int tmax;
};

void displayWeek(const WeekDay* days, int count, bool stale);
