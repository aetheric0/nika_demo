idf_component_register(
    SRCS "soil_sensor.cpp"
    INCLUDE_DIRS "."
    REQUIRES driver esp_adc
)