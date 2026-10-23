/*
 * button_handler.cpp — Track-skip pushbuttons (next, previous)
 *
 * GPIO interrupt-driven button handling. Each button triggers a falling-edge
 * interrupt that signals a semaphore; a single handler task debounces and
 * enqueues the corresponding Bluetooth command.
 *
 * NEVER calls BTstack APIs from the ISR — uses bluetoothEnqueue*() which are
 * queue-based and safe from any FreeRTOS task.
 *
 * Each button is active-LOW with INPUT_PULLUP (button press pulls the line
 * to GND). A simple debounce filter rejects bounces shorter than
 * BTN_DEBOUNCE_MS. Presses (not releases) enqueue the corresponding command.
 *
 * Note: play/pause is handled by the rotary encoder's pushbutton
 * (ENCODER_SW) in encoder.cpp.
 */

#include "config.h"
#include "bluetooth.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

typedef struct {
    gpio_num_t pin;
    TickType_t lastChange;
    void (*onPress)(void);
    const char *name;
} btn_t;

static void onForward(void)   { bluetoothEnqueueForward(); }
static void onBackward(void)  { bluetoothEnqueueBackward(); }

static btn_t buttons[] = {
    { (gpio_num_t)BTN_FORWARD,  0, onForward,  "Forward"  },
    { (gpio_num_t)BTN_BACKWARD, 0, onBackward, "Backward" },
};

static SemaphoreHandle_t btnSemaphore = nullptr;

static void IRAM_ATTR gpio_isr_handler(void *arg) {
    btn_t *b = (btn_t *)arg;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(btnSemaphore, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void button_task(void *) {
    // Install GPIO ISR service (shared across all GPIO interrupts)
    // Use ESP_FLAG_IRAM_ATTR for ISR that needs to be in IRAM
    esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        printf("[BTN] ISR service init failed: %d\n", err);
        vTaskDelete(nullptr);
        return;
    }
    // ESP_ERR_INVALID_STATE means it's already installed (by encoder task) - that's OK

    for (size_t i = 0; i < sizeof(buttons)/sizeof(buttons[0]); ++i) {
        gpio_config_t c = {};
        c.pin_bit_mask  = (1ULL << buttons[i].pin);
        c.mode          = GPIO_MODE_INPUT;
        c.pull_up_en    = GPIO_PULLUP_ENABLE;
        c.pull_down_en  = GPIO_PULLDOWN_DISABLE;
        c.intr_type     = GPIO_INTR_NEGEDGE;  // Falling edge = press (active LOW)
        gpio_config(&c);

        // Register ISR handler per-pin with this button's context
        gpio_isr_handler_add(buttons[i].pin, gpio_isr_handler, &buttons[i]);

        buttons[i].lastChange = xTaskGetTickCount();
    }

    while (true) {
        // Block until any button ISR fires
        if (xSemaphoreTake(btnSemaphore, portMAX_DELAY) == pdTRUE) {
            TickType_t now = xTaskGetTickCount();
            for (size_t i = 0; i < sizeof(buttons)/sizeof(buttons[0]); ++i) {
                // Confirm the pin is still LOW (press, not release bounce)
                if (gpio_get_level(buttons[i].pin) == 0) {
                    if ((now - buttons[i].lastChange) >= pdMS_TO_TICKS(BTN_DEBOUNCE_MS)) {
                        buttons[i].lastChange = now;
                        buttons[i].onPress();
                        printf("[BTN] %s press\n", buttons[i].name);
                    }
                }
            }
        }
    }
}

void buttonHandlerInit(void) {
    btnSemaphore = xSemaphoreCreateCounting(sizeof(buttons)/sizeof(buttons[0]), 0);
    xTaskCreate(button_task, "buttons", 2048, nullptr, 5, nullptr);
}