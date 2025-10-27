#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_random.h"
#include "driver/gpio.h"

static const char *TAG = "ADV_TIMERS";

// ================= CONFIGURATION =================
#define TIMER_POOL_SIZE              20
#define DYNAMIC_TIMER_MAX            10
#define PERFORMANCE_BUFFER_SIZE      100
#define HEALTH_CHECK_INTERVAL        1000

#define PERFORMANCE_LED     GPIO_NUM_2
#define HEALTH_LED          GPIO_NUM_4
#define STRESS_LED          GPIO_NUM_5
#define ERROR_LED           GPIO_NUM_18

// ================= DATA STRUCTURES =================
typedef struct {
    TimerHandle_t handle;
    bool in_use;
    uint32_t id;
    char name[16];
    TickType_t period;
    bool auto_reload;
    TimerCallbackFunction_t callback;
    void* context;
    uint32_t creation_time;
    uint32_t start_count;
    uint32_t callback_count;
} timer_pool_entry_t;

typedef struct {
    uint32_t callback_start_time;
    uint32_t callback_duration_us;
    uint32_t timer_id;
    bool accuracy_ok;
} performance_sample_t;

typedef struct {
    uint32_t total_timers_created;
    uint32_t active_timers;
    uint32_t pool_utilization;
    uint32_t dynamic_timers;
    uint32_t failed_creations;
    uint32_t callback_overruns;
    float average_accuracy;
    uint32_t free_heap_bytes;
} timer_health_t;

// ================= GLOBAL VARIABLES =================
timer_pool_entry_t timer_pool[TIMER_POOL_SIZE];
SemaphoreHandle_t pool_mutex;
SemaphoreHandle_t perf_mutex;
uint32_t next_timer_id = 1000;

performance_sample_t perf_buffer[PERFORMANCE_BUFFER_SIZE];
uint32_t perf_index = 0;

timer_health_t health_data = {0};

TimerHandle_t health_monitor_timer;
TimerHandle_t performance_timer;

TimerHandle_t dynamic_timers[DYNAMIC_TIMER_MAX];
uint32_t dynamic_timer_count = 0;

// ================= TIMER POOL =================
void init_timer_pool(void) {
    pool_mutex = xSemaphoreCreateMutex();
    for (int i = 0; i < TIMER_POOL_SIZE; i++) {
        timer_pool[i].handle = NULL;
        timer_pool[i].in_use = false;
    }
    ESP_LOGI(TAG, "Timer pool initialized (%d slots)", TIMER_POOL_SIZE);
}

timer_pool_entry_t* allocate_from_pool(const char* name, TickType_t period,
                                      bool auto_reload, TimerCallbackFunction_t callback) {
    if (xSemaphoreTake(pool_mutex, pdMS_TO_TICKS(50)) != pdTRUE)
        return NULL;
    timer_pool_entry_t* entry = NULL;
    for (int i = 0; i < TIMER_POOL_SIZE; i++) {
        if (!timer_pool[i].in_use) {
            entry = &timer_pool[i];
            entry->in_use = true;
            entry->id = next_timer_id++;
            strncpy(entry->name, name, sizeof(entry->name) - 1);
            entry->period = period;
            entry->auto_reload = auto_reload;
            entry->callback = callback;
            entry->handle = xTimerCreate(name, period, auto_reload, (void*)entry->id, callback);
            if (!entry->handle) {
                entry->in_use = false;
                health_data.failed_creations++;
                entry = NULL;
            } else {
                health_data.total_timers_created++;
            }
            break;
        }
    }
    xSemaphoreGive(pool_mutex);
    return entry;
}

void release_to_pool(uint32_t id) {
    if (xSemaphoreTake(pool_mutex, pdMS_TO_TICKS(50)) != pdTRUE)
        return;
    for (int i = 0; i < TIMER_POOL_SIZE; i++) {
        if (timer_pool[i].in_use && timer_pool[i].id == id) {
            xTimerDelete(timer_pool[i].handle, 0);
            timer_pool[i].in_use = false;
            timer_pool[i].handle = NULL;
            break;
        }
    }
    xSemaphoreGive(pool_mutex);
}

// ================= PERFORMANCE =================
void record_perf(uint32_t id, uint32_t duration_us, bool ok) {
    if (xSemaphoreTake(perf_mutex, 0) == pdTRUE) {
        performance_sample_t *s = &perf_buffer[perf_index];
        s->timer_id = id;
        s->callback_duration_us = duration_us;
        s->accuracy_ok = ok;
        s->callback_start_time = esp_timer_get_time()/1000;
        perf_index = (perf_index + 1) % PERFORMANCE_BUFFER_SIZE;
        if (duration_us > 1000)
            health_data.callback_overruns++;
        xSemaphoreGive(perf_mutex);
    }
}

void analyze_performance(void) {
    if (xSemaphoreTake(perf_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
        return;
    uint32_t total = 0, count = 0, ok_count = 0, max = 0, min = UINT32_MAX;
    for (int i = 0; i < PERFORMANCE_BUFFER_SIZE; i++) {
        if (perf_buffer[i].callback_duration_us > 0) {
            total += perf_buffer[i].callback_duration_us;
            if (perf_buffer[i].callback_duration_us > max)
                max = perf_buffer[i].callback_duration_us;
            if (perf_buffer[i].callback_duration_us < min)
                min = perf_buffer[i].callback_duration_us;
            if (perf_buffer[i].accuracy_ok)
                ok_count++;
            count++;
        }
    }
    if (count > 0) {
        uint32_t avg = total / count;
        health_data.average_accuracy = ((float)ok_count / count) * 100;
        ESP_LOGI(TAG, "📊 Avg=%luus Max=%luus Min=%luus Accuracy=%.1f%%", 
                 avg, max, min, health_data.average_accuracy);
        gpio_set_level(PERFORMANCE_LED, avg > 500);
    }
    xSemaphoreGive(perf_mutex);
}

// ================= CALLBACKS =================
void perf_callback(TimerHandle_t t) {
    uint32_t start = esp_timer_get_time();
    volatile uint32_t it = 100 + (esp_random() % 400);
    while (it--) {}
    uint32_t dur = esp_timer_get_time() - start;
    record_perf((uint32_t)pvTimerGetTimerID(t), dur, dur < 1000);
}

void stress_callback(TimerHandle_t t) {
    static int blink = 0;
    blink = !blink;
    gpio_set_level(STRESS_LED, blink);
}

void health_callback(TimerHandle_t t) {
    health_data.free_heap_bytes = esp_get_free_heap_size();
    int used = 0, active = 0;
    for (int i = 0; i < TIMER_POOL_SIZE; i++) {
        if (timer_pool[i].in_use) {
            used++;
            if (xTimerIsTimerActive(timer_pool[i].handle))
                active++;
        }
    }
    health_data.active_timers = active;
    health_data.pool_utilization = (used * 100) / TIMER_POOL_SIZE;
    gpio_set_level(HEALTH_LED, health_data.pool_utilization > 80);
    ESP_LOGI(TAG, "🏥 Health: Active=%lu Used=%lu%% Heap=%luB",
             health_data.active_timers, health_data.pool_utilization, health_data.free_heap_bytes);
}

// ================= DYNAMIC =================
TimerHandle_t create_dynamic(const char *name, uint32_t period_ms, TimerCallbackFunction_t cb) {
    if (dynamic_timer_count >= DYNAMIC_TIMER_MAX)
        return NULL;
    TimerHandle_t t = xTimerCreate(name, pdMS_TO_TICKS(period_ms), pdTRUE, (void*)next_timer_id++, cb);
    if (t) {
        dynamic_timers[dynamic_timer_count++] = t;
        xTimerStart(t, 0);
        ESP_LOGI(TAG, "Dynamic timer created: %s", name);
    }
    return t;
}

// ================= TASKS =================
void perf_analysis_task(void *p) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        analyze_performance();
        ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());
        gpio_set_level(ERROR_LED, esp_get_free_heap_size() < 20000);
    }
}

void stress_task(void *p) {
    timer_pool_entry_t *entries[10];
    for (int i = 0; i < 10; i++) {
        char n[16];
        sprintf(n, "S%d", i);
        entries[i] = allocate_from_pool(n, pdMS_TO_TICKS(100 + i*50), true, stress_callback);
        if (entries[i])
            xTimerStart(entries[i]->handle, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(30000));
    for (int i = 0; i < 10; i++)
        if (entries[i]) release_to_pool(entries[i]->id);
    for (int i = 0; i < 5; i++) {
        char n[16]; sprintf(n, "D%d", i);
        create_dynamic(n, 200 + i*100, perf_callback);
    }
    vTaskDelete(NULL);
}

// ================= INIT =================
void init_gpio(void) {
    gpio_set_direction(PERFORMANCE_LED, GPIO_MODE_OUTPUT);
    gpio_set_direction(HEALTH_LED, GPIO_MODE_OUTPUT);
    gpio_set_direction(STRESS_LED, GPIO_MODE_OUTPUT);
    gpio_set_direction(ERROR_LED, GPIO_MODE_OUTPUT);
}

void create_sys_timers(void) {
    health_monitor_timer = xTimerCreate("Health", pdMS_TO_TICKS(HEALTH_CHECK_INTERVAL),
                                       pdTRUE, NULL, health_callback);
    performance_timer = xTimerCreate("Perf", pdMS_TO_TICKS(500),
                                     pdTRUE, NULL, perf_callback);
    if (health_monitor_timer) xTimerStart(health_monitor_timer, 0);
    if (performance_timer) xTimerStart(performance_timer, 0);
}

void app_main(void) {
    ESP_LOGI(TAG, "Advanced Timer Management Starting...");
    init_gpio();
    init_timer_pool();
    perf_mutex = xSemaphoreCreateMutex();
    memset(perf_buffer, 0, sizeof(perf_buffer));
    create_sys_timers();
    xTaskCreate(perf_analysis_task, "PerfAnalysis", 4096, NULL, 8, NULL);
    vTaskDelay(pdMS_TO_TICKS(3000));
    xTaskCreate(stress_task, "StressTest", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "🚀 System Running (LED2=Perf, LED4=Health, LED5=Stress, LED18=Error)");
}