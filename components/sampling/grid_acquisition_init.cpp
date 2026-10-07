#include "grid_acquisition.hpp"
#include "sine_lut.hpp"
#include "mcal_reg.hpp"
#include "driver/gptimer.h"
#include "esp_log.h"

constexpr const char* TAG = "GRID_ACQ";

// ----------------------------------------------------------------------
// Hardware configuration
// ----------------------------------------------------------------------

constexpr uint16_t SAMPLING_FREQUENCY = 16000;
constexpr uint16_t LOGIC_ANALYZER_PIN = 4;

// Expected grid frequency (Kenyan drift simulation)
constexpr float GRID_FREQ = 49.7f;

// Phase parameters for current displacement offset
constexpr uint32_t PHASE_OFFSET_CURRENT = 1900000;

// ----------------------------------------------------------------------
// Ping‑pong buffer workspace
// ----------------------------------------------------------------------

struct PingPongWorkspace_t
{
    DataBuffer_t buffer[2];
    volatile uint8_t active_write_idx;
    volatile uint16_t sample_counter;
    volatile uint8_t completed_read_idx = 0;
};

static PingPongWorkspace_t g_workspace;

// Semaphore: Wakes the analytics task when a buffer is full
static SemaphoreHandle_t g_buffer_full_sem = nullptr;

// Timer handle
static gptimer_handle_t g_timer = nullptr;

// Phase accumulator for LUT-based sine generation (32-bit fractional)
static uint32_t g_phase_acc_voltage = 0;
static uint32_t g_phase_acc_current = 0;

// ----------------------------------------------------------------------
// Look‑up table helper: step size per sample
// For a given frequency f, table size N, sampling rate Fs:
// phase_increment = (f * N / Fs) * 2^24   (using 24‑bit fractional part)
// ----------------------------------------------------------------------

constexpr uint32_t LUT_SCALE = (1 << LUT_FRAC_BITS);

static const uint32_t PHASE_INCREMENT = (uint32_t)((GRID_FREQ * LUT_SIZE / SAMPLING_FREQUENCY) * LUT_SCALE + 0.5f);

// ----------------------------------------------------------------------
// ISR: High‑speed timer alarm callback
// ----------------------------------------------------------------------

static bool IRAM_ATTR on_timer_alarm(
    gptimer_handle_t timer,
    const gptimer_alarm_event_data_t *edata,
    void *user_ctx)
{

    // 0. Diagnostic: set GPIO high (measure ISR duration)
    mcal::gpio::set_pin_high(LOGIC_ANALYZER_PIN);

    BaseType_t high_task_awoken = pdFALSE;
    PingPongWorkspace_t *ws = static_cast<PingPongWorkspace_t *>(user_ctx);

    // 1. Read index locations from the pre-computed look-up array
    uint32_t idx_v = (g_phase_acc_voltage >> LUT_FRAC_BITS) & (LUT_SIZE - 1);
    g_phase_acc_voltage += PHASE_INCREMENT;

    uint32_t idx_i = (g_phase_acc_current >> LUT_FRAC_BITS) & (LUT_SIZE - 1);
    g_phase_acc_current += PHASE_INCREMENT;

    // 2. Store into active buffer
    uint8_t w_idx = ws->active_write_idx;
    uint16_t s_idx = ws->sample_counter;
    ws->buffer[w_idx].samples[s_idx].voltage_idx = static_cast<uint16_t>(idx_v);
    ws->buffer[w_idx].samples[s_idx].current_idx = static_cast<uint16_t>(idx_i);

    // 3. Advance sample counter
    uint32_t current_count = ws->sample_counter;
    ws->sample_counter = current_count + 1;
    if (ws->sample_counter >= BUFFER_SIZE)
    {
        // Buffer full: swap ping <-> pong
        ws->completed_read_idx = w_idx;
        ws->active_write_idx = (w_idx == 0) ? 1 : 0;
        ws->sample_counter = 0;

        // Signal analytics task
        if (g_buffer_full_sem != nullptr)
            xSemaphoreGiveFromISR(g_buffer_full_sem, &high_task_awoken);
    }

    // Diagnostic: clear GPIO
    mcal::gpio::set_pin_low(LOGIC_ANALYZER_PIN);

    return (high_task_awoken == pdTRUE);
}

// ----------------------------------------------------------------------
// Public initialisation
// ----------------------------------------------------------------------

bool grid_acquisition_init(void)
{

    // 1. Diagnostic GPIO (LOGIC ANALYZER)
    mcal::gpio::init_output_pin(LOGIC_ANALYZER_PIN);

    // 2. Create semaphore (starts empty)
    g_buffer_full_sem = xSemaphoreCreateBinary();
    if (g_buffer_full_sem == nullptr)
    {
        ESP_LOGE(TAG, "Failed to create semaphore");
        return false;
    }

    // 3. Initialize phase accumulators (starting phase for current offset)
    g_phase_acc_voltage = 0;
    g_phase_acc_current = PHASE_OFFSET_CURRENT;

    // 4. Configure hardware timer
    gptimer_config_t timer_cfg =
        {
            .clk_src = GPTIMER_CLK_SRC_DEFAULT,
            .direction = GPTIMER_COUNT_UP,
            .resolution_hz = 1000000,
            .intr_priority = 0,
            .flags = {
                .intr_shared = 0,
                .allow_pd = 0,
            }
        };

    esp_err_t ret = gptimer_new_timer(&timer_cfg, &g_timer);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create timer: %d", ret);
        return false;
    }

    // 5. Set alarm to fire every (1e6 / 16000) = 62.5 ticks (rounded to 62)
    gptimer_alarm_config_t alarm_cfg = {
        .alarm_count = 1000000 / SAMPLING_FREQUENCY,
        .reload_count = 0,
        .flags = { .auto_reload_on_alarm = 1 }
    };

    ret = gptimer_set_alarm_action(g_timer, &alarm_cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set alarm: %d", ret);
        return false;
    }

    // 6. Register ISR callback, passing our workspace as context
    gptimer_event_callbacks_t cbs =
        {
            .on_alarm = on_timer_alarm,
        };
    ret = gptimer_register_event_callbacks(g_timer, &cbs, &g_workspace);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register ISR: %d", ret);
        return false;
    }

    // 7. Enable and start the timer
    ret = gptimer_enable(g_timer);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to enable timer: %d", ret);
        return false;
    }
    ret = gptimer_start(g_timer);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start timer: %d", ret);
        return false;
    }

    ESP_LOGI(TAG, "Grid acquisition operational at %d Hz", SAMPLING_FREQUENCY);
    return true;
}

SemaphoreHandle_t grid_get_buffer_semaphore()
{
    return g_buffer_full_sem;
}

const DataBuffer_t *grid_get_populated_buffer()
{
    return &g_workspace.buffer[g_workspace.completed_read_idx];
}
