#include <stdio.h>
#include <stdint.h>   // ✅ สำหรับ intptr_t
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#define LED_HIGH_PIN GPIO_NUM_2
#define LED_MED_PIN  GPIO_NUM_4
#define LED_LOW_PIN  GPIO_NUM_5
#define BUTTON_PIN   GPIO_NUM_0

static const char *TAG = "PRIORITY_DEMO";

volatile uint32_t high_task_count = 0;
volatile uint32_t med_task_count = 0;
volatile uint32_t low_task_count = 0;
volatile bool priority_test_running = false;

// ===================== 🟥 High Priority =====================
void high_priority_task(void *pvParameters)
{
    ESP_LOGI(TAG, "High Priority Task started (Priority 5)");
    while (1)
    {
        if (priority_test_running)
        {
            high_task_count++;
            ESP_LOGI(TAG, "HIGH PRIORITY RUNNING (%d)", high_task_count);
            gpio_set_level(LED_HIGH_PIN, 1);
            for (int i = 0; i < 100000; i++)
            {
                volatile int dummy = i * 2;
                (void)dummy;
            }
            gpio_set_level(LED_HIGH_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

// ===================== 🟧 Medium Priority =====================
void medium_priority_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Medium Priority Task started (Priority 3)");
    while (1)
    {
        if (priority_test_running)
        {
            med_task_count++;
            ESP_LOGI(TAG, "Medium priority running (%d)", med_task_count);
            gpio_set_level(LED_MED_PIN, 1);
            for (int i = 0; i < 200000; i++)
            {
                volatile int dummy = i + 100;
                (void)dummy;
            }
            gpio_set_level(LED_MED_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

// ===================== 🟩 Low Priority =====================
void low_priority_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Low Priority Task started (Priority 1)");
    while (1)
    {
        if (priority_test_running)
        {
            low_task_count++;
            ESP_LOGI(TAG, "Low priority running (%d)", low_task_count);
            gpio_set_level(LED_LOW_PIN, 1);
            for (int i = 0; i < 500000; i++)
            {
                volatile int dummy = i - 50;
                (void)dummy;
                if (i % 100000 == 0)
                    vTaskDelay(1);
            }
            gpio_set_level(LED_LOW_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

// ===================== 🟦 Control Task =====================
void control_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Control Task started");
    while (1)
    {
        if (gpio_get_level(BUTTON_PIN) == 0 && !priority_test_running)
        {
            ESP_LOGW(TAG, "=== STARTING PRIORITY TEST ===");
            high_task_count = med_task_count = low_task_count = 0;
            priority_test_running = true;

            vTaskDelay(pdMS_TO_TICKS(10000)); // run 10s
            priority_test_running = false;

            ESP_LOGW(TAG, "=== PRIORITY TEST RESULTS ===");
            uint32_t total = high_task_count + med_task_count + low_task_count;
            if (total > 0)
            {
                ESP_LOGI(TAG, "High: %.1f%%", (float)high_task_count / total * 100);
                ESP_LOGI(TAG, "Medium: %.1f%%", (float)med_task_count / total * 100);
                ESP_LOGI(TAG, "Low: %.1f%%", (float)low_task_count / total * 100);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ===================== 🟨 Equal Priority Tasks (Round-Robin) =====================
void equal_priority_task(void *pvParameters)
{
    int task_id = (intptr_t)pvParameters;
    while (1)
    {
        if (priority_test_running)
        {
            ESP_LOGI(TAG, "Equal Priority Task %d running", task_id);
            for (int i = 0; i < 300000; i++)
            {
                volatile int dummy = i;
                (void)dummy;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50)); // yield for round-robin
    }
}

// ===================== 🧩 Priority Inversion Demo =====================
volatile bool shared_resource_busy = false;

void priority_inversion_high(void *pvParameters)
{
    while (1)
    {
        if (priority_test_running)
        {
            ESP_LOGW(TAG, "High priority task needs shared resource");
            while (shared_resource_busy)
            {
                ESP_LOGW(TAG, "High priority BLOCKED by low priority!");
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            shared_resource_busy = true;
            ESP_LOGI(TAG, "High priority got resource");
            vTaskDelay(pdMS_TO_TICKS(200));
            shared_resource_busy = false;
            ESP_LOGI(TAG, "High priority released resource");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void priority_inversion_low(void *pvParameters)
{
    while (1)
    {
        if (priority_test_running)
        {
            ESP_LOGI(TAG, "Low priority using shared resource");
            shared_resource_busy = true;
            vTaskDelay(pdMS_TO_TICKS(2000));
            shared_resource_busy = false;
            ESP_LOGI(TAG, "Low priority released resource");
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

// ===================== 🧠 app_main() =====================
void app_main(void)
{
    ESP_LOGI(TAG, "=== FreeRTOS Priority Scheduling Demo ===");

    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED_HIGH_PIN) | (1ULL << LED_MED_PIN) | (1ULL << LED_LOW_PIN),
        .pull_down_en = 0,
        .pull_up_en = 0};
    gpio_config(&io_conf);

    gpio_config_t button_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = 1ULL << BUTTON_PIN,
        .pull_up_en = 1,
        .pull_down_en = 0};
    gpio_config(&button_conf);

    ESP_LOGI(TAG, "Creating tasks...");

    // Step 1: Priority Demo
    xTaskCreate(high_priority_task, "HighPrio", 3072, NULL, 5, NULL);
    xTaskCreate(medium_priority_task, "MedPrio", 3072, NULL, 3, NULL);
    xTaskCreate(low_priority_task, "LowPrio", 3072, NULL, 1, NULL);
    xTaskCreate(control_task, "Control", 3072, NULL, 4, NULL);

    // Step 2: Round-Robin Tasks (Priority 2)
    xTaskCreate(equal_priority_task, "Equal1", 2048, (void *)1, 2, NULL);
    xTaskCreate(equal_priority_task, "Equal2", 2048, (void *)2, 2, NULL);
    xTaskCreate(equal_priority_task, "Equal3", 2048, (void *)3, 2, NULL);

    // Step 3: Priority Inversion Simulation
    xTaskCreate(priority_inversion_high, "InvHigh", 3072, NULL, 5, NULL);
    xTaskCreate(priority_inversion_low, "InvLow", 3072, NULL, 1, NULL);

    ESP_LOGI(TAG, "Press button to start test");
    ESP_LOGI(TAG, "LEDs: GPIO2=High, GPIO4=Med, GPIO5=Low");
}