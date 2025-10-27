#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "driver/gpio.h"
#include "soc/soc_memory_layout.h"
#include "esp_random.h"  // ✅ ต้องใส่เพิ่มใน ESP-IDF 5.2+

static const char *TAG = "MEM_OPT";

// GPIO สำหรับแสดงสถานะ optimization
#define LED_STATIC_ALLOC    GPIO_NUM_2
#define LED_ALIGNMENT_OPT   GPIO_NUM_4
#define LED_PACKING_OPT     GPIO_NUM_5
#define LED_MEMORY_SAVING   GPIO_NUM_18
#define LED_OPTIMIZATION    GPIO_NUM_19

// Alignment utilities
#define ALIGN_UP(num, align) (((num) + (align) - 1) & ~((align) - 1))
#define IS_ALIGNED(ptr, align) (((uintptr_t)(ptr) & ((align) - 1)) == 0)

// Static allocations
#define STATIC_BUFFER_SIZE   4096
#define STATIC_BUFFER_COUNT  8
#define TASK_STACK_SIZE      2048
#define MAX_TASKS            4

static uint8_t static_buffers[STATIC_BUFFER_COUNT][STATIC_BUFFER_SIZE] __attribute__((aligned(4)));
static bool static_buffer_used[STATIC_BUFFER_COUNT] = {false};
static SemaphoreHandle_t static_buffer_mutex;

static StackType_t task_stacks[MAX_TASKS][TASK_STACK_SIZE] __attribute__((aligned(8)));
static StaticTask_t task_buffers[MAX_TASKS];
static int next_task_slot = 0;

// Optimization statistics
typedef struct {
    size_t static_allocations;
    size_t dynamic_allocations;
    size_t alignment_optimizations;
    size_t packing_optimizations;
    size_t memory_saved_bytes;
    size_t fragmentation_reduced;
    uint64_t allocation_time_saved;
} optimization_stats_t;

static optimization_stats_t opt_stats = {0};

// Struct examples
typedef struct {
    char a;
    int b;
    char c;
    double d;
    char e;
} __attribute__((packed)) bad_struct_t;

typedef struct {
    double d;
    int b;
    char a;
    char c;
    char e;
} __attribute__((aligned(8))) good_struct_t;

typedef struct {
    const char* name;
    uint32_t caps;
    bool exec;
    bool dma;
} mem_region_t;

// Static buffer allocation/free
void* allocate_static_buffer(void) {
    void* buffer = NULL;
    if (xSemaphoreTake(static_buffer_mutex, pdMS_TO_TICKS(100))) {
        for (int i = 0; i < STATIC_BUFFER_COUNT; i++) {
            if (!static_buffer_used[i]) {
                static_buffer_used[i] = true;
                buffer = static_buffers[i];
                opt_stats.static_allocations++;
                gpio_set_level(LED_STATIC_ALLOC, 1);
                break;
            }
        }
        xSemaphoreGive(static_buffer_mutex);
    }
    return buffer;
}

void free_static_buffer(void* buffer) {
    if (!buffer) return;
    if (xSemaphoreTake(static_buffer_mutex, pdMS_TO_TICKS(100))) {
        for (int i = 0; i < STATIC_BUFFER_COUNT; i++) {
            if (buffer == static_buffers[i]) {
                static_buffer_used[i] = false;
                break;
            }
        }
        bool any_used = false;
        for (int i = 0; i < STATIC_BUFFER_COUNT; i++)
            if (static_buffer_used[i]) any_used = true;
        if (!any_used) gpio_set_level(LED_STATIC_ALLOC, 0);
        xSemaphoreGive(static_buffer_mutex);
    }
}

// Aligned malloc/free
void* aligned_malloc(size_t size, size_t alignment) {
    size_t total = size + alignment + sizeof(void*);
    void* raw = malloc(total);
    if (!raw) return NULL;
    uintptr_t raw_addr = (uintptr_t)raw + sizeof(void*);
    uintptr_t aligned_addr = ALIGN_UP(raw_addr, alignment);
    ((void**)aligned_addr)[-1] = raw;
    opt_stats.alignment_optimizations++;
    gpio_set_level(LED_ALIGNMENT_OPT, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
    gpio_set_level(LED_ALIGNMENT_OPT, 0);
    return (void*)aligned_addr;
}

void aligned_free(void* ptr) {
    if (ptr) free(((void**)ptr)[-1]);
}

// Struct optimization demo
void demonstrate_struct_optimization(void) {
    ESP_LOGI(TAG, "\n🏗️ STRUCT OPTIMIZATION DEMO");
    bad_struct_t bad; good_struct_t good;
    ESP_LOGI(TAG, "Bad struct size:  %d", sizeof(bad_struct_t));
    ESP_LOGI(TAG, "Good struct size: %d", sizeof(good_struct_t));
    size_t saved = sizeof(bad_struct_t) - sizeof(good_struct_t);
    opt_stats.memory_saved_bytes += saved * 1000;
    gpio_set_level(LED_PACKING_OPT, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(LED_PACKING_OPT, 0);
}

// Memory region analysis
void analyze_memory_regions(void) {
    ESP_LOGI(TAG, "\n🗺️ MEMORY REGION ANALYSIS");
    mem_region_t regions[] = {
        {"Internal RAM", MALLOC_CAP_INTERNAL, false, true},
        {"SPIRAM", MALLOC_CAP_SPIRAM, false, false},
        {"DMA Capable", MALLOC_CAP_DMA, false, true},
        {"Executable", MALLOC_CAP_EXEC, true, false},
    };
    for (int i = 0; i < 4; i++) {
        size_t total = heap_caps_get_total_size(regions[i].caps);
        size_t free = heap_caps_get_free_size(regions[i].caps);
        size_t largest = heap_caps_get_largest_free_block(regions[i].caps);
        if (total > 0) {
            float util = ((float)(total - free) / total) * 100.0;
            ESP_LOGI(TAG, "%s: total=%d free=%d largest=%d util=%.1f%%",
                     regions[i].name, total, free, largest, util);
        }
    }
}

// Memory access optimization
void optimize_memory_access_patterns(void) {
    ESP_LOGI(TAG, "\n⚡ MEMORY ACCESS PATTERNS");
    const size_t N = 1024;
    uint32_t *arr = aligned_malloc(N * sizeof(uint32_t), 32);
    for (int i = 0; i < N; i++) arr[i] = i;

    uint64_t t1 = esp_timer_get_time();
    volatile uint32_t s = 0;
    for (int i = 0; i < N; i++) s += arr[i];
    uint64_t seq = esp_timer_get_time() - t1;

    t1 = esp_timer_get_time();
    for (int i = 0; i < N; i++) s += arr[esp_random() % N];
    uint64_t rnd = esp_timer_get_time() - t1;

    ESP_LOGI(TAG, "Sequential: %llu μs, Random: %llu μs, Speedup %.2fx", seq, rnd, (float)rnd / seq);
    aligned_free(arr);
}

// Allocation benchmark
void benchmark_allocation_strategies(void) {
    ESP_LOGI(TAG, "\n🏃 ALLOCATION BENCHMARK");
    const int iter = 500;
    const size_t size = 256;

    uint64_t t1 = esp_timer_get_time();
    for (int i = 0; i < iter; i++) {
        void* p = malloc(size);
        memset(p, 0xAA, size);
        free(p);
    }
    uint64_t dyn = esp_timer_get_time() - t1;

    t1 = esp_timer_get_time();
    for (int i = 0; i < iter; i++) {
        void* p = allocate_static_buffer();
        memset(p, 0xAA, size);
        free_static_buffer(p);
    }
    uint64_t st = esp_timer_get_time() - t1;

    ESP_LOGI(TAG, "malloc: %llu μs, static: %llu μs, speedup %.2fx", dyn, st, (float)dyn / st);
    opt_stats.allocation_time_saved += (dyn > st ? dyn - st : 0);
}

// Static task creation
BaseType_t create_static_task(TaskFunction_t fn, const char* name, UBaseType_t prio) {
    if (next_task_slot >= MAX_TASKS) return pdFAIL;
    TaskHandle_t t = xTaskCreateStatic(fn, name, TASK_STACK_SIZE, NULL, prio,
                                       task_stacks[next_task_slot], &task_buffers[next_task_slot]);
    if (t) next_task_slot++;
    return t ? pdPASS : pdFAIL;
}

// Tasks
void optimization_test_task(void *pv) {
    while (1) {
        gpio_set_level(LED_OPTIMIZATION, 1);
        demonstrate_struct_optimization();
        analyze_memory_regions();
        optimize_memory_access_patterns();
        benchmark_allocation_strategies();
        gpio_set_level(LED_OPTIMIZATION, 0);
        vTaskDelay(pdMS_TO_TICKS(20000));
    }
}

void optimization_monitor_task(void *pv) {
    while (1) {
        ESP_LOGI(TAG, "\n📈 OPTIMIZATION STATS:");
        ESP_LOGI(TAG, "Static Alloc: %d", opt_stats.static_allocations);
        ESP_LOGI(TAG, "Align Opt:    %d", opt_stats.alignment_optimizations);
        ESP_LOGI(TAG, "Pack Opt:     %d", opt_stats.packing_optimizations);
        ESP_LOGI(TAG, "Saved:        %d bytes", opt_stats.memory_saved_bytes);
        ESP_LOGI(TAG, "Time saved:   %llu μs", opt_stats.allocation_time_saved);
        vTaskDelay(pdMS_TO_TICKS(15000));
    }
}

// Main
void app_main(void) {
    ESP_LOGI(TAG, "🚀 Memory Optimization Lab Starting...");
    gpio_set_direction(LED_STATIC_ALLOC, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_ALIGNMENT_OPT, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_PACKING_OPT, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_MEMORY_SAVING, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_OPTIMIZATION, GPIO_MODE_OUTPUT);

    static_buffer_mutex = xSemaphoreCreateMutex();
    demonstrate_struct_optimization();
    analyze_memory_regions();

    create_static_task(optimization_test_task, "OptTest", 5);
    xTaskCreate(optimization_monitor_task, "OptMon", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "✅ All tasks created successfully");
}