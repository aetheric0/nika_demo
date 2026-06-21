#pragma once

#include <cstdint>

constexpr uint16_t LUT_SIZE = 256;
constexpr uint8_t LUT_FRAC_BITS = 8; 

// Declared extern so that it compiles once inside sine_lut.cpp 
// and maps across any linking framework tracking module without collisions
extern const float sine_lut[LUT_SIZE];