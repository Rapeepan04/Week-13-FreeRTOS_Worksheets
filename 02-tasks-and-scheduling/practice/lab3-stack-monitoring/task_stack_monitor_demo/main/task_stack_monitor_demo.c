#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"

#define LED_OK GPIO_NUM_2
#define LED_WARNING GPIO_NUM_4

static const char *TAG = "STACK_MONITOR";

#define STACK_WARNING_THRESHOLD 512
#define STACK_CRITICAL_THRESHOLD 256

TaskHandle_t light_task_handle = NULL;
TaskHandle_t medium_task_handle = NULL;
TaskHandle_t heavy_task_handle = NULL;

void stack_monitor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Stack Monitor Task started");

    while (1) {
        ESP_LOGI(TAG, "\n=== STACK USAGE REPORT ===");

        TaskHandle_t tasks[] = {
            light_task_handle,
            medium_task_handle,
            heavy_task_handle,
            xTaskGetCurrentTaskHandle()
        };

        const char *task_names[] = {
            "LightTask", "MediumTask", "HeavyTask", "StackMonitor"
        };

        bool stack_warning = false;
        bool stack_critical = false;

        for (int i = 0; i < 4; i++) {
            if (tasks[i] != NULL) {
                UBaseType_t stack_remaining = uxTaskGetStackHighWaterMark(tasks[i]);
                uint32_t stack_bytes = stack_remaining * sizeof(StackType_t);
                ESP_LOGI(TAG, "%s: %d bytes remaining", task_names[i], stack_bytes);

                if (stack_bytes < STACK_CRITICAL_THRESHOLD) {
                    ESP_LOGE(TAG, "CRITICAL: %s stack very low!", task_names[i]);
                    stack_critical = true;
                } else if (stack_bytes < STACK_WARNING_THRESHOLD) {
                    ESP_LOGW(TAG, "WARNING: %s stack low", task_names[i]);
                    stack_warning = true;
                }
            }
        }

        if (stack_critical) {
            for (int i = 0; i < 6; i++) {
                gpio_set_level(LED_WARNING, 1);
                vTaskDelay(pdMS_TO_TICKS(100));
                gpio_set_level(LED_WARNING, 0);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            gpio_set_level(LED_OK, 0);
        } else if (stack_warning) {
            gpio_set_level(LED_WARNING, 1);
            gpio_set_level(LED_OK, 0);
        } else {
            gpio_set_level(LED_OK, 1);
            gpio_set_level(LED_WARNING, 0);
        }

        ESP_LOGI(TAG, "Free heap: %d bytes", esp_get_free_heap_size());
        ESP_LOGI(TAG, "Min free heap: %d bytes", esp_get_minimum_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

void light_stack_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Light Stack Task started (minimal usage)");
    int counter = 0;

    while (1) {
        counter++;
        ESP_LOGI(TAG, "Light task cycle: %d", counter);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void medium_stack_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Medium Stack Task started (moderate usage)");
    while (1) {
        char buffer[256];
        int numbers[50];
        memset(buffer, 'A', sizeof(buffer) - 1);
        buffer[sizeof(buffer) - 1] = '\0';
        for (int i = 0; i < 50; i++) numbers[i] = i * i;
        ESP_LOGI(TAG, "Medium: buffer[0]=%c, numbers[49]=%d", buffer[0], numbers[49]);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

void heavy_stack_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Heavy Stack Task started (high usage)");
    int cycle = 0;
    while (1) {
        cycle++;
        char large_buffer[1024];
        int large_numbers[200];
        char another_buffer[512];
        memset(large_buffer, 'X', sizeof(large_buffer) - 1);
        large_buffer[sizeof(large_buffer) - 1] = '\0';
        for (int i = 0; i < 200; i++) large_numbers[i] = i * cycle;
        snprintf(another_buffer, sizeof(another_buffer), "Cycle %d done", cycle);

        ESP_LOGW(TAG, "Heavy: %s, last num=%d", another_buffer, large_numbers[199]);
        UBaseType_t stack_remaining = uxTaskGetStackHighWaterMark(NULL);
        uint32_t stack_bytes = stack_remaining * sizeof(StackType_t);
        ESP_LOGI(TAG, "Heavy stack remaining: %d bytes", stack_bytes);

        if (stack_bytes < STACK_CRITICAL_THRESHOLD)
            ESP_LOGE(TAG, "⚠️ DANGER: Stack critically low!");

        vTaskDelay(pdMS_TO_TICKS(4000));
    }
}

void optimized_heavy_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Optimized Heavy Task started");
    char *large_buffer = malloc(1024);
    int *large_numbers = malloc(200 * sizeof(int));
    char *another_buffer = malloc(512);

    if (!large_buffer || !large_numbers || !another_buffer) {
        ESP_LOGE(TAG, "Heap allocation failed!");
        vTaskDelete(NULL);
    }

    int cycle = 0;
    while (1) {
        cycle++;
        memset(large_buffer, 'Y', 1023);
        for (int i = 0; i < 200; i++) large_numbers[i] = i * cycle;
        snprintf(another_buffer, 512, "Optimized cycle %d", cycle);

        UBaseType_t stack_remaining = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGI(TAG, "Optimized stack remaining: %d bytes",
                 stack_remaining * sizeof(StackType_t));
        vTaskDelay(pdMS_TO_TICKS(4000));
    }
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    ESP_LOGE("STACK_OVERFLOW", "Task %s overflowed its stack!", pcTaskName);
    for (int i = 0; i < 20; i++) {
        gpio_set_level(LED_WARNING, 1);
        vTaskDelay(pdMS_TO_TICKS(25));
        gpio_set_level(LED_WARNING, 0);
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    esp_restart();
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== FreeRTOS Stack Monitoring Demo ===");

    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED_OK) | (1ULL << LED_WARNING),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "GPIO2 = OK, GPIO4 = WARNING");
    xTaskCreate(light_stack_task, "LightTask", 1024, NULL, 2, &light_task_handle);
    xTaskCreate(medium_stack_task, "MediumTask", 2048, NULL, 2, &medium_task_handle);
    xTaskCreate(heavy_stack_task, "HeavyTask", 2048, NULL, 2, &heavy_task_handle);
    xTaskCreate(optimized_heavy_task, "OptHeavy", 3072, NULL, 2, NULL);
    xTaskCreate(stack_monitor_task, "StackMonitor", 4096, NULL, 3, NULL);
}