#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int second;
    int minute;
    int hour;
    int day;
    int month;
    int year;
    int dayOfWeek;
} RtcDateTime;

void rtcInit(void);
bool rtcIsAvailable(void);
RtcDateTime rtcGetTime(void);
void rtcSetTime(const RtcDateTime *t);
