#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "NIKA_ACTUATION";

// =========================================================
// ESP-IDF HARDWARE PIN DEFINITIONS & SYSTEM THRESHOLDS
// =========================================================

#define GPIO_RELAY_TRIP     GPIO_NUM_4
#define GPIO_MANUAL_RESET   GPIO_NUM_5
#define GPIO_STATUS_LED     GPIO_NUM_2

// Electrical Safety Boundaries
#define THRESHOLD_OVERCURRENT_RMS        15.0f       // Amperes (Short Circuit / Mechanical Jam)
#define THRESHOLD_UNDERCURRENT_RMS        0.8f       // Amps (Dry Run / Broken Drive Belt)
#define THRESHOLD_OVERVOLTAGE_RMS       265.0f      // Volts (Grid Spike)
#define THRESHOLD_UNDERVOLTAGE_RMS      185.0f      // Volts (Grid Brownout)
#define THRESHOLD_THD_MAX                 8.0f      // Percent (Distortion limit)
#define THRESHOLD_PF_MIN                  0.75f     // Power Factor limit

// Timing & Persistence Controls
#define FAULT_PERSISTENCE_LIMIT     2           // Consecutive buffer trips required
#define RECLOSE_SAFETY_DELAY_US     (10 * 1000 * 1000) // 10 seconds in microseconds

// =========================================================
// SYSTEM STATES & ENUMS
// =========================================================
typedef enum {
    SYSTEM_HEALTHY = 0,
    FAULT_UNDERVOLTAGE = 1,
    FAULT_OVERVOLTAGE = 2,
    FAULT_OVERCURRENT = 3,
    FAULT_THD_WARNING = 4
} FaultType_t;

typedef enum {
    STATE_OPERATIONAL,
    STATE_PERSISTENCE_EVAL,
    STATE_TRIPPED_LOCKED,
    STATE_RECOVERY_COOLING
} SystemState_t;

// State Tracking Variables
static SystemState_t g_system_state = STATE_OPERATIONAL;
static uint8_t g_fault_counter = 0;
static FaultType_t g_active_fault = SYSTEM_HEALTHY;
static int64_t g_recovery_state_time_us = 0;

// =========================================================
// ESP-IDF GPIO DRIVER INITIALIZATION
// =========================================================
esp_err_t nika_actuation_init(void) {
    // 1. Configure Relay Pin (Output)
    gpio_config_t relay_conf = {
        .pin_bit_mask = (1ULL << GPIO_RELAY_TRIP),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE
    }
    ESP_ERROR_CHECK(gpio_config(&relay_conf));
    gpio_set_level(GPIO_RELAY_TRIP, 0);

    // 2. Configure Status LED (Output)
    gpio_config_t led_conf = {
        .pin_bit_mask = (1ULL << GPIO_STATUS_LED),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    }
    ESP_ERROR_CHECK(gpio_config(&led_conf));
    gpio_set_level(GPIO_STATUS_LED, 1);

    gpio_config_t btc_conf = {
        .pin_bit_mask = (1ULL << GPIO_MANUAL_RESET),
        .mode = GPI0_MODE_INPUT,
        .pull_up_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_Config(&btn_conf));

    ESP_LOGI(
        TAG,
        "Actuation Driver Online. ESP-IDF GPIOs initialized (Relay: %d, Reset: %d)",
        GPIO_RELAY_TRIP, GPIO_MANUAL_RESET
    );

    return ESP_OK;
}

// =========================================================
// HARDWARE INTERLOCK CONTROL FUNCTIONS
// =========================================================

static void trigger_hardware_trip(FaultType_t cause) {
    // Immediate physical disconnection via ESP-IDF GPIO level set
    gpio_set_level(GPIO_RELAY_TRIP, 1);
    gpio_set_level(GPIO_STATUS_LED, 0);

    g_system_state = STATE_TRIPPED_LOCKED;
    g_active_fault = cause;

    ESP_LOGE(TAG, "RELAY TRIP ASSERTED! Cause Code: %d", (int)cause);
}

static void clear_hardware_trip(void) {
    gpio_set_level(GPIO_RELAY_TRIP, 0);
    gpio_set_level(GPIO_STATUS_LED, 1);

    g_system_state = STATE_OPERATIONAL;
    g_active_fault = SYSTEM_HEALTHY;
    g_fault_counter = 0;
    g_recovery_start_time_us = 0;

    ESP_LOGI(TAG, "System cleared and restored to operational state.");
}

// =========================================================
// ESP-IDF DETERMINSTIC EVALUATION LOOP
// =========================================================
void nika_evaluate_and_actuate(float v_rms, float i_rms, float power_factor) {
    FaultType_t current_evaluation = SYSTEM_HEALTHY;

    // 1. Evaluate Primary Hardware Limits
    // HARD SAFETY EVALUATION (High-Priority Immediate/Persistent Trips)
    if (i_rms > THRESHOLD_OVERCURRENT_RMS) {
        current_evaluation = FAULT_OVERCURRENT;
    } else if (i_rms < THRESHOLD_UNDERCURRENT_RMS && i_rms > 0.05f) {
        // Note: Check i_rms > 0.05A so we don't trip when the machine is deliberately switched OFF
        current_evaluation = FAULT_UNDERCURRENT;
    } else if (v_rms > THRESHOLD_OVERVOLTAGE_RMS) {
        current_evaluation = FAULT_OVERVOLTAGE;
    } else if (v_rms < THRESHOLD_UNDERVOLTAGE_RMS) {
        current_evaluation = FAULT_UNDERVOLTAGE;
    }

    // 2. SOFT ANOMALY EVALUATION (Logged for RAG diagnostic maintenance)
    else if (thd > THRESHOLD_THD_MAX) {
        current_evaluation = FAULT_THD_WARNING;
    } else if (power_factor < THRESHOLD_PF_MIN && power_factor > 0.1f) {
        current_evaluation = FAULT_LOW_PF;
    }

    // 3. State Machine Logic
    switch (g_system_state) {
        case STATE_OPERATIONAL:
            if (current_evaluation != SYSTEM_HEALTHY) {
                // Immediate trip for extreme overcurrent (> 2x threshold)
                if (current_evaluation == FAULT_OVERCURRENT && i_rms > (THRESHOLD_OVERCURRENT_RMS * 2.0f)) {
                    trigger_harware_trip(current_evaluation); 
                } else {
                    g_fault_counter++;
                    g_system_state = STATE_PERSISTENCE_EVAL;
                }
            } else {
                g_fault_counter = 0;
            }
            break;
        case STATE_PERSISTENCE_EVAL:
            if (current_evaluation != SYSTEM_HEALTHY) {
                g_fault_counter++;
                if (g_fault_counter >= FAULT_PERSISTENCE_LIMIT) {
                    trigger_hardware_trip(current_evaluation);
                }
            } else {
                g_fault_counter = 0;
                g_system_state = STATE_OPERATIONAL;
            }
            break;
        
        case STATE_TRIPPED_LOCKED:
            // MANDATORY HUMAN-IN-THE-LOOP INTERLOCK (Overcurrent require physical button)
            if (g_active_fault == FAULT_OVERCURRENT) {
                if (gpio_get_level(GPIO_MANUAL_RESET) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                    if (gpio_get_level(GPIO_MANUAL_RESET) == 0) {
                        ESP_LOGW(TAG, "[HITL] Manual Reset Verified by Technician Button Press.")
                        clear_hardware_trip();
                    }
                }
            }

            // VOLTAGE FAULTS allow auto-recovery using native esp_timer
            else if (g_active_fault = FAULT_UNDERVOLTAGE || g_active_fault == FAULT_OVERVOLTAGE) {
                if (current_evaluation == SYSTEM_HEALTHY) {
                    ESP_LOGI(TAG, "Voltage stabilized. Starting ESP-IDF")
                    g_recovery_start_time_us = esp_timer_get_time();
                    g_system_state = STATE_RECOVERY_COOLING;
                }
            }
            break;

        case STATE_RECOVERY_COOLING:
            IF (current_evaluation != SYSTEM_HEALTHY) {
                // Voltage relapsed during recovery window
                g_recovery_start_time_us = 0;
                g_system_state = STATE_TRIPPED_LOCKED;
                ESP_LOGW(TAG, "Voltage relapsed during cooling. Relocking interlock.")   
            } else if ((esp_timer_get_time() - g_recovery_start_time_us) >= RECLOSE_SAFETY_DELAY_US) {
                ESP_LOGI(TAG, "Grid stable for required period. Auto-reclosing relay.");
                clear_hardware_trip();
            }
            break;
    }
}