#include "analytics_task.hpp"
#include "grid_acquisition.hpp"
#include "sine_lut.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

constexpr const char* TAG = "ANALYTICS";
static void analytics_worker_loop(void *pvParameters);

    // The semaphore is defined in grid_acquisition.c; we need to access it.
    // Proper encapsulation would use a getter

    static void analytics_worker_loop(void *pvParameters)
{
    SemaphoreHandle_t sync_sem = grid_get_buffer_semaphore();

    while (true)
    {
        if (xSemaphoreTake(sync_sem, portMAX_DELAY) == pdTRUE)
        {
            const DataBuffer_t *active_data = grid_get_populated_buffer();

            // Extract the raw index integers passed from the ISR
            uint16_t raw_v_idx = active_data->samples[0].voltage_idx;
            uint16_t raw_i_idx = active_data->samples[0].current_idx;

            // Perform the float scaling processing safely here on Core 1
            float real_voltage = 325.0f * sine_lut[raw_v_idx];
            float real_current = 10.0f * sine_lut[raw_i_idx];

            ESP_LOGI(TAG, "[Processing Pipeline] Data package read complete. Element 0 Voltage: %.2fV, Current: %.2fA", real_voltage, real_current);

            // --ESP-DSP FFT OPERATIONS ATTACH HERE--
        }
    }
}

void analytics_task_start()
{
    xTaskCreatePinnedToCore(
        analytics_worker_loop,
        "Analytics_Engine",
        4096,
        nullptr,
        5,
        nullptr,
        1);
}