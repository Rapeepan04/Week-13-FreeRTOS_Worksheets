#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_random.h"

static const char *TAG = "BINARY_SEM";

// ======================= GPIO CONFIG ==========================
#define LED_PRODUCER GPIO_NUM_2
#define LED_CONSUMER GPIO_NUM_4
#define LED_TIMER    GPIO_NUM_5
#define BUTTON_PIN   GPIO_NUM_0

// ======================= SEMAPHORES ============================
SemaphoreHandle_t xBinarySemaphore;
SemaphoreHandle_t xTimerSemaphore;
SemaphoreHandle_t xButtonSemaphore;

// Timer handle
gptimer_handle_t gptimer = NULL;

// ======================= STATS STRUCT ==========================
typedef struct {
    uint32_t signals_sent;
    uint32_t signals_received;
    uint32_t timer_events;
    uint32_t button_presses;
} semaphore_stats_t;

semaphore_stats_t stats = {0, 0, 0, 0};

// ======================= TIMER ISR =============================
static bool IRAM_ATTR timer_callback(gptimer_handle_t timer,
                                    const gptimer_alarm_event_data_t *edata,
                                    void *user_data) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(xTimerSemaphore, &xHigherPriorityTaskWoken);
    return (xHigherPriorityTaskWoken == pdTRUE);
}

// ======================= BUTTON ISR ===========================
static void IRAM_ATTR button_isr_handler(void* arg) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(xButtonSemaphore, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

// ======================= PRODUCER TASK =========================
void producer_task(void *pvParameters) {
    int event_counter = 0;
    ESP_LOGI(TAG, "Producer task started");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000 + (esp_random() % 3000))); // 2–5 s
        event_counter++;
        ESP_LOGI(TAG, "🔥 Producer: Generating event #%d", event_counter);

        if (xSemaphoreGive(xBinarySemaphore) == pdTRUE) {
            stats.signals_sent++;
            ESP_LOGI(TAG, "✓ Producer: Event signaled successfully");
            gpio_set_level(LED_PRODUCER, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(LED_PRODUCER, 0);
        } else {
            ESP_LOGW(TAG, "✗ Producer: Failed to signal (already given?)");
        }
    }
}

// ======================= CONSUMER TASK =========================
void consumer_task(void *pvParameters) {
    ESP_LOGI(TAG, "Consumer task started - waiting for events...");
    while (1) {
        ESP_LOGI(TAG, "🔍 Consumer: Waiting for event...");
        if (xSemaphoreTake(xBinarySemaphore, pdMS_TO_TICKS(10000)) == pdTRUE) {
            stats.signals_received++;
            ESP_LOGI(TAG, "⚡ Consumer: Event received! Processing...");
            gpio_set_level(LED_CONSUMER, 1);
            vTaskDelay(pdMS_TO_TICKS(1000 + (esp_random() % 2000))); // 1–3 s
            gpio_set_level(LED_CONSUMER, 0);
            ESP_LOGI(TAG, "✓ Consumer: Event processed successfully");
        } else {
            ESP_LOGW(TAG, "⏰ Consumer: Timeout waiting for event");
        }
    }
}

// ======================= TIMER EVENT TASK ======================
void timer_event_task(void *pvParameters) {
    ESP_LOGI(TAG, "Timer event task started");
    while (1) {
        if (xSemaphoreTake(xTimerSemaphore, portMAX_DELAY) == pdTRUE) {
            stats.timer_events++;
            ESP_LOGI(TAG, "⏱️ Timer: Periodic timer event #%lu", stats.timer_events);
            gpio_set_level(LED_TIMER, 1);
            vTaskDelay(pdMS_TO_TICKS(200));
            gpio_set_level(LED_TIMER, 0);

            if (stats.timer_events % 5 == 0) {
                ESP_LOGI(TAG, "📊 Stats - Sent:%lu, Received:%lu, Timer:%lu, Button:%lu",
                         stats.signals_sent, stats.signals_received,
                         stats.timer_events, stats.button_presses);
            }
        }
    }
}

// ======================= BUTTON EVENT TASK =====================
void button_event_task(void *pvParameters) {
    ESP_LOGI(TAG, "Button event task started");
    while (1) {
        if (xSemaphoreTake(xButtonSemaphore, portMAX_DELAY) == pdTRUE) {
            stats.button_presses++;
            ESP_LOGI(TAG, "🔘 Button: Press detected #%lu", stats.button_presses);
            vTaskDelay(pdMS_TO_TICKS(300)); // Debounce
            ESP_LOGI(TAG, "🚀 Button: Triggering immediate producer event");
            xSemaphoreGive(xBinarySemaphore);
            stats.signals_sent++;
        }
    }
}

// ======================= MONITOR TASK ==========================
void monitor_task(void *pvParameters) {
    ESP_LOGI(TAG, "System monitor started");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(15000)); // every 15 sec
        ESP_LOGI(TAG, "\n═══ SEMAPHORE SYSTEM MONITOR ═══");
        ESP_LOGI(TAG, "Binary Semaphore Available: %s",
                 uxSemaphoreGetCount(xBinarySemaphore) ? "YES" : "NO");
        ESP_LOGI(TAG, "Timer Semaphore Count: %d",
                 uxSemaphoreGetCount(xTimerSemaphore));
        ESP_LOGI(TAG, "Button Semaphore Count: %d",
                 uxSemaphoreGetCount(xButtonSemaphore));
        ESP_LOGI(TAG, "Event Stats:");
        ESP_LOGI(TAG, "  Producer: %lu", stats.signals_sent);
        ESP_LOGI(TAG, "  Consumer: %lu", stats.signals_received);
        ESP_LOGI(TAG, "  Timer:    %lu", stats.timer_events);
        ESP_LOGI(TAG, "  Button:   %lu", stats.button_presses);

        float efficiency = stats.signals_sent > 0 ?
                           (float)stats.signals_received / stats.signals_sent * 100 : 0;
        ESP_LOGI(TAG, "  Efficiency: %.1f%%", efficiency);
        ESP_LOGI(TAG, "══════════════════════════════\n");
    }
}

// ======================= APP MAIN ==============================
void app_main(void) {
    ESP_LOGI(TAG, "Binary Semaphores Lab Starting...");

    // Configure GPIO
    gpio_set_direction(LED_PRODUCER, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_CONSUMER, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_TIMER, GPIO_MODE_OUTPUT);
    gpio_set_direction(BUTTON_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BUTTON_PIN, GPIO_PULLUP_ONLY);
    gpio_set_intr_type(BUTTON_PIN, GPIO_INTR_NEGEDGE);

    gpio_set_level(LED_PRODUCER, 0);
    gpio_set_level(LED_CONSUMER, 0);
    gpio_set_level(LED_TIMER, 0);

    // Create semaphores
    xBinarySemaphore = xSemaphoreCreateBinary();
    xTimerSemaphore = xSemaphoreCreateBinary();
    xButtonSemaphore = xSemaphoreCreateBinary();

    if (xBinarySemaphore && xTimerSemaphore && xButtonSemaphore) {
        ESP_LOGI(TAG, "All semaphores created successfully");

        // Button ISR setup
        gpio_install_isr_service(0);
        gpio_isr_handler_add(BUTTON_PIN, button_isr_handler, NULL);

        // Timer setup
        gptimer_config_t timer_config = {
            .clk_src = GPTIMER_CLK_SRC_DEFAULT,
            .direction = GPTIMER_COUNT_UP,
            .resolution_hz = 1000000,
        };
        ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &gptimer));

        gptimer_event_callbacks_t cbs = { .on_alarm = timer_callback };
        ESP_ERROR_CHECK(gptimer_register_event_callbacks(gptimer, &cbs, NULL));
        ESP_ERROR_CHECK(gptimer_enable(gptimer));

        gptimer_alarm_config_t alarm_config = {
            .alarm_count = 8000000, // 8s
            .reload_count = 0,
            .flags.auto_reload_on_alarm = true,
        };
        ESP_ERROR_CHECK(gptimer_set_alarm_action(gptimer, &alarm_config));
        ESP_ERROR_CHECK(gptimer_start(gptimer));
        ESP_LOGI(TAG, "Timer configured for 8-second intervals");

        // Create tasks
        xTaskCreate(producer_task, "Producer", 2048, NULL, 3, NULL);
        xTaskCreate(consumer_task, "Consumer", 2048, NULL, 2, NULL);
        xTaskCreate(timer_event_task, "TimerEvent", 2048, NULL, 2, NULL);
        xTaskCreate(button_event_task, "ButtonEvent", 2048, NULL, 4, NULL);
        xTaskCreate(monitor_task, "Monitor", 2048, NULL, 1, NULL);

        ESP_LOGI(TAG, "All tasks created. System operational.");
        ESP_LOGI(TAG, "💡 Press the BOOT button (GPIO0) to trigger immediate events!");
    } else {
        ESP_LOGE(TAG, "Failed to create semaphores!");
    }
}