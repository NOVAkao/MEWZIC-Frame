#include "bluetooth.h"
#include "config.h"

#include <stdlib.h>
#include <string.h>

#include "btstack.h"
#include "btstack_port_esp32.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern int a2dp_sink_setup(void);
extern int a2dp_sink_start(const bd_addr_t address);
extern bool a2dp_sink_is_connected(void);
extern uint8_t a2dp_sink_volume_up(void);
extern uint8_t a2dp_sink_volume_down(void);
extern uint8_t a2dp_sink_play_pause(void);
extern uint8_t a2dp_sink_forward(void);
extern uint8_t a2dp_sink_backward(void);
extern const char *a2dp_sink_get_title(unsigned max_len);
extern const char *a2dp_sink_get_artist(unsigned max_len);
extern bool a2dp_sink_get_playing(void);
extern bool a2dp_sink_has_started_playing(void);
extern bool a2dp_sink_has_cover_art(void);
extern unsigned a2dp_sink_get_cover_art_width(void);
extern unsigned a2dp_sink_get_cover_art_height(void);
extern unsigned a2dp_sink_get_cover_art_data(uint8_t *buffer, unsigned bufsize);

static bd_addr_t savedPeer = {0};
static bool haveSavedPeer = false;

static bool addressIsZero(const bd_addr_t address) {
    for (unsigned i = 0; i < sizeof(bd_addr_t); ++i) {
        if (address[i] != 0) return false;
    }
    return true;
}

static void loadSavedPeer(void) {
    nvs_handle_t handle;
    if (nvs_open(BT_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    size_t length = sizeof(savedPeer);
    esp_err_t status = nvs_get_blob(handle, BT_NVS_KEY_ADDR, savedPeer, &length);
    nvs_close(handle);
    haveSavedPeer = status == ESP_OK && length == sizeof(savedPeer) && !addressIsZero(savedPeer);
    if (haveSavedPeer) printf("[BT] Loaded peer: %s\n", bd_addr_to_str(savedPeer));
}

static void a2dpTask(void *) {
    btstack_init();
    if (a2dp_sink_setup() != 0) {
        printf("[BT] Reference AVRCP setup failed\n");
        vTaskDelete(nullptr);
        return;
    }
    printf("[BT] Starting reference A2DP/AVRCP stack\n");
    a2dp_sink_start(haveSavedPeer ? savedPeer : nullptr);
    vTaskDelete(nullptr);
}

void bluetoothInit(void) {
    esp_err_t status = nvs_flash_init();
    if (status == ESP_ERR_NVS_NO_FREE_PAGES || status == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        status = nvs_flash_init();
    }
    if (status != ESP_OK) printf("[BT] NVS init error: %d\n", status);
    loadSavedPeer();
    xTaskCreate(a2dpTask, "btstack", 8192, nullptr, 1, nullptr);
}

// --- Thread-safe wrappers: schedule BTstack calls on the BTstack run loop task ---

static void volume_up_callback(void * context) {
    a2dp_sink_volume_up();
    free(context);
}

static void volume_down_callback(void * context) {
    a2dp_sink_volume_down();
    free(context);
}

static void play_pause_callback(void * context) {
    a2dp_sink_play_pause();
    free(context);
}

static void forward_callback(void * context) {
    a2dp_sink_forward();
    free(context);
}

static void backward_callback(void * context) {
    a2dp_sink_backward();
    free(context);
}

static void enqueue_bt_command(void (*callback)(void *)) {
    btstack_context_callback_registration_t *registration =
        (btstack_context_callback_registration_t *)calloc(1, sizeof(*registration));
    if (!registration) return;
    registration->callback = callback;
    registration->context = registration;
    btstack_run_loop_execute_on_main_thread(registration);
}

void bluetoothEnqueueVolumeUp(void) { enqueue_bt_command(&volume_up_callback); }

void bluetoothEnqueueVolumeDown(void) { enqueue_bt_command(&volume_down_callback); }

void bluetoothEnqueuePlayPause(void) { enqueue_bt_command(&play_pause_callback); }

void bluetoothEnqueueForward(void) { enqueue_bt_command(&forward_callback); }

void bluetoothEnqueueBackward(void) { enqueue_bt_command(&backward_callback); }

bool bluetoothConnected(void) { return a2dp_sink_is_connected(); }

bt_screen_t bluetoothScreen(void) {
    if (!a2dp_sink_is_connected()) return BT_SCREEN_CLOCK;
    return a2dp_sink_has_started_playing() ? BT_SCREEN_MEDIA : BT_SCREEN_LOGO;
}

void bluetoothGetMedia(char *title, unsigned titleLength, char *artist, unsigned artistLength, bool *playing) {
    const char *sourceTitle = a2dp_sink_get_title(titleLength);
    const char *sourceArtist = a2dp_sink_get_artist(artistLength);
    if (title && titleLength) {
        strncpy(title, sourceTitle ? sourceTitle : "", titleLength - 1);
        title[titleLength - 1] = '\0';
    }
    if (artist && artistLength) {
        strncpy(artist, sourceArtist ? sourceArtist : "", artistLength - 1);
        artist[artistLength - 1] = '\0';
    }
    if (playing) *playing = a2dp_sink_get_playing();
}

bool bluetoothHasCoverArt(void) { return a2dp_sink_has_cover_art(); }
unsigned bluetoothGetCoverArtWidth(void) { return a2dp_sink_get_cover_art_width(); }
unsigned bluetoothGetCoverArtHeight(void) { return a2dp_sink_get_cover_art_height(); }
unsigned bluetoothGetCoverArtData(uint8_t *buffer, unsigned bufsize) {
    return a2dp_sink_get_cover_art_data(buffer, bufsize);
}