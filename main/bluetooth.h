#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum bt_screen_e {
    BT_SCREEN_CLOCK = 0,
    BT_SCREEN_LOGO,
    BT_SCREEN_MEDIA
} bt_screen_t;

// Initialize Bluetooth (NVS, BTstack task). Call once from app_main.
void bluetoothInit(void);

// Thread-safe commands for the encoder task.
// These enqueue a command to the BTstack run loop; no BTstack API is called
// from the caller's task. Safe to call from any FreeRTOS task.
void bluetoothEnqueueVolumeUp(void);
void bluetoothEnqueueVolumeDown(void);
void bluetoothEnqueuePlayPause(void);
void bluetoothEnqueueForward(void);
void bluetoothEnqueueBackward(void);

// Read connection state (UI task only, atomic bool read).
bool bluetoothConnected(void);

// Which screen the UI should show, derived from connection + playback state.
bt_screen_t bluetoothScreen(void);

// Snapshot the current now-playing metadata (thread-safe, mutex-protected).
void bluetoothGetMedia(char *title, unsigned tlen, char *artist, unsigned alen, bool *playing);

// Check if cover art is available and get its dimensions
bool bluetoothHasCoverArt(void);
unsigned bluetoothGetCoverArtWidth(void);
unsigned bluetoothGetCoverArtHeight(void);

// Get cover art data (returns pointer to 1-bit bitmap, caller provides buffer)
// Returns actual bytes copied (one bit per pixel, packed in bytes)
unsigned bluetoothGetCoverArtData(uint8_t *buffer, unsigned bufsize);

#ifdef __cplusplus
}
#endif