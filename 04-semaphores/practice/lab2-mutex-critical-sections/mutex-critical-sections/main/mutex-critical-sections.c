#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_random.h"

static const char *TAG = "MUTEX_LAB";

#define LED_TASK1 GPIO_NUM_2
#define LED_TASK2 GPIO_NUM_4
#define LED_TASK3 GPIO_NUM_5
#define LED_CRITICAL GPIO_NUM_18

SemaphoreHandle_t xMutex;

typedef struct {
    uint32_t counter;
    char shared_buffer[100];
    uint32_t checksum;
    uint32_t access_count;
} shared_resource_t;

shared_resource_t shared_data = {0, "", 0, 0};

typedef struct {
    uint32_t successful_access;
    uint32_t failed_access;
    uint32_t corruption_detected;
} access_stats_t;

access_stats_t stats = {0, 0, 0};

uint32_t calculate_checksum(const char* data, uint32_t counter) {
    uint32_t sum = counter;
    for (int i = 0; data[i] != '\0'; i++) sum += (uint32_t)data[i] * (i + 1);
    return sum;
}

void access_shared_resource(const char* task_name, gpio_num_t led_pin) {
    char temp_buffer[100];
    uint32_t temp_counter, expected_checksum;
    ESP_LOGI(TAG, "[%s] Requesting access...", task_name);

    if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
        stats.successful_access++;
        gpio_set_level(led_pin, 1);
        gpio_set_level(LED_CRITICAL, 1);

        temp_counter = shared_data.counter;
        strcpy(temp_buffer, shared_data.shared_buffer);
        expected_checksum = shared_data.checksum;

        uint32_t calc = calculate_checksum(temp_buffer, temp_counter);
        if (calc != expected_checksum && shared_data.access_count > 0) {
            ESP_LOGE(TAG, "[%s] ⚠️  DATA CORRUPTION DETECTED!", task_name);
            stats.corruption_detected++;
        }

        ESP_LOGI(TAG, "[%s] Current: #%lu, '%s'", task_name, temp_counter, temp_buffer);
        vTaskDelay(pdMS_TO_TICKS(500 + (esp_random() % 1000)));

        shared_data.counter = temp_counter + 1;
        snprintf(shared_data.shared_buffer, sizeof(shared_data.shared_buffer),
                 "Modified by %s #%lu", task_name, shared_data.counter);
        shared_data.checksum = calculate_checksum(shared_data.shared_buffer, shared_data.counter);
        shared_data.access_count++;

        ESP_LOGI(TAG, "[%s] ✓ Updated shared resource", task_name);

        vTaskDelay(pdMS_TO_TICKS(200 + (esp_random() % 500)));

        gpio_set_level(led_pin, 0);
        gpio_set_level(LED_CRITICAL, 0);
        xSemaphoreGive(xMutex);
    } else {
        ESP_LOGW(TAG, "[%s] ✗ Mutex timeout", task_name);
        stats.failed_access++;
    }
}

void high_priority_task(void *pvParameters) {
    ESP_LOGI(TAG, "High Priority Task started");
    while (1) {
        access_shared_resource("HIGH_PRI", LED_TASK1);
        vTaskDelay(pdMS_TO_TICKS(5000 + (esp_random() % 3000)));
    }
}

void medium_priority_task(void *pvParameters) {
    ESP_LOGI(TAG, "Medium Priority Task started");
    while (1) {
        access_shared_resource("MED_PRI", LED_TASK2);
        vTaskDelay(pdMS_TO_TICKS(3000 + (esp_random() % 2000)));
    }
}

void low_priority_task(void *pvParameters) {
    ESP_LOGI(TAG, "Low Priority Task started");
    while (1) {
        access_shared_resource("LOW_PRI", LED_TASK3);
        vTaskDelay(pdMS_TO_TICKS(2000 + (esp_random() % 1000)));
    }
}

void monitor_task(void *pvParameters) {
    ESP_LOGI(TAG, "Monitor started");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "\n═══ MUTEX MONITOR ═══");
        ESP_LOGI(TAG, "Counter: %lu", shared_data.counter);
        ESP_LOGI(TAG, "Buffer:  %s", shared_data.shared_buffer);
        ESP_LOGI(TAG, "Access count: %lu", shared_data.access_count);

        uint32_t calc = calculate_checksum(shared_data.shared_buffer, shared_data.counter);
        if (calc != shared_data.checksum && shared_data.access_count > 0) {
            ESP_LOGE(TAG, "⚠️  CURRENT DATA CORRUPTION DETECTED!");
            stats.corruption_detected++;
        }

        float rate = (stats.successful_access + stats.failed_access) > 0 ?
            (float)stats.successful_access /
            (stats.successful_access + stats.failed_access) * 100 : 0;

        ESP_LOGI(TAG, "Stats: success=%lu, failed=%lu, corrupt=%lu, rate=%.1f%%",
                 stats.successful_access, stats.failed_access,
                 stats.corruption_detected, rate);
        ESP_LOGI(TAG, "═════════════════════════\n");
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Mutex & Critical Sections Lab Starting...");

    gpio_set_direction(LED_TASK1, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_TASK2, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_TASK3, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_CRITICAL, GPIO_MODE_OUTPUT);

    gpio_set_level(LED_TASK1, 0);
    gpio_set_level(LED_TASK2, 0);
    gpio_set_level(LED_TASK3, 0);
    gpio_set_level(LED_CRITICAL, 0);

    xMutex = xSemaphoreCreateMutex();

    if (xMutex != NULL) {
        shared_data.counter = 0;
        strcpy(shared_data.shared_buffer, "Initial state");
        shared_data.checksum = calculate_checksum(shared_data.shared_buffer, shared_data.counter);

        xTaskCreate(high_priority_task, "HighPri", 3072, NULL, 5, NULL);
        xTaskCreate(medium_priority_task, "MedPri", 3072, NULL, 3, NULL);
        xTaskCreate(low_priority_task, "LowPri", 3072, NULL, 2, NULL);
        xTaskCreate(monitor_task, "Monitor", 3072, NULL, 1, NULL);

        ESP_LOGI(TAG, "All tasks created successfully");
    } else {
        ESP_LOGE(TAG, "Failed to create mutex!");
    }
}