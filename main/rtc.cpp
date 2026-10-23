#include "rtc.h"
#include "config.h"

#include <stdio.h>
#include <string.h>
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "RTC_DS3231";
static bool available = false;

static uint8_t bcdToDec(uint8_t v) { return (uint8_t)(((v >> 4) * 10) + (v & 0x0F)); }
static uint8_t decToBcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static esp_err_t readRegs(uint8_t reg, uint8_t *data, size_t len) {
    return i2c_master_write_read_device(RTC_I2C_PORT, RTC_I2C_ADDRESS, &reg, 1, data, len, pdMS_TO_TICKS(100));
}

static esp_err_t writeReg(uint8_t reg, uint8_t value) {
    uint8_t tx[2] = {reg, value};
    return i2c_master_write_to_device(RTC_I2C_PORT, RTC_I2C_ADDRESS, tx, sizeof(tx), pdMS_TO_TICKS(100));
}

// Build a timestamp from the firmware's compile time (__DATE__ / __TIME__),
// e.g. "Aug 27 2026" + "11:13:07". Used as a sane default when the RTC has
// lost its time and no better clock source is available.
static RtcDateTime buildTime(void) {
    RtcDateTime t = {};
    char mon[4] = {0};
    int day = 1, year = 2000, hour = 0, minute = 0, second = 0;
    sscanf(__DATE__, "%3s %d %d", mon, &day, &year);
    sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second);
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *p = strstr(months, mon);
    int month = p ? (int)((p - months) / 3) + 1 : 1;
    t.year = year; t.month = month; t.day = day;
    t.hour = hour; t.minute = minute; t.second = second;
    // Sakamoto's algorithm: dow 0=Sunday .. 6=Saturday.
    static const int mtab[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int y = year - (month < 3);
    int dow = (y + y/4 - y/100 + y/400 + mtab[month-1] + day) % 7;
    t.dayOfWeek = dow + 1; // DS3231 day-of-week register is 1..7
    return t;
}

void rtcSetTime(const RtcDateTime *t) {
    if (!t || !available) return;
    int year = t->year - 2000;
    uint8_t century = 0;
    if (year >= 100) { year -= 100; century = 0x80; } // matches rtcGetTime's century bit

    uint8_t tx[8];
    tx[0] = 0x00; // write starting at the seconds register
    tx[1] = decToBcd((uint8_t)(t->second % 60));
    tx[2] = decToBcd((uint8_t)(t->minute % 60));
    tx[3] = decToBcd((uint8_t)(t->hour % 24));                                       // 24-hour mode (bit 6 = 0)
    tx[4] = decToBcd((uint8_t)((t->dayOfWeek >= 1 && t->dayOfWeek <= 7) ? t->dayOfWeek : 1));
    tx[5] = decToBcd((uint8_t)t->day);
    tx[6] = (uint8_t)(decToBcd((uint8_t)t->month) | century);
    tx[7] = decToBcd((uint8_t)(year % 100));

    if (i2c_master_write_to_device(RTC_I2C_PORT, RTC_I2C_ADDRESS, tx, sizeof(tx), pdMS_TO_TICKS(100)) != ESP_OK) {
        ESP_LOGE(TAG, "RTC set failed");
        return;
    }
    // Time is valid again; clear the oscillator-stop flag if it is still set.
    uint8_t status = 0;
    if (readRegs(0x0F, &status, 1) == ESP_OK && (status & 0x80)) {
        writeReg(0x0F, (uint8_t)(status & ~0x80));
    }
}

void rtcInit(void) {
    i2c_config_t cfg = {};
    cfg.mode = I2C_MODE_MASTER;
    cfg.sda_io_num = (gpio_num_t)RTC_SDA;
    cfg.scl_io_num = (gpio_num_t)RTC_SCL;
    cfg.sda_pullup_en = GPIO_PULLUP_ENABLE;
    cfg.scl_pullup_en = GPIO_PULLUP_ENABLE;
    cfg.master.clk_speed = RTC_I2C_FREQ_HZ;

    esp_err_t err = i2c_param_config(RTC_I2C_PORT, &cfg);
    if (err != ESP_OK) { ESP_LOGE(TAG, "I2C config failed: %s", esp_err_to_name(err)); return; }
    err = i2c_driver_install(RTC_I2C_PORT, cfg.mode, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { ESP_LOGE(TAG, "I2C install failed: %s", esp_err_to_name(err)); return; }

    uint8_t status = 0;
    err = readRegs(0x0F, &status, 1);
    if (err != ESP_OK) { ESP_LOGE(TAG, "DS3231 not found at 0x68: %s", esp_err_to_name(err)); available = false; return; }

    // Clear oscillator-stop flag after detecting it. The time is not fabricated here.
    if (status & 0x80) {
        ESP_LOGW(TAG, "DS3231 oscillator-stop flag set; RTC time may need to be set once");
        status &= (uint8_t)~0x80;
        writeReg(0x0F, status);
    }
    available = true;

    // Auto-set the clock to the firmware build time when the RTC has no valid
    // date (fresh board, or a dead coin cell whose oscillator-stop flag was
    // already cleared on a prior boot). With a good battery the RTC keeps real
    // time across power cycles, so a plausible year is left untouched.
    RtcDateTime cur = rtcGetTime();
    if (cur.year < 2024) {
        RtcDateTime bt = buildTime();
        rtcSetTime(&bt);
        ESP_LOGW(TAG, "RTC year %d invalid; set to build time %04d-%02d-%02d %02d:%02d:%02d",
                 cur.year, bt.year, bt.month, bt.day, bt.hour, bt.minute, bt.second);
    }
    ESP_LOGI(TAG, "DS3231 initialized");
}

bool rtcIsAvailable(void) { return available; }

RtcDateTime rtcGetTime(void) {
    RtcDateTime t = {};
    if (!available) return t;

    uint8_t r[7] = {};
    if (readRegs(0x00, r, sizeof(r)) != ESP_OK) {
        available = false;
        return t;
    }

    t.second = bcdToDec(r[0] & 0x7F);
    t.minute = bcdToDec(r[1] & 0x7F);
    if (r[2] & 0x40) {
        int h = bcdToDec(r[2] & 0x1F);
        t.hour = (h % 12) + ((r[2] & 0x20) ? 12 : 0);
    } else {
        t.hour = bcdToDec(r[2] & 0x3F);
    }
    t.dayOfWeek = bcdToDec(r[3] & 0x07);
    t.day = bcdToDec(r[4] & 0x3F);
    t.month = bcdToDec(r[5] & 0x1F);
    int century = (r[5] & 0x80) ? 1 : 0;
    t.year = 2000 + bcdToDec(r[6]) + century * 100;
    return t;
}
