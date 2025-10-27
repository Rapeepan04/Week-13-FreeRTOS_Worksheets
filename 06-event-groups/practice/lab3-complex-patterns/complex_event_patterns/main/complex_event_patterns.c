#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "driver/gpio.h"

static const char *TAG = "COMPLEX_EVENTS";

// GPIO
#define LED_LIVING_ROOM GPIO_NUM_2
#define LED_KITCHEN GPIO_NUM_4
#define LED_BEDROOM GPIO_NUM_5
#define LED_SECURITY GPIO_NUM_18
#define LED_EMERGENCY GPIO_NUM_19

// State definitions
typedef enum {
    STATE_IDLE = 0,
    STATE_OCCUPIED,
    STATE_AWAY,
    STATE_SLEEP,
    STATE_ARMED,
    STATE_EMERGENCY
} home_state_t;

// Event Groups
EventGroupHandle_t sensor_events;
EventGroupHandle_t system_events;
EventGroupHandle_t pattern_events;

// Event bits
#define MOTION_DETECTED_BIT   (1 << 0)
#define DOOR_OPENED_BIT       (1 << 1)
#define DOOR_CLOSED_BIT       (1 << 2)
#define LIGHT_ON_BIT          (1 << 3)
#define LIGHT_OFF_BIT         (1 << 4)
#define SECURITY_ARMED_BIT    (1 << 5)
#define EMERGENCY_BIT         (1 << 6)

#define PATTERN_ENTRY_BIT     (1 << 0)
#define PATTERN_BREAKIN_BIT   (1 << 1)
#define PATTERN_SLEEP_BIT     (1 << 2)
#define PATTERN_WAKEUP_BIT    (1 << 3)

static home_state_t current_state = STATE_IDLE;
static SemaphoreHandle_t state_mutex;

// --- State name ---
const char *get_state_name(home_state_t s) {
    switch (s) {
        case STATE_IDLE: return "Idle";
        case STATE_OCCUPIED: return "Occupied";
        case STATE_AWAY: return "Away";
        case STATE_SLEEP: return "Sleep";
        case STATE_ARMED: return "Armed";
        case STATE_EMERGENCY: return "Emergency";
        default: return "Unknown";
    }
}

void change_state(home_state_t s) {
    if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        ESP_LOGI(TAG, "🏠 State: %s → %s", get_state_name(current_state), get_state_name(s));
        current_state = s;
        xSemaphoreGive(state_mutex);
    }
}

// --- Pattern actions ---
void normal_entry_action(void) {
    ESP_LOGI(TAG, "🏡 Normal Entry Detected - Lights ON");
    gpio_set_level(LED_LIVING_ROOM, 1);
    change_state(STATE_OCCUPIED);
}

void breakin_action(void) {
    ESP_LOGW(TAG, "🚨 Break-in Detected - Alarm ON");
    gpio_set_level(LED_SECURITY, 1);
    gpio_set_level(LED_EMERGENCY, 1);
    change_state(STATE_EMERGENCY);
}

void goodnight_action(void) {
    ESP_LOGI(TAG, "🌙 Goodnight Pattern - Sleep Mode");
    gpio_set_level(LED_LIVING_ROOM, 0);
    gpio_set_level(LED_KITCHEN, 0);
    gpio_set_level(LED_BEDROOM, 1);
    change_state(STATE_SLEEP);
}

void wakeup_action(void) {
    ESP_LOGI(TAG, "☀️ Wake-up Pattern - Good Morning!");
    gpio_set_level(LED_BEDROOM, 1);
    gpio_set_level(LED_KITCHEN, 1);
    change_state(STATE_OCCUPIED);
}

// --- Sensor Simulation ---
void motion_sensor_task(void *pv) {
    while (1) {
        if ((esp_random() % 100) < 20) {
            ESP_LOGI(TAG, "👀 Motion detected");
            xEventGroupSetBits(sensor_events, MOTION_DETECTED_BIT);
        }
        vTaskDelay(pdMS_TO_TICKS(3000 + (esp_random() % 4000)));
    }
}

void door_sensor_task(void *pv) {
    while (1) {
        if ((esp_random() % 100) < 10) {
            ESP_LOGI(TAG, "🚪 Door opened");
            xEventGroupSetBits(sensor_events, DOOR_OPENED_BIT);
            vTaskDelay(pdMS_TO_TICKS(2000));
            ESP_LOGI(TAG, "🔒 Door closed");
            xEventGroupSetBits(sensor_events, DOOR_CLOSED_BIT);
        }
        vTaskDelay(pdMS_TO_TICKS(5000 + (esp_random() % 5000)));
    }
}

void light_control_task(void *pv) {
    while (1) {
        if ((esp_random() % 100) < 15) {
            bool on = (esp_random() % 2);
            if (on) {
                ESP_LOGI(TAG, "💡 Light ON");
                xEventGroupSetBits(sensor_events, LIGHT_ON_BIT);
                gpio_set_level(LED_LIVING_ROOM, 1);
            } else {
                ESP_LOGI(TAG, "💡 Light OFF");
                xEventGroupSetBits(sensor_events, LIGHT_OFF_BIT);
                gpio_set_level(LED_LIVING_ROOM, 0);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(4000 + (esp_random() % 4000)));
    }
}

// --- Pattern recognition ---
void pattern_recognition_task(void *pv) {
    while (1) {
        EventBits_t bits = xEventGroupWaitBits(sensor_events, 0xFFFF, pdTRUE, pdFALSE, portMAX_DELAY);

        // Normal Entry
        if ((bits & DOOR_OPENED_BIT) && (bits & MOTION_DETECTED_BIT) && (bits & DOOR_CLOSED_BIT)) {
            normal_entry_action();
            xEventGroupSetBits(pattern_events, PATTERN_ENTRY_BIT);
        }

        // Break-in (armed)
        if ((current_state == STATE_ARMED) && (bits & DOOR_OPENED_BIT) && (bits & MOTION_DETECTED_BIT)) {
            breakin_action();
            xEventGroupSetBits(pattern_events, PATTERN_BREAKIN_BIT);
        }

        // Goodnight
        if ((bits & LIGHT_OFF_BIT) && (bits & MOTION_DETECTED_BIT)) {
            goodnight_action();
            xEventGroupSetBits(pattern_events, PATTERN_SLEEP_BIT);
        }

        // Wake-up
        if ((current_state == STATE_SLEEP) && (bits & LIGHT_ON_BIT) && (bits & MOTION_DETECTED_BIT)) {
            wakeup_action();
            xEventGroupSetBits(pattern_events, PATTERN_WAKEUP_BIT);
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// --- State Machine ---
void state_machine_task(void *pv) {
    while (1) {
        EventBits_t sys = xEventGroupWaitBits(system_events, 0xFFFF, pdTRUE, pdFALSE, pdMS_TO_TICKS(5000));
        if (sys & SECURITY_ARMED_BIT) {
            change_state(STATE_ARMED);
        }
        if (sys & EMERGENCY_BIT) {
            change_state(STATE_EMERGENCY);
        }

        if (current_state == STATE_EMERGENCY) {
            vTaskDelay(pdMS_TO_TICKS(8000));
            gpio_set_level(LED_SECURITY, 0);
            gpio_set_level(LED_EMERGENCY, 0);
            change_state(STATE_IDLE);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// --- Monitor ---
void monitor_task(void *pv) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "📊 State: %s | Events: 0x%04X | Free Heap: %d bytes",
                 get_state_name(current_state),
                 xEventGroupGetBits(sensor_events),
                 esp_get_free_heap_size());
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "🚀 Complex Event Patterns Lab Starting...");

    gpio_set_direction(LED_LIVING_ROOM, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_KITCHEN, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_BEDROOM, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_SECURITY, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_EMERGENCY, GPIO_MODE_OUTPUT);

    state_mutex = xSemaphoreCreateMutex();
    sensor_events = xEventGroupCreate();
    system_events = xEventGroupCreate();
    pattern_events = xEventGroupCreate();

    change_state(STATE_IDLE);

    xTaskCreate(motion_sensor_task, "Motion", 2048, NULL, 5, NULL);
    xTaskCreate(door_sensor_task, "Door", 2048, NULL, 5, NULL);
    xTaskCreate(light_control_task, "Light", 2048, NULL, 5, NULL);
    xTaskCreate(pattern_recognition_task, "Pattern", 4096, NULL, 7, NULL);
    xTaskCreate(state_machine_task, "State", 3072, NULL, 6, NULL);
    xTaskCreate(monitor_task, "Monitor", 3072, NULL, 3, NULL);

    ESP_LOGI(TAG, "All tasks started successfully ✅");
}