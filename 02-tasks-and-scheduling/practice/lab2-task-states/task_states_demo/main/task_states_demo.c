#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define LED_RUNNING GPIO_NUM_2
#define LED_READY GPIO_NUM_4
#define LED_BLOCKED GPIO_NUM_5
#define LED_SUSPENDED GPIO_NUM_18

#define BUTTON1_PIN GPIO_NUM_0
#define BUTTON2_PIN GPIO_NUM_35

static const char *TAG = "TASK_STATES";

TaskHandle_t state_demo_task_handle = NULL;
TaskHandle_t control_task_handle = NULL;

SemaphoreHandle_t demo_semaphore = NULL;

const char* state_names[] = {
    "Running", "Ready", "Blocked", "Suspended", "Deleted", "Invalid"
};

const char* get_state_name(eTaskState state)
{
    if (state <= eDeleted) return state_names[state];
    return state_names[5];
}

// ================== Task States Demonstration ==================
void state_demo_task(void *pvParameters)
{
    ESP_LOGI(TAG, "State Demo Task started");
    int cycle = 0;

    while (1)
    {
        cycle++;
        ESP_LOGI(TAG, "=== Cycle %d ===", cycle);

        // Running
        ESP_LOGI(TAG, "Task is RUNNING");
        gpio_set_level(LED_RUNNING, 1);
        gpio_set_level(LED_READY, 0);
        gpio_set_level(LED_BLOCKED, 0);
        gpio_set_level(LED_SUSPENDED, 0);

        for (int i = 0; i < 1000000; i++)
        {
            volatile int dummy = i * 2;
            (void)dummy;
        }

        // Ready
        ESP_LOGI(TAG, "Task will be READY (yield)");
        gpio_set_level(LED_RUNNING, 0);
        gpio_set_level(LED_READY, 1);
        taskYIELD();
        vTaskDelay(pdMS_TO_TICKS(100));

        // Blocked (waiting for semaphore)
        ESP_LOGI(TAG, "Task will be BLOCKED (waiting semaphore)");
        gpio_set_level(LED_READY, 0);
        gpio_set_level(LED_BLOCKED, 1);

        if (xSemaphoreTake(demo_semaphore, pdMS_TO_TICKS(2000)) == pdTRUE)
        {
            ESP_LOGI(TAG, "Got semaphore! RUNNING again");
            gpio_set_level(LED_BLOCKED, 0);
            gpio_set_level(LED_RUNNING, 1);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        else
        {
            ESP_LOGI(TAG, "Semaphore timeout!");
            gpio_set_level(LED_BLOCKED, 0);
        }

        // Blocked in delay
        ESP_LOGI(TAG, "Task BLOCKED (vTaskDelay)");
        gpio_set_level(LED_RUNNING, 0);
        gpio_set_level(LED_BLOCKED, 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
        gpio_set_level(LED_BLOCKED, 0);
    }
}

// Ready-state demo task (same priority)
void ready_state_demo_task(void *pvParameters)
{
    while (1)
    {
        ESP_LOGI(TAG, "Ready demo running");
        for (int i = 0; i < 100000; i++)
        {
            volatile int dummy = i;
            (void)dummy;
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

// ================== Control Task ==================
void control_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Control Task started");

    bool suspended = false;
    int cycle = 0;
    static bool external_deleted = false;

    while (1)
    {
        cycle++;

        // Button 1: Suspend/Resume
        if (gpio_get_level(BUTTON1_PIN) == 0)
        {
            vTaskDelay(pdMS_TO_TICKS(50));
            if (!suspended)
            {
                ESP_LOGW(TAG, "=== SUSPENDING ===");
                vTaskSuspend(state_demo_task_handle);
                gpio_set_level(LED_SUSPENDED, 1);
                gpio_set_level(LED_RUNNING, 0);
                gpio_set_level(LED_READY, 0);
                gpio_set_level(LED_BLOCKED, 0);
                suspended = true;
            }
            else
            {
                ESP_LOGW(TAG, "=== RESUMING ===");
                vTaskResume(state_demo_task_handle);
                gpio_set_level(LED_SUSPENDED, 0);
                suspended = false;
            }
            while (gpio_get_level(BUTTON1_PIN) == 0)
                vTaskDelay(pdMS_TO_TICKS(10));
        }

        // Button 2: Give semaphore
        if (gpio_get_level(BUTTON2_PIN) == 0)
        {
            vTaskDelay(pdMS_TO_TICKS(50));
            ESP_LOGW(TAG, "=== GIVING SEMAPHORE ===");
            xSemaphoreGive(demo_semaphore);
            while (gpio_get_level(BUTTON2_PIN) == 0)
                vTaskDelay(pdMS_TO_TICKS(10));
        }

        // Every 3 seconds: monitor
        if (cycle % 30 == 0)
        {
            ESP_LOGI(TAG, "=== TASK STATUS ===");
            eTaskState st = eTaskGetState(state_demo_task_handle);
            ESP_LOGI(TAG, "State Demo: %s", get_state_name(st));
            UBaseType_t prio = uxTaskPriorityGet(state_demo_task_handle);
            UBaseType_t stack = uxTaskGetStackHighWaterMark(state_demo_task_handle);
            ESP_LOGI(TAG, "Priority: %d, Stack: %d bytes", prio, stack * sizeof(StackType_t));
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ================== System Monitor ==================
void system_monitor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "System Monitor started");

    char *task_list = malloc(1024);
    char *stats = malloc(1024);
    if (!task_list || !stats)
    {
        ESP_LOGE(TAG, "Malloc failed");
        vTaskDelete(NULL);
        return;
    }

    while (1)
    {
        ESP_LOGI(TAG, "\n=== SYSTEM MONITOR ===");
        vTaskList(task_list);
        ESP_LOGI(TAG, "Task\t\tState\tPrio\tStack\tNum");
        ESP_LOGI(TAG, "%s", task_list);
        vTaskGetRunTimeStats(stats);
        ESP_LOGI(TAG, "\nRuntime Stats:\nTask\t\tAbs Time\t%%Time");
        ESP_LOGI(TAG, "%s", stats);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    free(task_list);
    free(stats);
}

// ================== Self Delete / External Delete ==================
void self_deleting_task(void *pvParameters)
{
    int *lifetime = (int *)pvParameters;
    ESP_LOGI(TAG, "Self-deleting task alive for %d sec", *lifetime);

    for (int i = *lifetime; i > 0; i--)
    {
        ESP_LOGI(TAG, "Self-deleting countdown: %d", i);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "Self-deleting now (DELETED)");
    vTaskDelete(NULL);
}

void external_delete_task(void *pvParameters)
{
    int count = 0;
    while (1)
    {
        ESP_LOGI(TAG, "External delete task: %d", count++);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ================== Monitor Specific Tasks ==================
void monitor_task_states(void)
{
    ESP_LOGI(TAG, "=== DETAILED TASK STATE MONITOR ===");
    TaskHandle_t tasks[] = {state_demo_task_handle, control_task_handle};
    const char* names[] = {"StateDemo", "Control"};
    int n = sizeof(tasks)/sizeof(tasks[0]);

    for (int i = 0; i < n; i++)
    {
        if (tasks[i])
        {
            eTaskState st = eTaskGetState(tasks[i]);
            UBaseType_t pr = uxTaskPriorityGet(tasks[i]);
            UBaseType_t st_remain = uxTaskGetStackHighWaterMark(tasks[i]);
            ESP_LOGI(TAG, "%s: %s | Priority=%d | Stack=%d bytes",
                     names[i], get_state_name(st), pr, st_remain * sizeof(StackType_t));
        }
    }
}

// ================== Main ==================
void app_main(void)
{
    ESP_LOGI(TAG, "=== FreeRTOS Task States Demo ===");

    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED_RUNNING) | (1ULL << LED_READY) |
                        (1ULL << LED_BLOCKED) | (1ULL << LED_SUSPENDED),
        .pull_down_en = 0,
        .pull_up_en = 0
    };
    gpio_config(&io_conf);

    gpio_config_t button_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BUTTON1_PIN) | (1ULL << BUTTON2_PIN),
        .pull_up_en = 1,
        .pull_down_en = 0
    };
    gpio_config(&button_conf);

    demo_semaphore = xSemaphoreCreateBinary();
    if (demo_semaphore == NULL)
    {
        ESP_LOGE(TAG, "Failed to create semaphore");
        return;
    }

    ESP_LOGI(TAG, "LEDs: 2=Running, 4=Ready, 5=Blocked, 18=Suspended");
    ESP_LOGI(TAG, "Buttons: 0=Suspend/Resume, 35=Give Semaphore");

    xTaskCreate(state_demo_task, "StateDemo", 4096, NULL, 3, &state_demo_task_handle);
    xTaskCreate(ready_state_demo_task, "ReadyDemo", 2048, NULL, 3, NULL);
    xTaskCreate(control_task, "Control", 3072, NULL, 4, &control_task_handle);
    xTaskCreate(system_monitor_task, "Monitor", 4096, NULL, 1, NULL);

    static int self_delete_time = 10;
    TaskHandle_t ext_delete_handle = NULL;
    xTaskCreate(self_deleting_task, "SelfDelete", 2048, &self_delete_time, 2, NULL);
    xTaskCreate(external_delete_task, "ExtDelete", 2048, NULL, 2, &ext_delete_handle);

    // ลบ external task หลัง 15 วินาที
    vTaskDelay(pdMS_TO_TICKS(15000));
    ESP_LOGW(TAG, "Deleting external task...");
    vTaskDelete(ext_delete_handle);

    ESP_LOGI(TAG, "All tasks created. Monitoring task states...");
}