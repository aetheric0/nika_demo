#include "grid_acquisition.hpp"
#include "analytics_task.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

constexpr const char* TAG = "MAIN";

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Starting NIKA HIL Production Workspace...");

    // 1. Initializing sampling engine
    if (!grid_acquisition_init())
    {
        ESP_LOGE(TAG, "Acquisition engine init failed! Process halted.");
        return;
    }

    // 2. Initialize soil moisture sensor (GPIO 11 / ADC1_CH10)
    if (!soil_sensor_init())
    {
        ESP_LOGE(TAG, "Soil sensor init failed!");
    }

    // 3. Start analytics task
    analytics_task_start();

    // 4. Start Terra Irrigation Controller task (Core 1)
    irrigation_task_start();

    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "System operational heartbeat check, free heap: %u bytes", esp_get_free_heap_size());
    }
}
