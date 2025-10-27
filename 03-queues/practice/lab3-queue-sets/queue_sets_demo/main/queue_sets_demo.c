#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_random.h"

static const char *TAG = "QUEUE_SETS";

// LED indicators
#define LED_SENSOR    GPIO_NUM_2
#define LED_USER      GPIO_NUM_4
#define LED_NETWORK   GPIO_NUM_5
#define LED_TIMER     GPIO_NUM_18
#define LED_PROCESSOR GPIO_NUM_19

QueueHandle_t xSensorQueue;
QueueHandle_t xUserQueue;
QueueHandle_t xNetworkQueue;
SemaphoreHandle_t xTimerSemaphore;
QueueSetHandle_t xQueueSet;

typedef struct {
    int sensor_id;
    float temperature;
    float humidity;
    uint32_t timestamp;
} sensor_data_t;

typedef struct {
    int button_id;
    bool pressed;
    uint32_t duration_ms;
} user_input_t;

typedef struct {
    char source[20];
    char message[100];
    int priority;
} network_message_t;

typedef struct {
    uint32_t sensor_count;
    uint32_t user_count;
    uint32_t network_count;
    uint32_t timer_count;
} message_stats_t;

message_stats_t stats = {0};

// -------- Sensor simulation task --------
void sensor_task(void *pvParameters) {
    sensor_data_t data;
    int id = 1;
    ESP_LOGI(TAG, "Sensor task started");
    while (1) {
        data.sensor_id = id;
        data.temperature = 20.0 + (esp_random() % 200) / 10.0;
        data.humidity = 30.0 + (esp_random() % 400) / 10.0;
        data.timestamp = xTaskGetTickCount();

        if (xQueueSend(xSensorQueue, &data, pdMS_TO_TICKS(100)) == pdPASS) {
            ESP_LOGI(TAG, "📊 Sensor: T=%.1f°C, H=%.1f%%", data.temperature, data.humidity);
            gpio_set_level(LED_SENSOR, 1);
            vTaskDelay(pdMS_TO_TICKS(50));
            gpio_set_level(LED_SENSOR, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(2000 + (esp_random() % 3000)));
    }
}

// -------- User input simulation task --------
void user_input_task(void *pvParameters) {
    user_input_t input;
    ESP_LOGI(TAG, "User input task started");
    while (1) {
        input.button_id = 1 + (esp_random() % 3);
        input.pressed = true;
        input.duration_ms = 100 + (esp_random() % 1000);
        if (xQueueSend(xUserQueue, &input, pdMS_TO_TICKS(100)) == pdPASS) {
            ESP_LOGI(TAG, "🔘 User: Button %d pressed for %dms", input.button_id, input.duration_ms);
            gpio_set_level(LED_USER, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(LED_USER, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(3000 + (esp_random() % 5000)));
    }
}

// -------- Network simulation task --------
void network_task(void *pvParameters) {
    network_message_t msg;
    const char* sources[] = {"WiFi", "LoRa", "Bluetooth", "Ethernet"};
    const char* messages[] = {"Status update", "Alert!", "Sync data", "Heartbeat", "Config changed"};
    ESP_LOGI(TAG, "Network task started");
    while (1) {
        strcpy(msg.source, sources[esp_random() % 4]);
        strcpy(msg.message, messages[esp_random() % 5]);
        msg.priority = 1 + (esp_random() % 5);
        if (xQueueSend(xNetworkQueue, &msg, pdMS_TO_TICKS(100)) == pdPASS) {
            ESP_LOGI(TAG, "🌐 Network [%s]: %s (P:%d)", msg.source, msg.message, msg.priority);
            gpio_set_level(LED_NETWORK, 1);
            vTaskDelay(pdMS_TO_TICKS(50));
            gpio_set_level(LED_NETWORK, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(1000 + (esp_random() % 3000)));
    }
}

// -------- Timer task --------
void timer_task(void *pvParameters) {
    ESP_LOGI(TAG, "Timer task started");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        xSemaphoreGive(xTimerSemaphore);
        ESP_LOGI(TAG, "⏰ Timer event fired");
        gpio_set_level(LED_TIMER, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(LED_TIMER, 0);
    }
}

// -------- Processor task (core of queue set) --------
void processor_task(void *pvParameters) {
    QueueSetMemberHandle_t member;
    sensor_data_t sdata;
    user_input_t uinput;
    network_message_t nmsg;

    ESP_LOGI(TAG, "Processor task waiting for events...");
    while (1) {
        member = xQueueSelectFromSet(xQueueSet, portMAX_DELAY);
        gpio_set_level(LED_PROCESSOR, 1);

        if (member == xSensorQueue && xQueueReceive(xSensorQueue, &sdata, 0) == pdPASS) {
            stats.sensor_count++;
            ESP_LOGI(TAG, "→ SENSOR: %.1f°C %.1f%%", sdata.temperature, sdata.humidity);
        }
        else if (member == xUserQueue && xQueueReceive(xUserQueue, &uinput, 0) == pdPASS) {
            stats.user_count++;
            ESP_LOGI(TAG, "→ USER: Button %d (%dms)", uinput.button_id, uinput.duration_ms);
        }
        else if (member == xNetworkQueue && xQueueReceive(xNetworkQueue, &nmsg, 0) == pdPASS) {
            stats.network_count++;
            ESP_LOGI(TAG, "→ NETWORK: [%s] %s", nmsg.source, nmsg.message);
        }
        else if (member == xTimerSemaphore && xSemaphoreTake(xTimerSemaphore, 0) == pdPASS) {
            stats.timer_count++;
            ESP_LOGI(TAG, "→ TIMER: Maintenance event");
            ESP_LOGI(TAG, "📈 Counts → Sensor:%lu | User:%lu | Net:%lu | Timer:%lu",
                     stats.sensor_count, stats.user_count, stats.network_count, stats.timer_count);
        }

        vTaskDelay(pdMS_TO_TICKS(200));
        gpio_set_level(LED_PROCESSOR, 0);
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "=== Queue Sets Demo Starting ===");
    gpio_set_direction(LED_SENSOR, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_USER, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_NETWORK, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_TIMER, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_PROCESSOR, GPIO_MODE_OUTPUT);

    xSensorQueue = xQueueCreate(5, sizeof(sensor_data_t));
    xUserQueue = xQueueCreate(3, sizeof(user_input_t));
    xNetworkQueue = xQueueCreate(8, sizeof(network_message_t));
    xTimerSemaphore = xSemaphoreCreateBinary();
    xQueueSet = xQueueCreateSet(5 + 3 + 8 + 1);

    if (xQueueSet && xSensorQueue && xUserQueue && xNetworkQueue && xTimerSemaphore) {
        xQueueAddToSet(xSensorQueue, xQueueSet);
        xQueueAddToSet(xUserQueue, xQueueSet);
        xQueueAddToSet(xNetworkQueue, xQueueSet);
        xQueueAddToSet(xTimerSemaphore, xQueueSet);

        xTaskCreate(sensor_task, "Sensor", 2048, NULL, 3, NULL);
        xTaskCreate(user_input_task, "User", 2048, NULL, 3, NULL);
        xTaskCreate(network_task, "Network", 2048, NULL, 3, NULL);
        xTaskCreate(timer_task, "Timer", 2048, NULL, 2, NULL);
        xTaskCreate(processor_task, "Processor", 3072, NULL, 4, NULL);
    }
}