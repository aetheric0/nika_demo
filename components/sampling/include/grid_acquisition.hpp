#ifndef GRID_ACQUISITION_HPP
#define GRID_ACQUISITION_HPP

#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

constexpr uint16_t BUFFER_SIZE = 1024;

struct GridSample_t
{
    uint16_t voltage_idx;
    uint16_t current_idx;
};

struct DataBuffer_t
{
    GridSample_t samples[BUFFER_SIZE];
};

// Accessor function definitions to pass pointers across components safely
bool grid_acquisition_init();
SemaphoreHandle_t grid_get_buffer_semaphore();
const DataBuffer_t *grid_get_populated_buffer();

#endif // GRID_ACQUISITION_HPP