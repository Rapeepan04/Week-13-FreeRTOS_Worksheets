#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "driver/gpio.h"

static const char *TAG = "EVENT_SYNC";

#define LED_BARRIER_SYNC    GPIO_NUM_2
#define LED_PIPELINE_STAGE1 GPIO_NUM_4
#define LED_PIPELINE_STAGE2 GPIO_NUM_5
#define LED_PIPELINE_STAGE3 GPIO_NUM_18
#define LED_WORKFLOW_ACTIVE GPIO_NUM_19

EventGroupHandle_t barrier_events;
EventGroupHandle_t pipeline_events;
EventGroupHandle_t workflow_events;

// --- Barrier Synchronization ---
#define WORKER_A_READY_BIT  (1 << 0)
#define WORKER_B_READY_BIT  (1 << 1)
#define WORKER_C_READY_BIT  (1 << 2)
#define WORKER_D_READY_BIT  (1 << 3)
#define ALL_WORKERS_READY   (WORKER_A_READY_BIT | WORKER_B_READY_BIT | WORKER_C_READY_BIT | WORKER_D_READY_BIT)

// --- Pipeline Processing ---
#define STAGE1_COMPLETE_BIT (1 << 0)
#define STAGE2_COMPLETE_BIT (1 << 1)
#define STAGE3_COMPLETE_BIT (1 << 2)
#define STAGE4_COMPLETE_BIT (1 << 3)
#define DATA_AVAILABLE_BIT  (1 << 4)
#define PIPELINE_RESET_BIT  (1 << 5)

// --- Workflow Management ---
#define WORKFLOW_START_BIT  (1 << 0)
#define APPROVAL_READY_BIT  (1 << 1)
#define RESOURCES_FREE_BIT  (1 << 2)
#define QUALITY_OK_BIT      (1 << 3)
#define WORKFLOW_DONE_BIT   (1 << 4)

// --- Structures ---
typedef struct {
    uint32_t worker_id;
    uint32_t cycle_number;
    uint32_t work_duration;
    uint64_t timestamp;
} worker_data_t;

typedef struct {
    uint32_t pipeline_id;
    uint32_t stage;
    float processing_data[4];
    uint32_t quality_score;
    uint64_t stage_timestamps[4];
} pipeline_data_t;

typedef struct {
    uint32_t workflow_id;
    char description[32];
    uint32_t priority;
    uint32_t estimated_duration;
    bool requires_approval;
} workflow_item_t;

// Queues
QueueHandle_t pipeline_queue;
QueueHandle_t workflow_queue;

// Stats
typedef struct {
    uint32_t barrier_cycles;
    uint32_t pipeline_completions;
    uint32_t workflow_completions;
    uint32_t synchronization_time_max;
    uint32_t synchronization_time_avg;
    uint64_t total_processing_time;
} sync_stats_t;

static sync_stats_t stats = {0};

// -------------------- BARRIER --------------------
void barrier_worker_task(void *pvParameters) {
    uint32_t worker_id = (uint32_t)pvParameters;
    EventBits_t my_ready_bit = (1 << worker_id);
    uint32_t cycle = 0;

    ESP_LOGI(TAG, "🏃 Worker %lu started", worker_id);

    while (1) {
        cycle++;
        uint32_t work_duration = 1000 + (esp_random() % 3000);
        ESP_LOGI(TAG, "👷 Worker %lu: Cycle %lu - independent (%lu ms)", worker_id, cycle, work_duration);
        vTaskDelay(pdMS_TO_TICKS(work_duration));

        uint64_t start = esp_timer_get_time();
        xEventGroupSetBits(barrier_events, my_ready_bit);

        EventBits_t bits = xEventGroupWaitBits(barrier_events, ALL_WORKERS_READY, pdTRUE, pdTRUE, pdMS_TO_TICKS(10000));
        uint32_t wait_time = (esp_timer_get_time() - start) / 1000;

        if ((bits & ALL_WORKERS_READY) == ALL_WORKERS_READY) {
            if (worker_id == 0) {
                stats.barrier_cycles++;
                gpio_set_level(LED_BARRIER_SYNC, 1);
                vTaskDelay(pdMS_TO_TICKS(200));
                gpio_set_level(LED_BARRIER_SYNC, 0);
            }
            if (wait_time > stats.synchronization_time_max)
                stats.synchronization_time_max = wait_time;
            stats.synchronization_time_avg = (stats.synchronization_time_avg + wait_time) / 2;
            ESP_LOGI(TAG, "🎯 Worker %lu barrier passed (%lu ms)", worker_id, wait_time);
        } else {
            ESP_LOGW(TAG, "⏰ Worker %lu timeout", worker_id);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// -------------------- PIPELINE --------------------
void pipeline_stage_task(void *pvParameters) {
    uint32_t stage_id = (uint32_t)pvParameters;
    EventBits_t stage_bit = (1 << stage_id);
    EventBits_t prev_bit = (stage_id > 0) ? (1 << (stage_id - 1)) : DATA_AVAILABLE_BIT;

    gpio_num_t leds[] = {LED_PIPELINE_STAGE1, LED_PIPELINE_STAGE2, LED_PIPELINE_STAGE3, LED_WORKFLOW_ACTIVE};
    const char* names[] = {"Input", "Process", "Filter", "Output"};

    ESP_LOGI(TAG, "🏭 Stage %lu (%s) started", stage_id, names[stage_id]);

    while (1) {
        xEventGroupWaitBits(pipeline_events, prev_bit, pdTRUE, pdTRUE, portMAX_DELAY);
        gpio_set_level(leds[stage_id], 1);

        pipeline_data_t data;
        if (xQueueReceive(pipeline_queue, &data, pdMS_TO_TICKS(100)) == pdTRUE) {
            data.stage = stage_id;
            data.stage_timestamps[stage_id] = esp_timer_get_time();

            uint32_t time = 500 + (esp_random() % 1000);
            ESP_LOGI(TAG, "⏳ Stage %lu processing (%lu ms)", stage_id, time);
            vTaskDelay(pdMS_TO_TICKS(time));

            if (stage_id < 3) {
                xQueueSend(pipeline_queue, &data, pdMS_TO_TICKS(100));
                xEventGroupSetBits(pipeline_events, stage_bit);
            } else {
                stats.pipeline_completions++;
                ESP_LOGI(TAG, "✅ Pipeline %lu done", data.pipeline_id);
            }
        }
        gpio_set_level(leds[stage_id], 0);
    }
}

void pipeline_data_generator_task(void *pvParameters) {
    uint32_t pid = 0;
    ESP_LOGI(TAG, "📦 Pipeline generator started");
    while (1) {
        pipeline_data_t d = {0};
        d.pipeline_id = ++pid;
        d.stage = 0;
        d.stage_timestamps[0] = esp_timer_get_time();
        xQueueSend(pipeline_queue, &d, pdMS_TO_TICKS(100));
        xEventGroupSetBits(pipeline_events, DATA_AVAILABLE_BIT);
        ESP_LOGI(TAG, "🚀 Data %lu injected", pid);
        vTaskDelay(pdMS_TO_TICKS(3000 + (esp_random() % 3000)));
    }
}

// -------------------- WORKFLOW --------------------
void approval_task(void *pvParameters) {
    while (1) {
        xEventGroupWaitBits(workflow_events, WORKFLOW_START_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
        uint32_t t = 1000 + (esp_random() % 2000);
        vTaskDelay(pdMS_TO_TICKS(t));
        bool ok = (esp_random() % 100) > 20;
        if (ok) xEventGroupSetBits(workflow_events, APPROVAL_READY_BIT);
        else xEventGroupClearBits(workflow_events, APPROVAL_READY_BIT);
        vTaskDelay(pdMS_TO_TICKS(5000));
        xEventGroupClearBits(workflow_events, APPROVAL_READY_BIT);
    }
}

void resource_manager_task(void *pvParameters) {
    bool free = true;
    while (1) {
        if (free) {
            xEventGroupSetBits(workflow_events, RESOURCES_FREE_BIT);
            vTaskDelay(pdMS_TO_TICKS(5000 + (esp_random() % 3000)));
            if ((esp_random() % 100) > 70) free = false;
        } else {
            xEventGroupClearBits(workflow_events, RESOURCES_FREE_BIT);
            vTaskDelay(pdMS_TO_TICKS(4000 + (esp_random() % 4000)));
            free = true;
        }
    }
}

void workflow_manager_task(void *pvParameters) {
    workflow_item_t wf;
    while (1) {
        if (xQueueReceive(workflow_queue, &wf, portMAX_DELAY) == pdTRUE) {
            xEventGroupSetBits(workflow_events, WORKFLOW_START_BIT);
            gpio_set_level(LED_WORKFLOW_ACTIVE, 1);
            EventBits_t need = RESOURCES_FREE_BIT | (wf.requires_approval ? APPROVAL_READY_BIT : 0);
            xEventGroupWaitBits(workflow_events, need, pdFALSE, pdTRUE, pdMS_TO_TICKS(10000));
            vTaskDelay(pdMS_TO_TICKS(wf.estimated_duration));
            uint32_t q = 60 + (esp_random() % 40);
            if (q > 80) {
                xEventGroupSetBits(workflow_events, WORKFLOW_DONE_BIT);
                stats.workflow_completions++;
            }
            gpio_set_level(LED_WORKFLOW_ACTIVE, 0);
            xEventGroupClearBits(workflow_events, WORKFLOW_START_BIT | WORKFLOW_DONE_BIT);
        }
    }
}

void workflow_generator_task(void *pvParameters) {
    uint32_t id = 0;
    const char* types[] = {"Data Processing","Report","Backup","Analysis","Test","Scan"};
    while (1) {
        workflow_item_t wf = {0};
        wf.workflow_id = ++id;
        wf.priority = 1 + (esp_random() % 5);
        wf.estimated_duration = 2000 + (esp_random() % 4000);
        wf.requires_approval = (esp_random() % 100) > 60;
        strcpy(wf.description, types[esp_random() % 6]);
        xQueueSend(workflow_queue, &wf, pdMS_TO_TICKS(500));
        vTaskDelay(pdMS_TO_TICKS(4000 + (esp_random() % 4000)));
    }
}

// -------------------- MONITOR --------------------
void statistics_monitor_task(void *pvParameters) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        ESP_LOGI(TAG, "📊 Barrier: %lu | Pipeline: %lu | Workflow: %lu", 
                 stats.barrier_cycles, stats.pipeline_completions, stats.workflow_completions);
    }
}

// -------------------- MAIN --------------------
void app_main(void) {
    ESP_LOGI(TAG, "🚀 Event Synchronization Lab Starting...");

    gpio_set_direction(LED_BARRIER_SYNC, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_PIPELINE_STAGE1, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_PIPELINE_STAGE2, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_PIPELINE_STAGE3, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_WORKFLOW_ACTIVE, GPIO_MODE_OUTPUT);

    barrier_events = xEventGroupCreate();
    pipeline_events = xEventGroupCreate();
    workflow_events = xEventGroupCreate();

    pipeline_queue = xQueueCreate(5, sizeof(pipeline_data_t));
    workflow_queue = xQueueCreate(8, sizeof(workflow_item_t));

    for (int i = 0; i < 4; i++) {
        char name[16];
        sprintf(name, "Worker%d", i);
        xTaskCreate(barrier_worker_task, name, 2048, (void*)i, 5, NULL);
    }

    for (int i = 0; i < 4; i++) {
        char name[16];
        sprintf(name, "Stage%d", i);
        xTaskCreate(pipeline_stage_task, name, 3072, (void*)i, 6, NULL);
    }

    xTaskCreate(pipeline_data_generator_task, "PipeGen", 2048, NULL, 4, NULL);
    xTaskCreate(workflow_manager_task, "WorkflowMgr", 3072, NULL, 7, NULL);
    xTaskCreate(approval_task, "Approval", 2048, NULL, 6, NULL);
    xTaskCreate(resource_manager_task, "ResourceMgr", 2048, NULL, 6, NULL);
    xTaskCreate(workflow_generator_task, "WorkflowGen", 2048, NULL, 4, NULL);
    xTaskCreate(statistics_monitor_task, "Stats", 2048, NULL, 3, NULL);

    ESP_LOGI(TAG, "System operational ✅");
}