#include "analytics_task.hpp"
#include "grid_acquisition.hpp"
#include "sine_lut.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#define FAULT_BUTTON_GPIO   GPIO_NUM_15
#define ALERT_LED_GPIO      GPIO_NUM_2

constexpr const char* TAG = "ANALYTICS";

// ------------------------------------------------
// Atomic Cross-Task State
// ------------------------------------------------
// Fault mode flag.
// In a real deployment, this would be controlled by a GPIO button, 
// a command over serial, or an internal trigger.
// For this example, we will just set it to false and toggle
// it manually.
// ------------------------------------------------


static std::atomic<bool> fault_mode{false};

// -----------------------------------------------
// CONSTANTS AND THRESHOLDS
// ----------------------------------------------
// Scale factors for the synthetic waveforms.
// Nominal peak voltage = 325 V (230 V RMS)
// Nominal peak current = 10 A
// In fault mode we reduce voltage by 15% (sag) and increase current by 20% to simulate an overloaded motor with harmonic distortion.
// ------------------------------------------------

static const float V_PEAK_NORMAL    = 325.0f;
static const float I_PEAK_NORMAL    =  10.0f;
static const float V_SAG_FACTOR     =   0.85f; // 15% voltage sag
static const float I_OVER_FACTOR    =   1.20f; // 20% overcurrent

// ------------------------------------------------
// Persistence filter parameters.
// We aggregate metrics over one second (15 buffers of 64ms each).
// An anomaly must be present for at least 2 consecutive 1-s windows.
// ------------------------------------------------

static const int AGGREGATION_BUFFERS = 15;  // ~1 second of data at 64ms per buffer
static const int PERSISTENCE_WINDOWS = 2;   // 2 seconds

// Thresholds for anomaly detection

static const float V_RMS_MIN = 200.0f;
static const float THD_MAX_NORMAL = 5.0f;
static const float PF_MIN_NORMAL = 0.60f;

// ------------------------------------------------
// Analytics task main loop
// ------------------------------------------------

static void analytics_worker_loop(void *pvParameters);

    // The semaphore is defined in grid_acquisition.c; we need to access it.
    // Proper encapsulation would use a getter

    static void analytics_worker_loop(void *pvParameters)
{
    SemaphoreHandle_t sync_sem = grid_get_buffer_semaphore();

    // Accumulators for 1-second aggregation
    float sum_v2 = 0.0f;
    float sum_i2 = 0.0f;
    float sum_p = 0.0f;
    int buffer_count = 0;

    // Persistence state
    int anomaly_streak = 0;
    int status_code = 0;

    // Heartbeat control
    uint32_t aggregation_count = 0;
    const uint32_t HEARTBEAT_INTERVAL = 1;  // send normal status every 1 aggregation (1 sec)
        // For production, set to 30

    // Ensure LED indicator is ready
    gpio_reset_pin(ALERT_LED_GPIO);
    gpio_set_direction(ALERT_LED_GPIO, GPIO_MODE_OUTPUT);

    ESP_LOGI(TAG, "Analytics Engine worker loop initialized on Core %d", xPortGetCoreID());

    while (true)
    {
        // wait for a full buffer
        if (xSemaphoreTake(sync_sem, portMAX_DELAY) == pdTRUE)
        {
            const DataBuffer_t *active_data = grid_get_populated_buffer();
            const bool local_fault_state = fault_mode.load(std::memory_order_relaxed);

            const float v_scale = (local_fault_state ? V_SAG_FACTOR : 1.0f) * V_PEAK_NORMAL;
            const float i_scale = (local_fault_state ? I_OVER_FACTOR : 1.0f) * I_PEAK_NORMAL;

            for (int i= 0; i < BUFFER_SIZE; i++)
            {
                // Extract the raw index integers passed from the ISR
                uint16_t raw_v_idx = active_data->samples[i].voltage_idx;
                uint16_t raw_i_idx = active_data->samples[i].current_idx;

                // Get instantaneous voltage and current
                float real_voltage = v_scale * sine_lut[raw_v_idx];
                float real_current = i_scale * sine_lut[raw_i_idx];

                // Accumulate squares and product
                sum_v2 += real_voltage * real_voltage;
                sum_i2 += real_current * real_current;
                sum_p += real_voltage * real_current;

                if (i == 0)
                {
                    ESP_LOGI(TAG, "[Processing Pipeline] Data package read complete. Element 0 Voltage: %.2fV, Current: %.2fA", real_voltage, real_current);

                }
            }

            buffer_count++;

            // ---------------------------------------------------------------
            // Once we have aggregated one second of data (15 buffers),
            // compute the average metrics and evaluate anomaly.
            // ----------------------------------------------------------------
            if (buffer_count >= AGGREGATION_BUFFERS)
            {
                const float total_samples = static_cast<float>(BUFFER_SIZE * buffer_count);

                float Vrms = sqrtf(sum_v2 / total_samples);
                float Irms = sqrtf(sum_i2 / total_samples);
                float avg_p = sum_p / total_samples;

                // Safe Power Factor computation with division-by-zero protection
                float denominator = Vrms * Irms;
                float PF = (denominator > 1e-6f) ? (avg_p / denominator) : 0.0f;

                // Numerical boundary clamping [-1.0, 1.0]
                if (PF > 1.0f) PF = 1.0f;
                if (PF < -1.0f) PF = -1.0f;

                // Simulated THD Caculation
                float THD = local_fault_state ? 15.0f : 1.0f; // Simulated THD

                // ----------------------------------------------------------------
                // Anomaly detection logic
                // ----------------------------------------------------------------
                bool anomaly_detected = (Vrms < V_RMS_MIN) || (THD > THD_MAX_NORMAL) || (PF < PF_MIN_NORMAL);

                if (anomaly_detected)
                {
                    anomaly_streak++;
                    status_code = (anomaly_streak >= PERSISTENCE_WINDOWS ? 2 : 1);
                }
                else
                {
                    anomaly_streak = 0;
                    status_code = 0;
                }

                // Actuate hardware alert LED
                gpio_set_level(ALERT_LED_GPIO, (status_code != 0) ? 1:0);

                if (status_code != 0) {
                    ESP_LOGW(
                        TAG, 
                        "⚠️ Power Anomaly Flagged [Status %d]: Vrms=%.2fV, Irms=%.2fA, PF=%.2f, THD=%.2f%%",
                        status_code, Vrms, Irms, PF, THD
                    );
                }

                // Decide whether to send immediately
                bool send_payload = (status_code != 0) || ((aggregation_count % HEARTBEAT_INTERVAL) == 0);

                if (send_payload)
                {
                    // Build a valid JSON payload
                    char json_payload[512];
                    int len = snprintf(
                        json_payload,
                        sizeof(json_payload),
                        "{\"node_id\":\"cold_room_compressor_01\","
                        "\"timestamp\":\"2026-08-18T10:30:00Z\","
                        "\"metric_type\":\"power_quality\","
                        "\"data_summary\":\"%s\","
                        "\"structured_metrics\":{"
                        "\"voltage_rms\": %.2f,"
                        "\"current_rms\": %.2f,"
                        "\"power_factor\": %.2f,"
                        "\"thd\": %.2f},"
                        "\"status_code\": %d}\n",
                        local_fault_state ? "Harmonic distortion and voltage sag detected": "Normal operation",
                        Vrms, Irms, PF, THD, status_code
                     );
                     if (len > 0 && len < sizeof(json_payload))
                     {
                        printf("%s", json_payload);
                        fflush(stdout);
                     }
                     else
                     {
                        ESP_LOGE(TAG, "JSON buffer overflow on payload construction");
                     }
                }
                aggregation_count++;

                // Reset accumulators for next aggregation window
                sum_v2 = 0.0f;
                sum_i2 = 0.0f;
                sum_p = 0.0f;
                buffer_count = 0;
                
            }
        }
    }
}

// ----------------------------------------------------------
// Non-Blocking Serial Command Task
// ----------------------------------------------------------

static void serial_command_task(void *pvParameters)
{
    char input[32];

    while (true)
    {
        // Read a line from USB serial
        if (fgets(input, sizeof(input), stdin) != nullptr)
        {
            if (strncmp(input, "FAULT", 5) == 0)
            {
                fault_mode.store(true, std::memory_order_relaxed);
                ESP_LOGW(TAG, "Serial Command: Fault mode ENABLED");
            }
            else if (strncmp(input, "NORMAL", 6) == 0)
            {
                fault_mode.store(false, std::memory_order_relaxed);
                ESP_LOGI(TAG, "Serial Command: Fault mode DISABLED");
            }
        }
        // Unconditional Delay: Yield CPU to prevent Task Watchdog Timer (TWDT) starve
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ---------------------------------------------------
// Physical Button Interlock Task
// ---------------------------------------------------

static void button_monitor_task(void *pvParameters)
{
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << FAULT_BUTTON_GPIO);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);

    bool last_pressed_state = false;

    while (true)
    {
        bool current_pressed_state = (gpio_get_level(FAULT_BUTTON_GPIO) == 0);  // pressed = low
        
        if (!last_pressed_state && current_pressed_state)
        {
            bool new_state = !fault_mode.load(std::memory_order_relaxed);
            fault_mode.store(new_state, std::memory_order_relaxed);
            if (new_state)
            {
                ESP_LOGW(TAG, "Button Trigger: Fault mode ENABLED");
            }
            else
            {
                ESP_LOGI(TAG, "Button Trigger: Fault mode DISABLED");
            }
        }
        last_pressed_state = current_pressed_state;
        vTaskDelay(pdMS_TO_TICKS(50));   // debounce
    }
}

void analytics_task_start()
{
    xTaskCreatePinnedToCore(
        analytics_worker_loop,
        "Analytics_Engine",
        8192,
        nullptr,
        5,
        nullptr,
        1);

        // Task for handling live serial commands (FAULT / NORMAL)
        xTaskCreatePinnedToCore(
            serial_command_task,
            "Serial_Command",
            4096,
            nullptr,
            4,
            nullptr,
            1
        );

        // Task for monitoring the physical fault toggle button
        xTaskCreatePinnedToCore(
            button_monitor_task,
            "Button_Monitor",
            2048,
            nullptr,
            3,
            nullptr,
            1
        );
}