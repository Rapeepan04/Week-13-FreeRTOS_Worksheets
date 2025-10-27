#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/gpio.h"

static const char *TAG = "QUEUE_LAB";

// LED Pins
#define LED_SENDER   GPIO_NUM_2
#define LED_RECEIVER GPIO_NUM_4

// Queue handle
QueueHandle_t xQueue;

// Data structure for messages
typedef struct {
    int id;
    char message[50];
    uint32_t timestamp;
} queue_message_t;

// Sender Task
void sender_task(void *pvParameters)
{
    queue_message_t message;
    int counter = 0;
    ESP_LOGI(TAG, "Sender task started");

    while (1) {
        message.id = counter++;
        snprintf(message.message, sizeof(message.message),
                 "Hello from sender #%d", message.id);
        message.timestamp = xTaskGetTickCount();

        BaseType_t status = xQueueSend(xQueue, &message, pdMS_TO_TICKS(1000));

        if (status == pdPASS) {
            ESP_LOGI(TAG, "Sent → ID=%d | MSG=%s | Time=%lu",
                     message.id, message.message, message.timestamp);
            gpio_set_level(LED_SENDER, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(LED_SENDER, 0);
        } else {
            ESP_LOGW(TAG, "Queue full! Message ID=%d dropped", message.id);
        }

        vTaskDelay(pdMS_TO_TICKS(2000));  // Send every 2s
    }
}

// Receiver Task
void receiver_task(void *pvParameters)
{
    queue_message_t received_message;
    ESP_LOGI(TAG, "Receiver task started");

    while (1) {
        BaseType_t status = xQueueReceive(xQueue, &received_message, pdMS_TO_TICKS(5000));

        if (status == pdPASS) {
            ESP_LOGI(TAG, "Received ← ID=%d | MSG=%s | Time=%lu",
                     received_message.id,
                     received_message.message,
                     received_message.timestamp);
            gpio_set_level(LED_RECEIVER, 1);
            vTaskDelay(pdMS_TO_TICKS(200));
            gpio_set_level(LED_RECEIVER, 0);
            vTaskDelay(pdMS_TO_TICKS(1500));  // Simulate processing
        } else {
            ESP_LOGW(TAG, "No message received (timeout)");
        }
    }
}

// Queue Monitor Task
void queue_monitor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Queue monitor started");

    while (1) {
        UBaseType_t messages = uxQueueMessagesWaiting(xQueue);
        UBaseType_t spaces = uxQueueSpacesAvailable(xQueue);

        ESP_LOGI(TAG, "Queue status → messages: %d | free spaces: %d", messages, spaces);

        printf("Queue: [");
        for (int i = 0; i < 5; i++) {
            if (i < messages)
                printf("■");
            else
                printf("□");
        }
        printf("]\n");

        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== FreeRTOS Basic Queue Demo ===");

    gpio_set_direction(LED_SENDER, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_RECEIVER, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_SENDER, 0);
    gpio_set_level(LED_RECEIVER, 0);

    // Create queue
    xQueue = xQueueCreate(5, sizeof(queue_message_t));
    if (xQueue == NULL) {
        ESP_LOGE(TAG, "Failed to create queue!");
        return;
    }

    ESP_LOGI(TAG, "Queue created successfully (5 messages capacity)");

    // Create tasks
    xTaskCreate(sender_task, "Sender", 2048, NULL, 2, NULL);
    xTaskCreate(receiver_task, "Receiver", 2048, NULL, 1, NULL);
    xTaskCreate(queue_monitor_task, "Monitor", 2048, NULL, 1, NULL);

    ESP_LOGI(TAG, "All tasks created. Queue demo running...");
}