#ifndef SOIL_SENSOR_HPP
#define SOIL_SENSOR_HPP

#include <stdbool.h>

bool soil_sensor_init(void);
float read_calibrated_soil_moisture(void);

#endif // SOIL_SENSOR_HPP