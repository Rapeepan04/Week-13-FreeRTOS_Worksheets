#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_random.h"

static const char *TAG = "SW_TIMERS";

#define LED_BLINK GPIO_NUM_2
#define LED_HEARTBEAT GPIO_NUM_4
#define LED_STATUS GPIO_NUM_5
#define LED_ONESHOT GPIO_NUM_18

TimerHandle_t xBlinkTimer;
TimerHandle_t xHeartbeatTimer;
TimerHandle_t xStatusTimer;
TimerHandle_t xOneShotTimer;
TimerHandle_t xDynamicTimer;

#define BLINK_PERIOD     500
#define HEARTBEAT_PERIOD 2000
#define STATUS_PERIOD    5000
#define ONESHOT_DELAY    3000

typedef struct {
    uint32_t blink_count;
    uint32_t heartbeat_count;
    uint32_t status_count;
    uint32_t oneshot_count;
    uint32_t dynamic_count;
} timer_stats_t;

timer_stats_t stats = {0, 0, 0, 0, 0};

bool led_blink_state = false;
bool led_heartbeat_state = false;

void dynamic_timer_callback(TimerHandle_t xTimer);

void blink_timer_callback(TimerHandle_t xTimer) {
    stats.blink_count++;
    led_blink_state = !led_blink_state;
    gpio_set_level(LED_BLINK, led_blink_state);
    ESP_LOGI(TAG, "💫 Blink Timer: Toggle #%lu (LED: %s)",
             stats.blink_count, led_blink_state ? "ON" : "OFF");

    if (stats.blink_count % 20 == 0) {
        ESP_LOGI(TAG, "🚀 Creating one-shot timer (3 second delay)");
        if (xTimerStart(xOneShotTimer, 0) != pdPASS)
            ESP_LOGW(TAG, "Failed to start one-shot timer");
    }
}

void heartbeat_timer_callback(TimerHandle_t xTimer) {
    stats.heartbeat_count++;
    ESP_LOGI(TAG, "💓 Heartbeat Timer: Beat #%lu", stats.heartbeat_count);

    gpio_set_level(LED_HEARTBEAT, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(LED_HEARTBEAT, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(LED_HEARTBEAT, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(LED_HEARTBEAT, 0);

    if (esp_random() % 4 == 0) {
        uint32_t new_period = 300 + (esp_random() % 400);
        ESP_LOGI(TAG, "🔧 Adjusting blink period to %lums", new_period);
        xTimerChangePeriod(xBlinkTimer, pdMS_TO_TICKS(new_period), 100);
    }
}

void status_timer_callback(TimerHandle_t xTimer) {
    stats.status_count++;
    ESP_LOGI(TAG, "📊 Status Timer: Update #%lu", stats.status_count);

    gpio_set_level(LED_STATUS, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(LED_STATUS, 0);

    ESP_LOGI(TAG, "═══ TIMER STATISTICS ═══");
    ESP_LOGI(TAG, "Blink events:     %lu", stats.blink_count);
    ESP_LOGI(TAG, "Heartbeat events: %lu", stats.heartbeat_count);
    ESP_LOGI(TAG, "Status updates:   %lu", stats.status_count);
    ESP_LOGI(TAG, "One-shot events:  %lu", stats.oneshot_count);
    ESP_LOGI(TAG, "Dynamic events:   %lu", stats.dynamic_count);
    ESP_LOGI(TAG, "═══════════════════════");
}

void oneshot_timer_callback(TimerHandle_t xTimer) {
    stats.oneshot_count++;
    ESP_LOGI(TAG, "⚡ One-shot Timer: Event #%lu", stats.oneshot_count);

    for (int i = 0; i < 5; i++) {
        gpio_set_level(LED_ONESHOT, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
        gpio_set_level(LED_ONESHOT, 0);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    uint32_t random_period = 1000 + (esp_random() % 3000);
    ESP_LOGI(TAG, "🎲 Creating dynamic timer (period: %lums)", random_period);

    xDynamicTimer = xTimerCreate("DynamicTimer",
                                 pdMS_TO_TICKS(random_period),
                                 pdFALSE, (void*)0,
                                 dynamic_timer_callback);

    if (xDynamicTimer != NULL)
        xTimerStart(xDynamicTimer, 0);
}

void dynamic_timer_callback(TimerHandle_t xTimer) {
    stats.dynamic_count++;
    ESP_LOGI(TAG, "🌟 Dynamic Timer: Event #%lu", stats.dynamic_count);

    gpio_set_level(LED_BLINK, 1);
    gpio_set_level(LED_HEARTBEAT, 1);
    gpio_set_level(LED_STATUS, 1);
    gpio_set_level(LED_ONESHOT, 1);
    vTaskDelay(pdMS_TO_TICKS(300));

    gpio_set_level(LED_BLINK, led_blink_state);
    gpio_set_level(LED_HEARTBEAT, 0);
    gpio_set_level(LED_STATUS, 0);
    gpio_set_level(LED_ONESHOT, 0);

    xTimerDelete(xTimer, 100);
    xDynamicTimer = NULL;
}

void timer_control_task(void *pvParameters) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        ESP_LOGI(TAG, "\n🎛️ TIMER CONTROL: Performing maintenance...");

        int action = esp_random() % 3;
        switch (action) {
            case 0:
                ESP_LOGI(TAG, "⏸️  Stop heartbeat 5s");
                xTimerStop(xHeartbeatTimer, 100);
                vTaskDelay(pdMS_TO_TICKS(5000));
                xTimerStart(xHeartbeatTimer, 100);
                break;
            case 1:
                ESP_LOGI(TAG, "🔄 Reset status timer");
                xTimerReset(xStatusTimer, 100);
                break;
            case 2:
                ESP_LOGI(TAG, "⚙️ Change blink period");
                uint32_t new_period = 200 + (esp_random() % 600);
                xTimerChangePeriod(xBlinkTimer, pdMS_TO_TICKS(new_period), 100);
                break;
        }
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Software Timers Lab Starting...");

    gpio_set_direction(LED_BLINK, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_HEARTBEAT, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_STATUS, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_ONESHOT, GPIO_MODE_OUTPUT);

    gpio_set_level(LED_BLINK, 0);
    gpio_set_level(LED_HEARTBEAT, 0);
    gpio_set_level(LED_STATUS, 0);
    gpio_set_level(LED_ONESHOT, 0);

    xBlinkTimer = xTimerCreate("BlinkTimer", pdMS_TO_TICKS(BLINK_PERIOD), pdTRUE, (void*)1, blink_timer_callback);
    xHeartbeatTimer = xTimerCreate("HeartbeatTimer", pdMS_TO_TICKS(HEARTBEAT_PERIOD), pdTRUE, (void*)2, heartbeat_timer_callback);
    xStatusTimer = xTimerCreate("StatusTimer", pdMS_TO_TICKS(STATUS_PERIOD), pdTRUE, (void*)3, status_timer_callback);
    xOneShotTimer = xTimerCreate("OneShotTimer", pdMS_TO_TICKS(ONESHOT_DELAY), pdFALSE, (void*)4, oneshot_timer_callback);

    if (xBlinkTimer && xHeartbeatTimer && xStatusTimer && xOneShotTimer) {
        xTimerStart(xBlinkTimer, 0);
        xTimerStart(xHeartbeatTimer, 0);
        xTimerStart(xStatusTimer, 0);
        xTaskCreate(timer_control_task, "TimerControl", 2048, NULL, 2, NULL);
        ESP_LOGI(TAG, "✅ All timers started successfully");
    } else {
        ESP_LOGE(TAG, "❌ Failed to create one or more timers");
    }
}