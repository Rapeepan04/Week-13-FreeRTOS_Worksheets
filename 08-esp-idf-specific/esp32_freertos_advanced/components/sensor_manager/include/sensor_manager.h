#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"

typedef struct {
    float temperature;
    float humidity;
    uint32_t timestamp;
} sensor_data_t;

esp_err_t sensor_manager_init(void);
esp_err_t sensor_manager_start(void);
QueueHandle_t sensor_manager_get_data_queue(void);