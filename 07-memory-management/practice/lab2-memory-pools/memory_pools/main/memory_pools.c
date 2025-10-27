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
#include "esp_random.h"   // ✅ สำคัญ! ต้อง include เองตั้งแต่ ESP-IDF v5.2+
#include "driver/gpio.h"

// Tag สำหรับ Log
static const char *TAG = "MEM_POOLS";

// GPIO สำหรับแสดงสถานะ pool
#define LED_SMALL_POOL     GPIO_NUM_2   // Small pool activity
#define LED_MEDIUM_POOL    GPIO_NUM_4   // Medium pool activity
#define LED_LARGE_POOL     GPIO_NUM_5   // Large pool activity
#define LED_POOL_FULL      GPIO_NUM_18  // Pool exhaustion
#define LED_POOL_ERROR     GPIO_NUM_19  // Pool error/corruption

// Memory pool configurations
#define SMALL_POOL_BLOCK_SIZE   64
#define SMALL_POOL_BLOCK_COUNT  32
#define MEDIUM_POOL_BLOCK_SIZE  256
#define MEDIUM_POOL_BLOCK_COUNT 16
#define LARGE_POOL_BLOCK_SIZE   1024
#define LARGE_POOL_BLOCK_COUNT  8
#define HUGE_POOL_BLOCK_SIZE    4096
#define HUGE_POOL_BLOCK_COUNT   4

// === Memory Pool Structs ===
typedef struct memory_block {
    struct memory_block* next;
    uint32_t magic;
    uint32_t pool_id;()
    uint64_t alloc_time;
} memory_block_t;

typedef struct {
    const char* name;
    size_t block_size;
    size_t block_count;
    size_t alignment;
    uint32_t caps;
    void* pool_memory;
    memory_block_t* free_list;
    uint32_t* usage_bitmap;
    size_t allocated_blocks;
    size_t peak_usage;
    uint64_t total_allocations;
    uint64_t total_deallocations;
    uint64_t allocation_time_total;
    uint64_t deallocation_time_total;
    uint32_t allocation_failures;
    SemaphoreHandle_t mutex;
    uint32_t pool_id;
} memory_pool_t;

// === Enum สำหรับ Pool ===
typedef enum {
    POOL_SMALL = 0,
    POOL_MEDIUM,
    POOL_LARGE,
    POOL_HUGE,
    POOL_COUNT
} pool_type_t;

static memory_pool_t pools[POOL_COUNT];
static bool pools_initialized = false;

// === Config สำหรับแต่ละ Pool ===
typedef struct {
    const char* name;
    size_t block_size;
    size_t block_count;
    uint32_t caps;
    gpio_num_t led_pin;
} pool_config_t;

static const pool_config_t pool_configs[POOL_COUNT] = {
    {"Small",  SMALL_POOL_BLOCK_SIZE,  SMALL_POOL_BLOCK_COUNT,  MALLOC_CAP_INTERNAL, LED_SMALL_POOL},
    {"Medium", MEDIUM_POOL_BLOCK_SIZE, MEDIUM_POOL_BLOCK_COUNT, MALLOC_CAP_INTERNAL, LED_MEDIUM_POOL},
    {"Large",  LARGE_POOL_BLOCK_SIZE,  LARGE_POOL_BLOCK_COUNT,  MALLOC_CAP_DEFAULT,  LED_LARGE_POOL},
    {"Huge",   HUGE_POOL_BLOCK_SIZE,   HUGE_POOL_BLOCK_COUNT,   MALLOC_CAP_SPIRAM,   LED_POOL_FULL}
};

// Magic numbers
#define POOL_MAGIC_FREE    0xDEADBEEF
#define POOL_MAGIC_ALLOC   0xCAFEBABE

// === Function: Initialize Pool ===
bool init_memory_pool(memory_pool_t* pool, const pool_config_t* config, uint32_t pool_id) {
    if (!pool || !config) return false;

    memset(pool, 0, sizeof(memory_pool_t));
    pool->name = config->name;
    pool->block_size = config->block_size;
    pool->block_count = config->block_count;
    pool->alignment = 4;
    pool->caps = config->caps;
    pool->pool_id = pool_id;

    size_t header_size = sizeof(memory_block_t);
    size_t aligned_block_size = (config->block_size + pool->alignment - 1) & ~(pool->alignment - 1);
    size_t total_block_size = header_size + aligned_block_size;
    size_t total_memory = total_block_size * config->block_count;

    pool->pool_memory = heap_caps_malloc(total_memory, config->caps);
    if (!pool->pool_memory) {
        ESP_LOGE(TAG, "Failed to allocate %s pool", config->name);
        return false;
    }

    size_t bitmap_bytes = (config->block_count + 7) / 8;
    pool->usage_bitmap = heap_caps_calloc(bitmap_bytes, 1, MALLOC_CAP_INTERNAL);
    if (!pool->usage_bitmap) {
        heap_caps_free(pool->pool_memory);
        return false;
    }

    uint8_t* memory_ptr = (uint8_t*)pool->pool_memory;
    pool->free_list = NULL;
    for (int i = 0; i < config->block_count; i++) {
        memory_block_t* block = (memory_block_t*)(memory_ptr + (i * total_block_size));
        block->magic = POOL_MAGIC_FREE;
        block->pool_id = pool_id;
        block->next = pool->free_list;
        pool->free_list = block;
    }

    pool->mutex = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "✅ %s pool: %d blocks × %d bytes", config->name, config->block_count, config->block_size);
    return true;
}

// === Allocate / Free ===
void* pool_malloc(memory_pool_t* pool) {
    if (!pool || !pool->mutex) return NULL;
    void* result = NULL;
    if (xSemaphoreTake(pool->mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (pool->free_list) {
            memory_block_t* block = pool->free_list;
            pool->free_list = block->next;
            block->magic = POOL_MAGIC_ALLOC;
            block->alloc_time = esp_timer_get_time();
            pool->allocated_blocks++;
            pool->total_allocations++;
            result = (uint8_t*)block + sizeof(memory_block_t);
        } else {
            pool->allocation_failures++;
            gpio_set_level(LED_POOL_FULL, 1);
        }
        xSemaphoreGive(pool->mutex);
    }
    return result;
}

bool pool_free(memory_pool_t* pool, void* ptr) {
    if (!pool || !ptr || !pool->mutex) return false;
    memory_block_t* block = (memory_block_t*)((uint8_t*)ptr - sizeof(memory_block_t));
    if (xSemaphoreTake(pool->mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (block->magic == POOL_MAGIC_ALLOC && block->pool_id == pool->pool_id) {
            block->magic = POOL_MAGIC_FREE;
            block->next = pool->free_list;
            pool->free_list = block;
            pool->allocated_blocks--;
            pool->total_deallocations++;
        } else {
            gpio_set_level(LED_POOL_ERROR, 1);
        }
        xSemaphoreGive(pool->mutex);
    }
    return true;
}

// === Smart Allocator ===
void* smart_pool_malloc(size_t size) {
    size_t required = size + 16;
    for (int i = 0; i < POOL_COUNT; i++) {
        if (required <= pools[i].block_size) {
            void* ptr = pool_malloc(&pools[i]);
            if (ptr) {
                gpio_set_level(pool_configs[i].led_pin, 1);
                vTaskDelay(pdMS_TO_TICKS(30));
                gpio_set_level(pool_configs[i].led_pin, 0);
                return ptr;
            }
        }
    }
    return heap_caps_malloc(size, MALLOC_CAP_DEFAULT);
}

bool smart_pool_free(void* ptr) {
    for (int i = 0; i < POOL_COUNT; i++) {
        if (pool_free(&pools[i], ptr)) return true;
    }
    heap_caps_free(ptr);
    return true;
}

// === Monitor ===
void print_pool_statistics(void) {
    ESP_LOGI(TAG, "\n📊 === POOL STATUS ===");
    for (int i = 0; i < POOL_COUNT; i++) {
        memory_pool_t* p = &pools[i];
        ESP_LOGI(TAG, "%s: %d/%d used | Failures: %d", 
                 p->name, p->allocated_blocks, p->block_count, p->allocation_failures);
    }
}

void pool_monitor_task(void *pv) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        print_pool_statistics();
        ESP_LOGI(TAG, "Free heap: %d bytes", esp_get_free_heap_size());
    }
}

void pool_stress_task(void *pv) {
    void* ptrs[50] = {0};
    int count = 0;
    while (1) {
        int action = esp_random() % 3;
        if (action == 0 && count < 50) {
            size_t sz = 32 + (esp_random() % 2000);
            ptrs[count++] = smart_pool_malloc(sz);
        } else if (action == 1 && count > 0) {
            int idx = esp_random() % count;
            if (ptrs[idx]) {
                smart_pool_free(ptrs[idx]);
                ptrs[idx] = NULL;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// === Main ===
void app_main(void) {
    ESP_LOGI(TAG, "🚀 Memory Pool Lab Starting...");
    gpio_set_direction(LED_SMALL_POOL, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_MEDIUM_POOL, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_LARGE_POOL, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_POOL_FULL, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_POOL_ERROR, GPIO_MODE_OUTPUT);

    for (int i = 0; i < POOL_COUNT; i++) {
        if (!init_memory_pool(&pools[i], &pool_configs[i], i + 1)) {
            ESP_LOGE(TAG, "Failed to init %s pool", pool_configs[i].name);
            return;
        }
    }
    pools_initialized = true;
    print_pool_statistics();

    xTaskCreate(pool_monitor_task, "PoolMonitor", 4096, NULL, 5, NULL);
    xTaskCreate(pool_stress_task, "PoolStress", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "✅ All tasks created successfully");
}
    