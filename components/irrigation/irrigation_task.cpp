#include "irrigation_task.hpp"
#include "soil_sensor.hpp"
#include "nika_actuation.hpp"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

constexpr const char* TAG = "TERRA_IRRIGATE";

// Pin definitions
#define GPIO_VALVE_SEC1     GPIO_NUM_10
#define GPIO_WATER_LEVEL    GPIO_NUM_7

// Thresholds
constexpr float CONFIG_SOIL_DRY_THRESHOLD = 20.0f;

void irrigation_controller_task(void *pvParameters)
{
    // 1. Initialize Valve Pin (Output for Sector 1 Solenoid / Indicator LED)
    gpio_reset_pin(GPIO_VALVE_SEC1);
    gpio_set_direction(GPIO_VALVE_SEC1, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_VALVE_SEC1, 0);

    // 2. Initialize Water Reservoir Float Switch (Input with Pull-up)
    gpio_config_t float_conf = {
        .pin_bit_mask = (1ULL << GPIO_WATER_LEVEL),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&float_confg);

    ESP_LOGI(TAG, "Terra Irrigation Controller Task Initialized.");

    while (true)
    {
        // Step A: Read calibrated soil moisture percentage from the soil sensor component
        float soil_moisture_vwc = read_calibrated_soil_moisture();

        // Step B: Check water tank level (Assuming active-low or active-high based on float wiring)
        bool water_available = (gpio_get_level(GPIO_WATER_LEVEL) == 1);

        // Step C: Agronomic Decision Logic
        if (soil_moisture_vwc < CONFIG_SOIL_DRY_THRESHOLD)
        {
            if(!water_available)
            {
                ESP_LOGW(TAG, "Dry soil detected (%.1f%% VWC), but water reservoir is EMPTY! Irrigation cannot proceed.", soil_moisture_vwc);
                gpio_set_level(GPIO_VALVE_SEC1, 0);
            }
            else {
                // Soil is dry AND water is available, Ask Nika for permission (Interlock Veto Architecture)
                bool nika_approved = nika_request_pump_activation();

                if (nika_approved)
                {
                    gpio_set_level(GPIO_VALVE_SEC1, 1);
                    ESP_LOGI(TAG, "🌱 Irrigation ACTIVE: VWC=%.1f%%, Valve OPEN, Pump Running.", soil_moisture_vwc);
                }
                else {
                    gpio_set_level(GPIO_VALVE_SEC1, 0);
                    ESP_LOGW(TAG, "🛑 Irrigation VETOED by Nika Safety Interlock! Electrical fault active.");
                }
            }
        } 
        else
        {
            // Soil is sufficiently moist; turn off valve
            gpio_set_level(GPIO_VALVE_SEC1, 0);
            // ESP_LOGI(TAG "Soil moisture Optimal (%.1f%% VWC). Valve CLOSED.", soil_moisture_vwc);
        }

        // Poll every 1000ms
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void irrigation_task_start(void)
{
    xTaskCreatePinnedToCore(
        irrigation_controller_task,
        "Terra_Irrigation",
        4096,
        nullptr,
        8,            // Priority below analytics, above basic publishers
        nullptr,
        1             // Pinned to App CPU (Core 1) alongside analytics
    );
}