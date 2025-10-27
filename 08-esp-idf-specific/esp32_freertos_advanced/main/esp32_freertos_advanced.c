#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "esp_ipc.h"
#include "sensor_manager.h"   // Component

static const char *TAG = "APP_ORCH";

// -------- Function Prototypes ----------
void core_info_task(void *parameter);
void hardware_integration_example(void);

// Simple demo task showing core distribution
void core_info_task(void *parameter)
{
    int core = xPortGetCoreID();
    ESP_LOGI(TAG, "Task running on Core %d", core);

    while (1)
    {
        ESP_LOGI(TAG, "Core %d alive", core);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32 FreeRTOS SMP Demo ===");

    // Task distribution demo
    xTaskCreate(core_info_task, "TaskA", 4096, NULL, 5, NULL);
    xTaskCreate(core_info_task, "TaskB", 4096, NULL, 5, NULL);

    // Hardware Integration Example
    hardware_integration_example();

    // Sensor Manager Component
    if (sensor_manager_init() == ESP_OK)
    {
        sensor_manager_start();
    }

    ESP_LOGI(TAG, "System running. Observe logs.");
}