// main/heap_management.c
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_random.h"
#include "driver/gpio.h"

static const char *TAG = "HEAP_MGMT";

// GPIO Indicators
#define LED_MEMORY_OK       GPIO_NUM_2
#define LED_LOW_MEMORY      GPIO_NUM_4
#define LED_MEMORY_ERROR    GPIO_NUM_5
#define LED_FRAGMENTATION   GPIO_NUM_18
#define LED_SPIRAM_ACTIVE   GPIO_NUM_19

// Thresholds
#define LOW_MEMORY_THRESHOLD       50000     // 50 KB
#define CRITICAL_MEMORY_THRESHOLD  20000     // 20 KB
#define FRAGMENTATION_THRESHOLD    0.30f
#define MAX_ALLOCATIONS            100

// Allocation tracking
typedef struct {
    void* ptr;
    size_t size;
    uint32_t caps;
    const char* description;
    uint64_t timestamp;
    bool is_active;
} memory_allocation_t;

typedef struct {
    uint32_t total_allocations;
    uint32_t total_deallocations;
    uint32_t current_allocations;
    uint64_t total_bytes_allocated;
    uint64_t total_bytes_deallocated;
    uint64_t peak_usage;
    uint32_t allocation_failures;
    uint32_t fragmentation_events;
    uint32_t low_memory_events;
} memory_stats_t;

static memory_allocation_t allocations[MAX_ALLOCATIONS];
static memory_stats_t stats = {0};
static SemaphoreHandle_t memory_mutex;
static bool memory_monitoring_enabled = true;

// ---- Helpers ----
static int find_free_allocation_slot(void) {
    for (int i = 0; i < MAX_ALLOCATIONS; i++) {
        if (!allocations[i].is_active) return i;
    }
    return -1;
}

static int find_allocation_by_ptr(void* ptr) {
    for (int i = 0; i < MAX_ALLOCATIONS; i++) {
        if (allocations[i].is_active && allocations[i].ptr == ptr) return i;
    }
    return -1;
}

// ---- Tracked alloc/free ----
static void* tracked_malloc(size_t size, uint32_t caps, const char* description) {
    void* ptr = heap_caps_malloc(size, caps);

    if (memory_monitoring_enabled && memory_mutex) {
        if (xSemaphoreTake(memory_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (ptr) {
                int slot = find_free_allocation_slot();
                if (slot >= 0) {
                    allocations[slot].ptr = ptr;
                    allocations[slot].size = size;
                    allocations[slot].caps = caps;
                    allocations[slot].description = description;
                    allocations[slot].timestamp = esp_timer_get_time();
                    allocations[slot].is_active = true;

                    stats.total_allocations++;
                    stats.current_allocations++;
                    stats.total_bytes_allocated += size;

                    size_t current_usage = stats.total_bytes_allocated - stats.total_bytes_deallocated;
                    if (current_usage > stats.peak_usage) stats.peak_usage = current_usage;

                    ESP_LOGI(TAG, "✅ Alloc %dB @%p (%s) slot=%d", (int)size, ptr, description, slot);
                } else {
                    ESP_LOGW(TAG, "⚠️ Tracking slots full");
                }
            } else {
                stats.allocation_failures++;
                ESP_LOGE(TAG, "❌ Alloc FAIL %dB (%s)", (int)size, description);
            }
            xSemaphoreGive(memory_mutex);
        }
    }
    return ptr;
}

static void tracked_free(void* ptr, const char* description) {
    if (!ptr) return;

    if (memory_monitoring_enabled && memory_mutex) {
        if (xSemaphoreTake(memory_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            int slot = find_allocation_by_ptr(ptr);
            if (slot >= 0) {
                allocations[slot].is_active = false;
                stats.total_deallocations++;
                stats.current_allocations--;
                stats.total_bytes_deallocated += allocations[slot].size;
                ESP_LOGI(TAG, "🗑️ Free %dB @%p (%s) slot=%d",
                         (int)allocations[slot].size, ptr, description, slot);
            } else {
                ESP_LOGW(TAG, "⚠️ Untracked free @%p (%s)", ptr, description);
            }
            xSemaphoreGive(memory_mutex);
        }
    }
    heap_caps_free(ptr);
}

// ---- Analysis & Monitoring ----
static void analyze_memory_status(void) {
    size_t internal_free    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t spiram_free      = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t total_free       = esp_get_free_heap_size();

    float internal_frag = 0.0f;
    if (internal_free > 0) internal_frag = 1.0f - ((float)internal_largest / (float)internal_free);

    ESP_LOGI(TAG, "\n📊 MEMORY STATUS");
    ESP_LOGI(TAG, "Internal Free:   %u", (unsigned)internal_free);
    ESP_LOGI(TAG, "Largest Block:   %u", (unsigned)internal_largest);
    ESP_LOGI(TAG, "SPIRAM Free:     %u", (unsigned)spiram_free);
    ESP_LOGI(TAG, "Total Free:      %u", (unsigned)total_free);
    ESP_LOGI(TAG, "Min Ever Free:   %u", (unsigned)esp_get_minimum_free_heap_size());
    ESP_LOGI(TAG, "Fragmentation:   %.1f%%", internal_frag * 100.0f);

    if (internal_free < CRITICAL_MEMORY_THRESHOLD) {
        gpio_set_level(LED_MEMORY_ERROR, 1);
        gpio_set_level(LED_LOW_MEMORY, 1);
        gpio_set_level(LED_MEMORY_OK, 0);
        stats.low_memory_events++;
        ESP_LOGW(TAG, "🚨 CRITICAL low memory");
    } else if (internal_free < LOW_MEMORY_THRESHOLD) {
        gpio_set_level(LED_LOW_MEMORY, 1);
        gpio_set_level(LED_MEMORY_ERROR, 0);
        gpio_set_level(LED_MEMORY_OK, 0);
        stats.low_memory_events++;
        ESP_LOGW(TAG, "⚠️ Low memory");
    } else {
        gpio_set_level(LED_MEMORY_OK, 1);
        gpio_set_level(LED_LOW_MEMORY, 0);
        gpio_set_level(LED_MEMORY_ERROR, 0);
    }

    if (internal_frag > FRAGMENTATION_THRESHOLD) {
        gpio_set_level(LED_FRAGMENTATION, 1);
        stats.fragmentation_events++;
        ESP_LOGW(TAG, "⚠️ High fragmentation");
    } else {
        gpio_set_level(LED_FRAGMENTATION, 0);
    }

    gpio_set_level(LED_SPIRAM_ACTIVE, spiram_free > 0 ? 1 : 0);
}

static void print_allocation_summary(void) {
    if (!memory_mutex) return;
    if (xSemaphoreTake(memory_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;

    ESP_LOGI(TAG, "\n📈 ALLOCATION STATS");
    ESP_LOGI(TAG, "Total allocs:     %lu", stats.total_allocations);
    ESP_LOGI(TAG, "Total frees:      %lu", stats.total_deallocations);
    ESP_LOGI(TAG, "Current in-use:   %lu", stats.current_allocations);
    ESP_LOGI(TAG, "Bytes alloc:      %llu", stats.total_bytes_allocated);
    ESP_LOGI(TAG, "Bytes freed:      %llu", stats.total_bytes_deallocated);
    ESP_LOGI(TAG, "Peak usage:       %llu", stats.peak_usage);
    ESP_LOGI(TAG, "Alloc failures:   %lu", stats.allocation_failures);
    ESP_LOGI(TAG, "Frag events:      %lu", stats.fragmentation_events);
    ESP_LOGI(TAG, "Low mem events:   %lu", stats.low_memory_events);

    if (stats.current_allocations > 0) {
        ESP_LOGI(TAG, "\n🔍 ACTIVE ALLOCS");
        for (int i = 0; i < MAX_ALLOCATIONS; i++) {
            if (allocations[i].is_active) {
                uint64_t age_ms = (esp_timer_get_time() - allocations[i].timestamp) / 1000;
                ESP_LOGI(TAG, "slot=%d size=%d ptr=%p caps=0x%X age=%llums desc=%s",
                         i, (int)allocations[i].size, allocations[i].ptr,
                         (unsigned)allocations[i].caps, age_ms,
                         allocations[i].description ? allocations[i].description : "-");
            }
        }
    }

    xSemaphoreGive(memory_mutex);
}

static void detect_memory_leaks(void) {
    if (!memory_mutex) return;
    if (xSemaphoreTake(memory_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;

    uint64_t now = esp_timer_get_time();
    int leaks = 0;
    size_t leaked_bytes = 0;

    ESP_LOGI(TAG, "\n🔍 LEAK SCAN (age>30s)");

    for (int i = 0; i < MAX_ALLOCATIONS; i++) {
        if (allocations[i].is_active) {
            uint64_t age_ms = (now - allocations[i].timestamp) / 1000;
            if (age_ms > 30000) {
                ESP_LOGW(TAG, "LEAK? slot=%d %dB @%p age=%llums desc=%s",
                         i, (int)allocations[i].size, allocations[i].ptr, age_ms,
                         allocations[i].description ? allocations[i].description : "-");
                leaks++;
                leaked_bytes += allocations[i].size;
            }
        }
    }

    if (leaks > 0) {
        ESP_LOGW(TAG, "Found %d potential leaks (%d bytes)", leaks, (int)leaked_bytes);
        gpio_set_level(LED_MEMORY_ERROR, 1);
    } else {
        ESP_LOGI(TAG, "No potential leaks");
        // keep LED as-is; only clear on good health if desired
    }

    xSemaphoreGive(memory_mutex);
}

// ---- Tasks ----
static void memory_stress_test_task(void *p) {
    ESP_LOGI(TAG, "🧪 Stress test start");
    void* ptrs[20] = {0};
    int n = 0;

    while (1) {
        int action = esp_random() % 3;

        if (action == 0 && n < 20) {
            size_t size = 100 + (esp_random() % 2100);
            uint32_t caps = (esp_random() % 2) ? MALLOC_CAP_INTERNAL : MALLOC_CAP_DEFAULT;
            void* pbuf = tracked_malloc(size, caps, "Stress");
            if (pbuf) {
                memset(pbuf, 0xAA, size);
                ptrs[n++] = pbuf;
                ESP_LOGI(TAG, "🔧 alloc %dB (n=%d)", (int)size, n);
            }
        } else if (action == 1 && n > 0) {
            int idx = esp_random() % n;
            if (ptrs[idx]) {
                tracked_free(ptrs[idx], "Stress");
                for (int i = idx; i < n - 1; i++) ptrs[i] = ptrs[i + 1];
                n--;
                ESP_LOGI(TAG, "🗑️ free (n=%d)", n);
            }
        } else {
            analyze_memory_status();
        }

        vTaskDelay(pdMS_TO_TICKS(1000 + (esp_random() % 2000)));
    }
}

static void memory_pool_test_task(void *p) {
    ESP_LOGI(TAG, "🏊 Pool test start");

    const size_t SZ[] = {64, 128, 256, 512, 1024};
    void* pool[5][10] = {0};

    while (1) {
        ESP_LOGI(TAG, "Alloc pools");
        for (int s = 0; s < 5; s++) {
            for (int i = 0; i < 10; i++) {
                char desc[24];
                snprintf(desc, sizeof(desc), "Pool%d_%d", s, i);
                pool[s][i] = tracked_malloc(SZ[s], MALLOC_CAP_INTERNAL, desc);
                if (pool[s][i]) memset(pool[s][i], 0x55 + s, SZ[s]);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5000));

        ESP_LOGI(TAG, "Free pools (reverse)");
        for (int s = 4; s >= 0; s--) {
            for (int i = 9; i >= 0; i--) {
                if (pool[s][i]) {
                    tracked_free(pool[s][i], "Pool");
                    pool[s][i] = NULL;
                }
            }
        }

        analyze_memory_status();
        vTaskDelay(pdMS_TO_TICKS(8000));
    }
}

static void large_allocation_test_task(void *p) {
    ESP_LOGI(TAG, "🐘 Large alloc test start");

    while (1) {
        size_t sz = 50000 + (esp_random() % 100000);
        ESP_LOGI(TAG, "Try large %dB", (int)sz);

        void* pbuf = tracked_malloc(sz, MALLOC_CAP_INTERNAL, "LargeInternal");
        if (!pbuf) {
            ESP_LOGW(TAG, "Internal fail, try SPIRAM");
            pbuf = tracked_malloc(sz, MALLOC_CAP_SPIRAM, "LargeSPIRAM");
        }

        if (pbuf) {
            uint64_t t0 = esp_timer_get_time();
            memset(pbuf, 0xFF, sz);
            uint64_t t1 = esp_timer_get_time();
            ESP_LOGI(TAG, "Write time: %llu us", (t1 - t0));
            vTaskDelay(pdMS_TO_TICKS(10000));
            tracked_free(pbuf, "Large");
        } else {
            ESP_LOGE(TAG, "Large alloc fail");
            analyze_memory_status();
        }

        vTaskDelay(pdMS_TO_TICKS(15000));
    }
}

static void memory_monitor_task(void *p) {
    ESP_LOGI(TAG, "📊 Monitor start");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        analyze_memory_status();
        print_allocation_summary();
        detect_memory_leaks();

        if (!heap_caps_check_integrity_all(true)) {
            ESP_LOGE(TAG, "🚨 HEAP CORRUPTED");
            gpio_set_level(LED_MEMORY_ERROR, 1);
        }

        ESP_LOGI(TAG, "Free heap: %u", (unsigned)esp_get_free_heap_size());
        ESP_LOGI(TAG, "Uptime: %llu ms\n", esp_timer_get_time() / 1000);
    }
}

static void heap_integrity_test_task(void *p) {
    ESP_LOGI(TAG, "🔍 Integrity test start");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ESP_LOGI(TAG, "Check integrity...");
        bool ok = heap_caps_check_integrity_all(false);
        if (ok) {
            ESP_LOGI(TAG, "✅ Heap OK");
        } else {
            ESP_LOGE(TAG, "❌ Heap FAIL");
            gpio_set_level(LED_MEMORY_ERROR, 1);
            heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
            if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0) {
                heap_caps_print_heap_info(MALLOC_CAP_SPIRAM);
            }
        }

        // small perf test
        const size_t TSZ = 4096;
        void* buf = tracked_malloc(TSZ, MALLOC_CAP_INTERNAL, "PerfTest");
        if (buf) {
            uint64_t t0 = esp_timer_get_time();
            for (int i = 0; i < 100; i++) memset(buf, i, TSZ);
            uint64_t tw = esp_timer_get_time() - t0;

            t0 = esp_timer_get_time();
            volatile uint8_t csum = 0;
            for (int i = 0; i < 100; i++) {
                uint8_t* b = (uint8_t*)buf;
                for (size_t j = 0; j < TSZ; j++) csum += b[j];
            }
            uint64_t tr = esp_timer_get_time() - t0;
            (void)csum;

            ESP_LOGI(TAG, "Perf: write=%llu us read=%llu us", tw, tr);
            tracked_free(buf, "PerfTest");
        }
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "🚀 Heap Management Lab Starting...");

    gpio_set_direction(LED_MEMORY_OK, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_LOW_MEMORY, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_MEMORY_ERROR, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_FRAGMENTATION, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_SPIRAM_ACTIVE, GPIO_MODE_OUTPUT);

    gpio_set_level(LED_MEMORY_OK, 0);
    gpio_set_level(LED_LOW_MEMORY, 0);
    gpio_set_level(LED_MEMORY_ERROR, 0);
    gpio_set_level(LED_FRAGMENTATION, 0);
    gpio_set_level(LED_SPIRAM_ACTIVE, 0);

    memory_mutex = xSemaphoreCreateMutex();
    if (!memory_mutex) {
        ESP_LOGE(TAG, "Create mutex failed");
        return;
    }
    memset(allocations, 0, sizeof(allocations));

    analyze_memory_status();

    ESP_LOGI(TAG, "\n🏗️ INITIAL HEAP (INTERNAL)");
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0) {
        ESP_LOGI(TAG, "\n🏗️ SPIRAM INFO");
        heap_caps_print_heap_info(MALLOC_CAP_SPIRAM);
    }

    xTaskCreate(memory_monitor_task,       "MemMonitor",  4096, NULL, 6, NULL);
    xTaskCreate(memory_stress_test_task,   "StressTest",  3072, NULL, 5, NULL);
    xTaskCreate(memory_pool_test_task,     "PoolTest",    3072, NULL, 5, NULL);
    xTaskCreate(large_allocation_test_task,"LargeAlloc",  2048, NULL, 4, NULL);
    xTaskCreate(heap_integrity_test_task,  "Integrity",   3072, NULL, 3, NULL);

    ESP_LOGI(TAG, "\n🎯 LEDs:");
    ESP_LOGI(TAG, "GPIO2  - Memory OK");
    ESP_LOGI(TAG, "GPIO4  - Low Memory");
    ESP_LOGI(TAG, "GPIO5  - Error/Leak");
    ESP_LOGI(TAG, "GPIO18 - Fragmentation");
    ESP_LOGI(TAG, "GPIO19 - SPIRAM Active");

    ESP_LOGI(TAG, "Heap Management System operational!");
}