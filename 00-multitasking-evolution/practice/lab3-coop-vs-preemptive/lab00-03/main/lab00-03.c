#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#define LED1_PIN GPIO_NUM_2   // Task 1 indicator
    #define LED2_PIN GPIO_NUM_4   // Task 2 indicator
#define LED3_PIN GPIO_NUM_5   // Emergency indicator
#define BUTTON_PIN GPIO_NUM_0 // Emergency button

// ===========================================================
// =============== Cooperative Multitasking =================
// ===========================================================

static const char *TAG_COOP = "COOPERATIVE";

static volatile bool emergency_flag = false;
static uint64_t task_start_time = 0;
static uint32_t max_response_time = 0;

typedef struct {
    void (*task_function)(void);
    const char* name;
    bool ready;
} coop_task_t;

void cooperative_task1(void)
{
    static uint32_t count = 0;
    ESP_LOGI(TAG_COOP, "Coop Task1 running: %d", count++);
    gpio_set_level(LED1_PIN, 1);

    for (int i = 0; i < 5; i++) {
        for (int j = 0; j < 50000; j++) { volatile int dummy = j * 2; }
        if (emergency_flag) {
            ESP_LOGW(TAG_COOP, "Task1 yielding for emergency");
            gpio_set_level(LED1_PIN, 0);
            return;
        }
        vTaskDelay(1);
    }
    gpio_set_level(LED1_PIN, 0);
}

void cooperative_task2(void)
{
    static uint32_t count = 0;
    ESP_LOGI(TAG_COOP, "Coop Task2 running: %d", count++);
    gpio_set_level(LED2_PIN, 1);

    for (int i = 0; i < 10; i++) {
        for (int j = 0; j < 30000; j++) { volatile int dummy = j + i; }
        if (emergency_flag) {
            ESP_LOGW(TAG_COOP, "Task2 yielding for emergency");
            gpio_set_level(LED2_PIN, 0);
            return;
        }
        vTaskDelay(1);
    }
    gpio_set_level(LED2_PIN, 0);
}

void cooperative_task3_emergency(void)
{
    if (emergency_flag) {
        uint64_t response_time = esp_timer_get_time() - task_start_time;
        uint32_t response_ms = (uint32_t)(response_time / 1000);

        if (response_ms > max_response_time)
            max_response_time = response_ms;

        ESP_LOGW(TAG_COOP, "EMERGENCY RESPONSE! Response time: %d ms (Max: %d ms)",
                 response_ms, max_response_time);

        gpio_set_level(LED3_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(200));
        gpio_set_level(LED3_PIN, 0);

        emergency_flag = false;
    }
}

void cooperative_scheduler(void)
{
    coop_task_t tasks[] = {
        {cooperative_task1, "Task1", true},
        {cooperative_task2, "Task2", true},
        {cooperative_task3_emergency, "Emergency", true}
    };
    int num_tasks = sizeof(tasks) / sizeof(tasks[0]);
    int current_task = 0;

    while (1) {
        if (gpio_get_level(BUTTON_PIN) == 0 && !emergency_flag) {
            emergency_flag = true;
            task_start_time = esp_timer_get_time();
            ESP_LOGW(TAG_COOP, "Emergency button pressed!");
        }

        if (tasks[current_task].ready)
            tasks[current_task].task_function();

        current_task = (current_task + 1) % num_tasks;
        vTaskDelay(pdMS_TO_TICKS(10)); // prevent watchdog
    }
}

void test_cooperative_multitasking(void)
{
    ESP_LOGI(TAG_COOP, "=== Cooperative Multitasking Demo ===");
    ESP_LOGI(TAG_COOP, "Tasks yield voluntarily");
    ESP_LOGI(TAG_COOP, "Press button to test emergency response");
    cooperative_scheduler();
}

// ===========================================================
// ================ Preemptive Multitasking ==================
// ===========================================================

static const char *TAG_PREEMPT = "PREEMPTIVE";
static volatile bool preempt_emergency = false;
static uint64_t preempt_start_time = 0;
static uint32_t preempt_max_response = 0;

void preemptive_task1(void *pvParameters)
{
    uint32_t count = 0;
    while (1) {
        ESP_LOGI(TAG_PREEMPT, "Preempt Task1: %d", count++);
        gpio_set_level(LED1_PIN, 1);

        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 50000; j++) { volatile int dummy = j * 2; }

        gpio_set_level(LED1_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void preemptive_task2(void *pvParameters)
{
    uint32_t count = 0;
    while (1) {
        ESP_LOGI(TAG_PREEMPT, "Preempt Task2: %d", count++);
        gpio_set_level(LED2_PIN, 1);

        for (int i = 0; i < 20; i++)
            for (int j = 0; j < 30000; j++) { volatile int dummy = j + i; }

        gpio_set_level(LED2_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

void preemptive_emergency_task(void *pvParameters)
{
    while (1) {
        if (gpio_get_level(BUTTON_PIN) == 0 && !preempt_emergency) {
            preempt_emergency = true;
            preempt_start_time = esp_timer_get_time();

            uint64_t response_time = esp_timer_get_time() - preempt_start_time;
            uint32_t response_ms = (uint32_t)(response_time / 1000);

            if (response_ms > preempt_max_response)
                preempt_max_response = response_ms;

            ESP_LOGW(TAG_PREEMPT, "IMMEDIATE EMERGENCY! Response: %d ms (Max: %d ms)",
                     response_ms, preempt_max_response);

            gpio_set_level(LED3_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(200));
            gpio_set_level(LED3_PIN, 0);

            preempt_emergency = false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void test_preemptive_multitasking(void)
{
    ESP_LOGI(TAG_PREEMPT, "=== Preemptive Multitasking Demo ===");
    ESP_LOGI(TAG_PREEMPT, "RTOS will preempt tasks automatically");
    ESP_LOGI(TAG_PREEMPT, "Press button to test emergency response");

    xTaskCreate(preemptive_task1, "PreTask1", 2048, NULL, 2, NULL);
    xTaskCreate(preemptive_task2, "PreTask2", 2048, NULL, 1, NULL);
    xTaskCreate(preemptive_emergency_task, "Emergency", 2048, NULL, 5, NULL);

    vTaskDelete(NULL);
}

// ===========================================================
// ======================== MAIN =============================
// ===========================================================

void app_main(void)
{
    // GPIO setup
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED1_PIN) | (1ULL << LED2_PIN) | (1ULL << LED3_PIN),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&io_conf);

    // Button setup
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = 1ULL << BUTTON_PIN;
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);

    ESP_LOGI("MAIN", "Multitasking Comparison Demo");
    ESP_LOGI("MAIN", "Choose test mode:");
    ESP_LOGI("MAIN", "1. Cooperative (uncomment line below)");
    ESP_LOGI("MAIN", "2. Preemptive (default mode)");

    // 🔁 เลือกโหมดทดสอบ (สลับ comment ตรงนี้)
    // test_cooperative_multitasking();  // Cooperative mode
    test_preemptive_multitasking();       // Preemptive mode
}