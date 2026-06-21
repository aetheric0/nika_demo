#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the ESP-DSP Radix-2 execution structures, 
 * pre-computes the Hann windowing coefficients, and spawns
 * the dedicated analytics worker thread on Core 1.
 */
void analytics_task_start();

#ifdef __cplusplus
}
#endif