#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "esp_random.h"
#include "esp_system.h"

static const char *TAG = "TIMER_APPS";

// Pin Definitions
#define STATUS_LED       GPIO_NUM_2
#define WATCHDOG_LED     GPIO_NUM_4
#define PATTERN_LED_1    GPIO_NUM_5
#define PATTERN_LED_2    GPIO_NUM_18
#define PATTERN_LED_3    GPIO_NUM_19
#define SENSOR_POWER     GPIO_NUM_21
#define SENSOR_PIN       GPIO_NUM_22

// Timer Periods
#define WATCHDOG_TIMEOUT_MS     5000
#define WATCHDOG_FEED_MS        2000
#define PATTERN_BASE_MS         500
#define SENSOR_SAMPLE_MS        1000
#define STATUS_UPDATE_MS        3000

typedef enum {
    PATTERN_OFF = 0,
    PATTERN_SLOW_BLINK,
    PATTERN_FAST_BLINK,
    PATTERN_HEARTBEAT,
    PATTERN_SOS,
    PATTERN_RAINBOW,
    PATTERN_MAX
} led_pattern_t;

typedef struct {
    float value;
    uint32_t timestamp;
    bool valid;
} sensor_data_t;

typedef struct {
    uint32_t watchdog_feeds;
    uint32_t watchdog_timeouts;
    uint32_t pattern_changes;
    uint32_t sensor_readings;
    uint32_t system_uptime_sec;
    bool system_healthy;
} system_health_t;

TimerHandle_t watchdog_timer;
TimerHandle_t feed_timer;
TimerHandle_t pattern_timer;
TimerHandle_t sensor_timer;
TimerHandle_t status_timer;

QueueHandle_t sensor_queue;
QueueHandle_t pattern_queue;

led_pattern_t current_pattern = PATTERN_OFF;
int pattern_step = 0;
system_health_t health_stats = {0, 0, 0, 0, 0, true};

typedef struct {
    int step;
    int direction;
    int intensity;
    bool state;
} pattern_state_t;

pattern_state_t pattern_state = {0, 1, 0, false};
esp_adc_cal_characteristics_t *adc_chars;

// ======= WATCHDOG SYSTEM =======

void recovery_callback(TimerHandle_t timer);

void watchdog_timeout_callback(TimerHandle_t timer) {
    health_stats.watchdog_timeouts++;
    health_stats.system_healthy = false;

    ESP_LOGE(TAG, "🚨 WATCHDOG TIMEOUT! System may be hung!");
    ESP_LOGE(TAG, "Feeds=%lu, Timeouts=%lu",
             health_stats.watchdog_feeds, health_stats.watchdog_timeouts);

    for (int i = 0; i < 10; i++) {
        gpio_set_level(WATCHDOG_LED, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
        gpio_set_level(WATCHDOG_LED, 0);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    ESP_LOGW(TAG, "In production: esp_restart() would be called here");
    xTimerReset(watchdog_timer, 0);
    health_stats.system_healthy = true;
}

void feed_watchdog_callback(TimerHandle_t timer) {
    static int feed_count = 0;
    feed_count++;

    if (feed_count == 15) {
        ESP_LOGW(TAG, "🐛 Simulating hang - stop feeds for 8s");
        xTimerStop(feed_timer, 0);
        TimerHandle_t recovery_timer = xTimerCreate("Recovery",
                                                    pdMS_TO_TICKS(8000),
                                                    pdFALSE,
                                                    (void*)0,
                                                    recovery_callback);
        xTimerStart(recovery_timer, 0);
        return;
    }

    health_stats.watchdog_feeds++;
    ESP_LOGI(TAG, "🍖 Feeding watchdog #%lu", health_stats.watchdog_feeds);
    xTimerReset(watchdog_timer, 0);
    gpio_set_level(STATUS_LED, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(STATUS_LED, 0);
}

void recovery_callback(TimerHandle_t timer) {
    ESP_LOGI(TAG, "🔄 System recovered - resuming watchdog feeds");
    xTimerStart(feed_timer, 0);
    xTimerDelete(timer, 0);
}

// ======= LED PATTERNS =======

void set_pattern_leds(bool led1, bool led2, bool led3) {
    gpio_set_level(PATTERN_LED_1, led1);
    gpio_set_level(PATTERN_LED_2, led2);
    gpio_set_level(PATTERN_LED_3, led3);
}

void change_led_pattern(led_pattern_t new_pattern);

void pattern_timer_callback(TimerHandle_t timer) {
    static uint32_t pattern_cycle = 0;
    pattern_cycle++;

    switch (current_pattern) {
        case PATTERN_OFF:
            set_pattern_leds(0, 0, 0);
            xTimerChangePeriod(timer, pdMS_TO_TICKS(1000), 0);
            break;
        case PATTERN_SLOW_BLINK:
            pattern_state.state = !pattern_state.state;
            set_pattern_leds(pattern_state.state, 0, 0);
            xTimerChangePeriod(timer, pdMS_TO_TICKS(1000), 0);
            break;
        case PATTERN_FAST_BLINK:
            pattern_state.state = !pattern_state.state;
            set_pattern_leds(0, pattern_state.state, 0);
            xTimerChangePeriod(timer, pdMS_TO_TICKS(200), 0);
            break;
        case PATTERN_HEARTBEAT: {
            int step = pattern_step % 10;
            bool pulse = (step < 2) || (step >= 3 && step < 5);
            set_pattern_leds(0, 0, pulse);
            pattern_step++;
            xTimerChangePeriod(timer, pdMS_TO_TICKS(100), 0);
            break;
        }
        case PATTERN_SOS: {
            static const char* sos = "...---...";
            static int sos_pos = 0;
            bool on = (sos[sos_pos] == '.');
            int duration = on ? 200 : 600;
            set_pattern_leds(on, on, on);
            sos_pos = (sos_pos + 1) % strlen(sos);
            if (sos_pos == 0) vTaskDelay(pdMS_TO_TICKS(1000));
            xTimerChangePeriod(timer, pdMS_TO_TICKS(duration), 0);
            break;
        }
        case PATTERN_RAINBOW: {
            int rainbow_step = pattern_step % 8;
            set_pattern_leds(rainbow_step & 1, rainbow_step & 2, rainbow_step & 4);
            pattern_step++;
            xTimerChangePeriod(timer, pdMS_TO_TICKS(300), 0);
            break;
        }
        default:
            set_pattern_leds(0, 0, 0);
            break;
    }

    if (pattern_cycle % 50 == 0) {
        led_pattern_t new_pattern = (current_pattern + 1) % PATTERN_MAX;
        change_led_pattern(new_pattern);
    }
}

void change_led_pattern(led_pattern_t new_pattern) {
    const char* names[] = {"OFF","SLOW_BLINK","FAST_BLINK","HEARTBEAT","SOS","RAINBOW"};
    ESP_LOGI(TAG, "🎨 Pattern: %s -> %s", names[current_pattern], names[new_pattern]);
    current_pattern = new_pattern;
    pattern_step = 0;
    pattern_state.step = 0;
    pattern_state.state = false;
    health_stats.pattern_changes++;
    xTimerReset(pattern_timer, 0);
}

// ======= SENSOR SYSTEM =======

float read_sensor_value(void) {
    gpio_set_level(SENSOR_POWER, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    uint32_t adc_reading = adc1_get_raw(ADC1_CHANNEL_0);
    uint32_t voltage = esp_adc_cal_raw_to_voltage(adc_reading, adc_chars);
    float value = (voltage / 1000.0) * 50.0;
    value += (esp_random() % 100 - 50) / 100.0;
    gpio_set_level(SENSOR_POWER, 0);
    return value;
}

void sensor_timer_callback(TimerHandle_t timer) {
    sensor_data_t data;
    data.value = read_sensor_value();
    data.timestamp = xTaskGetTickCount();
    data.valid = (data.value >= 0 && data.value <= 50);
    health_stats.sensor_readings++;

    BaseType_t hpw = pdFALSE;
    xQueueSendFromISR(sensor_queue, &data, &hpw);
    TickType_t new_period = pdMS_TO_TICKS(1000);
    if (data.value > 40.0) new_period = pdMS_TO_TICKS(500);
    else if (data.value < 25.0) new_period = pdMS_TO_TICKS(2000);
    xTimerChangePeriodFromISR(timer, new_period, &hpw);
    portYIELD_FROM_ISR(hpw);
}

// ======= STATUS SYSTEM =======

void status_timer_callback(TimerHandle_t timer) {
    health_stats.system_uptime_sec = pdTICKS_TO_MS(xTaskGetTickCount()) / 1000;
    ESP_LOGI(TAG, "\n═══════ SYSTEM STATUS ═══════");
    ESP_LOGI(TAG, "Uptime: %lus", health_stats.system_uptime_sec);
    ESP_LOGI(TAG, "Health: %s", health_stats.system_healthy ? "✅" : "❌");
    ESP_LOGI(TAG, "Feeds: %lu | Timeouts: %lu", health_stats.watchdog_feeds, health_stats.watchdog_timeouts);
    ESP_LOGI(TAG, "Patterns: %lu | Sensors: %lu", health_stats.pattern_changes, health_stats.sensor_readings);
    ESP_LOGI(TAG, "═════════════════════════════");
    gpio_set_level(STATUS_LED, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(STATUS_LED, 0);
}

// ======= TASKS =======

void sensor_processing_task(void *p) {
    sensor_data_t data;
    float sum = 0; int count = 0;
    while (1) {
        if (xQueueReceive(sensor_queue, &data, portMAX_DELAY)) {
            if (data.valid) {
                sum += data.value; count++;
                if (count >= 10) {
                    float avg = sum / count;
                    if (avg > 35.0) change_led_pattern(PATTERN_FAST_BLINK);
                    else if (avg < 15.0) change_led_pattern(PATTERN_SOS);
                    sum = 0; count = 0;
                }
            }
        }
    }
}

void system_monitor_task(void *p) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        if (health_stats.watchdog_timeouts > 5)
            health_stats.system_healthy = false;
        ESP_LOGI(TAG, "💾 Free heap: %d bytes", esp_get_free_heap_size());
    }
}

// ======= INITIALIZATION =======

void init_hardware(void) {
    gpio_set_direction(STATUS_LED, GPIO_MODE_OUTPUT);
    gpio_set_direction(WATCHDOG_LED, GPIO_MODE_OUTPUT);
    gpio_set_direction(PATTERN_LED_1, GPIO_MODE_OUTPUT);
    gpio_set_direction(PATTERN_LED_2, GPIO_MODE_OUTPUT);
    gpio_set_direction(PATTERN_LED_3, GPIO_MODE_OUTPUT);
    gpio_set_direction(SENSOR_POWER, GPIO_MODE_OUTPUT);
    gpio_set_level(SENSOR_POWER, 0);

    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC1_CHANNEL_0, ADC_ATTEN_DB_11);
    adc_chars = calloc(1, sizeof(esp_adc_cal_characteristics_t));
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, adc_chars);
}

void create_timers(void) {
    watchdog_timer = xTimerCreate("Watchdog", pdMS_TO_TICKS(WATCHDOG_TIMEOUT_MS), pdFALSE, (void*)1, watchdog_timeout_callback);
    feed_timer = xTimerCreate("Feed", pdMS_TO_TICKS(WATCHDOG_FEED_MS), pdTRUE, (void*)2, feed_watchdog_callback);
    pattern_timer = xTimerCreate("Pattern", pdMS_TO_TICKS(PATTERN_BASE_MS), pdTRUE, (void*)3, pattern_timer_callback);
    sensor_timer = xTimerCreate("Sensor", pdMS_TO_TICKS(SENSOR_SAMPLE_MS), pdTRUE, (void*)4, sensor_timer_callback);
    status_timer = xTimerCreate("Status", pdMS_TO_TICKS(STATUS_UPDATE_MS), pdTRUE, (void*)5, status_timer_callback);
}

void create_queues(void) {
    sensor_queue = xQueueCreate(20, sizeof(sensor_data_t));
    pattern_queue = xQueueCreate(10, sizeof(led_pattern_t));
}

void start_system(void) {
    xTimerStart(watchdog_timer, 0);
    xTimerStart(feed_timer, 0);
    xTimerStart(pattern_timer, 0);
    xTimerStart(sensor_timer, 0);
    xTimerStart(status_timer, 0);
    xTaskCreate(sensor_processing_task, "SensorProc", 4096, NULL, 5, NULL);
    xTaskCreate(system_monitor_task, "SysMon", 4096, NULL, 3, NULL);
}

void app_main(void) {
    ESP_LOGI(TAG, "Timer Applications Lab Starting...");
    init_hardware();
    create_queues();
    create_timers();
    start_system();
    change_led_pattern(PATTERN_SLOW_BLINK);
    ESP_LOGI(TAG, "🚀 System operational!");
}