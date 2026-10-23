#pragma once

// Set to 1 for the Wokwi simulator: Wokwi does NOT emulate the ESP32 Bluetooth
// radio, so the controller hangs forever in r_rwip_init on power-on (task_wdt
// spam, and it starves the main loop so nothing draws). In sim we skip
// Bluetooth init and just run the clock. Set to 0 (or remove) for real hardware
// to enable Bluetooth.
#define WOKWI_SIMULATION 0

#define RTC_SDA 21
#define RTC_SCL 22
#define RTC_I2C_ADDRESS 0x68
#define RTC_I2C_PORT I2C_NUM_0
#define RTC_I2C_FREQ_HZ 100000

#define DISPLAY_WIDTH 296
#define DISPLAY_HEIGHT 128
#define DISPLAY_CONTROLLER_WIDTH 128
#define DISPLAY_CONTROLLER_HEIGHT 296
#define DISPLAY_BUFFER_SIZE (DISPLAY_WIDTH * DISPLAY_HEIGHT / 8)
#define DISPLAY_REFRESH_INTERVAL_MS 15000
#define UI_POLL_MS 250
#define DISPLAY_BUSY_TIMEOUT_MS 15000
#define DISPLAY_BUSY_POLL_INTERVAL_MS 10
#define DISPLAY_MOSI 23
#define DISPLAY_SCLK 18
#define DISPLAY_CS 5
#define DISPLAY_DC 17
#define DISPLAY_RST 16
#define DISPLAY_BUSY 4
#define DISPLAY_SPI_HOST SPI2_HOST
#define DISPLAY_SPI_CLOCK_HZ 1000000
#define DISPLAY_SPI_QUEUE_SIZE 1
#define DISPLAY_SPI_MAX_TRANSFER_SIZE DISPLAY_BUFFER_SIZE
#define DISPLAY_BUSY_ACTIVE_LEVEL 1
#define DISPLAY_BUSY_READY_LEVEL 0
#define DISPLAY_RESET_HIGH_MS 200
#define DISPLAY_RESET_LOW_MS 2
#define DISPLAY_RESET_RELEASE_MS 200
#define DISPLAY_POST_RESET_DELAY_MS 100
#define DISPLAY_POST_SOFTWARE_RESET_DELAY_MS 10

#define EPD_CMD_DRIVER_OUTPUT_CONTROL 0x01
#define EPD_CMD_DATA_ENTRY_MODE 0x11
#define EPD_CMD_SOFTWARE_RESET 0x12
#define EPD_CMD_DISPLAY_UPDATE_CTRL_1 0x21
#define EPD_CMD_DISPLAY_UPDATE_CTRL_2 0x22
#define EPD_CMD_WRITE_RAM 0x24
#define EPD_CMD_BORDER_WAVEFORM 0x3C
#define EPD_CMD_SET_RAM_X_START_END 0x44
#define EPD_CMD_SET_RAM_Y_START_END 0x45
#define EPD_CMD_SET_RAM_X_COUNTER 0x4E
#define EPD_CMD_SET_RAM_Y_COUNTER 0x4F
#define EPD_CMD_MASTER_ACTIVATION 0x20
#define EPD_CMD_DEEP_SLEEP 0x10
#define EPD_DRIVER_OUTPUT_0 0x27
#define EPD_DRIVER_OUTPUT_1 0x01
#define EPD_DRIVER_OUTPUT_2 0x00
#define EPD_DATA_ENTRY_MODE 0x03
#define EPD_BORDER_WAVEFORM 0x05
#define EPD_DISPLAY_UPDATE_CTRL_1_A 0x00
#define EPD_DISPLAY_UPDATE_CTRL_1_B 0x80
#define EPD_FULL_REFRESH_CONTROL 0xF7

#define CLOCK_SCALE 8
#define CLOCK_START_Y 4
#define CLOCK_DIGIT_WIDTH (5 * CLOCK_SCALE)
#define CLOCK_DIGIT_HEIGHT (9 * CLOCK_SCALE)
#define CLOCK_DIGIT_SPACING (2 * CLOCK_SCALE)
#define CLOCK_COLON_X_OFFSET (CLOCK_DIGIT_WIDTH + CLOCK_SCALE)
#define MEDIA_TITLE_Y 20
#define MEDIA_ARTIST_Y 38
#define MEDIA_MAX_CHARS 10
#define MEDIA_FONT_SCALE 3
#define DATE_Y 100
#define DATE_SCALE 3

#define ENCODER_CLK 32
#define ENCODER_DT 33
#define ENCODER_SW 27
#define ENCODER_DEBOUNCE_MS 30
#define ENCODER_POLL_MS 2
#define ENCODER_STEP_DIVIDER 1

// Music control buttons (active LOW with INPUT_PULLUP).
// Play/Pause lives on the encoder's pushbutton (ENCODER_SW = 27); only
// forward/backward are on dedicated pushbuttons.
#define BTN_FORWARD 26
#define BTN_BACKWARD 25
#define BTN_DEBOUNCE_MS 30
#define BTN_POLL_MS 10

#define BT_DEVICE_NAME "ESP32 Spotify Controller"
#define BT_AUTO_CONNECT 1
#define BT_PAIRING_PIN "0000"
#define BT_AVRCP_RECONNECT_DELAY_MS 2000
#define BT_VOLUME_STEP 8
#define BT_NVS_NAMESPACE "spotify_bt"
#define BT_NVS_KEY_ADDR "phone_addr"
#define BT_MAX_RECONNECT_RETRIES 5
#define BT_CMD_QUEUE_SIZE 8
#define ENCODER_RATE_LIMIT_MS 100

#define DISPLAY_DEBUG_LOGGING 1

// Cover art configuration
// Spotify sends album art over AVRCP cover art as a JPEG that is typically
// 10-30KB. This board (no PSRAM, ~74KB free DRAM) cannot host the
// previously-configured 256KB buffer. 64KB is more than enough for Spotify's
// Bluetooth-metadata cover art; if a track's art exceeds this it is dropped
// (the "buffer overflow" diagnostic will fire) rather than failing the alloc.
#define DISPLAY_COVER_ART 1
#define DISPLAY_COVER_ART_WIDTH 120
#define DISPLAY_COVER_ART_HEIGHT 120
#define DISPLAY_COVER_ART_X 4
#define DISPLAY_COVER_ART_Y 4
#define DISPLAY_COVER_ART_PADDING 2
#define MAX_COVER_ART_SIZE (64*1024)  // 64KB JPEG data buffer (fits in DRAM)