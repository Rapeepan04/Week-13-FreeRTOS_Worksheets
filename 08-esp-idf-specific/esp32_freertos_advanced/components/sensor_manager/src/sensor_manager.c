#include "sensor_manager.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "SENSOR_MGR";

static QueueHandle_t sensor_data_queue = NULL;
static TaskHandle_t sensor_task_handle = NULL;

// Simulated sensor task
void sensor_task(void *parameter)
{
    sensor_data_t data;

    while (1)
    {
        data.temperature = 25.0 + (rand() % 200) / 10.0; // 25–45°C
        data.humidity = 40.0 + (rand() % 500) / 10.0;    // 40–90%
        data.timestamp = xTaskGetTickCount();

        if (xQueueSend(sensor_data_queue, &data, pdMS_TO_TICKS(100)) != pdTRUE)
        {
            ESP_LOGW(TAG, "Sensor data queue full");
        }

        ESP_LOGI(TAG, "Temp: %.1f°C  Humidity: %.1f%%  (Core %d)",
                 data.temperature, data.humidity, xPortGetCoreID());

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

esp_err_t sensor_manager_init(void)
{
    sensor_data_queue = xQueueCreate(10, sizeof(sensor_data_t));
    if (sensor_data_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create data queue");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Sensor manager initialized");
    return ESP_OK;
}

esp_err_t sensor_manager_start(void)
{
    if (xTaskCreatePinnedToCore(sensor_task, "SensorTask", 3072, NULL, 5, &sensor_task_handle, 1) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create sensor task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Sensor manager started on Core 1");
    return ESP_OK;
}

QueueHandle_t sensor_manager_get_data_queue(void)
{
    return sensor_data_queue;
}