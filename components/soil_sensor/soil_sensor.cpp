#include "soil_sensor.hpp"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

constexpr const char* TAG = "SOIL_SENSOR";

// ESP32-S3 ADC1 Channel 10 corresponds to GPIO 11
#define SOIL_ADC_UNIT       ADC_UNIT_1
#define SOIL_ADC_CHANNEL    ADC_CHANNEL_10

// Calibration Constants (Adjust these during live testing for specific soil sensor and environment)

// Capacitive sensor reads HIGHER in dry air, LOWER submerged in water.

constexpr int ADC_RAW_DRY = 3100;
constexpr int ADC_RAW_WET = 1350;

// Moving Average Filter Window Size
constexpr int FILTER_WINDOW_SIZE = 5;

static adc_oneshot_unit_handle_t g_adc1_handle = nullptr;
static int g_adc_samples[FILTER_WINDOW_SIZE] = {0};
static int g_sample_idx = 0;

// ----------------------------------------------------------------
// Initialize ADC One-Shot Mode for ESP32-S3 GPIO 11
// ----------------------------------------------------------------
bool soil_sensor_init(void)
{
    adc_oneshot_unit_init_config_t ={
        .unit_id = SOIL_ADC_UNIT,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    if (adc_oneshot_new_unit(&init_config, &g_adc1_handle) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize ADC unit for soil sensor");
        return false;
    }

    adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_12_0,     // Full-scale voltage range (~3.3V)
        .bitwidth = ADC_BITWIDTH_DEFAULT,   // 12-bit resolution (0 - 4095)
    };

    if (adc_oneshot_config_channel(g_adc1_handle, SOIL_ADC_CHANNEL, &channel_config) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to configure ADC channel 10 (GPIO 11) for soil sensor");
        return false;
    }

    ESP_LOGI(TAG, "Soil moisture sensor ADC initialized on GPIO 11 (ADC1_CH10)");
    return true; 
}

// ----------------------------------------------------------------
// Apply 5-Sample Moving Average Filter
// ----------------------------------------------------------------

static int apply_moving_average(int new_sample)
{
    g_adc_samples[g_sample_idx] = new_sample;
    g_sample_idx = (g_sample_idx + 1) % FILTER_WINDOW_SIZE;

    long sum = 0;
    for (int i = 0; i < FILTER_WINDOW_SIZE; i++)
    {
        sum += g_adc_samples[i];
    }
    return static_cast<int>(sum / FILTER_WINDOW_SIZE);
}

// ----------------------------------------------------------------
// Read Calibrated Volumetric Water Content (% VWC)
// ----------------------------------------------------------------

float read_calibrated_soil_moisture(void)
{
    int raw_adc = 0;
    if (adc_oneshot_read(g_adc1_handle, SOIL_ADC_CHANNEL, &raw_adc) != ESP_OK)
    {
        ESP_LOGW(TAG, "ADC read failed, returning last known safe state");
        return 50.0f;   // Default fallback
    }

    // Pass through 5-sample moving average filter
    int filtered_adc = apply_moving_average(raw_adc);

    // Linear Mapping Formula:
    // Because capcitive sensors output HIGHER counts when dry and LOWER counts when wet, we invert the mapping.
    float vwc = (static_cast<float>(ADC_RAW_DRY - filtered_adc) / static_cast<float>(ADC_RAW_DRY - ADC_RAW_WET)) * 100.0f;

    // Boundary Clamping [0.0%, 100.0%]
    if (vwc > 100.0f) vwc = 100.0f;
    if (vwc < 0.0f) vwc = 0.0f;

    // Debug log for serial terminal
    ESP_LOGI(TAG, "Raw ADC: %d | Filtered ADC: %d | VWC: %.1f%%", raw_adc, filtered_adc, vwc);

    return vwc;
}