#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_random.h"

static const char *TAG = "PROD_CONS";

// LED pins
#define LED_PRODUCER_1 GPIO_NUM_2
#define LED_PRODUCER_2 GPIO_NUM_4
#define LED_PRODUCER_3 GPIO_NUM_5
#define LED_CONSUMER_1 GPIO_NUM_18
#define LED_CONSUMER_2 GPIO_NUM_19

QueueHandle_t xProductQueue;
SemaphoreHandle_t xPrintMutex;

typedef struct {
    uint32_t produced;
    uint32_t consumed;
    uint32_t dropped;
} stats_t;

stats_t global_stats = {0, 0, 0};

typedef struct {
    int producer_id;
    int product_id;
    char product_name[30];
    uint32_t production_time;
    int processing_time_ms;
} product_t;

void safe_printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    if (xSemaphoreTake(xPrintMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        vprintf(format, args);
        xSemaphoreGive(xPrintMutex);
    }
    va_end(args);
}

void producer_task(void *pvParameters) {
    int producer_id = *((int*)pvParameters);
    product_t product;
    int product_counter = 0;
    gpio_num_t led_pin = (producer_id == 1) ? LED_PRODUCER_1 :
                         (producer_id == 2) ? LED_PRODUCER_2 :
                         (producer_id == 3) ? LED_PRODUCER_3 : LED_PRODUCER_1;

    safe_printf("Producer %d started\n", producer_id);

    while (1) {
        product.producer_id = producer_id;
        product.product_id = product_counter++;
        snprintf(product.product_name, sizeof(product.product_name),
                 "Product-P%d-#%d", producer_id, product.product_id);
        product.production_time = xTaskGetTickCount();
        product.processing_time_ms = 500 + (esp_random() % 2000);

        BaseType_t xStatus = xQueueSend(xProductQueue, &product, pdMS_TO_TICKS(100));
        if (xStatus == pdPASS) {
            global_stats.produced++;
            safe_printf("✓ Producer %d: Created %s (processing %dms)\n",
                        producer_id, product.product_name, product.processing_time_ms);
            gpio_set_level(led_pin, 1);
            vTaskDelay(pdMS_TO_TICKS(50));
            gpio_set_level(led_pin, 0);
        } else {
            global_stats.dropped++;
            safe_printf("✗ Producer %d: Queue full → Dropped %s\n",
                        producer_id, product.product_name);
        }

        int delay = 1000 + (esp_random() % 2000);
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}

void consumer_task(void *pvParameters) {
    int consumer_id = *((int*)pvParameters);
    product_t product;
    gpio_num_t led_pin = (consumer_id == 1) ? LED_CONSUMER_1 : LED_CONSUMER_2;

    safe_printf("Consumer %d started\n", consumer_id);

    while (1) {
        BaseType_t xStatus = xQueueReceive(xProductQueue, &product, pdMS_TO_TICKS(5000));

        if (xStatus == pdPASS) {
            global_stats.consumed++;
            uint32_t queue_time = xTaskGetTickCount() - product.production_time;
            safe_printf("→ Consumer %d: Processing %s (queue time %lums)\n",
                        consumer_id, product.product_name,
                        queue_time * portTICK_PERIOD_MS);

            gpio_set_level(led_pin, 1);
            vTaskDelay(pdMS_TO_TICKS(product.processing_time_ms));
            gpio_set_level(led_pin, 0);

            safe_printf("✓ Consumer %d: Finished %s\n",
                        consumer_id, product.product_name);
        } else {
            safe_printf("⏰ Consumer %d: Timeout – no product to process\n", consumer_id);
        }
    }
}

void statistics_task(void *pvParameters) {
    safe_printf("Statistics task started\n");
    while (1) {
        UBaseType_t q_items = uxQueueMessagesWaiting(xProductQueue);
        safe_printf("\n═══ SYSTEM STATS ═══\n");
        safe_printf("Produced: %lu\n", global_stats.produced);
        safe_printf("Consumed: %lu\n", global_stats.consumed);
        safe_printf("Dropped:  %lu\n", global_stats.dropped);
        safe_printf("Queue backlog: %d\n", q_items);
        safe_printf("Efficiency: %.1f%%\n",
                    global_stats.produced ?
                    (float)global_stats.consumed / global_stats.produced * 100 : 0);

        printf("Queue: [");
        for (int i = 0; i < 10; i++) printf(i < q_items ? "■" : "□");
        printf("]\n═════════════════════\n\n");

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void load_balancer_task(void *pvParameters) {
    const int MAX_QUEUE_SIZE = 8;
    safe_printf("Load balancer started\n");
    while (1) {
        UBaseType_t q_items = uxQueueMessagesWaiting(xProductQueue);
        if (q_items > MAX_QUEUE_SIZE) {
            safe_printf("⚠️  HIGH LOAD! Queue=%d\n", q_items);
            safe_printf("💡 Suggest adding more consumers\n");
            gpio_set_level(LED_PRODUCER_1, 1);
            gpio_set_level(LED_PRODUCER_2, 1);
            gpio_set_level(LED_PRODUCER_3, 1);
            gpio_set_level(LED_CONSUMER_1, 1);
            gpio_set_level(LED_CONSUMER_2, 1);
            vTaskDelay(pdMS_TO_TICKS(200));
            gpio_set_level(LED_PRODUCER_1, 0);
            gpio_set_level(LED_PRODUCER_2, 0);
            gpio_set_level(LED_PRODUCER_3, 0);
            gpio_set_level(LED_CONSUMER_1, 0);
            gpio_set_level(LED_CONSUMER_2, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "=== Producer-Consumer System Starting ===");

    gpio_set_direction(LED_PRODUCER_1, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_PRODUCER_2, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_PRODUCER_3, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_CONSUMER_1, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_CONSUMER_2, GPIO_MODE_OUTPUT);

    gpio_set_level(LED_PRODUCER_1, 0);
    gpio_set_level(LED_PRODUCER_2, 0);
    gpio_set_level(LED_PRODUCER_3, 0);
    gpio_set_level(LED_CONSUMER_1, 0);
    gpio_set_level(LED_CONSUMER_2, 0);

    xProductQueue = xQueueCreate(10, sizeof(product_t));
    xPrintMutex = xSemaphoreCreateMutex();

    if (xProductQueue && xPrintMutex) {
        ESP_LOGI(TAG, "Queue & Mutex created successfully");

        static int p1=1,p2=2,p3=3,c1=1,c2=2;
        xTaskCreate(producer_task, "Producer1", 3072, &p1, 3, NULL);
        xTaskCreate(producer_task, "Producer2", 3072, &p2, 3, NULL);
        xTaskCreate(producer_task, "Producer3", 3072, &p3, 3, NULL);
        xTaskCreate(consumer_task, "Consumer1", 3072, &c1, 2, NULL);
        xTaskCreate(consumer_task, "Consumer2", 3072, &c2, 2, NULL);
        xTaskCreate(statistics_task, "Stats", 3072, NULL, 1, NULL);
        xTaskCreate(load_balancer_task, "Balancer", 2048, NULL, 1, NULL);
    } else {
        ESP_LOGE(TAG, "Failed to create queue or mutex!");
    }
}