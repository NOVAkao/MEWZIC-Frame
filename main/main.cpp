#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"
#include "rtc.h"
#include "display.h"
#include "bluetooth.h"
#include "encoder.h"
#include "button_handler.h"

extern "C" void app_main(void) {
    printf("\n==============================\n");
    printf(" ESP32 Spotify Controller\n");
    printf(" Bluetooth Classic AVRCP\n");
    printf(" DS3231 + 2.9in E-PAPER\n");
    printf("==============================\n\n");

    // Enable informative component logs to trace display, RTC, and BT status on serial monitor.
    esp_log_level_set("*", ESP_LOG_INFO);

    printf("[BOOT] Initializing RTC...\n");
    rtcInit();
    printf("[BOOT] RTC available: %s\n", rtcIsAvailable() ? "yes" : "no");
    printf("[BOOT] Initializing e-paper...\n");
    displayInit();
    printf("[BOOT] E-paper initialization returned\n");

    // Draw the clock before starting BTstack. The Bluetooth task runs at a
    // higher priority and may not yield promptly during controller startup.
    if (rtcIsAvailable()) {
        RtcDateTime now = rtcGetTime();
        printf("[BOOT] First clock draw: %04d-%02d-%02d %02d:%02d:%02d\n",
               now.year, now.month, now.day, now.hour, now.minute, now.second);
        displayClock(now.hour, now.minute, now.day, now.month, now.year, now.dayOfWeek);
    } else {
        printf("[BOOT] Clock draw skipped: RTC unavailable\n");
    }

#if WOKWI_SIMULATION
    // Wokwi doesn't emulate the BT radio; bringing up the controller hangs and
    // starves this loop. Skip it so the clock renders. Test Bluetooth on hardware.
    printf("WOKWI_SIMULATION: Bluetooth disabled (radio not emulated); clock only\n");
#else
    bluetoothInit();
#endif
    encoderInit();
    buttonHandlerInit();

    // The e-paper is drawn by this task only (the BTstack task just updates state),
    // so there is no contention over the framebuffer / SPI bus.
    bt_screen_t shown = BT_SCREEN_CLOCK;          // Initialized to match boot clock draw
    int lastDrawnMinute = -1;
    int lastDrawnDay = -1;
    if (rtcIsAvailable()) {
        RtcDateTime now = rtcGetTime();
        lastDrawnMinute = now.minute;
        lastDrawnDay = now.day;
    }
    TickType_t lastRtcCheck = 0;
    char shownTitle[128] = "", shownArtist[128] = "";
    bool shownPlaying = false;
    bool shownHasArt = false;
    const TickType_t rtcCheckPeriod = pdMS_TO_TICKS(DISPLAY_REFRESH_INTERVAL_MS);
    // Local cover art buffer for the UI to read without contention.
    // Copied from bluetooth module under the cover mutex.
    static uint8_t artBuffer[(DISPLAY_COVER_ART_WIDTH * DISPLAY_COVER_ART_HEIGHT + 7) / 8];

    while (true) {
        bt_screen_t want = bluetoothScreen();
        TickType_t nowTick = xTaskGetTickCount();

        if (want == BT_SCREEN_CLOCK) {
            // Disconnected: check RTC every 15s, but only refresh e-paper when minute changes or switching screens
            if (shown != want || (nowTick - lastRtcCheck) >= rtcCheckPeriod) {
                if (rtcIsAvailable()) {
                    RtcDateTime now = rtcGetTime();
#if DISPLAY_DEBUG_LOGGING
                    printf("RTC Time: %02d:%02d:%02d %02d/%02d/%04d\n",
                           now.hour, now.minute, now.second, now.day, now.month, now.year);
#endif
                    if (shown != want || now.minute != lastDrawnMinute || now.day != lastDrawnDay) {
                        displayClock(now.hour, now.minute, now.day, now.month, now.year, now.dayOfWeek);
                        lastDrawnMinute = now.minute;
                        lastDrawnDay = now.day;
                    }
                } else {
                    printf("RTC unavailable\n");
                }
                lastRtcCheck = nowTick;
            }
        } else if (want == BT_SCREEN_LOGO) {
            // Connected but idle: show the cassette artwork once.
            if (shown != want) {
                printf("Bluetooth connected: showing cassette\n");
                displayCassette();
            }
        } else { // BT_SCREEN_MEDIA
            // Playing (or paused after having played): show track metadata with cover art
            char title[128], artist[128];
            bool playing;
            bluetoothGetMedia(title, sizeof(title), artist, sizeof(artist), &playing);

            // Snapshot cover art (if available)
            bool hasArt = bluetoothHasCoverArt();
            unsigned artW = 0, artH = 0;
            if (hasArt) {
                artW = bluetoothGetCoverArtWidth();
                artH = bluetoothGetCoverArtHeight();
                bluetoothGetCoverArtData(artBuffer, sizeof(artBuffer));
            } else {
                // Log why art is missing on the first frame and every ~5s
                static TickType_t lastArtDiag = 0;
                TickType_t nowTicks = xTaskGetTickCount();
                if (shown != want || (nowTicks - lastArtDiag) > pdMS_TO_TICKS(5000)) {
                    lastArtDiag = nowTicks;
                    printf("[ART] not ready (title='%s')\n", title);
                }
            }

            if (shown != want || playing != shownPlaying ||
                strcmp(title, shownTitle) != 0 || strcmp(artist, shownArtist) != 0 ||
                hasArt != shownHasArt) {
                printf("Now playing: %s - %s art=%s (%ux%u)\n",
                       title, artist, hasArt ? "yes" : "no",
                       hasArt ? artW : 0, hasArt ? artH : 0);
                displayMediaWithArt(title, artist, playing,
                                    hasArt ? artBuffer : nullptr, artW, artH);
                strncpy(shownTitle, title, sizeof(shownTitle) - 1); shownTitle[sizeof(shownTitle) - 1] = 0;
                strncpy(shownArtist, artist, sizeof(shownArtist) - 1); shownArtist[sizeof(shownArtist) - 1] = 0;
                shownPlaying = playing;
                shownHasArt = hasArt;
            }
        }

        shown = want;
        vTaskDelay(pdMS_TO_TICKS(UI_POLL_MS));
    }
}