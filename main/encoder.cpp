/*
 * encoder.cpp — Rotary encoder (volume + play/pause)
 *
 * GPIO interrupt-driven rotary encoder handling. The CLK pin triggers
 * an interrupt on any edge for quadrature decoding; the SW pin triggers
 * an interrupt on falling edge for play/pause.
 *
 * Quadrature decoding:
 *   One volume command is emitted per full detent (two edge transitions).
 *   Half-steps are accumulated to prevent double-firing on noisy encoders.
 *
 * Rate limiting:
 *   Volume commands are throttled to at most one per ENCODER_RATE_LIMIT_MS.
 *   Button debounce uses ENCODER_DEBOUNCE_MS.
 */

#include "encoder.h"
#include "config.h"
#include "bluetooth.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// Quadrature decoding lookup table
// [previous_state][current_state] -> direction (+1 CW, -1 CCW, 0 invalid)
static const int8_t quad_table[4][4] = {
    //         to 00  to 01  to 10  to 11
    /* from 00 */ { 0,   -1,   +1,    0 },
    /* from 01 */ {+1,    0,    0,   -1 },
    /* from 10 */ {-1,    0,    0,   +1 },
    /* from 11 */ { 0,   +1,   -1,    0 },
};

static SemaphoreHandle_t encSemaphore = nullptr;

// Encoder state (accessed only from the handler task)
static int encAccumulator = 0;
static int encLastA = 0;
static int encLastB = 0;
static int encLastButton = 0;
static TickType_t encLastButtonChange = 0;
static TickType_t encLastVolCmd = 0;

static void IRAM_ATTR encoder_isr_handler(void *arg) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(encSemaphore, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void encoder_task(void *) {
    // Install GPIO ISR service (shared across all GPIO interrupts)
    esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        printf("[ENC] ISR service init failed: %d\n", err);
        vTaskDelete(nullptr);
        return;
    }
    // ESP_ERR_INVALID_STATE means it's already installed (by button task) - that's OK

    // Configure CLK and DT as inputs with interrupts on CLK
    gpio_config_t clk_dt_conf = {};
    clk_dt_conf.pin_bit_mask  = (1ULL << ENCODER_CLK) | (1ULL << ENCODER_DT);
    clk_dt_conf.mode          = GPIO_MODE_INPUT;
    clk_dt_conf.pull_up_en    = GPIO_PULLUP_ENABLE;
    clk_dt_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    clk_dt_conf.intr_type     = GPIO_INTR_ANYEDGE;  // Interrupt on any edge
    gpio_config(&clk_dt_conf);

    // Configure SW as input with falling-edge interrupt (active LOW press)
    gpio_config_t sw_conf = {};
    sw_conf.pin_bit_mask  = (1ULL << ENCODER_SW);
    sw_conf.mode          = GPIO_MODE_INPUT;
    sw_conf.pull_up_en    = GPIO_PULLUP_ENABLE;
    sw_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    sw_conf.intr_type     = GPIO_INTR_NEGEDGE;  // Falling edge = press
    gpio_config(&sw_conf);

    // Register ISR handler for CLK pin (main quadrature interrupt)
    gpio_isr_handler_add((gpio_num_t)ENCODER_CLK, encoder_isr_handler, nullptr);

    // Register ISR handler for SW pin (pushbutton)
    gpio_isr_handler_add((gpio_num_t)ENCODER_SW, encoder_isr_handler, nullptr);

    // Seed initial levels so we don't fire on boot
    encLastA = gpio_get_level((gpio_num_t)ENCODER_CLK);
    encLastB = gpio_get_level((gpio_num_t)ENCODER_DT);
    encLastButton = gpio_get_level((gpio_num_t)ENCODER_SW);
    encLastButtonChange = xTaskGetTickCount();
    encLastVolCmd = 0;

    const TickType_t volRateLimit = pdMS_TO_TICKS(ENCODER_RATE_LIMIT_MS);

    while (true) {
        // Block until any encoder interrupt fires
        if (xSemaphoreTake(encSemaphore, portMAX_DELAY) == pdTRUE) {
            TickType_t now = xTaskGetTickCount();

            // --- Quadrature decoding (CLK/DT) ---
            int a = gpio_get_level((gpio_num_t)ENCODER_CLK);
            int b = gpio_get_level((gpio_num_t)ENCODER_DT);

            if (a != encLastA || b != encLastB) {
                int prevState = (encLastA << 1) | encLastB;
                int currState = (a << 1) | b;
                if (prevState >= 0 && prevState < 4 && currState >= 0 && currState < 4) {
                    encAccumulator += quad_table[prevState][currState];
                }
                encLastA = a;
                encLastB = b;

                if (encAccumulator >= 2) {
                    encAccumulator = 0;
                    if ((now - encLastVolCmd) >= volRateLimit) {
                        encLastVolCmd = now;
                        bluetoothEnqueueVolumeUp();
                        printf("[ENC] Volume up queued\n");
                    }
                } else if (encAccumulator <= -2) {
                    encAccumulator = 0;
                    if ((now - encLastVolCmd) >= volRateLimit) {
                        encLastVolCmd = now;
                        bluetoothEnqueueVolumeDown();
                        printf("[ENC] Volume down queued\n");
                    }
                }
            }

            // --- Pushbutton (SW) ---
            int button = gpio_get_level((gpio_num_t)ENCODER_SW);
            if (button != encLastButton) {
                if ((now - encLastButtonChange) >= pdMS_TO_TICKS(ENCODER_DEBOUNCE_MS)) {
                    encLastButtonChange = now;
                    encLastButton = button;
                    if (button == 0) { // press (active low)
                        bluetoothEnqueuePlayPause();
                        printf("[ENC] Play/pause queued\n");
                    }
                }
            }
        }
    }
}

void encoderInit(void) {
    encSemaphore = xSemaphoreCreateCounting(10, 0);  // Counting semaphore for multiple rapid events
    xTaskCreate(encoder_task, "encoder", 3072, nullptr, 5, nullptr);
}