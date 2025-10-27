#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_random.h"

static const char *TAG = "COUNTING_SEM";

#define LED_RESOURCE_1 GPIO_NUM_2
#define LED_RESOURCE_2 GPIO_NUM_4
#define LED_RESOURCE_3 GPIO_NUM_5
#define LED_PRODUCER   GPIO_NUM_18
#define LED_SYSTEM     GPIO_NUM_19

#define MAX_RESOURCES 3   // จำนวน resource ที่มีอยู่จริง
#define NUM_PRODUCERS 5   // จำนวน producer tasks

SemaphoreHandle_t xCountingSemaphore;

typedef struct {
    int resource_id;
    bool in_use;
    char current_user[20];
    uint32_t usage_count;
    uint32_t total_usage_time;
} resource_t;

resource_t resources[MAX_RESOURCES] = {
    {1, false, "", 0, 0},
    {2, false, "", 0, 0},
    {3, false, "", 0, 0}
};

typedef struct {
    uint32_t total_requests;
    uint32_t successful_acquisitions;
    uint32_t failed_acquisitions;
    uint32_t resources_in_use;
} system_stats_t;

system_stats_t stats = {0, 0, 0, 0};

int acquire_resource(const char* user_name) {
    for (int i = 0; i < MAX_RESOURCES; i++) {
        if (!resources[i].in_use) {
            resources[i].in_use = true;
            strcpy(resources[i].current_user, user_name);
            resources[i].usage_count++;
            switch (i) {
                case 0: gpio_set_level(LED_RESOURCE_1, 1); break;
                case 1: gpio_set_level(LED_RESOURCE_2, 1); break;
                case 2: gpio_set_level(LED_RESOURCE_3, 1); break;
            }
            stats.resources_in_use++;
            return i;
        }
    }
    return -1;
}

void release_resource(int index, uint32_t usage_time) {
    if (index >= 0 && index < MAX_RESOURCES) {
        resources[index].in_use = false;
        strcpy(resources[index].current_user, "");
        resources[index].total_usage_time += usage_time;
        switch (index) {
            case 0: gpio_set_level(LED_RESOURCE_1, 0); break;
            case 1: gpio_set_level(LED_RESOURCE_2, 0); break;
            case 2: gpio_set_level(LED_RESOURCE_3, 0); break;
        }
        stats.resources_in_use--;
    }
}

void producer_task(void *pvParameters) {
    int id = *((int*)pvParameters);
    char name[16];
    snprintf(name, sizeof(name), "Producer%d", id);
    ESP_LOGI(TAG, "%s started", name);

    while (1) {
        stats.total_requests++;
        ESP_LOGI(TAG, "🏭 %s: Requesting resource...", name);
        gpio_set_level(LED_PRODUCER, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
        gpio_set_level(LED_PRODUCER, 0);

        uint32_t start = xTaskGetTickCount();

        if (xSemaphoreTake(xCountingSemaphore, pdMS_TO_TICKS(8000)) == pdTRUE) {
            uint32_t wait_ms = (xTaskGetTickCount() - start) * portTICK_PERIOD_MS;
            stats.successful_acquisitions++;

            int res = acquire_resource(name);
            if (res >= 0) {
                ESP_LOGI(TAG, "✓ %s: Acquired resource %d (wait %lums)", name, res + 1, wait_ms);
                uint32_t use_time = 1000 + (esp_random() % 3000);
                vTaskDelay(pdMS_TO_TICKS(use_time));
                release_resource(res, use_time);
                xSemaphoreGive(xCountingSemaphore);
                ESP_LOGI(TAG, "✓ %s: Released resource %d", name, res + 1);
            } else {
                ESP_LOGE(TAG, "✗ %s: No free resource!", name);
                xSemaphoreGive(xCountingSemaphore);
            }
        } else {
            stats.failed_acquisitions++;
            ESP_LOGW(TAG, "⏰ %s: Timeout waiting for resource", name);
        }

        vTaskDelay(pdMS_TO_TICKS(2000 + (esp_random() % 3000)));
    }
}

void resource_monitor_task(void *pvParameters) {
    ESP_LOGI(TAG, "Resource monitor started");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        int available = uxSemaphoreGetCount(xCountingSemaphore);
        ESP_LOGI(TAG, "\n📊 RESOURCE STATUS (%d free of %d)", available, MAX_RESOURCES);
        for (int i = 0; i < MAX_RESOURCES; i++) {
            if (resources[i].in_use)
                ESP_LOGI(TAG, "  Resource %d BUSY by %s", i + 1, resources[i].current_user);
            else
                ESP_LOGI(TAG, "  Resource %d FREE", i + 1);
        }
        printf("Pool: [");
        for (int i = 0; i < MAX_RESOURCES; i++) printf(resources[i].in_use ? "■" : "□");
        printf("]\n");
    }
}

void statistics_task(void *pvParameters) {
    ESP_LOGI(TAG, "Statistics task started");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(12000));
        ESP_LOGI(TAG, "\n📈 SYSTEM STATISTICS");
        ESP_LOGI(TAG, "Total requests: %lu", stats.total_requests);
        ESP_LOGI(TAG, "Successful: %lu", stats.successful_acquisitions);
        ESP_LOGI(TAG, "Failed: %lu", stats.failed_acquisitions);
        if (stats.total_requests)
            ESP_LOGI(TAG, "Success rate: %.1f%%",
                     (float)stats.successful_acquisitions / stats.total_requests * 100);
        ESP_LOGI(TAG, "Resources in use: %lu", stats.resources_in_use);
    }
}

void load_generator_task(void *pvParameters) {
    ESP_LOGI(TAG, "Load generator started");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20000));
        ESP_LOGW(TAG, "🚀 LOAD BURST START");
        gpio_set_level(LED_SYSTEM, 1);

        for (int i = 0; i < MAX_RESOURCES + 2; i++) {
            if (xSemaphoreTake(xCountingSemaphore, pdMS_TO_TICKS(100)) == pdTRUE) {
                int res = acquire_resource("LoadGen");
                if (res >= 0) {
                    vTaskDelay(pdMS_TO_TICKS(500));
                    release_resource(res, 500);
                }
                xSemaphoreGive(xCountingSemaphore);
            } else {
                ESP_LOGW(TAG, "LoadGen: Pool exhausted");
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        gpio_set_level(LED_SYSTEM, 0);
        ESP_LOGI(TAG, "LOAD BURST COMPLETE\n");
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Counting Semaphores Lab Starting...");

    gpio_set_direction(LED_RESOURCE_1, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_RESOURCE_2, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_RESOURCE_3, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_PRODUCER, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_SYSTEM, GPIO_MODE_OUTPUT);

    gpio_set_level(LED_RESOURCE_1, 0);
    gpio_set_level(LED_RESOURCE_2, 0);
    gpio_set_level(LED_RESOURCE_3, 0);
    gpio_set_level(LED_PRODUCER, 0);
    gpio_set_level(LED_SYSTEM, 0);

    xCountingSemaphore = xSemaphoreCreateCounting(MAX_RESOURCES, MAX_RESOURCES);

    if (xCountingSemaphore != NULL) {
        static int producer_ids[NUM_PRODUCERS] = {1, 2, 3, 4, 5};
        for (int i = 0; i < NUM_PRODUCERS; i++) {
            char name[16];
            snprintf(name, sizeof(name), "Producer%d", i + 1);
            xTaskCreate(producer_task, name, 3072, &producer_ids[i], 3, NULL);
        }
        xTaskCreate(resource_monitor_task, "ResMonitor", 3072, NULL, 2, NULL);
        xTaskCreate(statistics_task, "Stats", 3072, NULL, 1, NULL);
        xTaskCreate(load_generator_task, "LoadGen", 3072, NULL, 4, NULL);

        ESP_LOGI(TAG, "System operational with %d resources, %d producers", 
                 MAX_RESOURCES, NUM_PRODUCERS);
    } else {
        ESP_LOGE(TAG, "Failed to create counting semaphore!");
    }
}